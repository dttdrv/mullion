#!/usr/bin/env python3
"""makes one source fault at a time and measures which tests notice it.

source and test selection are read-only. `collect` runs the coverage build's tests, each with its own hook notes
and shader directory; `list` preprocesses the build's translation units, without compiling, and lists mutations.
`run` copies the source to ta-mut-src and uses the existing ta-mut build, as lanes/t-gate.sh's caught does.
source lines absent from the preprocessor, directives, literals, assertions and logging calls are excluded.
lexical operators are deliberately conservative; this is a mutation score, not a proof of correctness.

PE functions use the coverage hook's module-relative PCs, symbolized with the coverage build's toolchain. the
unix converter has no hook: every test that emitted a shader is a candidate for every converter function. other
uninstrumented unix functions use every GPU test. these conservative selections are labelled, never reported as
measured function coverage. only functions with a measured selection, or such a candidate selection, are mutated.

source, coverage and test changes invalidate old results. skipped tests cannot kill a mutant or establish survival.
the baseline must pass in ta-mut before a test may kill a mutant. run.py owns the GPU lock and the Wine process;
this runner owns only its subprocesses. do not run t-gate.sh concurrently: it uses the same copy, build and Wine.

source/build paths default to the Mullion project beside lanes/t-gate.sh. source globs include the src/ prefix.
examples (after sourcing crossover/tools/env.sh):
  python3 tests/mutate.py collect --budget 3600
  python3 tests/mutate.py list --files 'src/d3d11/*'
  python3 tests/mutate.py run --files 'src/**' --budget 28800
  python3 tests/mutate.py run --files 'src/**' --sample 1000 17 --budget 28800
  python3 tests/mutate.py report
"""
import argparse
import bisect
import contextlib
import fcntl
import fnmatch
import hashlib
import itertools
import json
import os
import random
import re
import shlex
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.dont_write_bytecode = True
import coverage as coverage_tools
import run as test_run

ROOT = Path(__file__).resolve().parents[1]
# C++ lexical tokens, including raw strings and preprocessing numbers (C++ [lex.pptoken], [lex.string]).
TOKEN = re.compile(r'''(?P<comment>//(?:\\\r?\n|[^\n])*|/\*.*?\*/)|
    (?P<string>(?:u8|u|U|L)?R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})\(.*?\)(?P=delimiter)"|
    (?:u8|u|U|L)?"(?:\\.|[^"\\])*"|(?:u8|u|U|L)?'(?:\\.|[^'\\])*')|
    (?P<number>(?:\d|\.\d)(?:[eEpP][+-]|[\w'.])*)|
    (?P<word>[a-zA-Z_]\w*)|(?P<op><=>|<<=|>>=|->\*|\.\*|::|->|\+\+|--|&&|\|\||<=|>=|==|!=|
    <<|>>|[+*/%&|^!-]=|\.\.\.|[^\s])''', re.S | re.X)
CONTROL = {"if", "for", "while", "switch", "catch", "sizeof", "alignof", "decltype", "noexcept", "requires"}
PROTECTED = re.compile(r"^(?:assert|static_assert|_Static_assert|[A-Z_]*ASSERT[A-Z_]*|ERR|WARN|TRACE|DEBUG|INFO|FIXME|"
                       r"DXMT_UNREACHABLE|printf|fprintf|snprintf|NSLog|os_log\w*)$")
SOURCE_SUFFIXES = {".cpp", ".c", ".cc", ".cxx", ".hpp", ".h", ".hh", ".mm", ".m", ".metal", ".inc"}


def digest(text):
    return hashlib.sha256(text.encode()).hexdigest()


def tokens(source):
    directives = [(m.start(), m.end()) for m in re.finditer(r"^[ \t]*#(?:[^\n]*\\\n)*[^\n]*", source, re.M)]
    return [(m[0], m.start(), m.end(), m.lastgroup) for m in TOKEN.finditer(source)
            if m.lastgroup != "comment" and not any(a <= m.start() < b for a, b in directives)]


def pairs(ts):
    stack, found = [], {}
    for i, (value, *_) in enumerate(ts):
        if value in ("(", "[", "{"):
            stack.append(i)
        elif value in (")", "]", "}") and stack and ts[stack[-1]][0] == {")": "(", "]": "[", "}": "{"}[value]:
            begin = stack.pop()
            found[begin], found[i] = i, begin
    return found


def functions(source, file, ts, paired):
    found = []
    for i, token in enumerate(ts):
        if token[0] != "{" or i not in paired:
            continue
        start = i - 1
        while start >= 0 and ts[start][0] not in (";", "{", "}"):
            start = paired[start] - 1 if ts[start][0] in (")", "]") and start in paired else start - 1
        signature = i - 1 if i and ts[i - 1][0] == "]" else None
        for j in range(start + 1, i):
            if (ts[j][0] == "(" and j in paired and paired[j] < i and j and ts[j - 1][0] == "]" and
                not any(t[0] in ("{", "}") for t in ts[paired[j] + 1:i])):
                signature = j - 1
                break
        skipped = -1
        for j in range(start + 1, i):
            if signature is not None:
                break
            if j <= skipped:
                continue
            if ts[j][0] != "(" or j not in paired or paired[j] >= i or j == 0:
                continue
            prev = ts[j - 1][0]
            if prev in ("if", "for", "while", "switch", "catch"):
                signature = None
                break
            if prev in CONTROL or prev in ("__attribute__", "__declspec", "alignas"):
                skipped = paired[j]
                continue
            if prev == "]":
                signature = j - 1
                break
            if ts[j - 1][3] == "word" and signature is None:
                signature = j - 1
                break
        if signature is None:
            continue
        name, offset = ts[signature][:2]
        if name == "]":
            name = f"lambda@{source.count(chr(10), 0, offset) + 1}"
            offset = ts[paired[signature]][1]
        found.append({"file": file, "name": name, "line": source.count("\n", 0, offset) + 1,
                      "start": ts[start + 1][1], "body": token[1], "end": ts[paired[i]][2]})
    return found


