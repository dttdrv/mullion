// the render states a frame is drawn under and the targets it is drawn into: SetRenderState and GetRenderState over the
// states this fixed-function path can honour (d3d9_state.cpp turns their values into the D3D11 state objects a draw
// binds), the depth-stencil surfaces, the render targets, and the viewport a draw is placed and clipped by.
#include "d3d9_device.hpp"
#include <vector>

namespace mullion::d3d9 {

namespace {

float channel(D3DCOLOR colour, unsigned shift) { return (float)((colour >> shift) & 0xFF) / 255.0f; }

// the sRGB encoding of each 8-bit value, which D3DRS_SRGBWRITEENABLE applies to a clear colour. The transfer function
// is the piecewise one in the sRGB specification, as a table: a clear colour has only 256 values a channel, and the
// PE32 CRT this build runs against does not implement powf.
const uint8_t kSrgbByte[256] = {
    0x00, 0x0d, 0x16, 0x1c, 0x22, 0x26, 0x2a, 0x2e, 0x32, 0x35, 0x38, 0x3b, 0x3d, 0x40, 0x42, 0x45, 0x47, 0x49,
    0x4b, 0x4d, 0x4f, 0x51, 0x53, 0x55, 0x56, 0x58, 0x5a, 0x5c, 0x5d, 0x5f, 0x60, 0x62, 0x63, 0x65, 0x66, 0x68,
    0x69, 0x6a, 0x6c, 0x6d, 0x6e, 0x70, 0x71, 0x72, 0x73, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x7c, 0x7d, 0x7e,
    0x7f, 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x90,
    0x91, 0x92, 0x93, 0x94, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f, 0x9f,
    0xa0, 0xa1, 0xa2, 0xa3, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa7, 0xa8, 0xa9, 0xaa, 0xaa, 0xab, 0xac, 0xad, 0xad,
    0xae, 0xaf, 0xaf, 0xb0, 0xb1, 0xb2, 0xb2, 0xb3, 0xb4, 0xb4, 0xb5, 0xb6, 0xb6, 0xb7, 0xb8, 0xb9, 0xb9, 0xba,
    0xbb, 0xbb, 0xbc, 0xbd, 0xbd, 0xbe, 0xbe, 0xbf, 0xc0, 0xc0, 0xc1, 0xc2, 0xc2, 0xc3, 0xc4, 0xc4, 0xc5, 0xc5,
    0xc6, 0xc7, 0xc7, 0xc8, 0xc8, 0xc9, 0xca, 0xca, 0xcb, 0xcb, 0xcc, 0xcd, 0xcd, 0xce, 0xce, 0xcf, 0xd0, 0xd0,
    0xd1, 0xd1, 0xd2, 0xd2, 0xd3, 0xd4, 0xd4, 0xd5, 0xd5, 0xd6, 0xd6, 0xd7, 0xd7, 0xd8, 0xd8, 0xd9, 0xda, 0xda,
    0xdb, 0xdb, 0xdc, 0xdc, 0xdd, 0xdd, 0xde, 0xde, 0xdf, 0xdf, 0xe0, 0xe0, 0xe1, 0xe2, 0xe2, 0xe3, 0xe3, 0xe4,
    0xe4, 0xe5, 0xe5, 0xe6, 0xe6, 0xe7, 0xe7, 0xe8, 0xe8, 0xe9, 0xe9, 0xea, 0xea, 0xeb, 0xeb, 0xec, 0xec, 0xed,
    0xed, 0xee, 0xee, 0xee, 0xef, 0xef, 0xf0, 0xf0, 0xf1, 0xf1, 0xf2, 0xf2, 0xf3, 0xf3, 0xf4, 0xf4, 0xf5, 0xf5,
    0xf6, 0xf6, 0xf6, 0xf7, 0xf7, 0xf8, 0xf8, 0xf9, 0xf9, 0xfa, 0xfa, 0xfb, 0xfb, 0xfb, 0xfc, 0xfc, 0xfd, 0xfd,
    0xfe, 0xfe, 0xff, 0xff};

} // namespace

HRESULT
MTLD3D9Device::device_surface(IDirect3DSurface9 *surface, bool depth_stencil, IMTLD3D9Surface **out) {
  *out = nullptr;
  if (!surface)
    return D3DERR_INVALIDCALL;
  ComPtr<IMTLD3D9Surface> ours;
  if (FAILED(surface->QueryInterface(IID_IMTLD3D9Surface, reinterpret_cast<void **>(ours.GetAddressOf()))) ||
      ours->depth_stencil() != depth_stencil)
    return D3DERR_INVALIDCALL;
  // Surfaces from another device must not bind (Wine device.c test_device_mismatch SetRenderTarget).
  ComPtr<IDirect3DDevice9> owner;
  if (FAILED(surface->GetDevice(&owner)) || owner.Get() != static_cast<IDirect3DDevice9 *>(this))
    return D3DERR_INVALIDCALL;
  *out = ours.Detach();
  return S_OK;
}

ID3D11DepthStencilView *
MTLD3D9Device::bound_depth_stencil() const {
  if (!depth_stencil_view_ || !depth_stencil_)
    return nullptr;
  D3D11_TEXTURE2D_DESC desc{};
  depth_stencil_->texture()->GetDesc(&desc);
  if (desc.Width != target_width_ || desc.Height != target_height_ || desc.SampleDesc.Count != target_samples_)
    return nullptr;
  return depth_stencil_view_.Get();
}

// a clear of the whole target or of the rectangles the call names, as D3D9's is: `count` rectangles of `rects`, each
// clipped to the viewport, the scissor rectangle while the scissor test is on, and the target, and a rectangle that
// names no pixel clears none. No rectangles at all (`rects` NULL) is the whole of what those clips leave; `count` of 0
// with rectangles named clears none of them (Wine visual.c clear_test). Colour and depth have a D3D11 clear of a part
// of a view (ID3D11DeviceContext1::ClearView); the stencil of part of a surface has none, so a rectangle's stencil is a
// scissored draw that replaces it (clear_stencil_rects).
HRESULT
MTLD3D9Device::Clear(DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR colour, float depth, DWORD stencil) {
  if (HRESULT lost = lost_reason(); FAILED(lost))
    return lost;
  if (!(flags & (D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)))
    return S_OK;

  HRESULT hr = ensure_render_target();
  if (FAILED(hr))
    return hr;

  // a clear that names depth or stencil without a depth-stencil the target set can bind is invalid before any of it is
  // done, not after the colour has already been cleared (Wine device.c test_depthstenciltest)
  const bool clear_depth = (flags & D3DCLEAR_ZBUFFER) != 0;
  const bool clear_stencil = (flags & D3DCLEAR_STENCIL) != 0;
  // a clear reaches the depth-stencil surface even when the target set's sample count differs from it: D3D9 clears
  // both through such a pair (Wine visual.c test_multisample_mismatch), while a draw does not bind the mismatched
  // surface (bound_depth_stencil)
  ID3D11DepthStencilView *dsv = depth_stencil_view_.Get();
  if ((clear_depth || clear_stencil) && !dsv)
    return D3DERR_INVALIDCALL;

  // D3D9 clips a clear to the viewport, narrowed by the scissor rectangle while the scissor test is on (Wine visual.c
  // clear_test: a clear with a viewport set leaves the pixels outside it as they were, and a scissor clips both a full
  // clear and a rectangle of one). The target is the last clip.
  RECT clip = clip_rect();
  clip.left = std::max<LONG>(clip.left, 0);
  clip.top = std::max<LONG>(clip.top, 0);
  clip.right = std::min<LONG>(clip.right, (LONG)target_width_);
  clip.bottom = std::min<LONG>(clip.bottom, (LONG)target_height_);

  std::vector<D3D11_RECT> regions;
  if (!rects) {
    if (clip.right > clip.left && clip.bottom > clip.top)
      regions.push_back(D3D11_RECT{clip.left, clip.top, clip.right, clip.bottom});
  } else if (count) {
    regions.reserve(count);
    for (DWORD at = 0; at < count; at++) {
      const int64_t left = std::max<int64_t>(rects[at].x1, clip.left);
      const int64_t top = std::max<int64_t>(rects[at].y1, clip.top);
      const int64_t right = std::min<int64_t>(rects[at].x2, clip.right);
      const int64_t bottom = std::min<int64_t>(rects[at].y2, clip.bottom);
      if (right <= left || bottom <= top)
        continue;
      regions.push_back(D3D11_RECT{(LONG)left, (LONG)top, (LONG)right, (LONG)bottom});
    }
  }
  // every named rectangle was empty, or the call named rectangles but no count: there is nothing to clear, and the
  // target is as the call found it
  if (regions.empty())
    return S_OK;
  const UINT region_count = (UINT)regions.size();
  const D3D11_RECT *region_data = regions.data();
  // a single region that covers the target is the plain clear of the whole view
  const bool whole_target = region_count == 1 && region_data[0].left == 0 && region_data[0].top == 0 &&
                            region_data[0].right == (LONG)target_width_ && region_data[0].bottom == (LONG)target_height_;

  if (flags & D3DCLEAR_TARGET) {
    float rgba[4] = {channel(colour, 16), channel(colour, 8), channel(colour, 0), channel(colour, 24)};
    // D3DRS_SRGBWRITEENABLE converts the clear colour to sRGB, even on a target the view is not an sRGB one for (Wine
    // visual.c clear_test: 0x7f clears to 0xbb with the state on)
    DWORD srgb_write = FALSE;
    if (SUCCEEDED(state_.render().get(D3DRS_SRGBWRITEENABLE, &srgb_write)) && srgb_write) {
      rgba[0] = kSrgbByte[(colour >> 16) & 0xFF] / 255.0f;
      rgba[1] = kSrgbByte[(colour >> 8) & 0xFF] / 255.0f;
      rgba[2] = kSrgbByte[colour & 0xFF] / 255.0f;
    }
    // a clear clears every render target that is set, not only slot 0 (Wine visual.c multiple_rendertargets_test
    // clears a 16 by 16 texture in both slots and expects both to hold the colour)
    for (UINT at = 0; at < render_target_count_ && at < kMaxRenderTargets; at++) {
      if (!render_target_views_[at])
        continue;
      if (whole_target || !context1_)
        context_->ClearRenderTargetView(render_target_views_[at].Get(), rgba);
      else
        context1_->ClearView(render_target_views_[at].Get(), rgba, region_data, region_count);
    }
  }
  if (dsv && (clear_depth || clear_stencil)) {
    if (whole_target) {
      UINT surfaces = 0;
      if (clear_depth)
        surfaces |= D3D11_CLEAR_DEPTH;
      if (clear_stencil)
        surfaces |= D3D11_CLEAR_STENCIL;
      context_->ClearDepthStencilView(dsv, surfaces, depth, (UINT8)stencil);
    } else {
      // the depth of the rectangles, through the backend's clear of part of a view (ID3D11DeviceContext1::ClearView,
      // whose depth-stencil branch clears the depth of each rectangle it is given, at the depth it is given)
      if (clear_depth) {
        if (context1_) {
          context1_->ClearView(dsv, &depth, region_data, region_count);
        } else {
          ERR_ONCE("D3D9: clearing the depth of part of a surface needs the context's ClearView, which this device's "
                   "Direct3D 11 context has not.");
        }
      }
      // a rectangle's stencil: ClearView writes a DSV's depth, not its stencil, so the stencil of each rectangle is a
      // scissored draw that always replaces (Microsoft Learn, IDirect3DDevice9::Clear)
      if (clear_stencil)
        clear_stencil_rects(region_data, region_count, (UINT8)stencil);
    }
  }
  return S_OK;
}

// whether the device can draw with the state it has, and in how many passes: the fixed-function path draws every state
// it accepts in one, and a device with no target to draw into cannot draw at all
HRESULT
MTLD3D9Device::ValidateDevice(DWORD *passes) {
  if (!passes)
    return D3DERR_INVALIDCALL;
  HRESULT hr = ensure_render_target();
  if (FAILED(hr))
    return D3DERR_INVALIDCALL;
  *passes = 1;
  return S_OK;
}

HRESULT
MTLD3D9Device::GetRenderTarget(DWORD index, IDirect3DSurface9 **surface) {
  if (index >= kMaxRenderTargets || !surface)
    return D3DERR_INVALIDCALL;
  *surface = nullptr;
  HRESULT hr = ensure_render_target();
  if (FAILED(hr))
    return hr;
  // a slot the application has not set (or has cleared) has no target: D3D9 answers D3DERR_NOTFOUND, and reserves
  // D3DERR_INVALIDCALL for an index past NumSimultaneousRTs (Microsoft Learn, IDirect3DDevice9::GetRenderTarget)
  if (index >= render_target_count_ || !render_targets_[index])
    return D3DERR_NOTFOUND;
  *surface = render_targets_[index];
  (*surface)->AddRef();
  return S_OK;
}

// a surface SetRenderTarget or SetDepthStencilSurface can take: a texture this device draws into, or tests against
HRESULT
MTLD3D9Device::CreateRenderTarget(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multisample,
                                  DWORD multisample_quality, WINBOOL lockable, IDirect3DSurface9 **surface,
                                  HANDLE *shared_handle) {
  if (!surface || !width || !height)
    return D3DERR_INVALIDCALL;
  if (shared_handle)
    return extended_ ? D3DERR_INVALIDCALL : E_NOTIMPL;
  *surface = nullptr;
  // a lockable target is single-sampled: D3D9 refuses a multisampled one the CPU could lock (Microsoft Learn,
  // IDirect3DDevice9::CreateRenderTarget: "Lockable ... must be FALSE for a multisampled render target")
  if (lockable && multisample != D3DMULTISAMPLE_NONE)
    return D3DERR_INVALIDCALL;
  // a target is a default-pool resource of the device the frames failed on, and a Reset replaces that device
  if (HRESULT lost = lost_reason(); FAILED(lost))
    return lost;
  const DXGI_FORMAT dxgi_format = to_dxgi_format(format);
  if (dxgi_format == DXGI_FORMAT_UNKNOWN)
    return D3DERR_INVALIDCALL;
  UINT sample_count = 1, sample_quality = 0;
  if (multisample != D3DMULTISAMPLE_NONE) {
    UINT quality_levels = 0;
    // Unsupported sample counts (Wine device.c test_invalid_multisample: D3DMULTISAMPLE_15_SAMPLES) and quality
    // indices >= CheckDeviceMultiSampleType's levels are D3DERR_INVALIDCALL, not a leaked surface.
    if (FAILED(d3d11_device_->CheckMultisampleQualityLevels(dxgi_format, (UINT)multisample, &quality_levels)) ||
        !quality_levels || multisample_quality >= quality_levels)
      return D3DERR_INVALIDCALL;
    sample_count = (UINT)multisample;
    sample_quality = multisample_quality;
  }

  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = dxgi_format;
  desc.SampleDesc.Count = sample_count;
  desc.SampleDesc.Quality = sample_quality;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  ComPtr<ID3D11Texture2D> texture;
  if (FAILED(d3d11_device_->CreateTexture2D(&desc, nullptr, &texture)))
    return D3DERR_INVALIDCALL;

  // a lockable target also has the CPU's copy its lock goes through: a staging texture of the same size and format
  ComPtr<ID3D11Texture2D> staging;
  if (lockable) {
    D3D11_TEXTURE2D_DESC staging_desc = desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
    if (FAILED(d3d11_device_->CreateTexture2D(&staging_desc, nullptr, &staging)))
      return D3DERR_INVALIDCALL;
  }

  ComPtr<MTLD3D9Surface> result;
  result.Attach(new MTLD3D9Surface(
      static_cast<IDirect3DDevice9 *>(this), texture, format, D3DPOOL_DEFAULT, false, context_, D3DUSAGE_RENDERTARGET,
      false, nullptr, 0, nullptr, 0, nullptr, 0, false, true
  ));
  if (staging)
    result->set_render_target_staging(staging);
  *surface = result.Detach();
  return S_OK;
}

HRESULT
MTLD3D9Device::CreateDepthStencilSurface(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multisample,
                                         DWORD quality, WINBOOL discard, IDirect3DSurface9 **surface,
                                         HANDLE *shared_handle) {
  return create_depth_stencil_surface(width, height, format, multisample, quality, discard, 0, surface, shared_handle);
}

HRESULT
MTLD3D9Device::create_depth_stencil_surface(UINT width, UINT height, D3DFORMAT format,
                                            D3DMULTISAMPLE_TYPE multisample, DWORD multisample_quality, WINBOOL,
                                            DWORD extra_usage, IDirect3DSurface9 **surface,
                                            HANDLE *shared_handle, bool implicit) {
  if (!surface || !width || !height)
    return D3DERR_INVALIDCALL;
  if (shared_handle)
    return extended_ ? D3DERR_INVALIDCALL : E_NOTIMPL;
  *surface = nullptr;
  if (HRESULT lost = lost_reason(); FAILED(lost))
    return lost;
  const DXGI_FORMAT dxgi_format = to_dxgi_depth_format(format);
  if (dxgi_format == DXGI_FORMAT_UNKNOWN) {
    ERR_ONCE("D3D9: this build has no depth-stencil surface of that format.");
    return D3DERR_INVALIDCALL;
  }
  UINT sample_count = 1;
  if (multisample != D3DMULTISAMPLE_NONE) {
    UINT quality_levels = 0;
    if (FAILED(d3d11_device_->CheckMultisampleQualityLevels(dxgi_format, (UINT)multisample, &quality_levels)) ||
        !quality_levels || multisample_quality >= quality_levels)
      return D3DERR_INVALIDCALL;
    sample_count = (UINT)multisample;
  }

  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = dxgi_format;
  desc.SampleDesc.Count = sample_count;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  ComPtr<ID3D11Texture2D> texture;
  if (FAILED(d3d11_device_->CreateTexture2D(&desc, nullptr, &texture)))
    return D3DERR_INVALIDCALL;

  ComPtr<MTLD3D9Surface> result;
  result.Attach(new MTLD3D9Surface(
      static_cast<IDirect3DDevice9 *>(this), texture, format, D3DPOOL_DEFAULT, false, context_,
      D3DUSAGE_DEPTHSTENCIL | extra_usage, true, nullptr, 0, static_cast<IDirect3DDevice9 *>(this), 0, nullptr, 0, true,
      !implicit
  ));
  if (implicit)
    result->set_implicit_surface();
  *surface = result.Detach();
  return S_OK;
}

// D3D9's NumSimultaneousRTs targets: a pixel shader writes oC0 to slot 0 and oC3 to slot 3 (Microsoft Learn,
// IDirect3DDevice9::SetRenderTarget and D3DCAPS9::NumSimultaneousRTs). The slots are independent: slot 2 may be set
// with slot 1 empty, and clearing slot 1 leaves slot 2's target bound. Slot 0 is the frame's target and cannot be
// empty, and every target has slot 0's size (the depth-stencil surface may be larger, and is clipped to it).
HRESULT
MTLD3D9Device::SetRenderTarget(DWORD index, IDirect3DSurface9 *surface) {
  if (index >= kMaxRenderTargets)
    return D3DERR_INVALIDCALL;
  // slot 0 exists whatever the call does, and is what every other slot is measured against
  if (HRESULT hr = ensure_render_target(); FAILED(hr))
    return hr;
  // a higher slot is disabled by clearing it; the slots above it keep their targets
  if (!surface) {
    if (index == 0)
      return D3DERR_INVALIDCALL;
    release_render_target(index);
    render_target_views_[index].Reset();
    while (render_target_count_ > 1 && !render_target_views_[render_target_count_ - 1])
      render_target_count_--;
    return S_OK;
  }
  ComPtr<IMTLD3D9Surface> ours;
  if (FAILED(device_surface(surface, false, &ours)))
    return D3DERR_INVALIDCALL;
  // Offscreen plains and other non-RT surfaces may be GPU-writable but must not answer SetRenderTarget
  // (Wine device.c test_resource_access: Usage must include D3DUSAGE_RENDERTARGET).
  D3DSURFACE_DESC surface_desc{};
  if (FAILED(surface->GetDesc(&surface_desc)) || !(surface_desc.Usage & D3DUSAGE_RENDERTARGET))
    return D3DERR_INVALIDCALL;

  D3D11_TEXTURE2D_DESC desc, depth_desc;
  ours->texture()->GetDesc(&desc);
  // a target's own level: a level of a texture is as large as the chain has it, not the texture's top level
  const UINT level = ours->level();
  const UINT target_width = std::max(1u, desc.Width >> level);
  const UINT target_height = std::max(1u, desc.Height >> level);
  // slot 0 defines the set's size (a new slot 0 resets the slots above it, whose views are of the old size); every
  // other slot is slot 0's size
  if (index != 0 && (target_width != target_width_ || target_height != target_height_)) {
    ERR_ONCE("D3D9: a render target of another size than the first one is not taken.");
    return D3DERR_INVALIDCALL;
  }
  // the depth-stencil surface may be larger or smaller than the incoming target (Wine device.c sets a 128x128
  // depth surface while the back buffer is larger); sample counts must still match. The incoming target's
  // dimensions are the ones validated, not the slot-0 target being replaced
  // a multisampled target is taken over the automatic single-sample depth surface: D3D9 accepts it (Wine visual.c
  // test_multisample_stretch_rect sets a 2-sample target while the device's depth is single-sample), and the pair is
  // bound for a draw only when the counts agree (bound_depth_stencil). Two multisampled surfaces of different counts
  // are refused.
  if (depth_stencil_) {
    depth_stencil_->texture()->GetDesc(&depth_desc);
    if (depth_desc.SampleDesc.Count > 1 && desc.SampleDesc.Count > 1 &&
        depth_desc.SampleDesc.Count != desc.SampleDesc.Count) {
      ERR_ONCE("D3D9: a depth-stencil surface with another sample count than the render target is not taken.");
      return D3DERR_INVALIDCALL;
    }
  }
  // every target of the set has slot 0's sample count, and a new slot 0 brings its own
  if (index != 0 && desc.SampleDesc.Count > 1 && target_samples_ > 1 && desc.SampleDesc.Count != target_samples_) {
    ERR_ONCE("D3D9: a render target with another sample count than the first one is not taken.");
    return D3DERR_INVALIDCALL;
  }

  // the view of that level, not of the top one: a render target of a mipmapped texture draws into the level its surface
  // is, at the level's size
  D3D11_RENDER_TARGET_VIEW_DESC view_desc{};
  view_desc.Format = desc.Format;
  if (desc.SampleDesc.Count > 1) {
    view_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
  } else {
    view_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    view_desc.Texture2D.MipSlice = level;
  }
  ComPtr<ID3D11RenderTargetView> view;
  if (FAILED(d3d11_device_->CreateRenderTargetView(ours->texture(), &view_desc, &view)))
    return D3DERR_INVALIDCALL;
  release_render_target(index);
  // Application-created targets need a private ref while bound so public Release cannot free them mid-draw.
  // Implicit surfaces (device back buffer, additional-chain buffers, auto DS) stay alive via their owner until
  // destroy_implicit — a private ref must not be the thing that deletes them on unbind (Wine stateblock
  // rendertarget_switch: Release additional chain after SetRenderTarget back to the original).
  if (!ours->implicit_surface() && surface != static_cast<IDirect3DSurface9 *>(back_buffer_surface_)) {
    ours->add_private_ref();
    render_target_objects_[index] = ours.Get();
    render_target_held_[index] = true;
  }
  render_targets_[index] = surface;
  render_target_views_[index] = view;
  if (render_target_count_ <= index)
    render_target_count_ = index + 1;
  if (index != 0)
    return S_OK;
  if (target_width_ != target_width || target_height_ != target_height || target_samples_ != desc.SampleDesc.Count) {
    // the higher slots were views of the old size or sample count, and D3D11 needs every target of a draw to match
    for (UINT at = 1; at < kMaxRenderTargets; at++) {
      release_render_target(at);
      render_target_views_[at].Reset();
    }
    render_target_count_ = 1;
  }
  target_width_ = target_width;
  target_height_ = target_height;
  target_samples_ = desc.SampleDesc.Count;
  // D3D11 OMSetRenderTargets requires the depth-stencil size to match the RT. D3D9 allows a mismatched DS (Wine
  // device.c occlusion: large RT with the implicit desktop-sized DS) and keeps it bound. The binding stays; whether it
  // can be bound with the current targets is bound_depth_stencil()'s answer, so a target of the matching size again
  // gets the depth test back rather than having lost it
  // the new target is the whole of its own scissor until the application sets one for it
  scissor_rect_ = RECT{0, 0, (LONG)target_width, (LONG)target_height};
  // a target brings its own viewport: D3D9 places it on the target SetRenderTarget set (Wine's
  // device_reset_viewport_state, dlls/d3d9/device.c), so a game that switches targets and draws without another
  // SetViewport draws over the new target rather than over the old one's place in it
  viewport_ = D3DVIEWPORT9{0, 0, target_width, target_height, 0.0f, 1.0f};
  return S_OK;
}

HRESULT
MTLD3D9Device::SetDepthStencilSurface(IDirect3DSurface9 *surface) {
  if (!surface) {
    clear_depth_stencil();
    depth_stencil_view_.Reset();
    return S_OK;
  }
  ComPtr<IMTLD3D9Surface> ours;
  if (FAILED(device_surface(surface, true, &ours)))
    return D3DERR_INVALIDCALL;
  HRESULT hr = ensure_render_target();
  if (FAILED(hr))
    return hr;

  D3D11_TEXTURE2D_DESC desc;
  ours->texture()->GetDesc(&desc);
  // larger or smaller than the render target is allowed (Wine device.c; the viewport clips the intersection)
  // a depth-stencil surface is bound to the target set, so its sample count must be the set's: SetRenderTarget
  // checks the other order (a target given while this depth surface is set), and both leave the device as it was
  if (desc.SampleDesc.Count > 1 && target_samples_ > 1 && desc.SampleDesc.Count != target_samples_) {
    ERR_ONCE("D3D9: a depth-stencil surface with another sample count than the render target is not taken.");
    return D3DERR_INVALIDCALL;
  }

  // a depth texture is typeless so that it can be sampled as well, so the view names the concrete depth format
  D3DSURFACE_DESC bound_desc{};
  if (FAILED(surface->GetDesc(&bound_desc)))
    return D3DERR_INVALIDCALL;
  D3D11_DEPTH_STENCIL_VIEW_DESC depth_view_desc{};
  depth_view_desc.Format = to_dxgi_depth_format(bound_desc.Format);
  if (depth_view_desc.Format == DXGI_FORMAT_UNKNOWN)
    depth_view_desc.Format = desc.Format;
  if (desc.SampleDesc.Count > 1) {
    depth_view_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
  } else {
    depth_view_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    depth_view_desc.Texture2D.MipSlice = ours->level();
  }
  ComPtr<ID3D11DepthStencilView> view;
  if (FAILED(d3d11_device_->CreateDepthStencilView(ours->texture(), &depth_view_desc, &view)))
    return D3DERR_INVALIDCALL;
  clear_depth_stencil();
  depth_stencil_ = static_cast<MTLD3D9Surface *>(ours.Get());
  // an application-created depth-stencil surface is kept alive by a private reference while it is bound; the automatic
  // one is the device's implicit surface, made at reference count 0 and freed with the device
  if (!depth_stencil_->implicit_surface()) {
    depth_stencil_->add_private_ref();
    depth_stencil_held_ = true;
  }
  depth_stencil_view_ = view;
  return S_OK;
}

HRESULT
MTLD3D9Device::GetDepthStencilSurface(IDirect3DSurface9 **surface) {
  if (!surface)
    return D3DERR_INVALIDCALL;
  *surface = nullptr;
  if (!depth_stencil_)
    return D3DERR_NOTFOUND;
  *surface = static_cast<IDirect3DSurface9 *>(depth_stencil_);
  (*surface)->AddRef();
  return S_OK;
}

HRESULT
MTLD3D9Device::SetViewport(const D3DVIEWPORT9 *viewport) {
  if (!viewport)
    return D3DERR_INVALIDCALL;
  HRESULT hr = ensure_render_target();
  if (FAILED(hr))
    return hr;
  // Wine device.c SetViewport validates nothing but the pointer: the viewport is kept whole — extents past the
  // target, an empty one and an inverted Z range are all kept, and the raster clips the draw at the target (Wine
  // device.c test_set_viewport sets one as wide as the maximum texture, over a much smaller one)
  viewport_ = *viewport;
  if (recording_)
    record(StateKey{StateKey::Viewport, 0, 0});
  return S_OK;
}

HRESULT
MTLD3D9Device::GetViewport(D3DVIEWPORT9 *viewport) {
  if (!viewport)
    return D3DERR_INVALIDCALL;
  *viewport = viewport_;
  return S_OK;
}

HRESULT
MTLD3D9Device::SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX *matrix) {
  if (!matrix)
    return D3DERR_INVALIDCALL;
  D3DMATRIX *slot = state_.transform(state);
  if (!slot) {
    ERR_ONCE("D3D9: only the 256 world matrices, the view, the projection and texture 0..7 transforms are kept.");
    return D3DERR_INVALIDCALL;
  }
  *slot = *matrix;
  if (recording_)
    record(StateKey{StateKey::Transform, (UINT)state, 0});
  return S_OK;
}

