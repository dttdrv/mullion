// the render states a frame is drawn under and the targets it is drawn into: SetRenderState and GetRenderState over the
// states this fixed-function path can honour (d3d9_state.cpp turns their values into the D3D11 state objects a draw
// binds), the depth-stencil surfaces, the render targets, and the viewport a draw is placed and clipped by.
#include "d3d9_device.hpp"

namespace mullion::d3d9 {

namespace {

float channel(D3DCOLOR colour, unsigned shift) { return (float)((colour >> shift) & 0xFF) / 255.0f; }

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
  *out = ours.Detach();
  return S_OK;
}

HRESULT
MTLD3D9Device::Clear(DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR colour, float depth, DWORD stencil) {
  if (!(flags & (D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)))
    return S_OK;

  HRESULT hr = ensure_render_target();
  if (FAILED(hr))
    return hr;
  if (count && rects) {
    ERR_ONCE("D3D9: clearing part of a surface is not implemented yet; the whole of it is cleared.");
  }
  if (flags & D3DCLEAR_TARGET) {
    const float rgba[4] = {channel(colour, 16), channel(colour, 8), channel(colour, 0), channel(colour, 24)};
    context_->ClearRenderTargetView(current_target_view_.Get(), rgba);
  }
  // a device made without a depth-stencil surface has nothing to clear of one, as D3D9's has not
  UINT surfaces = 0;
  if ((flags & D3DCLEAR_ZBUFFER) && depth_stencil_view_)
    surfaces |= D3D11_CLEAR_DEPTH;
  if ((flags & D3DCLEAR_STENCIL) && depth_stencil_view_)
    surfaces |= D3D11_CLEAR_STENCIL;
  if (surfaces)
    context_->ClearDepthStencilView(depth_stencil_view_.Get(), surfaces, depth, (UINT8)stencil);
  return S_OK;
}

HRESULT
MTLD3D9Device::GetRenderTarget(DWORD index, IDirect3DSurface9 **surface) {
  if (index != 0 || !surface)
    return D3DERR_INVALIDCALL;
  *surface = nullptr;
  HRESULT hr = ensure_render_target();
  if (FAILED(hr))
    return hr;
  *surface = current_target_.Get();
  (*surface)->AddRef();
  return S_OK;
}

// a surface SetRenderTarget or SetDepthStencilSurface can take: a texture this device draws into, or tests against
HRESULT
MTLD3D9Device::CreateRenderTarget(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multisample,
                                  DWORD, WINBOOL lockable, IDirect3DSurface9 **surface, HANDLE *shared_handle) {
  if (!surface || shared_handle || !width || !height)
    return D3DERR_INVALIDCALL;
  *surface = nullptr;
  if (multisample != D3DMULTISAMPLE_NONE)
    return D3DERR_NOTAVAILABLE;
  if (lockable) {
    ERR_ONCE("D3D9: a render target whose bytes the CPU locks is not implemented yet.");
    return D3DERR_INVALIDCALL;
  }
  const DXGI_FORMAT dxgi_format = to_dxgi_format(format);
  if (dxgi_format == DXGI_FORMAT_UNKNOWN)
    return D3DERR_INVALIDCALL;

  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = dxgi_format;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  ComPtr<ID3D11Texture2D> texture;
  if (FAILED(d3d11_device_->CreateTexture2D(&desc, nullptr, &texture)))
    return D3DERR_INVALIDCALL;

  ComPtr<MTLD3D9Surface> result;
  result.Attach(new MTLD3D9Surface(static_cast<IDirect3DDevice9 *>(this), texture, format, false, context_));
  *surface = result.Detach();
  return S_OK;
}

HRESULT
MTLD3D9Device::CreateDepthStencilSurface(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multisample,
                                         DWORD, WINBOOL, IDirect3DSurface9 **surface, HANDLE *shared_handle) {
  if (!surface || shared_handle || !width || !height)
    return D3DERR_INVALIDCALL;
  *surface = nullptr;
  if (multisample != D3DMULTISAMPLE_NONE)
    return D3DERR_NOTAVAILABLE;
  const DXGI_FORMAT dxgi_format = to_dxgi_depth_format(format);
  if (dxgi_format == DXGI_FORMAT_UNKNOWN) {
    ERR_ONCE("D3D9: this build has no depth-stencil surface of that format.");
    return D3DERR_INVALIDCALL;
  }

  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = dxgi_format;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  ComPtr<ID3D11Texture2D> texture;
  if (FAILED(d3d11_device_->CreateTexture2D(&desc, nullptr, &texture)))
    return D3DERR_INVALIDCALL;

  ComPtr<MTLD3D9Surface> result;
  result.Attach(new MTLD3D9Surface(static_cast<IDirect3DDevice9 *>(this), texture, format, false, context_, true));
  *surface = result.Detach();
  return S_OK;
}

// a target of a size the depth-stencil surface does not have is refused: the two are one target's, and D3D9 refuses
// a pair whose sizes differ rather than drawing with half of one
HRESULT
MTLD3D9Device::SetRenderTarget(DWORD index, IDirect3DSurface9 *surface) {
  if (index != 0)
    return D3DERR_INVALIDCALL;
  ComPtr<IMTLD3D9Surface> ours;
  if (FAILED(device_surface(surface, false, &ours)))
    return D3DERR_INVALIDCALL;

  D3D11_TEXTURE2D_DESC desc, depth_desc;
  ours->texture()->GetDesc(&desc);
  if (depth_stencil_) {
    depth_stencil_->texture()->GetDesc(&depth_desc);
    if (depth_desc.Width != desc.Width || depth_desc.Height != desc.Height) {
      ERR_ONCE("D3D9: a render target of another size than the depth-stencil surface is not taken.");
      return D3DERR_INVALIDCALL;
    }
  }

  ComPtr<ID3D11RenderTargetView> view;
  if (FAILED(d3d11_device_->CreateRenderTargetView(ours->texture(), nullptr, &view)))
    return D3DERR_INVALIDCALL;
  current_target_ = surface;
  current_target_view_ = view;
  target_width_ = desc.Width;
  target_height_ = desc.Height;
  // a target brings its own viewport: D3D9 places it on the target SetRenderTarget set (Wine's
  // device_reset_viewport_state, dlls/d3d9/device.c), so a game that switches targets and draws without another
  // SetViewport draws over the new target rather than over the old one's place in it
  viewport_ = D3DVIEWPORT9{0, 0, desc.Width, desc.Height, 0.0f, 1.0f};
  return S_OK;
}

HRESULT
MTLD3D9Device::SetDepthStencilSurface(IDirect3DSurface9 *surface) {
  if (!surface) {
    depth_stencil_.Reset();
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
  if (desc.Width != target_width_ || desc.Height != target_height_) {
    ERR_ONCE("D3D9: a depth-stencil surface of another size than the render target is not taken.");
    return D3DERR_INVALIDCALL;
  }

  ComPtr<ID3D11DepthStencilView> view;
  if (FAILED(d3d11_device_->CreateDepthStencilView(ours->texture(), nullptr, &view)))
    return D3DERR_INVALIDCALL;
  depth_stencil_.Attach(static_cast<MTLD3D9Surface *>(ours.Detach()));
  depth_stencil_view_ = view;
  return S_OK;
}

HRESULT
MTLD3D9Device::GetDepthStencilSurface(IDirect3DSurface9 **surface) {
  if (!surface)
    return D3DERR_INVALIDCALL;
  *surface = depth_stencil_.Get();
  if (*surface)
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
  // a viewport is a rectangle of the target, and its depths are the device's 0 to 1
  if (!viewport->Width || !viewport->Height || viewport->X + viewport->Width > target_width_ ||
      viewport->Y + viewport->Height > target_height_ || viewport->MinZ < 0.0f || viewport->MaxZ > 1.0f ||
      viewport->MinZ > viewport->MaxZ) {
    ERR_ONCE("D3D9: that viewport is not one of the target's; the device keeps the one it had.");
    return D3DERR_INVALIDCALL;
  }
  viewport_ = *viewport;
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
MTLD3D9Device::SetRenderState(D3DRENDERSTATETYPE state, DWORD value) {
  if (!RenderState::supported(state)) {
    ERR_ONCE("D3D9: render state {} is not honored yet; the call is refused rather than ignored.", (int)state);
    return D3DERR_INVALIDCALL;
  }
  if (!RenderState::usable_value(state, value)) {
    ERR_ONCE("D3D9: render state {} cannot take {}; the device keeps the value it had.", (int)state, value);
    return D3DERR_INVALIDCALL;
  }
  return render_state_.set(state, value);
}

HRESULT
MTLD3D9Device::GetRenderState(D3DRENDERSTATETYPE state, DWORD *value) {
  if (!value || !RenderState::supported(state))
    return D3DERR_INVALIDCALL;
  return render_state_.get(state, value);
}

} // namespace mullion::d3d9
