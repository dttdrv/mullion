#!/usr/bin/env python3
"""runs the build's tests in a Wine, once as each GPU family, and prints what each made of them.

the tests are the build's own (meson's intro-tests.json: what tests/*/meson.build declares), run in a Wine the
build's libraries are installed into. a family other than the GPU's own lowers what the Metal bridge answers
(DXMT_GPU_FAMILY: src/winemetal/unix), so the paths an older GPU takes run here. a test passes when it says so on
every repeat: one that fails once, ends without a result or outlives its time has failed.
"""
import argparse
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


def run(command, env, seconds, log):
    """the command's verdict, and what it said last"""
    started = time.monotonic()
    with open(log, "wb") as out:
        process = subprocess.Popen(
            command, env=env, stdin=subprocess.DEVNULL, stdout=out, stderr=subprocess.STDOUT, start_new_session=True
        )
        try:
            status = process.wait(seconds)
        except subprocess.TimeoutExpired:
            # the test and what it started, which nothing else is in the session of
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
            return "failed", f"still running after {seconds} s", time.monotonic() - started
    lines = [line for line in log.read_text(errors="replace").splitlines() if line.strip()]
    # a test's result is the last line that starts with one
    said = next((line for line in reversed(lines) if line.split(":")[0] in (*RESULTS, "failed")), "")
    word = said.split(":")[0]
    if RESULTS.get(word) == status:
        return word, said, time.monotonic() - started
    last = lines[-1] if lines else "nothing"
    return "failed", said if word == "failed" else f"exit status {status} after: {last}", time.monotonic() - started


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", action="append", required=True, type=Path,
                        help="a meson build directory; with several, the first that has a library or the tests gives them")
    parser.add_argument("--wine", required=True, type=Path, help="a Wine's installation: the directory with bin/wine")
    parser.add_argument("--prefix", type=Path, help="the Wine prefix of the tests (made if missing); default: tests-prefix in the first build")
    parser.add_argument("--family", default="native,9,8,7",
                        help="the Apple GPU families to run as, 'native' for the GPU's own (default: %(default)s)")
    parser.add_argument("--repeat", type=int, default=1, help="how many times each test runs")
    parser.add_argument("--validate", action="store_true", help="run under Metal's API validation")
    parser.add_argument("--suite", action="append", default=[], help="only tests of this suite (may repeat)")
    parser.add_argument("--no-install", action="store_true", help="leave the Wine's libraries as they are")
    parser.add_argument("tests", nargs="*", help="only tests whose name starts with one of these")
    args = parser.parse_args()

    builds = [build.resolve() for build in args.build]
    wine = args.wine.resolve()
    prefix = (args.prefix or builds[0] / "tests-prefix").resolve()
    declared = [json.loads(path.read_text()) for build in builds
                for path in [build / "meson-info" / "intro-tests.json"] if path.exists()]
    declared = next((tests for tests in declared if tests), None)
    if not declared:
        sys.exit("no build was set up with tests (-Denable_tests=true)")
    tests = [t for t in declared
             if (not args.tests or any(t["name"].startswith(name) for name in args.tests))
             and (not args.suite or any(suite.split(":")[-1] in args.suite for suite in t["suite"]))]
    missing = [t["cmd"][0] for t in tests if not Path(t["cmd"][0]).exists()]
    if missing or not tests:
        sys.exit("not built: " + ", ".join(missing) if missing else "no such test")

    unix, pe = libraries(builds)
    base = dict(os.environ, WINEPREFIX=str(prefix), WINEDEBUG=os.environ.get("WINEDEBUG", "-all"), DXMT_SHADER_CACHE="0")
    base.pop("DXMT_GPU_FAMILY", None)
    # the build's libraries in place of the Wine's own, the shader compiler the prefix has, and no installers that
    # ask in a dialog
    own = sorted(path.stem for path in pe.values() if builtin(path))
    native = sorted(path.stem for path in pe.values() if not builtin(path))
    base["WINEDLLOVERRIDES"] = f"{','.join(native + ['d3dcompiler_47'])}=n;{','.join(own)}=b;mscoree,mshtml=d"
    if args.validate:
        base["MTL_DEBUG_LAYER"] = "1"
    binary = wine / "bin" / "wine"
    if not (prefix / "drive_c").exists():
        subprocess.run([binary, "wineboot", "-u"], env=base, stdin=subprocess.DEVNULL, check=True)
    if not args.no_install:
        install(wine, prefix, unix, pe)

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

    width = max(len(test["name"]) for test in tests)
    print("\n" + " " * width + "".join(f"  {family:>7}" for family in families))
    for test in tests:
        print(f"{test['name']:{width}}" + "".join(f"  {verdicts.get((test['name'], family), '-'):>7}" for family in families))
    counts = {word: sum(verdict == word for verdict in verdicts.values()) for word in ("passed", "failed", "skipped")}
    print(f"\n{counts['passed']} passed, {counts['failed']} failed, {counts['skipped']} skipped; logs in {logs}")
    sys.exit(counts["failed"] != 0)


if __name__ == "__main__":
    main()
