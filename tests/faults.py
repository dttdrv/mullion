#!/usr/bin/env python3
"""whether each fault of tests/breaks makes its test give a new answer.

a fault is tests/breaks/<test>[.<n>].patch: a change to the source that the test must notice. its `# family <f>` lines
name the families it runs as, the GPU's own when it has none. the source is copied beside the first build, the patch
applied, and built with that build's options; run.py then runs the test, every front end of it, on that build. a fault
is caught when the test then gives an answer it does not give on the first build, unfaulted: an answer it had before
is not the fault's. a test that stops early without the fault cannot tell a fault from what it stopped at.

  faults.py --build <library build> --build <tests build> --wine <wine> [--prefix <prefix>] <test or patch> ...
"""
import argparse
import ast
import configparser
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

import run

SOURCE = Path(__file__).resolve().parents[1]
RUN = Path(__file__).with_name("run.py")


def patches(names):
    """the faults the arguments name: a patch file as it is, a test as all of its patches"""
    found = []
    for name in names:
        found += [Path(name)] if name.endswith(".patch") else sorted(
            p for p in (SOURCE / "tests" / "breaks").glob(f"{name}*.patch")
            if re.fullmatch(rf"{re.escape(name)}(\.\d+)?\.patch", p.name))
    return found or sys.exit("no such fault")


def setup(library, faulted, source):
    """a build of the faulted source with the library build's options and machine files"""
    if (faulted / "build.ninja").exists():
        return 0
    line = configparser.ConfigParser()
    line.read(library / "meson-private" / "cmd_line.txt")
    files = [f"--{key.replace('_', '-')}={path}" for key in ("cross_file", "native_file")
             for path in ast.literal_eval(line.get("properties", key, fallback="[]"))]
    options = [f"-D{key}={value}" for key, value in line["options"].items()]
    return subprocess.run(["meson", "setup", faulted, source, *files, *options], capture_output=True).returncode


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    run.arguments(parser)
    parser.add_argument("faults", nargs="+", help="tests, whose patches all run, or patch files")
    args = parser.parse_args(argv)
    # the runs it starts install into the copy, never into the Wine itself
    run.cloned(args)
    library = args.build[0].resolve()
    source, faulted = library.with_name(library.name + "-fault-src"), library.with_name(library.name + "-fault")
    declared = {t["name"] for build in args.build for path in [build / "meson-info" / "intro-tests.json"] if path.exists()
                for t in json.loads(path.read_text())}
    common = [*[a for b in args.build[1:] for a in ("--build", b)], "--wine", args.wine,
              *(["--prefix", args.prefix] if args.prefix else []), *(["--validate"] if args.validate else [])]
    # what the unfaulted build answers, once per test and families in each run, as the source may have changed since
    answers = library.with_name(library.name + "-fault-answers")
    shutil.rmtree(answers, ignore_errors=True)
    answers.mkdir()
    missed = 0
    for patch in patches(args.faults):
        test = re.sub(r"(\.\d+)?\.patch$", "", patch.name)
        families = ",".join(re.findall(r"^# family (\S+)", patch.read_text(), re.M) or ["native"])
        names = sorted(declared & {test, test + "_dxil", test + "_dxbc"})
        before = answers / f"{test}-{families}.txt"
        # by content, not times: a file the last fault changed comes back newer than its objects, so it is rebuilt
        subprocess.run(["rsync", "-rlpc", "--delete", "--exclude", ".git", f"{SOURCE}/", f"{source}/"], check=True)
        if subprocess.run(["git", "apply", patch.resolve()], cwd=source, capture_output=True).returncode:
            said = "does not apply"
        elif setup(library, faulted, source) or subprocess.run(["ninja", "-C", faulted], capture_output=True).returncode:
            said = "does not build"
        elif not names:
            said = f"no test {test}"
        else:
            if not before.exists():
                out = subprocess.run([sys.executable, RUN, "--build", library, *common, "--family", families, "--record",
                                      before, "--exact", *names], capture_output=True, text=True)
            if before.exists():
                out = subprocess.run([sys.executable, RUN, "--build", faulted, *common, "--family", families, "--known",
                                      before, "--exact", *names], capture_output=True, text=True)
            new = [line[5:] for line in out.stdout.splitlines() if line.startswith("new: ")]
            if new:
                print(f"{patch.name}: caught, {new[0]}", flush=True)
                continue
            said = "NOT CAUGHT" if " known, each with its cause" in out.stdout else "did not run: " + out.stderr.strip()
        missed += 1
        print(f"{patch.name}: {said}", flush=True)
    return missed


if __name__ == "__main__":
    sys.exit(main() != 0)