HRESULT
MTLD3D9Device::GetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX *matrix) {
  if (!matrix)
    return D3DERR_INVALIDCALL;
  const D3DMATRIX *slot = state_.transform(state);
  if (!slot) {
    ERR_ONCE("D3D9: only the 256 world matrices, the view, the projection and texture 0..7 transforms are kept.");
    return D3DERR_INVALIDCALL;
  }
  *matrix = *slot;
  return S_OK;
}

// D3D9's world * matrix: the device's matrix and the one given are multiplied, so a vertex is transformed by both, the
// device's first (IDirect3DDevice9::MultiplyTransform, "this method multiplies the world matrix by the specified
// matrix"). The transform is a row vector's, so the product is the device's matrix times the given one.
HRESULT
MTLD3D9Device::MultiplyTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX *matrix) {
  if (!matrix)
    return D3DERR_INVALIDCALL;
  if (!state_.transform(state)) {
    ERR_ONCE("D3D9: only the 256 world matrices, the view, the projection and texture 0..7 transforms are kept.");
    return D3DERR_INVALIDCALL;
  }
  // Wine device.c test_multiply_transform: MultiplyTransform writes the primary device state even during
  // BeginStateBlock recording and is not captured into the recorded block (unlike SetTransform).
  state_.multiply(state, *matrix);
  return S_OK;
}

