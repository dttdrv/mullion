#!/usr/bin/env python3
# contract: the profile reader accounts for the measurements in Mullion's profile record, without attributing a
# shared pass's GPU time to individual pipelines. "A pass belongs to the first frame whose time is not before its
# command buffer's committed; what comes after the last frame is one more" and "A time of 18446744073709551615 was
# not sampled" (Mullion profile record contract, frame and pass). the expected totals use the chosen measurements.
import importlib.machinery
import importlib.util
import sys
import tempfile
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path
from unittest.mock import patch

sys.dont_write_bytecode = True
path = Path(__file__).resolve().parents[2] / "tools" / "mullion-diag"
assert path.is_file(), "the profile record reader must exist"
loader = importlib.machinery.SourceFileLoader("mullion_diag", str(path))
tool = importlib.util.module_from_spec(importlib.util.spec_from_loader(loader.name, loader))
loader.exec_module(tool)


def profile(directory):
    a = "01234567" + "a" * 32
    b = "89abcdef" + "b" * 32 + " " + "fedcba98" + "c" * 32
    c = "01234567" + "d" * 32
    missing = "18446744073709551615"
    lines = [
        "future\tthis line is ignored", "",
        "clock\t1000\t10", "clock\t2000\t20",
        "call\t1\t_SM50Convert", "call\t2\tMTLDevice_newLibrary",
        "call\t3\tMTLDevice_newRenderPipelineState", "call\t4\tMTLDevice_newFunction",
        "call\t5\tMTLCommandBuffer_waitUntilCompleted", "call\t6\tCAMetalLayer_nextDrawable",
        "call\t7\tMTLCommandBuffer_commit",
    ]
    buffers = [("a", 1, 90), ("b", 1, 100), ("a", 2, 99), ("b", 2, 80),
               ("b", 3, 85), ("b", 4, 86), ("b", 5, 87), ("a", 3, 101),
               ("a", 4, 200), ("c", 1, 201), ("c", 2, 400), ("a", 5, 401)]
    passes = [
        ("Render", f"{a} *2*6*0|{b} *3*0*12|{a} *1*3*0", "10\t30\t25\t40"),
        ("Compute", f"{a} *4*0*0", "40\t50"),
        ("Compute", f"{b} *1*0*0", "45\t55"),
        ("Blit", "", "70\t80"),
        ("Clear", "", f"90\t{missing}"),
        ("Clear", "", f"{missing}\t95"),
        ("Clear", "", f"{missing}\t{missing}"),
        ("Render", f"{b} *2*8*0", "100\t110\t110\t130"),
        ("Clear", "", "140\t150"),
        ("AccelerationStructure", f"{c} *2*0*0", f"{missing}\t{missing}"),
        ("Resolve", "", "160\t170"),
        ("Render", f"{a} *1*0*5|{b} *2*0*7", "200\t210\t215\t220"),
    ]
    for number, ((queue, buffer, committed), (kind, runs, stages)) in enumerate(zip(buffers, passes)):
        # completion order does not decide the frame, and buffer numbers are local to a queue
        lines.append(f"pass\t{queue}\t{buffer}\t{number}\t{kind}\t{(number + 1) * 1_000_000}\t{runs}\t{stages}")
        lines.append(f"cmdbuf\t{queue}\t{buffer}\t{committed}\t999\t0\t1000")
    lines += [
        "frame\ta\t1\t100", "unix\t5:2:7\t6:1:3\t7:9:100",
        "frame\ta\t4\t200", "unix\t5:1:20\t6:1:4\t1:2:8",
        "frame\tc\t2\t400", "unix\t7:1:2",
        f"span\tpipeline\t{a} \t1\t80\t50\t1:11\t2:13\t3:5\t4:7\t7:2",
        f"span\tpipeline\t{a} \t2\t100\t10\t1:3\t4:4",
        "span\texecute\t\t1\t90\t12\t5:4\t7:1", "span\tpresent\t\t1\t95\t9\t6:2",
        f"span\tpipeline\t{b} \t2\t101\t20\t2:8\t1:6",
        "span\texecute\t\t2\t200\t30\t1:1", "span\tpresent\t\t2\t199\t10",
        f"span\tpipeline\t{c} \t3\t250\t7", "span\tpresent\t\t3\t350\t5",
        "span\texecute\t\t3\t401\t6\t1:3", f"span\tpipeline\t{b} \t3\t401\t8\t4:2",
    ]
    path = directory / "mullion-1.diag"
    path.write_text("\n".join(lines) + "\n")
    record = tool.Record(path)
    assert record.frames == [100, 200, 400]
    assert [record.frame_of(p) for p in record.passes] == [0] * 7 + [1] * 2 + [2] * 2 + [3]
    assert record.passes[0].stages == [(10, 30), (25, 40)]
    gpu = [35, 10, 10, 10, 0, 0, 0, 30, 10, 0, 10, 15]
    assert [p.gpu for p in record.passes] == gpu
    assert record.passes[0].runs == [(a, 2, 6, 0), (b, 3, 0, 12), (a, 1, 3, 0)]
    assert record.passes[0].what() == "01234567, 89abcdef fedcba98"
    assert tool.union([(20, 30), (10, 25), (30, 40), (50, 60)]) == 40
    assert tool.union([]) == 0
    partial = tool.Pass("a", "1", "0", "Render", "0", "", "10", missing, "20", "25")
    assert partial.stages == [(20, 25)] and partial.gpu == 5
    near_missing = tool.Pass("a", "1", "0", "Compute", "0", "", str(int(missing) - 4), str(int(missing) - 1))
    assert near_missing.gpu == 3
    try:
        tool.Pass("a", "1", "0", "Compute", "0", "", "10")
    except ValueError:
        pass
    else:
        raise AssertionError("an unpaired stage timestamp must not be silently discarded")
    rows = tool.frame_rows(record)
    expected = [
        (None, 7, 65, 55, 11, 9, 12, 12, 9, 60, 14, 29, 10),
        (100, 2, 40, 40, 2, 8, 0, 30, 10, 20, 6, 8, 24),
        (200, 2, 10, 10, 2, 0, 0, 0, 5, 7, 0, 0, 0),
        (None, 1, 15, 15, 3, 0, 12, 6, 0, 8, 0, 2, None),
    ]
    keys = ("since", "passes", "gpu_sum", "gpu_clock", "draws", "vertices", "indices",
            "execute", "present", "pipeline", "converter", "metal", "waits")
    assert [row["frame"] for row in rows] == [0, 1, 2, 3]
    for row, values in zip(rows, expected):
        for key, value in zip(keys, values):
            assert row[key] == value, (row["frame"], key, row[key], value)
    assert tool.by_pipeline(record.passes) == {
        (a, b): [50, 2, 9, 9, 24], (a,): [10, 1, 4, 0, 0],
        (b,): [40, 2, 3, 8, 0], (): [30, 6, 0, 0, 0], (c,): [0, 1, 2, 0, 0],
    }
    assert tool.pipeline_runs(record.passes) == {a: [8, 9, 5], b: [8, 8, 19], c: [2, 0, 0]}
    assert tool.pipeline_runs(record.passes_in(0)) == {a: [7, 9, 0], b: [4, 0, 12]}
    assert tool.by_kind(record.passes) == {
        "Render": [80, 3], "Compute": [20, 2], "Blit": [10, 1], "Clear": [10, 4],
        "AccelerationStructure": [0, 1], "Resolve": [10, 1],
    }
    work, made, calls = tool.cpu_totals(record)
    assert work == {"pipeline": [95, 5], "execute": [48, 3], "present": [24, 3]}
    assert made == {a: [60, 2, 14, 29], b: [28, 2, 6, 10], c: [7, 1, 0, 0]}
    assert calls == {5: [27, 3], 6: [7, 2], 7: [102, 10], 1: [8, 2]}
    work, made, calls = tool.cpu_totals(record, 1)
    assert work == {"pipeline": [20, 1], "execute": [30, 1], "present": [10, 1]}
    assert made == {b: [20, 1, 6, 8]}
    assert calls == {5: [20, 1], 6: [4, 1], 1: [8, 2]}
    assert tool.cpu_totals(record, 3)[2] == {}
    assert record.waits({5: 7, 6: 3, 7: 100}) == 10
    assert record.compiling({1: 11, 2: 13, 3: 5, 4: 7, 7: 2}) == (11, 25)
    assert record.compiling({99: 100}) == (0, 0) and record.waits({99: 100}) == 0
    stats = tool.frame_stats(record)
    assert stats == {"presented": 3, "first": 0, "last": 2, "intervals": 2,
                     "mean": 150, "median": 150, "longest": 200, "fps": 1e9 / 150}
    assert tool.frame_stats(record, 1, 2) == {
        "presented": 3, "first": 1, "last": 2, "intervals": 1,
        "mean": 200, "median": 200, "longest": 200, "fps": 1e9 / 200,
    }
    assert tool.frame_stats(record, 0, 100) == stats
    for first, last in ((1, 1), (2, 1), (5, None)):
        assert tool.frame_stats(record, first, last)["intervals"] == 0
        assert tool.frame_stats(record, first, last)["mean"] is None
    ranked = [(0, "zero"), (7, "long"), (2, "short")]
    assert tool.ranked(ranked, 1) == ([(7, "long")], 2, 2, 9)
    assert tool.ranked(ranked, 0) == ([], 9, 3, 9)
    for top in (3, 4):
        assert tool.ranked(ranked, top) == ([(7, "long"), (2, "short"), (0, "zero")], 0, 0, 9)
    assert tool.ranked([], 1) == ([], 0, 0, 0)

    expected_passes = [list(range(7)), [7, 8], [9, 10], [11], list(range(12))]
    expected_kinds = [
        [(35, "Render: 1 passes"), (20, "Compute: 2 passes"), (10, "Blit: 1 passes"), (0, "Clear: 3 passes")],
        [(30, "Render: 1 passes"), (10, "Clear: 1 passes")],
        [(0, "AccelerationStructure: 1 passes"), (10, "Resolve: 1 passes")],
        [(15, "Render: 1 passes")],
        [(80, "Render: 3 passes"), (20, "Compute: 2 passes"), (10, "Blit: 1 passes"), (10, "Clear: 4 passes"),
         (0, "AccelerationStructure: 1 passes"), (10, "Resolve: 1 passes")],
    ]
    expected_pipelines = [
        [(35, "01234567, 89abcdef fedcba98: 1 passes, 6 draws, 9 vertices, 12 indices"),
         (10, "01234567: 1 passes, 4 draws, 0 vertices, 0 indices"),
         (10, "89abcdef fedcba98: 1 passes, 1 draws, 0 vertices, 0 indices"),
         (10, "(no pipeline): 4 passes, 0 draws, 0 vertices, 0 indices")],
        [(30, "89abcdef fedcba98: 1 passes, 2 draws, 8 vertices, 0 indices"),
         (10, "(no pipeline): 1 passes, 0 draws, 0 vertices, 0 indices")],
        [(0, "01234567: 1 passes, 2 draws, 0 vertices, 0 indices"),
         (10, "(no pipeline): 1 passes, 0 draws, 0 vertices, 0 indices")],
        [(15, "01234567, 89abcdef fedcba98: 1 passes, 3 draws, 0 vertices, 12 indices")],
        [(50, "01234567, 89abcdef fedcba98: 2 passes, 9 draws, 9 vertices, 24 indices"),
         (10, "01234567: 1 passes, 4 draws, 0 vertices, 0 indices"),
         (40, "89abcdef fedcba98: 2 passes, 3 draws, 8 vertices, 0 indices"),
         (30, "(no pipeline): 6 passes, 0 draws, 0 vertices, 0 indices"),
         (0, "01234567: 1 passes, 2 draws, 0 vertices, 0 indices")],
    ]
    expected_work = [
        [(60, "pipeline: 2 spans"), (12, "execute: 1 spans"), (9, "present: 1 spans")],
        [(20, "pipeline: 1 spans"), (30, "execute: 1 spans"), (10, "present: 1 spans")],
        [(7, "pipeline: 1 spans"), (5, "present: 1 spans")],
        [(6, "execute: 1 spans"), (8, "pipeline: 1 spans")],
        [(95, "pipeline: 5 spans"), (48, "execute: 3 spans"), (24, "present: 3 spans")],
    ]
    expected_made = [
        [(60, "01234567: 2 spans")], [(20, "89abcdef fedcba98: 1 spans")],
        [(7, "01234567: 1 spans")], [(8, "89abcdef fedcba98: 1 spans")],
        [(60, "01234567: 2 spans"), (28, "89abcdef fedcba98: 2 spans"), (7, "01234567: 1 spans")],
    ]
    expected_bridge = [
        [(7, "MTLCommandBuffer_waitUntilCompleted: 2 calls"), (3, "CAMetalLayer_nextDrawable: 1 calls"),
         (100, "MTLCommandBuffer_commit: 9 calls")],
        [(20, "MTLCommandBuffer_waitUntilCompleted: 1 calls"), (4, "CAMetalLayer_nextDrawable: 1 calls"),
         (8, "SM50Convert: 2 calls")],
        [(2, "MTLCommandBuffer_commit: 1 calls")], [],
        [(27, "MTLCommandBuffer_waitUntilCompleted: 3 calls"), (7, "CAMetalLayer_nextDrawable: 2 calls"),
         (102, "MTLCommandBuffer_commit: 10 calls"), (8, "SM50Convert: 2 calls")],
    ]
    expected_runs = [
        {"01234567: 7 draws, 9 vertices, 0 indices", "89abcdef fedcba98: 4 draws, 0 vertices, 12 indices"},
        {"89abcdef fedcba98: 2 draws, 8 vertices, 0 indices"},
        {"01234567: 2 draws, 0 vertices, 0 indices"},
        {"01234567: 1 draws, 0 vertices, 5 indices", "89abcdef fedcba98: 2 draws, 0 vertices, 7 indices"},
        {"01234567: 8 draws, 9 vertices, 5 indices", "89abcdef fedcba98: 8 draws, 8 vertices, 19 indices",
         "01234567: 2 draws, 0 vertices, 0 indices"},
    ]
    for index, frame in enumerate((0, 1, 2, 3, None)):
        args = ["mullion-diag", "report", str(path), "--top", "15"]
        if frame is not None:
            args += ["--frame", str(frame)]
        output = StringIO()
        with patch.object(sys, "argv", args), redirect_stdout(output):
            with patch.object(tool, "table", wraps=tool.table) as tables:
                tool.main()
        kinds, pipelines, passes, work, made, bridge = [call.args[1] for call in tables.call_args_list]
        assert kinds == expected_kinds[index]
        assert [(time, text.split(";")[0]) for time, text in pipelines] == expected_pipelines[index]
        runs = {part for _, text in pipelines if "; pipeline totals across selected passes: " in text
                for part in text.split("; pipeline totals across selected passes: ")[1].split("; ")}
        assert runs == expected_runs[index]
        assert len(passes) == len(expected_passes[index])
        for n, (time, text) in zip(expected_passes[index], passes):
            assert time == gpu[n] and f" pass {n} " in text
            assert f"CPU encoding {n + 1:.3f} ms" in text
            assert text in output.getvalue()
        assert work == expected_work[index]
        assert [(time, text.split(",")[0]) for time, text in made] == expected_made[index]
        assert bridge == expected_bridge[index]


