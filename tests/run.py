#!/usr/bin/env python3
"""runs the build's tests in a Wine, once as each GPU family, and prints what each made of them.

the tests are the build's own (meson's intro-tests.json: what tests/*/meson.build declares), run in a Wine the
build's libraries are installed into. a family other than the GPU's own lowers what the Metal bridge answers
(DXMT_GPU_FAMILY: src/winemetal/unix), so the paths an older GPU takes run here. a test passes when it says so on
every repeat: one that fails once, ends without a result or outlives its time has failed.
"""
import argparse
import contextlib
import fcntl
import json
import os
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

# what a test's last word and exit status make of it (tests/*/..._test.hpp)
RESULTS = {"passed": 0, "skipped": 77}
# a runner that is told to end ends as one that is interrupted: through its finally clauses
signal.signal(signal.SIGTERM, lambda *_: sys.exit("ended"))
# how long a test waits for a machine short of memory before the run gives up
PATIENCE = 1800
# after this many tests the machine's lock is left free for this many seconds
TURNS, TURN = 6, 6
# how long a test may say nothing after a fault of the GPU's before it counts as hung
QUIET = 30


def libraries(builds):
    """the libraries a Wine takes from the builds, the first build that has one giving it: unix ones, then PE ones"""
    unix, pe = {}, {}
    for build in builds:
        for path in sorted(build.glob("src/*/unix/*.so")):
            unix.setdefault(path.name, path)
        for path in sorted(build.glob("src/*/*.dll")):
            pe.setdefault(path.name, path)
    return unix, pe


# Wine's names for a PE file's machine (IMAGE_FILE_HEADER.Machine)
MACHINES = {0x8664: "x86_64", 0xAA64: "aarch64", 0x14C: "i386"}


def machine(pe):
    data = pe.read_bytes()
    header = int.from_bytes(data[0x3C:0x40], "little")
    return MACHINES[int.from_bytes(data[header + 4:header + 6], "little")]


def builtin(pe):
    """whether the build marked the library as one of Wine's own (winebuild --builtin), which Wine loads from beside
    its own; a library without the mark is an application's, loaded from the prefix"""
    with open(pe, "rb") as file:
        file.seek(0x40)
        return file.read(16) == b"Wine builtin DLL"


def install(wine, prefix, unix, pe):
    for path in pe.values():
        shutil.copy2(path, prefix / "drive_c" / "windows" / "system32")
        if builtin(path):
            shutil.copy2(path, wine / "lib" / "wine" / f"{machine(path)}-windows")
        if path.stem + ".so" in unix:
            shutil.copy2(unix[path.stem + ".so"], wine / "lib" / "wine" / f"{machine(path)}-unix")


# what the library and Metal say when the GPU's work went wrong under a test: a command buffer that failed
# (src/d3d12/d3d12_command_queue.cpp, src/dxmt/dxmt_command_queue.cpp), a rule of Metal's validation broken
FAULTS = ("Device error", "failed assertion")


@contextlib.contextmanager
def alone():
    """one GPU job at a time on a machine that has others: MULLION_TEST_LOCK names their lock, a directory that holds
    its owner's process id and is taken over when the owner is gone. held for one test, so the others get their turn"""
    lock = os.environ.get("MULLION_TEST_LOCK")
    while lock:
        # the guard is never removed: publication and takeover must lock the same inode, including shell users
        with open(lock + ".guard", "a") as guard:
            fcntl.flock(guard, fcntl.LOCK_EX)
            try:
                os.mkdir(lock)
                Path(lock, "pid").write_text(str(os.getpid()))
                break
            except FileExistsError:
                try:
                    os.kill(int(Path(lock, "pid").read_text()), 0)
                except (ProcessLookupError, FileNotFoundError, ValueError):
                    shutil.rmtree(lock, ignore_errors=True)
        time.sleep(5)
    try:
        yield
    finally:
        if lock:
            with open(lock + ".guard", "a") as guard:
                fcntl.flock(guard, fcntl.LOCK_EX)
                shutil.rmtree(lock, ignore_errors=True)
            # the others look for the lock every few seconds: every few tests it stays free long enough for them
            alone.held = getattr(alone, "held", 0) + 1
            if alone.held % TURNS == 0:
                time.sleep(TURN)


