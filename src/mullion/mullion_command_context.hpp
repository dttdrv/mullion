/*
 * Copyright 2026 Feifan He for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#pragma once

#include "Metal.hpp"
#include <cstdlib>
#include <cstring>
#include "mullion_buffer.hpp"
#include "mullion_command_constants.hpp"
#include "mullion_texture.hpp"
#include "log/log.hpp"

namespace mullion {

template <typename Context> struct SimpleCommandContext {
  Context &ctx;

  WMT::Reference<WMT::ComputePipelineState> getComputePipeline(std::string name);
  void startComputePass();
  void endPass();
  void startBlitPass();
  void setComputeBuffer(uint32_t index, WMT::Buffer buffer, uint64_t offset);
  void setComputePSO(WMT::ComputePipelineState pso, WMTSize tgsize);
  void dispatch(WMTSize size);
  void setComputeTexture(uint32_t index, const Rc<Texture> &texture, uint64_t viewId, int flags);
  // binds the window of the view that holds texel `first`, which becomes that texel's index in it
  void setComputeTexelBuffer(uint32_t index, const Rc<Buffer> &buffer, uint64_t viewId, int flags, uint32_t &first);
  void setComputeBuffer(uint32_t index, const Rc<Buffer> &buffer, uint32_t offset, uint32_t length, int flags);
  void *setComputeBytes(uint32_t index, uint32_t length);

  void startRenderPass();
  /**
  All attachements (including depth-stencil) must have the same array length and sample count
  There must be no 'holes' (if index 2 is set, 0 and 1 must be also set)
  */
  void setColorAttachment(uint32_t index, const Rc<Texture> &texture, uint64_t viewId, uint32_t depth_plane);
  void setDepthStencilAttachment(const Rc<Texture> &texture, uint64_t viewId, uint32_t dsv_flag);
  void setRenderPSO(WMT::RenderPipelineState pso);
  void setViewport(WMTViewport viewport);
  void setDepthStencilState(WMT::DepthStencilState dsso);
  void setScissorRect(WMTScissorRect rect);
  void setStencilReference(uint8_t stencil_ref);
  void setFragmentTexture(uint32_t index, const Rc<Texture> &texture, uint64_t viewId);
  void *setFragmentBytes(uint32_t index, uint32_t length);
  void draw(
      WMTPrimitiveType primitive, uint32_t vertex_start, uint32_t vertex_count, int32_t base_instance,
      uint32_t instance_count
  );
};

template <typename Context> class RGB32Copy {
public:
  RGB32Copy(Context &ctx) : ctx_(ctx) {}

  void run(bool expand, WMT::Buffer src, uint64_t src_offset, WMT::Buffer dst, uint64_t dst_offset,
           RGB32CopyLayout layout, WMTSize size) {
    auto &pso = pipelines_[expand];
    if (!pso) {
      pso = ctx_.getComputePipeline(expand ? "cs_expand_rgb32" : "cs_pack_rgb32");
      if (!pso) {
        ERR("Failed to create RGB32 transfer pipeline");
        abort();
      }
    }
    ctx_.endPass();
    ctx_.startComputePass();
    ctx_.setComputePSO(pso, {pso.threadExecutionWidth(), 1, 1});
    ctx_.setComputeBuffer(0, src, src_offset);
    ctx_.setComputeBuffer(1, dst, dst_offset);
    memcpy(ctx_.setComputeBytes(2, sizeof(layout)), &layout, sizeof(layout));
    ctx_.dispatch(size);
    ctx_.endPass();
    ctx_.startBlitPass();
  }

private:
  SimpleCommandContext<Context> ctx_;
  WMT::Reference<WMT::ComputePipelineState> pipelines_[2];
};

} // namespace mullion