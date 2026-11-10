#pragma once

#include "../airconv/airconv_sparse.h"

#ifdef __METAL__
#define CONSTANT constant
#else
#define CONSTANT
#endif

namespace mullion {

constexpr CONSTANT auto kCustomBufferArgumentIndex0 = 30;
constexpr CONSTANT auto kCustomBufferArgumentIndex1 = 29;
constexpr CONSTANT auto kSparseBufferArgumentIndex = kCustomBufferArgumentIndex1 - 1;

// the compute tessellator's work for a render pass (tessellation_count, tessellation_scan, tessellation_emit), in the
// GPU heap. a draw's patches are counted over its instances, from the pass's `first`; its factors are words in the heap,
// each patch's 4 (a triangle's) or 6 (a quad's); `kind` is its domain (1: quad), partitioning << 1 and counter-clockwise
// << 3; `arguments` its indexed-indirect draw arguments (5 words); where the pass's arena has no room for it, the mesh
// draw that takes it instead counts its patches from word `fallback` (0: it has none), which is otherwise 0, and is
// word `source`, the draw's own, where it is drawn
struct TessellationDraw {
  uint32_t first, patches, factors, kind, arguments, fallback, source;
};

// a pass's draws (`draws`, words in the heap, `draw_count` of them), its patches' counts (3 words a patch: frames,
// points, triangles, which the scan makes exclusive sums of, then the totals), its arena (`points` and `indices`, words
// in the heap, room for `point_room` points and `index_room` indices; the points a multiple of 3 words, as indices
// number points from the heap's start), and what the scan writes: the emit dispatch's threadgroups (3 words), what the
// pass needed, for the arena's next size, and the frames of the patches that fit, which the emit makes
struct TessellationPass {
  uint32_t draws, draw_count, patches, counts, points, indices, point_room, index_room;
  uint32_t emit_groups[3], need_points, need_indices, frames;
};

constexpr CONSTANT uint32_t kTessellationThreads = 64;

// D3D11.3 19.1.3.1 defines D24_UNORM_S8_UINT with 24 normalized depth bits.
constexpr CONSTANT auto kD24UnormMax = (1u << 24) - 1;

struct RGB32CopyLayout {
  sm50_sparse_qword src_row, src_image, dst_row, dst_image;
};

struct SparseTexelClear {
  sm50_sparse_qword byte_offset;
  sm50_sparse_qword texel_size;
};

// the draw's pixel invocations and strip primitive correction, captured before its index buffer can change
struct PipelineStatisticsDraw {
  sm50_sparse_qword indices, index_bytes, result, skips, arguments;
  sm50_sparse_qword primitives;
  sm50_sparse_qword vertices, instances, start, index_stride;
  sm50_sparse_qword first, step, cut, threads;
};

// the counts the shaders add to are at their SM50_PIPELINE_STATISTIC words
struct PipelineStatisticsResult {
  sm50_sparse_qword pixels, primitives, vertices;
  sm50_sparse_qword gs_invocations, gs_primitives, rasterized, gs_inputs;
};

struct PipelineStatisticsDispatch {
  sm50_sparse_qword invocations, threads;
};

constexpr CONSTANT auto kPresentFCIndex_BackbufferSizeMatched = 0x100;
constexpr CONSTANT auto kPresentFCIndex_BackbufferIsSRGB = 0x103;
constexpr CONSTANT auto kPresentFCIndex_HDRPQ = 0x101;
constexpr CONSTANT auto kPresentFCIndex_WithHDRMetadata = 0x102;
constexpr CONSTANT auto kPresentFCIndex_BackbufferIsMS = 0x104;
constexpr CONSTANT auto kPresentFCIndex_GammaEnabled = 0x105;
constexpr CONSTANT auto kPresentFCIndex_BackbufferIsXRBias = kPresentFCIndex_GammaEnabled + 1;

// a ray tracing pipeline's runtime calls its shaders through the dispatchers the pipeline links
// (mullion_command.metal), not through its function table; and whether the pipeline has any hit shaders, and
// intersection shaders (true where not given)
constexpr CONSTANT auto kRayFCIndex_DirectCalls = 0x200;
constexpr CONSTANT auto kRayFCIndex_AnyHitShaders = 0x201;
constexpr CONSTANT auto kRayFCIndex_IntersectionShaders = 0x202;

} // namespace mullion
