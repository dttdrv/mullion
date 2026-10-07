#!/usr/bin/env python3
"""runs the test suites others wrote against Windows in a Wine with the build's libraries, and prints what changed
against the verdicts recorded in tests/conformance.

the suites are not in this tree. Wine's own Direct3D tests (dlls/{d3d11,d3d10core,dxgi,d3d12}/tests) are built from
a Wine source tree and the build tree made from it (--wine-source, --wine-build); vkd3d-proton's Direct3D 12 tests
from a checkout of it with its khronos/Vulkan-Headers submodule (--vkd3d-proton). each test function runs in a
process of its own, as Windows: neither harness then excuses what it knows Wine or vkd3d to get wrong.

a verdict is what the harness counted: 'passed', 'failed', 'partly' (nothing failed, and the test skipped some of
itself, as for a feature the device does not report: no pass), 'skipped' (it ran no check), 'crashed' (it ended
without a count), 'timeout', and 'faulted' when it passed while the GPU's work went wrong (run.FAULTS). a test that crashed
runs once more with the libraries' addresses logged, and where it ended is named: the assertion that failed, or the
fault's library or program, function, and source line when the build has debug information. tests/conformance has a file a suite, of
each test's verdict, where it crashed, and why it is not 'passed'; --update records this run's verdicts and places
and keeps the reasons.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import run

RECORDS = Path(__file__).parent / "conformance"
# both harnesses end with their counts: Wine's include/wine/test.h, vkd3d-proton's include/private/vkd3d_test.h
COUNTS = re.compile(r"(\d+) tests executed \(.*?(\d+) failures?.*?(\d+) skipped")
# an unhandled exception's address, as Wine's test harness and Wine itself say it, and a library's load address
FAULT = re.compile(r"unhandled exception \w+ at ([0-9A-Fa-f]+)|Unhandled .* at address ([0-9A-Fa-f]+)")
LOADED = re.compile(r'Loaded L"([^"]+)" at ([0-9A-Fa-f]+)')
ASSERTION = re.compile(r"Assertion failed: (.*)")
# Wine's Direct3D tests that are one source file
WINE_TESTS = ("d3d11", "d3d10core", "dxgi", "d3d12")


def build(commands):
    """runs the compiler's commands, eight at a time, and ends with what the first that failed said"""
    with ThreadPoolExecutor(8) as pool:
        for done in pool.map(lambda command: subprocess.run(command, capture_output=True, text=True), commands):
            if done.returncode:
                sys.exit(f"{' '.join(map(str, done.args))}\n{done.stderr}")


def makefile(path):
    """a makefile's variables, each a list of words"""
    text = path.read_text().replace("\\\n", " ")
    return {name: value.split() for name, value in re.findall(r"^(\w+)\s*=(.*)$", text, re.M)}


def wine_suite(dll, source, objects, out):
    """Wine's tests of one library: the program, its test functions, and how one of them is run"""
    tests, made = source / "dlls" / dll / "tests", makefile(objects / "Makefile")
    rules, arch = makefile(tests / "Makefile.in"), made["PE_ARCHS"][0]
    cc, flags = made[f"{arch}_CC"], made[f"{arch}_EXTRACFLAGS"] + made[f"{arch}_CFLAGS"]
    includes = [f"-I{objects / 'include'}", f"-I{source / 'include'}", f"-I{source / 'include' / 'msvcrt'}"]
    # the test's main() queues or calls its test functions by name: each such statement of its body goes behind a
    # question of the environment, in a copy, so that one function runs alone. deeper statements are left: they are
    # what a test's child process runs
    code = (tests / f"{dll}.c").read_text()
    head, body = code.split("\nSTART_TEST(")
    call = re.compile(r"^(    )((?:queue|run)_\w+\([^;]*?\b(test_\w+)\b[^;]*\);|(test_\w+)\([^;]*\);)$", re.M)
    name = lambda match: match[3] or match[4]
    names = list(dict.fromkeys(map(name, call.finditer(body))))
    wants = 'static int wants(const char *test) { const char *only = getenv("MULLION_TEST"); return !only || !strcmp(only, test); }'
    guarded = call.sub(lambda match: f'{match[1]}if (wants("{name(match)}")) {match[2]}', body)
    out.mkdir(parents=True, exist_ok=True)
    (out / f"{dll}.c").write_text(f"{head}\n{wants}\nSTART_TEST({guarded}")
    sources = [out / f"{dll}.c", objects / "dlls" / dll / "tests" / "testlist.c"]
    compiled = [out / f"{dll}.{n}.o" for n in range(len(sources))]
    # as tools/makedep.c builds a test: one of Wine's own sources, with msvcrt for its C library
    commands = [cc + ["-c", "-o", o, s, f"-I{tests}", *includes, "-D__WINESRC__", "-D_MSVCR_VER=0", *flags, "-w"]
                for s, o in zip(sources, compiled)]
    for rc in (name for name in rules["SOURCES"] if name.endswith(".rc")):
        compiled.append(out / f"{dll}.res")
        commands.append([objects / "tools" / "wrc" / "wrc", "-u", "-o", compiled[-1], "--nostdinc", f"-I{tests}",
                         *includes, tests / rc])
    build(commands)
    # the import libraries of the build tree: the test's own, then what every test of Wine's links
    libraries = [next(objects.glob(f"dlls/*/{arch}-windows/lib{name}.a")) for name in rules["IMPORTS"]]
    libraries += [objects / where / f"{arch}-windows" / f"lib{Path(where).name}.a" for where in
                  ("libs/winecrt0", "libs/compiler-rt", "dlls/msvcrt", "dlls/kernel32", "dlls/ntdll")]
    exe = out / f"{dll}_test.exe"
    build([[objects / "tools" / "winegcc" / "winegcc", "-o", exe, "--wine-objdir", objects, f"--cc-cmd={' '.join(cc)}",
            "-b", f"{arch}-windows", *rules.get("EXTRADLLFLAGS", []), *compiled, *libraries, *made[f"{arch}_LDFLAGS"]]])
    return names, lambda name: ([exe, dll], {"MULLION_TEST": name, "WINETEST_PLATFORM": "windows"})


