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
