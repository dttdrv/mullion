# D3D9 architecture decision (bounded, 2026-11-08)

The plan asks for one decision between the current D3D9 implementation and the one in upstream PR #160
(`https://github.com/dttdrv/mullion/pull/160`, fetched here as `origin/pr-160`, head `57616e6522`, merge base
`2d1850f585`). This is the evidence available on this host after the execution pass; it is deliberately not a
wholesale replacement decision.

## What the two candidates are

| | Current branch (`src/d3d9`, this checkout) | PR #160 (`origin/pr-160`) |
| --- | --- | --- |
| Shape | D3D9 frontend over the existing D3D11 layer; fixed-function draws are precompiled HLSL variants (`d3d9_fixed_function_shaders.hpp`) | New D3D9 device over Mullion's command infrastructure, with a new airconv D3D9 shader frontend (`src/airconv/d3d9_converter.cpp`, `d3d9_decoder.cpp`, `nt/air_vertex_puller.cpp`) that emits SM1–3 bytecode |
| Size | 68 files changed, 16 748 insertions / 249 deletions against the merge base | includes 157 files, 23 060 insertions / 560 deletions against this branch's HEAD |
| Fixed-function texture typing | one `Texture2D` shader for every texture dimension; the cube/volume case is missing | `TextureType` per stage (`Texture2D`/`Cube`/`Volume`), `dcl_sampler` of that type, texcoord `z`/`w` defaulted to 0 |
| ProcessVertices | present in this branch | present in the new frontend (`d3d9_converter.cpp`, "ProcessVertices: the output registers go to a buffer") |
| Shared code | reuses D3D11 descriptors, PE32 marshaling and the Metal bridge already validated here | does not touch `src/d3d12` or `src/winemetal`; a D3D9-local rewrite |

## Evidence measured on this host (current branch)

- 37/37 local D3D9 tests on PE64 and PE32, validated and unvalidated, on the current artifact (identities
  `ede7ada443667c06`, `5eb56f8de4da9763`, `be6b5d21154f1637`, `1c711344ba0032d5`).
- Wine 8.17 D3D9 entries on the current PE64 artifact: visual 901 failures / 140 804 executed (was 920 before the two
  fixes below), device 70, stateblock 0, d3d9ex 53.
- Two conformance failures fixed in this pass, with local regressions: `SetAutoGenFilterType` on a non-autogen
  texture, and the depth-stencil binding across a mismatched render target.
- The one remaining `test_mipmap_autogen` cluster (3 checks) is the fixed-function cube sampler, which this branch
  does not have and PR #160 does.

PR #160 was **not built or run here**. Its own report documents untested areas and PE32 failures on a non-Apple virtual
GPU, so its test counts are not accepted as this host's evidence (the plan says not to replace on the author's counts).

## Decision

**Retain the current implementation and backport specific subsystems; do not adopt the alternative frontend now.**

1. **Backport the fixed-function texture typing first** (highest value, smallest reach): teach the generated
   fixed-function shaders the per-stage texture dimension (2D, cube, volume) and the missing-coordinate default
   (zero), as PR #160's `TextureType`/`dcl_sampler` path does. This is the remaining 3 `test_mipmap_autogen` checks and
   also covers volume-stage sampling. It touches only `src/d3d9/d3d9_fixed_function_shaders.hpp` and the draw path's
   shader selection.
2. **Evaluate ProcessVertices as a separate subsystem**: reproduce the Wine `test_process_vertices` cases against the
   current implementation; if the current path is incomplete, port the PR #160 converter approach for that operation
   only, with the existing local contract tests kept.
3. **Re-evaluate wholesale adoption only after a like-for-like run**: both candidates must run the same 37 local tests,
   the four Wine D3D9 entries, and PE32 GPU readback on this host. Until then the current branch's PE32 descriptor and
   bridge work stays where it is; a rewrite would discard that evidence.

## Compatibility map and cost

- The backport is D3D9-local: `src/d3d9` and the generated fixed-function shader header. It does not touch D3D10/11/12
  shared code, the PE32 descriptor work or the Metal bridge.
- The full PR #160 frontend would replace the D3D9 device and add a second shader frontend; its maintenance cost
  includes keeping the airconv D3D9 decoder/converter and the existing DXBC/DXIL paths in agreement.
- All existing distinguishing tests stay, whichever path is chosen.

## Next action

Implement the fixed-function cube/volume sampler variant and run the three `test_mipmap_autogen` checks (plus the
existing 37 local tests and the visual entry). Record the result before deciding whether ProcessVertices needs the
PR #160 converter.