def mutations(source, file, active=None, control=None):
    lines = [0] + [m.end() for m in re.finditer("\n", source)]
    ts = tokens(source)
    if active is not None:
        ts = [t for t in ts if bisect.bisect_right(lines, t[1]) in active]
    paired = pairs(ts)
    funcs = functions(source, file, ts, paired)
    protected = []
    for i, (value, start, end, _) in enumerate(ts):
        if PROTECTED.match(value) or value == "Logger":
            j = i + 1
            while j < len(ts) and ts[j][0] not in ("(", ";", "{", "}"):
                j += 1
            if j < len(ts) and ts[j][0] == "(" and j in paired:
                protected.append((start, ts[paired[j]][2]))
            else:
                protected.append((start, end))
    made = {}

    def add(begin, end, new, operator):
        start, stop = ts[begin][1], ts[end][2]
        func = min((f for f in funcs if f["body"] < start < f["end"]),
                   key=lambda f: f["end"] - f["start"], default=None)
        if not func or any(a < stop and start < b for a, b in protected):
            return
        if active is not None and any(bisect.bisect_right(lines, ts[i][1]) not in active
                                      for i in range(begin, end + 1)):
            return
        line = bisect.bisect_right(lines, start)
        col = start - lines[line - 1] + 1
        first = lines[line - 1]
        last = source.find("\n", stop)
        last = len(source) if last < 0 else last
        original = source[first:last]
        identity = f"{file}:{line}:{col}:{operator}:{digest(original)}"
        made[identity] = {"id": identity, "file": file, "line": line, "column": col, "operator": operator,
                          "line_hash": digest(original), "function": func["name"], "function_line": func["line"],
                          "start": start, "end": stop, "old": source[start:stop], "new": new,
                          "original": original, "mutated": source[first:start] + new + source[stop:last]}

    replacements = {"<=": "<", ">=": ">", "&&": "||", "||": "&&", "true": "false", "false": "true"}
    for i, (value, start, end, kind) in enumerate(ts):
        if control:
            control.check()
        if value in ("if", "while") and i + 1 in paired and ts[i + 1][0] == "(":
            close = paired[i + 1]
            if close > i + 2 and not any(t[0] in (";", "auto", "=", "const", "volatile") for t in ts[i + 2:close]):
                add(i + 2, close - 1, "!(" + source[ts[i + 2][1]:ts[close - 1][2]] + ")", "negate-condition")
        if value in replacements:
            add(i, i, replacements[value], "boundary" if value in ("<=", ">=") else
                "logical" if value in ("&&", "||") else "boolean")
        if value in ("<", ">"):
            # plain relational expressions; template brackets are not arithmetic boundaries
            left = i - 1
            while left >= 0 and ts[left][0] not in (";", "{", "}", "(", "&&", "||"):
                left -= 1
            right = i + 1
            while right < len(ts) and ts[right][0] not in (";", "{", "}", ")", "&&", "||"):
                right += 1
            segment = ts[left + 1:right]
            if sum(t[0] in ("<", ">") for t in segment) == 1 and i and i + 1 < len(ts):
                add(i, i, value + "=", "boundary")
        if value in ("+", "-") and i and i + 1 < len(ts):
            if ts[i - 1][3] in ("word", "number") or ts[i - 1][0] in (")", "]"):
                add(i, i, "-" if value == "+" else "+", "arithmetic")
                if ts[i + 1][3] == "word":
                    last = i + 1
                    while last + 2 < len(ts) and ts[last + 1][0] in (".", "->") and ts[last + 2][3] == "word":
                        last += 2
                    if last + 1 < len(ts) and ts[last + 1][0] in (";", ",", ")", "]", "<", ">", "<=", ">="):
                        add(i, last, "", "drop-offset")
        if kind == "number" and re.fullmatch(r"(?:0[xX][\da-fA-F']+|0[bB][01']+|[\d']+)[uUlL]*", value):
            if i and ts[i - 1][0] != "case":
                add(i, i, f"({value} + 1)", "constant-up")
                add(i, i, f"({value} - 1)", "constant-down")
        if value == "[" and i in paired and i and ts[i - 1][3] == "word":
            close = paired[i]
            if close > i + 1 and any(t[3] == "word" for t in ts[i + 1:close]):
                if i < 2 or ts[i - 2][3] != "word" or ts[i - 2][0] == "return":
                    add(i, close, "[0]", "drop-index")
        if i >= 2 and i + 4 < len(ts) and ts[i - 1][0] in (".", "->") and kind == "word":
            # fields already compared on the same object have compatible operands, unlike arbitrary neighbours
            if ts[i + 1][0] in ("<", "<=", ">", ">=", "==", "!=") and ts[i - 2][0] == ts[i + 2][0] and (
                ts[i - 1][0] == ts[i + 3][0] and ts[i + 4][3] == "word" and value != ts[i + 4][0]):
                add(i, i, ts[i + 4][0], "neighbour-field")
        if value == ";":
            begin = i - 1
            while begin >= 0 and ts[begin][0] not in (";", "{", "}"):
                if ts[begin][0] in (")", "]") and begin in paired:
                    opening = paired[begin]
                    if opening and ts[opening - 1][0] in CONTROL:
                        break
                    begin = opening
                begin -= 1
            begin += 1
            stmt = ts[begin:i]
            if not stmt:
                continue
            values = [t[0] for t in stmt]
            assignment = len(stmt) > 1 and stmt[0][3] == "word" and values[1] in ("=", "+=", "-=", ".", "->")
            call = stmt[0][3] == "word" and "(" in values and "=" not in values and (
                len(stmt) > 1 and values[1] in ("(", "::", ".", "->"))
            if values[0] in ("return", "break", "continue") or assignment or call:
                add(begin, i, ";", "remove-statement")
    return funcs, sorted(made.values(), key=lambda m: (m["file"], m["start"], m["operator"]))


