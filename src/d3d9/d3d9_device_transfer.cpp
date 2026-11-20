// the copies between surfaces a frame is read back through and the surfaces a frame is drawn from: ColorFill, which
// fills a default surface with a colour, StretchRect, which copies a rectangle of one surface to another (a scaled one
// is a draw of the source into the destination), UpdateSurface, which copies the CPU's bytes of a system-memory surface
// into a default one, and UpdateTexture, which does the same for the levels of a managed texture (Microsoft Learn,
// IDirect3DDevice9::ColorFill, ::StretchRect, ::UpdateSurface, ::UpdateTexture).
#include "d3d9_device.hpp"
#include "d3d9_clip_blit_shaders.hpp"
#include <algorithm>
#include <cstring>
#include <vector>

namespace mullion::d3d9 {

namespace {

float channel(D3DCOLOR colour, unsigned shift) { return (float)((colour >> shift) & 0xFF) / 255.0f; }

// a rectangle of a surface, clamped to it: D3D9's calls take NULL for the whole of it
bool clamp_rect(const RECT *rect, UINT width, UINT height, D3D11_BOX &box) {
  const int64_t left = rect ? std::max<int64_t>(rect->left, 0) : 0;
  const int64_t top = rect ? std::max<int64_t>(rect->top, 0) : 0;
  const int64_t right = rect ? std::min<int64_t>(rect->right, width) : width;
  const int64_t bottom = rect ? std::min<int64_t>(rect->bottom, height) : height;
  if (right <= left || bottom <= top)
    return false;
  box = D3D11_BOX{(UINT)left, (UINT)top, 0, (UINT)right, (UINT)bottom, 1};
  return true;
}

bool overlaps(const D3D11_BOX &a, const D3D11_BOX &b) {
  return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

} // namespace

// The render target view of one surface: the level and slice of its texture the surface is, which is how a surface of a
// cube face or a mip is drawn into at its own size. A format with no render target view has none.
HRESULT
MTLD3D9Device::surface_render_view(IMTLD3D9Surface *surface, ID3D11RenderTargetView **view) {
  D3D11_TEXTURE2D_DESC texture_desc{};
  surface->texture()->GetDesc(&texture_desc);
  D3D11_RENDER_TARGET_VIEW_DESC view_desc{};
  view_desc.Format = texture_desc.Format;
  if (texture_desc.SampleDesc.Count > 1) {
    if (texture_desc.ArraySize == 1) {
      view_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
    } else {
      view_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
      view_desc.Texture2DMSArray.FirstArraySlice = surface->array_slice();
      view_desc.Texture2DMSArray.ArraySize = 1;
    }
  } else if (texture_desc.ArraySize == 1) {
    view_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    view_desc.Texture2D.MipSlice = surface->level();
  } else {
    view_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
    view_desc.Texture2DArray.MipSlice = surface->level();
    view_desc.Texture2DArray.FirstArraySlice = surface->array_slice();
    view_desc.Texture2DArray.ArraySize = 1;
  }
  if (FAILED(d3d11_device_->CreateRenderTargetView(surface->texture(), &view_desc, view)))
    return D3DERR_INVALIDCALL;
  return S_OK;
}

// A default-pool surface filled with one colour, whole or in the rectangles the call names. The surface takes a render
// target view of its own (ColorFill is not a clear of the frame's target, so it neither reads nor changes what is
// bound); a system-memory surface or a depth-stencil surface is refused, as D3D9 refuses one.
HRESULT
MTLD3D9Device::ColorFill(IDirect3DSurface9 *surface, const RECT *rect, D3DCOLOR colour) {
  if (!surface)
    return D3DERR_INVALIDCALL;
  ComPtr<IMTLD3D9Surface> ours;
  if (FAILED(device_surface(surface, false, &ours)))
    return D3DERR_INVALIDCALL;
  if (ours->lockable()) {
    ERR_ONCE("D3D9: ColorFill fills a default-pool surface; this one's bytes are the CPU's.");
    return D3DERR_INVALIDCALL;
  }

  D3DSURFACE_DESC desc{};
  if (FAILED(surface->GetDesc(&desc)))
    return D3DERR_INVALIDCALL;
  D3D11_BOX box{};
  if (rect && !clamp_rect(rect, desc.Width, desc.Height, box))
    return S_OK;

  ComPtr<ID3D11RenderTargetView> view;
  if (FAILED(surface_render_view(ours.Get(), &view))) {
    ERR_ONCE("D3D9: this surface's format takes no render target view, so ColorFill cannot fill it.");
    return D3DERR_INVALIDCALL;
  }
  const float rgba[4] = {channel(colour, 16), channel(colour, 8), channel(colour, 0), channel(colour, 24)};
  if (!rect) {
    context_->ClearRenderTargetView(view.Get(), rgba);
    return S_OK;
  }
  // a part of the surface is a clear of part of the view, which the 11.1 context has
  if (!context1_) {
    ERR_ONCE("D3D9: ColorFill of part of a surface needs the context's ClearView, which this device's Direct3D 11 "
             "context has not.");
    return D3DERR_INVALIDCALL;
  }
  const D3D11_RECT region{(LONG)box.left, (LONG)box.top, (LONG)box.right, (LONG)box.bottom};
  context1_->ClearView(view.Get(), rgba, &region, 1);
  return S_OK;
}

// The objects a scaled copy draws with, made the first time one is asked for: the blit's two shaders (a strip of four
// vertices over the viewport, which samples the source at the rectangle its constant holds), the two samplers StretchRect's
// filters pick between, the one constant, and the states that keep the draw from being anything but a copy -- no blend,
// no depth, no cull, no scissor (d3d9_clip_blit_shaders.hpp).
HRESULT
MTLD3D9Device::ensure_blit_objects() {
  if (blit_vertex_shader_ && blit_pixel_shader_ && blit_point_sampler_ && blit_linear_sampler_ && blit_constants_ &&
      blit_blend_ && blit_depth_ && blit_raster_)
    return S_OK;
  HRESULT hr = d3d11_device_->CreateVertexShader(
      kBlitVertexShader, sizeof(kBlitVertexShader), nullptr, &blit_vertex_shader_
  );
  if (SUCCEEDED(hr))
    hr = d3d11_device_->CreatePixelShader(kBlitPixelShader, sizeof(kBlitPixelShader), nullptr, &blit_pixel_shader_);
  D3D11_SAMPLER_DESC sampler{};
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
  sampler.MaxLOD = D3D11_FLOAT32_MAX;
  sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  if (SUCCEEDED(hr))
    hr = d3d11_device_->CreateSamplerState(&sampler, &blit_point_sampler_);
  sampler.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
  if (SUCCEEDED(hr))
    hr = d3d11_device_->CreateSamplerState(&sampler, &blit_linear_sampler_);
  D3D11_BUFFER_DESC constants{};
  constants.ByteWidth = 4 * sizeof(float);
  constants.Usage = D3D11_USAGE_DYNAMIC;
  constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  constants.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (SUCCEEDED(hr))
    hr = d3d11_device_->CreateBuffer(&constants, nullptr, &blit_constants_);
  D3D11_BLEND_DESC blend{};
  blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  if (SUCCEEDED(hr))
    hr = d3d11_device_->CreateBlendState(&blend, &blit_blend_);
  D3D11_DEPTH_STENCIL_DESC depth{};
  if (SUCCEEDED(hr))
    hr = d3d11_device_->CreateDepthStencilState(&depth, &blit_depth_);
  D3D11_RASTERIZER_DESC raster{};
  raster.FillMode = D3D11_FILL_SOLID;
  raster.CullMode = D3D11_CULL_NONE;
  raster.DepthClipEnable = TRUE;
  if (SUCCEEDED(hr))
    hr = d3d11_device_->CreateRasterizerState(&raster, &blit_raster_);
  if (FAILED(hr)) {
    ERR("D3D9: the objects StretchRect's scaled copy draws with are not made: 0x", std::hex, hr);
    release_blit_objects();
    return D3DERR_INVALIDCALL;
  }
  return S_OK;
}

// The blit's objects belong to the Direct3D 11 device that made them, so a Reset that makes a new device lets them go.
void
MTLD3D9Device::release_blit_objects() {
  blit_vertex_shader_.Reset();
  blit_pixel_shader_.Reset();
  blit_point_sampler_.Reset();
  blit_linear_sampler_.Reset();
  blit_constants_.Reset();
  blit_blend_.Reset();
  blit_depth_.Reset();
  blit_raster_.Reset();
  blit_scratch_.Reset();
}

// A single-sample texture of the size and format asked for, which a copy or a resolve lands in before the blit samples
// it: a multisampled source cannot be sampled as one texel, a source that is no shader resource has no view, and a
// source that is the destination cannot be read while it is written. One is kept, and made again only for another shape.
ID3D11Texture2D *
MTLD3D9Device::blit_scratch(UINT width, UINT height, DXGI_FORMAT format) {
  if (blit_scratch_) {
    D3D11_TEXTURE2D_DESC held{};
    blit_scratch_->GetDesc(&held);
    if (held.Width == width && held.Height == height && held.Format == format)
      return blit_scratch_.Get();
    blit_scratch_.Reset();
  }
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(d3d11_device_->CreateTexture2D(&desc, nullptr, &blit_scratch_)))
    return nullptr;
  return blit_scratch_.Get();
}

// The source's rectangle drawn over the destination's: a strip of four vertices covers the viewport, which is set to the
// destination rectangle of the destination's level, and each pixel of it samples the source at the matching point of the
// source rectangle through the filter the call named (point for D3DTEXF_NONE and D3DTEXF_POINT, linear for
// D3DTEXF_LINEAR). The destination is a render target of this device and the draw is its own: what the frame had bound is
// not read, and the next draw binds its own state again.
HRESULT
MTLD3D9Device::stretch_blit(IMTLD3D9Surface *source, const D3D11_BOX &box, IMTLD3D9Surface *target, const RECT &destination,
                            bool linear) {
  HRESULT hr = ensure_blit_objects();
  if (FAILED(hr))
    return hr;

  D3D11_TEXTURE2D_DESC source_desc{}, target_desc{};
  source->texture()->GetDesc(&source_desc);
  target->texture()->GetDesc(&target_desc);
  const UINT source_sub = D3D11CalcSubresource(source->level(), source->array_slice(), source_desc.MipLevels);
  const UINT target_sub = D3D11CalcSubresource(target->level(), target->array_slice(), target_desc.MipLevels);

  ComPtr<ID3D11RenderTargetView> target_view;
  if (FAILED(surface_render_view(target, &target_view))) {
    ERR_ONCE("D3D9: StretchRect's destination takes no render target view, so a scaled copy cannot be drawn into it.");
    return D3DERR_INVALIDCALL;
  }

  // what the draw samples: the source's own level when it can be sampled, or a single-sample copy of it
  ID3D11Texture2D *sampled = source->texture();
  UINT sampled_width = std::max(1u, source_desc.Width >> source->level());
  UINT sampled_height = std::max(1u, source_desc.Height >> source->level());
  D3D11_BOX rectangle = box;
  D3D11_SHADER_RESOURCE_VIEW_DESC view_desc{};
  view_desc.Format = source_desc.Format;
  if (source_desc.ArraySize == 1) {
    view_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view_desc.Texture2D.MostDetailedMip = source->level();
    view_desc.Texture2D.MipLevels = 1;
  } else {
    view_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    view_desc.Texture2DArray.MostDetailedMip = source->level();
    view_desc.Texture2DArray.MipLevels = 1;
    view_desc.Texture2DArray.FirstArraySlice = source->array_slice();
    view_desc.Texture2DArray.ArraySize = 1;
  }
  if (source_desc.SampleDesc.Count > 1) {
    // a multisampled source is resolved whole, and the rectangle is taken from the resolved texture
    ID3D11Texture2D *resolved = blit_scratch(sampled_width, sampled_height, source_desc.Format);
    if (!resolved)
      return D3DERR_INVALIDCALL;
    context_->ResolveSubresource(resolved, 0, source->texture(), source_sub, source_desc.Format);
    sampled = resolved;
    view_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view_desc.Texture2D.MostDetailedMip = 0;
    view_desc.Texture2D.MipLevels = 1;
  } else if (!(source_desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) ||
             (source->texture() == target->texture() && source_sub == target_sub)) {
    // the rectangle alone is copied into a texture that can be sampled, and that texture is sampled whole
    sampled_width = box.right - box.left;
    sampled_height = box.bottom - box.top;
    ID3D11Texture2D *copy = blit_scratch(sampled_width, sampled_height, source_desc.Format);
    if (!copy)
      return D3DERR_INVALIDCALL;
    context_->CopySubresourceRegion(copy, 0, 0, 0, 0, source->texture(), source_sub, &box);
    sampled = copy;
    rectangle = D3D11_BOX{0, 0, 0, sampled_width, sampled_height, 1};
    view_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view_desc.Texture2D.MostDetailedMip = 0;
    view_desc.Texture2D.MipLevels = 1;
  }
  ComPtr<ID3D11ShaderResourceView> source_view;
  if (FAILED(d3d11_device_->CreateShaderResourceView(sampled, &view_desc, &source_view))) {
    ERR_ONCE("D3D9: StretchRect's source takes no shader resource view, so a scaled copy cannot sample it.");
    return D3DERR_INVALIDCALL;
  }

  // the source rectangle, as the texture coordinates the blit's constant holds
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context_->Map(blit_constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    return D3DERR_INVALIDCALL;
  const float coordinates[4] = {(float)rectangle.left / (float)sampled_width, (float)rectangle.top / (float)sampled_height,
                                (float)rectangle.right / (float)sampled_width,
                                (float)rectangle.bottom / (float)sampled_height};
  memcpy(mapped.pData, coordinates, sizeof(coordinates));
  context_->Unmap(blit_constants_.Get(), 0);

  const D3D11_VIEWPORT viewport{(float)destination.left, (float)destination.top,
                                (float)(destination.right - destination.left),
                                (float)(destination.bottom - destination.top), 0.0f, 1.0f};
  ID3D11RenderTargetView *views[1] = {target_view.Get()};
  context_->OMSetRenderTargets(1, views, nullptr);
  context_->OMSetBlendState(blit_blend_.Get(), nullptr, 0xFFFFFFFF);
  context_->OMSetDepthStencilState(blit_depth_.Get(), 0);
  context_->RSSetState(blit_raster_.Get());
  context_->RSSetViewports(1, &viewport);
  context_->IASetInputLayout(nullptr);
  context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  context_->VSSetShader(blit_vertex_shader_.Get(), nullptr, 0);
  ID3D11Buffer *constants = blit_constants_.Get();
  context_->VSSetConstantBuffers(0, 1, &constants);
  context_->PSSetShader(blit_pixel_shader_.Get(), nullptr, 0);
  ID3D11ShaderResourceView *resources[1] = {source_view.Get()};
  context_->PSSetShaderResources(0, 1, resources);
  ID3D11SamplerState *sampler = linear ? blit_linear_sampler_.Get() : blit_point_sampler_.Get();
  context_->PSSetSamplers(0, 1, &sampler);
  context_->Draw(4, 0);

  // nothing of the blit stays bound for the draw that follows to find
  resources[0] = nullptr;
  context_->PSSetShaderResources(0, 1, resources);
  context_->OMSetRenderTargets(0, nullptr, nullptr);
  return S_OK;
}

// A rectangle of one surface copied to another: D3D9's StretchRect. Rectangles of the same size are a copy of the
// pixels (CopySubresourceRegion, or a resolve when the source is multisampled), and rectangles of two sizes are the
// source drawn over the destination through the call's filter (stretch_blit). A format that is not the destination's,
// a destination whose bytes are the CPU's or are multisampled, and a rectangle that is not a part of its surface are
// refused (IDirect3DDevice9::StretchRect).
HRESULT
MTLD3D9Device::StretchRect(IDirect3DSurface9 *src_surface, const RECT *src_rect, IDirect3DSurface9 *dst_surface,
                           const RECT *dst_rect, D3DTEXTUREFILTERTYPE filter) {
  if (!src_surface || !dst_surface)
    return D3DERR_INVALIDCALL;
  if (filter != D3DTEXF_NONE && filter != D3DTEXF_POINT && filter != D3DTEXF_LINEAR)
    return D3DERR_INVALIDCALL;
  if (HRESULT lost = lost_reason(); FAILED(lost))
    return lost;

  D3DSURFACE_DESC src_desc{}, dst_desc{};
  if (FAILED(src_surface->GetDesc(&src_desc)) || FAILED(dst_surface->GetDesc(&dst_desc)))
    return D3DERR_INVALIDCALL;
  // a depth-stencil surface is a surface StretchRect copies or resolves, so it is asked for as one
  const bool src_depth = (src_desc.Usage & D3DUSAGE_DEPTHSTENCIL) != 0;
  const bool dst_depth = (dst_desc.Usage & D3DUSAGE_DEPTHSTENCIL) != 0;
  ComPtr<IMTLD3D9Surface> source, target;
  if (FAILED(device_surface(src_surface, src_depth, &source)) ||
      FAILED(device_surface(dst_surface, dst_depth, &target)))
    return D3DERR_INVALIDCALL;

  // a depth-stencil surface is not a render target: it is copied to another of the same format and size, or a
  // multisampled one resolves into a single-sample one, and no rectangle is a part of it (Microsoft Learn,
  // IDirect3DDevice9::StretchRect: a depth-stencil surface supports only these, and Wine visual.c depth_blit_test and
  // multisampled_depth_test do both)
  if (src_depth || dst_depth) {
    if (!src_depth || !dst_depth || src_desc.Format != dst_desc.Format || src_desc.Width != dst_desc.Width ||
        src_desc.Height != dst_desc.Height)
      return D3DERR_INVALIDCALL;
    const auto whole_rect = [](const RECT *rect, UINT width, UINT height) {
      return !rect ||
             (rect->left == 0 && rect->top == 0 && rect->right == (LONG)width && rect->bottom == (LONG)height);
    };
    if (!whole_rect(src_rect, src_desc.Width, src_desc.Height) ||
        !whole_rect(dst_rect, dst_desc.Width, dst_desc.Height))
      return D3DERR_INVALIDCALL;
    D3D11_TEXTURE2D_DESC source_texture{}, target_texture{};
    source->texture()->GetDesc(&source_texture);
    target->texture()->GetDesc(&target_texture);
    if (target_texture.SampleDesc.Count > 1 || source_texture.Format != target_texture.Format)
      return D3DERR_INVALIDCALL;
    const UINT source_sub = D3D11CalcSubresource(source->level(), source->array_slice(), source_texture.MipLevels);
    const UINT target_sub = D3D11CalcSubresource(target->level(), target->array_slice(), target_texture.MipLevels);
    if (source->texture() == target->texture() && source_sub == target_sub)
      return D3DERR_INVALIDCALL;
    if (source_texture.SampleDesc.Count > 1)
      context_->ResolveSubresource(
          target->texture(), target_sub, source->texture(), source_sub, to_dxgi_depth_format(src_desc.Format)
      );
    else
      context_->CopySubresourceRegion(target->texture(), target_sub, 0, 0, 0, source->texture(), source_sub, nullptr);
    return S_OK;
  }

  // Different formats are allowed: StretchRect converts through a blit (Wine device.c test_stretch_rect across
  // A8R8G8B8/X8R8G8B8/R5G6B5). Same-format unscaled copies use CopySubresourceRegion below.
  const bool same_format = src_desc.Format == dst_desc.Format;

  // Wine device.c test_stretch_rect: a game's StretchRect runs between surfaces whose bytes are not the CPU's. With
  // D3DDEVCAPS2_CAN_STRETCHRECT_FROM_TEXTURES the source may be a texture level, and the destination a render-target
  // texture level; a texture level that is not a render target cannot be written by one, and no depth-stencil surface
  // takes part (Microsoft Learn, IDirect3DDevice9::StretchRect: the source and destination are created in the default
  // pool, a texture surface needs that cap, and only a render target is a destination).
  const bool src_tex = source->texture_surface();
  const bool dst_tex = target->texture_surface();
  const bool src_rt = (src_desc.Usage & D3DUSAGE_RENDERTARGET) != 0;
  const bool dst_rt = (dst_desc.Usage & D3DUSAGE_RENDERTARGET) != 0;
  const bool src_plain = !src_tex && !src_rt;
  const bool dst_plain = !dst_tex && !dst_rt;
  if (dst_tex && !dst_rt) {
    ERR_ONCE("D3D9: StretchRect's destination is a texture level that is not a render target, which only a render "
             "target's level may be.");
    return D3DERR_INVALIDCALL;
  }
  if (dst_plain) {
    if (!(src_plain || src_tex) || src_desc.Pool != D3DPOOL_DEFAULT || dst_desc.Pool != D3DPOOL_DEFAULT) {
      ERR_ONCE("D3D9: StretchRect into an offscreen plain needs a DEFAULT offscreen source and destination.");
      return D3DERR_INVALIDCALL;
    }
  } else if (dst_rt) {
    if (src_desc.Pool != D3DPOOL_DEFAULT) {
      ERR_ONCE("D3D9: StretchRect's source is not a DEFAULT-pool surface, which a render target's copy needs.");
      return D3DERR_INVALIDCALL;
    }
    if (!src_rt && !src_plain && !src_tex) {
      ERR_ONCE("D3D9: StretchRect's destination is a render target; its source is neither a render target, a DEFAULT "
               "offscreen plain nor a texture level.");
      return D3DERR_INVALIDCALL;
    }
  } else {
    ERR_ONCE("D3D9: StretchRect's destination is neither a render target nor a DEFAULT offscreen plain.");
    return D3DERR_INVALIDCALL;
  }
  if (target->lockable()) {
    ERR_ONCE("D3D9: StretchRect's destination is a default-pool surface; this one's bytes are the CPU's.");
    return D3DERR_INVALIDCALL;
  }

  D3D11_TEXTURE2D_DESC source_texture{}, target_texture{};
  source->texture()->GetDesc(&source_texture);
  target->texture()->GetDesc(&target_texture);
  const UINT source_sub = D3D11CalcSubresource(source->level(), source->array_slice(), source_texture.MipLevels);
  const UINT target_sub = D3D11CalcSubresource(target->level(), target->array_slice(), target_texture.MipLevels);
  // a rectangle that is NULL is the whole of its surface, and the destination's has to be a part of it
  const RECT whole_destination{0, 0, (LONG)dst_desc.Width, (LONG)dst_desc.Height};
  const RECT destination = dst_rect ? *dst_rect : whole_destination;
  if (destination.left < 0 || destination.top < 0 || destination.right <= destination.left ||
      destination.bottom <= destination.top || destination.right > (LONG)dst_desc.Width ||
      destination.bottom > (LONG)dst_desc.Height) {
    ERR_ONCE("D3D9: StretchRect's destination rectangle is not a part of its surface.");
    return D3DERR_INVALIDCALL;
  }
  D3D11_BOX source_box{};
  if (!clamp_rect(src_rect, src_desc.Width, src_desc.Height, source_box))
    return S_OK;
  const UINT width = source_box.right - source_box.left;
  const UINT height = source_box.bottom - source_box.top;
  const UINT destination_width = (UINT)(destination.right - destination.left);
  const UINT destination_height = (UINT)(destination.bottom - destination.top);
  const bool scaled = width != destination_width || height != destination_height;

  // a multisampled source resolves into a single-sample destination of the same format (Microsoft Learn,
  // IDirect3DDevice9::StretchRect: "StretchRect can be used to perform a stretch blit from a multisampled surface to
  // a non-multisampled surface"). A copy between two surfaces of the same multisample count is accepted too, as D3D9
  // accepts it (Wine visual.c test_multisample_stretch_rect scales a single-sample source into a multisampled
  // destination); Direct3D 11 has no copy between multisampled resources, so the destination keeps its own samples.
  if (target_texture.SampleDesc.Count > 1) {
    // D3D9 accepts a copy into a multisampled surface, even one of another sample count (Wine visual.c
    // test_multisample_stretch_rect copies a single-sample 128 by 128 surface into a 2-sample one and reads it back
    // through a resolve). Direct3D 11 has no copy into or between multisampled resources, so the source is drawn into
    // every sample of the destination (stretch_blit resolves a multisampled source first).
    return stretch_blit(source.Get(), source_box, target.Get(), destination, filter == D3DTEXF_LINEAR);
  }

  // the same surface cannot be both what is read and what is written where the two rectangles meet
  const D3D11_BOX destination_box{(UINT)destination.left, (UINT)destination.top, 0, (UINT)destination.right,
                                  (UINT)destination.bottom, 1};
  if (source->texture() == target->texture() && source_sub == target_sub && overlaps(source_box, destination_box)) {
    ERR_ONCE("D3D9: StretchRect's source and destination overlap on one surface.");
    return D3DERR_INVALIDCALL;
  }

  // A scaled or cross-format copy is the source drawn over the destination (stretch_blit); a same-format unscaled
  // one is CopySubresourceRegion straight through.

  if (scaled || !same_format) {
    // a rectangle the source does not have is refused rather than stretched from what is left of it
    if (src_rect && (source_box.left != (UINT)src_rect->left || source_box.top != (UINT)src_rect->top ||
                     source_box.right != (UINT)src_rect->right || source_box.bottom != (UINT)src_rect->bottom)) {
      ERR_ONCE("D3D9: StretchRect's source rectangle is not a part of its surface.");
      return D3DERR_INVALIDCALL;
    }
    return stretch_blit(source.Get(), source_box, target.Get(), destination, filter == D3DTEXF_LINEAR);
  }

  if (source_texture.SampleDesc.Count > 1) {
    // the whole of a surface resolves in place; a rectangle of one resolves into a texture of the source's size and
    // is copied from there, since a resolve takes no rectangle. A scaled copy is the blit, which resolves the source
    // itself (Wine visual.c test_multisample_stretch_rect stretches a 128 by 128 multisampled surface into 64 by 64)
    if (!src_rect && !dst_rect && width == dst_desc.Width && height == dst_desc.Height) {
      context_->ResolveSubresource(target->texture(), target_sub, source->texture(), source_sub, source_texture.Format);
      return S_OK;
    }
    if (scaled)
      return stretch_blit(source.Get(), source_box, target.Get(), destination, filter == D3DTEXF_LINEAR);
    ID3D11Texture2D *resolved = blit_scratch(
        std::max(1u, source_texture.Width >> source->level()), std::max(1u, source_texture.Height >> source->level()),
        source_texture.Format
    );
    if (!resolved)
      return D3DERR_INVALIDCALL;
    context_->ResolveSubresource(resolved, 0, source->texture(), source_sub, source_texture.Format);
    context_->CopySubresourceRegion(
        target->texture(), target_sub, destination_box.left, destination_box.top, 0, resolved, 0, &source_box
    );
    return S_OK;
  }
  context_->CopySubresourceRegion(
      target->texture(), target_sub, destination_box.left, destination_box.top, 0, source->texture(), source_sub,
      &source_box
  );
  return S_OK;
}

// A system-memory texture's levels copied into a default-pool texture's, which is D3D9's UpdateTexture (Wine
// device.c test_update_texture_pool: only SYSTEMMEM → DEFAULT succeeds). The two are the same format and size, with the
// same level count, and each level's bytes are the ones the application authored into the source. A cube copies its six
// faces level by level; a volume locks each level and uploads into the default-pool resource.
HRESULT
MTLD3D9Device::UpdateTexture(IDirect3DBaseTexture9 *source, IDirect3DBaseTexture9 *destination) {
  if (!source || !destination || source == destination)
    return D3DERR_INVALIDCALL;

  ComPtr<IDirect3DCubeTexture9> source_cube, destination_cube;
  const bool source_is_cube = SUCCEEDED(source->QueryInterface(
      __uuidof(IDirect3DCubeTexture9), reinterpret_cast<void **>(source_cube.GetAddressOf())
  ));
  const bool destination_is_cube = SUCCEEDED(destination->QueryInterface(
      __uuidof(IDirect3DCubeTexture9), reinterpret_cast<void **>(destination_cube.GetAddressOf())
  ));
  if (source_is_cube || destination_is_cube) {
    // a cube is copied to a cube: a face has no other texture to be a face of
    if (!source_is_cube || !destination_is_cube) {
      ERR_ONCE("D3D9: UpdateTexture copies a cube texture to a cube texture, not to another kind.");
      return D3DERR_INVALIDCALL;
    }
    ComPtr<IMTLD3D9Texture> source_ours, destination_ours;
    if (FAILED(source_cube->QueryInterface(IID_IMTLD3D9Texture, reinterpret_cast<void **>(source_ours.GetAddressOf()))) ||
        FAILED(destination_cube->QueryInterface(
            IID_IMTLD3D9Texture, reinterpret_cast<void **>(destination_ours.GetAddressOf())
        )))
      return D3DERR_INVALIDCALL;
    // Wine device.c test_update_texture_pool: only SYSTEMMEM → DEFAULT succeeds (MANAGED sources are refused).
    if (source_ours->pool() != D3DPOOL_SYSTEMMEM || destination_ours->pool() != D3DPOOL_DEFAULT) {
      ERR_ONCE("D3D9: UpdateTexture's source is a system-memory texture and its destination a default-pool one.");
      return D3DERR_INVALIDCALL;
    }
    const UINT levels = source_cube->GetLevelCount();
    if (levels != destination_cube->GetLevelCount()) {
      ERR_ONCE("D3D9: UpdateTexture's textures have the same level count; these have ", levels, " and ",
               destination_cube->GetLevelCount(), ".");
      return D3DERR_INVALIDCALL;
    }
    D3DSURFACE_DESC source_desc{}, destination_desc{};
    if (FAILED(source_cube->GetLevelDesc(0, &source_desc)) || FAILED(destination_cube->GetLevelDesc(0, &destination_desc)))
      return D3DERR_INVALIDCALL;
    if (source_desc.Format != destination_desc.Format || source_desc.Width != destination_desc.Width) {
      ERR_ONCE("D3D9: UpdateTexture's textures are the same format and size, and these are not.");
      return D3DERR_INVALIDCALL;
    }
    // each face's level as the application authored it, uploaded into the same face and level of the destination: the
    // subresource of a cube is its face's slice, and the slice's levels follow one another
    ID3D11Texture2D *target = destination_ours->texture();
    for (UINT face = 0; face < 6; face++) {
      for (UINT level = 0; level < levels; level++) {
        D3DLOCKED_RECT locked{};
        if (FAILED(source_cube->LockRect((D3DCUBEMAP_FACES)face, level, &locked, nullptr, D3DLOCK_READONLY))) {
          ERR_ONCE("D3D9: UpdateTexture's source face ", face, " level ", level, " could not be locked.");
          return D3DERR_INVALIDCALL;
        }
        D3DLOCKED_RECT dest_locked{};
        if (SUCCEEDED(destination_cube->LockRect((D3DCUBEMAP_FACES)face, level, &dest_locked, nullptr, 0))) {
          D3DSURFACE_DESC level_desc{};
          source_cube->GetLevelDesc(level, &level_desc);
          const UINT rows = level_desc.Height;
          const UINT row_bytes = level_desc.Width * pixel_size(level_desc.Format);
          for (UINT row = 0; row < rows; row++) {
            memcpy(static_cast<char *>(dest_locked.pBits) + (size_t)row * dest_locked.Pitch,
                   static_cast<const char *>(locked.pBits) + (size_t)row * locked.Pitch, row_bytes);
          }
          destination_cube->UnlockRect((D3DCUBEMAP_FACES)face, level);
        } else if (target) {
          context_->UpdateSubresource(target, D3D11CalcSubresource(level, face, levels), nullptr, locked.pBits,
                                      (UINT)locked.Pitch, 0);
        } else {
          source_cube->UnlockRect((D3DCUBEMAP_FACES)face, level);
          return D3DERR_INVALIDCALL;
        }
        source_cube->UnlockRect((D3DCUBEMAP_FACES)face, level);
      }
    }
    destination_ours->sampling_changed();
    return S_OK;
  }

  ComPtr<IDirect3DVolumeTexture9> source_volume, destination_volume;
  if (SUCCEEDED(source->QueryInterface(
          __uuidof(IDirect3DVolumeTexture9), reinterpret_cast<void **>(source_volume.GetAddressOf())
      )) &&
      SUCCEEDED(destination->QueryInterface(
          __uuidof(IDirect3DVolumeTexture9), reinterpret_cast<void **>(destination_volume.GetAddressOf())
      ))) {
    ComPtr<IMTLD3D9Texture> source_ours, destination_ours;
    if (FAILED(source_volume->QueryInterface(IID_IMTLD3D9Texture, reinterpret_cast<void **>(source_ours.GetAddressOf()))) ||
        FAILED(destination_volume->QueryInterface(
            IID_IMTLD3D9Texture, reinterpret_cast<void **>(destination_ours.GetAddressOf())
        )))
      return D3DERR_INVALIDCALL;
    if (source_ours->pool() != D3DPOOL_SYSTEMMEM || destination_ours->pool() != D3DPOOL_DEFAULT ||
        !destination_ours->volume())
      return D3DERR_INVALIDCALL;
    const UINT levels = source_volume->GetLevelCount();
    if (levels != destination_volume->GetLevelCount())
      return D3DERR_INVALIDCALL;
    ID3D11Texture3D *target = destination_ours->volume();
    for (UINT level = 0; level < levels; level++) {
      D3DLOCKED_BOX locked{};
      if (FAILED(source_volume->LockBox(level, &locked, nullptr, D3DLOCK_READONLY)))
        return D3DERR_INVALIDCALL;
      D3DLOCKED_BOX dest_locked{};
      if (SUCCEEDED(destination_volume->LockBox(level, &dest_locked, nullptr, 0))) {
        D3DVOLUME_DESC level_desc{};
        source_volume->GetLevelDesc(level, &level_desc);
        for (UINT z = 0; z < level_desc.Depth; z++) {
          for (UINT y = 0; y < level_desc.Height; y++) {
            memcpy(static_cast<char *>(dest_locked.pBits) + (size_t)z * dest_locked.SlicePitch +
                       (size_t)y * dest_locked.RowPitch,
                   static_cast<const char *>(locked.pBits) + (size_t)z * locked.SlicePitch +
                       (size_t)y * locked.RowPitch,
                   level_desc.Width * pixel_size(level_desc.Format));
          }
        }
        destination_volume->UnlockBox(level);
      } else if (target) {
        context_->UpdateSubresource(target, level, nullptr, locked.pBits, (UINT)locked.RowPitch,
                                    (UINT)locked.SlicePitch);
      } else {
        source_volume->UnlockBox(level);
        return D3DERR_INVALIDCALL;
      }
      source_volume->UnlockBox(level);
    }
    destination_ours->sampling_changed();
    return S_OK;
  }

  ComPtr<IDirect3DTexture9> source_texture, destination_texture;
  if (FAILED(source->QueryInterface(
          __uuidof(IDirect3DTexture9), reinterpret_cast<void **>(source_texture.GetAddressOf())
      )) ||
      FAILED(destination->QueryInterface(
          __uuidof(IDirect3DTexture9), reinterpret_cast<void **>(destination_texture.GetAddressOf())
      ))) {
    ERR_ONCE("D3D9: UpdateTexture copies between textures of one kind: 2D, cube or volume.");
    return D3DERR_INVALIDCALL;
  }

  ComPtr<IMTLD3D9Texture> source_ours, destination_ours;
  if (FAILED(source_texture->QueryInterface(IID_IMTLD3D9Texture, reinterpret_cast<void **>(source_ours.GetAddressOf()))) ||
      FAILED(destination_texture->QueryInterface(
          IID_IMTLD3D9Texture, reinterpret_cast<void **>(destination_ours.GetAddressOf())
      )))
    return D3DERR_INVALIDCALL;
  // Wine device.c test_update_texture_pool: only SYSTEMMEM → DEFAULT succeeds.
  if (source_ours->pool() != D3DPOOL_SYSTEMMEM || destination_ours->pool() != D3DPOOL_DEFAULT) {
    ERR_ONCE("D3D9: UpdateTexture's source is a system-memory texture and its destination a default-pool one.");
    return D3DERR_INVALIDCALL;
  }

  const UINT levels = source_texture->GetLevelCount();
  if (levels != destination_texture->GetLevelCount()) {
    ERR_ONCE("D3D9: UpdateTexture's textures have the same level count; these have ", levels, " and ",
             destination_texture->GetLevelCount(), ".");
    return D3DERR_INVALIDCALL;
  }

  D3DSURFACE_DESC source_desc{}, destination_desc{};
  if (FAILED(source_texture->GetLevelDesc(0, &source_desc)) ||
      FAILED(destination_texture->GetLevelDesc(0, &destination_desc)))
    return D3DERR_INVALIDCALL;
  if (source_desc.Format != destination_desc.Format || source_desc.Width != destination_desc.Width ||
      source_desc.Height != destination_desc.Height) {
    ERR_ONCE("D3D9: UpdateTexture's textures are the same format and size, and these are not.");
    return D3DERR_INVALIDCALL;
  }

  // Prefer locking the destination when it is CPU-authored (DYNAMIC): Wine's pool test reads back through LockRect,
  // which sees the CPU shadow, not a lone UpdateSubresource into the GPU texture.
  ID3D11Texture2D *target = destination_ours->texture();
  IMTLD3D9LockTarget *source_lock = nullptr;
  if (SUCCEEDED(source_texture->QueryInterface(IID_IMTLD3D9LockTarget, reinterpret_cast<void **>(&source_lock))))
    source_texture->Release(); // QI AddRef'd the texture; keep an uncounted view for peek_level
  for (UINT level = 0; level < levels; level++) {
    D3DLOCKED_RECT locked{};
    bool unlock_source = false;
    if (SUCCEEDED(source_texture->LockRect(level, &locked, nullptr, D3DLOCK_READONLY))) {
      unlock_source = true;
    } else if (!source_lock || FAILED(source_lock->peek_level(level, &locked))) {
      // Sibling mip still LockRect'd: peek the CPU shadow without taking another lock (Wine device.c).
      ERR_ONCE("D3D9: UpdateTexture's source level ", level, " could not be locked.");
      return D3DERR_INVALIDCALL;
    }
    D3DLOCKED_RECT dest_locked{};
    if (SUCCEEDED(destination_texture->LockRect(level, &dest_locked, nullptr, 0))) {
      D3DSURFACE_DESC level_desc{};
      source_texture->GetLevelDesc(level, &level_desc);
      const UINT rows = format_height_blocks(level_desc.Format, level_desc.Height);
      const UINT row_bytes = format_row_pitch(level_desc.Format, level_desc.Width);
      for (UINT row = 0; row < rows; row++) {
        memcpy(static_cast<char *>(dest_locked.pBits) + (size_t)row * dest_locked.Pitch,
               static_cast<const char *>(locked.pBits) + (size_t)row * locked.Pitch, row_bytes);
      }
      destination_texture->UnlockRect(level);
    } else if (target) {
      context_->UpdateSubresource(target, level, nullptr, locked.pBits, (UINT)locked.Pitch, 0);
    } else {
      if (unlock_source)
        source_texture->UnlockRect(level);
      return D3DERR_INVALIDCALL;
    }
    if (unlock_source)
      source_texture->UnlockRect(level);
  }
  // a destination written through texture() has changed under any sampling twin (an L8 texture's)
  destination_ours->sampling_changed();
  return S_OK;
}

// A system-memory surface's bytes copied into a default-pool one at a point, which is D3D9's UpdateSurface: no stretch,
// no format conversion, and the destination's bytes stay the device's. The source is mapped and its rectangle written
// into the destination with the mapped row pitch, so the copy is the pixels the application wrote and no others.
HRESULT
MTLD3D9Device::UpdateSurface(IDirect3DSurface9 *src_surface, const RECT *src_rect, IDirect3DSurface9 *dst_surface,
                             const POINT *dst_point) {
  if (!src_surface || !dst_surface)
    return D3DERR_INVALIDCALL;
  const POINT origin{0, 0};
  if (!dst_point)
    dst_point = &origin;
  ComPtr<IMTLD3D9Surface> source, target;
  if (FAILED(device_surface(src_surface, false, &source)) || FAILED(device_surface(dst_surface, false, &target)))
    return D3DERR_INVALIDCALL;

  D3DSURFACE_DESC src_desc{}, dst_desc{};
  if (FAILED(src_surface->GetDesc(&src_desc)) || FAILED(dst_surface->GetDesc(&dst_desc)))
    return D3DERR_INVALIDCALL;
  // Pool contract, not lockable(): texture GetSurfaceLevel surfaces report lockable false but still LockRect through
  // the parent (Wine device.c UpdateSurface while a sibling mip stays locked).
  if (src_desc.Pool != D3DPOOL_SYSTEMMEM || dst_desc.Pool != D3DPOOL_DEFAULT) {
    ERR_ONCE("D3D9: UpdateSurface copies a system-memory surface into a default-pool one.");
    return D3DERR_INVALIDCALL;
  }
  if (src_desc.Format != dst_desc.Format) {
    ERR_ONCE("D3D9: UpdateSurface does not convert formats: this copy is from ", (unsigned)src_desc.Format, " to ",
             (unsigned)dst_desc.Format, ".");
    return D3DERR_INVALIDCALL;
  }

  D3D11_BOX source_box{};
  if (!clamp_rect(src_rect, src_desc.Width, src_desc.Height, source_box))
    return S_OK;
  const UINT width = source_box.right - source_box.left;
  const UINT height = source_box.bottom - source_box.top;
  if (dst_point->x < 0 || dst_point->y < 0 || (UINT)dst_point->x + width > dst_desc.Width ||
      (UINT)dst_point->y + height > dst_desc.Height) {
    ERR_ONCE("D3D9: UpdateSurface's rectangle does not fit the destination at the point it names.");
    return D3DERR_INVALIDCALL;
  }
  const UINT pixel_bytes = pixel_size(src_desc.Format);
  if (!pixel_bytes && !block_size(src_desc.Format))
    return D3DERR_INVALIDCALL;

  // Prefer LockRect so SYSTEMMEM texture levels use the CPU shadow (staging Map is not the lock path).
  D3DLOCKED_RECT locked{};
  const bool locked_ok = SUCCEEDED(src_surface->LockRect(&locked, src_rect, D3DLOCK_READONLY));
  if (!locked_ok) {
    ERR_ONCE("D3D9: UpdateSurface's source could not be locked.");
    return D3DERR_INVALIDCALL;
  }
  D3D11_TEXTURE2D_DESC target_texture{};
  target->texture()->GetDesc(&target_texture);
  const UINT target_sub = D3D11CalcSubresource(target->level(), target->array_slice(), target_texture.MipLevels);
  const D3D11_BOX destination_box{(UINT)dst_point->x, (UINT)dst_point->y, 0, (UINT)dst_point->x + width,
                                  (UINT)dst_point->y + height, 1};
  context_->UpdateSubresource(target->texture(), target_sub, &destination_box, locked.pBits, (UINT)locked.Pitch, 0);
  src_surface->UnlockRect();
  return S_OK;
}

} // namespace mullion::d3d9
