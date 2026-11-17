# contract: run.py's GPU lock (MULLION_TEST_LOCK) is held by one live process at a time, and a run marked
# MULLION_TEST_URGENT goes first: while a live urgent run waits, no other run takes the lock even when it is free.
# an urgent run's mark lasts as long as the run, and a dead process's mark or lock is taken over.
# an attempt that does not take the lock sleeps before the next one; the stubbed sleep tells the two apart, and lets
# a takeover make its one more attempt. a run told to end while it waits for the lock ends, and its waiting thread
# with it.
import importlib.util
import os
import subprocess
import sys
import tempfile
from pathlib import Path


class Waited(Exception):
    pass


def main():
    spec = importlib.util.spec_from_file_location('run', sys.argv[1])
    run = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(run)

    sleeps = [0]

    def wait(seconds):
        if not sleeps[0]:
            raise Waited
        sleeps[0] -= 1

    sleep = run.time.sleep
    run.time.sleep = wait
    dead = subprocess.Popen(['true'])
    dead.wait()
    live = os.getppid()
    failures = []
    with tempfile.TemporaryDirectory() as temporary:
        lock = str(Path(temporary, 'gpu.lock'))
        urgent = Path(lock + '.urgent')

        def attempt(case, marks, holder, marked, expected, retries=0):
            sleeps[0] = retries
            for name in ('marker', 'held'):
                if hasattr(run.alone, name):
                    delattr(run.alone, name)
            urgent.mkdir(exist_ok=True)
            for pid in marks:
                Path(urgent, str(pid)).touch()
            if holder:
                os.mkdir(lock)
                Path(lock, 'pid').write_text(str(holder))
            os.environ.pop('MULLION_TEST_URGENT', None)
            if marked:
                os.environ['MULLION_TEST_URGENT'] = '1'
            os.environ['MULLION_TEST_LOCK'] = lock
            try:
                with run.alone():
                    got = 'took' if Path(lock, 'pid').read_text() == str(os.getpid()) else 'took without owning'
            except Waited:
                got = 'waited'
            if got != expected:
                failures.append(f'{case}: {got}, want {expected}')
            for path in list(urgent.glob('*')):
                path.unlink()
            if os.path.isdir(lock):
                for path in Path(lock).glob('*'):
                    path.unlink()
                os.rmdir(lock)

        attempt('free lock, no urgent run', [], None, False, 'took')
        attempt('free lock, a live urgent run waits', [live], None, False, 'waited')
        attempt('free lock, a dead urgent run left its mark', [dead.pid], None, False, 'took')
        attempt('free lock, an urgent run among urgent ones', [live], None, True, 'took')
        attempt('held by a live run, urgent', [], live, True, 'waited')
        attempt('held by a dead run, not urgent', [], dead.pid, False, 'took', retries=1)

        # the mark is there while the urgent run lives and gone after it
        script = (f'import importlib.util, os\n'
                  f'spec = importlib.util.spec_from_file_location("run", {sys.argv[1]!r})\n'
                  f'run = importlib.util.module_from_spec(spec); spec.loader.exec_module(run)\n'
                  f'with run.alone():\n'
                  f'    print(os.path.exists({str(urgent)!r} + "/" + str(os.getpid())))\n')
        env = dict(os.environ, MULLION_TEST_LOCK=lock, MULLION_TEST_URGENT='1')
        child = subprocess.run([sys.executable, '-c', script], env=env, capture_output=True, text=True)
        if child.stdout.strip() != 'True':
            failures.append(f'urgent run held the lock without its mark: {child.stdout!r} {child.stderr!r}')
        if list(urgent.glob('*')):
            failures.append(f'urgent run left its mark: {[p.name for p in urgent.glob("*")]}')

        # a thread waits for a lock a live process holds while the main thread waits for it, as run.py's pool does
        run.time.sleep = sleep
        os.mkdir(lock)
        Path(lock, 'pid').write_text(str(live))
        script = (f'import importlib.util, threading\n'
                  f'spec = importlib.util.spec_from_file_location("run", {sys.argv[1]!r})\n'
                  f'run = importlib.util.module_from_spec(spec); spec.loader.exec_module(run)\n'
                  f'waiter = threading.Thread(target=lambda: run.alone().__enter__())\n'
                  f'waiter.start()\n'
                  f'print("waiting", flush=True)\n'
                  f'waiter.join()\n')
        env = dict(os.environ, MULLION_TEST_LOCK=lock)
        env.pop('MULLION_TEST_URGENT', None)
        child = subprocess.Popen([sys.executable, '-c', script], env=env, stdout=subprocess.PIPE, text=True)
        child.stdout.readline()
        child.terminate()
        try:
            child.wait(30)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait()
            failures.append('a run told to end while it waited for the lock went on waiting')
        if Path(lock + '.waiting', str(child.pid)).exists():
            failures.append('a run that ended while it waited for the lock left its mark')
        Path(lock, 'pid').unlink()
        os.rmdir(lock)
    for failure in failures:
        print(f'wrong: {failure}')
    print(f'{"failed" if failures else "passed"}: {len(failures)} wrong results')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
