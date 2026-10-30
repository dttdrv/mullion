#!/usr/bin/env python3
"""perf_probe: the D3D12 performance probe (tests/perf/d3d12_perf_probe.cpp) against a build of Mullion.

  tests/perf/perf_probe.py run --build BUILD --wine WINE [--prefix PREFIX] [--no-install] [--exe PROBE.exe]
                               [--prepare-exe PROBE.exe] [--label NAME] [--frames N] [--warmup N] [--scenes a,b,...]
                               [--repeat N] [--env K=V ...] [--out FILE.json]
  tests/perf/perf_probe.py compare BEFORE.json AFTER.json

run: each scene (or each of --scenes) in a process of its own, --repeat times, keeping the run with the lowest median
CPU time (the others are mostly thermal or scheduling noise), and every value the probe printed into a JSON file
(default: BUILD/perf/NAME.json). the Wine and its prefix are the tests' (tests/run.py): the build's libraries are
installed into the Wine, as mullion-test does, unless --no-install.

the probe is the build's own (BUILD/tests/perf/d3d12_perf_probe.exe), of the build's machine: an x86_64 build's in an
x86_64 Wine, an ARM64EC build's (build-arm64ec.txt) in an arm64 Wine, natively. --exe runs another, such as an x86_64
build's probe in an arm64 Wine that emulates x86_64, the way x86_64 games run there. a probe whose prefix has no
dxcompiler.dll of its machine (that one) takes its DXIL scenes' shaders from a cache that --prepare-exe, a probe of the
Wine's own machine, fills first.

the probe compiles its shaders as the tests do: DXBC with the prefix's d3dcompiler_47.dll (Microsoft's, as from
winetricks: Wine's own lacks some of what the scenes use), DXIL with its dxcompiler.dll. a scene whose compiler fails
is reported as FAILED with the compiler's message.

GPU times drift between runs (thermal state, clocks): compare a change by alternating the builds, a run of each at a
time, more than once.

compare: per scene, the headline values of two runs and their change.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import run as tests  # noqa: E402  (tests/run.py: the builds, the Wine, its prefix)

# the variables a run is recorded with
RECORDED = ("MULLION_", "DXMT_", "MTL_", "WINE", "PROBE_")


def probe_machine(exe, build):
    """the probe's machine: its build's host (an ARM64EC PE names x86_64 in its header), or, for one from elsewhere,
    its header's"""
    machines = build / "meson-info" / "intro-machines.json"
    if exe.is_relative_to(build) and machines.exists():
        return json.loads(machines.read_text())["host"]["cpu_family"]
    return tests.machine(exe)


def windows_path(path):
    return "Z:" + str(Path(path).resolve()).replace("/", "\\")


def list_scenes(wine, exe, env):
    """the probe's scene names, and the set of those that need DXIL (tagged [dxil])"""
    out = subprocess.run([wine, exe, "list"], env=env, capture_output=True, text=True, timeout=120).stdout
    names, dxil = [], set()
    for line in out.replace("\r", "").splitlines():
        m = re.match(r"^([a-z_0-9]+)\s+(\[dxil\])?", line)
        if m and not line.startswith(("info:", "warn")):
            names.append(m.group(1))
            if m.group(2):
                dxil.add(m.group(1))
    return names, dxil


def prepare_dxil(wine, exe, scenes, env, timeout, frames, warmup, salt):
    """fills the DXIL cache: `exe`, which can load dxcompiler.dll, sets each scene up once"""
    for scene in scenes:
        p = subprocess.run([wine, exe, "prepare", scene, str(frames), str(warmup), str(salt)], env=env, capture_output=True,
                           text=True, timeout=timeout)
        if "D3D12_PERF_PROBE_OK" not in p.stdout:
            tail = "\n".join(l for l in (p.stdout + p.stderr).replace("\r", "").splitlines() if "error" in l)[-600:]
            print("%-18s DXIL prepare failed\n%s" % (scene, tail), flush=True)


def parse(text):
    r = {}
    for kind in ("cpu", "gpu"):
        m = re.search(kind + r" frames (\d+) mean ([\d.]+) median ([\d.]+) p95 ([\d.]+) max ([\d.]+) ms", text)
        if m:
            for name, value in zip(["frames", "mean", "median", "p95", "max"], m.groups()):
                r["%s_%s" % (kind, name)] = float(value)
    m = re.search(r"first ([\d.]+) ms", text)
    if m:
        r["first_ms"] = float(m.group(1))
    for name, value in re.findall(r"^extra (\w+)=([-\d.]+)", text, re.M):
        r[name] = float(value)
    r["ok"] = "D3D12_PERF_PROBE_OK" in text
    return r


