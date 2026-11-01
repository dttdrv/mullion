#pragma once

#include "Metal.hpp"
#include "../airconv/airconv_public.h"
#include "d3d12.h"
#include "mullion_buffer.hpp"
#include "mullion_command_constants.hpp"
#include "mullion_format.hpp"
#include "mullion_texture.hpp"
#include "rc/util_rc_ptr.hpp"
#include <array>
#include <bit>
#include <cfloat>
#include <cmath>
#include <span>
#include <unordered_map>

namespace mullion {

template <typename Emit>
void EncodePipelineStatisticsDraw(Emit emit, WMT::RenderPipelineState count, WMT::RenderPipelineState restore,
                                  WMT::Buffer arguments, uint64_t offset, WMT::Buffer result, uint64_t result_offset,
                                  WMT::RenderPipelineState scan = {}, uint32_t scanners = 0) {
  auto &resource = emit.template operator()<wmtcmd_render_useresource>();
  resource.type = WMTRenderCommandUseResource;
  resource.resource = result;
  resource.usage = WMTResourceUsageRead | WMTResourceUsageWrite;
  resource.stages = WMTRenderStageVertex | WMTRenderStageMesh | WMTRenderStageFragment;
  auto &pipeline = emit.template operator()<wmtcmd_render_setpso>();
  pipeline.type = WMTRenderCommandSetPSO;
  pipeline.pso = count;
  auto &input = emit.template operator()<wmtcmd_render_setbuffer>();
  input.type = WMTRenderCommandSetVertexBuffer;
  input.buffer = arguments;
  input.offset = offset;
  input.index = kCustomBufferArgumentIndex0;
  auto &draw = emit.template operator()<wmtcmd_render_draw>();
  draw.type = WMTRenderCommandDraw;
  draw.primitive_type = WMTPrimitiveTypePoint;
  draw.vertex_start = draw.base_instance = 0;
  draw.vertex_count = draw.instance_count = 1;
  auto &zeroed = emit.template operator()<wmtcmd_render_memory_barrier>();
  zeroed.type = WMTRenderCommandMemoryBarrier;
  zeroed.scope = WMTBarrierScopeBuffers;
  zeroed.stages_after = WMTRenderStageVertex;
  zeroed.stages_before = WMTRenderStageVertex | WMTRenderStageMesh | WMTRenderStageFragment;
  if (scan) {
    auto &strip_pipeline = emit.template operator()<wmtcmd_render_setpso>();
    strip_pipeline.type = WMTRenderCommandSetPSO;
    strip_pipeline.pso = scan;
    auto &strips = emit.template operator()<wmtcmd_render_draw>();
    strips.type = WMTRenderCommandDraw;
    strips.primitive_type = WMTPrimitiveTypePoint;
    strips.vertex_start = strips.base_instance = 0;
    strips.vertex_count = scanners;
    strips.instance_count = 1;
  }
  auto &pixel = emit.template operator()<wmtcmd_render_setbuffer>();
  pixel.type = WMTRenderCommandSetFragmentBuffer;
  pixel.buffer = result;
  pixel.offset = result_offset;
  pixel.index = SM50_BINDING_INDEX_PIPELINE_STATISTICS;
  auto &geometry = emit.template operator()<wmtcmd_render_setbuffer>();
  geometry.type = WMTRenderCommandSetMeshBuffer;
  geometry.buffer = result;
  geometry.offset = result_offset;
  geometry.index = SM50_BINDING_INDEX_PIPELINE_STATISTICS;
  auto &previous = emit.template operator()<wmtcmd_render_setpso>();
  previous.type = WMTRenderCommandSetPSO;
  previous.pso = restore;
}

// Metal clips a triangle that reaches 1.5 past its viewport's center, in normalized coordinates, into pieces, and on
// their seams runs a pixel shader once for each piece. D3D allows that (D3D11.3 15.4), but applications draw past the
// viewport as if it never happened, a full-screen triangle reaches 3. so Metal's viewports are made larger about their
// centers, and the stage before the rasterizer multiplies x and y by the inverse (SM50_BINDING_INDEX_RASTER_SCALE):
// positions land on the same pixels, and the cut moves out by the factor. a cut past that of the largest viewport, a
// whole target of `largest` pixels, loses the depth plane (#72; an M5 Pro keeps it exact up to the cut of its own
// largest, 32768 pixels). the factor is the largest power of two, which keeps the scale exact, that keeps every cut
// inside that one. returns the inverse
inline float
EnlargeViewports(std::span<WMTViewport> viewports, double largest) {
  constexpr double cut = 1.5;
  double reach = INFINITY;
  for (auto &v : viewports)
    for (auto [origin, size] : {std::pair{v.originX, v.width}, std::pair{v.originY, v.height}})
      if (size > 0)
        reach = std::min(reach, (largest - std::abs(2 * origin + size - largest) / cut) / size);
  double factor = std::isfinite(reach) && reach >= 1 ? std::exp2(std::floor(std::log2(reach))) : 1;
  for (auto &v : viewports) {
    v.originX -= (factor - 1) * v.width / 2, v.originY -= (factor - 1) * v.height / 2;
    v.width *= factor, v.height *= factor;
  }
  return float(1 / factor);
}

// what a line drawn as triangles keeps of the rasterizer state, which those of SM50_SHADER_PSO_GEOMETRY_SHADER_DATA's
// quad_lines are: a line is not culled, filled or biased, and faces front (D3D11.3 15.10, 15.13, 15.14)
template <typename State>
inline void
RasterizeAsLines(State &state) {
  state.fill_mode = WMTTriangleFillModeFill;
  state.cull_mode = WMTCullModeNone;
  state.depth_bias = state.scole_scale = state.depth_bias_clamp = 0;
  state.winding = WMTWindingCounterClockwise;
}

// the target the pixel shader blends itself (SM50_SHADER_BLEND), with Metal's blending off for it, where Metal's
// blender keeps other results than D3D's: a blended float target, whose NaNs and signed zeros D3D keeps through every
// weight (D3D11.3 17.5) and Metal's rewritten equation loses (it drops a Zero weight's product, folds a weight both
// sides share, and reverse subtracts complementary weights into -0), and a UINT target's logic op (17.7), which Metal
// offers only privately and saturates where D3D keeps the channel's bits. `desc` is a D3D11 or D3D12 render target blend description, whose
// members and values agree
template <typename Desc>
inline SM50_SHADER_BLEND
ShaderBlend(WMTColorAttachmentBlendInfo &color, const Desc &desc, bool logic) {
  // a format without alpha reads its destination alpha as 1 into the blender (D3D11.3 19.3.4), where Metal reads the
  // channel it stores the format with; SourceAlphaSaturated is min(As, 1 - Ad) for color and 1 for alpha
  if (GET_SWIZZLE_ALPHA(color.pixel_format) == WMTTextureSwizzleOne)
    for (auto [factor, saturated] : {std::pair{&color.src_rgb_blend_factor, WMTBlendFactorZero},
                                     {&color.dst_rgb_blend_factor, WMTBlendFactorZero},
                                     {&color.src_alpha_blend_factor, WMTBlendFactorOne},
                                     {&color.dst_alpha_blend_factor, WMTBlendFactorOne}})
      *factor = *factor == WMTBlendFactorDestinationAlpha             ? WMTBlendFactorOne
                : *factor == WMTBlendFactorOneMinusDestinationAlpha ? WMTBlendFactorZero
                : *factor == WMTBlendFactorSourceAlphaSaturated     ? saturated
                                                                    : *factor;
  SM50_SHADER_BLEND blend{};
  auto format = WMTPixelFormat(ORIGINAL_FORMAT(color.pixel_format));
  bool integer = IsIntegerFormat(format);
  if (logic && integer) {
    WMTClearColor high{FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX};
    SanitizeRTVClearColor(format, high);
    blend.logic = true;
    blend.logic_op = desc.LogicOp;
    for (auto [bits, value] : {std::pair{&blend.bits[0], high.r}, {&blend.bits[1], high.g}, {&blend.bits[2], high.b},
                               {&blend.bits[3], high.a}})
      *bits = std::bit_width(uint64_t(value));
  } else {
    MTL_DXGI_FORMAT_DESC description{};
    description.PixelFormat = format;
    if (!color.blending_enabled || integer || BorderColorRange(description)[1] <= 1)
      return blend;
    blend.blend = true;
    blend.src = desc.SrcBlend;
    blend.dst = desc.DestBlend;
    blend.op = desc.BlendOp;
    blend.src_alpha = desc.SrcBlendAlpha;
    blend.dst_alpha = desc.DestBlendAlpha;
    blend.op_alpha = desc.BlendOpAlpha;
  }
  color.blending_enabled = false;
  return blend;
}

struct TileBarrierPSOKey {
  WMTPixelFormat color_formats[8];
  unsigned raster_sample_count;
};
} // namespace mullion

