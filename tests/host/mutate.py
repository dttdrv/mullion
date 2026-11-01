# contract: mutations make "One change in one place each"; "Comments, log text, assertions and code compiled out
# are not mutated" (Mullion mutation contract, Mutants). "A function no test reaches is not mutated" (Which tests).
# the snippets and expected edits below follow those rules, independently of the library's behaviour.
# ordering follows "functions that the most tests reach first" and sampling "draws evenly over files"
# (Mullion mutation contract, fix-up 3).
import importlib.util
import json
import multiprocessing
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from contextlib import redirect_stderr, redirect_stdout
from io import StringIO
from unittest.mock import patch

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
spec = importlib.util.spec_from_file_location("mutate", Path(__file__).resolve().parents[1] / "mutate.py")
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)


class Mutations(unittest.TestCase):
    def setUp(self):
        # the runner keeps the peaks of this machine's tests, which these tests' commands are not
        import run
        peaks = tempfile.TemporaryDirectory()
        self.addCleanup(peaks.cleanup)
        patcher = patch.object(run, "PEAKS", Path(peaks.name) / "peaks.json")
        patcher.start()
        self.addCleanup(patcher.stop)

    def make(self, source, active=None):
        return tool.mutations(source, "src/example.cpp", active)[1]

    def test_operators(self):
        code = """int work(State &s, int *data, int count, int offset) {
  if (count < s.end && count <= s.limit) s.dirty = true;
  if (count > s.begin || count >= s.floor) restore();
  while (count != offset) { count = count - 1; }
  s.dirty = false;
  if (s.begin <= s.end) restore();
  return data[count] + offset;
}
"""
        made = self.make(code)
        self.assertEqual({m["operator"] for m in made}, {
            "negate-condition", "boundary", "logical", "remove-statement", "constant-up", "constant-down",
            "drop-index", "drop-offset", "arithmetic", "neighbour-field", "boolean",
        })
        edits = {(m["old"], m["new"]) for m in made}
        for edit in [("<", "<="), ("<=", "<"), (">", ">="), (">=", ">"), ("&&", "||"), ("||", "&&"),
                     ("true", "false"), ("false", "true"), ("-", "+"), ("+", "-"), ("[count]", "[0]"),
                     ("+ offset", ""), ("restore();", ";"), ("1", "(1 + 1)"), ("1", "(1 - 1)"),
                     ("count < s.end && count <= s.limit", "!(count < s.end && count <= s.limit)"),
                     ("begin", "end"), ("return data[count] + offset;", ";")]:
            self.assertIn(edit, edits)
        for m in made:
            changed = tool.apply(code, m)
            self.assertEqual(changed, code[:m["start"]] + m["new"] + code[m["end"]:])
            self.assertNotEqual(changed, code)

    def test_exclusions(self):
        code = '''// if (x < 2) false;
#define BAD(x) ((x) + 1)
int work(int x) {
  const char *text = R"tag(if (x < 2) { true; })tag";
  const char *escaped = "quote \\\" x + 1";
  assert(x < 2 && x + 1);
  static_assert(2 < 3);
  WARN("value", x + 1);
  Logger::log("value", x + 1);
  // false && x < 2
  /* true || x > 2 */
#if 0
  if (x < 2) return false;
#else
  return x + 1;
#endif
}
'''
        made = self.make(code, {3, 4, 5, 6, 7, 8, 9, 15, 17})
        self.assertTrue(made)
        self.assertEqual({m["line"] for m in made}, {15})

    def test_inactive_syntax_and_numbers(self):
        code = "int f() {\n#if 0\n nonsense { if (false) {\n#endif\n return 1e+8 + 0x1p-2;\n}\n"
        funcs, made = tool.mutations(code, "src/test.cpp", {1, 5, 6})
        self.assertEqual([f["name"] for f in funcs], ["f"])
        self.assertTrue(made)
        self.assertFalse(any(m["old"] in ("1e", "0x1p", "8", "2", "false") for m in made))

    def test_continued_comments_and_crlf(self):
        code = "int f(int x) {\r\n // comment \\\r\n if (x < 2) return false;\r\n return x + 1;\r\n}\r\n"
        made = self.make(code)
        self.assertEqual({m["line"] for m in made}, {4})
        for m in made:
            self.assertEqual(tool.apply(code, m).count("\r\n"), code.count("\r\n"))

    def test_functions_and_templates(self):
        code = '''namespace n {
struct S {
  S() : first(1), second(2) { first = first + 1; }
  int value(int x) const { if (x < second) return x; return first; }
};
int f(int x) {
  auto lambda = [x](int y) { return x + y; };
  auto v = cast<int>(x);
  if (v < x) return lambda(v);
  return 0;
}
}
'''
        funcs, made = tool.mutations(code, "src/example.cpp")
        self.assertEqual([f["name"] for f in funcs], ["S", "value", "f", "lambda@7"])
        self.assertFalse(any(m["operator"] == "boundary" and m["line"] == 8 for m in made))
        self.assertFalse(any(m["operator"] == "remove-statement" and m["old"].startswith("auto") for m in made))

    def test_stable_identity(self):
        code = "int f(int x) { return x + 1; }\nint unrelated() { return 8; }\n"
        a = [m["id"] for m in self.make(code) if m["line"] == 1]
        b = [m["id"] for m in self.make(code.replace("return 8", "return 7")) if m["line"] == 1]
        c = [m["id"] for m in self.make(code.replace("x + 1", "x + 2")) if m["line"] == 1]
        self.assertEqual(a, b)
        self.assertTrue(set(a).isdisjoint(c))

    def test_return_types_attributes_and_nested_lambda(self):
        code = '''decltype(helper()) f() { return 2; }
__attribute__((unused)) int g() { return 3; }
void h() { emit([](int x) { return x + 1; }); }
struct S { S() : value([]() { return 1; }()) { value = value + 1; } int value; };
void i() { if (use([](int x) { return x + 1; })) { restore(); } }
'''
        funcs, _ = tool.mutations(code, "src/test.cpp")
        self.assertEqual([f["name"] for f in funcs],
                         ["f", "g", "h", "lambda@3", "lambda@4", "S", "i", "lambda@5"])

    def test_routing(self):
        funcs, made = tool.mutations("int f() { return 1; }\nint g() { return 2; }\n", "src/example.cpp")
        index = {"functions": {"src/example.cpp": {"1": ["slow", "fast"]}},
                 "tests": {"slow": {"seconds": 4}, "fast": {"seconds": 1}}}
        reached, unreached = tool.route(funcs, made, index)
        self.assertEqual({m["function"] for m in reached}, {"f"})
        self.assertEqual(reached[0]["tests"], ["fast", "slow"])
        self.assertEqual([f["name"] for f in unreached], ["g"])

    def test_priority_and_sample(self):
        made = [dict(id=f"{file}:{i}", file=file, start=i, operator="boolean", tests=list(range(i % 3 + 1)))
                for file, count in [("a", 8), ("b", 8), ("c", 1)] for i in range(count)]
        ordered = tool.schedule(made, None)
        self.assertEqual([len(m["tests"]) for m in ordered], sorted((len(m["tests"]) for m in made), reverse=True))
        for count in (1, 3, 7, len(made), len(made) + 1):
            sample = tool.schedule(made, [count, 17])
            self.assertEqual(len(sample), min(count, len(made)))
            self.assertEqual(len({m["id"] for m in sample}), len(sample))
            self.assertEqual(sample, tool.schedule(list(reversed(made)), [count, 17]))
            if count >= 3:
                counts = [sum(m["file"] == f for m in sample) for f in ("a", "b", "c")]
                self.assertEqual(counts[2], 1)
                self.assertLessEqual(abs(counts[0] - counts[1]), 1)
        self.assertNotEqual(tool.schedule(made, [7, 17]), tool.schedule(made, [7, 18]))
        self.assertEqual(tool.schedule([], [1, 17]), [])
        lexical = [{k: v for k, v in m.items() if k != "tests"} for m in made]
        self.assertEqual(len(tool.schedule(lexical, [7, 17])), 7)

    def test_sample_estimate(self):
        records = [{"file": file, "status": status} for file, status in
                   [("a", "killed"), ("a", "survived"), ("b", "killed")]]
        score, low, high = tool.estimate(records, {"a": 100, "b": 1})
        self.assertAlmostEqual(score, 51 / 101)
        self.assertLessEqual(low, score)
        self.assertGreaterEqual(high, score)
        self.assertEqual(tool.estimate(records, {"a": 2, "b": 1}), (2 / 3, 2 / 3, 2 / 3))
        records.append({"file": "b", "status": "not-built"})
        self.assertEqual(tool.estimate(records, {"a": 2, "b": 2}), (2 / 3, 2 / 3, 2 / 3))
        self.assertIsNone(tool.estimate(records, {"a": 2, "b": 2, "c": 1}))
        self.assertIsNone(tool.estimate([{"file": "a", "status": "not-built"}], {"a": 1}))
        self.assertIsNone(tool.estimate([{"file": "a", "status": "inconclusive"}], {"a": 1}))
        # half of a 200-mutant population: Var(mean) <= (200-100)/(4*100*(200-1)) (STAT 506 2.2).
        rows = [{"file": "a", "status": "killed" if i < 50 else "survived"} for i in range(100)]
        radius = (100 / (4 * 100 * 199) / ((1 - 0.95) / 2)) ** 0.5
        score, low, high = tool.estimate(rows, {"a": 200})
        self.assertEqual(score, 0.5)
        self.assertAlmostEqual(low, 0.5 - radius)
        self.assertAlmostEqual(high, (0.5 + radius) / (1 - radius))

    def test_sample_report(self):
        population = {"src/a.cpp": 2, "src/b.cpp": 1}
        made = [dict(id=str(i), file=file, context="current", status=status, runs=[], tests=["a"], coverage="measured",
                     line=1, column=i + 1, operator="boolean", function_line=1, function="f",
                     original="return true;", mutated="return false;")
                for i, (file, status) in enumerate([("src/a.cpp", "killed"), ("src/a.cpp", "survived"),
                                                   ("src/b.cpp", "not-built")])]
        state = dict(context="current", population=population, files=list(population),
                     selected=3, candidates=4, sample=[3, 17], plan=[m["id"] for m in made],
                     results={m["id"]: m for m in made}, unreached=[], excluded=[])
        state["results"]["other-sample"] = dict(made[0])
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "results.json"

            def output():
                tool.save(path, state)
                out = StringIO()
                with redirect_stdout(out):
                    tool.report(path)
                return out.getvalue()

            text = output()
            self.assertIn("src: 1/2 killed (50.0%)", text)
            self.assertIn("3 completed of 3 eligible mutations; 3 recorded of 3 scheduled; 4 lexical candidates", text)
            self.assertIn("population estimate 50.0%, conservative 95% sampling bound [50.0%, 50.0%]", text)
            state["results"]["1"]["status"] = "interrupted"
            text = output()
            self.assertIn("2 completed of 3 eligible mutations", text)
            self.assertNotIn("population estimate", text)

    def test_sample_arguments(self):
        funcs, made = tool.mutations("int f() { return true; }", "src/test.cpp")
        with tempfile.TemporaryDirectory() as tmp:
            argv = ["mutate.py", "list", "--lanes", tmp, "--coverage", str(Path(tmp) / "coverage.json"),
                    "--sample", "1", "17"]
            out = StringIO()
            with patch.object(sys, "argv", argv), patch.object(tool, "inventory", return_value=(funcs, made, [])), \
                 patch.object(tool.signal, "signal"), redirect_stdout(out):
                tool.main()
            self.assertIn("1 sampled, seed 17", out.getvalue())
            self.assertEqual(sum(line.startswith("src/test.cpp:") for line in out.getvalue().splitlines()), 1)
            argv[-2] = "0"
            with patch.object(sys, "argv", argv), redirect_stderr(StringIO()), self.assertRaises(SystemExit) as ended:
                tool.main()
            self.assertEqual(ended.exception.code, 2)

    def test_lock_publication(self):
        # "one GPU job at a time by MULLION_TEST_LOCK" (Mullion mutation contract, Running).
        ctx = multiprocessing.get_context("fork")
        publishing, publish, first, release, started, second = [ctx.Event() for _ in range(6)]
        with tempfile.TemporaryDirectory() as tmp:
            lock = Path(tmp) / "gpu.lock"

            def owner():
                write = Path.write_text

                def paused(path, text, *args, **kwargs):
                    if path == lock / "pid":
                        publishing.set()
                        if not publish.wait(10):
                            raise RuntimeError("publication not released")
                    return write(path, text, *args, **kwargs)

                with patch.dict(os.environ, MULLION_TEST_LOCK=str(lock)), patch.object(Path, "write_text", paused):
                    with tool.test_run.alone():
                        first.set()
                        release.wait(10)

            def contender():
                sleep = time.sleep
                with patch.dict(os.environ, MULLION_TEST_LOCK=str(lock)), \
                     patch.object(tool.test_run.time, "sleep", lambda _: sleep(0.01)):
                    started.set()
                    with tool.test_run.alone():
                        second.set()

            processes = [ctx.Process(target=owner), ctx.Process(target=contender)]
            try:
                processes[0].start()
                self.assertTrue(publishing.wait(5))
                processes[1].start()
                self.assertTrue(started.wait(5))
                self.assertFalse(second.wait(0.3), "contender stole an owner still publishing its pid")
                publish.set()
                self.assertTrue(first.wait(5))
                self.assertFalse(second.wait(0.3), "contender stole a live owner")
                release.set()
                self.assertTrue(second.wait(5))
                for process in processes:
                    process.join(5)
                    self.assertEqual(process.exitcode, 0)
                self.assertFalse(lock.exists())
            finally:
                publish.set()
                release.set()
                for process in processes:
                    if process.pid is not None:
                        if process.is_alive():
                            process.kill()
                        process.join(5)

    def test_runner_installation_lifetime(self):
        ctx = multiprocessing.get_context("fork")
        first, second, started, release = (ctx.Event() for _ in range(4))
        with tempfile.TemporaryDirectory() as tmp:
            def runner(entered, wait):
                temporary = Path(tmp) / str(os.getpid())
                temporary.mkdir()
                def environment(_):
                    entered.set()
                    return [], None, {}

                def own(*_):
                    if wait and not release.wait(5):
                        raise RuntimeError("runner was not released")
                    return 0

                with patch.object(sys, "argv", ["run.py", "--build", tmp, "--wine", tmp]), \
                     patch.object(tempfile, "gettempdir", return_value=str(temporary)), \
                     patch.object(tool.test_run, "chosen", return_value=[]), \
                     patch.object(tool.test_run, "environment", environment), \
                     patch.object(tool.test_run, "own", own):
                    started.set()
                    tool.test_run.main()

            processes = [ctx.Process(target=runner, args=(first, True)), ctx.Process(target=runner, args=(second, False))]
            try:
                processes[0].start()
                self.assertTrue(first.wait(5))
                started.clear()
                processes[1].start()
                self.assertTrue(started.wait(5))
                self.assertFalse(second.wait(0.3), "another runner installed DLLs before the running test finished")
                release.set()
                self.assertTrue(second.wait(5))
                for process in processes:
                    process.join(5)
                    self.assertEqual(process.exitcode, 0)
            finally:
                release.set()
                for process in processes:
                    if process.pid is not None:
                        if process.is_alive():
                            process.kill()
                        process.join(5)

    def test_runner_readonly_wine(self):
        with tempfile.TemporaryDirectory() as tmp:
            wine = Path(tmp) / "wine"
            wine.mkdir(mode=0o500)
            try:
                with patch.object(sys, "argv", ["run.py", "--build", tmp, "--wine", str(wine), "--no-install"]), \
                     patch.object(tool.test_run, "chosen", return_value=[]), \
                     patch.object(tool.test_run, "environment", return_value=([], None, {})), \
                     patch.object(tool.test_run, "own", return_value=0), self.assertRaises(SystemExit) as ended:
                    tool.test_run.main()
                self.assertEqual(ended.exception.code, 0)
            finally:
                wine.chmod(0o700)

    def test_lock_dead_owner(self):
        with tempfile.TemporaryDirectory() as tmp:
            lock = Path(tmp) / "gpu.lock"
            code = (f"import os,sys;sys.path.insert(0, {str(tool.ROOT / 'tests')!r});import run;"
                    "guard=run.alone();guard.__enter__();os._exit(0)")
            process = subprocess.run([sys.executable, "-c", code],
                                     env=dict(os.environ, MULLION_TEST_LOCK=str(lock)), timeout=5)
            self.assertEqual(process.returncode, 0)
            self.assertTrue((lock / "pid").exists())
            for pid in ((lock / "pid").read_text(), None, "unfinished"):
                lock.mkdir(exist_ok=True)
                if pid is None:
                    (lock / "pid").unlink(missing_ok=True)
                else:
                    (lock / "pid").write_text(pid)
                with patch.dict(os.environ, MULLION_TEST_LOCK=str(lock)), patch.object(tool.test_run.time, "sleep"):
                    with tool.test_run.alone():
                        self.assertEqual((lock / "pid").read_text(), str(os.getpid()))
                self.assertFalse(lock.exists())

    def test_shader_routing(self):
        index = {"functions": {}, "tests": {
            "dxbc": {"shaders": True, "shader_kinds": ["DXBC"], "gpu": True, "seconds": 1},
            "dxil": {"shaders": True, "shader_kinds": ["DXIL"], "gpu": True, "seconds": 2},
            "plain": {"shaders": False, "gpu": True, "seconds": 0.5},
        }}
        for file, wanted in [("src/airconv/dxbc_converter.cpp", ["dxbc"]),
                             ("src/airconv/dxil_converter.cpp", ["dxil"]),
                             ("src/airconv/air_type.cpp", ["dxbc", "dxil"]),
                             ("src/winemetal/unix/bridge.c", ["plain", "dxbc", "dxil"])]:
            funcs, made = tool.mutations("int f() { return 1; }", file)
            reached, unreached = tool.route(funcs, made, index)
            self.assertFalse(unreached)
            self.assertEqual(reached[0]["tests"], wanted)
            self.assertNotEqual(reached[0]["coverage"], "measured")

    def test_symbol_mapping(self):
        from argparse import Namespace
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "src").mkdir()
            source = "int\nf(int x) { return x + 1; }\n"
            file = root / "src/test.cpp"
            file.write_text(source)
            (root / "meson-info").mkdir()
            (root / "meson-info/meson-info.json").write_text(json.dumps({"directories": {"source": str(root)}}))
            notes = root / "notes"
            notes.mkdir()
            (notes / "test.dll.123.txt").write_text("18\n18\n")
            funcs, _ = tool.mutations(source, "src/test.cpp")
            frames = [{"Symbol": [{"FileName": str(file), "Line": 2, "StartLine": 1, "FunctionName": "f"}]}]

            class Symbolizer:
                def command(self, command, log, input_path):
                    if input_path.read_text() != "0x18\n":
                        raise AssertionError(input_path.read_text())
                    log.write_text("\n".join(json.dumps(frame) for frame in frames) + "\n")
                    return 0, 0

            args = Namespace(coverage_build=root, source=root)
            with patch.object(tool.coverage_tools, "tools", return_value=([], root)), \
                 patch.object(tool.test_run, "libraries", return_value=({}, {"test.dll": root / "test.dll"})):
                self.assertEqual(tool.symbolized(args, notes, funcs, Symbolizer(), root), {("src/test.cpp", 2)})
                frames[0]["Symbol"][0]["FileName"] = ""
                with self.assertRaisesRegex(RuntimeError, "no source locations"):
                    tool.symbolized(args, notes, funcs, Symbolizer(), root)

    def test_coverage_setup_debug(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            like = root / "like"
            (like / "meson-private").mkdir(parents=True)
            (like / "meson-info").mkdir()
            (like / "meson-private/cmd_line.txt").write_text("[options]\nbuildtype=release\n[properties]\n")
            (like / "meson-info/meson-info.json").write_text(json.dumps({"directories": {"source": str(root)}}))
            with patch.object(tool.coverage_tools, "tools", return_value=(["cc"], root)), \
                 patch.object(tool.coverage_tools.subprocess, "run") as commands:
                tool.coverage_tools.setup(like, root / "out")
            configure = commands.call_args_list[1].args[0]
            self.assertIn("-Ddebug=true", configure)
            self.assertIn("-Dcpp_args=" + tool.coverage_tools.FLAG, configure)

    def test_resume_and_atomic_results(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "results.json"
            tool.save(path, {"results": {"a": {"status": "killed"}}})
            self.assertEqual(json.loads(path.read_text())["results"]["a"]["status"], "killed")
            self.assertEqual(list(Path(tmp).iterdir()), [path])
        self.assertTrue(tool.finished({"status": "survived", "context": "a"}, "a"))
        self.assertFalse(tool.finished({"status": "interrupted", "context": "a"}, "a"))
        self.assertFalse(tool.finished({"status": "killed", "context": "a"}, "b"))

    def test_runner_verdict(self):
        self.assertEqual(tool.verdict("native passed test 0.1 s passed: 0 wrong\n", 0), "passed")
        self.assertEqual(tool.verdict("native skipped test 0.1 s skipped: unsupported\n", 0), "skipped")
        self.assertEqual(tool.verdict("native failed test 0.1 s failed: wrong bytes\n", 1), "failed")
        self.assertEqual(tool.verdict("no such test\n", 1), "error")
        self.assertEqual(tool.verdict("native passed test 0.1 s passed\n", 1), "error")

    def test_exact_selection(self):
        import run
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "meson-info").mkdir()
            exe = root / "test.exe"
            exe.touch()
            (root / "meson-info/intro-tests.json").write_text(json.dumps([
                {"name": name, "cmd": [str(exe)], "suite": []} for name in ["test", "test_more"]]))
            from argparse import Namespace
            args = Namespace(build=[root], tests=["test"], suite=[], exact=True)
            self.assertEqual([t["name"] for t in run.chosen(args)], ["test"])
            # a whole name that is not a test is a mistake, not a smaller selection
            args.tests = ["test", "tset"]
            with self.assertRaises(SystemExit):
                run.chosen(args)
            args.tests, args.exact = ["test"], False
            self.assertEqual(len(run.chosen(args)), 2)
            (root / "meson-info/intro-benchmarks.json").write_text(json.dumps([
                {"name": "test", "cmd": [str(exe), "bench"], "suite": []}]))
            args.benchmark = True
            self.assertEqual([t["cmd"] for t in run.chosen(args)], [[str(exe), "bench"]])

    def test_runner_lock_not_inherited(self):
        # a test runs while its run holds the GPU lock for it: one that runs tests itself (these, through run.own)
        # would wait for that lock until its timeout
        import run
        from argparse import Namespace
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            script = "import os; print('failed: has the lock' if 'MULLION_TEST_LOCK' in os.environ else 'passed: ok')"
            tests = [{"name": "nested", "cmd": [sys.executable, "-c", script], "env": {}, "timeout": 30,
                      "is_parallel": True}]
            args = Namespace(late=None, family="native", benchmark=False, repeat=1, jobs=1)
            run.KNOWN = root / "known.txt"
            with patch.dict(os.environ, MULLION_TEST_LOCK=str(root / "gpu.lock")), redirect_stdout(StringIO()) as out:
                failed = run.own(args, tests, [root], None, dict(os.environ))
            self.assertEqual(failed, 0, out.getvalue())

    def test_runner_schedule(self):
        # --jobs runs tests side by side, one meson declares not parallel after them by itself, and a test that failed
        # beside others again alone, which a test already run alone is not
        import run
        from argparse import Namespace
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            script = ("import sys, time; name, at = sys.argv[1], sys.argv[2]; log = open(at + '/runs', 'a', buffering=1)\n"
                      "log.write(f'start {name} {time.time()}\\n'); time.sleep(0.5)\n"
                      "first = name != 'flaky' or not __import__('os').path.exists(at + '/flaky')\n"
                      "name == 'flaky' and open(at + '/flaky', 'a'); log.write(f'end {name} {time.time()}\\n')\n"
                      "ok = name != 'serial' and (name != 'flaky' or not first)\n"
                      "print('passed: ok' if ok else 'failed: wrong'); sys.exit(0 if ok else 1)")
            tests = [{"name": name, "cmd": [sys.executable, "-c", script, name, tmp], "env": {}, "timeout": 30,
                      "is_parallel": name != "serial"} for name in ["a", "b", "flaky", "serial"]]
            args = Namespace(late=None, family="native", benchmark=False, repeat=1, jobs=len(tests))
            run.KNOWN = root / "known.txt"
            # each measured, as one never measured runs alone
            run.PEAKS.write_text(json.dumps({run.measured(t["cmd"], dict(os.environ, **t["env"])): 0 for t in tests}))
            with redirect_stdout(StringIO()) as out:
                failed = run.own(args, tests, [root], None, dict(os.environ))
            runs = [line.split() for line in (root / "runs").read_text().splitlines()]
            at = {}
            for word, name, t in runs:
                at.setdefault((word, name), []).append(float(t))
            self.assertEqual(failed, 1, out.getvalue())
            self.assertLess(at["start", "b"][0], at["end", "a"][0])
            self.assertLess(at["start", "a"][0], at["end", "b"][0])
            self.assertGreaterEqual(at["start", "serial"][0],
                                    max(at["end", name][0] for name in ["a", "b", "flaky"]))
            self.assertEqual((len(at["start", "serial"]), len(at["start", "flaky"])), (1, 2))
            self.assertIn("passed alone after failing beside others: native flaky", out.getvalue())

    def test_runner_known(self):
        # a failure or skip counts unless tests/known.txt has it by what the test said; a listed test that passes counts
        # too, so a known line never outlives its cause and hides the next failure of that test. a test that stopped
        # early cannot be listed: it hides every case after where it stopped
        import run
        from argparse import Namespace
        said = {"same": "failed: 3 wrong results", "other": "failed: 3 wrong results", "fixed": "passed: 0 wrong results",
                "fresh": "failed: hr (line 280)", "skip": "skipped: no 16x", "fine": "passed: 0 wrong results"}
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            chip = subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"], capture_output=True, text=True).stdout.strip()
            (root / "a.txt").write_text("wrong: a\n")
            three, two, one = (run.signature(f"failed: {n} wrong results", root / "a.txt") for n in (3, 2, 1))
            run.KNOWN = root / "known.txt"
            run.KNOWN.write_text(f"# a comment\nnative({chip}) same {three} -- the cause\n"
                                 f"native({chip}) other {two} -- the cause\n"
                                 f"native({chip}) fixed {one} -- the cause\n"
                                 f"9 same {three} -- another family's\n")
            tests = [{"name": name, "cmd": [sys.executable, "-c", "import sys; print(('wrong: a\\n' if 'wrong' in sys.argv[1] "
                                            "else '') + sys.argv[1]); sys.exit({'passed': 0, 'skipped': 77}"
                                            ".get(sys.argv[1].split(':')[0], 1))", text],
                      "env": {}, "timeout": 2, "is_parallel": True} for name, text in said.items()]
            args = Namespace(late=None, family="native", benchmark=False, repeat=1, jobs=1)
            with redirect_stdout(StringIO()) as out:
                counted = run.own(args, tests, [root], None, dict(os.environ))
            lines = out.getvalue().splitlines()
            self.assertEqual(sorted(re.findall(r"^(?:new|passes, still listed as known): native\([^)]*\) (\S+)",
                                               out.getvalue(), re.M)), ["fixed", "fresh", "other", "skip"], out.getvalue())
            self.assertEqual(counted, 4)
            self.assertIn(f"new: native({chip}) other {three} (known: {two})", lines)
            self.assertIn("1 known, each with its cause in " + str(run.KNOWN), lines)
            for line in [f"9 same {three}", "9 fresh failed: hr (line 280) -- the cause", "9 same failed: 3 wrong results -- the cause"]:
                run.KNOWN.write_text(line + "\n")
                with self.assertRaises(SystemExit, msg=line):
                    run.known()
            run.KNOWN.write_text("9 skip skipped: no 16x -- the cause\n")
            self.assertEqual(run.known(), {("9", "skip"): "skipped: no 16x"})
            # a list to compare with that is not there is a mistake, not an empty list
            with self.assertRaises(SystemExit), redirect_stdout(StringIO()):
                run.own(Namespace(late=None, family="native", benchmark=False, repeat=1, jobs=1, known=root / "none.txt"),
                        tests[:1], [root], None, dict(os.environ))
            # a validated run is compared with its own list
            run.KNOWN_VALIDATED = root / "known-validated.txt"
            run.KNOWN_VALIDATED.write_text(f"native({chip}) skip skipped: no 16x -- the cause\n")
            with redirect_stdout(StringIO()) as out:
                counted = run.own(Namespace(late=None, family="native", benchmark=False, repeat=1, jobs=1, validate=True),
                                  [test for test in tests if test["name"] == "skip"], [root], None, dict(os.environ))
            self.assertEqual(counted, 0, out.getvalue())
            self.assertIn("1 known, each with its cause in " + str(run.KNOWN_VALIDATED), out.getvalue().splitlines())

    def test_runner_answers(self):
        # one count of wrong results is two failures when the results differ, and a test that answers differently on
        # a repeat has failed, whatever it said each time
        import run
        from argparse import Namespace
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            chip = subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"], capture_output=True, text=True).stdout.strip()
            (root / "a.txt").write_text("wrong: a 1,1 = 0\nfailed: 1 wrong results\n")
            (root / "hung.txt").write_text("step: 2\n")
            self.assertEqual(run.signature("still running after 31 s: step: 2", root / "hung.txt"), "still running")
            run.KNOWN = root / "known.txt"
            known = run.signature("failed: 1 wrong results", root / "a.txt")
            run.KNOWN.write_text("".join(f"native({chip}) {name} {known} -- the cause\n" for name in ["same", "twin"]))
            script = ("import sys; name, at = sys.argv[1], sys.argv[2]; open(at, 'a').write('x'); runs = len(open(at).read())\n"
                      "said = {'same': 'a', 'twin': 'b', 'flip': 'a' if runs == 1 else None}[name]\n"
                      "print(f'wrong: {said} 1,1 = 0\\nfailed: 1 wrong results' if said else 'passed: 0 wrong results')\n"
                      "sys.exit(1 if said else 0)")
            tests = [{"name": name, "cmd": [sys.executable, "-c", script, name, str(root / name)], "env": {},
                      "timeout": 30, "is_parallel": True} for name in ["same", "twin", "flip"]]
            args = Namespace(late=None, family="native", benchmark=False, repeat=2, jobs=1, record=root / "record.txt")
            with redirect_stdout(StringIO()) as out:
                counted = run.own(args, tests, [root], None, dict(os.environ))
            self.assertEqual(sorted(re.findall(r"^new: native\([^)]*\) (\S+)", out.getvalue(), re.M)), ["flip", "twin"],
                             out.getvalue())
            # what a run records is what a later run compares with: every failure, the unstable one included, which
            # cannot be compared with
            recorded = args.record.read_text().splitlines()
            self.assertEqual(sorted(re.match(r"native\([^)]*\) (\S+)", line)[1] for line in recorded), ["flip", "same", "twin"])
            with self.assertRaises(SystemExit):
                run.known(args.record)
            args.record.write_text("".join(line + "\n" for line in recorded if " flip " not in line))
            self.assertEqual(run.known(args.record)[f"native({chip})", "same"], known)
            self.assertEqual(set(run.known(args.record)), {(f"native({chip})", "same"), (f"native({chip})", "twin")})
            self.assertEqual(counted, 2)
            self.assertRegex(out.getvalue(), r"failed +flip .*unstable: ")
            self.assertEqual([len((root / name).read_text()) for name in ["same", "twin", "flip"]], [2, 2, 2])

    def test_faults(self):
        # a fault is caught only when its test answers differently than on the unfaulted build, whatever known.txt holds:
        # a test that already fails, and fails the same with the fault, has not caught it. the fault is built from the
        # faulted source alone, without the fault before it, and one that does not apply, build or run is not caught
        import faults
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            source, breaks = root / "src", root / "src" / "tests" / "breaks"
            breaks.mkdir(parents=True)
            (source / "meson.build").write_text("project('t', 'c')\nexecutable('value', 'value.c')\n")
            (source / "value.c").write_text('#include <stdio.h>\nint main(void) { puts("1"); return 0; }\n')
            (source / "other.txt").write_text("a\n")
            value = lambda said: f'--- a/value.c\n+++ b/value.c\n@@ -2 +2 @@\n-int main(void) {{ puts("1"); return 0; }}\n+{said}\n'
            (breaks / "t.1.patch").write_text("# a value the test sees\n# family 9\n" + value('int main(void) { puts("2"); return 0; }'))
            (breaks / "t.2.patch").write_text("# a change the test does not see\n--- a/other.txt\n+++ b/other.txt\n@@ -1 +1 @@\n-a\n+b\n")
            (breaks / "t.3.patch").write_text("# made for another source\n" + value("x").replace('"1"', '"3"', 1))
            (breaks / "u.patch").write_text("# does not compile\n" + value("int main(void) {"))
            (breaks / "v.patch").write_text("# its test does not run\n--- a/other.txt\n+++ b/other.txt\n@@ -1 +1 @@\n-a\n+c\n")
            subprocess.run(["meson", "setup", root / "lib", source, "-Dbuildtype=release"], check=True, capture_output=True)
            subprocess.run(["ninja", "-C", root / "lib"], check=True, capture_output=True)
            (root / "tests" / "meson-info").mkdir(parents=True)
            (root / "tests" / "meson-info" / "intro-tests.json").write_text(json.dumps([{"name": n} for n in ["t", "t_dxil", "v"]]))
            calls = root / "calls"
            # the unfaulted value 1 fails the test, as a known failure would; only another value is a new answer
            (root / "run.py").write_text(
                "import subprocess, sys\n"
                f"a = sys.argv; open({str(calls)!r}, 'a').write(' '.join(a[1:]) + '\\n')\n"
                "if a[a.index('--exact') + 1] == 'v':\n    sys.exit('no such test')\n"
                "said = subprocess.run([a[2] + '/value'], capture_output=True, text=True).stdout.strip()\n"
                "answer = '' if said == '0' else f'9 t failed: {said}'\n"
                "if '--record' in a:\n    open(a[a.index('--record') + 1], 'w').write(answer + ' -- before\\n' if answer else '')\n"
                "known = open(a[a.index('--known') + 1]).read() if '--known' in a else ''\n"
                "print('0 known, each with its cause in known.txt')\n"
                "if answer and answer + ' -- before' not in known:\n    print('new: ' + answer)\n")
            faults.SOURCE, faults.RUN = source, root / "run.py"
            with redirect_stdout(StringIO()) as out:
                missed = faults.main(["--build", str(root / "lib"), "--build", str(root / "tests"), "--wine", str(root),
                                      "t", "u", "v"])
            self.assertEqual(out.getvalue().splitlines(), ["t.1.patch: caught, 9 t failed: 2", "t.2.patch: NOT CAUGHT",
                                                           "t.3.patch: does not apply", "u.patch: does not build",
                                                           "v.patch: did not run: no such test"])
            self.assertEqual(missed, 4)
            ran = calls.read_text().splitlines()
            self.assertEqual(len(ran), 5, ran)
            self.assertTrue(ran[0].startswith("--build " + str(root / "lib") + " ") and "--record" in ran[0], ran[0])
            self.assertTrue(ran[1].startswith("--build " + str(root / "lib-fault") + " ") and "--known" in ran[1], ran[1])
            self.assertIn("--build " + str(root / "tests"), ran[1])
            self.assertIn("--family 9 --known", ran[1])
            self.assertTrue(ran[1].endswith("--exact t t_dxil"), ran[1])
            self.assertIn("--family native --record", ran[2])

    def test_stop_and_deadline(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            control = tool.Control(float("inf"), root / "STOP")
            (root / "STOP").touch()
            with self.assertRaises(tool.Ended):
                control.command([sys.executable, "-c", "raise RuntimeError('must not run')"], root / "log")
            self.assertFalse((root / "log").exists())
            (root / "STOP").unlink()
            control = tool.Control(0.15, root / "STOP")
            started = time.monotonic()
            with self.assertRaises(tool.Ended):
                control.command([sys.executable, "-c", "import time; time.sleep(60)"], root / "log")
            self.assertLess(time.monotonic() - started, 6)

    def test_sigterm_reaps_child(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            child_pid = root / "child.pid"
            child = ("import os,time;from pathlib import Path;Path(" + repr(str(child_pid)) +
                     ").write_text(str(os.getpid()));time.sleep(60)")
            code = f'''import signal, sys
sys.path.insert(0, {str(Path(__file__).resolve().parents[1])!r})
from pathlib import Path
import mutate
c = mutate.Control(float('inf'), Path({str(root / 'STOP')!r}))
signal.signal(signal.SIGTERM, lambda *_: setattr(c, 'reason', 'SIGTERM'))
try:
    c.command([sys.executable, '-c', {child!r}], Path({str(root / 'log')!r}))
except mutate.Ended:
    sys.exit(0)
sys.exit(1)
'''
            process = subprocess.Popen([sys.executable, "-c", code], stdin=subprocess.DEVNULL)
            try:
                limit = time.monotonic() + 5
                while time.monotonic() < limit and (not child_pid.exists() or not child_pid.read_text()):
                    time.sleep(0.02)
                self.assertTrue(child_pid.exists())
                process.send_signal(signal.SIGTERM)
                self.assertEqual(process.wait(timeout=6), 0)
                with self.assertRaises(ProcessLookupError):
                    os.kill(int(child_pid.read_text()), 0)
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=6)

    def test_isolation_and_interruption(self):
        from argparse import Namespace
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "alias"
            root.symlink_to(Path(tmp), target_is_directory=True)
            source, copy, build = root / "source", root / "ta-mut-src", root / "ta-mut"
            (source / "src").mkdir(parents=True)
            (build / "meson-info").mkdir(parents=True)
            original = "int f() { return 1; }\nint g() { return 2; }\n"
            (source / "src/test.cpp").write_text(original)
            (build / "meson-info/meson-info.json").write_text(json.dumps({"directories": {"source": str(copy)}}))
            funcs, made = tool.mutations(original, "src/test.cpp")
            index = {"source_hash": "source", "test_binaries": {}, "complete": True,
                     "functions": {"src/test.cpp": {"1": ["a"], "2": ["a", "b"]}},
                     "tests": {key: {"name": key, "family": "native", "seconds": n}
                               for n, key in enumerate(["a", "b"])}}
            coverage = root / "coverage.json"
            coverage.write_text(json.dumps(index))
            commands = root / "commands.json"
            commands.write_text("[]")
            index["commands"] = tool.digest(commands.read_text())
            coverage.write_text(json.dumps(index))
            (build / "meson-info/intro-buildoptions.json").write_text("[]")
            args = Namespace(source=source, coverage=coverage, results=root / "results.json",
                             build=build, validate=False, sample=None,
                             commands=commands, files=["src/*"], tests_build=build)

            class Commands:
                def check(self):
                    pass

                def command(self, command, log):
                    if command[0] == "rsync":
                        shutil.copytree(source, copy, dirs_exist_ok=True)
                    return 0, 0.1

            calls = []

            def execute(args, test, build, log, control):
                text = (copy / "src/test.cpp").read_text()
                calls.append((test["name"], text))
                if text != original:
                    raise tool.Ended("test cancellation")
                return "passed", 0.1

            with patch.object(tool, "inventory", return_value=(funcs, made, [])), \
                 patch.object(tool, "fingerprint", return_value="source"), \
                 patch.object(tool, "artifacts", return_value={}), \
                 patch.object(tool, "execute", side_effect=execute):
                with self.assertRaises(tool.Ended):
                    tool.run_mutants(args, Commands(), root)
            self.assertEqual([c[0] for c in calls[:2]], ["a", "b"])
            self.assertTrue(all(c[1] == original for c in calls[:2]))
            self.assertNotEqual(calls[2][1], original)
            self.assertEqual(calls[2][1].splitlines()[0], original.splitlines()[0])
            self.assertEqual((source / "src/test.cpp").read_text(), original)
            self.assertEqual((copy / "src/test.cpp").read_text(), original)
            records = json.loads(args.results.read_text())["results"]
            self.assertEqual([r["status"] for r in records.values()], ["interrupted"])
            calls.clear()

            def fail(args, test, build, log, control):
                calls.append(test["name"])
                return ("passed" if log.name.startswith("baseline-") else "failed"), 0.1

            reached = [m for m in made if m["function"] == "g"]
            with patch.object(tool, "inventory", return_value=(funcs, reached[:1], [])), \
                 patch.object(tool, "fingerprint", return_value="source"), \
                 patch.object(tool, "artifacts", return_value={}), \
                 patch.object(tool, "execute", side_effect=fail):
                with redirect_stdout(StringIO()):
                    tool.run_mutants(args, Commands(), root)
                    tool.run_mutants(args, Commands(), root)
            self.assertEqual(calls, ["a", "b", "a"])
            records = json.loads(args.results.read_text())["results"]
            self.assertEqual([r["status"] for r in records.values()], ["killed"])
            output = StringIO()
            with redirect_stdout(output):
                tool.report(args.results)
            self.assertIn("src/test.cpp: 1/1 killed (100.0%)", output.getvalue())
            args.sample = [1, 17]
            with patch.object(tool, "inventory", return_value=(funcs, reached, [])), \
                 patch.object(tool, "fingerprint", return_value="source"), \
                 patch.object(tool, "artifacts", return_value={}), \
                 patch.object(tool, "execute", side_effect=fail), redirect_stdout(StringIO()):
                tool.run_mutants(args, Commands(), root)
                calls.clear()
                tool.run_mutants(args, Commands(), root)
            self.assertFalse(calls, "resuming the same sample ran a completed mutant")
            state = json.loads(args.results.read_text())
            self.assertEqual(state["sample"], [1, 17])
            self.assertEqual(len(state["plan"]), 1)
            self.assertEqual(state["selected"], len(reached))
            self.assertEqual(state["population"], {"src/test.cpp": len(reached)})


if __name__ == "__main__":
    result = unittest.main(exit=False).result
    print("passed: mutation runner" if result.wasSuccessful() else "failed: mutation runner")
    sys.exit(not result.wasSuccessful())
