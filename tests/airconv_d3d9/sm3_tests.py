import math, os, subprocess, sys

AIRCONV = os.environ['AIRCONV']
RENDERCHECK = os.environ['RENDERCHECK']
WORK_ROOT = os.environ.get('WORK', '/tmp/airconv_d3d9')
WINE = os.environ.get('WINE', 'wine')
HLSLC = os.environ.get('HLSLC', 'hlslc.exe')
WORK = f'{WORK_ROOT}/math'
os.makedirs(WORK, exist_ok=True)
ENV = dict(os.environ, WINEDEBUG='-all')

def c(i): return [i + 0.5, i + 0.25, -(i + 0.75), i * 0.5 + 1]
T = [0.25, 0.5, 0.75, 1.0]
def add(a, b): return [x + y for x, y in zip(a, b)]
def mul(a, b): return [x * y for x, y in zip(a, b)]
def sc(a, s): return [x * s for x in a]
def dot(a, b): return sum(x * y for x, y in zip(a, b))
def sat(a): return [min(max(x, 0.0), 1.0) for x in a]
def sign(x): return 1.0 if x > 0 else -1.0 if x < 0 else 0.0
def norm3(a): l = math.sqrt(dot(a[:3], a[:3])); return [x / l for x in a[:3]]

TESTS = [
    ('add', 'c[1] + c[2]', lambda: add(c(1), c(2))),
    ('sub', 'c[1] - c[2]', lambda: [x - y for x, y in zip(c(1), c(2))]),
    ('mad', 'c[1] * t + c[3]', lambda: add(mul(c(1), T), c(3))),
    ('lerp', 'lerp(c[1], c[2], t)', lambda: [a + (b - a) * w for a, b, w in zip(c(1), c(2), T)]),
    ('rcp', '1.0 / c[3]', lambda: [1 / x for x in c(3)]),
    ('rsqrt', 'rsqrt(abs(c[3]))', lambda: [1 / math.sqrt(abs(x)) for x in c(3)]),
    ('exp2', 'exp2(t * 3)', lambda: [2 ** (x * 3) for x in T]),
    ('log2', 'log2(c[2].xyww)', lambda: [math.log2(x) for x in [c(2)[0], c(2)[1], c(2)[3], c(2)[3]]]),
    ('pow', 'pow(abs(c[2]), t)', lambda: [abs(x) ** y for x, y in zip(c(2), T)]),
    ('frac', 'frac(c[5] * 1.37)', lambda: [x * 1.37 - math.floor(x * 1.37) for x in c(5)]),
    ('sign', 'sign(c[2] * float4(1, 0, 1, -1))', lambda: [sign(x) for x in mul(c(2), [1, 0, 1, -1])]),
    ('step', 'step(0.5, t)', lambda: [1.0 if x >= 0.5 else 0.0 for x in T]),
    ('select', 't.x < 0.3 ? c[1] : c[2]', lambda: c(1)),
    ('dp4', 'dot(c[1], c[2]).xxxx', lambda: [dot(c(1), c(2))] * 4),
    ('dp3', 'dot(c[1].xyz, c[2].xyz).xxxx', lambda: [dot(c(1)[:3], c(2)[:3])] * 4),
    ('cross', 'float4(cross(c[1].xyz, c[2].xyz), 1)',
     lambda: [c(1)[1] * c(2)[2] - c(1)[2] * c(2)[1], c(1)[2] * c(2)[0] - c(1)[0] * c(2)[2],
              c(1)[0] * c(2)[1] - c(1)[1] * c(2)[0], 1]),
    ('normalize', 'float4(normalize(c[3].xyz), 0.5)', lambda: norm3(c(3)) + [0.5]),
    ('minmax', 'min(c[1], t) + max(c[2], -t)', lambda: add([min(a, b) for a, b in zip(c(1), T)], [max(a, -b) for a, b in zip(c(2), T)])),
    ('sincos', 'float4(sin(t.x * 3), cos(t.y * 3), sin(t.z), cos(t.w))',
     lambda: [math.sin(0.75), math.cos(1.5), math.sin(0.75), math.cos(1.0)]),
    ('saturate', 'saturate(c[0] - 0.75)', lambda: sat([x - 0.75 for x in c(0)])),
    ('abs', 'abs(c[2])', lambda: [abs(x) for x in c(2)]),
    ('matrix', 'mul(t, float4x4(c[4], c[5], c[6], c[7]))',
     lambda: [sum(T[r] * c(4 + r)[col] for r in range(4)) for col in range(4)]),
    ('loop', 'L', lambda: [sum(c(k)[i] for k in range(4)) for i in range(4)]),
    ('tex2d', 'tex2D(s0, t.xy)', lambda: [0.9, 1.0, 1.1, 1.2]),
    ('lit', 'lit(t.x, t.y, 2.0)', lambda: [1.0, 0.25, 0.25, 1.0]),
]