// D3D9's scissor rectangle: the part of the target D3DRS_SCISSORTESTENABLE clips every draw to. It cannot name a pixel
// outside the target, which is the only validity D3D9 gives it; an empty rectangle clips everything (Microsoft Learn,
// IDirect3DDevice9::SetScissorRect).
HRESULT
MTLD3D9Device::SetScissorRect(const RECT *rect) {
  if (!rect)
    return D3DERR_INVALIDCALL;
  HRESULT hr = ensure_render_target();
  if (FAILED(hr))
    return hr;
  if (rect->left < 0 || rect->top < 0 || rect->right < rect->left || rect->bottom < rect->top ||
      rect->right > (LONG)target_width_ || rect->bottom > (LONG)target_height_) {
    ERR_ONCE("D3D9: that scissor rectangle is not one of the target's; the device keeps the one it had.");
    return D3DERR_INVALIDCALL;
  }
  scissor_rect_ = *rect;
  return S_OK;
}

HRESULT
MTLD3D9Device::GetScissorRect(RECT *rect) {
  if (!rect)
    return D3DERR_INVALIDCALL;
  *rect = scissor_rect_;
  return S_OK;
}

// the rectangle a draw is clipped to: the viewport, narrowed by the scissor rectangle while D3DRS_SCISSORTESTENABLE is on.
// D3D11's scissor is what performs both clips, since a viewport there places a draw but does not clip it.
RECT
MTLD3D9Device::clip_rect() const {
  RECT clip{(LONG)viewport_.X, (LONG)viewport_.Y, (LONG)(viewport_.X + viewport_.Width),
            (LONG)(viewport_.Y + viewport_.Height)};
  if (!state_.render().scissor_test_enabled())
    return clip;
  clip.left = std::max(clip.left, scissor_rect_.left);
  clip.top = std::max(clip.top, scissor_rect_.top);
  clip.right = std::min(clip.right, scissor_rect_.right);
  clip.bottom = std::min(clip.bottom, scissor_rect_.bottom);
  // an empty intersection is an empty rectangle at its left/top corner: D3D11 refuses one whose right is before its left
  clip.right = std::max(clip.right, clip.left);
  clip.bottom = std::max(clip.bottom, clip.top);
  return clip;
}

