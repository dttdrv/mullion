// the render states a frame is drawn under and the targets it is drawn into: SetRenderState and GetRenderState over the
// states this fixed-function path can honour (d3d9_state.cpp turns their values into the D3D11 state objects a draw
// binds), the depth-stencil surfaces, the render targets, and the viewport a draw is placed and clipped by.
#include "d3d9_device.hpp"
#include <vector>

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

// a clear of the whole target or of the rectangles the call names, as D3D9's is: `count` rectangles of `rects`, each
// clamped to the target, and a rectangle that names no pixel clears none. no rectangles at all is the whole of it.
// colour and depth have a D3D11 clear of a part of a view (ID3D11DeviceContext1::ClearView); the stencil of part of a
// surface has none, so a rectangle's stencil bits are left as they were and the log says so, rather than the whole
// surface's being cleared as if the call had named no rectangle (the rest of d3d9_device_targets.cpp's Clear).
HRESULT
MTLD3D9Device::Clear(DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR colour, float depth, DWORD stencil) {
  if (HRESULT lost = lost_reason(); FAILED(lost))
    return lost;
  if (!(flags & (D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)))
    return S_OK;
  if (count && !rects)
    return D3DERR_INVALIDCALL;

  HRESULT hr = ensure_render_target();
  if (FAILED(hr))
    return hr;

  // the rectangles, clamped to the target: a right or bottom edge past it is the target's own
  std::vector<D3D11_RECT> regions;
  if (count) {
    regions.reserve(count);
    for (DWORD at = 0; at < count; at++) {
      const int64_t left = std::max<int64_t>(rects[at].x1, 0), top = std::max<int64_t>(rects[at].y1, 0);
      const int64_t right = std::min<int64_t>(rects[at].x2, target_width_);
      const int64_t bottom = std::min<int64_t>(rects[at].y2, target_height_);
      if (right <= left || bottom <= top)
        continue;
      regions.push_back(D3D11_RECT{(LONG)left, (LONG)top, (LONG)right, (LONG)bottom});
    }
    // every named rectangle was empty: there is nothing to clear, and the target is as the call found it
    if (regions.empty())
      return S_OK;
  }
  const UINT region_count = (UINT)regions.size();
  const D3D11_RECT *region_data = regions.empty() ? nullptr : regions.data();

  if (flags & D3DCLEAR_TARGET) {
    const float rgba[4] = {channel(colour, 16), channel(colour, 8), channel(colour, 0), channel(colour, 24)};
    if (region_count && context1_)
      context1_->ClearView(current_target_view_.Get(), rgba, region_data, region_count);
    else
      context_->ClearRenderTargetView(current_target_view_.Get(), rgba);
  }
  // a device made without a depth-stencil surface has nothing to clear of one, as D3D9's has not
  if (depth_stencil_view_) {
    const bool clear_depth = (flags & D3DCLEAR_ZBUFFER) != 0;
    const bool clear_stencil = (flags & D3DCLEAR_STENCIL) != 0;
    if (!region_count) {
      UINT surfaces = 0;
      if (clear_depth)
        surfaces |= D3D11_CLEAR_DEPTH;
      if (clear_stencil)
        surfaces |= D3D11_CLEAR_STENCIL;
      if (surfaces)
        context_->ClearDepthStencilView(depth_stencil_view_.Get(), surfaces, depth, (UINT8)stencil);
    } else if (clear_depth || clear_stencil) {
      // the depth of the rectangles, through the backend's clear of part of a view (ID3D11DeviceContext1::ClearView,
      // whose depth-stencil branch clears the depth of each rectangle it is given, at the depth it is given)
      if (clear_depth && context1_) {
        context1_->ClearView(depth_stencil_view_.Get(), &depth, region_data, region_count);
      } else if (clear_depth) {
        {
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
  if (lockable) {
    ERR_ONCE("D3D9: a render target whose bytes the CPU locks is not implemented yet.");
    return D3DERR_INVALIDCALL;
  }
  // a target is a default-pool resource of the device the frames failed on, and a Reset replaces that device
  if (HRESULT lost = lost_reason(); FAILED(lost))
    return lost;
  const DXGI_FORMAT dxgi_format = to_dxgi_format(format);
  if (dxgi_format == DXGI_FORMAT_UNKNOWN)
    return D3DERR_INVALIDCALL;
  UINT sample_count = 1, sample_quality = 0;
  if (multisample != D3DMULTISAMPLE_NONE) {
    UINT quality = 0;
    if (FAILED(d3d11_device_->CheckMultisampleQualityLevels(dxgi_format, (UINT)multisample, &quality)) || !quality)
      return D3DERR_NOTAVAILABLE;
    sample_count = (UINT)multisample;
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

  ComPtr<MTLD3D9Surface> result;
  result.Attach(new MTLD3D9Surface(
      static_cast<IDirect3DDevice9 *>(this), texture, format, D3DPOOL_DEFAULT, false, context_, D3DUSAGE_RENDERTARGET
  ));
  *surface = result.Detach();
  return S_OK;
}

HRESULT
MTLD3D9Device::CreateDepthStencilSurface(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multisample,
                                         DWORD, WINBOOL, IDirect3DSurface9 **surface, HANDLE *shared_handle) {
  if (!surface || shared_handle || !width || !height)
    return D3DERR_INVALIDCALL;
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
    UINT quality = 0;
    if (FAILED(d3d11_device_->CheckMultisampleQualityLevels(dxgi_format, (UINT)multisample, &quality)) || !quality)
      return D3DERR_NOTAVAILABLE;
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
      static_cast<IDirect3DDevice9 *>(this), texture, format, D3DPOOL_DEFAULT, false, context_, D3DUSAGE_DEPTHSTENCIL, true
  ));
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
  // a target's own level: a level of a texture is as large as the chain has it, not the texture's top level
  const UINT level = ours->level();
  const UINT target_width = std::max(1u, desc.Width >> level);
  const UINT target_height = std::max(1u, desc.Height >> level);
  if (depth_stencil_) {
    depth_stencil_->texture()->GetDesc(&depth_desc);
    if (depth_desc.Width != desc.Width || depth_desc.Height != desc.Height) {
      ERR_ONCE("D3D9: a render target of another size than the depth-stencil surface is not taken.");
      return D3DERR_INVALIDCALL;
    }
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
  current_target_ = surface;
  current_target_view_ = view;
  target_width_ = target_width;
  target_height_ = target_height;
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
    ERR_ONCE("D3D9: only the world, view, projection and texture 0..7 transforms are kept: this build performs no "
             "vertex blending, so that transform would be one no draw reads.");
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
    ERR_ONCE("D3D9: only the world, view, projection and texture 0..7 transforms are kept: that one has no value to "
             "give.");
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
    ERR_ONCE("D3D9: only the world, view and projection transforms are kept: that one has nothing to multiply.");
    return D3DERR_INVALIDCALL;
  }
  state_.multiply(state, *matrix);
  if (recording_)
    record(StateKey{StateKey::Transform, (UINT)state, 0});
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
  for (const StateKey &recorded : recorded_keys_)
    if (recorded == key)
      return;
  recorded_keys_.push_back(key);
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
  recording_ = true;
  recorded_keys_.clear();
  return S_OK;
}

HRESULT
MTLD3D9Device::EndStateBlock(IDirect3DStateBlock9 **block) {
  if (!block)
    return D3DERR_INVALIDCALL;
  *block = nullptr;
  if (!recording_)
    return D3DERR_INVALIDCALL;
  recording_ = false;
  std::vector<StateKey> keys = std::move(recorded_keys_);
  recorded_keys_.clear();
  ComPtr<MTLD3D9StateBlock> result;
  result.Attach(new MTLD3D9StateBlock(this, D3DSBT_ALL));
  const HRESULT hr = result->capture_recorded(keys);
  if (FAILED(hr))
    return hr;
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

  ComPtr<IDirect3DSurface9> previous_target;
  GetRenderTarget(0, &previous_target);
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

  SetRenderTarget(0, previous_target.Get());
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
