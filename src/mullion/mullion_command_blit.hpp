/*
 * Copyright 2026 David Skoland
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

#include "mullion_command_constants.hpp"
#include "mullion_command_context.hpp"
#include <unordered_map>

namespace mullion {

// Scaled, format-converting color copy between 2D views, drawn as a full-viewport triangle.
template <typename Context> class BlitColor {
public:
  BlitColor(WMT::Device device, Context &ctx) : device_(device), ctx_(ctx) {
    auto library = ctx_.getDefaultLibrary();
    vs_ = library.newFunction("vs_present_quad");
    fs_nearest_ = library.newFunction("fs_blit_nearest");
    fs_linear_ = library.newFunction("fs_blit_linear");
    WMTDepthStencilInfo ds_info;
    ds_info.front_stencil.enabled = false;
    ds_info.back_stencil.enabled = false;
    ds_info.depth_compare_function = WMTCompareFunctionAlways;
    ds_info.depth_write_enabled = false;
    depth_state_ = device.newDepthStencilState(ds_info);
  }

  void
  blit(
      const Rc<Texture> &src, TextureViewKey src_view, const float (&src_rect)[4], const Rc<Texture> &dst,
      TextureViewKey dst_view, const WMTScissorRect &dst_rect, bool linear
  ) {
    auto format = dst->pixelFormat(dst_view);
    uint32_t samples = dst->sampleCount();
    uint64_t key = (uint64_t)format << 8 | samples << 1 | linear;
    auto it = psos_.find(key);
    if (it == psos_.end()) {
      WMTRenderPipelineInfo info;
      WMT::InitializeRenderPipelineInfo(info);
      info.vertex_function = vs_;
      info.fragment_function = linear ? fs_linear_ : fs_nearest_;
      info.colors[0].pixel_format = format;
      info.raster_sample_count = samples;
      info.rasterization_enabled = true;
      info.input_primitive_topology = WMTPrimitiveTopologyClassTriangle;
      WMT::Reference<WMT::Error> err;
      it = psos_.emplace(key, device_.newRenderPipelineState(info, err)).first;
    }
    if (!it->second)
      return;

    float width = src->width(src_view), height = src->height(src_view);
    ctx_.startRenderPass();
    ctx_.setColorAttachment(0, dst, dst_view, 0);
    ctx_.setRenderPSO(it->second);
    ctx_.setDepthStencilState(depth_state_);
    ctx_.setViewport({(double)dst_rect.x, (double)dst_rect.y, (double)dst_rect.width, (double)dst_rect.height, 0.0, 1.0});
    ctx_.setScissorRect(dst_rect);
    ctx_.setFragmentTexture(0, src, src_view);
    auto rect = (float *)ctx_.setFragmentBytes(kCustomBufferArgumentIndex0, sizeof(float) * 4);
    rect[0] = src_rect[0] / width;
    rect[1] = src_rect[1] / height;
    rect[2] = (src_rect[2] - src_rect[0]) / width;
    rect[3] = (src_rect[3] - src_rect[1]) / height;
    ctx_.draw(WMTPrimitiveTypeTriangle, 0, 3, 0, 1);
    ctx_.endPass();
  }

private:
  WMT::Device device_;
  SimpleCommandContext<Context> ctx_;
  WMT::Reference<WMT::Function> vs_;
  WMT::Reference<WMT::Function> fs_nearest_;
  WMT::Reference<WMT::Function> fs_linear_;
  WMT::Reference<WMT::DepthStencilState> depth_state_;
  std::unordered_map<uint64_t, WMT::Reference<WMT::RenderPipelineState>> psos_;
};

} // namespace mullion
