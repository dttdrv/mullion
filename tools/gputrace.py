#!/usr/bin/env python3
"""Read a Metal GPU trace from a script, through gpudebug(1) (macOS 27 and later).

  report TRACE [--profile] [--json OUT]   what the trace holds: its command buffers and encoders, each render
                                          attachment's texture (format, usage, storage and compression), and with
                                          --profile the GPU's cost ranking of encoders and shaders and its counters
  fetch-final TRACE --out PNG             the last render pass's first color attachment: what a frame presented, for
                                          comparing one lane's image with another's (diff)
  capture --out T.gputrace [--name N] [--mullion [--first F] [--count C]] -- CMD...
                                          runs CMD and captures its Metal process named N (default: CMD's last
                                          argument's base name): with --mullion, Mullion captures command buffers F to
                                          F+C-1 from inside it; without, gpucapture attaches from outside and captures
                                          until it exits, which needs a debuggable process
  diff A.png B.png [--tolerance T]        how two images differ: the share of pixels off by more than T per channel
                                          (0..255), the largest difference, and the bounding box of the pixels that differ

A trace comes from Mullion's own capture (capture --mullion, or MULLION_CAPTURE_TRIGGER and the other MULLION_CAPTURE_*
options, docs/DEBUGGING.md section 5), which works in any Wine, or from gpucapture(1) attached from outside, which
works only for a debuggable process (com.apple.security.get-task-allow): not a Wine signed for distribution.
gpudebug replays the trace on this Mac; --profile needs an M3 or later. Nothing here needs Xcode.
"""
import argparse
import json
import re
import struct
import subprocess
import sys
import zlib
from pathlib import Path


class Session:
    """one gpudebug session on a trace: commands sent one at a time, so a missing node fails only its command"""

    def __init__(self, trace, out=None, timeout=900):
        args = ["gpudebug", "-t", str(trace), "-q", "--json", "--timeout", str(timeout), "-c", "status"]
        if out:
            args[1:1] = ["-o", str(out)]
        result = subprocess.run(args, capture_output=True, text=True)
        match = re.search(r"Session (\d+) created", result.stdout + result.stderr)
        if not match:
            raise RuntimeError(f"gpudebug did not open {trace}: {(result.stdout + result.stderr).strip()}")
        self.id = match[1]

    def run(self, *commands):
        """the JSON values the commands printed, or None for a command that failed"""
        values = []
        for command in commands:
            result = subprocess.run(["gpudebug", "-s", self.id, "-q", "--json", "-c", command],
                                    capture_output=True, text=True)
            values.append(last_json(result.stdout) if result.returncode == 0 else None)
        return values if len(values) != 1 else values[0]

    def close(self):
        subprocess.run(["gpudebug", "--terminate", self.id], capture_output=True)

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


def last_json(text):
    """the last JSON value in gpudebug's output: list prints one line, info a pretty-printed object"""
    decoder, at, value = json.JSONDecoder(), 0, None
    while True:
        start = text.find("{", at)
        if start < 0:
            return value
        try:
            value, at = decoder.raw_decode(text, start)
        except json.JSONDecodeError:
            at = start + 1


def children(listing):
    return (listing or {}).get("children", [])


def texts(child):
    return [v.get("value") for v in child.get("values", []) if v]


def walk_commands(session):
    """command buffers, their encoders, and each render encoder's attachments with their textures' properties"""
    buffers = []
    for cb in children(session.run("go /commands", "list --all")[1]):
        if not cb["name"].startswith("cb"):
            continue
        encoders = []
        for encoder in children(session.run(f"go /commands/{cb['name']}", "list --all")[1]):
            entry = {"name": encoder["name"], "summary": texts(encoder)}
            if encoder["name"].startswith("re"):
                attachments = []
                path = f"/commands/{cb['name']}/{encoder['name']}"
                for child in children(session.run(f"go {path}", "list --all")[1]):
                    if re.fullmatch(r"color\d+|depth|stencil", child["name"]):
                        info = session.run(f"go {path}", f"info {child['name']}")[1] or {}
                        attachments.append({"name": child["name"], **{key: info.get(key) for key in (
                            "label", "pixelFormat", "dimensions", "usage", "storageMode", "compressionType",
                            "loadAction", "storeAction", "allocatedSize")}})
                entry["attachments"] = attachments
            encoders.append(entry)
        buffers.append({"name": cb["name"], "summary": texts(cb), "encoders": encoders})
    return buffers


