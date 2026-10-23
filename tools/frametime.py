#!/usr/bin/env python3
"""measures a frame's cost through Mullion's own D3D11 and D3D12 paths, as each GPU family.

the harness is tests/frametime/frametime.cpp: one swap chain, a constant-buffer update and two pipeline changes a
frame, presents every frame, and prints one line a frame with its CPU time. this runs it the way tests/run.py runs
the build's tests (the same prefix, the same native libraries, the same family switch) and reads the GPU's own frame
intervals from Mullion's profile records with tools/mullion-diag.

  tools/frametime.py --build build --wine ../toolchains/wine --prefix ../prefix --family native,9 --repeats 3

prints, a row an API and family, the median and p95 of the CPU frame time over the repeats (a repeat's own figure,
then the worst of the repeats) and the GPU's median and longest frame interval from mullion-diag. --json writes the
same numbers for comparing two runs.
"""
import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tests"))
import run  # noqa: E402  the runner's own prefix, library install and family environment


def environment(args):
    """the prefix and environment the build's tests use, with its libraries installed"""
    args.prefix = Path(args.prefix).resolve() if args.prefix else None
    namespace = argparse.Namespace(build=[Path(args.build).resolve()], wine=Path(args.wine).resolve(),
                                   prefix=args.prefix, validate=False, no_install=False)
    _builds, binary, base = run.environment(namespace)
    return binary, base


def one(binary, base, exe, api, family, frames, warmup, draws, diag):
    """runs the harness once: its per-frame CPU times, and what mullion-diag makes of the GPU's intervals"""
    env = dict(base)
    env.pop("MULLION_GPU_FAMILY", None)
    if family != "native":
        env["MULLION_GPU_FAMILY"] = family
    if diag:
        env["MULLION_DIAG"] = "profile"
        env["MULLION_DIAG_PATH"] = str(diag)
    path = ROOT / "tools" / "mullion-diag"
    result = subprocess.run([str(binary), str(exe), api, str(frames), str(warmup), str(draws)], env=env,
                            capture_output=True, text=True, timeout=1800)
    cpu = [float(line.split()[2]) for line in result.stdout.splitlines() if line.startswith("frame ")]
    summary = next((line for line in result.stdout.splitlines() if line.startswith("result cpu ")), "")
    if result.returncode != 0 or not cpu:
        raise SystemExit(f"{api} on {family} failed ({result.returncode}): "
                         f"{(summary or result.stdout or result.stderr).strip()[:400]}")
    fields = dict(re.findall(r"(\w+)=([0-9.]+)", summary))
    gpu = {}
    if diag:
        said = subprocess.run([sys.executable, str(path), "frames", str(diag), "--from", str(warmup)],
                              capture_output=True, text=True)
        gpu = {name: float(value) for name, value in re.findall(r"(median|longest) ([0-9.]+) ms", said.stdout)}
    return {"cpu_median": float(fields["median"]), "cpu_p95": float(fields["p95"]),
            "cpu_p99": float(fields["p99"]), "cpu_min": float(fields["min"]),
            "gpu_median": gpu.get("median"), "gpu_longest": gpu.get("longest"), "frames": len(cpu)}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", required=True, help="the build directory with tests/frametime/frametime.exe")
    parser.add_argument("--wine", required=True, help="a Wine's installation: the directory with bin/wine")
    parser.add_argument("--prefix", help="the Wine prefix of the tests (made if missing)")
    parser.add_argument("--family", default="native,9", help="the Apple GPU families to run as (default: %(default)s)")
    parser.add_argument("--api", default="d3d11,d3d12", help="the APIs to run (default: %(default)s)")
    parser.add_argument("--frames", type=int, default=600, help="frames measured after warmup (default: %(default)s)")
    parser.add_argument("--warmup", type=int, default=60, help="frames not measured (default: %(default)s)")
    parser.add_argument("--draws", type=int, default=128, help="draws a frame, half of them changing pipeline state (default: %(default)s)")
    parser.add_argument("--repeats", type=int, default=3, help="runs of each API and family (default: %(default)s)")
    parser.add_argument("--json", type=Path, help="write the numbers here")
    args = parser.parse_args()
    exe = Path(args.build).resolve() / "tests" / "frametime" / "frametime.exe"
    if not exe.exists():
        sys.exit(f"not built: {exe} (meson compile -C {args.build})")
    binary, base = environment(args)
    table, rows = {}, []
    for api in args.api.split(","):
        for family in args.family.split(","):
            runs = []
            for _ in range(args.repeats):
                with tempfile.TemporaryDirectory(prefix="frametime-") as directory:
                    runs.append(one(binary, base, exe, api, family, args.frames, args.warmup, args.draws, Path(directory)))
            medians = [r["cpu_median"] for r in runs]
            gpu = [r["gpu_median"] for r in runs if r["gpu_median"] is not None]
            row = {"api": api, "family": family, "runs": runs,
                   "cpu_median": statistics.median(medians), "cpu_median_spread": max(medians) - min(medians),
                   "cpu_p95": max(r["cpu_p95"] for r in runs),
                   "cpu_p99": max(r["cpu_p99"] for r in runs),
                   "gpu_median": statistics.median(gpu) if gpu else None,
                   "gpu_longest": max(r["gpu_longest"] for r in runs if r["gpu_longest"] is not None) if gpu else None}
            rows.append(row)
            table[api, family] = row
            print(f"{api:>5}  {family:>6}  cpu median {row['cpu_median']:7.3f} ms (spread {row['cpu_median_spread']:.3f})"
                  f"  p95 {row['cpu_p95']:7.3f}  p99 {row['cpu_p99']:7.3f}"
                  + (f"  gpu median {row['gpu_median']:7.3f}  longest {row['gpu_longest']:7.3f}"
                     if row["gpu_median"] is not None else ""), flush=True)
    if args.json:
        args.json.write_text(json.dumps(rows, indent=2) + "\n")
        print(f"wrote {args.json}")


if __name__ == "__main__":
    main()
