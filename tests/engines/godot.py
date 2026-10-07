#!/usr/bin/env python3
"""runs Godot's renderer on the build's Direct3D 12 and judges every frame against the same frame from Godot's own
Vulkan driver on this machine (MoltenVK in Godot's macOS build), with its Metal driver as a second opinion.

the scene (tests/engines/godot: built in code, each frame a function of its number) goes through the renderer's
features a phase at a time. Godot writes each frame as a PNG (--write-movie, at --fixed-fps, for --quit-after
frames); the Windows build runs in the tests' Wine with --rendering-driver d3d12, the macOS build beside it with
--rendering-driver vulkan. the drivers are given the same SPIR-V and the same commands by one renderer, so a frame
that is not the same picture is the translation's doing. the same picture is not the same bytes (two shader
compilers, two orders of float operations): a frame passes when its mean difference and its share of clearly
different pixels are no more than twice what two runs of the Vulkan driver differ by themselves, plus a floor.
Godot's Metal driver draws some of the scene differently from its Vulkan driver (the frames with global
illumination and fog, here); how far the two are apart is printed and judged by nobody.
the Windows build has to end by itself once it has written its frames: a run that is still there after --seconds is
a wrong result of its own. it is Godot's own executable that is run, not its console wrapper: the wrapper waits
until every process of its job has gone, and in a Wine the processes Wine itself starts for a first window
(explorer.exe) are in that job and stay.

needs Pillow and NumPy. --godot names a directory with the two official builds unpacked (Godot_v*_win64.exe and Godot.app); they are not part of the tree.
"""
import argparse
import shutil
import sys
from pathlib import Path

import numpy
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import run

SCENE = Path(__file__).resolve().parent / "godot"
# the floor of a frame's allowance: a mean difference of one step of 8 bits, and one pixel in a thousand that
# differs by more than CLEAR steps
MEAN, SHARE, CLEAR = 1.0, 0.001, 24


def frames(directory):
    return sorted(directory.glob("frame*.png"))


def differ(a, b):
    """how two frames differ: the mean absolute difference in 8-bit steps, and the share of pixels with a channel
    more than CLEAR apart"""
    x, y = (numpy.asarray(Image.open(f).convert("RGB"), dtype=numpy.int16) for f in (a, b))
    if x.shape != y.shape:
        return float("inf"), 1.0
    apart = numpy.abs(x - y)
    return float(apart.mean()), float((apart.max(axis=2) > CLEAR).mean())


def record(command, env, out, args, log):
    """one run of the scene into `out`; the frames it wrote"""
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    status = run.finish(command, env, args.seconds, log)
    return status, frames(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    run.arguments(parser)
    parser.add_argument("--godot", required=True, type=Path, help="the directory with Godot's Windows and macOS builds")
    parser.add_argument("--out", required=True, type=Path, help="where the frames, logs and differences go")
    parser.add_argument("--frames", type=int, default=216, help="frames to draw (default: %(default)s, the scene's nine phases)")
    parser.add_argument("--seconds", type=int, default=600, help="the most a run may take (default: %(default)s)")
    args = parser.parse_args()
    builds, binary, base = run.environment(args)
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    windows = next(args.godot.glob("Godot_v*_win64.exe")).resolve()
    native = (args.godot / "Godot.app" / "Contents" / "MacOS" / "Godot").resolve()

    def scene(name):
        # a copy of its own: Godot keeps a cache beside the project
        shutil.rmtree(out / name / "scene", ignore_errors=True)
        shutil.copytree(SCENE, out / name / "scene")
        return out / name / "scene"

    def options(movie):
        return ["--rendering-method", "forward_plus", "--audio-driver", "Dummy", "--windowed", "--resolution", "640x360",
                "--fixed-fps", "30", "--quit-after", str(args.frames), "--write-movie", movie]

    z = lambda path: "Z:" + str(path).replace("/", "\\")
    runs = {}
    for name, driver in (("vulkan", "vulkan"), ("vulkan-again", "vulkan"), ("metal", "metal")):
        runs[name] = record([native, "--path", scene(name), "--rendering-driver", driver, *options(str(out / name / "frames" / "frame.png"))],
                            dict(base), out / name / "frames", args, out / f"{name}.txt")
    runs["d3d12"] = record([binary, windows, "--path", z(scene("d3d12")), "--rendering-driver", "d3d12", *options(z(out / "d3d12" / "frames" / "frame.png"))],
                           base, out / "d3d12" / "frames", args, out / "d3d12.txt")
    for name, (status, written) in runs.items():
        print(f"{name}: exit status {status}, {len(written)} frames", flush=True)
    reference, again, tested, second = (runs[name][1] for name in ("vulkan", "vulkan-again", "d3d12", "metal"))
    if len(reference) < args.frames or len(again) != len(reference):
        sys.exit("failed: the Vulkan driver's runs did not write the frames; no reference")
    wrong = []
    worst = (0.0, 0.0)
    for index, frame in enumerate(reference):
        noise = differ(frame, again[index])
        worst = max(worst, noise)
        if index >= len(tested):
            wrong.append((index, "not written"))
            continue
        mean, share = differ(frame, tested[index])
        if mean > 2 * noise[0] + MEAN or share > 2 * noise[1] + SHARE:
            wrong.append((index, f"mean {mean:.2f} and {share:.2%} of the pixels clearly different; two Vulkan runs: {noise[0]:.2f} and {noise[1]:.2%}"))
            # what differs, as a picture: the differences times 8
            x, y = (numpy.asarray(Image.open(f).convert("RGB"), dtype=numpy.int16) for f in (frame, tested[index]))
            if x.shape == y.shape:
                (out / "different").mkdir(exist_ok=True)
                Image.fromarray(numpy.clip(numpy.abs(x - y) * 8, 0, 255).astype(numpy.uint8)).save(out / "different" / frame.name)
    for index, why in wrong[:12]:
        print(f"wrong: frame {index} (phase {index // 24}): {why}")
    library = [line for line in (out / "d3d12.txt").read_text(errors="replace").splitlines() if run.faults([line]) or line.startswith("err:")]
    for line in library[:8]:
        print(f"library: {line[:300]}")
    print(f"two Vulkan runs differ by at most mean {worst[0]:.2f}, {worst[1]:.2%}")
    apart = [differ(a, b) for a, b in zip(reference, second)]
    if apart:
        print(f"Godot's Metal driver against its Vulkan driver: at most mean {max(a[0] for a in apart):.2f}, {max(a[1] for a in apart):.2%}; "
              f"{sum(a[0] > MEAN for a in apart)} frames over the floor, the first {next((i for i, a in enumerate(apart) if a[0] > MEAN), None)}")
    lingered = runs["d3d12"][0] is None
    if lingered:
        print(f"wrong: the Direct3D 12 run wrote its frames and was still there after {args.seconds} s")
    print(f"{'failed' if wrong or library or lingered else 'passed'}: {len(wrong)} of {len(reference)} frames are not the Vulkan driver's picture"
          + (f"; differences in {out / 'different'}" if wrong else ""))
    sys.exit(bool(wrong or library or lingered))


if __name__ == "__main__":
    main()
