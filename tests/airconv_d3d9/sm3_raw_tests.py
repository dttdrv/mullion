import os, struct, subprocess, sys

AIRCONV = os.environ['AIRCONV']
RENDERCHECK = os.environ['RENDERCHECK']
WORK_ROOT = os.environ.get('WORK', '/tmp/airconv_d3d9')
WORK = f'{WORK_ROOT}/raw'
os.makedirs(WORK, exist_ok=True)
MATH = f'{WORK_ROOT}/math'

TEMP, INPUT, CONST, ADDR, OUTPUT, CONSTINT, COLOROUT, CONSTBOOL, LOOP, LABEL, PRED = 0, 1, 2, 3, 6, 7, 8, 14, 15, 18, 19
OP = dict(mov=1, add=2, mul=5, call=25, callnz=26, loop=27, ret=28, endloop=29, label=30, dcl=31, rep=38, endrep=39,
          if_=40, ifc=41, else_=42, endif=43, breakc=45, mova=46, defb=47, defi=48, texkill=65, def_=81, setp=94)
GT, GE = 1, 3

def rt(t): return ((t & 7) << 28) | ((t & 0x18) << 8)
def dst(t, n, mask=0xF): return 0x80000000 | rt(t) | n | (mask << 16)
def src(t, n, swz=0xE4, mod=0, rel=False): return 0x80000000 | rt(t) | n | (swz << 16) | (mod << 24) | ((1 << 13) if rel else 0)
def addr(t, comp=0): return 0x80000000 | rt(t) | ((comp * 0x55) << 16)
def ins(op, *ops, controls=0, pred=False): return [OP[op] | (len(ops) << 24) | (controls << 16) | ((1 << 28) if pred else 0), *ops]
def f(x): return struct.unpack('<I', struct.pack('<f', x))[0]
def dcl(usage, idx, d): return ins('dcl', 0x80000000 | usage | (idx << 16), d)
REP = lambda c: c * 0x55

def vs(body):
    return [0xFFFE0300, *dcl(0, 0, dst(INPUT, 0)), *dcl(0, 0, dst(OUTPUT, 0)), *dcl(5, 0, dst(OUTPUT, 1)),
            *body, *ins('mov', dst(OUTPUT, 0), src(INPUT, 0)), 0x0000FFFF]

def vs_with_tail(body, tail):
    return [0xFFFE0300, *dcl(0, 0, dst(INPUT, 0)), *dcl(0, 0, dst(OUTPUT, 0)), *dcl(5, 0, dst(OUTPUT, 1)),
            *body, *ins('mov', dst(OUTPUT, 0), src(INPUT, 0)), *ins('ret'), *tail, 0x0000FFFF]

def ps(body):
    return [0xFFFF0300, *body, 0x0000FFFF]

def c(i): return [i + 0.5, i + 0.25, -(i + 0.75), i * 0.5 + 1]
def add(*vs_): return [sum(v[i] for v in vs_) for i in range(4)]