def compiler(builds):
    """the C compiler the build's own programs for Windows are made with"""
    return next(json.loads(path.read_text()) for b in builds for path in [b / "meson-info" / "intro-compilers.json"]
                if path.exists())["host"]["c"]["exelist"]


def vkd3d_suite(checkout, builds, binary, env, out):
    """vkd3d-proton's Direct3D 12 tests: the program, its tests, and how one of them is run"""
    # the build's compiler, and the IDL compiler of its toolchain
    cc = compiler(builds)
    widl = cc[0].rsplit("-", 1)[0] + "-widl"
    meson = lambda path, name: re.findall(r"'(\w+\.\w+)'", re.search(rf"{name} = \[(.*?)\]", path.read_text(), re.S)[1])
    (out / "include").mkdir(parents=True, exist_ok=True)
    build([[widl, "-h", "-o", out / "include" / f"{Path(idl).stem}.h", checkout / "include" / idl]
           for idl in meson(checkout / "include" / "meson.build", "vkd3d_idl")])
    sources = [checkout / "tests" / name for name in meson(checkout / "tests" / "meson.build", "d3d12_test_src")]
    sources += [checkout / "tests" / "d3d12_test_utils.c", *sorted((checkout / "libs" / "vkd3d-common").glob("*.c"))]
    # what its meson.build gives every source of a build for Windows without traces
    version = re.search(r"version : '([\d.]+)'", (checkout / "meson.build").read_text())[1]
    flags = ["-D_GNU_SOURCE", "-D_WIN32_WINNT=0x600", "-DVKD3D_NO_TRACE_MESSAGES", f'-DPACKAGE_VERSION="{version}"',
             "-msse2", "-O1", "-g", "-w"]
    includes = [out / "include", checkout / "include", checkout / "include" / "private", checkout / "tests",
                checkout / "khronos" / "Vulkan-Headers" / "include"]
    compiled = [out / f"{source.stem}.o" for source in sources]
    build([cc + ["-c", "-o", o, s, *flags, *(f"-I{i}" for i in includes)] for s, o in zip(sources, compiled)])
    exe = out / "d3d12.exe"
    # tests/meson.build's dependencies, and vkd3d_extra_libs of a build for Windows
    libraries = ("d3d12", "dxgi", "version", "pathcch", "psapi", "ntdll")
    build([cc + ["-o", exe, *compiled, *(f"-l{name}" for name in libraries), "-static"]])
    listed = subprocess.run([binary, exe, "--list-tests"], env=env, capture_output=True, text=True, timeout=120).stdout
    # the stress tests run for minutes: vkd3d-proton's own runner (tests/test-runner.sh) leaves them out too
    names = [name for name in listed.split() if name.startswith("test_") and "stress" not in name]
    return names, lambda name: ([exe], {"VKD3D_TEST_MATCH": name, "VKD3D_TEST_PLATFORM": "windows"})


def verdict(status, text):
    counts = COUNTS.findall(text)
    if status is None:
        return "timeout"
    if not counts:
        return "crashed"
    executed, failures, skipped = map(int, counts[-1])
    if failures or status:
        return "failed" if failures else "crashed"
    if run.faults(text.splitlines()):
        return "faulted"
    return "skipped" if not executed else "partly" if skipped else "passed"


