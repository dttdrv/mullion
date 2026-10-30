#!/usr/bin/env python3
"""runs the test suites others wrote against Windows in a Wine with the build's libraries, and prints what changed
against the verdicts recorded in tests/conformance.

the suites are not in this tree. Wine's own Direct3D tests (dlls/{d3d11,d3d10core,dxgi,d3d12,d3d9}/tests) are built from
a Wine source tree and the build tree made from it (--wine-source, --wine-build); vkd3d-proton's Direct3D 12 tests
from a checkout of it with its khronos/Vulkan-Headers submodule (--vkd3d-proton); vkd3d's shader tests from a checkout
of vkd3d (--vkd3d), each file through the build's Direct3D 12 and Direct3D 11. each test function or file runs in a
process of its own, as Windows: no harness then excuses what it knows Wine or vkd3d to get wrong.

a verdict is what the harness counted: 'passed', 'failed', 'partly' (nothing failed, and the test skipped some of
itself, as for a feature the device does not report: no pass), 'skipped' (it ran no check), 'crashed' (it ended
without a count), 'timeout', and 'faulted' when it passed while the GPU's work went wrong (run.FAULTS). a test that crashed
runs once more with the libraries' addresses logged, and where it ended is named: the assertion that failed, or the
fault's library or program, function, and source line when the build has debug information. tests/conformance has a file a suite, of
what a run found of each test and nothing else: its verdict, where it crashed, what its checks said they skipped, how
many failed and what the first said. --update records this run; a test whose verdict or skips differ is printed.
"""
import argparse
import json
import os
import re
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
# what a check says: '<source or test>:<line>: [<its context>: ]Test[s] skipped: <why>', or 'Test failed: <what>'
SKIPPED = re.compile(r"^\S+?:\d+:.*?Tests? skipped: (.*)$", re.M)
FAILED = re.compile(r"^\S+?:\d+:.*?Test failed:.*$", re.M)
# Wine's Direct3D tests that are one source file
WINE_TESTS = ("d3d11", "d3d10core", "dxgi", "d3d12", "d3d9")


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


def vkd3d_suite(checkout, builds, binary, env, out):
    """vkd3d-proton's Direct3D 12 tests: the program, its tests, and how one of them is run"""
    # the build's compiler, and the IDL compiler of its toolchain
    cc = run.compiler(builds)
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


def shader_suite(checkout, dxcompiler, builds, env, out):
    """vkd3d's shader tests (its .shader_test files), by the shader runner of its build for Windows (Makefile.am's
    crosstest), with the expectations of Windows: the files, and how one runs through an executor, d3d12.dll or
    d3d11.dll. shader model 6 compiles with Microsoft's DXC (--dxcompiler), lower ones with the Wine's d3dcompiler_47"""
    cc, made = run.compiler(builds), makefile(checkout / "Makefile.am")
    places = {"srcdir": checkout, "builddir": out}

    def words(name):
        text = re.sub(r"\$[({](\w+)[)}]", lambda m: str(places[m[1]]) if m[1] in places else " ".join(words(m[1])),
                      " ".join(made[name]))
        return text.split()

    headers = words("widl_headers")
    for header in headers:
        (out / header).parent.mkdir(parents=True, exist_ok=True)
    build([[cc[0].rsplit("-", 1)[0] + "-widl", f"-I{checkout / 'include'}", "-h", "-o", out / header,
            checkout / Path(header).with_suffix(".idl")] for header in headers])
    # configure's results: each macro the runner's sources test has a fallback for a compiler without it
    (out / "include" / "config.h").write_text("")
    exe = out / "shader_runner.exe"
    # the crosstest rule's libraries, the toolchain's import library of d3d12.dll standing for crosslibs/d3d12
    build([cc + words("CROSS_CFLAGS") + ["-w", "-o", exe, *words("shader_runner_cross_sources"),
                                         "-ld3d12", "-ldxgi", "-lgdi32", "-ld3dcompiler_47"]])
    for dll in dxcompiler.glob("*.dll"):
        (out / dll.name).unlink(missing_ok=True)
        (out / dll.name).symlink_to(dll)
    # the drive the prefix maps the file system's root to, for the files' paths
    root = next(d.name for d in (Path(env["WINEPREFIX"]) / "dosdevices").iterdir() if os.readlink(d) == "/")
    tests = checkout / "tests"
    names = [str(Path(path).relative_to("tests")) for path in words("vkd3d_shader_tests")]
    return names, lambda executor: lambda name: (
        [exe, f"{root}{tests / name}", "--filter-executor", executor], {"VKD3D_TEST_PLATFORM": "windows"})


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


def site(text, files):
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
    return f"{module}: {run.named(files[module], address - base)}"