HRESULT
MTLD3D9Device::SetRenderState(D3DRENDERSTATETYPE state, DWORD value) {
  if (!RenderState::supported(state)) {
    ERR_ONCE("D3D9: render state {} is not honored yet; the call is refused rather than ignored.", (int)state);
    return D3DERR_INVALIDCALL;
  }
  if (!RenderState::usable_value(state, value)) {
    ERR_ONCE("D3D9: render state {} cannot take {}; the device keeps the value it had.", (int)state, value);
    return D3DERR_INVALIDCALL;
  }
  const HRESULT hr = state_.render().set(state, value);
  if (SUCCEEDED(hr) && recording_)
    record(StateKey{StateKey::Render, (UINT)state, 0});
  return hr;
}

HRESULT
MTLD3D9Device::GetRenderState(D3DRENDERSTATETYPE state, DWORD *value) {
  if (!value || !RenderState::supported(state))
    return D3DERR_INVALIDCALL;
  return state_.render().get(state, value);
}

// the state a block being recorded carries: every setter that succeeds while the device is in recording mode adds its
// state here, once (a state set twice is one state, with the second value, which the block reads from the device)
void
MTLD3D9Device::record(StateKey key) {
  StateValue value;
  // read_key reads the device's current value (what the setter just applied).
  const bool captured = recording_snapshot_ && SUCCEEDED(recording_snapshot_->read_key(key, value));
  auto restore = [&] {
    if (!recording_snapshot_)
      return;
    const bool was_recording = recording_;
    recording_ = false;
    recording_snapshot_->Apply();
    // Apply skips a NULL vertex declaration; the BeginStateBlock snapshot still needs the device cleared back to
    // that NULL (Wine stateblock.c abort chain expects DEFAULT after End).
    StateValue decl;
    if (SUCCEEDED(recording_snapshot_->stored_key(StateKey{StateKey::Declaration, 0, 0}, decl)) && !decl.object)
      SetVertexDeclaration(nullptr);
    recording_ = was_recording;
  };
  for (size_t at = 0; at < recorded_keys_.size(); at++) {
    if (recorded_keys_[at] == key) {
      if (captured)
        recorded_values_[at] = value;
      restore();
      return;
    }
  }
  recorded_keys_.push_back(key);
  recorded_values_.push_back(value);
  restore();
}