def empty_and_short(directory):
    path = directory / "mullion-2.diag"
    for times in ([], [100], [100, 100], [100_000_000, 200_000_000, 500_000_000, 700_000_000]):
        path.write_text("".join(f"frame\ta\t{n}\t{time}\n" for n, time in enumerate(times)))
        record = tool.Record(str(path))
        stats = tool.frame_stats(record)
        assert stats["presented"] == len(times)
        assert stats["intervals"] == max(0, len(times) - 1)
        assert record.passes_in() == []
        assert tool.cpu_totals(record) == ({}, {}, {})
        assert tool.by_pipeline([]) == tool.pipeline_runs([]) == tool.by_kind([]) == {}
        assert all(row["waits"] is None for row in tool.frame_rows(record))
        if len(times) < 2:
            assert all(stats[key] is None for key in ("mean", "median", "longest", "fps"))
        elif times == [100, 100]:
            assert stats["mean"] == stats["median"] == stats["longest"] == 0
            assert stats["fps"] is None
        else:
            assert (stats["mean"], stats["median"], stats["longest"]) == (200_000_000, 200_000_000, 300_000_000)
    for args, expected in (
        (["--from", "1", "--to", "3"], "frames 1 to 3: mean 250.000 ms, median 250.000 ms, "
         "longest 300.000 ms, 4.00 frames a second"),
        (["--from", "2", "--to", "2"], "frames 2 to 2: no intervals between presents"),
        ([], "frames 0 to 3: mean 200.000 ms, median 200.000 ms, longest 300.000 ms, 5.00 frames a second"),
    ):
        output = StringIO()
        with patch.object(sys, "argv", ["mullion-diag", "frames", str(path), *args]), redirect_stdout(output):
            tool.main()
        assert output.getvalue().strip() == f"mullion-2.diag: 4 frames presented; {expected}"
    assert tool.records(directory) == sorted(directory.glob("*.diag"))
    assert tool.records(path) == [path]
    empty = directory / "empty"
    empty.mkdir()
    assert tool.records(empty) == []


