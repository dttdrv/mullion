#!/usr/bin/env python3
"""runs the build's Direct3D tests through other translation layers beside Mullion, and sorts where they disagree.

a test of Mullion's says what Direct3D should give and checks it, so it runs as it is on any implementation: DXVK,
D3DMetal, a Mullion of another revision. a lane is a Wine and, optionally, a directory of DLLs that stand in for its
own: they are copied beside a copy of each test's program and loaded first (WINEDLLOVERRIDES n,b), as a
launcher stages DLLs beside a game. the verdicts are then sorted:

- Mullion alone fails: the others give what the test asks, so the fault is likely Mullion's (or Metal's under it);
  these come first.
- every lane fails: the test asks for something no implementation here gives, or Wine's part is wrong.
- Mullion alone passes: another lane's fault, worth knowing where a game differs between routes.

the first lane is the one compared with the others ("mullion" unless named otherwise). usage:

  tests/lanes.py --build build \\
      --lane mullion=<Wine Contents with the build's libraries installed (run.py --clone)> \\
      --lane dxvk=<Wine Contents>:<directory of DXVK's d3d11.dll and dxgi.dll> \\
      [--lane d3dmetal=<x86_64 Wine>:<D3DMetal's x86_64-windows>:<its prefix>] [--prefix PREFIX] [--jobs N] d3d11_

a lane's own prefix (its third field) is for a Wine that cannot share the tests' (an x86_64 Wine under Rosetta).
tests are named by prefix, as run.py's are; window tests and the timing suite are left out.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import run as runner


def verdict(code, output):
    last = next((line for line in reversed(output.splitlines()) if line.strip()), "")
    if code == 77 or last.startswith("skipped"):
        return "skipped", last
    if code == 0 and not last.startswith("failed"):
        return "passed", last
    return "failed", last or f"exit {code}"


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--lane", action="append", required=True, help="NAME=WINE[:DLL DIRECTORY[:PREFIX]]")
    parser.add_argument("--prefix", type=Path, help="default: tests-prefix in the build, as run.py's")
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--work", type=Path, default=None, help="where the lanes' copies go (default: build/lanes)")
    parser.add_argument("tests", nargs="+", help="test name prefixes")
    args = parser.parse_args()
    build = args.build.resolve()
    prefix = (args.prefix or build / "tests-prefix").resolve()
    work = (args.work or build / "lanes").resolve()
    lanes = []
    for spec in args.lane:
        name, _, rest = spec.partition("=")
        wine, *more = rest.split(":")
        dlls = Path(more[0]).expanduser().resolve() if more and more[0] else None
        lane_prefix = Path(more[1]).expanduser().resolve() if len(more) > 1 else prefix
        binary = Path(wine).expanduser() / "bin" / "wine"
        if not binary.exists():
            sys.exit(f"lane {name}: no {binary}")
        lanes.append((name, binary, dlls, lane_prefix))
    tests = [t for t in json.load(open(build / "meson-info" / "intro-tests.json"))
             if any(t["name"].startswith(p) for p in args.tests)
             and not {"window", "timing"} & set(t.get("suite", []))]
    if not tests:
        sys.exit("no test is named so")

    def run(test, lane):
        name, binary, dlls, lane_prefix = lane
        exe = Path(test["cmd"][0])
        here = work / name / exe.parent.name
        here.mkdir(parents=True, exist_ok=True)
        local = here / exe.name
        if not local.exists() or local.stat().st_mtime < exe.stat().st_mtime:
            shutil.copy2(exe, local)
        overrides = ["d3dcompiler_47=n,b", "winedbg.exe=d"]
        if dlls:
            names = sorted(p.stem for p in dlls.glob("*.dll"))
            for dll in dlls.glob("*.dll"):
                target = here / dll.name
                if not target.exists():
                    shutil.copy2(dll, target)
            overrides.insert(0, ",".join(names) + "=n,b")
        env = dict(os.environ, WINEPREFIX=str(lane_prefix), WINEDEBUG="-all", WINEDLLOVERRIDES=";".join(overrides),
                   **test.get("env", {}))
        try:
            # one GPU job at a time on the machine, as run.py's tests (MULLION_TEST_LOCK)
            with runner.alone():
                done = subprocess.run([binary, local, *test["cmd"][1:]], env=env, cwd=here, capture_output=True,
                                      text=True, timeout=test.get("timeout") or 600)
            return verdict(done.returncode, done.stdout)
        except subprocess.TimeoutExpired:
            return "failed", "outlived its time"

    results = {}
    with ThreadPoolExecutor(args.jobs) as pool:
        futures = {(t["name"], lane[0]): pool.submit(run, t, lane) for t in tests for lane in lanes}
        for key, future in futures.items():
            results[key] = future.result()
    first = lanes[0][0]
    others = [lane[0] for lane in lanes[1:]]
    groups = {"alone": [], "all": [], "only": [], "mixed": []}
    width = max(len(t["name"]) for t in tests)
    print(f"{'':{width}}  " + "  ".join(f"{lane[0]:8}" for lane in lanes))
    for test in tests:
        row = [results[(test["name"], lane[0])][0] for lane in lanes]
        print(f"{test['name']:{width}}  " + "  ".join(f"{v:8}" for v in row))
        mine, theirs = row[0], [v for v in row[1:] if v != "skipped"]
        if mine == "failed" and theirs and all(v == "passed" for v in theirs):
            groups["alone"].append(test["name"])
        elif mine == "failed" and theirs and all(v == "failed" for v in theirs):
            groups["all"].append(test["name"])
        elif mine == "passed" and any(v == "failed" for v in theirs):
            groups["only"].append(test["name"])
        elif mine == "failed":
            groups["mixed"].append(test["name"])
    titles = {"alone": f"{first} alone fails (likely its fault)", "all": "every lane fails",
              "only": f"{first} passes where {', '.join(others)} fail(s)", "mixed": f"{first} fails, the others disagree"}
    for key, title in titles.items():
        if groups[key]:
            print(f"\n{title}:")
            for name in groups[key]:
                print(f"  {name}: " + "; ".join(f"{lane[0]}: {results[(name, lane[0])][1][:100]}" for lane in lanes))
    return 0


if __name__ == "__main__":
    sys.exit(main())
