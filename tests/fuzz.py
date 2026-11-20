#!/usr/bin/env python3
"""metamorphic fuzzing of Mullion's shader converter: random integer programs, run on the GPU, against an exact
reference and against forms of themselves that must compute the same.

each program is a few expressions over three inputs a thread loads (uint arithmetic, bit operations, shifts,
divisions, comparisons and selects, and signed shifts and comparisons), written as an HLSL compute shader of 64
threads that the D3D11 harness (tests/d3d11/d3d11_shader_fuzz.cpp) compiles with Microsoft's compiler and runs
through Mullion. there is no WARP on a Mac to compare with, so the oracles are two:

- the reference: Python evaluates every expression by Direct3D's rules (D3D11.3 functional specification 22.13:
  shift amounts use their low five bits; an unsigned division or remainder by zero gives 0xffffffff; firstbithigh
  and firstbitlow of zero give 0xffffffff), so a word the GPU writes otherwise is a converter (or Metal) bug.
- the forms: every program is also written with its inputs passed through `^ zero` (a zero loaded from the buffer,
  which the compiler cannot fold), inside a function, and inside a loop that runs once (a count loaded from the
  buffer): what one computes the others must, and a form that alone differs points at the construct it adds.

inputs come from the buffer, so Microsoft's compiler folds nothing: the GPU computes every expression. a program
that fails is kept in --out with its inputs, the words expected and the words written, and the command that reruns
it. usage:

  tests/fuzz.py --build build --wine <Wine Contents, with the build's libraries installed (run.py --clone)>
                [--prefix PREFIX] [--seed N] [--programs N] [--batch N] [--out DIR]
"""
import argparse
import os
import random
import shutil
import subprocess
import sys
from pathlib import Path

M = 0xFFFFFFFF
THREADS = 64
ZERO = f"inputs[{THREADS * 3}]"  # a zero the compiler cannot see is one
OUTPUTS = 6  # expressions a program writes per thread
INTERESTING = [0, 1, 2, 3, 31, 32, 33, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF, 0x10000, 0x7FFFFFFF,
               0x80000000, 0x80000001, 0xFFFFFFFE, 0xFFFFFFFF, 0x55555555, 0xAAAAAAAA]


def signed(x):
    return x - (1 << 32) if x & 0x80000000 else x


def firstbithigh(x):
    return x.bit_length() - 1 if x else M


def firstbitlow(x):
    return (x & -x).bit_length() - 1 if x else M


def reversebits(x):
    return int(f"{x:032b}"[::-1], 2)