def arguments(parser):
    parser.add_argument("--wine-source", type=Path, help="a Wine source tree, for Wine's Direct3D tests")
    parser.add_argument("--wine-build", type=Path, help="the build tree made from --wine-source")
    parser.add_argument("--vkd3d-proton", type=Path, help="a checkout of vkd3d-proton, for its Direct3D 12 tests")
    parser.add_argument("--vkd3d", type=Path, help="a checkout of vkd3d, for its shader tests (needs --dxcompiler)")
    parser.add_argument("--dxcompiler", type=Path, help="the directory of DXC's dxcompiler.dll for the build's machine")
    parser.add_argument("--update", action="store_true", help="record this run's verdicts in tests/conformance")
    parser.add_argument("--seconds", type=int, default=120, help="how long one test may run (default: %(default)s)")
    parser.add_argument("--only", action="append", default=[], help="only this suite, test, or suite:test (may repeat)")
    parser.add_argument("--skip", action="append", default=[], help="not this suite, test, or suite:test (may repeat)")


def conformance(args, builds, binary, base):
    """runs the suites the arguments name; the number of verdicts that differ from the recorded ones"""
    out = builds[0] / "conformance"
    pe = {path.name: path for path in run.libraries(builds)[1].values()}
    suites = {}
    if args.wine_source and args.wine_build:
        for dll in WINE_TESTS:
            suites[f"wine-{dll}"] = wine_suite(dll, args.wine_source.resolve(), args.wine_build.resolve(), out / "wine")
    if args.vkd3d_proton:
        suites["vkd3d-proton-d3d12"] = vkd3d_suite(args.vkd3d_proton.resolve(), builds, binary, base, out / "vkd3d-proton")
    if args.vkd3d:
        if not args.dxcompiler:
            sys.exit("--vkd3d needs --dxcompiler: its shader model 6 tests fail without DXC")
        names, command = shader_suite(args.vkd3d.resolve(), args.dxcompiler.resolve(), builds, base, out / "vkd3d")
        for executor in ("d3d12", "d3d11"):
            suites[f"vkd3d-shader-{executor}"] = names, command(f"{executor}.dll")
    named = lambda suite, name, names: any(n in (suite, name, f"{suite}:{name}") for n in names)
    wanted = lambda suite, name: (not args.only or named(suite, name, args.only)) and not named(suite, name, args.skip)
    changed = 0
    for suite, (names, command) in suites.items():
        names = [name for name in names if wanted(suite, name)]
        logs = out / "logs" / suite
        logs.mkdir(parents=True, exist_ok=True)

        def one(name):
            argv, env = command(name)
            (logs / name).parent.mkdir(parents=True, exist_ok=True)
            status = run.finish([binary, *argv], dict(base, **env), args.seconds, logs / f"{name}.txt")
            text = (logs / f"{name}.txt").read_text(errors="replace")
            word, at = verdict(status, text), ""
            files = dict(pe, **{Path(argv[0]).name.lower(): argv[0]})
            if word == "timeout":
                # what went wrong under it, and where its threads are (run.where)
                at = "; ".join(run.faults(text.splitlines())[:1] + re.findall(r"^stuck: (.*)$", text, re.M)[-1:])
            if word == "crashed":
                run.finish([binary, *argv], dict(base, **env, WINEDEBUG="+loaddll"), args.seconds, logs / f"{name}.crash.txt")
                at = site((logs / f"{name}.crash.txt").read_text(errors="replace"), files)
            failed = FAILED.findall(text)
            found = {"verdict": word, "at": at, "skipped": sorted(set(SKIPPED.findall(text))), "failed": len(failed),
                     "first": failed[0] if failed else ""}
            return {key: value for key, value in found.items() if value}

        with ThreadPoolExecutor(args.jobs) as pool:
            ran = dict(zip(names, pool.map(one, names)))
        got = {name: found["verdict"] for name, found in ran.items()}
        record = RECORDS / f"{suite}.json"
        recorded = json.loads(record.read_text()) if record.exists() else {}
        for name in names:
            was, skipped = recorded.get(name, {}), ran[name].get("skipped", [])
            if (got[name], skipped) != (was.get("verdict"), was.get("skipped", [])):
                changed += 1
                print(f"{suite:14}  {name:48} {was.get('verdict', 'unrecorded')} -> {got[name]}  "
                      f"{ran[name].get('at', '')}  skipped: {skipped}", flush=True)
        if args.update:
            kept = {name: entry for name, entry in recorded.items() if name not in ran}
            RECORDS.mkdir(exist_ok=True)
            record.write_text(json.dumps(dict(sorted({**kept, **ran}.items())), indent=1) + "\n")
        counts = {word: sum(v == word for v in got.values()) for word in sorted(set(got.values()))}
        print(f"{suite:14}  {len(got)} ran: " + ", ".join(f"{n} {word}" for word, n in counts.items()), flush=True)
    print(f"{changed} verdicts differ from tests/conformance; logs in {out / 'logs'}")
    return changed


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    run.arguments(parser)
    arguments(parser)
    # run.options() has it too, so tests/mullion-test, which takes both, gets it from there
    parser.add_argument("--jobs", type=int, default=1, help="how many tests run at once (default: %(default)s)")
    args = parser.parse_args()
    with run.session(args) as runtime:
        sys.exit(conformance(args, *runtime) != 0)


if __name__ == "__main__":
    main()
