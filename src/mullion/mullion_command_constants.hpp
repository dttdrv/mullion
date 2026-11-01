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