namespace std {
template <> struct hash<mullion::TileBarrierPSOKey> {
  size_t
  operator()(const mullion::TileBarrierPSOKey &v) const noexcept {
    constexpr size_t binsize = sizeof(v);
    return std::hash<string_view>{}({reinterpret_cast<const char *>(&v), binsize});
  };
};

template <> struct equal_to<mullion::TileBarrierPSOKey> {
  bool
  operator()(const mullion::TileBarrierPSOKey &x, const mullion::TileBarrierPSOKey &y) const {
    constexpr size_t binsize = sizeof(x);
    return std::string_view({reinterpret_cast<const char *>(&x), binsize}) ==
           std::string_view({reinterpret_cast<const char *>(&y), binsize});
  }
};
}; // namespace std

namespace mullion {

class ArgumentEncodingContext;

class InternalCommandLibrary {
public:
  InternalCommandLibrary(WMT::Device device);

  WMT::Library
  getLibrary() {
    return library_;
  }
  WMT::RenderPipelineState PipelineStatistics(bool scan = false);
  // zeroes a stream output draw's scratch inside its render pass (zero_words)
  WMT::RenderPipelineState ZeroWords();
  // copies a one-pass stream output draw's filled sizes to its scratch inside its render pass, a slot a vertex
  // (stream_output_snapshot, SM50StreamOutputSinglePass)
  WMT::RenderPipelineState StreamOutputSnapshot();
  // the kernel `name` as a compute pipeline, made once for the device
  WMT::Reference<WMT::ComputePipelineState> ComputePipeline(const std::string &name);