def site(text, files, symbolizer):
    """where a log with the libraries' load addresses says its run ended: the assertion that failed, or the fault's
    module, function and line, the function being the one the source has there, not what the compiler put into
    it. `files` are the modules' files by name"""
    assertion = ASSERTION.search(text)
    fault = FAULT.search(text)
    if assertion or not fault:
        return f"assertion: {assertion[1].strip()}" if assertion else ""
    address = int(fault[1] or fault[2], 16)
    loaded = [(int(base, 16), re.split(r"[\\/]+", path)[-1].lower()) for path, base in LOADED.findall(text)]
    base, module = max(((b, m) for b, m in loaded if b <= address), default=(0, "?"))
    if module not in files:
        return f"{module}+{address - base:#x}"
    named = subprocess.run([symbolizer, f"--obj={files[module]}", "--relative-address", "-f", "-C", "-p", "-s", hex(address - base)],
                           capture_output=True, text=True).stdout.strip()
    return f"{module}: {named.splitlines()[-1].replace(' (inlined by) ', '') if named else hex(address - base)}"


def arguments(parser):
    parser.add_argument("--wine-source", type=Path, help="a Wine source tree, for Wine's Direct3D tests")
    parser.add_argument("--wine-build", type=Path, help="the build tree made from --wine-source")
    parser.add_argument("--vkd3d-proton", type=Path, help="a checkout of vkd3d-proton, for its Direct3D 12 tests")
    parser.add_argument("--update", action="store_true", help="record this run's verdicts in tests/conformance")
    parser.add_argument("--jobs", type=int, default=1, help="how many tests run at once (default: %(default)s)")
    parser.add_argument("--seconds", type=int, default=120, help="how long one test may run (default: %(default)s)")
    parser.add_argument("--only", action="append", default=[], help="only this suite, test, or suite:test (may repeat)")
    parser.add_argument("--skip", action="append", default=[], help="not this suite, test, or suite:test (may repeat)")


def conformance(args, builds, binary, base):
    """runs the suites the arguments name; the number of verdicts that differ from the recorded ones"""
    out = builds[0] / "conformance"
    pe = {path.name: path for path in run.libraries(builds)[1].values()}
    # the toolchain's symbolizer, beside its compiler
    symbolizer = Path(shutil.which(compiler(builds)[0])).parent / "llvm-symbolizer"
    suites = {}
    if args.wine_source and args.wine_build:
        for dll in WINE_TESTS:
            suites[f"wine-{dll}"] = wine_suite(dll, args.wine_source.resolve(), args.wine_build.resolve(), out / "wine")
    if args.vkd3d_proton:
        suites["vkd3d-proton"] = vkd3d_suite(args.vkd3d_proton.resolve(), builds, binary, base, out / "vkd3d-proton")
    named = lambda suite, name, names: any(n in (suite, name, f"{suite}:{name}") for n in names)
    wanted = lambda suite, name: (not args.only or named(suite, name, args.only)) and not named(suite, name, args.skip)
    changed = 0
    for suite, (names, command) in suites.items():
        names = [name for name in names if wanted(suite, name)]
        logs = out / "logs" / suite
        logs.mkdir(parents=True, exist_ok=True)

        def one(name):
            argv, env = command(name)
            status = run.finish([binary, *argv], dict(base, **env), args.seconds, logs / f"{name}.txt")
            word = verdict(status, (logs / f"{name}.txt").read_text(errors="replace"))
            files = dict(pe, **{Path(argv[0]).name.lower(): argv[0]})
            if word != "crashed":
                return word, ""
            run.finish([binary, *argv], dict(base, **env, WINEDEBUG="+loaddll"), args.seconds, logs / f"{name}.crash.txt")
            return word, site((logs / f"{name}.crash.txt").read_text(errors="replace"), files, symbolizer)

        with ThreadPoolExecutor(args.jobs) as pool:
            ran = dict(zip(names, pool.map(one, names)))
        got, places = {name: word for name, (word, at) in ran.items()}, {name: at for name, (word, at) in ran.items() if at}
        record = RECORDS / f"{suite}.json"
        recorded = json.loads(record.read_text()) if record.exists() else {}
        for name in names:
            was = recorded.get(name, {}).get("verdict", "unrecorded")
            if got[name] != was:
                changed += 1
                print(f"{suite:14}  {name:48} {was} -> {got[name]}  {places.get(name, '')}", flush=True)
        if args.update:
            # a reason goes with its verdict: a test whose verdict changed has none until someone looks
            kept = {name: entry for name, entry in recorded.items() if name not in got}
            new = {name: dict(recorded[name] if recorded.get(name, {}).get("verdict") == v else {"verdict": v},
                              **({"at": places[name]} if name in places else {})) for name, v in got.items()}
            RECORDS.mkdir(exist_ok=True)
            record.write_text(json.dumps(dict(sorted({**kept, **new}.items())), indent=1) + "\n")
        counts = {word: sum(v == word for v in got.values()) for word in sorted(set(got.values()))}
        print(f"{suite:14}  {len(got)} ran: " + ", ".join(f"{n} {word}" for word, n in counts.items()), flush=True)
    print(f"{changed} verdicts differ from tests/conformance; logs in {out / 'logs'}")
    return changed


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    run.arguments(parser)
    arguments(parser)
    args = parser.parse_args()
    sys.exit(conformance(args, *run.environment(args)) != 0)


if __name__ == "__main__":
    main()
