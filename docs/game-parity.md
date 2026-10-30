# Game-parity matrix

Template per title: executable/build, API, arch, Mullion commit + loaded DLL hashes, Wine/CrossOver, macOS/GPU/RAM, D3DMetal version, resolution/settings/upscaler, scene route, cold vs warm cache, visual/crash/hang, median/p95/p99, stutter, memory growth.

Parity target: same scene/settings/hardware as D3DMetal; report median/p95/p99 frame time ratio. Do not claim parity from a HUD feature level.

Optimization: #64 allocator-owned ICB pooling, #65 D3D12 parse intern, and D3D9 dirty constant tracking are in tree. #66 MetalFX config file and #70 category tracing stay env-gated and were not landed as a measured change. Nothing in this table is a D3DMetal frame-time ratio. #52 sincos range is unchanged.

## Seed titles

| Title | API | Arch | Status | Notes |
|---|---|---|---|---|
| Black Myth: Wukong | D3D12 | x64 | **not run** | README screenshots were shader-compile, not a warm-cache benchmark |
| God of War | D3D11 | x64 | **not run** | |
| God of War Ragnarök | D3D12 | x64 | **not run** | same as Wukong: compile-screen numbers are not a row |
| Red Dead Redemption 2 | D3D12 | x64 | **not run** | |
| Dragon's Dogma 2 | D3D12 | x64 | **not run** | |
| Marvel's Spider-Man Remastered | D3D12 | x64 | **not run** | |
| a UE5 title | D3D12 | x64 | **not run** | pick one actually installed |
| an RE Engine title | D3D12 | x64 | **not run** | Gears E-Day / RE9 only if installed |
| a D3D9 FF title | D3D9 | i386 or x64 | **not run** | enable_d3d9 build; log refusals via `MULLION_LOG_PATH` |
| a D3D9 SM2 title | D3D9 | i386 or x64 | **not run** | |
| a D3D9 SM3 title | D3D9 | i386 or x64 | **not run** | vs_3_0/ps_3_0 pixel tests exist (`d3d9_ff_extra`); caps report 3.0 |

Untested titles stay unclaimed. APIs D3DMetal does not cover use native Windows (or another established impl) as reference.

## Validation environment (this implementation pass)

- Host: Apple M4 Pro, macOS 26.5.2 (25F84), arm64
- Wine: `toolchains/wine` (`wine-8.17`)
- Toolchain: `toolchains/llvm-mingw-20260908-ucrt-macos-universal`
- Baseline commit: `96dbae09` on `local/shared-resources`; this pass is uncommitted on top of that (`AIRCONV_VERSION` 85)
- Builds: `build32` (win32, enable_d3d9) and `buildfresh` (win64, enable_d3d9/d3d12)
- PE32+ D3D9: `python3 tests/run.py --build buildfresh --wine toolchains/wine --family native,9 d3d9_` — 56 passed (native + family 9; `d3d9_states` re-run after FOGENABLE storage)
- PE32 D3D9: `python3 tests/run.py --build build32 --build buildfresh --wine toolchains/wine --family native,9 d3d9_` — 56 passed, 0 failed (i386 `d3dcompiler_47` in syswow64 so HLSL is Microsoft's compiler, not vkd3d-shader)
- Wave 4 checks on PE32+ native and family 9: `d3d11_border`, `d3d11_present_flags`, `d3d12_agility`, `d3d11_geometry_indirect_parts` passed; host `host_stream_range` and `host_typed_window` passed
- Games in the table above: **not run**
- M1/M2 hardware: **not run** (this host is M4 Pro; `supportsFamily` lowering is not a substitute)
- ARM64EC: **not run**
- D3DMetal frame-time ratios: **not run**
- Measured optimization: #64 ICB pool, #65 D3D12 parse intern, D3D9 dirty constants landed; `tests/perf` runner keeps every repeat/variance/failure. #66/#70 not landed. D3DMetal comparison **not run**.