  // a texture of `type` that a shader reads and writes as `sample`, bound in place of an unbound one: Metal leaves a
  // null texture's functions undefined. its metadata carries SM50_TEXTURE_METADATA_UNBOUND, and the shader drops what
  // it answers. `fresh` is set when it was made by this call, and `backing` is a texture buffer's buffer
  struct StandIn {
    WMT::Texture texture;
    uint64_t id;
    WMT::Buffer backing;
    bool fresh;
  };
  StandIn UnboundTexture(WMTTextureType type, SM50_SAMPLE_TYPE sample, bool write);

private:
  // a vertex function that runs without rasterizing, made when first used
  WMT::RenderPipelineState VertexPipeline(WMT::Reference<WMT::RenderPipelineState> &pipeline, const char *name);

  WMT::Reference<WMT::Library> library_;
  WMT::Device device_;
  mullion::mutex pipelines_mutex_;
  WMT::Reference<WMT::RenderPipelineState> pipeline_statistics_draw_[2], zero_words_, stream_output_snapshot_;
  std::unordered_map<std::string, WMT::Reference<WMT::ComputePipelineState>> compute_pipelines_;
  struct Unbound {
    WMT::Reference<WMT::Texture> texture;
    uint64_t id;
    WMT::Reference<WMT::Buffer> backing;
  };
  std::unordered_map<uint32_t, Unbound> unbound_textures_;
};

class EmulatedCommandContext {
  friend class CommandQueue;
public:
  EmulatedCommandContext(WMT::Device device, InternalCommandLibrary &lib, ArgumentEncodingContext &ctx);

  void
  ClearBufferUint(
      WMT::Buffer buffer, uint64_t byte_offset, uint64_t elements_uint, const std::array<uint32_t, 4> &value
  ) {
    setComputePipelineState(clear_buffer_uint_pipeline, {32, 1, 1});
    setComputeBuffer(buffer, byte_offset, 0);
    setComputeBytes(value.data(), 16, 1);
    setComputeBytes(&elements_uint, 4, 2);
    dispatchThreads({elements_uint, 1, 1});
  }

  void
  ClearTextureBufferUint(WMT::Texture texture, uint32_t offset, uint32_t size, const std::array<uint32_t, 4> &value) {
    setComputePipelineState(clear_texture_buffer_uint_pipeline, {32, 1, 1});
    setComputeTexture(texture, 0);
    struct CLEAR_UINT_DATA {
      const std::array<uint32_t, 4> value;
      uint32_t offset;
      uint32_t size;
      uint32_t padding[2];
    } data {value, offset, size};
    setComputeBytes(&data, sizeof(data), 1);
    dispatchThreads({size, 1, 1});
  }

  void
  ClearBufferFloat(
      WMT::Buffer buffer, uint64_t byte_offset, uint64_t elements_uint, const std::array<float, 4> &value
  ) {
    // just reinterpret float as uint
    setComputePipelineState(clear_buffer_uint_pipeline, {32, 1, 1});
    setComputeBuffer(buffer, byte_offset, 0);
    setComputeBytes(value.data(), 16, 1);
    setComputeBytes(&elements_uint, 4, 2);
    dispatchThreads({elements_uint, 1, 1});
  }