def finish(command, env, seconds, log):
    """runs the command with its output in the log: its exit status, or None when it outlived its time. nothing
    starts on a machine short of memory (the kernel's pressure level, 1 when normal): a GPU test there can take the
    machine down with it. the test waits for the memory, up to PATIENCE seconds, and not while it has the machine's
    lock"""
    for waited in range(0, PATIENCE + 1, 10):
        with alone():
            level = subprocess.run(["sysctl", "-n", "kern.memorystatus_vm_pressure_level"], capture_output=True, text=True).stdout.strip()
            if level == "1":
                with open(log, "wb") as out:
                    end = time.monotonic() + seconds
                    process = subprocess.Popen(
                        command, env=env, stdin=subprocess.DEVNULL, stdout=out, stderr=subprocess.STDOUT, start_new_session=True
                    )
                    # the test and what it started, which nothing else is in the session of, do not outlive their time
                    # or this runner: a test left behind would use the GPU without the lock
                    try:
                        # a test whose GPU work failed, or that called a result wrong, has failed: once it is also
                        # quiet, nothing waits for it
                        while time.monotonic() < end:
                            try:
                                return process.wait(QUIET)
                            except subprocess.TimeoutExpired:
                                lines = log.read_text(errors="replace").splitlines()
                                if time.time() - log.stat().st_mtime > QUIET and (
                                        faults(lines) or any(line.startswith("wrong: ") for line in lines)):
                                    break
                        return None
                    finally:
                        if process.poll() is None:
                            os.killpg(process.pid, signal.SIGKILL)
                            process.wait()
        time.sleep(10)
    sys.exit(f"memory pressure level {level} for {PATIENCE} s: no test started")


def faults(lines):
    return [line for line in lines if any(fault in line for fault in FAULTS)]


def run(command, env, seconds, log):
    """the command's verdict, and what it said last"""
    started = time.monotonic()
    status = finish(command, env, seconds, log)
    lines = [line for line in log.read_text(errors="replace").splitlines() if line.strip()]
    if status is None:
        return "failed", f"still running after {time.monotonic() - started:.0f} s: {lines[-1] if lines else 'nothing'}", time.monotonic() - started
    # a test's result is the last line that starts with one
    said = next((line for line in reversed(lines) if line.split(":")[0] in (*RESULTS, "failed")), "")
    word = said.split(":")[0]
    # a result read while the GPU's work had failed is no result: the readback may hold what an earlier pass left
    if word == "passed" and faults(lines):
        return "failed", f"said '{said}' after: {faults(lines)[0]}", time.monotonic() - started
    if RESULTS.get(word) == status:
        return word, said, time.monotonic() - started
    last = lines[-1] if lines else "nothing"
    return "failed", said if word == "failed" else f"exit status {status} after: {last}", time.monotonic() - started


def arguments(parser):
    """what says where the tests run: the builds, the Wine and its prefix"""
    parser.add_argument("--build", action="append", required=True, type=Path,
                        help="a meson build directory; with several, the first that has a library or the tests gives them")
    parser.add_argument("--wine", required=True, type=Path, help="a Wine's installation: the directory with bin/wine")
    parser.add_argument("--prefix", type=Path, help="the Wine prefix of the tests (made if missing); default: tests-prefix in the first build")
    parser.add_argument("--validate", action="store_true", help="run under Metal's API validation")
    parser.add_argument("--no-install", action="store_true", help="leave the Wine's libraries as they are")


def environment(args):
    """the builds, the Wine's binary and the environment of a test in it, with the builds' libraries installed"""
    builds = [build.resolve() for build in args.build]
    wine = args.wine.resolve()
    prefix = (args.prefix or builds[0] / "tests-prefix").resolve()
    unix, pe = libraries(builds)
    base = dict(os.environ, WINEPREFIX=str(prefix), WINEDEBUG=os.environ.get("WINEDEBUG", "-all"), DXMT_SHADER_CACHE="0")
    base.pop("DXMT_GPU_FAMILY", None)
    # the build's libraries in place of the Wine's own, the shader compiler the prefix has, and no installer or
    # debugger that asks in a dialog
    own = sorted(path.stem for path in pe.values() if builtin(path))
    native = sorted(path.stem for path in pe.values() if not builtin(path))
    base["WINEDLLOVERRIDES"] = f"{','.join(native + ['d3dcompiler_47'])}=n;{','.join(own)}=b;mscoree,mshtml,winedbg.exe=d"
    if args.validate:
        base["MTL_DEBUG_LAYER"] = "1"
    binary = wine / "bin" / "wine"
    if not (prefix / "drive_c").exists():
        subprocess.run([binary, "wineboot", "-u"], env=base, stdin=subprocess.DEVNULL, check=True)
        # a display mode a test asks for is Wine's to give (win32u, sysparams.c), never the machine's display's
        subprocess.run([binary, "reg", "add", r"HKCU\Software\Wine\X11 Driver", "/v", "EmulateModeset", "/d", "Y", "/f"],
                       env=base, stdin=subprocess.DEVNULL, check=True)
    if not args.no_install:
        install(wine, prefix, unix, pe)
    return builds, binary, base