TESTS = [
    ('loop_al', 'vs', vs([*ins('defi', dst(CONSTINT, 0), 3, 1, 1, 0), *ins('mov', dst(TEMP, 0), src(CONST, 0, mod=0)),
                          *ins('add', dst(TEMP, 0), src(TEMP, 0), src(TEMP, 0, mod=1)),
                          *ins('loop', src(LOOP, 0), src(CONSTINT, 0)),
                          *ins('add', dst(TEMP, 0), src(TEMP, 0), src(CONST, 0, rel=True), addr(LOOP)),
                          *ins('endloop'), *ins('mov', dst(OUTPUT, 1), src(TEMP, 0))]),
     lambda: add(c(1), c(2), c(3))),
    ('mova', 'vs', vs([*ins('def_', dst(CONST, 40), f(2.4), f(0), f(0), f(0)), *ins('mova', dst(ADDR, 0, 0x1), src(CONST, 40, REP(0))),
                       *ins('mov', dst(OUTPUT, 1), src(CONST, 1, rel=True), addr(ADDR, 0))]),
     lambda: c(3)),
    ('rep_int_register', 'vs', vs([*ins('mov', dst(TEMP, 0), src(CONST, 0)), *ins('add', dst(TEMP, 0), src(TEMP, 0), src(TEMP, 0, mod=1)),
                                   *ins('rep', src(CONSTINT, 1)), *ins('add', dst(TEMP, 0), src(TEMP, 0), src(CONST, 1)), *ins('endrep'),
                                   *ins('mov', dst(OUTPUT, 1), src(TEMP, 0))]),
     lambda: [4 * x for x in c(1)]),
    ('if_bool', 'vs', vs([*ins('mov', dst(TEMP, 0), src(CONST, 0)), *ins('if_', src(CONSTBOOL, 0, 0)),
                          *ins('add', dst(TEMP, 0), src(TEMP, 0), src(CONST, 1)), *ins('endif'),
                          *ins('if_', src(CONSTBOOL, 1, 0)), *ins('add', dst(TEMP, 0), src(TEMP, 0), src(CONST, 2)), *ins('else_'),
                          *ins('add', dst(TEMP, 0), src(TEMP, 0), src(CONST, 3)), *ins('endif'),
                          *ins('mov', dst(OUTPUT, 1), src(TEMP, 0))]),
     lambda: add(c(0), c(1), c(3))),
    ('defb', 'vs', vs([*ins('defb', dst(CONSTBOOL, 5), 1), *ins('mov', dst(TEMP, 0), src(CONST, 0)), *ins('if_', src(CONSTBOOL, 5, 0)),
                       *ins('mov', dst(TEMP, 0), src(CONST, 6)), *ins('endif'), *ins('mov', dst(OUTPUT, 1), src(TEMP, 0))]),
     lambda: c(6)),
    ('predicate', 'vs', vs([*ins('setp', dst(PRED, 0), src(CONST, 1), src(CONST, 2), controls=GT),
                            *ins('mov', dst(TEMP, 0), src(CONST, 0)), *ins('mov', dst(TEMP, 0), src(PRED, 0), src(CONST, 5), pred=True),
                            *ins('mov', dst(TEMP, 1), src(CONST, 7)), *ins('mov', dst(TEMP, 1), src(PRED, 0, REP(2)), src(CONST, 6), pred=True),
                            *ins('mov', dst(TEMP, 2), src(CONST, 8)), *ins('mov', dst(TEMP, 2), src(PRED, 0, REP(0)), src(CONST, 6), pred=True),
                            *ins('add', dst(TEMP, 0), src(TEMP, 0), src(TEMP, 1)), *ins('add', dst(OUTPUT, 1), src(TEMP, 0), src(TEMP, 2))]),
     lambda: add([c(0)[0], c(0)[1], c(5)[2], c(0)[3]], c(6), c(8))),
    ('call', 'vs', vs_with_tail([*ins('mov', dst(TEMP, 0), src(CONST, 0)), *ins('call', src(LABEL, 0)),
                                 *ins('callnz', src(LABEL, 1), src(CONSTBOOL, 1, 0)), *ins('callnz', src(LABEL, 1), src(CONSTBOOL, 0, 0)),
                                 *ins('mov', dst(OUTPUT, 1), src(TEMP, 0))],
                                [*ins('label', src(LABEL, 0)), *ins('add', dst(TEMP, 0), src(TEMP, 0), src(CONST, 4)), *ins('ret'),
                                 *ins('label', src(LABEL, 1)), *ins('add', dst(TEMP, 0), src(TEMP, 0), src(CONST, 9)), *ins('ret')]),
     lambda: add(c(0), c(4), c(9))),
    ('breakc', 'vs', vs([*ins('defi', dst(CONSTINT, 0), 10, 0, 1, 0), *ins('mov', dst(TEMP, 0), src(CONST, 0)),
                         *ins('rep', src(CONSTINT, 0)), *ins('add', dst(TEMP, 0), src(TEMP, 0), src(CONST, 1)),
                         *ins('breakc', src(TEMP, 0, REP(0)), src(CONST, 4, REP(0)), controls=GE), *ins('endrep'),
                         *ins('mov', dst(OUTPUT, 1), src(TEMP, 0))]),
     lambda: add(c(0), c(1), c(1), c(1))),
    ('texkill_keep', 'ps', ps([*ins('mov', dst(TEMP, 0), src(CONST, 1, mod=11)), *ins('texkill', dst(TEMP, 0)),
                               *ins('mov', dst(COLOROUT, 0), src(CONST, 3))]),
     lambda: c(3)),
    ('texkill_discard', 'ps', ps([*ins('mov', dst(TEMP, 0), src(CONST, 2)), *ins('texkill', dst(TEMP, 0, 0x7)),
                                  *ins('mov', dst(COLOROUT, 0), src(CONST, 3))]),
     lambda: [-7.0] * 4),
    ('texkill_masked', 'ps', ps([*ins('mov', dst(TEMP, 0), src(CONST, 2)), *ins('texkill', dst(TEMP, 0, 0x3)),
                                 *ins('mov', dst(COLOROUT, 0), src(CONST, 3))]),
     lambda: c(3)),
]

