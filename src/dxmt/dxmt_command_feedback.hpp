#pragma once

#include "dxmt_command_context.hpp"

namespace dxmt {

// moves sampler feedback between a map and the R8 textures or buffers D3D12 transcodes it with. a map has one texel
// per mip region of its paired texture's first mip, holding a bit for each mip wanted: bit n in the first region of
// each 2^n by 2^n block of regions, which is that mip's region. the kernels are in dxmt_command.metal
template <typename Context> class TranscodeFeedback {
public:
  // DXMTFeedbackMetadata
  // DXMTFeedbackMetadata, whose pairs give it their alignment
  struct alignas(2 * sizeof(uint32_t)) Metadata {
    uint32_t map_offset[2]; // of the rectangle, in regions of mip `level`
    uint32_t offset[2];     // of the rectangle in the texture, or in a buffer's rows
    uint32_t level;
    uint32_t mips;    // a decode looks at this many mips from `level` up
    uint32_t min_mip; // the map's kind: a mip number per region, or one mip's regions as 0 and 0xff
    uint32_t pitch;   // of a buffer's rows
    uint32_t clear;   // an encode forgets before it records
  };

  TranscodeFeedback(Context &ctx) : ctx_(ctx) {
    const char *names[2][2] = {
        {"cs_feedback_decode", "cs_feedback_decode_buffer"}, {"cs_feedback_encode", "cs_feedback_encode_buffer"}
    };
    for (auto encode : {0, 1})
      for (auto buffer : {0, 1})
        pipelines_[encode][buffer] = ctx_.getComputePipeline(names[encode][buffer]);
  }

  // `size` regions of each array slice of the views, to or from `texture`, or `buffer` without one
  void
  run(bool encode, const Rc<Texture> &map, TextureViewKey map_view, const Rc<Texture> &texture, TextureViewKey view,
      const Rc<Buffer> &buffer, Metadata meta, WMTSize size) {
    ctx_.startComputePass();
    ctx_.setComputePSO(pipelines_[encode][!texture], {32, 1, 1});
    ctx_.setComputeTexture(0, map, map_view, ResourceAccess::ReadWrite);
    auto access = encode ? ResourceAccess::Read : ResourceAccess::Write;
    if (texture)
      ctx_.setComputeTexture(1, texture, view, access);
    else
      ctx_.setComputeBuffer(1, buffer, 0, buffer->length(), access);
    meta.clear = encode;
    do {
      memcpy(ctx_.setComputeBytes(2, sizeof(meta)), &meta, sizeof(meta));
      ctx_.dispatch(size);
    } while (meta.clear--);
    ctx_.endPass();
  }

private:
  SimpleCommandContext<Context> ctx_;
  WMT::Reference<WMT::ComputePipelineState> pipelines_[2][2];
};

} // namespace dxmt