  void
  ClearTextureBufferFloat(WMT::Texture texture, uint32_t offset, uint32_t size, const std::array<float, 4> &value) {
    setComputePipelineState(clear_texture_buffer_float_pipeline, {32, 1, 1});
    struct CLEAR_FLOAT_DATA {
      const std::array<float, 4> value;
      uint32_t offset;
      uint32_t size;
      uint32_t padding[2];
    } data {value, offset, size};
    setComputeTexture(texture, 0);
    setComputeBytes(&data, sizeof(data), 1);
    dispatchThreads({size, 1, 1});
  }

  void
  ClearTexture2DUint(WMT::Texture texture, const std::array<uint32_t, 4> &value) {
    setComputePipelineState(clear_texture_2d_uint_pipeline, {8, 4, 1});
    setComputeTexture(texture, 0);
    setComputeBytes(value.data(), 16, 1);
    dispatchThreads({texture.width(), texture.height(), 1});
  }

  void
  ClearTexture2DFloat(WMT::Texture texture, const std::array<float, 4> &value) {
    setComputePipelineState(clear_texture_2d_float_pipeline, {8, 4, 1});
    setComputeTexture(texture, 0);
    setComputeBytes(value.data(), 16, 1);
    dispatchThreads({texture.width(), texture.height(), 1});
  }

  void
  ClearTexture2DArrayUint(WMT::Texture texture, const std::array<uint32_t, 4> &value) {
    setComputePipelineState(clear_texture_2d_array_uint_pipeline, {8, 4, 1});
    setComputeTexture(texture, 0);
    setComputeBytes(value.data(), 16, 1);
    dispatchThreads({texture.width(), texture.height(), texture.arrayLength()});
  }

  void
  ClearTexture2DArrayFloat(WMT::Texture texture, const std::array<float, 4> &value) {
    setComputePipelineState(clear_texture_2d_array_float_pipeline, {8, 4, 1});
    setComputeTexture(texture, 0);
    setComputeBytes(value.data(), 16, 1);
    dispatchThreads({texture.width(), texture.height(), texture.arrayLength()});
  }

  void
  ClearTexture3DFloat(
      WMT::Texture texture, const std::array<float, 4> &value, uint32_t firstSlice, uint32_t sliceCount
  ) {
    setComputePipelineState(clear_texture_3d_float_pipeline, {8, 4, 1});
    setComputeTexture(texture, 0);
    setComputeBytes(value.data(), 16, 1);
    setComputeBytes(&firstSlice, sizeof(firstSlice), 2);
    dispatchThreads({texture.width(), texture.height(), sliceCount});
  }

  void
  ClearTexture3DUint(
      WMT::Texture texture, const std::array<uint32_t, 4> &value, uint32_t firstSlice, uint32_t sliceCount
  ) {
    setComputePipelineState(clear_texture_3d_uint_pipeline, {8, 4, 1});
    setComputeTexture(texture, 0);
    setComputeBytes(value.data(), 16, 1);
    setComputeBytes(&firstSlice, sizeof(firstSlice), 2);
    dispatchThreads({texture.width(), texture.height(), sliceCount});
  }

  void
  MarshalGSDispatchArguments(WMT::RenderCommandEncoder encoder, WMT::Buffer commands, uint32_t commands_offset) {
    encoder.setRenderPipelineState(gs_draw_arguments_marshal);
    encoder.setVertexBuffer(commands, commands_offset, kCustomBufferArgumentIndex0);
    encoder.drawPrimitives(WMTPrimitiveTypePoint, 0, 1);
    encoder.setVertexBuffer({}, 0, kCustomBufferArgumentIndex0);
  }

  void
  MarshalDrawAutoArguments(WMT::RenderCommandEncoder encoder, WMT::Buffer commands, uint32_t commands_offset) {
    encoder.setRenderPipelineState(draw_auto_arguments_marshal);
    encoder.setVertexBuffer(commands, commands_offset, kCustomBufferArgumentIndex0);
    encoder.drawPrimitives(WMTPrimitiveTypePoint, 0, 1);
    encoder.setVertexBuffer({}, 0, kCustomBufferArgumentIndex0);
  }

  void
  MarshalTSDispatchArguments(WMT::RenderCommandEncoder encoder, WMT::Buffer commands, uint32_t commands_offset) {
    encoder.setRenderPipelineState(ts_draw_arguments_marshal);
    encoder.setVertexBuffer(commands, commands_offset, kCustomBufferArgumentIndex0);
    encoder.drawPrimitives(WMTPrimitiveTypePoint, 0, 1);
    encoder.setVertexBuffer({}, 0, kCustomBufferArgumentIndex0);
  }

