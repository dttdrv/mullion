#!/usr/bin/env python3
"""runs tests/d3d12/d3d12_flight.cpp and checks the flight recorder's dumps (WMTFlightDump): which dumps a run
writes, in order ('== <reason>' sections), and that each holds the records of what the program says it did ('fact'
lines), in time order. a run that fails nothing writes no dump.

removed: a removed device dumps, and so does the exit after it; the first holds the list's execution, the Signal of the
program's fence to 1, its command buffer's completion (Metal's MTLCommandBufferStatusCompleted, 4) and the removal
with DXGI_ERROR_DEVICE_REMOVED (0x887a0005, winerror.h).
exception: the first access violation dumps, its heading naming the module of its address (kernelbase's RaiseException),
the second does not; the exit's dump holds both.
quiet: nothing failed: no dump.
many: more signals than a ring holds: the dump keeps the last of them, consecutive up to the program's last, and
counts the others overwritten.
cost: not judged; how long a record takes its thread, printed.
d3d11: MULLION_FLIGHT=exit dumps a run that failed nothing; it holds the context's command buffer committing and
completing.
busy: each dump made while other threads record holds the dumping thread's record from just before it.
"""
import argparse
import re
import shutil
import sys
from pathlib import Path

import run

COMPLETED = 4
REMOVED = 0x887A0005
# far past any ring of a thread's last frames, which holds thousands of records
SIGNALS = 100_000


def sections(path):
    """[(reason, header, [(ms before the dump, thread, text)])], or the first line that is neither"""
    out = []
    for line in path.read_text(errors="replace").splitlines():
        if m := re.fullmatch(r"== ([^,]+), (.*)", line):
            out.append((m[1], m[2], []))
        elif out and (m := re.fullmatch(r" *(-?[\d.]+) +(\d+) (.*)", line)):
            out[-1][2].append((float(m[1]), int(m[2]), m[3]))
        else:
            return line
    return out