def apply(source, mutant):
    if source[mutant["start"]:mutant["end"]] != mutant["old"]:
        raise ValueError(f"source changed at {mutant['id']}")
    return source[:mutant["start"]] + mutant["new"] + source[mutant["end"]:]


def save(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode="w", dir=path.parent, delete=False) as out:
        temporary = Path(out.name)
        try:
            json.dump(data, out, indent=1, sort_keys=True)
            out.write("\n")
            out.flush()
            os.fsync(out.fileno())
            os.replace(temporary, path)
        finally:
            temporary.unlink(missing_ok=True)


class Ended(Exception):
    pass


class Control:
    def __init__(self, budget, stop):
        self.deadline = time.monotonic() + budget
        self.stop = stop
        self.reason = ""

    def check(self):
        if self.reason or self.stop.exists() or time.monotonic() >= self.deadline:
            raise Ended(self.reason or (f"{self.stop} appeared" if self.stop.exists() else "budget used up"))

    def command(self, argv, log, errors=None, input_path=None, **kwargs):
        self.check()
        started = time.monotonic()
        log.parent.mkdir(parents=True, exist_ok=True)
        with contextlib.ExitStack() as opened:
            out = opened.enter_context(log.open("wb"))
            err = opened.enter_context(errors.open("wb")) if errors else subprocess.STDOUT
            stdin = opened.enter_context(input_path.open("rb")) if input_path else subprocess.DEVNULL
            process = subprocess.Popen(list(map(str, argv)), stdout=out, stderr=err,
                                       stdin=stdin, start_new_session=True, **kwargs)
            try:
                while process.poll() is None:
                    self.check()
                    time.sleep(0.1)
                self.check()
                return process.returncode, time.monotonic() - started
            finally:
                if process.poll() is None:
                    try:
                        os.killpg(process.pid, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        try:
                            os.killpg(process.pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                        process.wait()


def fingerprint(source, with_tests=True):
    value = hashlib.sha256()
    for where in ("src", "include", "libs", "external", *(("tests",) if with_tests else ())):
        for path in sorted((source / where).rglob("*")):
            if path.is_file() and "__pycache__" not in path.parts and ".git" not in path.parts:
                value.update(str(path.relative_to(source)).encode())
                value.update(path.read_bytes())
    for path in (source / "meson.build", source / "meson.options"):
        value.update(path.read_bytes())
    return value.hexdigest()


def artifacts(build, tests=False):
    if tests:
        declared = json.loads((build / "meson-info/intro-tests.json").read_text())
        paths = {Path(t["cmd"][0]) for t in declared if t["cmd"][0].endswith(".exe")}
    else:
        unix, pe = test_run.libraries([build])
        paths = set(unix.values()) | set(pe.values())
    return {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(paths)}


def source_path(path, bases):
    for base in bases:
        try:
            return str(Path(path).resolve().relative_to(base.resolve()))
        except ValueError:
            pass
    return None


def active_lines(args, control, temporary):
    """preprocessing only: the compiler decides the active platform/configuration branches, not this parser"""
    commands = json.loads(args.commands.read_text())
    old = Path(json.loads((args.commands.parent / "meson-info/meson-info.json").read_text())["directories"]["source"])
    log = temporary / "ninja-commands.txt"
    code, _ = control.command(["ninja", "-C", args.commands.parent, "-t", "commands"], log)
    if code:
        raise RuntimeError(f"cannot read Metal preprocessing commands: {log.read_text()}")
    for text in log.read_text().splitlines():
        argv = shlex.split(text)
        sources = [part for part in argv if part.endswith(".metal")]
        if sources and "-c" in argv:
            commands.append({"directory": str(args.commands.parent), "arguments": argv, "file": sources[0]})
    active = {}
    seen = set()
    marker = re.compile(r'^#\s+(\d+)\s+"([^"]+)"')
    for entry in commands:
        control.check()
        original = (Path(entry["directory"]) / entry["file"]).resolve()
        relative = source_path(original, [old])
        if not relative or not relative.startswith("src/"):
            continue
        argv = entry.get("arguments") or shlex.split(entry["command"])
        rewritten, i = [], 0
        while i < len(argv):
            part = argv[i]
            if part in ("-o", "-MF", "-MQ", "-MT"):
                i += 2
                continue
            if part not in ("-c", "-MD", "-MMD"):
                part = part.replace(str(old), str(args.source))
                part = part.replace(os.path.relpath(old, entry["directory"]), str(args.source))
                rewritten.append(part)
            i += 1
        log = temporary / "preprocess.txt"
        errors = temporary / "preprocess-errors.txt"
        identity = (entry["directory"], tuple(rewritten))
        if identity in seen:
            continue
        seen.add(identity)
        code, _ = control.command([*rewritten, *(f"-I{p}" for p in args.include), "-E", "-fno-color-diagnostics"],
                                  log, errors=errors, cwd=entry["directory"])
        if code:
            raise RuntimeError(f"preprocessing {relative} failed:\n" + errors.read_text(errors="replace"))
        file, line = None, 0
        with log.open(errors="replace") as output:
            for text in output:
                match = marker.match(text)
                if match:
                    named = Path(match[2])
                    if not named.is_absolute():
                        named = Path(entry["directory"]) / named
                    file = source_path(named, [args.source, old])
                    line = int(match[1])
                else:
                    if file and file.startswith("src/") and text.strip():
                        active.setdefault(file, set()).add(line)
                    line += 1
    return active


def inventory(args, control, temporary):
    active = active_lines(args, control, temporary)
    funcs, made, excluded = [], [], []
    for path in sorted((args.source / "src").rglob("*")):
        control.check()
        file = str(path.relative_to(args.source))
        if not path.is_file() or path.suffix not in SOURCE_SUFFIXES or not any(
            fnmatch.fnmatchcase(file, p) for p in args.files
        ):
            continue
        text = path.read_bytes().decode()
        if file not in active:
            excluded.append(file)
            continue
        found, changes = mutations(text, file, active[file], control)
        starts = [0] + [m.end() for m in re.finditer("\n", text)]
        funcs += [f for f in found if bisect.bisect_right(starts, f["body"]) in active[file]]
        made += changes
    return funcs, made, excluded


def route(funcs, made, index):
    selection, unreached = {}, []
    for f in funcs:
        key = (f["file"], f["line"], f["name"])
        reached = index.get("functions", {}).get(f["file"], {}).get(str(f["line"]), [])
        mode = "measured"
        if f["file"].startswith("src/airconv/"):
            kind = "DXBC" if Path(f["file"]).name.startswith("dxbc_") else (
                "DXIL" if Path(f["file"]).name.startswith("dxil_") else None)
            reached = [name for name, t in index["tests"].items()
                       if t.get("shaders") and (not kind or kind in t.get("shader_kinds", []))]
            mode = "shader candidates"
        elif "/unix/" in f["file"]:
            reached = [name for name, t in index["tests"].items() if t.get("gpu")]
            mode = "unix candidates"
        selection[key] = (sorted(set(reached), key=lambda n: (index["tests"][n]["seconds"], n)), mode)
        if not reached:
            unreached.append(dict(f, coverage=mode))
    selected = []
    for m in made:
        tests, mode = selection.get((m["file"], m["function_line"], m["function"]), ([], "unknown"))
        if tests:
            selected.append(dict(m, tests=tests, coverage=mode))
    return selected, unreached


def verdict(text, code):
    words = re.findall(r"^\s*(?:native|\d+)\s+(passed|failed|skipped)\s+\S+\s+[\d.]+\s+s\b", text, re.M)
    if len(words) != 1 or (words[0] != "failed" and code) or (words[0] == "failed" and not code):
        return "error"
    return words[0]


def execute(args, test, build, log, control, env=None):
    name, family = test["name"], test["family"]
    command = [sys.executable, args.source / "tests/run.py", "--build", build, "--build", args.tests_build,
               "--wine", args.wine, "--prefix", args.prefix, "--exact", "--family", family, name]
    if args.validate:
        command.append("--validate")
    environment = dict(os.environ, MULLION_TEST_LOCK=str(args.lock))
    environment.pop("MULLION_COVERAGE", None)
    environment.pop("MULLION_TEST_SHADERS", None)
    environment.update(env or {})
    code, seconds = control.command(command, log, env=environment)
    raw = build / "meson-logs/tests" / f"{name}.{family}.0.txt"
    if raw.exists():
        log.with_suffix(".test.txt").write_bytes(raw.read_bytes())
    return verdict(log.read_text(errors="replace"), code), seconds


def symbolized(args, directory, funcs, control, temporary):
    _, bin = coverage_tools.tools(args.coverage_build)
    libraries = test_run.libraries([args.coverage_build])[1]
    libraries = {name.lower(): path for name, path in libraries.items()}
    locations = set()
    source = Path(json.loads((args.coverage_build / "meson-info/meson-info.json").read_text())["directories"]["source"])
    by_file = {}
    for f in funcs:
        by_file.setdefault(f["file"], []).append(f)
    for module, dll in sorted(libraries.items()):
        addresses = sorted({int(word, 16) for note in directory.glob("*.txt")
                            if note.name.lower().startswith(module + ".") for word in note.read_text().split()})
        if not addresses:
            continue
        # llvm-symbolizer's JSON on stdin is one object per address; --relative-address matches hook.c's offsets
        log = temporary / "symbols.txt"
        inputs = temporary / "addresses.txt"
        inputs.write_text("\n".join(hex(a) for a in addresses) + "\n")
        code, _ = control.command([bin / "llvm-symbolizer", f"--obj={dll}", "--relative-address",
                                   "--output-style=JSON"], log, input_path=inputs)
        if code:
            raise RuntimeError(f"symbolization failed for {dll}: {log.read_text()}")
        with log.open() as output:
            for row in output:
                address = json.loads(row)
                for frame in address.get("Symbol", []):
                    if not frame["FileName"] or not frame["Line"]:
                        raise RuntimeError(f"{dll} has no source locations; configure coverage with -Ddebug=true")
                    file = source_path(frame["FileName"], [source, args.source])
                    if not file or file not in by_file:
                        continue
                    line = frame.get("StartLine") or frame["Line"]
                    text = (args.source / file).read_bytes().decode()
                    starts = [0] + [m.end() for m in re.finditer("\n", text)]
                    if not 0 < line <= len(starts):
                        continue
                    offset = starts[line - 1]
                    candidates = [f for f in by_file[file] if f["start"] <= offset < f["end"] or f["line"] == line]
                    if candidates:
                        f = min(candidates, key=lambda f: f["end"] - f["start"])
                        locations.add((file, f["line"]))
                    else:
                        raise RuntimeError(f"cannot map covered function {frame['FunctionName']} at {file}:{line}")
    return locations


def collect(args, control, temporary):
    options = json.loads((args.coverage_build / "meson-info/intro-buildoptions.json").read_text())
    if not next((o["value"] for o in options if o["name"] == "debug"), False):
        raise RuntimeError("coverage build needs -Ddebug=true for source locations; rebuild it before collecting")
    for name in ("c_args", "cpp_args"):
        if coverage_tools.FLAG not in next((o["value"] for o in options if o["name"] == name), []):
            raise RuntimeError(f"coverage build is missing {coverage_tools.FLAG} in {name}")
    info = json.loads((args.coverage_build / "meson-info/meson-info.json").read_text())
    built_source = Path(info["directories"]["source"])
    if fingerprint(args.source, False) != fingerprint(built_source, False):
        raise RuntimeError("coverage build's source differs from --source; rebuild coverage from this source first")
    funcs, _, _ = inventory(argparse.Namespace(**{**vars(args), "files": ["src/*"]}), control, temporary)
    provenance = {"source_hash": fingerprint(args.source), "libraries": artifacts(args.coverage_build),
                  "test_binaries": artifacts(args.tests_build, True), "families": args.family,
                  "validate": args.validate,
                  "commands": digest(args.commands.read_text())}
    index = dict(provenance, functions={}, tests={}, skipped={}, complete=False)
    if args.coverage.exists():
        previous = json.loads(args.coverage.read_text())
        if all(previous.get(k) == v for k, v in provenance.items()):
            index = previous
    save(args.coverage, index)
    declared = json.loads((args.tests_build / "meson-info/intro-tests.json").read_text())
    for test in sorted(declared, key=lambda t: t["name"]):
        if test["name"] == "host_mutate":
            continue
        for family in args.family.split(","):
            control.check()
            if not test["cmd"][0].endswith(".exe") and family != args.family.split(",")[0]:
                continue
            key = test["name"] + "@" + family
            if key in index["tests"] or key in index["skipped"]:
                continue
            directory = args.coverage.parent / "notes" / key
            # each retry gets fresh notes, so an interrupted run cannot supply coverage of the next
            with tempfile.TemporaryDirectory(prefix="collect-", dir=temporary) as tmp:
                notes = Path(tmp) / "notes"
                shaders = Path(tmp) / "shaders"
                notes.mkdir()
                shaders.mkdir()
                windows = lambda p: "Z:" + str(p).replace("/", "\\")
                entry = {"name": test["name"], "family": family, "gpu": test["cmd"][0].endswith(".exe")}
                word, seconds = execute(args, entry, args.coverage_build, args.coverage.parent / "logs" / f"{key}.txt",
                                        control, {"MULLION_COVERAGE": windows(notes),
                                                  "MULLION_TEST_SHADERS": windows(shaders)})
                if word == "failed" or word == "error":
                    raise RuntimeError(f"baseline {key}: {word}; see {args.coverage.parent / 'logs' / (key + '.txt')}")
                if word == "skipped":
                    index["skipped"][key] = {"verdict": word}
                    save(args.coverage, index)
                    print(f"{key}: skipped, excluded from coverage", flush=True)
                    continue
                kinds = {kind for shader in shaders.glob("*.dxbc") for kind in coverage_tools.shader_parts(shader)}
                entry.update(seconds=seconds, shaders=bool(kinds),
                             shader_kinds=sorted(({"DXIL"} if "DXIL" in kinds else set()) |
                                                 ({"DXBC"} if kinds & {"SHEX", "SHDR"} else set())))
                for file, line in symbolized(args, notes, funcs, control, temporary):
                    index["functions"].setdefault(file, {}).setdefault(str(line), []).append(key)
                directory.mkdir(parents=True, exist_ok=True)
                for note in notes.glob("*.txt"):
                    (directory / note.name).write_bytes(note.read_bytes())
                index["tests"][key] = entry
                save(args.coverage, index)
                print(f"{key}: {seconds:.1f} s, shader candidates={entry['shaders']}", flush=True)
    if not index["functions"]:
        raise RuntimeError("no PE function notes mapped; coverage collection cannot establish unreached functions")
    if (provenance["source_hash"] != fingerprint(args.source) or
        provenance["libraries"] != artifacts(args.coverage_build) or
        provenance["test_binaries"] != artifacts(args.tests_build, True)):
        raise RuntimeError("source or binaries changed during collection; collect again")
    index["complete"] = True
    save(args.coverage, index)


def finished(record, context):
    return (record.get("context") == context and
            record.get("status") in ("killed", "survived", "not-built", "inconclusive"))


def schedule(selected, sample):
    if not sample:
        return sorted(selected, key=lambda m: (-len(m.get("tests", [])), m["file"], m["start"], m["operator"]))
    rng = random.Random(sample[1])
    files = {}
    for m in sorted(selected, key=lambda m: m["id"]):
        files.setdefault(m["file"], []).append(m)
    pools = list(files.values())
    for pool in pools:
        rng.shuffle(pool)
    rng.shuffle(pools)
    return list(itertools.islice((m for row in itertools.zip_longest(*pools) for m in row if m), sample[0]))


def estimate(records, population):
    killed, valid, variance = 0, 0, 0
    total = sum(population.values())
    for file, size in population.items():
        rows = [r for r in records if r["file"] == file]
        count = len(rows)
        if not count or any(r["status"] not in ("killed", "survived", "not-built") for r in rows):
            return None
        weight = size / total
        killed += weight * sum(r["status"] == "killed" for r in rows) / count
        valid += weight * sum(r["status"] in ("killed", "survived") for r in rows) / count
        if count < size:
            variance += weight ** 2 * (size - count) / (4 * count * (size - 1))
    if not valid:
        return None
    # STAT 506 lessons 2 and 6 (online.stat.psu.edu): finite-population variance, p(1-p) <= 1/4.
    # Chebyshev at 2.5% for each of killed and valid gives a joint conservative 95% ratio bound.
    radius = (variance / ((1 - 0.95) / 2)) ** 0.5
    low = max(0, killed - radius) / min(1, valid + radius)
    high = min(1, (killed + radius) / (valid - radius)) if valid > radius else 1
    return killed / valid, low, high


def run_mutants(args, control, temporary):
    index = json.loads(args.coverage.read_text())
    if not index.get("complete"):
        raise RuntimeError("coverage collection is incomplete; resume collect before reporting unreached functions")
    if index["source_hash"] != fingerprint(args.source):
        raise RuntimeError("source or tests changed since coverage collection; collect again before mutating")
    if index["test_binaries"] != artifacts(args.tests_build, True):
        raise RuntimeError("test binaries changed since coverage collection; collect again before mutating")
    if index["commands"] != digest(args.commands.read_text()):
        raise RuntimeError("preprocessing configuration changed since coverage collection; collect again")
    funcs, made, excluded = inventory(args, control, temporary)
    selected, unreached = route(funcs, made, index)
    population = {file: sum(m["file"] == file for m in selected) for file in sorted({m["file"] for m in selected})}
    planned = schedule(selected, args.sample)
    context = digest(json.dumps(index, sort_keys=True) + str(args.validate) + repr(sorted(args.files)) +
                     args.commands.read_text() + json.dumps(artifacts(args.tests_build, True), sort_keys=True) +
                     (args.build / "meson-info/intro-buildoptions.json").read_text())
    state = json.loads(args.results.read_text()) if args.results.exists() else {"results": {}}
    state.update(context=context, unreached=unreached, excluded=excluded, candidates=len(made), selected=len(selected),
                 files=sorted({f["file"] for f in funcs}), plan=[m["id"] for m in planned],
                 sample=args.sample, population=population)
    save(args.results, state)
    directories = json.loads((args.build / "meson-info/meson-info.json").read_text())["directories"]
    copy = Path(directories["source"]).resolve()
    source = args.source.resolve()
    if (copy != (args.build.parent / "ta-mut-src").resolve() or copy == source or source in copy.parents or
        copy in source.parents):
        raise RuntimeError(f"ta-mut must be configured for its own sibling ta-mut-src, not {copy}")
    code, _ = control.command(["rsync", "-rlpc", "--delete", "--exclude", ".git", "--exclude", "build",
                               "--exclude", "__pycache__", str(args.source) + "/", str(copy) + "/"],
                              temporary / "copy.txt")
    if code:
        raise RuntimeError(f"source copy failed: {(temporary / 'copy.txt').read_text()}")
    logs = args.results.parent / "logs"
    code, _ = control.command(["ninja", "-C", args.build], logs / "baseline-build.txt")
    if code:
        raise RuntimeError(f"baseline did not build: {logs / 'baseline-build.txt'}")
    if index["commands"] != digest(args.commands.read_text()):
        raise RuntimeError("baseline build changed its preprocessing configuration; collect again")
    needed = {key for m in planned if not finished(state["results"].get(m["id"], {}), context) for key in m["tests"]}
    for key in sorted(needed, key=lambda k: (index["tests"][k]["seconds"], k)):
        word, _ = execute(args, index["tests"][key], args.build, logs / f"baseline-{key}.txt", control)
        if word != "passed":
            raise RuntimeError(f"unmutated baseline {key}: {word}; see {logs / ('baseline-' + key + '.txt')}")
    for mutant in planned:
        control.check()
        if finished(state["results"].get(mutant["id"], {}), context):
            continue
        path = copy / mutant["file"]
        if not path.resolve().is_relative_to(copy):
            raise RuntimeError(f"mutation target escapes the isolated copy: {path}")
        original = path.read_bytes().decode()
        record = dict(mutant, context=context, status="interrupted", runs=[])
        name = digest(mutant["id"])
        try:
            path.write_bytes(apply(original, mutant).encode())
            log = logs / f"{name}.build.txt"
            code, seconds = control.command(["ninja", "-C", args.build], log)
            record.update(build_seconds=seconds, build_log=str(log))
            if code:
                record["status"] = "not-built"
            else:
                record["status"] = "survived"
                for key in mutant["tests"]:
                    log = logs / f"{name}.{key}.txt"
                    word, seconds = execute(args, index["tests"][key], args.build, log, control)
                    record["runs"].append({"test": key, "verdict": word, "seconds": seconds, "log": str(log)})
                    if word == "failed":
                        record.update(status="killed", killed_by=key)
                        break
                    if word != "passed":
                        record["status"] = "inconclusive"
                        if word == "error":
                            raise RuntimeError(f"test runner failed for {key}: {log}")
            print(f"{mutant['id']}: {record['status']} {record.get('killed_by', '')}", flush=True)
        except Ended:
            record["status"] = "interrupted"
            raise
        finally:
            path.write_bytes(original.encode())
            state["results"][mutant["id"]] = record
            save(args.results, state)


def report(path):
    state = json.loads(path.read_text())
    plan = set(state.get("plan", state["results"]))
    records = [r for key, r in state["results"].items() if key in plan and r["context"] == state["context"]]
    scores = {str(p): {"killed": 0, "survived": 0, "not-built": 0, "inconclusive": 0, "interrupted": 0}
              for file in state["files"] for p in (Path(file), *Path(file).parents) if str(p) != "."}
    for record in records:
        file = Path(record["file"])
        for name in [str(file), *(str(p) for p in file.parents if str(p) != ".")]:
            scores.setdefault(name, {"killed": 0, "survived": 0, "not-built": 0, "inconclusive": 0, "interrupted": 0})
            scores[name][record["status"]] += 1
    complete = len(records) == len(plan) and all(r["status"] in ("killed", "survived", "not-built") for r in records)
    for name, counts in sorted(scores.items()):
        total = counts["killed"] + counts["survived"]
        score = f"{100 * counts['killed'] / total:.1f}%" if total else "unscored"
        print(f"{name}: {counts['killed']}/{total} killed ({score}); " +
              ", ".join(f"{n} {status}" for status, n in counts.items() if status not in ("killed", "survived")))
        if state.get("sample"):
            population = {f: n for f, n in state["population"].items() if name in (f, *map(str, Path(f).parents))}
            rows = [r for r in records if r["file"] in population]
            bounds = estimate(rows, population) if complete else None
            if bounds:
                point, low, high = bounds
                print(f"  population estimate {100 * point:.1f}%, conservative 95% sampling bound "
                      f"[{100 * low:.1f}%, {100 * high:.1f}%]")
            else:
                print("  sampling bound unavailable: incomplete sample, missing files, or no buildable mutants")
    groups = {}
    for r in records:
        if r["status"] == "survived":
            groups.setdefault((r["file"], r["function_line"], r["function"]), []).append(r)
    ranked = sorted(groups.items(), key=lambda item: (-max(len(r["tests"]) for r in item[1]), item[0]))
    for (file, line, name), mutants in ranked:
        print(f"\nsurvivors in {file}:{line} {name}:")
        for r in sorted(mutants, key=lambda r: (-len(r["tests"]), r["line"], r["column"], r["operator"])):
            print(f"  {file}:{r['line']}:{r['column']} {r['operator']}, {len(r['tests'])} tests ({r['coverage']})")
            print("    original: " + repr(r["original"]))
            print("    mutated:  " + repr(r["mutated"]))
    for f in state["unreached"]:
        print(f"unreached: {f['file']}:{f['line']} {f['name']} ({f['coverage']})")
    for r in records:
        if r["status"] in ("not-built", "inconclusive", "interrupted"):
            print(f"{r['status']}: {r['file']}:{r['line']}:{r['column']} {r['operator']} {r.get('build_log', '')}")
    for file in state["excluded"]:
        print(f"not in preprocessed build: {file}")
    completed = sum(r["status"] in ("killed", "survived", "not-built") for r in records)
    print(f"{completed} completed of {state['selected']} eligible mutations; {len(records)} recorded of "
          f"{len(plan)} scheduled; {state['candidates']} lexical candidates")
    if state.get("sample"):
        print(f"sample: requested {state['sample'][0]}, seed {state['sample'][1]}, evenly over files")
    for kind, costs in (("build", [r["build_seconds"] for r in records if "build_seconds" in r]),
                        ("test", [t["seconds"] for r in records for t in r["runs"]])):
        if costs:
            costs.sort()
            print(f"{kind}: {len(costs)} measurements, median {costs[len(costs) // 2]:.1f} s, total {sum(costs):.1f} s")


def main():
    defaults = argparse.ArgumentParser(add_help=False)
    test_run.options(defaults)
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("collect", "list", "run", "report"))
    parser.add_argument("--source", type=Path, default=ROOT)
    parser.add_argument("--lanes", type=Path)
    parser.add_argument("--build-root", type=Path)
    parser.add_argument("--compile-commands", dest="commands", type=Path)
    parser.add_argument("--include", action="append", type=Path, default=[],
                        help="extra read-only preprocessing headers; does not change the mutation build")
    parser.add_argument("--coverage", type=Path)
    parser.add_argument("--results", type=Path)
    parser.add_argument("--files", nargs="+", default=["src/*"])
    parser.add_argument("--sample", nargs=2, type=int, metavar=("COUNT", "SEED"),
                        help="draw at most COUNT mutants evenly over files, reproducibly from SEED")
    parser.add_argument("--budget", type=float, default=float("inf"),
                        help="wall seconds including preparation and builds")
    parser.add_argument("--jobs", type=int, choices=[1], default=1, help="GPU jobs: always one")
    parser.add_argument("--family", default=defaults.get_default("family"), help="coverage collection families")
    parser.add_argument("--validate", action="store_true")
    parser.add_argument("--wine", type=Path)
    parser.add_argument("--prefix", type=Path)
    parser.add_argument("--coverage-build", type=Path)
    parser.add_argument("--tests-build", type=Path)
    args = parser.parse_args()
    args.source = args.source.resolve()
    args.include = [p.resolve() for p in args.include]
    if not args.lanes:
        args.lanes = next((p for a in args.source.parents for p in (a, a / "lanes")
                           if (p / "t-gate.sh").is_file()), None)
    if not args.lanes:
        parser.error("cannot find lanes/t-gate.sh; set --lanes")
    args.lanes = args.lanes.resolve()
    base = (args.build_root or args.lanes.parent / "build").resolve()
    for name, default in {"build": base / "ta-mut", "commands": base / "ta-mut/compile_commands.json",
                          "coverage": base / "ta-mutations/coverage.json",
                          "results": base / "ta-mutations/results.json",
                          "wine": base / "ta-wine-break", "prefix": base / "ta-pfx-break",
                          "coverage_build": base / "ta-cov", "tests_build": base / "ta-tests"}.items():
        setattr(args, name, (getattr(args, name, None) or default).resolve())
    args.lock = Path(os.environ.get("MULLION_TEST_LOCK", str(args.lanes.parent / "crossover/tools/gpu.lock"))).resolve()
    if not args.budget > 0 or any(not p.startswith("src/") or ".." in Path(p).parts for p in args.files):
        parser.error("budget must be positive and globs must be under src/")
    if args.sample and args.sample[0] <= 0:
        parser.error("sample count must be positive")
    if args.command == "report":
        report(args.results)
        return
    control = Control(args.budget, args.lanes / "STOP")
    for sig in (signal.SIGTERM, signal.SIGINT):
        signal.signal(sig, lambda number, _: setattr(control, "reason", signal.Signals(number).name))
    with tempfile.TemporaryDirectory(prefix="mutate-") as tmp:
        temporary = Path(tmp)
        try:
            if args.command == "list":
                funcs, made, excluded = inventory(args, control, temporary)
                index = json.loads(args.coverage.read_text()) if args.coverage.exists() else None
                if index and index["source_hash"] != fingerprint(args.source):
                    raise RuntimeError("coverage is stale; collect again or use a different --coverage path")
                selected, unreached = route(funcs, made, index) if index and index.get("complete") else ([], [])
                planned = schedule(selected if index and index.get("complete") else made, args.sample)
                for directory in sorted({str(Path(m["file"]).parent) for m in made}):
                    count = sum(str(Path(m["file"]).parent) == directory for m in made)
                    print(f"{directory}: {count} candidates")
                for m in planned:
                    print(f"{m['file']}:{m['line']}:{m['column']} {m['function']} {m['operator']}: "
                          f"{m['old']!r} -> {m['new']!r}")
                for f in unreached:
                    print(f"unreached: {f['file']}:{f['line']} {f['name']}")
                for file in excluded:
                    print(f"not in preprocessed build: {file}")
                print(f"{len(made)} candidates in {len(funcs)} functions; " +
                      (f"{len(selected)} selected, {len(unreached)} unreached" if index and index.get("complete")
                       else "complete coverage not collected: selection unknown"))
                if args.sample:
                    print(f"{len(planned)} sampled, seed {args.sample[1]}")
            else:
                args.results.parent.mkdir(parents=True, exist_ok=True)
                with (base / "ta-mutation.lock").open("a") as lock:
                    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    if args.command == "collect":
                        collect(args, control, temporary)
                    else:
                        run_mutants(args, control, temporary)
        except Ended as stopped:
            print(f"stopped: {stopped}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, KeyError) as error:
        sys.exit(str(error))