# each operation: its HLSL, written from its operands' HLSL, and its value from theirs
BINARY = {
    "add": (lambda a, b: f"({a} + {b})", lambda a, b: (a + b) & M),
    "sub": (lambda a, b: f"({a} - {b})", lambda a, b: (a - b) & M),
    "mul": (lambda a, b: f"({a} * {b})", lambda a, b: (a * b) & M),
    "and": (lambda a, b: f"({a} & {b})", lambda a, b: a & b),
    "or": (lambda a, b: f"({a} | {b})", lambda a, b: a | b),
    "xor": (lambda a, b: f"({a} ^ {b})", lambda a, b: a ^ b),
    "shl": (lambda a, b: f"({a} << {b})", lambda a, b: (a << (b & 31)) & M),
    "shr": (lambda a, b: f"({a} >> {b})", lambda a, b: a >> (b & 31)),
    "ashr": (lambda a, b: f"uint(asint({a}) >> {b})", lambda a, b: (signed(a) >> (b & 31)) & M),
    # a divisor xors the zero the buffer holds (ZERO): Microsoft's compiler takes x / x for 1, also where it only
    # finds the two are one value after simplifying (x << 0, c ? x : x), where Direct3D's division by zero gives all ones
    "div": (lambda a, b: f"({a} / ({b} ^ {ZERO}))", lambda a, b: a // b if b else M),
    "mod": (lambda a, b: f"({a} % ({b} ^ {ZERO}))", lambda a, b: a % b if b else M),
    "min": (lambda a, b: f"min({a}, {b})", min),
    "max": (lambda a, b: f"max({a}, {b})", max),
    "lt": (lambda a, b: f"uint({a} < {b})", lambda a, b: int(a < b)),
    "eq": (lambda a, b: f"uint({a} == {b})", lambda a, b: int(a == b)),
    "slt": (lambda a, b: f"uint(asint({a}) < asint({b}))", lambda a, b: int(signed(a) < signed(b))),
}
UNARY = {
    "not": (lambda a: f"(~{a})", lambda a: ~a & M),
    "neg": (lambda a: f"(0u - {a})", lambda a: -a & M),
    "countbits": (lambda a: f"countbits({a})", lambda a: bin(a).count("1")),
    "reversebits": (lambda a: f"reversebits({a})", reversebits),
    "firstbithigh": (lambda a: f"firstbithigh({a})", firstbithigh),
    "firstbitlow": (lambda a: f"firstbitlow({a})", firstbitlow),
}


def expression(rng, depth):
    """an expression: (its HLSL over a, b, c, its evaluator, whether it reads an input). Microsoft's compiler folds
    what only literals make by rules of its own (it drops a shift by a folded amount past 31, and refuses a division
    it folds to one by zero): so every operation reads an input, and a divisor is never a literal (and see ZERO).
    what is left is the GPU's to compute, as Windows would"""
    def variable():
        name = rng.choice("abc")
        return name, (lambda env, name=name: env[name]), True

    if depth == 0 or rng.random() < 0.2:
        if rng.random() < 0.75:
            return variable()
        # literals in int's range: Microsoft's compiler types a larger one otherwise; the inputs have those values
        value = rng.choice([v for v in INTERESTING if v < 0x80000000] + [rng.getrandbits(31)])
        return f"{value}u", (lambda env, value=value: value), False
    kind = rng.random()
    if kind < 0.15:
        op = rng.choice(list(UNARY))
        text, value, reads = expression(rng, depth - 1)
        if not reads:
            text, value, reads = variable()
        write, compute = UNARY[op]
        return write(text), (lambda env: compute(value(env))), True
    if kind < 0.25:
        parts = [expression(rng, depth - 1) for _ in range(4)]
        if not (parts[0][2] or parts[1][2]):
            parts[0] = variable()
        (c_text, c_value, _), (d_text, d_value, _), (x_text, x_value, _), (y_text, y_value, _) = parts
        return (f"(({c_text} < {d_text}) ? {x_text} : {y_text})",
                lambda env: x_value(env) if c_value(env) < d_value(env) else y_value(env), True)
    op = rng.choice(list(BINARY))
    a_text, a_value, a_reads = expression(rng, depth - 1)
    b_text, b_value, b_reads = expression(rng, depth - 1)
    if op in ("div", "mod") and not b_reads:
        b_text, b_value, b_reads = variable()
    # a literal shift past 31 it folds as it likes, and differently in a loop: a literal amount is within 0..31
    if op in ("shl", "shr", "ashr") and not b_reads:
        amount = b_value({}) & 31
        b_text, b_value = f"{amount}u", (lambda env, amount=amount: amount)
    if not (a_reads or b_reads):
        a_text, a_value, a_reads = variable()
    write, compute = BINARY[op]
    return write(a_text, b_text), (lambda env: compute(a_value(env), b_value(env))), True


def program(seed):
    rng = random.Random(seed)
    expressions = [expression(rng, rng.randint(2, 4))[:2] for _ in range(OUTPUTS)]
    inputs = []
    for _ in range(THREADS * 3):
        inputs.append(rng.choice(INTERESTING) if rng.random() < 0.6 else rng.getrandbits(rng.choice([4, 8, 16, 32])))
    inputs += [0, 1]  # zero and one, at THREADS * 3 and THREADS * 3 + 1
    expected = []
    for thread in range(THREADS):
        env = dict(zip("abc", inputs[thread * 3:thread * 3 + 3]))
        expected += [value(env) & M for _, value in expressions]
    return [text for text, _ in expressions], inputs, expected


FORMS = ("plain", "xor_zero", "function", "loop")


def hlsl(texts, form):
    head = f"// outputs: {THREADS * OUTPUTS}\nBuffer<uint> inputs : register(t0);\nRWStructuredBuffer<uint> outputs : register(u0);\n"
    body = "\n".join(f"  r[{i}] = {text};" for i, text in enumerate(texts))
    load = ("  uint a = inputs[t * 3], b = inputs[t * 3 + 1], c = inputs[t * 3 + 2];\n"
            f"  uint zero = inputs[{THREADS * 3}], one = inputs[{THREADS * 3 + 1}];\n")
    if form == "xor_zero":
        load += "  a ^= zero; b ^= zero; c ^= zero;\n"
    store = f"  for (uint k = 0; k < {OUTPUTS}; k++)\n    outputs[t * {OUTPUTS} + k] = r[k];\n"
    if form == "function":
        return (head + f"void f(uint a, uint b, uint c, out uint r[{OUTPUTS}]) {{\n{body}\n}}\n"
                f"[numthreads({THREADS}, 1, 1)]\nvoid cs(uint t : SV_GroupIndex) {{\n{load}  uint r[{OUTPUTS}];\n"
                f"  f(a, b, c, r);\n{store}}}\n")
    if form == "loop":
        body = "\n".join("  " + line for line in body.split("\n"))
        return (head + f"[numthreads({THREADS}, 1, 1)]\nvoid cs(uint t : SV_GroupIndex) {{\n{load}"
                f"  uint r[{OUTPUTS}] = (uint[{OUTPUTS}])0;\n  [loop] for (uint i = 0; i < one; i++) {{\n{body}\n  }}\n{store}}}\n")
    return (head + f"[numthreads({THREADS}, 1, 1)]\nvoid cs(uint t : SV_GroupIndex) {{\n{load}  uint r[{OUTPUTS}];\n"
            f"{body}\n{store}}}\n")


def windows(path):
    return "Z:" + str(path).replace("/", "\\")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--wine", type=Path, required=True, help="a Wine's Contents, with the build's libraries")
    parser.add_argument("--prefix", type=Path, help="default: tests-prefix in the build, as run.py's")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--programs", type=int, default=200)
    parser.add_argument("--batch", type=int, default=50, help="programs a Wine process runs")
    parser.add_argument("--out", type=Path, default=Path("fuzz-out"))
    args = parser.parse_args()
    harness = args.build / "tests" / "d3d11" / "d3d11_shader_fuzz.exe"
    if not harness.exists():
        sys.exit(f"{harness} is not built")
    prefix = (args.prefix or args.build / "tests-prefix").resolve()
    harness = harness.resolve()
    args.out = args.out.resolve()
    work = args.out / "work"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    env = dict(os.environ, WINEPREFIX=str(prefix), WINEDEBUG="-all", WINEDLLOVERRIDES="d3dcompiler_47=n,b",
               MULLION_SHADER_CACHE="0")
    failures = ran = skipped = 0
    seeds = list(range(args.seed, args.seed + args.programs))
    for start in range(0, len(seeds), args.batch):
        batch = seeds[start:start + args.batch]
        cases = {}
        for seed in batch:
            texts, inputs, expected = program(seed)
            for form in FORMS:
                path = work / f"{seed}-{form}.hlsl"
                path.write_text(hlsl(texts, form))
                Path(str(path) + ".in").write_text("".join(f"{word:08x}\n" for word in inputs))
                cases[windows(path)] = (seed, form, path, expected)
        listing = work / "list.txt"
        listing.write_text("".join(f"{name}\n" for name in cases))
        run = subprocess.run([args.wine / "bin" / "wine", harness, "@" + windows(listing)], env=env,
                             capture_output=True, text=True, timeout=1200)
        written = {}
        for line in run.stdout.splitlines():
            name, _, rest = line.partition(": ")
            written[name] = rest.split()
        # a harness that ran nothing failed to start: no program is wrong for it
        if not written:
            sys.exit(f"the harness ran nothing (exit {run.returncode}):\n{run.stderr[-2000:]}")
        for name, (seed, form, path, expected) in cases.items():
            ran += 1
            got = written.get(name)
            words = [int(w, 16) for w in got] if got and got[0] != "error" else None
            if words == expected:
                continue
            # Microsoft's compiler refuses a division it folds to one by zero: no shader, nothing to compare
            if got and "X4010:" in " ".join(got):
                skipped += 1
                continue
            failures += 1
            keep = args.out / f"{seed}-{form}"
            keep.mkdir(parents=True, exist_ok=True)
            shutil.copy(path, keep / "shader.hlsl")
            shutil.copy(str(path) + ".in", keep / "shader.hlsl.in")
            wrong = []
            if words is None:
                wrong.append(" ".join(got) if got else "no output (the harness ended early: see stderr)")
            else:
                for i, (e, w) in enumerate(zip(expected, words)):
                    if e != w:
                        wrong.append(f"thread {i // OUTPUTS} r[{i % OUTPUTS}]: expected {e:08x}, wrote {w:08x}")
            (keep / "wrong.txt").write_text("\n".join(wrong) + "\n")
            print(f"seed {seed} {form}: {len(wrong)} wrong; first: {wrong[0]} ({keep})", flush=True)
    print(f"{ran} shaders ({len(seeds)} programs in {len(FORMS)} forms), {failures} wrong, {skipped} the compiler refused")
    shutil.rmtree(work, ignore_errors=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