def judge(mode, facts, dump):
    if mode == "quiet":
        return [f"a run that failed nothing wrote {dump.name}"] if dump.exists() else []
    if not dump.exists():
        return ["no dump"]
    got = sections(dump)
    if isinstance(got, str):
        return [f"a line that is no record: {got[:120]!r}"]
    want = {"removed": ["a removed device", "exit"], "exception": ["exception", "exit"], "many": ["a removed device", "exit"],
            "d3d11": ["exit"], "busy": ["busy"] * int(facts["busy"][0][0]) + ["exit"] if "busy" in facts else []}[mode]
    # an exception's dump names the module of its address: "exception in <module>+<offset>"
    reasons = [re.sub(r"^exception in [^+ ]+\+0x[0-9a-f]+$", "exception", reason) for reason, _, _ in got]
    wrong = [] if reasons == want else [f"dumps {[reason for reason, _, _ in got]}, want {want}"]
    for reason, _, records in got:
        times = [ms for ms, _, _ in records]
        if times != sorted(times, reverse=True) or any(ms < 0 for ms in times):
            wrong.append(f"{reason}: records out of time order")
    if wrong:
        return wrong
    first = [text for _, _, text in got[0][2]]
    if mode == "removed":
        (queue, fence, signals), = facts["signal"]
        for pattern in ["ExecuteCommandLists of 1$", f"Signal of fence {fence} to {signals}$",
                        f"completes, status {COMPLETED}$", f"is removed: {REMOVED:#x}$"]:
            if not any(re.search(pattern, text) for text in first):
                wrong.append(f"the first dump has no record matching '{pattern}'")
        order = [next((i for i, text in enumerate(first) if re.search(p, text)), -1)
                 for p in [f"Signal of fence {fence}", "is removed"]]
        if order[0] > order[1]:
            wrong.append("the removal is recorded before the Signal before it")
    elif mode == "exception":
        # RaiseException raises at its own address, in kernelbase
        if not re.fullmatch(r"exception in kernelbase\.dll\+0x[0-9a-f]+", got[0][0], re.IGNORECASE):
            wrong.append(f"the exception's dump does not name its module: {got[0][0]!r}")
        raised = [record for record in first if record.startswith("exception 0xc0000005")]
        if len(raised) != 1 or not raised[0].endswith(f"information 0x1 {facts['exception'][0][0]}"):
            wrong.append(f"the first dump's exceptions: {raised}")
        last = [text for _, _, text in got[1][2] if text.startswith("exception 0xc0000005")]
        if [text.rsplit(" ", 1)[1] for text in last] != [address for address, in facts["exception"]]:
            wrong.append(f"the exit's exceptions: {last}")
    elif mode == "many":
        (queue, fence, signals), = facts["signal"]
        overwritten = int(re.search(r"\((\d+) overwritten", got[0][1]).group(1))
        values = [int(m.group(1)) for text in first if (m := re.search(f"Signal of fence {fence} to (\\d+)$", text))]
        if not overwritten or not values or values[0] == 1:
            wrong.append(f"{overwritten} overwritten, the first signal kept is {values[:1]}")
        elif values != list(range(values[0], int(signals) + 1)):
            wrong.append(f"the signals kept are not the last ones in order: {values[:3]} ... {values[-3:]}")
    elif mode == "busy":
        for d, (_, _, records) in enumerate(got[:-1], 1):
            if not any(text == f"busy {d}" for _, _, text in records):
                wrong.append(f"dump {d} has not its thread's record 'busy {d}'")
    elif mode == "d3d11":
        for pattern in [r"chunk \d+ of frame \d+ commits as", f"chunk \\d+ completes, status {COMPLETED}$"]:
            if not any(re.search(pattern, text) for text in first):
                wrong.append(f"the dump has no record matching '{pattern}'")
    return wrong


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    run.arguments(parser)
    parser.add_argument("--out", required=True, type=Path, help="where the dumps and logs go")
    parser.add_argument("--seconds", type=int, default=300, help="the most a run may take (default: %(default)s)")
    parser.add_argument("--family", default="native", help="the Apple GPU family to run as, as tests/run.py names it")
    args = parser.parse_args()
    with run.session(args) as (builds, binary, base):
        base = run.as_family(base, args.family)
        exe = next(path for build in builds for path in build.glob("tests/*/d3d12_flight.exe"))
        failed = 0
        for mode in ("removed", "exception", "quiet", "many", "d3d11", "busy"):
            out = (args.out / mode).resolve()
            shutil.rmtree(out, ignore_errors=True)
            out.mkdir(parents=True)
            env = dict(base, MULLION_DIAG_PATH=str(out))
            env.pop("MULLION_FLIGHT", None)
            if mode == "d3d11":
                env["MULLION_FLIGHT"] = "exit"
            if mode == "many":
                env["FLIGHT_SIGNALS"] = str(SIGNALS)
            status = run.finish([binary, exe, mode], env, args.seconds, out / "log.txt")
            text = (out / "log.txt").read_text(errors="replace")
            facts = {}
            for kind, rest in re.findall(r"^fact (\w+) ?(.*)$", text, re.M):
                facts.setdefault(kind, []).append(tuple(rest.split()))
            wrong = ([f"exit status {status}"] if status else []) + judge(mode, facts, out / f"{exe.name}.flight")
            failed += bool(wrong)
            print(f"{mode}: {'failed' if wrong else 'passed'}", *(f"  {line}" for line in wrong[:8]), sep="\n",
                  flush=True)
        out = (args.out / "cost").resolve()
        out.mkdir(parents=True, exist_ok=True)
        status = run.finish([binary, exe, "cost"], dict(base), args.seconds, out / "log.txt")
        cost = re.search(r"^fact cost (\S+)$", (out / "log.txt").read_text(errors="replace"), re.M)
        print(f"a record takes {cost[1]} ns" if cost and not status else f"cost: exit status {status}", flush=True)
        sys.exit(failed != 0 or status != 0)


if __name__ == "__main__":
    main()
