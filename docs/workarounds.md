# Workarounds for Apple's Metal stack

Mullion works around some faults in macOS's Metal driver and shader compiler. Each workaround has a name, applies
only where its fault was seen, and can be turned off to check whether a macOS release or GPU still needs it:

```
MULLION_DISABLE_WORKAROUNDS=memory_limit wine game.exe     # one, or several separated by commas
MULLION_DISABLE_WORKAROUNDS=all wine game.exe              # every one
```

The first time a workaround changes what happens, the log says so once with a warning; `MULLION_TRACE_API=heavy`
logs every time. The registry is [`mullion_workarounds.hpp`](../src/mullion/mullion_workarounds.hpp): a new workaround
adds its name there, its row below, and a test that fails without it.

| Name | Fault | Where | What Mullion does |
|---|---|---|---|
| `memory_limit` | Allocating more than `recommendedMaxWorkingSetSize` and making it resident panics the kernel ([KosmicKrisp's KK_WORKAROUND_20](https://docs.mesa3d.org/drivers/kosmickrisp/workarounds.html), fixed in macOS 27) | macOS 26 and earlier | Refuses an application's buffer, texture, committed resource or heap that would take the device's allocated memory past 95% of the working set, with `E_OUTOFMEMORY` ([`mullion_memory.hpp`](../src/mullion/mullion_memory.hpp)). The budget DXGI reports is that 95% on every macOS |

## In the shader converter

The converter's workarounds change the shaders it writes, so they are named in `MULLION_AIRCONV_SKIP` (comma
separated), which also names its other optional steps, and a shader cache made with one left out is a cache of other
shaders: clear it, or use `MULLION_SHADER_CACHE=0`, when comparing. [`tests/fuzz.py`](../tests/fuzz.py) found these,
and [`tools/metal-bench/integer_miscompiles.swift`](../tools/metal-bench/integer_miscompiles.swift) shows each in plain
Metal, so a macOS release can be checked for it without Mullion.

| `MULLION_AIRCONV_SKIP` | Fault | Where | What the converter does |
|---|---|---|---|
| `sign-bit-counts` | Metal's compiler counts the leading zeros and the ones of a value it can tell is all ones or zero (a sign-extended bool) as if it were 16 bits wide: `clz(c == 0 ? ~0u : 0u)` is 16, `popcount` of it 16 | M6, macOS 27.3 | Every 32-bit `clz` is a `ctz` of the reversed bits, and every 32-bit `popcount` a `popcount` of them, which Metal does not fold back ([`airconv_context.cpp`](../src/airconv/airconv_context.cpp)) |
| `constant-dividends` | Metal's compiler divides a constant by a variable wrongly for some divisors, how many depending on the program around it: `0xfffffffe / 3` gives `0x55555555`, and over a million divisors `0xfffffffe / y` was wrong for 12% in one kernel (a division of two variables is right) | M6, macOS 27.3 | A 32-bit constant dividend goes through `reverse_bits` twice, which Metal does not fold, so the division is of two values |

Not worked around: the same 16-bit view of a sign-extended bool also reaches `min` and `max` of two such values and a
third (`min(min(v1, v2), a)` is `0xffff`). Doing so would cost every integer `min` and `max` an operation, and the
values only come from comparisons a shader stores as integers; `tests/fuzz.py` still finds it in about one program in
a thousand.

The converter itself made DXBC's `udiv` with an LLVM division by a divisor that may be zero, which LLVM takes as a
promise it is not, and dropped the all-ones a division by zero gives where a `firstbithigh` read the remainder. That
was Mullion's fault, not Metal's: it divides by one where the divisor is zero, as its DXIL path did already.

## Faults other drivers report, not yet seen in Mullion

LunarG's KosmicKrisp (Vulkan on Metal 4) keeps a dated list of faults in
[its documentation](https://docs.mesa3d.org/drivers/kosmickrisp/workarounds.html). Mullion emits Metal IR (AIR)
rather than Metal Shading Language, so faults in the MSL front end do not reach it, but faults in the back-end
compiler and the hardware can. These have no Mullion workaround yet; each needs a test that shows whether it
reaches Mullion's output before one is added:

| KosmicKrisp | Fault | Where |
|---|---|---|
| KK_WORKAROUND_6 | Device memory written and read again by one thread is not coherent unless the pointer is marked coherent | macOS 26 (fixed in 27) |
| KK_WORKAROUND_11 | Several `MTL4Compiler` objects used at once corrupt the heap (FB22683138) | macOS 26 (fixed in 27 beta 5) |
| KK_WORKAROUND_13 | Metal 4's out-of-bounds guarantee for index buffers fails for 16-bit indices not 32-bit aligned (FB23291220) | macOS 26.5, 27 betas |
| KK_WORKAROUND_14 | `NaN * 0.0` folds to `0.0` under relaxed math | macOS 26.5, 27 betas |
| KK_WORKAROUND_16 | An out-of-range LOD clamps instead of reading zero | M5, macOS 27 |
| KK_WORKAROUND_18 | Stale reads after a barrier | macOS 26 (fixed in 27) |
| KK_WORKAROUND_19 | Spills to threadgroup memory read zero under `dispatchThreadsWithIndirectBuffer:` (FB25516856) | M1, M2 |

Apple's compiler aborting on a device-coherent texture access in Metal 4's AIR on the M5 Ultra
(`src/airconv/dxbc_converter.cpp`, the comment on globally coherent textures) is a fault of this kind that Mullion
already avoids.