// a block of the state `type` names, captured now: D3D9's CreateStateBlock captures as it makes the block
HRESULT
MTLD3D9Device::CreateStateBlock(D3DSTATEBLOCKTYPE type, IDirect3DStateBlock9 **block) {
  if (!block)
    return D3DERR_INVALIDCALL;
  *block = nullptr;
  if (type != D3DSBT_ALL && type != D3DSBT_PIXELSTATE && type != D3DSBT_VERTEXSTATE)
    return D3DERR_INVALIDCALL;
  if (recording_) {
    ERR_ONCE("D3D9: a state block cannot be captured while the device is recording one (BeginStateBlock).");
    return D3DERR_INVALIDCALL;
  }
  ComPtr<MTLD3D9StateBlock> result;
  result.Attach(new MTLD3D9StateBlock(this, type));
  const HRESULT hr = result->capture_all();
  if (FAILED(hr))
    return hr;
  *block = result.Detach();
  return S_OK;
}

// D3D9's BeginStateBlock/EndStateBlock: the device records the states the calls between them set, and hands back a
// block of them. the calls still change the device as they always do, which is what D3D9's recording mode does
HRESULT
MTLD3D9Device::BeginStateBlock() {
  if (recording_)
    return D3DERR_INVALIDCALL;
  // Recording a block is not applying one: EndStateBlock returns the device to this snapshot after it has read the
  // values the recording collected. Without the snapshot, the recorded test writes would remain on the live device.
  ComPtr<MTLD3D9StateBlock> snapshot;
  snapshot.Attach(new MTLD3D9StateBlock(this, D3DSBT_ALL, false));
  const HRESULT hr = snapshot->capture_all();
  if (FAILED(hr))
    return hr;
  recording_snapshot_.Attach(snapshot.Detach());
  recording_ = true;
  recorded_keys_.clear();
  recorded_values_.clear();
  return S_OK;
}