# ps_1_x: no instruction length field, result shift in the destination, implicit v#/t# inputs and r0 output
OP1 = dict(mov=1, add=2, sub=3, mul=5, texcoord=64, texkill=65, tex=66, texreg2gb=70, texm3x2pad=71, texm3x2tex=72,
           cnd=80, def_=81, bem=89)
BIAS, BX2, COMP = 2, 4, 6
def ins1(op, *ops): return [OP1[op], *ops]
def dst1(t, n, mask=0xF, shift=0, sat=False): return dst(t, n, mask) | ((shift & 0xF) << 24) | ((1 << 20) if sat else 0)
def ps1(minor, body): return [0xFFFF0100 | minor, *body, 0x0000FFFF]
PHASE = 0x0000FFFD
CLAMP = lambda v: [max(-1.0, min(1.0, x)) for x in v]
TC0 = [0.25, 0.5, 0.75, 1.0]
TEXEL = lambda x, y: [0.1 + 0.4 * (x + 2 * y) + 0.1 * k for k in range(4)]

TESTS += [
    ('ps11_tex', 'ps', ps1(1, [*ins1('tex', dst(ADDR, 0)), *ins1('mov', dst(TEMP, 0), src(ADDR, 0))]), lambda: TEXEL(0, 1)),
    ('ps11_texcoord', 'ps', ps1(1, [*ins1('texcoord', dst(ADDR, 0)), *ins1('mov', dst(TEMP, 0), src(ADDR, 0))]), lambda: TC0),
    ('ps11_const_clamp', 'ps', ps1(1, [*ins1('mov', dst(TEMP, 0), src(CONST, 1))]), lambda: CLAMP(c(1))),
    ('ps11_def_clamp', 'ps', ps1(1, [*ins1('def_', dst(CONST, 4), f(2), f(-3), f(0.5), f(1)), *ins1('mov', dst(TEMP, 0), src(CONST, 4))]),
     lambda: [1, -1, 0.5, 1]),
    ('ps11_shift_x2', 'ps', ps1(1, [*ins1('mul', dst1(TEMP, 0, shift=1), src(CONST, 0), src(CONST, 0))]),
     lambda: [2 * x * x for x in c(0)]),
    ('ps11_shift_d2_sat', 'ps', ps1(1, [*ins1('add', dst1(TEMP, 0, shift=-1 & 0xF, sat=True), src(CONST, 0), src(CONST, 0, mod=BX2))]),
     lambda: [max(0.0, min(1.0, (x + 2 * x - 1) / 2)) for x in c(0)]),
    ('ps11_bias_comp', 'ps', ps1(1, [*ins1('sub', dst(TEMP, 0), src(CONST, 0, mod=BIAS), src(CONST, 0, mod=COMP))]),
     lambda: [(x - 0.5) - (1 - x) for x in c(0)]),
    ('ps11_cnd', 'ps', ps1(1, [*ins1('mov', dst(TEMP, 0), src(CONST, 0)), *ins1('cnd', dst(TEMP, 0), src(TEMP, 0, REP(3)), src(CONST, 1), src(CONST, 2))]),
     lambda: CLAMP(c(1))),
    ('ps12_texreg2gb', 'ps', ps1(2, [*ins1('texcoord', dst(ADDR, 0)), *ins1('texreg2gb', dst(ADDR, 1), src(ADDR, 0)),
                                     *ins1('mov', dst(TEMP, 0), src(ADDR, 1))]), lambda: TEXEL(1, 1)),
    ('ps11_texm3x2', 'ps', ps1(1, [*ins1('texcoord', dst(ADDR, 0)), *ins1('texm3x2pad', dst(ADDR, 1), src(ADDR, 0)),
                                   *ins1('texm3x2tex', dst(ADDR, 2), src(ADDR, 0)), *ins1('mov', dst(TEMP, 0), src(ADDR, 2))]),
     lambda: TEXEL(0, 0)),
    ('ps11_texkill_keep', 'ps', ps1(1, [*ins1('texkill', dst(ADDR, 0)), *ins1('mov', dst(TEMP, 0), src(CONST, 0))]), lambda: c(0)),
    ('ps14_texld', 'ps', ps1(4, [*ins1('tex', dst(TEMP, 0), src(ADDR, 0))]), lambda: TEXEL(0, 1)),
    ('ps14_texcrd', 'ps', ps1(4, [*ins1('texcoord', dst(TEMP, 0, 0x7), src(ADDR, 0)), *ins1('mov', dst(TEMP, 0, 0x8), src(CONST, 0))]),
     lambda: TC0[:3] + [c(0)[3]]),
    ('ps14_phase_bem', 'ps', ps1(4, [*ins1('bem', dst(TEMP, 1, 0x3), src(CONST, 0), src(CONST, 1)), PHASE,
                                     *ins1('mov', dst(TEMP, 0), src(CONST, 2)), *ins1('mov', dst(TEMP, 0, 0x3), src(TEMP, 1))]),
     lambda: c(0)[:2] + CLAMP(c(2))[2:]),
]