LOOP_BODY = 'float4 s = 0; [loop] for (int k = 0; k < 4; k++) s += c[k]; return s;'
HEADER = 'float4 c[8] : register(c0);\nbool b[4] : register(b0);\nsampler2D s0 : register(s0);\n'

def body(expr):
    return LOOP_BODY if expr == 'L' else f'return {expr};'

def source(name, stage, model, expr):
    src = f'{WORK}/{name}_{stage}{model}.hlsl'
    with open(src, 'w') as f:
        f.write(HEADER)
        f.write('struct VSOut { float4 pos : POSITION; float4 t : TEXCOORD0; };\n')
        if stage == 'ps':
            f.write('VSOut vs_main(float4 pos : POSITION) { VSOut o; o.pos = pos; o.t = float4(0.25, 0.5, 0.75, 1.0); return o; }\n')
            f.write(f'float4 calc(float4 t) {{ {body(expr)} }}\n')
            f.write('float4 ps_main(float4 t : TEXCOORD0) : COLOR0 { return calc(t); }\n')
        else:
            f.write(f'float4 calc(float4 t) {{ {body(expr)} }}\n')
            f.write('VSOut vs_main(float4 pos : POSITION) { VSOut o; o.pos = pos; o.t = calc(float4(0.25, 0.5, 0.75, 1.0)); return o; }\n')
            f.write('float4 ps_main(float4 t : TEXCOORD0) : COLOR0 { return t; }\n')
    return src

def build(name, stage, model):
    out = {}
    for kind in ('vs', 'ps'):
        base = f'{WORK}/{name}_{stage}{model}_{kind}'
        if not os.path.exists(base + '.bin'):
            msg = open(base + '.bin.err').read().strip() if os.path.exists(base + '.bin.err') else ''
            return None, f'hlsl: {msg.splitlines()[0] if msg else "?"}'
        r = subprocess.run([AIRCONV, base + '.bin', '-A', '-o', base + '.metallib', '--reflection', base + '.refl'],
                           capture_output=True, text=True)
        if r.returncode:
            return None, 'airconv: ' + (r.stderr.strip().splitlines() or ['?'])[0]
        out[kind] = base
    return out, None

def variants():
    for name, expr, expected in TESTS:
        for stage in ('ps', 'vs'):
            if stage == 'vs' and name == 'tex2d':
                continue
            for model in ('2_0', '3_0'):
                yield name, expr, expected, stage, model

# Starting Wine costs seconds, so every missing shader compiles in one batch.
jobs = []
for name, expr, expected, stage, model in variants():
    src = source(name, stage, model, expr)
    for kind, entry in (('vs', 'vs_main'), ('ps', 'ps_main')):
        base = f'{WORK}/{name}_{stage}{model}_{kind}'
        if not os.path.exists(base + '.bin'):
            jobs.append(f'{src}\t{entry}\t{kind}_{model}\t{base}.bin\n')
if jobs:
    with open(f'{WORK}/jobs.txt', 'w') as f:
        f.writelines(jobs)
    subprocess.run([WINE, HLSLC, '-b', f'{WORK}/jobs.txt'], env=ENV, capture_output=True)

passed = failed = skipped = 0
for name, expr, expected in TESTS:
    for stage in ('ps', 'vs'):
        if stage == 'vs' and name == 'tex2d':
            continue
        for model in ('2_0', '3_0'):
            label = f'{name} {stage}_{model}'
            files, err = build(name, stage, model)
            if err and err.startswith('hlsl'):
                skipped += 1
                print(f'SKIP {label}: {err}')
                continue
            if err:
                failed += 1
                print(f'FAIL {label}: {err}')
                continue
            r = subprocess.run([RENDERCHECK, files['vs'] + '.metallib', files['vs'] + '.refl',
                                files['ps'] + '.metallib', files['ps'] + '.refl'], capture_output=True, text=True)
            try:
                got = [float(x) for x in r.stdout.split()]
            except ValueError:
                got = []
            want = expected()
            if len(got) == 4 and all(abs(g - w) <= 1e-3 * max(1, abs(w)) for g, w in zip(got, want)):
                passed += 1
            else:
                failed += 1
                print(f'FAIL {label}: got {got} want {[round(w, 5) for w in want]} {r.stderr.strip()[:200]}')
sys.stdout.flush()
print(f'{passed} passed, {failed} failed, {skipped} skipped')