HRESULT
MTLD3D9Device::EndStateBlock(IDirect3DStateBlock9 **block) {
  if (!block)
    return D3DERR_INVALIDCALL;
  // Failed EndStateBlock must leave *block unchanged (Wine device.c passes 0xdeadbeef).
  if (!recording_)
    return D3DERR_INVALIDCALL;
  *block = nullptr;
  recording_ = false;
  std::vector<StateKey> keys = std::move(recorded_keys_);
  std::vector<StateValue> values = std::move(recorded_values_);
  recorded_keys_.clear();
  recorded_values_.clear();
  ComPtr<MTLD3D9StateBlock> snapshot = std::move(recording_snapshot_);
  ComPtr<MTLD3D9StateBlock> result;
  result.Attach(new MTLD3D9StateBlock(this, D3DSBT_ALL));
  const HRESULT hr = result->capture_recorded(keys, values);
  if (FAILED(hr)) {
    if (snapshot)
      snapshot->Apply();
    return hr;
  }
  // Every recorded setter already restored the snapshot after capturing. A final Apply would also try to undo
  // side effects of setters that are not state-block state (SetRenderTarget's viewport/scissor): Wine's
  // device_reset_viewport_state test expects those to remain, and Apply of a 640x480 viewport onto a 128x128
  // target fails and used to crash the suite on the subsequent Release of a null block pointer.
  snapshot.Reset();
  *block = result.Detach();
  return S_OK;
}