def textures(session, limit):
    """every texture the trace holds (resources/textures), with its format, usage, storage and compression"""
    result = []
    for child in children(session.run("go /resources/textures", "list --all")[1])[:limit]:
        info = session.run(f"info @{child['name']}") or {}
        result.append({"name": child["name"], **{key: info.get(key) for key in (
            "label", "pixelFormat", "dimensions", "usage", "storageMode", "compressionType", "allocatedSize")}})
    return result


def profile(session, top):
    """the GPU's cost ranking of encoders and shaders, and every counter's summary value"""
    if not session.run("profile run"):
        return {"error": "profiling failed (it needs an M3 or later)"}
    ranking = lambda path: [{"name": c["name"], "values": texts(c)}
                            for c in children(session.run(f"go {path}", "list --all")[1])][:top]
    counters = {}
    for group in children(session.run("go /performance/timeline/counters", "list --all")[1]):
        listing = session.run(f"go /performance/timeline/counters/{group['name']}", "list --all")[1]
        counters[group["name"]] = {c["name"]: (texts(c) or [None])[0] for c in children(listing)}
    return {"encoders": ranking("/performance/encoders"), "shaders": ranking("/performance/shaders"),
            "counters": counters}


def report(args):
    with Session(args.trace) as session:
        status = session.run("status") or {}
        buffers = walk_commands(session)
        result = {"trace": str(args.trace), "device": status.get("device"), "summary": status.get("summary"),
                  "command_buffers": buffers}
        if args.profile:
            result["profile"] = profile(session, args.top)
        if args.textures:
            result["textures"] = textures(session, args.textures)
    compression = {}
    for cb in buffers:
        for encoder in cb["encoders"]:
            for attachment in encoder.get("attachments", []):
                kind = attachment.get("compressionType") or "unknown"
                compression[kind] = compression.get(kind, 0) + 1
    result["attachment_compression"] = compression
    if args.json:
        args.json.write_text(json.dumps(result, indent=2) + "\n")
    print(f"{result['device']}: {result['summary']}")
    print("render attachments by compression: " + ", ".join(f"{k} {v}" for k, v in sorted(compression.items())))
    for cb in buffers:
        for encoder in cb["encoders"]:
            for attachment in encoder.get("attachments", []):
                if attachment.get("compressionType") not in ("Lossless", "Lossy"):
                    print(f"  uncompressed {cb['name']}/{encoder['name']}/{attachment['name']}: "
                          f"{attachment.get('label')} {attachment.get('pixelFormat')} usage {attachment.get('usage')}")
    if args.textures:
        kinds = {}
        for texture in result["textures"]:
            kind = texture.get("compressionType") or "unknown"
            kinds[kind] = kinds.get(kind, 0) + 1
        print(f"textures by compression ({len(result['textures'])} read): " +
              ", ".join(f"{k} {v}" for k, v in sorted(kinds.items())))
        for texture in result["textures"]:
            if texture.get("compressionType") not in ("Lossless", "Lossy") and texture.get("storageMode") == "Private":
                print(f"  uncompressed private {texture['name']}: {texture.get('label')} {texture.get('pixelFormat')} "
                      f"usage {texture.get('usage')}")
    if args.profile and "error" not in result["profile"]:
        print("costliest encoders: " + ", ".join(f"{e['name']} {e['values'][1:2]}" for e in result["profile"]["encoders"][:5]))
        print("costliest shaders: " + ", ".join(f"{s['values'][0]} {s['values'][2:3]}" for s in result["profile"]["shaders"][:5]))
    return 0