  uint32_t SIMDWidth() const { return clear_buffer_uint_pipeline.threadExecutionWidth(); }

private:
  void setComputePipelineState(WMT::ComputePipelineState state, const WMTSize &threadgroup_size);

  void dispatchThreads(const WMTSize &grid_size);

  void setComputeBuffer(WMT::Buffer buffer, uint64_t offset, uint8_t index);

  void setComputeTexture(WMT::Texture texture, uint8_t index);

  void setComputeBytes(const void *buf, uint64_t length, uint8_t index);

  ArgumentEncodingContext &ctx;
  WMT::Reference<WMT::ComputePipelineState> clear_texture_1d_uint_pipeline;
  WMT::Reference<WMT::ComputePipelineState> clear_texture_1d_array_uint_pipeline;
  WMT::Reference<WMT::ComputePipelineState> clear_texture_2d_uint_pipeline;
  WMT::Reference<WMT::ComputePipelineState> clear_texture_2d_array_uint_pipeline;
  WMT::Reference<WMT::ComputePipelineState> clear_texture_3d_uint_pipeline;
  WMT::Reference<WMT::ComputePipelineState> clear_texture_buffer_uint_pipeline;
  WMT::Reference<WMT::ComputePipelineState> clear_buffer_uint_pipeline;
  WMT::Reference<WMT::ComputePipelineState> clear_texture_1d_float_pipeline;
  WMT::Reference<WMT::ComputePipelineState> clear_texture_1d_array_float_pipeline;
  WMT::Reference<WMT::ComputePipelineState> clear_texture_2d_float_pipeline;
  WMT::Reference<WMT::ComputePipelineState> clear_texture_2d_array_float_pipeline;
  WMT::Reference<WMT::ComputePipelineState> clear_texture_3d_float_pipeline;
  WMT::Reference<WMT::ComputePipelineState> clear_texture_buffer_float_pipeline;

  WMT::Reference<WMT::RenderPipelineState> gs_draw_arguments_marshal;
  WMT::Reference<WMT::RenderPipelineState> ts_draw_arguments_marshal;
  WMT::Reference<WMT::RenderPipelineState> draw_auto_arguments_marshal;
};

class DepthStencilBlitContext {

public:
  DepthStencilBlitContext(WMT::Device device, InternalCommandLibrary &lib, ArgumentEncodingContext &ctx);

  void copyFromBuffer(
      const Rc<Buffer> &src, uint64_t src_offset, uint64_t src_length, uint32_t bytes_per_row, uint32_t bytes_per_image,
      const Rc<Texture> &depth_stencil, uint32_t level, uint32_t slice, bool from_d24s8
  );

  void copyFromTexture(
      const Rc<Texture> &depth_stencil, uint32_t level, uint32_t slice, const Rc<Buffer> &dst, uint64_t dst_offset,
      uint64_t dst_length, uint32_t bytes_per_row, uint32_t bytes_per_image, bool to_d24s8
  );

private:
  ArgumentEncodingContext &ctx_;
  WMT::Device device_;
  WMT::Reference<WMT::DepthStencilState> depth_stencil_state_;
  WMT::Reference<WMT::RenderPipelineState> pso_copy_d24s8_;
  WMT::Reference<WMT::RenderPipelineState> pso_copy_d32s8_;
  WMT::Reference<WMT::ComputePipelineState> pso_copy_to_buffer_d24s8_;
  WMT::Reference<WMT::ComputePipelineState> pso_copy_to_buffer_d32s8_;
};

class MTLFXMVScaleContext {
public:
  MTLFXMVScaleContext(WMT::Device device, InternalCommandLibrary &lib, ArgumentEncodingContext &ctx);

  void dispatch(
      const Rc<Texture> &dilated, TextureViewKey view_dilated, const Rc<Texture> &downscaled,
      TextureViewKey view_downscaled, float mv_scale_x, float mv_scale_y
  );

private:
  ArgumentEncodingContext &ctx_;
  WMT::Device device_;
  WMT::Reference<WMT::ComputePipelineState> pso_downscale_dilated_mv_;
};

constexpr auto kBarrierTileSize = 16;

class TileBarrierContext {
public:
  TileBarrierContext(WMT::Device device, InternalCommandLibrary &lib, ArgumentEncodingContext &ctx);

  void dispatch();

private:
  WMT::RenderPipelineState getPSO(TileBarrierPSOKey &format);

  ArgumentEncodingContext &ctx_;
  WMT::Device device_;
  WMT::Reference<WMT::Function> tile_function_;
  std::unordered_map<TileBarrierPSOKey, WMT::Reference<WMT::RenderPipelineState>> psos_;
};

} // namespace mullion