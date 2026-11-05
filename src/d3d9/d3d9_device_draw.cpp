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

#include "d3d9_device.hpp"
#include "d3d9_fixed_function.hpp"
#include "d3d9_state_block.hpp"
#include "d3d9_format.hpp"
#include "d3d9_texture.hpp"
#include "log/log.hpp"
#include <algorithm>
#include <bit>
#include <cstring>

namespace mullion {

namespace {

WMTBlendFactor
ToBlendFactor(DWORD blend) {
  switch (blend) {
  case D3DBLEND_ZERO:
    return WMTBlendFactorZero;
  case D3DBLEND_ONE:
    return WMTBlendFactorOne;
  case D3DBLEND_SRCCOLOR:
    return WMTBlendFactorSourceColor;
  case D3DBLEND_INVSRCCOLOR:
    return WMTBlendFactorOneMinusSourceColor;
  case D3DBLEND_SRCALPHA:
    return WMTBlendFactorSourceAlpha;
  case D3DBLEND_INVSRCALPHA:
    return WMTBlendFactorOneMinusSourceAlpha;
  case D3DBLEND_DESTALPHA:
    return WMTBlendFactorDestinationAlpha;
  case D3DBLEND_INVDESTALPHA:
    return WMTBlendFactorOneMinusDestinationAlpha;
  case D3DBLEND_DESTCOLOR:
    return WMTBlendFactorDestinationColor;
  case D3DBLEND_INVDESTCOLOR:
    return WMTBlendFactorOneMinusDestinationColor;
  case D3DBLEND_SRCALPHASAT:
    return WMTBlendFactorSourceAlphaSaturated;
  case D3DBLEND_BLENDFACTOR:
    return WMTBlendFactorBlendColor;
  case D3DBLEND_INVBLENDFACTOR:
    return WMTBlendFactorOneMinusBlendColor;
  case D3DBLEND_SRCCOLOR2:
    return WMTBlendFactorSource1Color;
  case D3DBLEND_INVSRCCOLOR2:
    return WMTBlendFactorOneMinusSource1Color;
  default:
    return WMTBlendFactorOne;
  }
}

// BOTHSRCALPHA and BOTHINVSRCALPHA set both factors from the source blend.
void
ResolveBlendPair(DWORD src, DWORD dst, WMTBlendFactor &out_src, WMTBlendFactor &out_dst) {
  if (src == D3DBLEND_BOTHSRCALPHA) {
    out_src = WMTBlendFactorSourceAlpha;
    out_dst = WMTBlendFactorOneMinusSourceAlpha;
  } else if (src == D3DBLEND_BOTHINVSRCALPHA) {
    out_src = WMTBlendFactorOneMinusSourceAlpha;
    out_dst = WMTBlendFactorSourceAlpha;
  } else {
    out_src = ToBlendFactor(src);
    out_dst = ToBlendFactor(dst);
  }
}

WMTBlendOperation
ToBlendOp(DWORD op) {
  switch (op) {
  case D3DBLENDOP_SUBTRACT:
    return WMTBlendOperationSubtract;
  case D3DBLENDOP_REVSUBTRACT:
    return WMTBlendOperationReverseSubtract;
  case D3DBLENDOP_MIN:
    return WMTBlendOperationMin;
  case D3DBLENDOP_MAX:
    return WMTBlendOperationMax;
  default:
    return WMTBlendOperationAdd;
  }
}

WMTCompareFunction
ToCompare(DWORD func) {
  switch (func) {
  case D3DCMP_NEVER:
    return WMTCompareFunctionNever;
  case D3DCMP_LESS:
    return WMTCompareFunctionLess;
  case D3DCMP_EQUAL:
    return WMTCompareFunctionEqual;
  case D3DCMP_LESSEQUAL:
    return WMTCompareFunctionLessEqual;
  case D3DCMP_GREATER:
    return WMTCompareFunctionGreater;
  case D3DCMP_NOTEQUAL:
    return WMTCompareFunctionNotEqual;
  case D3DCMP_GREATEREQUAL:
    return WMTCompareFunctionGreaterEqual;
  default:
    return WMTCompareFunctionAlways;
  }
}

WMTStencilOperation
ToStencilOp(DWORD op) {
  switch (op) {
  case D3DSTENCILOP_ZERO:
    return WMTStencilOperationZero;
  case D3DSTENCILOP_REPLACE:
    return WMTStencilOperationReplace;
  case D3DSTENCILOP_INCRSAT:
    return WMTStencilOperationIncrementClamp;
  case D3DSTENCILOP_DECRSAT:
    return WMTStencilOperationDecrementClamp;
  case D3DSTENCILOP_INVERT:
    return WMTStencilOperationInvert;
  case D3DSTENCILOP_INCR:
    return WMTStencilOperationIncrementWrap;
  case D3DSTENCILOP_DECR:
    return WMTStencilOperationDecrementWrap;
  default:
    return WMTStencilOperationKeep;
  }
}

WMTSamplerAddressMode
ToAddressMode(DWORD mode) {
  switch (mode) {
  case D3DTADDRESS_MIRROR:
    return WMTSamplerAddressModeMirrorRepeat;
  case D3DTADDRESS_CLAMP:
    return WMTSamplerAddressModeClampToEdge;
  case D3DTADDRESS_BORDER:
    return WMTSamplerAddressModeClampToBorderColor;
  case D3DTADDRESS_MIRRORONCE:
    return WMTSamplerAddressModeMirrorClampToEdge;
  default:
    return WMTSamplerAddressModeRepeat;
  }
}

uint8_t
ToWriteMask(DWORD mask) {
  uint8_t out = 0;
  if (mask & D3DCOLORWRITEENABLE_RED)
    out |= WMTColorWriteMaskRed;
  if (mask & D3DCOLORWRITEENABLE_GREEN)
    out |= WMTColorWriteMaskGreen;
  if (mask & D3DCOLORWRITEENABLE_BLUE)
    out |= WMTColorWriteMaskBlue;
  if (mask & D3DCOLORWRITEENABLE_ALPHA)
    out |= WMTColorWriteMaskAlpha;
  return out;
}

bool
ToPrimitive(D3DPRIMITIVETYPE type, UINT count, WMTPrimitiveType &prim, WMTPrimitiveTopologyClass &topology, UINT &vertices) {
  switch (type) {
  case D3DPT_POINTLIST:
    prim = WMTPrimitiveTypePoint;
    topology = WMTPrimitiveTopologyClassPoint;
    vertices = count;
    return true;
  case D3DPT_LINELIST:
    prim = WMTPrimitiveTypeLine;
    topology = WMTPrimitiveTopologyClassLine;
    vertices = count * 2;
    return true;
  case D3DPT_LINESTRIP:
    prim = WMTPrimitiveTypeLineStrip;
    topology = WMTPrimitiveTopologyClassLine;
    vertices = count + 1;
    return true;
  case D3DPT_TRIANGLELIST:
    prim = WMTPrimitiveTypeTriangle;
    topology = WMTPrimitiveTopologyClassTriangle;
    vertices = count * 3;
    return true;
  case D3DPT_TRIANGLESTRIP:
    prim = WMTPrimitiveTypeTriangleStrip;
    topology = WMTPrimitiveTopologyClassTriangle;
    vertices = count + 2;
    return true;
  case D3DPT_TRIANGLEFAN:
    prim = WMTPrimitiveTypeTriangle;
    topology = WMTPrimitiveTopologyClassTriangle;
    vertices = count * 3;
    return true;
  default:
    return false;
  }
}

// Metal restarts 16-bit strips at index 0xFFFF and D3D9 doesn't, so such strips are drawn with 32-bit indices
static bool
NeedsWideStripIndices(D3DPRIMITIVETYPE Type, D3DFORMAT Format, const uint16_t *pIndices, UINT Count) {
  return (Type == D3DPT_TRIANGLESTRIP || Type == D3DPT_LINESTRIP) && Format == D3DFMT_INDEX16 &&
         std::find(pIndices, pIndices + Count, 0xFFFF) != pIndices + Count;
}

UINT
VertexCount(D3DPRIMITIVETYPE type, UINT count) {
  WMTPrimitiveType prim;
  WMTPrimitiveTopologyClass topology;
  UINT vertices = 0;
  ToPrimitive(type, count, prim, topology, vertices);
  return type == D3DPT_TRIANGLEFAN ? count + 2 : vertices;
}

// Metal has no fans; rewrite as a list. Index(i) yields the i-th fan vertex.
template <typename Fn>
std::vector<uint32_t>
FanToList(UINT primitives, Fn &&index) {
  std::vector<uint32_t> list;
  list.reserve(primitives * 3);
  for (UINT i = 0; i < primitives; i++) {
    list.push_back(index(0));
    list.push_back(index(i + 1));
    list.push_back(index(i + 2));
  }
  return list;
}

} // namespace

void
MTLD3D9Device::EndRenderPass() {
  if (!pass_active_)
    return;
  Emit([](ArgumentEncodingContext &enc) { enc.endPass(); });
  pass_active_ = false;
  pass_argbuf_size_ = nullptr;
}

uint64_t
MTLD3D9Device::PreAllocateArgumentBuffer(size_t Size, size_t Alignment) {
  uint64_t aligned = (*pass_argbuf_size_ + Alignment - 1) & ~(uint64_t)(Alignment - 1);
  *pass_argbuf_size_ = aligned + Size;
  return aligned;
}

WMTPixelFormat
MTLD3D9Device::RenderTargetFormat(MTLD3D9Surface *pSurface) {
  // a D3DFMT_NULL render target has no storage and no color attachment
  if (!pSurface->texture())
    return WMTPixelFormatInvalid;
  return pSurface->texture()->pixelFormat(RenderTargetView(pSurface));
}

TextureViewKey
MTLD3D9Device::RenderTargetView(MTLD3D9Surface *pSurface) {
  if (!render_states_[D3DRS_SRGBWRITEENABLE] || pSurface->IsDepthStencil() || !pSurface->texture())
    return pSurface->viewKey();
  auto texture = pSurface->texture();
  auto format = texture->pixelFormat(pSurface->viewKey());
  auto srgb = Recall_sRGB(format);
  if (srgb == format)
    return pSurface->viewKey();
  return texture->createView({
      .format = srgb,
      .type = texture->textureType(pSurface->viewKey()),
      .firstMiplevel = pSurface->level(),
      .miplevelCount = 1,
      .firstArraySlice = pSurface->slice(),
      .arraySize = 1,
  });
}

// a depth buffer smaller than the render target is left out, as Windows draws the whole target with it bound
// (Wine's test_occlusion_query)
MTLD3D9Surface *
MTLD3D9Device::PassDepthStencil() {
  auto ds = depth_stencil_.ptr();
  auto rt = render_targets_[0].ptr();
  if (ds && rt && rt->texture() && (ds->Desc().Width < rt->Desc().Width || ds->Desc().Height < rt->Desc().Height))
    return nullptr;
  return ds;
}

bool
MTLD3D9Device::BeginRenderPass() {
  if (pass_active_)
    return true;

  struct Target {
    Rc<Texture> texture;
    TextureViewKey view;
    WMTPixelFormat format = WMTPixelFormatInvalid;
  };
  std::array<Target, caps::MaxSimultaneousRenderTargets> targets;
  uint32_t count = 0, width = 0, height = 0, samples = 1;
  for (uint32_t i = 0; i < caps::MaxSimultaneousRenderTargets; i++) {
    auto rt = render_targets_[i].ptr();
    if (!rt)
      continue;
    count = i + 1;
    // draws to a D3DFMT_NULL target cover the depth buffer
    if (!rt->texture())
      continue;
    targets[i] = {rt->texture(), RenderTargetView(rt), RenderTargetFormat(rt)};
    if (!width) {
      width = rt->Desc().Width;
      height = rt->Desc().Height;
      samples = std::max<uint32_t>(1, rt->Desc().MultiSampleType);
    }
  }
  Target depth;
  if (auto ds = PassDepthStencil()) {
    depth = {ds->texture(), ds->viewKey(), ds->texture()->pixelFormat()};
    if (!width) {
      width = ds->Desc().Width;
      height = ds->Desc().Height;
      samples = ds->texture()->sampleCount();
    }
  }
  if (!width)
    return false;

  auto argbuf_size = std::make_unique<uint64_t>(0);
  pass_argbuf_size_ = argbuf_size.get();
  Emit([targets, depth, count, width, height, samples,
        argbuf_size = std::move(argbuf_size)](ArgumentEncodingContext &enc) mutable {
    uint32_t planar = depth.texture.ptr() ? DepthStencilPlanarFlags(depth.format) : 0;
    auto &info = *enc.startRenderPass(planar, 0, count, *argbuf_size);
    for (uint32_t i = 0; i < count; i++) {
      if (!targets[i].texture.ptr())
        continue;
      auto &color = info.colors[i];
      color.attachment = enc.access<PipelineStage::Pixel>(targets[i].texture, targets[i].view, ResourceAccess::ReadWrite);
      color.depth_plane = 0;
      color.load_action = WMTLoadActionLoad;
      color.store_action = WMTStoreActionStore;
      info.tile_barrier_pso_key.color_formats[i] = targets[i].format;
    }
    if (planar & 1) {
      info.depth.attachment = enc.access<PipelineStage::Pixel>(depth.texture, depth.view, ResourceAccess::ReadWrite);
      info.depth.load_action = WMTLoadActionLoad;
      info.depth.store_action = WMTStoreActionStore;
    }
    if (planar & 2) {
      info.stencil.attachment = enc.access<PipelineStage::Pixel>(depth.texture, depth.view, ResourceAccess::ReadWrite);
      info.stencil.load_action = WMTLoadActionLoad;
      info.stencil.store_action = WMTStoreActionStore;
    }
    info.render_target_width = width;
    info.render_target_height = height;
    info.render_target_array_length = 1;
    info.tile_barrier_pso_key.raster_sample_count = samples;
  });
  pass_active_ = true;
  return true;
}

WMT::RenderPipelineState
MTLD3D9Device::GetPipelineState(WMTPrimitiveTopologyClass Topology, WMT::Function VS, WMT::Function PS) {
  WMTRenderPipelineInfo info;
  WMT::InitializeRenderPipelineInfo(info);
  info.vertex_function = VS;
  info.fragment_function = PS;
  info.rasterization_enabled = !so_target_.ptr();
  info.input_primitive_topology = Topology;
  info.raster_sample_count = 1;

  bool blend = render_states_[D3DRS_ALPHABLENDENABLE];
  WMTBlendFactor src, dst, src_alpha, dst_alpha;
  ResolveBlendPair(render_states_[D3DRS_SRCBLEND], render_states_[D3DRS_DESTBLEND], src, dst);
  if (render_states_[D3DRS_SEPARATEALPHABLENDENABLE])
    ResolveBlendPair(render_states_[D3DRS_SRCBLENDALPHA], render_states_[D3DRS_DESTBLENDALPHA], src_alpha, dst_alpha);
  else
    src_alpha = src, dst_alpha = dst;
  auto op = ToBlendOp(render_states_[D3DRS_BLENDOP]);
  auto op_alpha = render_states_[D3DRS_SEPARATEALPHABLENDENABLE] ? ToBlendOp(render_states_[D3DRS_BLENDOPALPHA]) : op;
  static const D3DRENDERSTATETYPE write_masks[] = {
      D3DRS_COLORWRITEENABLE, D3DRS_COLORWRITEENABLE1, D3DRS_COLORWRITEENABLE2, D3DRS_COLORWRITEENABLE3
  };

  for (uint32_t i = 0; i < caps::MaxSimultaneousRenderTargets; i++) {
    auto rt = render_targets_[i].ptr();
    if (!rt)
      continue;
    auto &color = info.colors[i];
    color.pixel_format = RenderTargetFormat(rt);
    color.write_mask = ToWriteMask(render_states_[write_masks[i]]);
    info.raster_sample_count = std::max<uint32_t>(1, rt->Desc().MultiSampleType);
    bool blendable = int(adapter_->GetFormatCapability(rt->Desc().Format)) & int(FormatCapability::Blend);
    if (blend && blendable) {
      // render targets without alpha read it as 1, back buffers aside (Wine's visual test_blend)
      auto no_alpha = [&](WMTBlendFactor f) {
        if (rt->IsBackBuffer() || !D3D9HasNoAlpha(rt->Desc().Format))
          return f;
        return f == WMTBlendFactorDestinationAlpha           ? WMTBlendFactorOne
               : f == WMTBlendFactorOneMinusDestinationAlpha ? WMTBlendFactorZero
                                                             : f;
      };
      color.blending_enabled = true;
      color.src_rgb_blend_factor = no_alpha(src);
      color.dst_rgb_blend_factor = no_alpha(dst);
      color.src_alpha_blend_factor = no_alpha(src_alpha);
      color.dst_alpha_blend_factor = no_alpha(dst_alpha);
      color.rgb_blend_operation = op;
      color.alpha_blend_operation = op_alpha;
    }
  }
  if (auto ds = PassDepthStencil()) {
    auto format = ds->texture()->pixelFormat();
    uint32_t planar = DepthStencilPlanarFlags(format);
    if (planar & 1)
      info.depth_pixel_format = format;
    if (planar & 2)
      info.stencil_pixel_format = format;
    if (!render_targets_[0])
      info.raster_sample_count = ds->texture()->sampleCount();
  }

  // NVIDIA's switch: D3DRS_ADAPTIVETESS_Y = 'ATOC' with alpha testing on turns on alpha to coverage
  info.alpha_to_coverage_enabled = render_states_[D3DRS_ADAPTIVETESS_Y] == MAKEFOURCC('A', 'T', 'O', 'C') &&
                                   render_states_[D3DRS_ALPHATESTENABLE] && info.raster_sample_count > 1;

  Sha1HashState h;
  h.update(VS.handle);
  h.update(PS.handle);
  h.update(info.rasterization_enabled);
  h.update(info.alpha_to_coverage_enabled);
  h.update(info.colors);
  h.update(info.depth_pixel_format);
  h.update(info.stencil_pixel_format);
  h.update(info.input_primitive_topology);
  h.update(info.raster_sample_count);
  auto key = h.final();
  if (auto it = pipelines_.find(key); it != pipelines_.end())
    return it->second;

  WMT::Reference<WMT::Error> err;
  auto pso = GetMTLDevice().newRenderPipelineState(info, err);
  if (!pso)
    ERR("D3D9: failed to create pipeline state: ", err ? err.description().getUTF8String() : "unknown error");
  pipelines_.emplace(key, pso);
  return pso;
}

WMT::DepthStencilState
MTLD3D9Device::GetDepthStencilState() {
  auto &rs = render_states_;
  auto ds = PassDepthStencil();
  bool has_depth = ds && (DepthStencilPlanarFlags(ds->texture()->pixelFormat()) & 1);
  bool has_stencil = ds && (DepthStencilPlanarFlags(ds->texture()->pixelFormat()) & 2);

  WMTDepthStencilInfo info = {};
  bool depth = has_depth && rs[D3DRS_ZENABLE] != D3DZB_FALSE;
  info.depth_compare_function = depth ? ToCompare(rs[D3DRS_ZFUNC]) : WMTCompareFunctionAlways;
  info.depth_write_enabled = depth && rs[D3DRS_ZWRITEENABLE];

  bool stencil = has_stencil && rs[D3DRS_STENCILENABLE];
  auto stencil_info = [&](DWORD fail, DWORD zfail, DWORD pass, DWORD func) {
    WMTStencilInfo s = {};
    s.enabled = stencil;
    s.stencil_fail_op = ToStencilOp(rs[fail]);
    s.depth_fail_op = ToStencilOp(rs[zfail]);
    s.depth_stencil_pass_op = ToStencilOp(rs[pass]);
    s.stencil_compare_function = stencil ? ToCompare(rs[func]) : WMTCompareFunctionAlways;
    s.read_mask = (uint8_t)rs[D3DRS_STENCILMASK];
    s.write_mask = stencil ? (uint8_t)rs[D3DRS_STENCILWRITEMASK] : 0;
    return s;
  };
  info.front_stencil = stencil_info(D3DRS_STENCILFAIL, D3DRS_STENCILZFAIL, D3DRS_STENCILPASS, D3DRS_STENCILFUNC);
  info.back_stencil = rs[D3DRS_TWOSIDEDSTENCILMODE] ? stencil_info(
                                                          D3DRS_CCW_STENCILFAIL, D3DRS_CCW_STENCILZFAIL,
                                                          D3DRS_CCW_STENCILPASS, D3DRS_CCW_STENCILFUNC
                                                      )
                                                    : info.front_stencil;

  auto key = Sha1HashState::compute(&info, sizeof(info));
  if (auto it = depth_stencil_states_.find(key); it != depth_stencil_states_.end())
    return it->second;
  auto state = GetMTLDevice().newDepthStencilState(info);
  depth_stencil_states_.emplace(key, state);
  return state;
}

Rc<Sampler>
MTLD3D9Device::GetSampler(uint32_t Index, DWORD Lod) {
  auto &ss = sampler_states_[Index];
  WMTSamplerInfo info = {};
  auto filter = [](DWORD f) {
    return f == D3DTEXF_POINT || f == D3DTEXF_NONE ? WMTSamplerMinMagFilterNearest : WMTSamplerMinMagFilterLinear;
  };
  info.mag_filter = filter(ss[D3DSAMP_MAGFILTER]);
  info.min_filter = filter(ss[D3DSAMP_MINFILTER]);
  info.mip_filter = ss[D3DSAMP_MIPFILTER] == D3DTEXF_NONE    ? WMTSamplerMipFilterNotMipmapped
                    : ss[D3DSAMP_MIPFILTER] == D3DTEXF_POINT ? WMTSamplerMipFilterNearest
                                                             : WMTSamplerMipFilterLinear;
  info.s_address_mode = ToAddressMode(ss[D3DSAMP_ADDRESSU]);
  info.t_address_mode = ToAddressMode(ss[D3DSAMP_ADDRESSV]);
  info.r_address_mode = ToAddressMode(ss[D3DSAMP_ADDRESSW]);
  // Metal only has three border colors.
  D3DCOLOR border = ss[D3DSAMP_BORDERCOLOR];
  info.border_color = (border >> 24) == 0 ? WMTSamplerBorderColorTransparentBlack
                      : (border & 0xFFFFFF) == 0xFFFFFF ? WMTSamplerBorderColorOpaqueWhite
                                                        : WMTSamplerBorderColorOpaqueBlack;
  info.compare_function = WMTCompareFunctionNever;
  // the bound view starts at the texture's LOD, and MAXMIPLEVEL only limits further when mipmapping
  info.lod_min_clamp = (float)(ss[D3DSAMP_MAXMIPLEVEL] > Lod ? ss[D3DSAMP_MAXMIPLEVEL] - Lod : 0);
  info.lod_max_clamp = 1000.0f;
  bool anisotropic = ss[D3DSAMP_MINFILTER] == D3DTEXF_ANISOTROPIC || ss[D3DSAMP_MAGFILTER] == D3DTEXF_ANISOTROPIC;
  info.max_anisotroy = anisotropic ? std::clamp<DWORD>(ss[D3DSAMP_MAXANISOTROPY], 1, 16) : 1;
  info.normalized_coords = true;
  info.support_argument_buffers = true;
  // 'GET4' and 'GET1' in the bias switch AMD's FETCH4 on and off rather than biasing
  DWORD bias = ss[D3DSAMP_MIPMAPLODBIAS];
  float lod_bias = bias == MAKEFOURCC('G', 'E', 'T', '4') || bias == MAKEFOURCC('G', 'E', 'T', '1')
                       ? 0.0f
                       : std::bit_cast<float>(bias);

  Sha1HashState h;
  h.update(info);
  h.update(lod_bias);
  auto key = h.final();
  if (auto it = samplers_.find(key); it != samplers_.end())
    return it->second;
  auto sampler = Sampler::createSampler(GetMTLDevice(), info, lod_bias);
  samplers_.emplace(key, sampler);
  return sampler;
}

// apps may corrupt the vtable of a texture they bind (Wine's test_get_set_texture), so the type is read without a
// virtual call. The three texture classes only differ in their interface, which adds nothing but a vtable pointer.
template <typename Fn>
static void
WithTexture(IDirect3DBaseTexture9 *pTexture, Fn &&fn) {
  auto *base = static_cast<MTLD3D9TextureBase<IDirect3DTexture9> *>(static_cast<IDirect3DTexture9 *>(pTexture));
  switch (base->Desc().Type) {
  case D3DRTYPE_TEXTURE:
    return fn(static_cast<MTLD3D9Texture *>(pTexture));
  case D3DRTYPE_CUBETEXTURE:
    return fn(static_cast<MTLD3D9CubeTexture *>(pTexture));
  case D3DRTYPE_VOLUMETEXTURE:
    return fn(static_cast<MTLD3D9VolumeTexture *>(pTexture));
  default:
    return;
  }
}

void
MTLD3D9Device::ReleaseTextureBinding(BoundTexture &Binding) {
  if (Binding.texture)
    WithTexture(Binding.texture, [](auto *t) { t->ReleasePrivate(); });
  Binding.texture = nullptr;
}

void
MTLD3D9Device::BindStageResources(bool Pixel, MTLD3D9ShaderCode *Code, const float *FFConstants, UINT FFConstantCount) {
  auto &refl = Code->reflection();
  uint32_t stage = Pixel ? 1 : 0;
  std::vector<float> merged;
  if (!FFConstants && !Code->relativeDefs().empty()) {
    merged.assign(const_float_[stage].begin(), const_float_[stage].end());
    for (auto &def : Code->relativeDefs())
      if (def.reg < 256)
        std::memcpy(&merged[def.reg * 4], def.value, sizeof(def.value));
    FFConstants = merged.data();
    FFConstantCount = 256;
  }

  if (refl.NumConstantBuffers) {
    for (uint32_t slot = 0; slot < 4; slot++) {
      if (!(refl.ConstantBufferSlotMask & (1 << slot)))
        continue;
      if (slot == 0 && FFConstants) {
        Rc<Buffer> buffer;
        UINT offset = 0;
        UploadToRing(ff_constants_, ff_constants_offset_, FFConstants, FFConstantCount * 16, 256, buffer, offset);
        Emit([buffer = std::move(buffer), offset, Pixel, FFConstantCount](ArgumentEncodingContext &enc) mutable {
          if (Pixel)
            enc.bindConstantBuffer<PipelineStage::Pixel>(0, offset, FFConstantCount * 16, std::move(buffer));
          else
            enc.bindConstantBuffer<PipelineStage::Vertex>(0, offset, FFConstantCount * 16, std::move(buffer));
        });
        continue;
      }
      auto &storage = const_buffers_[stage][slot];
      if (!storage) {
        UINT sizes[4] = {256 * 16, 16 * 16, 16 * 4, (sizeof(SM30_RUNTIME_CONSTANTS) + 255) & ~255u};
        storage = std::make_unique<MTLD3D9BufferStorage>(this, sizes[slot], D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY);
        const_dirty_[stage] |= 1 << slot;
      }
      if (const_dirty_[stage] & (1 << slot)) {
        void *data = nullptr;
        storage->Lock(0, 0, &data, D3DLOCK_DISCARD);
        const void *src = slot == 0   ? (const void *)const_float_[stage].data()
                          : slot == 1 ? (const void *)const_int_[stage].data()
                          : slot == 2 ? (const void *)const_bool_[stage].data()
                                      : (const void *)&runtime_constants_;
        std::memset(data, 0, storage->length());
        std::memcpy(data, src, slot == 3 ? sizeof(runtime_constants_) : storage->length());
        storage->Unlock();
        const_dirty_[stage] &= ~(1u << slot);
      }
      storage->MarkUsed(queue().CurrentSeqId());
      Emit([buffer = storage->buffer(), length = storage->length(), slot, Pixel](ArgumentEncodingContext &enc) mutable {
        if (Pixel)
          enc.bindConstantBuffer<PipelineStage::Pixel>(slot, 0, length, std::move(buffer));
        else
          enc.bindConstantBuffer<PipelineStage::Vertex>(slot, 0, length, std::move(buffer));
      });
    }
    auto offset = PreAllocateArgumentBuffer(refl.NumConstantBuffers << 3, 32);
    Emit([refl = &refl, cbs = Code->constantBuffers(), offset, Pixel](ArgumentEncodingContext &enc) {
      if (Pixel)
        enc.encodeConstantBuffers<PipelineStage::Pixel, PipelineKind::Ordinary>(refl, cbs, offset);
      else
        enc.encodeConstantBuffers<PipelineStage::Vertex, PipelineKind::Ordinary>(refl, cbs, offset);
    });
  }

  if (refl.NumArguments) {
    for (uint32_t slot = 0; slot < 16; slot++) {
      if (!(refl.SamplerSlotMask & (1 << slot)))
        continue;
      uint32_t index = Pixel ? slot : caps::MaxSamplersPS + slot;
      Rc<Texture> texture;
      TextureViewKey view;
      DWORD lod = 0;
      if (auto bound = textures_[index].texture) {
        // only the low bit counts, as on AMD (Wine's visual srgbtexture_test)
        bool srgb = sampler_states_[index][D3DSAMP_SRGBTEXTURE] & 1;
        WithTexture(bound, [&](auto *t) {
          texture = t->texture();
          if (texture.ptr()) {
            view = t->SampleView(srgb);
            lod = t->GetLOD();
          }
        });
      }
      Emit([texture = std::move(texture), view, sampler = GetSampler(index, lod), slot,
            Pixel](ArgumentEncodingContext &enc) mutable {
        if (Pixel) {
          enc.bindTexture<PipelineStage::Pixel>(slot, std::move(texture), view);
          enc.bindSampler<PipelineStage::Pixel>(slot, sampler);
        } else {
          enc.bindTexture<PipelineStage::Vertex>(slot, std::move(texture), view);
          enc.bindSampler<PipelineStage::Vertex>(slot, sampler);
        }
      });
    }
    auto offset = PreAllocateArgumentBuffer(refl.ArgumentTableQwords << 3, 32);
    Emit([refl = &refl, args = Code->arguments(), offset, Pixel](ArgumentEncodingContext &enc) {
      if (Pixel)
        enc.encodeShaderResources<PipelineStage::Pixel, PipelineKind::Ordinary>(refl, args, offset);
      else
        enc.encodeShaderResources<PipelineStage::Vertex, PipelineKind::Ordinary>(refl, args, offset);
    });
  }
}

void
MTLD3D9Device::UpdateRuntimeConstants(bool Pretransformed) {
  auto f = [&](D3DRENDERSTATETYPE state) { return std::bit_cast<float>(render_states_[state]); };
  SM30_RUNTIME_CONSTANTS c = {};
  c.alpha_ref = (render_states_[D3DRS_ALPHAREF] & 0xFF) / 255.0f;
  c.point_size = f(D3DRS_POINTSIZE);
  c.point_size_min = f(D3DRS_POINTSIZE_MIN);
  c.point_size_max = f(D3DRS_POINTSIZE_MAX);
  D3DCOLOR fog = render_states_[D3DRS_FOGCOLOR];
  c.fog_color[0] = ((fog >> 16) & 0xFF) / 255.0f;
  c.fog_color[1] = ((fog >> 8) & 0xFF) / 255.0f;
  c.fog_color[2] = (fog & 0xFF) / 255.0f;
  c.fog_color[3] = 1.0f;
  c.fog_start = f(D3DRS_FOGSTART);
  c.fog_end = f(D3DRS_FOGEND);
  c.fog_density = f(D3DRS_FOGDENSITY);
  c.fog_scale = c.fog_end != c.fog_start ? 1.0f / (c.fog_end - c.fog_start) : 0.0f;
  std::memcpy(c.clip_planes, clip_planes_.data(), sizeof(c.clip_planes));
  c.depth_bias[0] = f(D3DRS_DEPTHBIAS);
  for (uint32_t i = 0; i < 8; i++) {
    auto &tss = texture_stage_states_[i];
    for (uint32_t k = 0; k < 4; k++)
      c.bump_env[i][k] = std::bit_cast<float>(tss[D3DTSS_BUMPENVMAT00 + k]);
    c.bump_luminance[i][0] = std::bit_cast<float>(tss[D3DTSS_BUMPENVLSCALE]);
    c.bump_luminance[i][1] = std::bit_cast<float>(tss[D3DTSS_BUMPENVLOFFSET]);
    // vertex processing moves a COUNT2 or COUNT3 divisor into w; pretransformed coordinates keep it in place
    DWORD flags = tss[D3DTSS_TEXTURETRANSFORMFLAGS];
    DWORD count = flags & ~D3DTTFF_PROJECTED;
    if (flags & D3DTTFF_PROJECTED)
      c.projection[i][Pretransformed && count >= 2 && count <= 3 ? count - 1 : 3] = 1.0f;
  }
  if (std::memcmp(&c, &runtime_constants_, sizeof(c))) {
    runtime_constants_ = c;
    const_dirty_[0] |= 8;
    const_dirty_[1] |= 8;
  }
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetClipPlane(DWORD Index, const float *pPlane) {
  auto lock = Lock();
  if (!pPlane)
    return D3DERR_INVALIDCALL;
  // indices past the last plane address the last one (Wine's test_clip_planes_limits)
  Index = std::min<DWORD>(Index, caps::MaxClipPlanes - 1);
  if (recording_) {
    recording_->SetClipPlane(Index, pPlane);
    return D3D_OK;
  }
  std::memcpy(clip_planes_[Index].data(), pPlane, sizeof(float) * 4);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetClipPlane(DWORD Index, float *pPlane) {
  if (!pPlane)
    return D3DERR_INVALIDCALL;
  Index = std::min<DWORD>(Index, caps::MaxClipPlanes - 1);
  std::memcpy(pPlane, clip_planes_[Index].data(), sizeof(float) * 4);
  return D3D_OK;
}

bool
MTLD3D9Device::PrepareDraw(WMTPrimitiveTopologyClass Topology, bool Instanced, uint32_t &InstanceCount) {
  InstanceCount = 1;
  if (!vertex_declaration_) {
    ERR_ONCE("D3D9: draw without a vertex declaration");
    return false;
  }
  // regenerating mips is a blit, so it has to happen before the render pass starts
  for (auto &binding : textures_)
    if (binding.texture)
      WithTexture(binding.texture, [](auto *t) {
        t->FlushManaged();
        t->FlushMips();
      });
  std::array<float, ff::VSConstantCount * 4> ff_vs_constants = {};
  std::array<float, ff::PSConstantCount * 4> ff_ps_constants = {};
  // POSITIONT skips vertex processing even with a vertex shader set
  bool positiont = std::any_of(
      vertex_declaration_->elements().begin(), vertex_declaration_->elements().end(),
      [](auto &e) { return e.Usage == D3DDECLUSAGE_POSITIONT; }
  );
  MTLD3D9VertexShader *vertex_shader = positiont ? nullptr : vertex_shader_.ptr();
  MTLD3D9ShaderCode *vs_code = vertex_shader ? vertex_shader->code() : FixedFunctionVS(ff_vs_constants.data());
  MTLD3D9ShaderCode *ps_code = pixel_shader_ ? pixel_shader_->code() : FixedFunctionPS(ff_ps_constants.data());
  if (!vs_code || !ps_code)
    return false;

  if (Instanced && (streams_[0].frequency & D3DSTREAMSOURCE_INDEXEDDATA))
    InstanceCount = std::max(1u, streams_[0].frequency & 0x3FFFFFFF);

  std::vector<SM50_IA_INPUT_ELEMENT> elements;
  uint32_t slot_mask = 0;
  for (auto &input : vs_code->inputs()) {
    for (auto &element : vertex_declaration_->elements()) {
      if (element.Usage != (BYTE)input.usage || element.UsageIndex != input.usage_index)
        continue;
      uint32_t format = D3D9DeclTypeToAttributeFormat(element.Type);
      if (!format)
        break;
      auto &stream = streams_[element.Stream];
      bool instanced = stream.frequency & D3DSTREAMSOURCE_INSTANCEDATA;
      elements.push_back({
          .reg = input.reg,
          .slot = element.Stream,
          .aligned_byte_offset = element.Offset,
          .format = format,
          .step_function = instanced ? 1u : 0u,
          .step_rate = instanced ? std::max(1u, stream.frequency & 0x3FFFFFFF) : 0u,
      });
      slot_mask |= 1u << element.Stream;
      break;
    }
  }

  auto metal_version = (SM50_SHADER_METAL_VERSION)GetMullionDevice().metalVersion();
  UpdateRuntimeConstants(positiont);
  bool points = Topology == WMTPrimitiveTopologyClassPoint;

  SM30_SHADER_D3D9_DATA vs_d3d9 = {};
  vs_d3d9.type = SM30_SHADER_D3D9;
  vs_d3d9.clip_plane_mask = render_states_[D3DRS_CLIPPLANEENABLE] & 0x3F;
  vs_d3d9.flags = points ? SM30_SHADER_FLAG_POINT_SIZE : 0;

  SM30_SHADER_D3D9_DATA ps_d3d9 = {};
  ps_d3d9.type = SM30_SHADER_D3D9;
  if (render_states_[D3DRS_ALPHATESTENABLE])
    ps_d3d9.alpha_func = render_states_[D3DRS_ALPHAFUNC];
  // D3D9 leaves fog to ps_3_0 shaders themselves
  bool shader_fog = pixel_shader_ && ps_code->bytecode()[0] >= 0xFFFF0300;
  if (render_states_[D3DRS_FOGENABLE] && !shader_fog) {
    switch (render_states_[D3DRS_FOGTABLEMODE]) {
    case D3DFOG_EXP:
      ps_d3d9.fog_mode = SM30_FOG_EXP;
      break;
    case D3DFOG_EXP2:
      ps_d3d9.fog_mode = SM30_FOG_EXP2;
      break;
    case D3DFOG_LINEAR:
      ps_d3d9.fog_mode = SM30_FOG_LINEAR;
      break;
    default:
      ps_d3d9.fog_mode = SM30_FOG_VERTEX;
      break;
    }
    // table fog reads w unless the projection is orthogonal, even with a vertex shader
    auto &proj = transforms_[D3DTS_PROJECTION];
    if (ps_d3d9.fog_mode != SM30_FOG_VERTEX && (proj._14 != 0 || proj._24 != 0 || proj._34 != 0 || proj._44 != 1))
      ps_d3d9.flags |= SM30_SHADER_FLAG_FOG_W;
  }
  if (render_states_[D3DRS_SHADEMODE] == D3DSHADE_FLAT)
    ps_d3d9.flags |= SM30_SHADER_FLAG_FLAT_SHADING;
  if (points && render_states_[D3DRS_POINTSPRITEENABLE])
    ps_d3d9.flags |= SM30_SHADER_FLAG_POINT_SPRITE;
  // native depth formats (not INTZ and friends) sample as a depth comparison
  shadow_samplers_ = 0;
  for (uint32_t i = 0; i < caps::MaxSamplersPS; i++) {
    auto texture = textures_[i].texture;
    if (!texture || texture->GetType() == D3DRTYPE_VOLUMETEXTURE)
      continue;
    D3DSURFACE_DESC desc = {};
    if (texture->GetType() == D3DRTYPE_TEXTURE)
      static_cast<IDirect3DTexture9 *>(texture)->GetLevelDesc(0, &desc);
    else
      static_cast<IDirect3DCubeTexture9 *>(texture)->GetLevelDesc(0, &desc);
    D3D9_FORMAT_DESC format;
    if (D3D9QueryFormat(desc.Format, format) && (format.Flags & D3D9_FORMAT_DEPTH) &&
        !(format.Flags & D3D9_FORMAT_SAMPLEABLE_DEPTH)) {
      shadow_samplers_ |= 1u << i;
      if (sampler_states_[i][D3DSAMP_MAGFILTER] == D3DTEXF_LINEAR || sampler_states_[i][D3DSAMP_MINFILTER] == D3DTEXF_LINEAR)
        shadow_samplers_ |= 1u << (16 + i);
    }
  }
  ps_d3d9.shadow_samplers = shadow_samplers_;
  for (uint32_t i = 0; i < caps::MaxSamplersPS; i++) {
    auto texture = textures_[i].texture;
    auto &ss = sampler_states_[i];
    if (!texture || texture->GetType() != D3DRTYPE_TEXTURE || ss[D3DSAMP_MIPMAPLODBIAS] != MAKEFOURCC('G', 'E', 'T', '4') ||
        ss[D3DSAMP_MAGFILTER] != D3DTEXF_POINT)
      continue;
    D3DSURFACE_DESC desc = {};
    static_cast<IDirect3DTexture9 *>(texture)->GetLevelDesc(0, &desc);
    switch ((DWORD)desc.Format) {
    case D3DFMT_L8:
    case D3DFMT_L16:
    case D3DFMT_R16F:
    case D3DFMT_R32F:
    case MAKEFOURCC('D', 'F', '1', '6'):
    case MAKEFOURCC('D', 'F', '2', '4'):
    case MAKEFOURCC('I', 'N', 'T', 'Z'):
      ps_d3d9.fetch4_samplers |= 1u << i;
      break;
    default:
      break;
    }
  }
  for (uint32_t i = 0; i < caps::MaxSamplersPS; i++)
    if (!textures_[i].texture && (ps_code->reflection().SamplerSlotMask & (1u << i)))
      ps_d3d9.unbound_samplers |= 1u << i;
  ps_d3d9.flags |= SM30_SHADER_FLAG_VS_OUTPUTS;
  // D3D9 biases after clipping, and Metal's own bias scales with each primitive's depth for float formats
  if (PassDepthStencil() && render_states_[D3DRS_DEPTHBIAS])
    ps_d3d9.flags |= SM30_SHADER_FLAG_DEPTH_BIAS;
  std::copy(vs_code->outputs().begin(), vs_code->outputs().end(), ps_d3d9.vs_outputs);
  if (ps_code->bytecode()[0] < 0xFFFF0200) {
    for (uint32_t i = 0; i < 8; i++) {
      auto texture = textures_[i].texture;
      D3DRESOURCETYPE type = texture ? texture->GetType() : D3DRTYPE_TEXTURE;
      uint32_t bits = type == D3DRTYPE_CUBETEXTURE ? 1 : type == D3DRTYPE_VOLUMETEXTURE ? 2 : 0;
      ps_d3d9.ps1_texture_types |= bits << (2 * i);
    }
  }

  SM50_SHADER_IA_INPUT_LAYOUT_DATA ia = {};
  ia.next = &vs_d3d9;
  ia.type = SM50_SHADER_IA_INPUT_LAYOUT;
  ia.index_buffer_format = SM50_INDEX_BUFFER_FORMAT_NONE;
  ia.slot_mask = slot_mask;
  ia.num_elements = elements.size();
  ia.elements = elements.data();
  auto vs_key = Sha1HashState()
                    .update(elements.data(), elements.size() * sizeof(elements[0]))
                    .update(slot_mask)
                    .update(vs_d3d9.clip_plane_mask)
                    .update(vs_d3d9.flags)
                    .final();
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *vs_args = (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&ia;
  std::vector<SM50_STREAM_OUTPUT_ELEMENT> so_elements;
  SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA so = {};
  if (so_target_.ptr()) {
    for (uint32_t reg = 0; reg < so_registers_; reg++)
      for (uint32_t c = 0; c < 4; c++)
        so_elements.push_back({reg, c, 0, (reg * 4 + c) * 4});
    so.next = &ia;
    so.type = SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT;
    so.num_output_slots = 1;
    so.num_elements = so_elements.size();
    so.strides[0] = so_registers_ * 16;
    so.elements = so_elements.data();
    vs_args = (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&so;
    vs_key = Sha1HashState().update(vs_key).update(so_registers_).final();
  }
  auto vs = vs_code->GetFunction(GetMTLDevice(), vs_key, vs_args, metal_version);

  SM50_SHADER_PSO_PIXEL_SHADER_DATA ps_data = {};
  ps_data.next = &ps_d3d9;
  ps_data.type = SM50_SHADER_PSO_PIXEL_SHADER;
  ps_data.sample_mask = 0xffffffff;
  if (render_targets_[0] && render_targets_[0]->Desc().MultiSampleType >= D3DMULTISAMPLE_2_SAMPLES)
    ps_data.sample_mask = render_states_[D3DRS_MULTISAMPLEMASK];
  for (uint32_t i = 0; i < caps::MaxSimultaneousRenderTargets; i++)
    if (render_targets_[i])
      ps_data.pixel_formats[i] = RenderTargetFormat(render_targets_[i].ptr());
  auto ps_key = Sha1HashState()
                    .update(ps_data.pixel_formats)
                    .update(ps_data.sample_mask)
                    .update(ps_d3d9.alpha_func)
                    .update(ps_d3d9.fog_mode)
                    .update(ps_d3d9.flags)
                    .update(ps_d3d9.ps1_texture_types)
                    .update(ps_d3d9.vs_outputs)
                    .update(ps_d3d9.shadow_samplers)
                    .update(ps_d3d9.fetch4_samplers)
                    .update(ps_d3d9.unbound_samplers)
                    .final();
  WMT::Function ps;
  if (!so_target_.ptr())
    ps = ps_code->GetFunction(GetMTLDevice(), ps_key, (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&ps_data, metal_version);
  if (!vs || (!ps && !so_target_.ptr()))
    return false;

  if (!BeginRenderPass())
    return false;

  auto pso = GetPipelineState(Topology, vs, ps);
  if (!pso)
    return false;
  Emit([pso](ArgumentEncodingContext &enc) {
    auto &cmd = enc.encodeRenderCommand<wmtcmd_render_setpso>();
    cmd.type = WMTRenderCommandSetPSO;
    cmd.pso = pso;
    enc.currentRenderEncoder()->last_pso = pso;
  });

  Emit([dsso = GetDepthStencilState(), ref = (uint8_t)render_states_[D3DRS_STENCILREF]](ArgumentEncodingContext &enc) {
    auto &cmd = enc.encodeRenderCommand<wmtcmd_render_setdsso>();
    cmd.type = WMTRenderCommandSetDSSO;
    cmd.dsso = dsso;
    cmd.stencil_ref = ref;
  });

  {
    wmtcmd_render_setrasterizerstate raster = {};
    raster.type = WMTRenderCommandSetRasterizerState;
    raster.fill_mode = render_states_[D3DRS_FILLMODE] == D3DFILL_SOLID ? WMTTriangleFillModeFill : WMTTriangleFillModeLines;
    raster.cull_mode = render_states_[D3DRS_CULLMODE] == D3DCULL_CW    ? WMTCullModeFront
                       : render_states_[D3DRS_CULLMODE] == D3DCULL_CCW ? WMTCullModeBack
                                                                       : WMTCullModeNone;
    // without D3DPMISCCAPS_CLIPTLVERTS pretransformed vertices aren't clipped against the depth range
    // (Wine's visual z_range_test, depth_clamp_test)
    raster.depth_clip_mode = positiont ? WMTDepthClipModeClamp : WMTDepthClipModeClip;
    raster.winding = WMTWindingClockwise;
    raster.depth_bias = 0.0f;
    raster.scole_scale = std::bit_cast<float>(render_states_[D3DRS_SLOPESCALEDEPTHBIAS]);
    raster.depth_bias_clamp = 0.0f;
    Emit([raster](ArgumentEncodingContext &enc) {
      auto &cmd = enc.encodeRenderCommand<wmtcmd_render_setrasterizerstate>();
      auto next = cmd.next;
      cmd = raster;
      cmd.next = next;
    });
  }

  {
    // D3D9 puts pixel centers on integer coordinates, Metal at +0.5
    auto [min_z, max_z] = EffectiveDepthRange();
    WMTViewport viewport = {
        viewport_.X + 0.5, viewport_.Y + 0.5, (double)viewport_.Width, (double)viewport_.Height, min_z, max_z
    };
    MTLD3D9Surface *target =
        render_targets_[0] && (render_targets_[0]->texture() || !depth_stencil_) ? render_targets_[0].ptr() : depth_stencil_.ptr();
    LONG width = target->Desc().Width, height = target->Desc().Height;
    WMTScissorRect scissor = {0, 0, (uint64_t)width, (uint64_t)height};
    if (render_states_[D3DRS_SCISSORTESTENABLE]) {
      LONG left = std::clamp(scissor_rect_.left, 0L, width), top = std::clamp(scissor_rect_.top, 0L, height);
      LONG right = std::clamp(scissor_rect_.right, left, width), bottom = std::clamp(scissor_rect_.bottom, top, height);
      scissor = {(uint64_t)left, (uint64_t)top, (uint64_t)(right - left), (uint64_t)(bottom - top)};
    }
    D3DCOLOR factor = render_states_[D3DRS_BLENDFACTOR];
    Emit([viewport, scissor, factor, ref = (uint8_t)render_states_[D3DRS_STENCILREF]](ArgumentEncodingContext &enc) {
      auto &vp = enc.encodeRenderCommand<wmtcmd_render_setviewport>();
      vp.type = WMTRenderCommandSetViewport;
      vp.viewport = viewport;
      auto &sc = enc.encodeRenderCommand<wmtcmd_render_setscissorrect>();
      sc.type = WMTRenderCommandSetScissorRect;
      sc.scissor_rect = scissor;
      auto &bc = enc.encodeRenderCommand<wmtcmd_render_setblendcolor>();
      bc.type = WMTRenderCommandSetBlendFactorAndStencilRef;
      bc.red = ((factor >> 16) & 0xFF) / 255.0f;
      bc.green = ((factor >> 8) & 0xFF) / 255.0f;
      bc.blue = (factor & 0xFF) / 255.0f;
      bc.alpha = ((factor >> 24) & 0xFF) / 255.0f;
      bc.stencil_ref = ref;
    });
  }
  if (slot_mask) {
    uint64_t seq = queue().CurrentSeqId();
    for (uint32_t slot = 0; slot < caps::MaxStreams; slot++) {
      if (!(slot_mask & (1u << slot)))
        continue;
      Rc<Buffer> buffer;
      UINT offset = 0, stride = 0;
      if (slot == 0 && up_stream_.buffer.ptr()) {
        buffer = up_stream_.buffer;
        offset = up_stream_.offset;
        stride = up_stream_.stride;
      } else if (auto vb = streams_[slot].buffer.ptr()) {
        buffer = vb->storage().buffer();
        offset = streams_[slot].offset;
        stride = streams_[slot].stride;
        vb->storage().MarkUsed(seq);
      }
      Emit([slot, offset, stride, buffer = std::move(buffer)](ArgumentEncodingContext &enc) mutable {
        enc.bindVertexBuffer(slot, offset, stride, std::move(buffer));
      });
    }
    auto offset = PreAllocateArgumentBuffer(16 * std::popcount(slot_mask), 32);
    Emit([slot_mask, offset](ArgumentEncodingContext &enc) {
      enc.encodeVertexBuffers<PipelineKind::Ordinary>(slot_mask, offset);
    });
  }

  BindStageResources(false, vs_code, vertex_shader ? nullptr : ff_vs_constants.data(), ff::VSConstantCount);
  if (so_target_.ptr()) {
    Emit([buffer = so_target_](ArgumentEncodingContext &enc) mutable {
      enc.bindStreamOutputBuffer(0, std::move(buffer), {});
    });
    auto offset = PreAllocateArgumentBuffer(16 * kStreamOutputSlots, 32);
    Emit([offset](ArgumentEncodingContext &enc) { enc.encodeVertexStreamOutput(offset); });
    return true;
  }
  BindStageResources(true, ps_code, pixel_shader_ ? nullptr : ff_ps_constants.data(), ff::PSConstantCount);
  return true;
}

HRESULT
MTLD3D9Device::UploadToRing(
    std::unique_ptr<MTLD3D9BufferStorage> &Ring, UINT &RingOffset, const void *pData, UINT Size, UINT Alignment,
    Rc<Buffer> &OutBuffer, UINT &OutOffset
) {
  constexpr UINT kRingSize = 4 << 20;
  if (!Ring || Ring->length() < Size) {
    Ring = std::make_unique<MTLD3D9BufferStorage>(
        this, std::max(kRingSize, Size * 2), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY
    );
    RingOffset = Ring->length();
  }
  UINT offset = (RingOffset + Alignment - 1) / Alignment * Alignment;
  void *data = nullptr;
  if (offset + Size > Ring->length()) {
    offset = 0;
    Ring->Lock(0, 0, &data, D3DLOCK_DISCARD);
  } else {
    Ring->Lock(offset, Size, &data, D3DLOCK_NOOVERWRITE);
    data = (char *)data - offset;
  }
  std::memcpy((char *)data + offset, pData, Size);
  Ring->Unlock();
  Ring->MarkUsed(queue().CurrentSeqId());
  RingOffset = offset + Size;
  OutBuffer = Ring->buffer();
  OutOffset = offset;
  return D3D_OK;
}

HRESULT
MTLD3D9Device::DrawWithIndices(
    D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT StartIndex, UINT PrimitiveCount, Rc<Buffer> IndexBuffer,
    UINT IndexOffset, D3DFORMAT IndexFormat, bool Instanced
) {
  WMTPrimitiveType prim;
  WMTPrimitiveTopologyClass topology;
  UINT count;
  if (!ToPrimitive(PrimitiveType, PrimitiveCount, prim, topology, count))
    return D3DERR_INVALIDCALL;
  uint32_t instances;
  if (!PrepareDraw(topology, Instanced, instances))
    return D3D_OK;

  UINT index_size = IndexFormat == D3DFMT_INDEX32 ? 4 : 2;
  Emit([prim, count, instances, BaseVertexIndex, buffer = std::move(IndexBuffer),
        offset = IndexOffset + StartIndex * index_size, index_size](ArgumentEncodingContext &enc) mutable {
    enc.bindIndexBuffer(std::move(buffer));
    enc.bumpVisibilityResultOffset();
    auto [index_buffer, sub_offset] = enc.currentIndexBuffer();
    enc.resolveRenderPassBarrier();
    auto &cmd = enc.encodeRenderCommand<wmtcmd_render_draw_indexed>();
    cmd.type = WMTRenderCommandDrawIndexed;
    cmd.primitive_type = prim;
    cmd.index_type = index_size == 4 ? WMTIndexTypeUInt32 : WMTIndexTypeUInt16;
    cmd.index_count = count;
    cmd.index_buffer = index_buffer;
    cmd.index_buffer_offset = offset + sub_offset;
    cmd.instance_count = instances;
    cmd.base_vertex = BaseVertexIndex;
    cmd.base_instance = 0;
  });
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) {
  auto lock = Lock();
  // without a declaration (FVF or explicit) nothing can be drawn (Wine's test_draw_primitive)
  if (!vertex_declaration_)
    return D3DERR_INVALIDCALL;
  if (!PrimitiveCount)
    return D3D_OK;

  if (PrimitiveType == D3DPT_TRIANGLEFAN) {
    auto list = FanToList(PrimitiveCount, [&](UINT i) { return StartVertex + i; });
    Rc<Buffer> buffer;
    UINT offset;
    UploadToRing(up_indices_, up_index_offset_, list.data(), list.size() * 4, 4, buffer, offset);
    return DrawWithIndices(D3DPT_TRIANGLELIST, 0, 0, PrimitiveCount, buffer, offset, D3DFMT_INDEX32);
  }

  WMTPrimitiveType prim;
  WMTPrimitiveTopologyClass topology;
  UINT count;
  if (!ToPrimitive(PrimitiveType, PrimitiveCount, prim, topology, count))
    return D3DERR_INVALIDCALL;
  uint32_t instances;
  if (!PrepareDraw(topology, false, instances))
    return D3D_OK;
  Emit([prim, StartVertex, count, instances](ArgumentEncodingContext &enc) {
    enc.bumpVisibilityResultOffset();
    enc.resolveRenderPassBarrier();
    auto &cmd = enc.encodeRenderCommand<wmtcmd_render_draw>();
    cmd.type = WMTRenderCommandDraw;
    cmd.primitive_type = prim;
    cmd.vertex_start = StartVertex;
    cmd.vertex_count = count;
    cmd.base_instance = 0;
    cmd.instance_count = instances;
  });
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::DrawIndexedPrimitive(
    D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT StartIndex,
    UINT PrimitiveCount
) {
  auto lock = Lock();
  if (!vertex_declaration_)
    return D3DERR_INVALIDCALL;
  if (!PrimitiveCount)
    return D3D_OK;
  if (!index_buffer_)
    return D3DERR_INVALIDCALL;

  auto format = index_buffer_->Desc().Format;
  index_buffer_->storage().MarkUsed(queue().CurrentSeqId());
  if (PrimitiveType == D3DPT_TRIANGLEFAN) {
    void *data = nullptr;
    index_buffer_->storage().Lock(0, 0, &data, D3DLOCK_READONLY);
    auto list = FanToList(PrimitiveCount, [&](UINT i) -> uint32_t {
      UINT at = StartIndex + i;
      return format == D3DFMT_INDEX32 ? ((const uint32_t *)data)[at] : ((const uint16_t *)data)[at];
    });
    index_buffer_->storage().Unlock();
    Rc<Buffer> buffer;
    UINT offset;
    UploadToRing(up_indices_, up_index_offset_, list.data(), list.size() * 4, 4, buffer, offset);
    return DrawWithIndices(
        D3DPT_TRIANGLELIST, BaseVertexIndex, 0, PrimitiveCount, buffer, offset, D3DFMT_INDEX32, true
    );
  }
  if (format == D3DFMT_INDEX16 && (PrimitiveType == D3DPT_TRIANGLESTRIP || PrimitiveType == D3DPT_LINESTRIP)) {
    void *data = nullptr;
    index_buffer_->storage().Lock(0, 0, &data, D3DLOCK_READONLY);
    auto indices = (const uint16_t *)data + StartIndex;
    UINT count = VertexCount(PrimitiveType, PrimitiveCount);
    std::vector<uint32_t> wide;
    if (NeedsWideStripIndices(PrimitiveType, format, indices, count))
      wide.assign(indices, indices + count);
    index_buffer_->storage().Unlock();
    if (!wide.empty()) {
      Rc<Buffer> buffer;
      UINT offset;
      UploadToRing(up_indices_, up_index_offset_, wide.data(), wide.size() * 4, 4, buffer, offset);
      return DrawWithIndices(PrimitiveType, BaseVertexIndex, 0, PrimitiveCount, buffer, offset, D3DFMT_INDEX32, true);
    }
  }
  return DrawWithIndices(
      PrimitiveType, BaseVertexIndex, StartIndex, PrimitiveCount, index_buffer_->storage().buffer(), 0, format, true
  );
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::DrawPrimitiveUP(
    D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void *pVertexStreamZeroData,
    UINT VertexStreamZeroStride
) {
  auto lock = Lock();
  if (!vertex_declaration_)
    return D3DERR_INVALIDCALL;
  if (!pVertexStreamZeroData || !VertexStreamZeroStride)
    return D3DERR_INVALIDCALL;
  if (!PrimitiveCount)
    return D3D_OK;

  UINT vertices = VertexCount(PrimitiveType, PrimitiveCount);
  UploadToRing(
      up_vertices_, up_vertex_offset_, pVertexStreamZeroData, vertices * VertexStreamZeroStride, VertexStreamZeroStride,
      up_stream_.buffer, up_stream_.offset
  );
  // the ring offset is a multiple of the stride, so draw from vertex 0 of a shifted stream
  up_stream_.stride = VertexStreamZeroStride;
  streams_[0] = {};
  streams_[0].stride = VertexStreamZeroStride;
  HRESULT hr = DrawPrimitive(PrimitiveType, 0, PrimitiveCount);
  up_stream_ = {};
  streams_[0] = {};
  return hr;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::DrawIndexedPrimitiveUP(
    D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, const void *pIndexData,
    D3DFORMAT IndexDataFormat, const void *pVertexStreamZeroData, UINT VertexStreamZeroStride
) {
  auto lock = Lock();
  if (!vertex_declaration_)
    return D3DERR_INVALIDCALL;
  if (!pIndexData || !pVertexStreamZeroData || !VertexStreamZeroStride)
    return D3DERR_INVALIDCALL;
  if (IndexDataFormat != D3DFMT_INDEX16 && IndexDataFormat != D3DFMT_INDEX32)
    return D3DERR_INVALIDCALL;
  if (!PrimitiveCount)
    return D3D_OK;

  UploadToRing(
      up_vertices_, up_vertex_offset_, pVertexStreamZeroData, (MinVertexIndex + NumVertices) * VertexStreamZeroStride,
      VertexStreamZeroStride, up_stream_.buffer, up_stream_.offset
  );
  up_stream_.stride = VertexStreamZeroStride;
  streams_[0] = {};
  streams_[0].stride = VertexStreamZeroStride;

  UINT index_size = IndexDataFormat == D3DFMT_INDEX32 ? 4 : 2;
  UINT indices = VertexCount(PrimitiveType, PrimitiveCount);
  HRESULT hr;
  Rc<Buffer> buffer;
  UINT offset;
  if (PrimitiveType == D3DPT_TRIANGLEFAN) {
    auto list = FanToList(PrimitiveCount, [&](UINT i) -> uint32_t {
      return index_size == 4 ? ((const uint32_t *)pIndexData)[i] : ((const uint16_t *)pIndexData)[i];
    });
    UploadToRing(up_indices_, up_index_offset_, list.data(), list.size() * 4, 4, buffer, offset);
    hr = DrawWithIndices(D3DPT_TRIANGLELIST, 0, 0, PrimitiveCount, buffer, offset, D3DFMT_INDEX32);
  } else if (NeedsWideStripIndices(PrimitiveType, IndexDataFormat, (const uint16_t *)pIndexData, indices)) {
    std::vector<uint32_t> wide((const uint16_t *)pIndexData, (const uint16_t *)pIndexData + indices);
    UploadToRing(up_indices_, up_index_offset_, wide.data(), wide.size() * 4, 4, buffer, offset);
    hr = DrawWithIndices(PrimitiveType, 0, 0, PrimitiveCount, buffer, offset, D3DFMT_INDEX32);
  } else {
    UploadToRing(up_indices_, up_index_offset_, pIndexData, indices * index_size, 4, buffer, offset);
    hr = DrawWithIndices(PrimitiveType, 0, 0, PrimitiveCount, buffer, offset, IndexDataFormat);
  }
  up_stream_ = {};
  streams_[0] = {};
  index_buffer_ = nullptr;
  return hr;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateVertexDeclaration(
    const D3DVERTEXELEMENT9 *pVertexElements, IDirect3DVertexDeclaration9 **ppDecl
) {
  auto lock = Lock();
  InitReturnPtr(ppDecl);
  if (!pVertexElements || !ppDecl)
    return D3DERR_INVALIDCALL;
  std::vector<D3DVERTEXELEMENT9> elements;
  for (auto e = pVertexElements; e->Stream != 0xFF; e++) {
    if (e->Stream >= caps::MaxStreams || elements.size() >= 64)
      return D3DERR_INVALIDCALL;
    // only the end marker may be UNUSED, and offsets are 4-byte aligned (Wine's test_unused_declaration_type)
    if (e->Type == D3DDECLTYPE_UNUSED || (e->Offset & 3))
      return E_FAIL;
    elements.push_back(*e);
  }
  *ppDecl = ref(new MTLD3D9VertexDeclaration(this, std::move(elements)));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetVertexDeclaration(IDirect3DVertexDeclaration9 *pDecl) {
  auto lock = Lock();
  if (recording_) {
    recording_->SetVertexDeclaration(pDecl);
    return D3D_OK;
  }
  vertex_declaration_ = static_cast<MTLD3D9VertexDeclaration *>(pDecl);
  fvf_ = vertex_declaration_ ? vertex_declaration_->fvf() : 0;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetVertexDeclaration(IDirect3DVertexDeclaration9 **ppDecl) {
  auto lock = Lock();
  InitReturnPtr(ppDecl);
  if (!ppDecl)
    return D3DERR_INVALIDCALL;
  if (vertex_declaration_)
    *ppDecl = vertex_declaration_.ref();
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetFVF(DWORD FVF) {
  auto lock = Lock();
  if (!FVF)
    return D3D_OK;
  if (recording_) {
    recording_->SetFVF(FVF);
    return D3D_OK;
  }
  auto &decl = fvf_declarations_[FVF];
  if (!decl)
    decl = new MTLD3D9VertexDeclaration(this, D3D9DeclarationFromFVF(FVF), FVF);
  vertex_declaration_ = decl;
  fvf_ = FVF;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetFVF(DWORD *pFVF) {
  if (!pFVF)
    return D3DERR_INVALIDCALL;
  *pFVF = fvf_;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateVertexShader(const DWORD *pFunction, IDirect3DVertexShader9 **ppShader) {
  auto lock = Lock();
  InitReturnPtr(ppShader);
  if (!ppShader)
    return D3DERR_INVALIDCALL;
  std::unique_ptr<MTLD3D9ShaderCode> code;
  HRESULT hr = MTLD3D9ShaderCode::Create(pFunction, false, code);
  if (FAILED(hr))
    return hr;
  shader_codes_.push_back(std::move(code));
  *ppShader = ref(new MTLD3D9VertexShader(this, shader_codes_.back().get()));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreatePixelShader(const DWORD *pFunction, IDirect3DPixelShader9 **ppShader) {
  auto lock = Lock();
  InitReturnPtr(ppShader);
  if (!ppShader)
    return D3DERR_INVALIDCALL;
  std::unique_ptr<MTLD3D9ShaderCode> code;
  HRESULT hr = MTLD3D9ShaderCode::Create(pFunction, true, code);
  if (FAILED(hr))
    return hr;
  shader_codes_.push_back(std::move(code));
  *ppShader = ref(new MTLD3D9PixelShader(this, shader_codes_.back().get()));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetVertexShader(IDirect3DVertexShader9 *pShader) {
  auto lock = Lock();
  if (recording_) {
    recording_->SetVertexShader(pShader);
    return D3D_OK;
  }
  vertex_shader_ = static_cast<MTLD3D9VertexShader *>(pShader);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetVertexShader(IDirect3DVertexShader9 **ppShader) {
  auto lock = Lock();
  InitReturnPtr(ppShader);
  if (!ppShader)
    return D3DERR_INVALIDCALL;
  if (vertex_shader_)
    *ppShader = vertex_shader_.ref();
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetPixelShader(IDirect3DPixelShader9 *pShader) {
  auto lock = Lock();
  if (recording_) {
    recording_->SetPixelShader(pShader);
    return D3D_OK;
  }
  pixel_shader_ = static_cast<MTLD3D9PixelShader *>(pShader);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetPixelShader(IDirect3DPixelShader9 **ppShader) {
  auto lock = Lock();
  InitReturnPtr(ppShader);
  if (!ppShader)
    return D3DERR_INVALIDCALL;
  if (pixel_shader_)
    *ppShader = pixel_shader_.ref();
  return D3D_OK;
}

template <typename T>
static HRESULT
RecordRegisters(
    MTLD3D9StateBlock *Block, void (MTLD3D9StateBlock::*Record)(bool, UINT, const T *, UINT), bool Pixel, UINT Start,
    const T *pData, UINT Count, UINT Max
) {
  if (!pData || Start > Max || Count > Max - Start)
    return D3DERR_INVALIDCALL;
  (Block->*Record)(Pixel, Start, pData, Count);
  return D3D_OK;
}

template <typename T, size_t N>
static HRESULT
SetRegisters(std::array<T, N> &regs, UINT Start, const void *pData, UINT Count, UINT Components, UINT Max) {
  if (!pData || Start > Max || Count > Max - Start)
    return D3DERR_INVALIDCALL;
  std::memcpy(regs.data() + Start * Components, pData, Count * Components * sizeof(T));
  return D3D_OK;
}

template <typename T, size_t N>
static HRESULT
GetRegisters(const std::array<T, N> &regs, UINT Start, void *pData, UINT Count, UINT Components, UINT Max) {
  if (!pData || Start > Max || Count > Max - Start)
    return D3DERR_INVALIDCALL;
  std::memcpy(pData, regs.data() + Start * Components, Count * Components * sizeof(T));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetVertexShaderConstantF(UINT StartRegister, const float *pConstantData, UINT Vector4fCount) {
  auto lock = Lock();
  if (recording_)
    return RecordRegisters(recording_.ptr(), &MTLD3D9StateBlock::SetConstantF, false, StartRegister, pConstantData, Vector4fCount, caps::MaxFloatConstantsVS);
  const_dirty_[0] |= 1;
  return SetRegisters(const_float_[0], StartRegister, pConstantData, Vector4fCount, 4, caps::MaxFloatConstantsVS);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetVertexShaderConstantF(UINT StartRegister, float *pConstantData, UINT Vector4fCount) {
  return GetRegisters(const_float_[0], StartRegister, pConstantData, Vector4fCount, 4, caps::MaxFloatConstantsVS);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetVertexShaderConstantI(UINT StartRegister, const int *pConstantData, UINT Vector4iCount) {
  auto lock = Lock();
  if (recording_)
    return RecordRegisters(recording_.ptr(), &MTLD3D9StateBlock::SetConstantI, false, StartRegister, pConstantData, Vector4iCount, caps::MaxOtherConstants);
  const_dirty_[0] |= 2;
  return SetRegisters(const_int_[0], StartRegister, pConstantData, Vector4iCount, 4, caps::MaxOtherConstants);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetVertexShaderConstantI(UINT StartRegister, int *pConstantData, UINT Vector4iCount) {
  return GetRegisters(const_int_[0], StartRegister, pConstantData, Vector4iCount, 4, caps::MaxOtherConstants);
}

static HRESULT
SetBools(std::array<uint32_t, 16> &regs, UINT Start, const BOOL *pData, UINT Count) {
  if (!pData || Start > caps::MaxOtherConstants || Count > caps::MaxOtherConstants - Start)
    return D3DERR_INVALIDCALL;
  for (UINT i = 0; i < Count; i++)
    regs[Start + i] = pData[i] ? 1 : 0;
  return D3D_OK;
}

static HRESULT
GetBools(const std::array<uint32_t, 16> &regs, UINT Start, BOOL *pData, UINT Count) {
  if (!pData || Start > caps::MaxOtherConstants || Count > caps::MaxOtherConstants - Start)
    return D3DERR_INVALIDCALL;
  for (UINT i = 0; i < Count; i++)
    pData[i] = regs[Start + i];
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetVertexShaderConstantB(UINT StartRegister, const BOOL *pConstantData, UINT BoolCount) {
  auto lock = Lock();
  if (recording_)
    return RecordRegisters(recording_.ptr(), &MTLD3D9StateBlock::SetConstantB, false, StartRegister, pConstantData, BoolCount, caps::MaxOtherConstants);
  const_dirty_[0] |= 4;
  return SetBools(const_bool_[0], StartRegister, pConstantData, BoolCount);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetVertexShaderConstantB(UINT StartRegister, BOOL *pConstantData, UINT BoolCount) {
  return GetBools(const_bool_[0], StartRegister, pConstantData, BoolCount);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetPixelShaderConstantF(UINT StartRegister, const float *pConstantData, UINT Vector4fCount) {
  auto lock = Lock();
  if (recording_)
    return RecordRegisters(recording_.ptr(), &MTLD3D9StateBlock::SetConstantF, true, StartRegister, pConstantData, Vector4fCount, caps::MaxFloatConstantsPS);
  const_dirty_[1] |= 1;
  return SetRegisters(const_float_[1], StartRegister, pConstantData, Vector4fCount, 4, caps::MaxFloatConstantsPS);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetPixelShaderConstantF(UINT StartRegister, float *pConstantData, UINT Vector4fCount) {
  return GetRegisters(const_float_[1], StartRegister, pConstantData, Vector4fCount, 4, caps::MaxFloatConstantsPS);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetPixelShaderConstantI(UINT StartRegister, const int *pConstantData, UINT Vector4iCount) {
  auto lock = Lock();
  if (recording_)
    return RecordRegisters(recording_.ptr(), &MTLD3D9StateBlock::SetConstantI, true, StartRegister, pConstantData, Vector4iCount, caps::MaxOtherConstants);
  const_dirty_[1] |= 2;
  return SetRegisters(const_int_[1], StartRegister, pConstantData, Vector4iCount, 4, caps::MaxOtherConstants);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetPixelShaderConstantI(UINT StartRegister, int *pConstantData, UINT Vector4iCount) {
  return GetRegisters(const_int_[1], StartRegister, pConstantData, Vector4iCount, 4, caps::MaxOtherConstants);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetPixelShaderConstantB(UINT StartRegister, const BOOL *pConstantData, UINT BoolCount) {
  auto lock = Lock();
  if (recording_)
    return RecordRegisters(recording_.ptr(), &MTLD3D9StateBlock::SetConstantB, true, StartRegister, pConstantData, BoolCount, caps::MaxOtherConstants);
  const_dirty_[1] |= 4;
  return SetBools(const_bool_[1], StartRegister, pConstantData, BoolCount);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetPixelShaderConstantB(UINT StartRegister, BOOL *pConstantData, UINT BoolCount) {
  return GetBools(const_bool_[1], StartRegister, pConstantData, BoolCount);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9 *pStreamData, UINT OffsetInBytes, UINT Stride) {
  auto lock = Lock();
  if (StreamNumber >= caps::MaxStreams)
    return D3DERR_INVALIDCALL;
  if (recording_) {
    recording_->SetStreamSource(StreamNumber, pStreamData, OffsetInBytes, Stride);
    return D3D_OK;
  }
  auto &stream = streams_[StreamNumber];
  stream.buffer = static_cast<MTLD3D9VertexBuffer *>(pStreamData);
  // unbinding keeps the previous offset and stride
  if (pStreamData) {
    stream.offset = OffsetInBytes;
    stream.stride = Stride;
  }
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetStreamSource(
    UINT StreamNumber, IDirect3DVertexBuffer9 **ppStreamData, UINT *pOffsetInBytes, UINT *pStride
) {
  auto lock = Lock();
  InitReturnPtr(ppStreamData);
  if (StreamNumber >= caps::MaxStreams || !ppStreamData || !pOffsetInBytes || !pStride)
    return D3DERR_INVALIDCALL;
  auto &stream = streams_[StreamNumber];
  if (stream.buffer)
    *ppStreamData = stream.buffer.ref();
  *pOffsetInBytes = stream.offset;
  *pStride = stream.stride;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetStreamSourceFreq(UINT StreamNumber, UINT Setting) {
  auto lock = Lock();
  if (StreamNumber >= caps::MaxStreams)
    return D3DERR_INVALIDCALL;
  bool indexed = Setting & D3DSTREAMSOURCE_INDEXEDDATA, instanced = Setting & D3DSTREAMSOURCE_INSTANCEDATA;
  if ((indexed && instanced) || (StreamNumber == 0 && instanced) || !Setting)
    return D3DERR_INVALIDCALL;
  if (recording_) {
    recording_->SetStreamSourceFreq(StreamNumber, Setting);
    return D3D_OK;
  }
  streams_[StreamNumber].frequency = Setting;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetStreamSourceFreq(UINT StreamNumber, UINT *pSetting) {
  if (StreamNumber >= caps::MaxStreams || !pSetting)
    return D3DERR_INVALIDCALL;
  *pSetting = streams_[StreamNumber].frequency;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetIndices(IDirect3DIndexBuffer9 *pIndexData) {
  auto lock = Lock();
  if (recording_) {
    recording_->SetIndices(pIndexData);
    return D3D_OK;
  }
  index_buffer_ = static_cast<MTLD3D9IndexBuffer *>(pIndexData);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetIndices(IDirect3DIndexBuffer9 **ppIndexData) {
  auto lock = Lock();
  InitReturnPtr(ppIndexData);
  if (!ppIndexData)
    return D3DERR_INVALIDCALL;
  if (index_buffer_)
    *ppIndexData = index_buffer_.ref();
  return D3D_OK;
}

static int
SamplerIndex(DWORD Sampler) {
  if (Sampler < caps::MaxSamplersPS)
    return Sampler;
  if (Sampler >= D3DVERTEXTEXTURESAMPLER0 && Sampler <= D3DVERTEXTEXTURESAMPLER3)
    return caps::MaxSamplersPS + (Sampler - D3DVERTEXTEXTURESAMPLER0);
  return -1;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetTexture(DWORD Stage, IDirect3DBaseTexture9 *pTexture) {
  auto lock = Lock();
  int index = SamplerIndex(Stage);
  if (index < 0)
    return Stage == D3DDMAPSAMPLER ? D3D_OK : D3DERR_INVALIDCALL;
  if (recording_) {
    recording_->SetTexture(index, pTexture);
    return D3D_OK;
  }
  auto &binding = textures_[index];
  if (binding.texture == pTexture)
    return D3D_OK;
  if (pTexture)
    WithTexture(pTexture, [](auto *t) { t->AddRefPrivate(); });
  ReleaseTextureBinding(binding);
  binding.texture = pTexture;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetTexture(DWORD Stage, IDirect3DBaseTexture9 **ppTexture) {
  auto lock = Lock();
  InitReturnPtr(ppTexture);
  int index = SamplerIndex(Stage);
  if (!ppTexture || index < 0)
    return D3DERR_INVALIDCALL;
  if (auto texture = textures_[index].texture) {
    texture->AddRef();
    *ppTexture = texture;
  }
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) {
  auto lock = Lock();
  int index = SamplerIndex(Sampler);
  if (index < 0 || Type < D3DSAMP_ADDRESSU || Type > D3DSAMP_DMAPOFFSET)
    return index < 0 && Sampler == D3DDMAPSAMPLER ? D3D_OK : D3DERR_INVALIDCALL;
  if (recording_) {
    recording_->SetSamplerState(index, Type, Value);
    return D3D_OK;
  }
  sampler_states_[index][Type] = Value;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD *pValue) {
  auto lock = Lock();
  int index = SamplerIndex(Sampler);
  if (!pValue || index < 0 || Type < D3DSAMP_ADDRESSU || Type > D3DSAMP_DMAPOFFSET)
    return D3DERR_INVALIDCALL;
  *pValue = sampler_states_[index][Type];
  return D3D_OK;
}

} // namespace mullion