def fetch_final(args):
    """the last render encoder's color0, in the last command buffer that has one"""
    out = args.out.resolve()
    with Session(args.trace, out.parent) as session:
        for cb in reversed([c for c in children(session.run("go /commands", "list --all")[1]) if c["name"].startswith("cb")]):
            encoders = [e for e in children(session.run(f"go /commands/{cb['name']}", "list --all")[1])
                        if e["name"].startswith("re")]
            for encoder in reversed(encoders):
                fetched = session.run(f"go /commands/{cb['name']}/{encoder['name']}", f"fetch color0 --out {out}")[1]
                if fetched and out.exists():
                    print(f"{out}: {cb['name']}/{encoder['name']}/color0 {fetched.get('width')}x{fetched.get('height')}")
                    return 0
    print(f"{args.trace}: no render pass with a color attachment", file=sys.stderr)
    return 1


def capture(args):
    """CMD's Metal work captured: by Mullion inside the process (--mullion), or by gpucapture attached from outside"""
    import os
    import time
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        print("capture: no command to run", file=sys.stderr)
        return 2
    name = args.name or Path(command[-1].replace("\\", "/")).name
    out = args.out.resolve()
    if args.mullion:
        # Mullion's own capture (src/mullion/mullion_capture.cpp): command buffers N to N + count of the executable,
        # counted as they are committed (MULLION_CAPTURE_BY_CHUNK), so work that never presents is captured too
        out.parent.mkdir(parents=True, exist_ok=True)
        before = set(out.parent.glob("*.gputrace"))
        env = dict(os.environ, MTL_CAPTURE_ENABLED="1", MULLION_CAPTURE_EXECUTABLE=Path(name).stem,
                   MULLION_CAPTURE_BY_CHUNK="1", MULLION_CAPTURE_FRAME=str(args.first), MULLION_CAPTURE_FRAMES=str(args.count),
                   MULLION_CAPTURE_PATH=str(out.parent) + "/")
        status = subprocess.run(command, env=env).returncode
        made = sorted(set(out.parent.glob("*.gputrace")) - before, key=lambda path: path.stat().st_mtime)
        if not made:
            print(f"capture: {name} wrote no trace (exit status {status})", file=sys.stderr)
            return 1
        if out.exists():
            import shutil
            shutil.rmtree(out) if out.is_dir() else out.unlink()
        made[-1].rename(out)
        print(f"{out}: command buffers {args.first} to {args.first + args.count - 1} of {name}, exit status {status}")
        return 0
    # gpucapture attaches only to a debuggable process (com.apple.security.get-task-allow): not a Wine signed for
    # distribution, such as MetalSharp's, which --mullion captures instead
    def listed():
        text = subprocess.run(["gpucapture", "list"], capture_output=True, text=True).stdout
        rows = [line.split() for line in text.splitlines()[1:]]
        return {int(row[0]): (row[2], "non-debuggable" in line) for row, line in
                zip(rows, text.splitlines()[1:]) if len(row) >= 3 and row[0].isdigit()}
    before = set(listed())
    env = dict(os.environ, MTL_CAPTURE_ENABLED="1", MTLCAPTURE_WAIT_FOR_SIGNAL="1")
    process = subprocess.Popen(command, env=env)
    deadline, found = time.monotonic() + args.timeout, None
    while found is None and time.monotonic() < deadline and process.poll() is None:
        found = next(((p, blocked) for p, (n, blocked) in listed().items()
                      if p not in before and n.lower().startswith(name.lower()[:15])), None)
        if found is None:
            time.sleep(0.25)
    if found is None or found[1]:
        process.kill()
        print(f"capture: {name} " + ("is not debuggable (it lacks get-task-allow): use --mullion" if found
                                      else "appeared as no Metal process"), file=sys.stderr)
        return 1
    started = subprocess.run(["gpucapture", "start", "-p", str(found[0]), "-u", "-o", str(out)],
                             capture_output=True, text=True)
    status = process.wait()
    print(f"{out}: process {found[0]} ({name}) captured, exit status {status}"
          + ("" if started.returncode == 0 else f"; gpucapture: {(started.stdout + started.stderr).strip()}"))
    return 0 if out.exists() else 1