def d3d11(directory):
    path = directory / "d3d11.diag"
    path.write_text("frame\t1\t1\t200\n"
                    "frame\td3d11:1\t1\t100\n"
                    "cmdbuf\t1\t1\t190\t200\t180\t190\n"
                    "cmdbuf\td3d11:1\t1\t110\t180\t120\t160\t90\t1\t0\t0\n"
                    "cmdbuf\td3d11:1\t2\t111\t181\t0\t0\t90\t0\t0\t0\n"
                    "pass\td3d11:1\t1\t2\tRender\t7\tRender:123456789*2*6*0\t120\t130\t140\t160\n"
                    "target\td3d11:1\t1\t2\tcolor\t0\t123\t8\t4\t70\t0\t0\n")
    record = tool.Record(path)
    assert record.frames == [100, 200] and len(record.cmdbufs) == 3
    assert record.cmdbufs["d3d11:1", 2] == (111, 181, 0, 0, 90, 0, 0, 0)
    assert record.frame_of(record.passes[0]) == 0, "encoder delay must not move a submitted pass into the next frame"
    assert record.targets["d3d11:1", 1, 2] == [("color", "0", "123", "8", "4", "70", "0", "0")]
    assert tool.frame_rows(record)[0]["gpu_clock"] == 30
    assert tool.short_name("Render:123456789") == "Render:123456789"
    tile = tool.Pass("d3d11:1", "1", "3", "Render", "1", "Tile:45*1*0*0|Render:123456789*2*6*0", "1", "2")
    assert tile.draws == 2 and tile.tile_dispatches == 1 and tile.pipelines == ("Tile:45", "Render:123456789")


with tempfile.TemporaryDirectory() as directory:
    profile(Path(directory))
    empty_and_short(Path(directory))
    d3d11(Path(directory))
print("passed: host_diag_profile")