passed = failed = 0
for name, stage, tokens, expected in TESTS:
    base = f'{WORK}/{name}'
    with open(base + '.bin', 'wb') as fh:
        fh.write(struct.pack(f'<{len(tokens)}I', *tokens))
    r = subprocess.run([AIRCONV, base + '.bin', '-A', '-o', base + '.metallib', '--reflection', base + '.refl'],
                       capture_output=True, text=True)
    if r.returncode:
        failed += 1
        print(f'FAIL {name}: airconv: {r.stderr.strip()}')
        continue
    other = f'{MATH}/add_vs3_0_ps' if stage == 'vs' else f'{MATH}/add_ps3_0_vs'
    v, p = (base, other) if stage == 'vs' else (other, base)
    out = subprocess.run([RENDERCHECK, v + '.metallib', v + '.refl', p + '.metallib', p + '.refl'],
                         capture_output=True, text=True)
    try:
        got = [float(x) for x in out.stdout.split()]
    except ValueError:
        got = []
    want = expected()
    if len(got) == 4 and all(abs(g - w) <= 1e-3 * max(1, abs(w)) for g, w in zip(got, want)):
        passed += 1
    else:
        failed += 1
        print(f'FAIL {name}: got {got} want {want} {out.stderr.strip()[:300]}')
print(f'{passed} passed, {failed} failed')