def read_png(path):
    """width, height, channels and rows of an 8-bit, non-interlaced PNG (what gpudebug writes)"""
    data = Path(path).read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    at, chunks, header = 8, [], None
    while at < len(data):
        length, kind = struct.unpack(">I4s", data[at:at + 8])
        body = data[at + 8:at + 8 + length]
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            chunks.append(body)
        at += 12 + length
    width, height, depth, color, _, _, interlace = header
    channels = {0: 1, 2: 3, 4: 2, 6: 4}.get(color)
    if depth != 8 or interlace or not channels:
        raise ValueError(f"{path}: only 8-bit non-interlaced grey/RGB/RGBA PNGs are read")
    raw, stride, rows, previous = zlib.decompress(b"".join(chunks)), width * channels, [], bytearray(width * channels)
    for y in range(height):
        kind, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            left = line[x - channels] if x >= channels else 0
            up, corner = previous[x], previous[x - channels] if x >= channels else 0
            if kind == 1:
                line[x] = (line[x] + left) & 255
            elif kind == 2:
                line[x] = (line[x] + up) & 255
            elif kind == 3:
                line[x] = (line[x] + (left + up) // 2) & 255
            elif kind == 4:
                p = left + up - corner
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - corner)
                line[x] = (line[x] + (left if pa <= pb and pa <= pc else up if pb <= pc else corner)) & 255
        rows.append(line)
        previous = line
    return width, height, channels, rows


def diff(args):
    wa, ha, ca, a = read_png(args.a)
    wb, hb, cb, b = read_png(args.b)
    if (wa, ha) != (wb, hb):
        print(f"sizes differ: {wa}x{ha} and {wb}x{hb}")
        return 2
    channels, differing, largest, box = min(ca, cb, 3), 0, 0, None
    for y in range(ha):
        for x in range(wa):
            delta = max(abs(a[y][x * ca + c] - b[y][x * cb + c]) for c in range(channels))
            largest = max(largest, delta)
            if delta > args.tolerance:
                differing += 1
                box = (min(box[0], x), min(box[1], y), max(box[2], x), max(box[3], y)) if box else (x, y, x, y)
    share = differing / (wa * ha)
    result = {"pixels": wa * ha, "differing": differing, "share": share, "largest": largest, "box": box}
    print(json.dumps(result))
    return 0 if share <= args.share else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    p = commands.add_parser("report", help="what a trace holds, and its GPU profile")
    p.add_argument("trace", type=Path)
    p.add_argument("--profile", action="store_true", help="replay with GPU profiling (M3 or later)")
    p.add_argument("--top", type=int, default=20, help="entries of each cost ranking (default: %(default)s)")
    p.add_argument("--json", type=Path, help="write the whole report here")
    p.add_argument("--textures", type=int, nargs="?", const=500, default=0,
                   help="also list the trace's textures with their compression, up to this many (default: 500)")
    p.set_defaults(handler=report)
    p = commands.add_parser("fetch-final", help="the last render pass's color0 as a PNG")
    p.add_argument("trace", type=Path)
    p.add_argument("--out", type=Path, required=True)
    p.set_defaults(handler=fetch_final)
    p = commands.add_parser("capture", help="run a command and capture its Metal process to the end")
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--name", help="the Metal process's name, as gpucapture list shows it")
    p.add_argument("--timeout", type=float, default=120, help="seconds to wait for the process (default: %(default)s)")
    p.add_argument("--mullion", action="store_true",
                   help="capture with Mullion inside the process (any Wine, signed or not) instead of gpucapture")
    p.add_argument("--first", type=int, default=1, help="with --mullion, the first command buffer (default: %(default)s)")
    p.add_argument("--count", type=int, default=100, help="with --mullion, how many command buffers (default: %(default)s)")
    p.add_argument("command", nargs=argparse.REMAINDER)
    p.set_defaults(handler=capture)
    p = commands.add_parser("diff", help="compare two PNGs")
    p.add_argument("a", type=Path)
    p.add_argument("b", type=Path)
    p.add_argument("--tolerance", type=int, default=2, help="per-channel difference ignored, 0..255 (default: %(default)s)")
    p.add_argument("--share", type=float, default=0.0,
                   help="share of differing pixels still called equal; exit status 1 above it (default: %(default)s)")
    p.set_defaults(handler=diff)
    args = parser.parse_args()
    return args.handler(args)


if __name__ == "__main__":
    sys.exit(main())