// an event query, whose GetData says the GPU has finished what was issued before it: the call a title makes at the end
// of its frame and waits on (d3d9_query.cpp)
HRESULT
MTLD3D9Device::CreateQuery(D3DQUERYTYPE type, IDirect3DQuery9 **query) {
  return MTLD3D9Query::create(this, d3d11_device_.Get(), context_.Get(), type, query);
}

HRESULT
MTLD3D9Device::ensure_stencil_fill_target() {
  if (stencil_fill_target_ && stencil_fill_width_ == target_width_ && stencil_fill_height_ == target_height_)
    return S_OK;
  stencil_fill_target_.Reset();
  HRESULT hr = CreateRenderTarget(
      target_width_, target_height_, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &stencil_fill_target_, nullptr
  );
  if (FAILED(hr))
    return hr;
  stencil_fill_width_ = target_width_;
  stencil_fill_height_ = target_height_;
  return S_OK;
}

HRESULT
MTLD3D9Device::clear_stencil_rects(const D3D11_RECT *rects, UINT count, UINT8 stencil) {
  if (!rects || !count || !depth_stencil_view_)
    return S_OK;
  HRESULT hr = ensure_stencil_fill_target();
  if (FAILED(hr))
    return hr;

  // the targets the fill draws over: it writes one, and the set the application had is put back afterwards
  IDirect3DSurface9 *previous_targets[kMaxRenderTargets]{};
  const UINT previous_count = render_target_count_;
  for (UINT at = 0; at < previous_count; at++)
    previous_targets[at] = render_targets_[at];
  DWORD color_write = 0, stencil_enable = 0, stencil_func = 0, stencil_pass = 0, stencil_ref = 0;
  DWORD z_enable = 0, z_func = 0, z_write = 0, cull = 0, fvf = 0;
  RECT scissor{};
  GetRenderState(D3DRS_COLORWRITEENABLE, &color_write);
  GetRenderState(D3DRS_STENCILENABLE, &stencil_enable);
  GetRenderState(D3DRS_STENCILFUNC, &stencil_func);
  GetRenderState(D3DRS_STENCILPASS, &stencil_pass);
  GetRenderState(D3DRS_STENCILREF, &stencil_ref);
  GetRenderState(D3DRS_ZENABLE, &z_enable);
  GetRenderState(D3DRS_ZFUNC, &z_func);
  GetRenderState(D3DRS_ZWRITEENABLE, &z_write);
  GetRenderState(D3DRS_CULLMODE, &cull);
  GetFVF(&fvf);
  GetScissorRect(&scissor);

  for (DWORD at = kMaxRenderTargets - 1; at >= 1; at--)
    SetRenderTarget(at, nullptr);
  SetRenderTarget(0, stencil_fill_target_.Get());
  SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
  SetRenderState(D3DRS_STENCILENABLE, TRUE);
  SetRenderState(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
  SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);
  SetRenderState(D3DRS_STENCILREF, stencil);
  SetRenderState(D3DRS_ZENABLE, TRUE);
  SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
  SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
  SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  SetFVF(kPreTransformedFVF);

  for (UINT at = 0; at < count; at++) {
    const RECT clip{(LONG)rects[at].left, (LONG)rects[at].top, (LONG)rects[at].right, (LONG)rects[at].bottom};
    SetScissorRect(&clip);
    PreTransformedVertex verts[4] = {
        {(float)clip.left, (float)clip.top, 0.0f, 1.0f, 0},
        {(float)clip.right, (float)clip.top, 0.0f, 1.0f, 0},
        {(float)clip.left, (float)clip.bottom, 0.0f, 1.0f, 0},
        {(float)clip.right, (float)clip.bottom, 0.0f, 1.0f, 0},
    };
    DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, verts, sizeof(PreTransformedVertex));
  }

  SetRenderTarget(0, previous_targets[0]);
  for (UINT at = 1; at < previous_count; at++)
    SetRenderTarget(at, previous_targets[at]);
  SetRenderState(D3DRS_COLORWRITEENABLE, color_write);
  SetRenderState(D3DRS_STENCILENABLE, stencil_enable);
  SetRenderState(D3DRS_STENCILFUNC, stencil_func);
  SetRenderState(D3DRS_STENCILPASS, stencil_pass);
  SetRenderState(D3DRS_STENCILREF, stencil_ref);
  SetRenderState(D3DRS_ZENABLE, z_enable);
  SetRenderState(D3DRS_ZFUNC, z_func);
  SetRenderState(D3DRS_ZWRITEENABLE, z_write);
  SetRenderState(D3DRS_CULLMODE, cull);
  SetFVF(fvf);
  SetScissorRect(&scissor);
  return S_OK;
}

} // namespace mullion::d3d9