def explain(lines, most=6):
    """what a failed test's log says went wrong: the step it was in (tests/trace.hpp), its wrong results, what the
    library said, and Metal's own errors and validation text. at most `most` lines of a kind, each kind once"""
    kinds = {
        "in step": [line[6:] for line in lines if line.startswith("step: ")][-1:],
        "wrong": [line[7:] for line in lines if line.startswith("wrong: ")],
        "library": [line for line in lines if line.startswith(("err:", "warn:")) and not faults([line])],
        "Metal": [line for line in lines if faults([line]) or "-[MTL" in line or "AGXMetal" in line],
    }
    said = []
    for kind, found in kinds.items():
        found = list(dict.fromkeys(found))
        said += [f"{kind}: {line}" for line in found[:most]]
        if len(found) > most:
            said.append(f"{kind}: {len(found) - most} more")
    return said


def options(parser):
    """what says which of the build's tests run, and how"""
    parser.add_argument("--family", default="native,9,8,7",
                        help="the Apple GPU families to run as, 'native' for the GPU's own (default: %(default)s)")
    parser.add_argument("--repeat", type=int, default=1, help="how many times each test runs")
    parser.add_argument("--suite", action="append", default=[], help="only tests of this suite (may repeat)")
    parser.add_argument("--exact", action="store_true", help="match whole test names instead of prefixes")
    parser.add_argument("tests", nargs="*", help="test name prefixes, or whole names with --exact")


def chosen(args):
    """the build's tests the arguments name"""
    declared = [json.loads(path.read_text()) for build in args.build
                for path in [build.resolve() / "meson-info" / "intro-tests.json"] if path.exists()]
    declared = next((tests for tests in declared if tests), None)
    if not declared:
        sys.exit("no build was set up with tests (-Denable_tests=true)")
    tests = [t for t in declared
             if (not args.tests or any(t["name"] == name if args.exact else t["name"].startswith(name)
                                       for name in args.tests))
             and (not args.suite or any(suite.split(":")[-1] in args.suite for suite in t["suite"]))]
    missing = [t["cmd"][0] for t in tests if not Path(t["cmd"][0]).exists()]
    if missing or not tests:
        sys.exit("not built: " + ", ".join(missing) if missing else "no such test")
    return tests


def own(args, tests, builds, binary, base):
    """runs the build's tests as each family; how many failed"""
    logs = builds[0] / "meson-logs" / "tests"
    logs.mkdir(parents=True, exist_ok=True)
    families = args.family.split(",")
    verdicts = {}
    for family in families:
        env = dict(base) if family == "native" else dict(base, DXMT_GPU_FAMILY=family)
        for test in tests:
            native = not test["cmd"][0].endswith(".exe")
            # a test of the host's own does not meet the GPU
            if native and family != families[0]:
                continue
            command = test["cmd"] if native else [binary] + test["cmd"]
            verdict, said, seconds = "passed", "", 0.0
            for attempt in range(args.repeat):
                log = logs / f"{test['name']}.{family}.{attempt}.txt"
                verdict, said, taken = run(command, dict(env, **test["env"]), test["timeout"], log)
                seconds += taken
                if verdict == "failed":
                    break
            verdicts[test["name"], family] = verdict
            print(f"{family:>6}  {verdict:7}  {test['name']:32} {seconds:6.1f} s  {said}", flush=True)
            if verdict == "failed":
                for line in explain(log.read_text(errors="replace").splitlines()) + [f"log: {log}"]:
                    print(f"{'':17}{line}", flush=True)

    width = max(len(test["name"]) for test in tests)
    print("\n" + " " * width + "".join(f"  {family:>7}" for family in families))
    for test in tests:
        print(f"{test['name']:{width}}" + "".join(f"  {verdicts.get((test['name'], family), '-'):>7}" for family in families))
    counts = {word: sum(verdict == word for verdict in verdicts.values()) for word in ("passed", "failed", "skipped")}
    print(f"\n{counts['passed']} passed, {counts['failed']} failed, {counts['skipped']} skipped; logs in {logs}")
    return counts["failed"]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    arguments(parser)
    options(parser)
    args = parser.parse_args()
    tests = chosen(args)
    sys.exit(own(args, tests, *environment(args)) != 0)


if __name__ == "__main__":
    main()