def run_scene(wine, exe, scene, frames, warmup, env, timeout, salt):
    started = time.time()
    try:
        p = subprocess.run([wine, exe, scene, str(frames), str(warmup), str(salt)], env=env, capture_output=True, text=True,
                           timeout=timeout)
        text = (p.stdout + p.stderr).replace("\r", "")
    except subprocess.TimeoutExpired as e:
        text = (e.stdout.decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")) + "\nTIMEOUT"
    r = parse(text)
    r["wall_s"] = round(time.time() - started, 1)
    if not r["ok"]:
        r["log_tail"] = "\n".join(l for l in text.splitlines() if not l.startswith(("info:", "fixme")))[-1500:]
    return r


def cmd_run(args):
    builds, wine, env = tests.environment(args)
    exe = Path(args.exe).resolve() if args.exe else builds[0] / "tests" / "perf" / "d3d12_perf_probe.exe"
    if not exe.exists():
        sys.exit(f"{exe}: no probe (a build set up with -Denable_tests=true and -Denable_d3d12=true makes one)")
    for kv in args.env or []:
        k, v = kv.split("=", 1)
        env[k] = v
    cache = builds[0] / "perf" / "dxilcache"
    cache.mkdir(parents=True, exist_ok=True)
    env.setdefault("PROBE_DXIL_CACHE", windows_path(cache))
    machine = probe_machine(exe, builds[0])
    print(f"probe {exe} ({machine}), Wine {wine}, prefix {env['WINEPREFIX']}", flush=True)
    all_scenes, dxil_scenes = list_scenes(wine, exe, env)
    scenes = args.scenes.split(",") if args.scenes else all_scenes
    # one salt per run: the cold compile scenes get shaders no earlier run (or shader cache) has seen
    salt = int(time.time()) % 100000
    if args.prepare_exe:
        prepare_dxil(wine, Path(args.prepare_exe).resolve(), [s for s in scenes if s in dxil_scenes], env, args.timeout,
                     args.frames, args.warmup, salt)
    data = {"label": args.label, "exe": str(exe), "machine": machine, "wine": str(wine),
            "builds": [str(b) for b in builds], "frames": args.frames, "warmup": args.warmup,
            "env": {k: env[k] for k in sorted(env) if k.startswith(RECORDED)},
            "time": time.strftime("%Y-%m-%d %H:%M:%S"), "scenes": {}}
    for scene in scenes:
        runs = [run_scene(wine, exe, scene, args.frames, args.warmup, env, args.timeout, salt) for _ in range(args.repeat)]
        cpu = [r.get("cpu_median") for r in runs if r.get("ok") and isinstance(r.get("cpu_median"), (int, float))]
        mean = sum(cpu) / len(cpu) if cpu else None
        variance = (sum((v - mean) ** 2 for v in cpu) / len(cpu)) if cpu and mean is not None else None
        data["scenes"][scene] = {
            "repeats": runs,
            "ok": any(r.get("ok") for r in runs),
            "failures": sum(0 if r.get("ok") else 1 for r in runs),
            "cpu_mean": mean,
            "cpu_variance": variance,
            "cpu_median": sorted(cpu)[len(cpu) // 2] if cpu else None,
            "cpu_min": min(cpu) if cpu else None,
            "cpu_max": max(cpu) if cpu else None,
        }
        row = data["scenes"][scene]
        if row["ok"]:
            print("%-22s cpu mean %7.3f ms (median %7.3f, var %7.3f, %u/%u ok)" % (
                scene, row["cpu_mean"] or 0, row["cpu_median"] or 0, row["cpu_variance"] or 0,
                args.repeat - row["failures"], args.repeat), flush=True)
        else:
            print("%-22s FAILED all %u repeats\n%s" % (scene, args.repeat, runs[-1].get("log_tail", "")), flush=True)
    out = Path(args.out) if args.out else builds[0] / "perf" / f"{args.label}.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(data, indent=1))
    print("wrote", out)


def cmd_compare(args):
    before, after = (json.loads(Path(p).read_text()) for p in (args.before, args.after))
    print("%-22s %-28s %10s %10s %8s" % ("scene", "value", before["label"][:10], after["label"][:10], "change"))
    for scene, b in before["scenes"].items():
        a = after["scenes"].get(scene)
        if not a:
            continue
        keys = ["cpu_median", "cpu_p95", "gpu_median"] + [k for k in b if k not in ("ok", "wall_s", "frames", "log_tail", "first_ms")
                                                         and not k.startswith(("cpu_", "gpu_"))]
        for k in keys:
            if k in b and k in a and isinstance(b[k], (int, float)) and b[k]:
                print("%-22s %-28s %10.3f %10.3f %+7.1f%%" % (scene, k, b[k], a[k], 100.0 * (a[k] - b[k]) / b[k]))
        print()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    run = sub.add_parser("run")
    tests.arguments(run)
    run.add_argument("--exe", help="the probe to run (default: the first build's)")
    run.add_argument("--prepare-exe", help="a probe that can load dxcompiler.dll, which fills the DXIL cache first")
    run.add_argument("--label", default="baseline")
    run.add_argument("--frames", type=int, default=200)
    run.add_argument("--warmup", type=int, default=30)
    run.add_argument("--repeat", type=int, default=1)
    run.add_argument("--timeout", type=int, default=240)
    run.add_argument("--scenes")
    run.add_argument("--env", action="append", help="K=V for the probe's environment (may repeat)")
    run.add_argument("--out")
    cmp = sub.add_parser("compare")
    cmp.add_argument("before")
    cmp.add_argument("after")
    args = parser.parse_args()
    cmd_run(args) if args.cmd == "run" else cmd_compare(args)


if __name__ == "__main__":
    sys.exit(main())
