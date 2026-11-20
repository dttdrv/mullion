// a Direct3D 9 device's own object: the D3D11 device and the window's implicit swap chain it presents through, the back
// buffer a frame is drawn into, and the CPU surfaces a frame is read back into. the render states, targets and
// depth-stencil surfaces are d3d9_device_targets.cpp's, the geometry a draw reads d3d9_device_draw.cpp's, and the
// class itself d3d9_device.hpp's.
#include "d3d9_device.hpp"

#include "d3d9_direct3d.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <dxgi1_4.h>
#include <memory>
#include <thread>

namespace mullion::d3d9 {

const IID IID_IMTLD3D9DeviceLifetime = {0x7f2b0a61, 0x4c3d, 0x4c8e, {0x9e, 0x2a, 0x51, 0x3b, 0x77, 0x0f, 0x6d, 0x12}};
const IID IID_IMTLD3D9DeviceResources = {0x2c8e5a94, 0x1b76, 0x4f0d, {0x83, 0x5a, 0x6d, 0x1c, 0x02, 0x9e, 0x47, 0x51}};

namespace {

constexpr WCHAR kFocusProp[] = L"MullionD3D9Focus";
constexpr WCHAR kFocusWndProcProp[] = L"MullionD3D9FocusWndProc";

LRESULT CALLBACK d3d9_focus_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  auto *device = reinterpret_cast<MTLD3D9Device *>(GetPropW(hwnd, kFocusProp));
  if (device && (msg == WM_ACTIVATEAPP || msg == WM_ACTIVATE)) {
    const bool activating = (msg == WM_ACTIVATEAPP) ? (wp != FALSE)
                                                    : (LOWORD(wp) != WA_INACTIVE);
    if (!activating)
      device->notify_focus_lost();
    else
      device->notify_focus_gained();
  }
  // Exclusive devices restore the monitor-sized frame when the window is moved/resized away (Wine device.c
  // test_window_position): one restore on the first size change, then leave subsequent sizes alone.
  if (device && (msg == WM_WINDOWPOSCHANGED || msg == WM_SIZE || msg == WM_MOVE)) {
    device->restore_fullscreen_window_if_needed();
  }
  auto original = reinterpret_cast<WNDPROC>(GetPropW(hwnd, kFocusWndProcProp));
  if (!original)
    original = DefWindowProcW;
  return CallWindowProcW(original, hwnd, msg, wp, lp);
}

} // namespace

void
MTLD3D9Device::release_device_children() {
  for (UINT stream = 0; stream < kMaxStreams; stream++)
    clear_stream(stream);
  release_indices();
  clear_bound_shaders();
  declaration_ = nullptr;
  declaration_inputs_ = nullptr;
  // the declarations SetFVF made are the device's, and this is the cache that frees them
  for (auto &entry : fvf_declarations_)
    entry.second->free_device_cache();
  fvf_declarations_.clear();
  for (UINT stage = 0; stage < kTotalTextureStages; stage++) {
    if (texture_sources_[stage]) {
      texture_sources_[stage]->release_private_ref();
      texture_sources_[stage] = nullptr;
    }
    textures_[stage] = nullptr;
  }
  cursor_set_ = false;
  cursor_visible_ = false;
  if (cursor_handle_) {
    DestroyCursor(cursor_handle_);
    cursor_handle_ = nullptr;
  }
  if (exclusive_reported_) {
    d3d9_leave_exclusive_mode(direct3d_.Get());
    exclusive_reported_ = false;
  }
  apply_fullscreen_window_state(false);
  destroy_depth_stencil();
  depth_stencil_view_.Reset();
  for (UINT at = 0; at < kMaxRenderTargets; at++)
    release_render_target(at);
  for (auto &view : render_target_views_)
    view.Reset();
  render_target_count_ = 0;
  invalidate_back_buffer();
  render_target_.Reset();
  back_buffer_texture_.Reset();
  if (swap_chain_) {
    swap_chain_->destroy_implicit();
    swap_chain_ = nullptr;
  }
  // release the DXGI swap chain with the device so a later CreateDevice can take the same HWND (DXGI allows one
  // swap chain per window; leaving it alive after the D3D9 wrapper goes is why d3d9_lifetime's next CreateDevice
  // failed with DXGI_ERROR_INVALID_CALL)
  if (swapchain_) {
    if (context_) {
      context_->OMSetRenderTargets(0, nullptr, nullptr);
      context_->Flush();
    }
    swapchain_.Reset();
  }
  additional_chains_.clear();
  up_scratch_.Reset();
  up_index_scratch_.Reset();
  up_scratch_size_ = 0;
  up_index_scratch_size_ = 0;
  decl_scratch_.Reset();
  decl_scratch_size_ = 0;
  fan_index_scratch_.Reset();
  fan_index_scratch_size_ = 0;
  fog_passthrough_ps_.Reset();
  pos_tex_passthrough_vs_.Reset();
}

// The state a Reset loses (Microsoft Learn, IDirect3DDevice9::Reset: "all state information is lost"): the streams and
// indices, the shaders set and the declaration, the textures and the texture stages' samplers and operations, the
// constants, the transforms (which start identity again), and the render states, which go back to the defaults a device
// is made with. The shader objects an application made are not destroyed: Reset only unbinds them, as D3D9's shaders
// survive a Reset.
void
MTLD3D9Device::reset_state() {
  for (UINT stream = 0; stream < kMaxStreams; stream++)
    clear_stream(stream);
  release_indices();
  up_scratch_.Reset();
  up_index_scratch_.Reset();
  up_scratch_size_ = 0;
  up_index_scratch_size_ = 0;
  decl_scratch_.Reset();
  decl_scratch_size_ = 0;
  fan_index_scratch_.Reset();
  fan_index_scratch_size_ = 0;
  // Reset loses the render target set: slot 0 is the new chain's back buffer, taken when the next draw asks for it
  for (UINT at = 0; at < kMaxRenderTargets; at++)
    release_render_target(at);
  for (auto &view : render_target_views_)
    view.Reset();
  render_target_count_ = 0;
  clear_bound_shaders();
  declaration_ = nullptr;
  declaration_inputs_ = nullptr;
  declaration_passthroughs_.clear();
  fvf_ = 0;
  for (UINT stage = 0; stage < kTotalTextureStages; stage++) {
    if (texture_sources_[stage]) {
      texture_sources_[stage]->release_private_ref();
      texture_sources_[stage] = nullptr;
    }
    textures_[stage] = nullptr;
  }
  for (auto &sampler : sampler_states_)
    sampler = SamplerState9();
  texture_stages_[0] = TextureStageState9(D3DTOP_MODULATE, 0);
  for (UINT stage = 1; stage < kMaxBlendStages; stage++)
    texture_stages_[stage] = TextureStageState9(D3DTOP_DISABLE, stage);
  for (UINT stream = 0; stream < kMaxStreams; stream++)
    stream_freq_[stream] = 1;
  vertex_constants_f_.fill(0.0f);
  vertex_constants_i_.fill(0);
  vertex_constants_b_.fill(0);
  pixel_constants_f_.fill(0.0f);
  pixel_constants_i_.fill(0);
  pixel_constants_b_.fill(0);
  vs_f_dirty_ = vs_i_dirty_ = vs_b_dirty_ = true;
  ps_f_dirty_ = ps_i_dirty_ = ps_b_dirty_ = true;
  ps_alpha_dirty_ = true;
  uploaded_alpha_ref_ = ~0u;
  // the lighting state and the transforms are lost with everything else: the lights, their enables and the material go
  // back to the defaults a device is made with, and the three matrices to the identity
  state_.reset();
  scissor_rect_ = RECT{};
  in_scene_ = false;
  recording_ = false;
  recorded_keys_.clear();
}

// A windowed chain sized and formatted again. The application has to have let go of the implicit swap chain's back
// buffer and of any depth-stencil surface it took first, as D3D9 requires ("an application should release any explicit
// render targets, depth stencil surfaces, additional swap chains, state blocks, and D3DPOOL_DEFAULT resources"); a
// reference it still holds is refused rather than left addressing storage that is gone.
//
// The caller's D3DPRESENT_PARAMETERS reports the new chain as zero in BackBufferCount, BackBufferWidth and
// BackBufferHeight and D3DFMT_UNKNOWN in BackBufferFormat, as the method documents; the device keeps the values it
// actually made.
HRESULT
MTLD3D9Device::Reset(D3DPRESENT_PARAMETERS *parameters) {
  if (!parameters)
    return D3DERR_INVALIDCALL;
  // The swap effects: this device's DXGI chain is a flip model chain, which is what D3DSWAPEFFECT_DISCARD and
  // D3DSWAPEFFECT_FLIP are (a Present shows the back buffer and the buffer drawn into next holds nothing the application
  // may keep); D3DSWAPEFFECT_COPY leaves the back buffer as it was, which Present provides by putting the frame it showed
  // back (present()). D3DSWAPEFFECT_OVERLAY is an Ex swap effect for a display this build does not have.
  const DWORD effect = (DWORD)parameters->SwapEffect;
  if (effect == 0 || effect == 4 /* OVERLAY */ || effect > 5) {
    ERR_ONCE("D3D9: Reset to swap effect ", effect, ", which this build has no presentation for.");
    return D3DERR_INVALIDCALL;
  }
  if (!extended_ && effect == 5 /* D3DSWAPEFFECT_FLIPEX */)
    return D3DERR_INVALIDCALL;
  if (effect != D3DSWAPEFFECT_DISCARD && effect != D3DSWAPEFFECT_FLIP && effect != D3DSWAPEFFECT_COPY &&
      effect != 5 /* D3DSWAPEFFECT_FLIPEX */) {
    ERR_ONCE("D3D9: Reset to swap effect ", effect, ", which this build has no presentation for.");
    return D3DERR_INVALIDCALL;
  }
  if (effect == D3DSWAPEFFECT_COPY && parameters->BackBufferCount > 1)
    return D3DERR_INVALIDCALL;
  // D3D9Ex accepts up to 30 back buffers; classic D3D9 stops at 3 (Wine d3d9ex.c test_swapchain_parameters tests 19
  // to 26; device.c test_swapchain_parameters).
  if (parameters->BackBufferCount > (extended_ ? 30u : 3u))
    return D3DERR_INVALIDCALL;
  {
    const DWORD interval = parameters->PresentationInterval;
    const bool interval_ok =
        interval == D3DPRESENT_INTERVAL_DEFAULT ||
        ((interval == D3DPRESENT_INTERVAL_ONE || interval == D3DPRESENT_INTERVAL_TWO ||
          interval == D3DPRESENT_INTERVAL_THREE || interval == D3DPRESENT_INTERVAL_FOUR ||
          interval == D3DPRESENT_INTERVAL_IMMEDIATE) &&
         (caps_.PresentationIntervals & interval) != 0);
    if (!interval_ok)
      return D3DERR_INVALIDCALL;
  }
  // multisampling is the discard chain's alone (Microsoft Learn, D3DPRESENT_PARAMETERS::MultiSampleType)
  if (parameters->MultiSampleType != D3DMULTISAMPLE_NONE && effect != D3DSWAPEFFECT_DISCARD) {
    ERR_ONCE("D3D9: Reset to a multisampled chain that is not D3DSWAPEFFECT_DISCARD.");
    return D3DERR_INVALIDCALL;
  }
  // the window a windowed chain presents to: the one the parameters name, or the focus window CreateDevice was given.
  // A window that is not one is refused, as D3D9 refuses it (IDirect3DDevice9::Reset: "If Windowed is TRUE ...
  // hDeviceWindow must be a valid window"). Focus and device windows are distinct when the application separates them.
  HWND target_window = parameters->hDeviceWindow ? parameters->hDeviceWindow : window_;
  if (!IsWindow(target_window)) {
    ERR_ONCE("D3D9: Reset names a window that is not one.");
    return D3DERR_INVALIDCALL;
  }
  // a different presentation HWND is a new DXGI chain: the old chain belongs to the HWND it was made for. The focus
  // window (window_) is never replaced here — GetCreationParameters must keep reporting it.
  const bool new_window = target_window != present_hwnd();
  // nothing to resize, and nothing to make a recovery's chain over: a chain the device was made with, or the one an
  // earlier failed recovery releases (a lost device is still one a Reset can try again)
  if (!swapchain_ && !swap_chain_)
    return D3DERR_INVALIDCALL;

  // D3DFMT_UNKNOWN is the desktop's format for a windowed chain; this build's desktop is X8R8G8B8, and a chain that
  // already had a format keeps it
  D3DFORMAT format = parameters->BackBufferFormat;
  if (format == D3DFMT_UNKNOWN)
    format = present_.BackBufferFormat != D3DFMT_UNKNOWN ? present_.BackBufferFormat : D3DFMT_X8R8G8B8;
  const DXGI_FORMAT dxgi_format = to_dxgi_format(format);
  if (dxgi_format == DXGI_FORMAT_UNKNOWN)
    return D3DERR_INVALIDCALL;

  // a width or height of zero is the presentation window's client area, as CreateDevice's is
  UINT width = parameters->BackBufferWidth, height = parameters->BackBufferHeight;
  if (!width || !height) {
    RECT client{};
    if (!GetClientRect(target_window, &client))
      return D3DERR_INVALIDCALL;
    if (!width)
      width = (UINT)(client.right - client.left);
    if (!height)
      height = (UINT)(client.bottom - client.top);
  }
  if (!width || !height)
    return D3DERR_INVALIDCALL;

  // Classic D3D9 fullscreen focus loss: Reset while the desktop holds focus stays DEVICELOST (Wine device.c
  // test_lost_device). D3D9Ex never loses the device on focus loss (Wine d3d9ex.c test_lost_device).
  if (!extended_ && !present_.Windowed) {
    const HWND focus = window_ && IsWindow(window_) ? window_ : present_hwnd();
    const HWND foreground = GetForegroundWindow();
    const bool desktop_has_focus = foreground && (foreground == GetDesktopWindow());
    if (cooperative_ == Cooperative::Lost && focus && foreground == focus)
      cooperative_ = Cooperative::NotReset;
    if (cooperative_ == Cooperative::Lost ||
        (desktop_has_focus && cooperative_ != Cooperative::NotReset && cooperative_ != Cooperative::Ok)) {
      cooperative_ = Cooperative::Lost;
      return D3DERR_DEVICELOST;
    }
    if (cooperative_ == Cooperative::Lost)
      return D3DERR_DEVICELOST;
  }
  // A device whose frames failed is the one a Reset replaces, so nothing it holds is the application's to have released
  // first: the surfaces of the old device are gone with it. A live D3D9 device keeps the references it has, and an
  // application reference above them would be left addressing storage the resize replaces. D3D9Ex Reset keeps
  // outstanding back-buffer / depth surfaces as orphaned objects (Wine d3d9ex.c test_backbuffer_resize).
  const bool lost = d3d11_device_ && FAILED(d3d11_device_->GetDeviceRemovedReason());
  if (!lost && !extended_) {
    if (back_buffer_surface_ && back_buffer_surface_->public_references() > 0) {
      ERR_ONCE("D3D9: Reset needs every application reference to the implicit swap chain's back buffer released first.");
      return D3DERR_INVALIDCALL;
    }
    if (depth_stencil_ && depth_stencil_->public_references() > 0) {
      ERR_ONCE("D3D9: Reset needs every application reference to a depth-stencil surface released first.");
      return D3DERR_INVALIDCALL;
    }
    // a multisampled chain is a count (2 to 16) the Direct3D 11 device reports quality for, checked before the chain is
    // touched so that a refusal leaves it as it was
    if (parameters->MultiSampleType != D3DMULTISAMPLE_NONE) {
      UINT quality = 0;
      if ((UINT)parameters->MultiSampleType < 2 || (UINT)parameters->MultiSampleType > 16 ||
          FAILED(d3d11_device_->CheckMultisampleQualityLevels(dxgi_format, (UINT)parameters->MultiSampleType,
                                                              &quality)) ||
          !quality) {
        ERR_ONCE("D3D9: Reset to a multisampled chain of ", (unsigned)parameters->MultiSampleType,
                 " samples, which the device has no quality for in this format.");
        return D3DERR_INVALIDCALL;
      }
    }
  }

  if (lost) {
    // the chain the parameters describe, over the device the Reset makes: the fields the application left to the window
    // are the sizes it asked for, and a chain the device already had keeps its format
    present_ = *parameters;
    if (!present_.hDeviceWindow)
      present_.hDeviceWindow = window_;
    present_.BackBufferWidth = width;
    present_.BackBufferHeight = height;
    present_.BackBufferFormat = format;
    const HRESULT recovered = recreate_after_loss();
    if (FAILED(recovered)) {
      ERR("D3D9: the device a Reset was to make is not there; the device stays lost (0x", std::hex, recovered, ").");
      return D3DERR_DEVICENOTRESET;
    }
    return finish_reset(parameters);
  }

  if (!swapchain_)
    return D3DERR_INVALIDCALL;

  // nothing of the old frame stays bound while the chain replaces its buffers, which is what DXGI's ResizeBuffers needs
  // (IDXGISwapChain::ResizeBuffers: "release all of its direct and indirect references on the back buffers")
  context_->Flush();
  context_->OMSetRenderTargets(0, nullptr, nullptr);
  for (UINT at = 0; at < kMaxRenderTargets; at++)
    release_render_target(at);
  for (auto &view : render_target_views_)
    view.Reset();
  render_target_count_ = 0;
  destroy_depth_stencil();
  depth_stencil_view_.Reset();
  invalidate_back_buffer();

  // a DXGI flip chain holds at most 16 buffers, while D3D9Ex lets the application name up to 30 back buffers: the
  // chain keeps the 16 it can and D3D9's own count stays what the application asked for
  const UINT buffer_count = std::min<UINT>(std::max<UINT>(parameters->BackBufferCount, 2), 16);
  HRESULT hr = S_OK;
  if (new_window) {
    swapchain_.Reset();
    present_ = *parameters;
    if (!present_.hDeviceWindow)
      present_.hDeviceWindow = window_;
    present_.BackBufferWidth = width;
    present_.BackBufferHeight = height;
    present_.BackBufferFormat = format;
    hr = create_chain(factory_.Get());
    if (FAILED(hr))
      return hr;
  } else {
    const bool skip_window_changes = (behavior_flags_ & D3DCREATE_NOWINDOWCHANGES) != 0;
    if (!skip_window_changes) {
      if (!parameters->Windowed)
        swapchain_->SetFullscreenState(TRUE, nullptr);
      else if (!present_.Windowed)
        swapchain_->SetFullscreenState(FALSE, nullptr);
    }
    hr = swapchain_->ResizeBuffers(buffer_count, width, height, dxgi_format, 0);
    if (FAILED(hr)) {
      ERR("D3D9: the chain did not resize: 0x", std::hex, hr);
      return D3DERR_INVALIDCALL;
    }
    DXGI_SWAP_CHAIN_DESC1 created{};
    swapchain_->GetDesc1(&created);
    width_ = created.Width;
    height_ = created.Height;
    present_ = *parameters;
    if (!present_.hDeviceWindow)
      present_.hDeviceWindow = window_;
    present_.BackBufferWidth = created.Width;
    present_.BackBufferHeight = created.Height;
    // the chain DXGI made keeps two buffers for a flip model; D3D9's own count is what the application asked for, and a
    // D3D9 chain always has at least one back buffer (Wine's device test: GetPresentParameters reports 1, not 2)
    present_.BackBufferCount = std::max<UINT>(parameters->BackBufferCount, 1);
    present_.BackBufferFormat = format;
  }
  if (swap_chain_)
    swap_chain_->set_present_parameters(present_);
  return finish_reset(parameters);
}

// What both roads to a new chain leave the device with: the state a Reset loses, the new frame as the whole viewport and
// scissor rectangle, an automatic depth-stencil surface of the new size (so depth keeps working across a resolution
// change, or a recovery, without the application making another), and the fields D3D9 leaves to be queried again
// (Microsoft Learn, IDirect3DDevice9::Reset: "BackBufferCount, BackBufferWidth, and BackBufferHeight are set to zero.
// BackBufferFormat is set to D3DFMT_UNKNOWN for windowed mode only").
HRESULT
MTLD3D9Device::finish_reset(D3DPRESENT_PARAMETERS *parameters) {
  cooperative_ = Cooperative::Ok;
  // Track exclusive fullscreen for IDirect3D9::GetAdapterDisplayMode.
  if (!present_.Windowed) {
    if (!exclusive_reported_) {
      d3d9_enter_exclusive_mode(direct3d_.Get(), present_.BackBufferWidth, present_.BackBufferHeight);
      exclusive_reported_ = true;
    } else {
      d3d9_leave_exclusive_mode(direct3d_.Get());
      d3d9_enter_exclusive_mode(direct3d_.Get(), present_.BackBufferWidth, present_.BackBufferHeight);
    }
  } else if (exclusive_reported_) {
    d3d9_leave_exclusive_mode(direct3d_.Get());
    exclusive_reported_ = false;
  }
  apply_fullscreen_window_state(!present_.Windowed);
  // D3D9Ex keeps the device state across Reset: Wine's d3d9 layer calls wined3d_device_reset with reset_state only for
  // a classic device, and d3d9ex.c test_backbuffer_resize draws with the FVF it set before the Reset. A classic device
  // loses the state, as IDirect3DDevice9::Reset documents.
  if (!extended_) {
    reset_state();
    viewport_ = D3DVIEWPORT9{0, 0, width_, height_, 0.0f, 1.0f};
    scissor_rect_ = RECT{0, 0, (LONG)width_, (LONG)height_};
  }
  if (present_.EnableAutoDepthStencil) {
    ComPtr<IDirect3DSurface9> auto_depth;
    HRESULT hr = create_depth_stencil_surface(
        width_, height_, present_.AutoDepthStencilFormat, present_.MultiSampleType, 0, TRUE, 0, &auto_depth, nullptr, true
    );
    if (SUCCEEDED(hr))
      hr = SetDepthStencilSurface(auto_depth.Get());
    if (FAILED(hr)) {
      ERR("D3D9: no automatic depth-stencil surface of format {} for the chain a Reset made: {}",
          (uint32_t)present_.AutoDepthStencilFormat, (uint32_t)hr);
      return D3DERR_INVALIDCALL;
    }
    // the automatic surface is the new chain's, and it is what D3DRS_ZENABLE follows on a classic device; an Ex device
    // keeps the render state it had
    if (!extended_)
      state_.render().set(D3DRS_ZENABLE, TRUE);
  }
  parameters->BackBufferWidth = 0;
  parameters->BackBufferHeight = 0;
  parameters->BackBufferCount = 0;
  parameters->BackBufferFormat = D3DFMT_UNKNOWN;
  return S_OK;
}

void
MTLD3D9Device::invalidate_back_buffer() {
  auto orphan = [&](MTLD3D9Surface *surface) {
    if (!surface)
      return;
    if (surface->public_references() > 0) {
      // Snapshot away from the DXGI swap-chain buffer so ResizeBuffers can drop every chain reference.
      ComPtr<ID3D11Texture2D> standalone;
      if (ID3D11Texture2D *src = surface->texture()) {
        D3D11_TEXTURE2D_DESC desc{};
        src->GetDesc(&desc);
        desc.MiscFlags = 0;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        if (SUCCEEDED(d3d11_device_->CreateTexture2D(&desc, nullptr, &standalone)) && context_)
          context_->CopyResource(standalone.Get(), src);
      }
      surface->detach_as_orphan(standalone);
    } else {
      surface->destroy_implicit();
    }
  };
  orphan(back_buffer_surface_);
  back_buffer_surface_ = nullptr;
  for (MTLD3D9Surface *&extra : extra_back_buffers_) {
    orphan(extra);
    extra = nullptr;
  }
  extra_back_buffers_.clear();
  render_target_.Reset();
  back_buffer_texture_.Reset();
  front_.reset();
}

void
MTLD3D9Device::clear_depth_stencil() {
  if (depth_stencil_held_ && depth_stencil_)
    depth_stencil_->release_private_ref();
  depth_stencil_ = nullptr;
  depth_stencil_held_ = false;
}

void
MTLD3D9Device::destroy_depth_stencil() {
  if (depth_stencil_) {
    if (depth_stencil_->implicit_surface())
      depth_stencil_->destroy_implicit();
    else if (depth_stencil_held_)
      depth_stencil_->release_private_ref();
  }
  depth_stencil_ = nullptr;
  depth_stencil_held_ = false;
}

void
MTLD3D9Device::release_render_target(UINT index) {
  if (index >= kMaxRenderTargets)
    return;
  if (render_target_held_[index] && render_target_objects_[index])
    render_target_objects_[index]->release_private_ref();
  render_targets_[index] = nullptr;
  render_target_objects_[index] = nullptr;
  render_target_held_[index] = false;
}

void
MTLD3D9Device::unbind_surface(IMTLD3D9Surface *surface) {
  if (!surface)
    return;
  for (UINT index = 0; index < kMaxRenderTargets; index++) {
    if (render_target_objects_[index] == surface)
      release_render_target(index);
  }
  if (depth_stencil_ && static_cast<IMTLD3D9Surface *>(depth_stencil_) == surface)
    clear_depth_stencil();
}

void
MTLD3D9Device::unbind_texture(IMTLD3D9Texture *texture) {
  if (!texture)
    return;
  for (UINT stage = 0; stage < kTotalTextureStages; stage++) {
    if (texture_sources_[stage] == texture) {
      texture_sources_[stage]->release_private_ref();
      texture_sources_[stage] = nullptr;
      textures_[stage] = nullptr;
    }
  }
}

// The chain a device presents through, over the device it has now. A DXGI chain belongs to the device that made it, so a
// Reset that recreates the device makes a new chain; the chain object an application may hold is the same one, and it
// presents this device's frames through the device it asks (IMTLD3D9PresentSource).
HRESULT
MTLD3D9Device::create_chain(IDXGIFactory2 *factory) {
  // an additional chain is a buffer of the Direct3D 11 device this one replaces, so it goes with it
  additional_chains_.clear();
  const DXGI_FORMAT format = to_dxgi_format(present_.BackBufferFormat);
  // CreateDevice with hDeviceWindow NULL still reports the focus window (Wine device.c GetPresentParameters).
  if (!present_.hDeviceWindow)
    present_.hDeviceWindow = window_;
  HWND present_window = present_hwnd();
  if (format == DXGI_FORMAT_UNKNOWN || !present_window || !IsWindow(present_window) || !factory)
    return D3DERR_INVALIDCALL;

  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = present_.BackBufferWidth;
  desc.Height = present_.BackBufferHeight;
  desc.Format = format;
  desc.Stereo = FALSE;
  desc.SampleDesc.Count = 1;
  // a flip model chain shows the buffer drawn into last and turns over to
  // another one, which is what a D3D9 D3DSWAPEFFECT_DISCARD chain does, and it
  // has at least two buffers to turn between
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  // DXGI's flip model takes 2 to 16 buffers; a D3D9Ex chain may ask for as many as 30, which D3D9 reports back while
  // the DXGI chain keeps the 16 it has (Wine d3d9ex.c test_swapchain_parameters tests 19/21/23/25)
  desc.BufferCount = std::min<UINT>(std::max<UINT>(present_.BackBufferCount, 2), 16);
  desc.Scaling = DXGI_SCALING_STRETCH;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  desc.Flags = 0;

  // D3DCREATE_NOWINDOWCHANGES: keep DXGI windowed so enterFullscreenMode does not rewrite styles/rects on the
  // device window (Wine device.c test_window_styles). D3D9 still reports present_.Windowed as the app asked.
  const bool skip_window_changes = (behavior_flags_ & D3DCREATE_NOWINDOWCHANGES) != 0;
  // Capture application styles before DXGI mutates the HWND for exclusive fullscreen.
  if (!present_.Windowed && !skip_window_changes)
    save_device_window_styles_if_needed();
  const bool dxgi_fullscreen = !present_.Windowed && !skip_window_changes;
  DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen{};
  fullscreen.Windowed = dxgi_fullscreen ? FALSE : TRUE;
  fullscreen.RefreshRate.Numerator = 0;
  fullscreen.RefreshRate.Denominator = 0;
  fullscreen.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
  fullscreen.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
  HRESULT hr = factory->CreateSwapChainForHwnd(
      d3d11_device_.Get(), present_window, &desc, dxgi_fullscreen ? &fullscreen : nullptr, nullptr, &swapchain_
  );
  if (FAILED(hr)) {
    // DXGI: one swap chain per HWND. D3D9: multiple devices may share a focus window (Wine
    // device.c test_render_target_device_mismatch). Bind this device's chain to a private proxy window.
    if (owns_swapchain_window_ && swapchain_window_ && swapchain_window_ != present_window &&
        swapchain_window_ != window_) {
      DestroyWindow(swapchain_window_);
      swapchain_window_ = nullptr;
      owns_swapchain_window_ = false;
    }
    static ATOM proxy_class = 0;
    if (!proxy_class) {
      WNDCLASSEXW wc{};
      wc.cbSize = sizeof(wc);
      wc.lpfnWndProc = DefWindowProcW;
      wc.hInstance = GetModuleHandleW(nullptr);
      wc.lpszClassName = L"MullionD3D9SwapProxy";
      proxy_class = RegisterClassExW(&wc);
    }
    RECT client{0, 0, (LONG)desc.Width, (LONG)desc.Height};
    AdjustWindowRectEx(&client, WS_POPUP, FALSE, 0);
    HWND proxy = CreateWindowExW(
        0, L"MullionD3D9SwapProxy", L"", WS_POPUP, 0, 0, client.right - client.left, client.bottom - client.top,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr
    );
    if (!proxy) {
      ERR("D3D9: no swap chain for the window: 0x", std::hex, hr);
      return D3DERR_NOTAVAILABLE;
    }
    present_window = proxy;
    hr = factory->CreateSwapChainForHwnd(
        d3d11_device_.Get(), present_window, &desc, dxgi_fullscreen ? &fullscreen : nullptr, nullptr, &swapchain_
    );
    if (FAILED(hr)) {
      DestroyWindow(proxy);
      ERR("D3D9: no swap chain for the window: 0x", std::hex, hr);
      return D3DERR_NOTAVAILABLE;
    }
    swapchain_window_ = proxy;
    owns_swapchain_window_ = true;
  } else {
    swapchain_window_ = present_window;
    owns_swapchain_window_ = false;
  }

  DXGI_SWAP_CHAIN_DESC1 created{};
  swapchain_->GetDesc1(&created);
  width_ = created.Width;
  height_ = created.Height;
  present_.BackBufferWidth = created.Width;
  present_.BackBufferHeight = created.Height;
  present_.BackBufferCount = std::max<UINT>(present_.BackBufferCount, 1);

  if (swap_chain_)
    swap_chain_->set_present_parameters(present_);
  else
    swap_chain_ = new MTLD3D9SwapChain(
        static_cast<IDirect3DDevice9 *>(this), static_cast<IMTLD3D9PresentSource *>(this), present_, {}, extended_
    );
  apply_fullscreen_window_state(!present_.Windowed);
  return S_OK;
}

void
MTLD3D9Device::save_device_window_styles_if_needed() {
  if (behavior_flags_ & D3DCREATE_NOWINDOWCHANGES)
    return;
  HWND device_hwnd = present_.hDeviceWindow ? present_.hDeviceWindow : window_;
  if (!device_hwnd || !IsWindow(device_hwnd) || styles_saved_)
    return;
  saved_device_style_ = GetWindowLongW(device_hwnd, GWL_STYLE);
  saved_device_exstyle_ = GetWindowLongW(device_hwnd, GWL_EXSTYLE);
  GetWindowRect(device_hwnd, &saved_device_rect_);
  styles_saved_ = true;
}

RECT
MTLD3D9Device::fullscreen_monitor_rect() const {
  RECT rect{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
  HMONITOR monitor = nullptr;
  if (direct3d_)
    monitor = direct3d_->GetAdapterMonitor(adapter_ordinal_);
  if (monitor) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info))
      rect = info.rcMonitor;
  }
  return rect;
}

void
MTLD3D9Device::notify_focus_lost() {
  if (present_.Windowed || (behavior_flags_ & D3DCREATE_NOWINDOWCHANGES))
    return;
  // D3D9Ex stays cooperative-OK and reports occlusion from Present/CheckDeviceState instead.
  if (!extended_ && presented_once_ && cooperative_ == Cooperative::Ok)
    cooperative_ = Cooperative::Lost;
  if (extended_)
    return;
  HWND device_hwnd = present_.hDeviceWindow ? present_.hDeviceWindow : window_;
  if (device_hwnd && IsWindow(device_hwnd)) {
    // Windows minimizes the device window on exclusive focus loss (Wine device.c test_window_style).
    SetWindowLongW(device_hwnd, GWL_STYLE, GetWindowLongW(device_hwnd, GWL_STYLE) | WS_MINIMIZE);
    SetWindowLongW(device_hwnd, GWL_EXSTYLE, GetWindowLongW(device_hwnd, GWL_EXSTYLE) | WS_EX_TOPMOST);
  }
}

void
MTLD3D9Device::notify_focus_gained() {
  if (present_.Windowed)
    return;
  if (cooperative_ == Cooperative::Lost)
    cooperative_ = Cooperative::NotReset;
  HWND device_hwnd = present_.hDeviceWindow ? present_.hDeviceWindow : window_;
  if (device_hwnd && IsWindow(device_hwnd)) {
    LONG style = GetWindowLongW(device_hwnd, GWL_STYLE);
    if (style & WS_MINIMIZE)
      SetWindowLongW(device_hwnd, GWL_STYLE, style & ~WS_MINIMIZE);
  }
}

void
MTLD3D9Device::restore_fullscreen_window_if_needed() {
  if (present_.Windowed || (behavior_flags_ & D3DCREATE_NOWINDOWCHANGES))
    return;
  if (fullscreen_restore_used_)
    return;
  HWND device_hwnd = present_.hDeviceWindow ? present_.hDeviceWindow : window_;
  if (!device_hwnd || !IsWindow(device_hwnd))
    return;
  const RECT monitor = fullscreen_monitor_rect();
  RECT current{};
  GetWindowRect(device_hwnd, &current);
  if (current.left == monitor.left && current.top == monitor.top && current.right == monitor.right &&
      current.bottom == monitor.bottom)
    return;
  fullscreen_restore_used_ = true;
  SetWindowPos(device_hwnd, HWND_TOPMOST, monitor.left, monitor.top, monitor.right - monitor.left,
               monitor.bottom - monitor.top, SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void
MTLD3D9Device::apply_fullscreen_window_state(bool fullscreen) {
  if (behavior_flags_ & D3DCREATE_NOWINDOWCHANGES)
    return;
  // Prefer the application device window over any DXGI proxy HWND.
  HWND device_hwnd = present_.hDeviceWindow ? present_.hDeviceWindow : window_;
  if (!device_hwnd || !IsWindow(device_hwnd))
    return;
  HWND focus = window_ && IsWindow(window_) ? window_ : device_hwnd;

  if (fullscreen) {
    save_device_window_styles_if_needed();
    // Classic D3D9 adds WS_VISIBLE / WS_EX_TOPMOST (Wine device.c test_window_style). D3D9Ex leaves the
    // original style bits alone and only resizes (Wine d3d9ex.c test_window_style).
    if (!extended_) {
      SetWindowLongW(device_hwnd, GWL_STYLE, saved_device_style_ | WS_VISIBLE);
      SetWindowLongW(device_hwnd, GWL_EXSTYLE, saved_device_exstyle_ | WS_EX_TOPMOST);
    }
    const RECT monitor = fullscreen_monitor_rect();
    fullscreen_restore_used_ = false;
    SetWindowPos(device_hwnd, extended_ ? HWND_TOP : HWND_TOPMOST, monitor.left, monitor.top,
                 monitor.right - monitor.left, monitor.bottom - monitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    if (!extended_) {
      SetWindowLongW(device_hwnd, GWL_STYLE, saved_device_style_ | WS_VISIBLE);
      SetWindowLongW(device_hwnd, GWL_EXSTYLE, saved_device_exstyle_ | WS_EX_TOPMOST);
    }

    // Focus window is always subclassed in exclusive mode, even when it is also the device window (Wine
    // device.c test_wndproc / test_lost_device).
    if (focus && !focus_subclassed_) {
      focus_wndproc_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(focus, GWLP_WNDPROC,
                                                                   reinterpret_cast<LONG_PTR>(d3d9_focus_wndproc)));
      SetPropW(focus, kFocusProp, this);
      SetPropW(focus, kFocusWndProcProp, reinterpret_cast<HANDLE>(focus_wndproc_));
      focus_subclassed_ = true;
    }
  } else {
    if (styles_saved_) {
      // Classic D3D9: WS_VISIBLE / WS_EX_TOPMOST stick after Reset to windowed (Wine device.c).
      // D3D9Ex: restore the exact pre-exclusive styles (Wine d3d9ex.c test_window_style).
      const LONG style = extended_ ? saved_device_style_ : (saved_device_style_ | WS_VISIBLE);
      const LONG exstyle = extended_ ? saved_device_exstyle_ : (saved_device_exstyle_ | WS_EX_TOPMOST);
      SetWindowLongW(device_hwnd, GWL_STYLE, style);
      SetWindowLongW(device_hwnd, GWL_EXSTYLE, exstyle);
      SetWindowPos(device_hwnd, extended_ ? HWND_NOTOPMOST : HWND_TOPMOST, saved_device_rect_.left,
                   saved_device_rect_.top, saved_device_rect_.right - saved_device_rect_.left,
                   saved_device_rect_.bottom - saved_device_rect_.top, SWP_FRAMECHANGED | SWP_NOACTIVATE);
      SetWindowLongW(device_hwnd, GWL_EXSTYLE, exstyle);
      styles_saved_ = false;
    }
    if (focus_subclassed_ && focus) {
      SetWindowLongPtrW(focus, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(focus_wndproc_));
      RemovePropW(focus, kFocusProp);
      RemovePropW(focus, kFocusWndProcProp);
      focus_subclassed_ = false;
      focus_wndproc_ = nullptr;
    }
    fullscreen_restore_used_ = false;
  }
}

HRESULT
MTLD3D9Device::init(IDXGIFactory2 *factory) {
  HWND hwnd = present_hwnd();
  if (to_dxgi_format(present_.BackBufferFormat) == DXGI_FORMAT_UNKNOWN || !hwnd || !IsWindow(hwnd))
    return D3DERR_INVALIDCALL;
  // a multisampled chain is the discard chain's (its back buffer is a texture of the device's own that Present resolves,
  // since a flip model chain has no multisampled buffer), at a count the Direct3D 11 device reports quality for
  if (present_.MultiSampleType != D3DMULTISAMPLE_NONE &&
      (present_.SwapEffect != D3DSWAPEFFECT_DISCARD || (UINT)present_.MultiSampleType < 2 ||
       (UINT)present_.MultiSampleType > 16))
    return D3DERR_NOTAVAILABLE;

  factory_ = factory;
  // the Direct3D 11 device this Direct3D 9 device draws over is this build's own arrangement, not a device the
  // application asked for by name: the internal bit says so, and the router leaves this call alone (d3d11.cpp's
  // MULLION_INTERNAL_DEVICE). D3D11CoreCreateDevice takes the factory it is never given: it reads the adapter.
  HRESULT hr = D3D11CoreCreateDevice(factory_.Get(), adapter_.Get(),
                                     D3D11_CREATE_DEVICE_BGRA_SUPPORT | 0x40000000 /* MULLION_INTERNAL_DEVICE */,
                                     nullptr, 0, &d3d11_device_);
  if (FAILED(hr)) {
    ERR("D3D9: no D3D11 device over the adapter: 0x", std::hex, hr);
    return D3DERR_NOTAVAILABLE;
  }
  d3d11_device_->GetImmediateContext(&context_);
  // the same context as the one that clears a part of a view; a context that is not the 11.1 one keeps every clear
  // whole, which is what the clears below fall back to
  context_.As(&context1_);
  if (present_.MultiSampleType != D3DMULTISAMPLE_NONE) {
    UINT quality = 0;
    if (FAILED(d3d11_device_->CheckMultisampleQualityLevels(to_dxgi_format(present_.BackBufferFormat),
                                                            (UINT)present_.MultiSampleType, &quality)) ||
        !quality)
      return D3DERR_NOTAVAILABLE;
  }

  // the fixed-function path a device draws through, made once with the device: a device without it can clear and
  // present, but not draw, and this milestone's device draws
  hr = fixed_function_.initialize(d3d11_device_.Get());
  if (FAILED(hr)) {
    ERR("D3D9: the fixed-function path did not start: 0x", std::hex, hr);
    return D3DERR_NOTAVAILABLE;
  }

  hr = create_chain(factory);
  if (FAILED(hr)) {
    ERR("D3D9: the chain of a new device did not start: 0x", std::hex, hr);
    return hr;
  }
  if (!present_.Windowed) {
    d3d9_enter_exclusive_mode(direct3d_.Get(), present_.BackBufferWidth, present_.BackBufferHeight);
    exclusive_reported_ = true;
  }

  // the viewport of a device that has drawn nothing yet is its chain's whole back buffer, as D3D9's is
  viewport_ = D3DVIEWPORT9{0, 0, width_, height_, 0.0f, 1.0f};
  scissor_rect_ = RECT{0, 0, (LONG)width_, (LONG)height_};
  // D3D9's documented defaults for every stage, sampler, stream and render state (Wine device.c
  // test_texture_stage_states expects DISABLE past stage 0 and TEXCOORDINDEX == stage)
  reset_state();
  viewport_ = D3DVIEWPORT9{0, 0, width_, height_, 0.0f, 1.0f};
  scissor_rect_ = RECT{0, 0, (LONG)width_, (LONG)height_};

  // a chain made with an automatic depth-stencil surface gets one, so a game that asked for depth has it from its
  // first frame rather than drawing every pixel in the order it drew them
  if (present_.EnableAutoDepthStencil) {
    ComPtr<IDirect3DSurface9> auto_depth;
    HRESULT depth_hr = create_depth_stencil_surface(width_, height_, present_.AutoDepthStencilFormat,
                                                    present_.MultiSampleType, 0, TRUE, 0, &auto_depth, nullptr, true);
    if (SUCCEEDED(depth_hr))
      depth_hr = SetDepthStencilSurface(auto_depth.Get());
    if (FAILED(depth_hr)) {
      ERR("D3D9: no automatic depth-stencil surface of format {}: {}", (uint32_t)present_.AutoDepthStencilFormat,
          (uint32_t)depth_hr);
      return D3DERR_INVALIDCALL;
    }
    // the documented default for D3DRS_ZENABLE follows EnableAutoDepthStencil: a device whose chain made a depth
    // surface starts with depth testing on, and one without starts with it off (the member's own default)
    state_.render().set(D3DRS_ZENABLE, TRUE);
  }
  return S_OK;
}

// A lost device's Reset: the Direct3D 11 device the frames failed on is released with everything the device held on it,
// and a new one is made over the same adapter. D3D9's contract for Reset is that "all state information is lost" and
// that the state-block, default and dynamic pools go with it, while "D3DPOOL_MANAGED resources ... are not affected by a
// device reset": the state is reset by the caller, and the managed pool's resources are the ones registered with the
// device, each putting its levels onto the new one.
//
// Nothing of the failed device is waited for: what was submitted to a removed device is cancelled by it, and the queue
// behind it has already reported the fault the device is lost for.
HRESULT
MTLD3D9Device::recreate_after_loss() {
  // every reference the device holds on the old device's objects, before the device itself goes: the frame's views, the
  // default texture and the constant buffers, the scratch buffers a _UP draw staged into, the state objects and the
  // shaders the fixed-function path draws with, and the chain
  invalidate_back_buffer();
  destroy_depth_stencil();
  depth_stencil_view_.Reset();
  for (UINT at = 0; at < kMaxRenderTargets; at++)
    release_render_target(at);
  for (auto &view : render_target_views_)
    view.Reset();
  render_target_count_ = 0;
  additional_chains_.clear();
  fan_index_scratch_.Reset();
  fan_index_scratch_size_ = 0;
  target_width_ = target_height_ = 0;
  default_texture_.Reset();
  default_texture_view_.Reset();
  vs_constants_f_.Reset();
  vs_constants_i_.Reset();
  vs_constants_b_.Reset();
  ps_constants_f_.Reset();
  ps_constants_i_.Reset();
  ps_constants_b_.Reset();
  ps_alpha_constants_.Reset();
  vs_clip_planes_.Reset();
  fog_passthrough_ps_.Reset();
  pos_tex_passthrough_vs_.Reset();
  release_blit_objects();
  up_scratch_.Reset();
  up_index_scratch_.Reset();
  up_scratch_size_ = 0;
  up_index_scratch_size_ = 0;
  for (UINT stream = 0; stream < kMaxStreams; stream++)
    clear_stream(stream);
  release_indices();
  clear_bound_shaders();
  declaration_ = nullptr;
  declaration_inputs_ = nullptr;
  declaration_passthroughs_.clear();
  for (UINT stage = 0; stage < kTotalTextureStages; stage++) {
    if (texture_sources_[stage]) {
      texture_sources_[stage]->release_private_ref();
      texture_sources_[stage] = nullptr;
    }
    textures_[stage] = nullptr;
  }
  fixed_function_.release_device();
  swapchain_.Reset();
  context1_.Reset();
  context_.Reset();
  d3d11_device_.Reset();

  HRESULT hr = D3D11CoreCreateDevice(factory_.Get(), adapter_.Get(),
                                     D3D11_CREATE_DEVICE_BGRA_SUPPORT | 0x40000000 /* MULLION_INTERNAL_DEVICE */,
                                     nullptr, 0, &d3d11_device_);
  if (FAILED(hr)) {
    ERR("D3D9: no Direct3D 11 device to reset onto: 0x", std::hex, hr);
    return D3DERR_NOTAVAILABLE;
  }
  d3d11_device_->GetImmediateContext(&context_);
  context_.As(&context1_);
  hr = fixed_function_.initialize(d3d11_device_.Get());
  if (FAILED(hr))
    return D3DERR_NOTAVAILABLE;
  hr = create_chain(factory_.Get());
  if (FAILED(hr))
    return hr;

  // the managed pool survives: each of its textures puts the levels the game authored back onto the new device. A
  // texture that could not be rebuilt is reported rather than drawn as something the game did not make
  for (IMTLD3D9ResetTarget *target : reset_targets_)
    target->reset_device(d3d11_device_.Get(), context_);
  return S_OK;
}

HRESULT
MTLD3D9Device::ensure_back_buffer() {
  if (back_buffer_texture_)
    return S_OK;

  ComPtr<ID3D11Texture2D> buffer;
  HRESULT hr = S_OK;
  if (multisampled_chain()) {
    // a flip model chain has no multisampled buffer, so the back buffer the application draws into is a texture of the
    // device's own, with the chain's size and format and the sample count asked for; Present resolves it into the
    // chain's buffer
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = to_dxgi_format(present_.BackBufferFormat);
    desc.SampleDesc.Count = (UINT)present_.MultiSampleType;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    hr = d3d11_device_->CreateTexture2D(&desc, nullptr, &buffer);
    if (FAILED(hr)) {
      ERR("D3D9: no multisampled back buffer of ", (unsigned)present_.MultiSampleType, " samples: 0x", std::hex, hr);
      return D3DERR_INVALIDCALL;
    }
  } else {
    hr = swapchain_->GetBuffer(0, IID_PPV_ARGS(&buffer));
  }
  if (FAILED(hr)) {
    ERR("D3D9: the swap chain has no buffer 0: 0x", std::hex, hr);
    return D3DERR_INVALIDCALL;
  }
  ComPtr<ID3D11RenderTargetView> view;
  D3D11_RENDER_TARGET_VIEW_DESC multisampled_view{};
  multisampled_view.Format = to_dxgi_format(present_.BackBufferFormat);
  multisampled_view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
  hr = d3d11_device_->CreateRenderTargetView(buffer.Get(), multisampled_chain() ? &multisampled_view : nullptr, &view);
  if (FAILED(hr)) {
    ERR("D3D9: the back buffer takes no render target view: 0x", std::hex, hr);
    return D3DERR_INVALIDCALL;
  }

  back_buffer_texture_ = buffer;
  render_target_ = view;
  const bool lockable = (present_.Flags & D3DPRESENTFLAG_LOCKABLE_BACKBUFFER) != 0;
  back_buffer_surface_ = new MTLD3D9Surface(
      static_cast<IDirect3DDevice9 *>(this), back_buffer_texture_, present_.BackBufferFormat, D3DPOOL_DEFAULT, lockable,
      context_, D3DUSAGE_RENDERTARGET, false, nullptr, 0,
      swap_chain_ ? static_cast<IDirect3DSwapChain9 *>(swap_chain_) : nullptr, 0, nullptr, 0, true
  );
  if (lockable) {
    D3D11_TEXTURE2D_DESC desc{};
    buffer->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC staging_desc = desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
    staging_desc.MiscFlags = 0;
    staging_desc.SampleDesc.Count = 1;
    staging_desc.SampleDesc.Quality = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (SUCCEEDED(d3d11_device_->CreateTexture2D(&staging_desc, nullptr, &staging)))
      back_buffer_surface_->set_render_target_staging(staging);
  }
  // D3D9 makes the implicit back buffer at reference count 0: the application's first GetRenderTarget/GetBackBuffer is
  // what gives it its one device reference (Wine's test_refcount)
  back_buffer_surface_->set_implicit();
  return S_OK;
}

// what a frame is drawn into: the surface SetRenderTarget named, or the back buffer when it named none. a target of
// the app's is a surface of this device's, so it outlives the reference the app holds.
HRESULT
MTLD3D9Device::ensure_render_target() {
  if (render_target_views_[0])
    return S_OK;
  HRESULT hr = ensure_back_buffer();
  if (FAILED(hr))
    return hr;
  release_render_target(0);
  render_targets_[0] = static_cast<IDirect3DSurface9 *>(back_buffer_surface_);
  render_target_views_[0] = render_target_;
  // a frame drawn before SetRenderTarget names another has the one target
  for (UINT at = 1; at < kMaxRenderTargets; at++) {
    release_render_target(at);
    render_target_views_[at].Reset();
  }
  render_target_count_ = 1;
  target_width_ = width_;
  target_height_ = height_;
  // the chain's back buffer is the first target, and a multisampled chain's is multisampled: its automatic depth
  // surface has that count too, so Reset must establish the count here rather than leave the default
  target_samples_ = multisampled_chain() ? (UINT)present_.MultiSampleType : 1;
  // D3D9's scissor starts as the whole target
  if (!scissor_rect_.right && !scissor_rect_.bottom)
    scissor_rect_ = RECT{0, 0, (LONG)width_, (LONG)height_};
  return S_OK;
}

// D3D9's lost device: GPU removal, or a fullscreen classic device that lost foreground focus (Wine device.c).
// D3D9Ex does not use cooperative Lost for focus (Wine d3d9ex.c test_lost_device).
HRESULT
MTLD3D9Device::lost_reason() const {
  if (!extended_ && (cooperative_ == Cooperative::Lost || cooperative_ == Cooperative::NotReset))
    return D3DERR_DEVICELOST;
  if (!d3d11_device_)
    return S_OK;
  return FAILED(d3d11_device_->GetDeviceRemovedReason()) ? D3DERR_DEVICELOST : S_OK;
}

bool
MTLD3D9Device::exclusive_occluded() const {
  if (present_.Windowed)
    return false;
  const HWND focus = window_ && IsWindow(window_) ? window_ : present_hwnd();
  const HWND foreground = GetForegroundWindow();
  return focus && foreground && foreground != focus;
}

HRESULT
MTLD3D9Device::TestCooperativeLevel() {
  if (d3d11_device_ && FAILED(d3d11_device_->GetDeviceRemovedReason())) {
    cooperative_ = Cooperative::NotReset;
    return D3DERR_DEVICENOTRESET;
  }
  // D3D9Ex never reports DEVICELOST from focus loss (Wine d3d9ex.c test_lost_device).
  if (extended_) {
    cooperative_ = Cooperative::Ok;
    return D3D_OK;
  }
  // Windowed classic devices are not lost when focus moves (Wine device.c).
  if (present_.Windowed) {
    cooperative_ = Cooperative::Ok;
    return D3D_OK;
  }
  const HWND focus = window_ && IsWindow(window_) ? window_ : present_hwnd();
  const HWND foreground = GetForegroundWindow();
  if (cooperative_ == Cooperative::Lost) {
    if (focus && foreground == focus) {
      cooperative_ = Cooperative::NotReset;
      return D3DERR_DEVICENOTRESET;
    }
    return D3DERR_DEVICELOST;
  }
  if (cooperative_ == Cooperative::NotReset)
    return D3DERR_DEVICENOTRESET;
  // Defer focus-loss until after the first Present so CreateDevice+TCL while the harness is unfocused stays OK.
  if (presented_once_ && focus && foreground && foreground != focus) {
    cooperative_ = Cooperative::Lost;
    notify_focus_lost();
    return D3DERR_DEVICELOST;
  }
  return D3D_OK;
}

HRESULT
MTLD3D9Device::SetPaletteEntries(UINT palette_idx, const PALETTEENTRY *entries) {
  if (palette_idx > 255 || !entries)
    return D3DERR_INVALIDCALL;
  // Without ALPHAPALETTE, peFlags must be 0 or 0xff per entry (Wine device.c palette test).
  if (!(caps_.TextureCaps & D3DPTEXTURECAPS_ALPHAPALETTE)) {
    for (UINT at = 0; at < 256; at++) {
      if (entries[at].peFlags != 0 && entries[at].peFlags != 0xff)
        return D3DERR_INVALIDCALL;
    }
  }
  memcpy(palettes_[palette_idx], entries, sizeof(palettes_[palette_idx]));
  return D3D_OK;
}

HRESULT
MTLD3D9Device::GetPaletteEntries(UINT palette_idx, PALETTEENTRY *entries) {
  if (palette_idx > 255 || !entries)
    return D3DERR_INVALIDCALL;
  memcpy(entries, palettes_[palette_idx], sizeof(palettes_[palette_idx]));
  return D3D_OK;
}

HRESULT
MTLD3D9Device::SetCurrentTexturePalette(UINT palette_idx) {
  if (palette_idx > 255)
    return D3DERR_INVALIDCALL;
  current_palette_ = palette_idx;
  return D3D_OK;
}

HRESULT
MTLD3D9Device::GetCurrentTexturePalette(UINT *palette_idx) {
  if (!palette_idx)
    return D3DERR_INVALIDCALL;
  *palette_idx = current_palette_;
  return D3D_OK;
}

HRESULT
MTLD3D9Device::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS *parameters, IDirect3DSwapChain9 **chain) {
  if (!parameters || !chain)
    return D3DERR_INVALIDCALL;
  *chain = nullptr;
  if (HRESULT lost = lost_reason(); FAILED(lost))
    return lost;
  // an additional chain is a windowed one: a fullscreen device presents its own chain only, and D3D9 refuses to make
  // another (Microsoft Learn, IDirect3DDevice9::CreateAdditionalSwapChain: "This method fails when the device is in
  // fullscreen mode"; Wine's device test asks after a fullscreen Reset)
  if (!present_.Windowed)
    return D3DERR_INVALIDCALL;
  HWND window = parameters->hDeviceWindow ? parameters->hDeviceWindow : present_hwnd();
  // D3D9 reports the window the chain actually presents on, including the device's when the caller left it NULL
  // (Wine's device test: GetPresentParameters of a chain made with hDeviceWindow NULL returns the device window)
  parameters->hDeviceWindow = window;
  auto extra = std::make_unique<MTLD3D9AdditionalPresent>();
  HRESULT hr = extra->init(d3d11_device_.Get(), context_.Get(), factory_.Get(), window, *parameters,
                           static_cast<IDirect3DDevice9 *>(this));
  if (FAILED(hr)) {
    ERR_ONCE("D3D9: CreateAdditionalSwapChain needs a windowed DISCARD chain without MSAA.");
    return hr;
  }
  // an additional chain holds one counted device reference and is owned by the application alone: the device does
  // not keep a public ComPtr on it (that would leave the chain at refcount 2 and hide the device's own count). Reset
  // still clears any chains the application left behind through the weak list below.
  ComPtr<MTLD3D9SwapChain> result;
  result.Attach(new MTLD3D9SwapChain(
      static_cast<IDirect3DDevice9 *>(this), nullptr, *parameters, std::move(extra), extended_
  ));
  additional_chains_.push_back(result.Get());
  *chain = result.Detach();
  return S_OK;
}

HRESULT
MTLD3D9Device::present(const RECT *source, const RECT *, HWND window, const RGNDATA *, DWORD flags) {
  if (!swapchain_)
    return D3DERR_INVALIDCALL;
  if (HRESULT lost = lost_reason(); FAILED(lost))
    return lost;
  if (window && window != window_ && !IsWindow(window))
    return D3DERR_INVALIDCALL;
  if (flags & ~(DWORD)D3DPRESENT_DONOTWAIT) {
    ERR_ONCE("D3D9: Present flags 0x", std::hex, flags, " are not honoured yet (D3DPRESENT_DONOTWAIT is).");
    return D3DERR_INVALIDCALL;
  }

  // what was cleared and drawn into goes to the window before the chain turns
  // over
  context_->Flush();
  DXGI_PRESENT_PARAMETERS dirty{};
  RECT rect{};
  if (source) {
    rect = *source;
    dirty.DirtyRectsCount = 1;
    dirty.pDirtyRects = &rect;
  }

  // The buffer the chain shows. A flip model chain is never multisampled, so a multisampled back buffer is the device's
  // own texture and Present resolves it into the chain's buffer 0, which is the frame shown (Microsoft Learn,
  // D3DPRESENT_PARAMETERS::MultiSampleType: the multisampled back buffer is resolved when it is presented).
  ComPtr<ID3D11Texture2D> shown;
  const HRESULT have_back_buffer = ensure_back_buffer();
  if (multisampled_chain()) {
    if (FAILED(have_back_buffer) || FAILED(swapchain_->GetBuffer(0, IID_PPV_ARGS(&shown))))
      return D3DERR_INVALIDCALL;
    D3D11_TEXTURE2D_DESC shown_desc{};
    shown->GetDesc(&shown_desc);
    context_->ResolveSubresource(shown.Get(), 0, back_buffer_texture_.Get(), 0, shown_desc.Format);
  } else {
    shown = back_buffer_texture_;
  }
  // the frame being shown is kept: it is what GetFrontBufferData reads, and what a D3DSWAPEFFECT_COPY chain's back buffer
  // is put back from
  if (shown)
    front_.capture(shown.Get());

  UINT dxgi_flags = (flags & D3DPRESENT_DONOTWAIT) ? DXGI_PRESENT_DO_NOT_WAIT : 0;
  HRESULT hr = swapchain_->Present1(
      to_sync_interval(present_.PresentationInterval), dxgi_flags, source ? &dirty : nullptr
  );
  if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
    return D3DERR_WASSTILLDRAWING;
  if (FAILED(hr)) {
    ERR("D3D9: Present failed: 0x", std::hex, hr);
    return (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) ? D3DERR_DEVICELOST : D3DERR_INVALIDCALL;
  }
  // the next frame is drawn into the buffer this frame's view addresses: the chain turns its buffers over by giving
  // this buffer the next buffer's storage, so this device keeps its view and surface of buffer 0 across a Present.
  //
  // What that next buffer holds is D3DSWAPEFFECT_DISCARD's: nothing the application may rely on, which is also what
  // D3DSWAPEFFECT_FLIP leaves (the buffers are rotated and the one drawn into next is an older frame). A
  // D3DSWAPEFFECT_COPY chain's Present leaves the back buffer as it was (Microsoft Learn, D3DSWAPEFFECT: "the contents
  // of the back buffer are not affected"), so the frame shown is put back. A multisampled back buffer is the device's
  // own texture that the chain only resolves from, so it is untouched whatever the effect.
  if (present_.SwapEffect == D3DSWAPEFFECT_COPY && !multisampled_chain() && back_buffer_texture_ && front_.copy())
    context_->CopyResource(back_buffer_texture_.Get(), front_.copy());
  presented_once_ = true;
  // D3D9Ex reports occlusion as a success code rather than DEVICELOST (Wine d3d9ex.c test_lost_device).
  if (extended_ && exclusive_occluded())
    return S_PRESENT_OCCLUDED;
  return S_OK;
}

HRESULT
MTLD3D9Device::PresentEx(const RECT *source, const RECT *destination, HWND window, const RGNDATA *dirty, DWORD flags) {
  return present(source, destination, window, dirty, flags);
}

HRESULT
MTLD3D9Device::CheckDeviceState(HWND destination_window) {
  if (d3d11_device_ && FAILED(d3d11_device_->GetDeviceRemovedReason()))
    return D3DERR_DEVICELOST;
  if (!extended_)
    return lost_reason();
  // Wine d3d9 device.c d3d9_device_CheckDeviceState: a windowed chain is never occluded. A fullscreen one is occluded
  // while it does not own the desktop, reported the other way round for a destination that is not its device window.
  if (present_.Windowed)
    return D3D_OK;
  // Wine compares the destination with the device window as it is: a NULL destination is not the device window, so a
  // healthy device reports it occluded and a hidden one reports it fine (d3d9ex.c test_lost_device passes NULL).
  const bool device_ok = !exclusive_occluded();
  if (destination_window != present_hwnd())
    return device_ok ? S_PRESENT_OCCLUDED : D3D_OK;
  return device_ok ? D3D_OK : S_PRESENT_OCCLUDED;
}

// The last frame the implicit chain presented, copied into a system-memory surface the way GetRenderTargetData copies a
// render target (Microsoft Learn, IDirect3DDevice9::GetFrontBufferData). Before the first Present there is no frame to
// read, which is D3DERR_INVALIDCALL.
HRESULT
MTLD3D9Device::front_buffer_data(IDirect3DSurface9 *destination) {
  if (HRESULT lost = lost_reason(); FAILED(lost))
    return lost;
  return front_.read(context_.Get(), present_.BackBufferFormat, destination);
}

HRESULT
MTLD3D9Device::back_buffer(UINT index, IDirect3DSurface9 **surface) {
  if (!surface)
    return D3DERR_INVALIDCALL;
  *surface = nullptr;
  const UINT count = std::max<UINT>(present_.BackBufferCount, 1);
  if (index >= count)
    return D3DERR_INVALIDCALL;
  HRESULT hr = ensure_back_buffer();
  if (FAILED(hr))
    return hr;
  if (index == 0) {
    *surface = static_cast<IDirect3DSurface9 *>(back_buffer_surface_);
    (*surface)->AddRef();
    return S_OK;
  }
  // Additional back buffers of a multi-buffer chain (Wine visual.c test_flip): wrap DXGI GetBuffer(i), or a
  // same-sized DEFAULT RT when the Metal/DXGI chain only exposes buffer 0.
  if (extra_back_buffers_.size() < count)
    extra_back_buffers_.resize(count);
  if (!extra_back_buffers_[index]) {
    ComPtr<ID3D11Texture2D> buffer;
    if (!swapchain_ || FAILED(swapchain_->GetBuffer(index, IID_PPV_ARGS(&buffer)))) {
      D3D11_TEXTURE2D_DESC desc{};
      desc.Width = width_ ? width_ : present_.BackBufferWidth;
      desc.Height = height_ ? height_ : present_.BackBufferHeight;
      desc.MipLevels = 1;
      desc.ArraySize = 1;
      desc.Format = to_dxgi_format(present_.BackBufferFormat);
      desc.SampleDesc.Count = 1;
      desc.Usage = D3D11_USAGE_DEFAULT;
      desc.BindFlags = D3D11_BIND_RENDER_TARGET;
      if (!desc.Width || !desc.Height || desc.Format == DXGI_FORMAT_UNKNOWN ||
          FAILED(d3d11_device_->CreateTexture2D(&desc, nullptr, &buffer)))
        return D3DERR_INVALIDCALL;
    }
    const bool lockable = (present_.Flags & D3DPRESENTFLAG_LOCKABLE_BACKBUFFER) != 0;
    extra_back_buffers_[index] = new MTLD3D9Surface(
        static_cast<IDirect3DDevice9 *>(this), buffer, present_.BackBufferFormat, D3DPOOL_DEFAULT, lockable, context_,
        D3DUSAGE_RENDERTARGET, false, nullptr, 0,
        swap_chain_ ? static_cast<IDirect3DSwapChain9 *>(swap_chain_) : nullptr, 0, nullptr, 0, true
    );
    extra_back_buffers_[index]->set_implicit();
    if (lockable) {
      D3D11_TEXTURE2D_DESC desc{};
      buffer->GetDesc(&desc);
      D3D11_TEXTURE2D_DESC staging_desc = desc;
      staging_desc.Usage = D3D11_USAGE_STAGING;
      staging_desc.BindFlags = 0;
      staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
      staging_desc.MiscFlags = 0;
      staging_desc.SampleDesc.Count = 1;
      staging_desc.SampleDesc.Quality = 0;
      ComPtr<ID3D11Texture2D> staging;
      if (SUCCEEDED(d3d11_device_->CreateTexture2D(&staging_desc, nullptr, &staging)))
        extra_back_buffers_[index]->set_render_target_staging(staging);
    }
  }
  *surface = static_cast<IDirect3DSurface9 *>(extra_back_buffers_[index]);
  (*surface)->AddRef();
  return S_OK;
}

HRESULT
MTLD3D9Device::display_mode(D3DDISPLAYMODE *mode) {
  if (!mode)
    return D3DERR_INVALIDCALL;
  // Fullscreen devices report the back-buffer mode, not the desktop (Wine device.c test_display_modes).
  if (!present_.Windowed && width_ && height_) {
    mode->Width = width_;
    mode->Height = height_;
    mode->RefreshRate = 60;
    // Fullscreen display mode format is the adapter format (X8R8G8B8), not the back-buffer alpha variant.
    mode->Format = D3DFMT_X8R8G8B8;
    return S_OK;
  }
  return direct3d_->GetAdapterDisplayMode(adapter_ordinal_, mode);
}

// the chains in the order D3D9 indexes them: the implicit one is 0 and CreateAdditionalSwapChain's follow it
IDirect3DSwapChain9 *
MTLD3D9Device::swap_chain_at(UINT index) {
  if (index == 0)
    return swap_chain_ ? static_cast<IDirect3DSwapChain9 *>(swap_chain_) : nullptr;
  if (index - 1 < additional_chains_.size())
    return additional_chains_[index - 1];
  return nullptr;
}

HRESULT
MTLD3D9Device::display_mode_at(UINT swap_chain, D3DDISPLAYMODE *mode) {
  if (!mode)
    return D3DERR_INVALIDCALL;
  if (swap_chain == 0)
    return display_mode(mode);
  IDirect3DSwapChain9 *chain = swap_chain_at(swap_chain);
  if (!chain)
    return D3DERR_INVALIDCALL;
  return chain->GetDisplayMode(mode);
}

// D3D9Ex's raster status: no Mac API reports where the display's raster is, so the blank and the scan line are counted
// from the refresh the display mode names, which is how DXGI's own WaitForVBlank estimates it (src/dxgi/dxgi_output.cpp).
// The last tenth of the period is the blank, and the lines before it are the visible ones (Microsoft Learn,
// D3DRASTER_STATUS)
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetRasterStatus(UINT swap_chain, D3DRASTER_STATUS *status) {
  if (!status || !swap_chain_at(swap_chain))
    return D3DERR_INVALIDCALL;
  D3DDISPLAYMODE mode{};
  if (FAILED(display_mode_at(swap_chain, &mode)))
    return D3DERR_INVALIDCALL;
  const double hz = mode.RefreshRate > 1 ? mode.RefreshRate : 60.0;
  const std::chrono::duration<double> period(1.0 / hz);
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const double phase = std::fmod(std::chrono::duration<double>(now).count(), period.count()) / period.count();
  const bool blank = phase >= 0.9;
  status->InVBlank = blank ? TRUE : FALSE;
  const double visible = blank ? 1.0 : phase / 0.9;
  const UINT height = mode.Height ? mode.Height : 1;
  status->ScanLine = (UINT)std::min<double>(visible * height, height - 1);
  return S_OK;
}

// Waits for the display's next vertical blank through the output's own call, which is the API Metal's window server
// path backs (Microsoft Learn, IDXGIOutput::WaitForVBlank); an adapter without an output, or a Mac whose output does
// not report it, waits out the refresh the display mode names
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::WaitForVBlank(UINT swap_chain) {
  if (!swap_chain_at(swap_chain))
    return D3DERR_INVALIDCALL;
  ComPtr<IDXGIOutput> output;
  if (adapter_ && SUCCEEDED(adapter_->EnumOutputs(0, &output)) && output)
    return output->WaitForVBlank();
  D3DDISPLAYMODE mode{};
  if (FAILED(display_mode_at(swap_chain, &mode)))
    return D3DERR_INVALIDCALL;
  const double hz = mode.RefreshRate > 1 ? mode.RefreshRate : 60.0;
  const std::chrono::duration<double> period(1.0 / hz);
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const auto next = std::chrono::steady_clock::time_point(
      std::chrono::duration_cast<std::chrono::steady_clock::duration>((std::floor(now / period) + 1) * period)
  );
  std::this_thread::sleep_until(next);
  return S_OK;
}

// A resource of this device is a Metal allocation the GPU reads in place, without a residency transition, so it is
// resident: the call answers S_OK for the resources this device made, and refuses a null entry or another device's
// rather than answering for a lifetime it does not own (Microsoft Learn, IDirect3DDevice9Ex::CheckResourceResidency)
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CheckResourceResidency(IDirect3DResource9 **resources, UINT32 count) {
  if (!resources && count)
    return D3DERR_INVALIDCALL;
  if (HRESULT lost = lost_reason(); FAILED(lost))
    return lost;
  for (UINT32 at = 0; at < count; at++) {
    if (!resources[at])
      return D3DERR_INVALIDCALL;
    ComPtr<IDirect3DDevice9> owner;
    if (FAILED(resources[at]->GetDevice(&owner)) || owner.Get() != static_cast<IDirect3DDevice9 *>(this))
      return D3DERR_INVALIDCALL;
  }
  return S_OK;
}

// the working set the Metal device recommends less what its textures have already taken (IDXGIAdapter3::QueryVideoMemoryInfo,
// which is the same answer D3D12's drivers give): D3D9's own drivers answer this question with the memory their
// allocations may still take, which is what a title reads to choose its texture sizes
UINT
MTLD3D9Device::GetAvailableTextureMem() {
  // Wine device.c test_vidmem_accounting expects CreateTexture(DEFAULT) to reduce this figure by tens of MB.
  // DXGI QueryVideoMemoryInfo on Metal often reports Budget==CurrentUsage (constant zero free) or a free pool that
  // does not move with D3D9 DEFAULT allocations, so the figure is a synthetic 2 GiB pool minus tracked DEFAULT bytes.
  // D3D9Ex has no D3DERR_OUTOFVIDEOMEMORY and reports a rough, unchanging figure (Wine d3d9ex.c
  // test_vidmem_accounting: creating 80 MB of textures changes it by less than 1 MB), so only classic devices track.
  constexpr uint64_t kPool = 2048ull << 20;
  const uint64_t used = extended_ ? 0 : tracked_texture_bytes_;
  if (used >= kPool)
    return 0;
  return (UINT)std::min<uint64_t>(kPool - used, UINT_MAX);
}

HRESULT
MTLD3D9Device::source_surface(IDirect3DSurface9 *surface, IMTLD3D9Surface **out) {
  *out = nullptr;
  if (!surface)
    return D3DERR_INVALIDCALL;
  ComPtr<IMTLD3D9Surface> ours;
  if (FAILED(surface->QueryInterface(IID_IMTLD3D9Surface, reinterpret_cast<void **>(ours.GetAddressOf()))) ||
      ours->lockable() || ours->depth_stencil())
    return D3DERR_INVALIDCALL;
  *out = ours.Detach();
  return S_OK;
}

HRESULT
MTLD3D9Device::staging_surface(IDirect3DSurface9 *surface, IMTLD3D9Surface **out) {
  *out = nullptr;
  if (!surface)
    return D3DERR_INVALIDCALL;
  ComPtr<IMTLD3D9Surface> ours;
  if (FAILED(surface->QueryInterface(IID_IMTLD3D9Surface, reinterpret_cast<void **>(ours.GetAddressOf()))) ||
      !ours->lockable())
    return D3DERR_INVALIDCALL;
  *out = ours.Detach();
  return S_OK;
}

// a frame's bytes read back: the copy of a render target into a surface whose
// bytes are the CPU's, which the surface then locks (GetRenderTargetData)
HRESULT
MTLD3D9Device::GetRenderTargetData(IDirect3DSurface9 *render_target, IDirect3DSurface9 *destination) {
  ComPtr<IMTLD3D9Surface> source, target;
  if (FAILED(source_surface(render_target, &source)) || FAILED(staging_surface(destination, &target)))
    return D3DERR_INVALIDCALL;

  D3D11_TEXTURE2D_DESC source_desc, target_desc;
  source->texture()->GetDesc(&source_desc);
  target->texture()->GetDesc(&target_desc);
  // a multisampled target has no pixels to read until it is resolved (StretchRect onto a single-sample surface does);
  // D3D9 refuses it (Microsoft Learn, IDirect3DDevice9::GetRenderTargetData: "the source ... cannot be multisampled")
  if (source_desc.SampleDesc.Count > 1) {
    ERR_ONCE("D3D9: GetRenderTargetData of a multisampled render target; resolve it with StretchRect first.");
    return D3DERR_INVALIDCALL;
  }
  // a level's size is the texture's shifted down to that level: the destination is that level's size
  const UINT source_width = std::max(1u, source_desc.Width >> source->level());
  const UINT source_height = std::max(1u, source_desc.Height >> source->level());
  if (source_width != target_desc.Width || source_height != target_desc.Height ||
      source_desc.Format != target_desc.Format)
    return D3DERR_INVALIDCALL;

  // the surface's own level: a level of a mipmapped texture is read back as that level, not as the top one. the
  // destination surface is the level's size and format, which is what a subresource copy needs (Microsoft Learn,
  // ID3D11DeviceContext::CopySubresourceRegion)
  context_->CopySubresourceRegion(target->texture(), 0, 0, 0, 0, source->texture(), source->level(), nullptr);
  return S_OK;
}

HRESULT
MTLD3D9Device::CreateOffscreenPlainSurface(UINT width, UINT height, D3DFORMAT format, D3DPOOL pool,
                                           IDirect3DSurface9 **surface, HANDLE *shared_handle) {
  if (!surface)
    return D3DERR_INVALIDCALL;
  *surface = nullptr;
  if (shared_handle && !extended_)
    return E_NOTIMPL;
  const bool user_memory = shared_handle && *shared_handle && pool == D3DPOOL_SYSTEMMEM;
  if (shared_handle && !user_memory)
    return D3DERR_INVALIDCALL;
  // SYSTEMMEM and SCRATCH are the CPU's (Microsoft Learn, D3DPOOL_SCRATCH: "Resources are kept off the device and are
  // not accessible by Direct3D ... The application can lock the memory"). DEFAULT is the device's: a render target
  // ColorFill fills, or the destination StretchRect and UpdateSurface write. A default surface takes a render target
  // view when its format can, which is what ColorFill needs (an offscreen surface of a compressed format has none and
  // is only a copy's source).
  if (pool != D3DPOOL_SYSTEMMEM && pool != D3DPOOL_DEFAULT && pool != D3DPOOL_SCRATCH)
    return D3DERR_INVALIDCALL;
  // a default-pool surface is the device's, and the device the frames failed on is replaced by a Reset
  if (pool == D3DPOOL_DEFAULT) {
    if (HRESULT lost = lost_reason(); FAILED(lost))
      return lost;
  }
  DXGI_FORMAT dxgi_format = to_dxgi_format(format);
  // YUY2/UYVY SCRATCH plains are CPU-authored without driver support (Wine test_surface_blocks).
  const bool yuv_cpu = (format == D3DFMT_YUY2 || format == D3DFMT_UYVY) && pool == D3DPOOL_SCRATCH;
  if (yuv_cpu)
    dxgi_format = DXGI_FORMAT_R8G8_UNORM;
  if (dxgi_format == DXGI_FORMAT_UNKNOWN || !width || !height)
    return D3DERR_INVALIDCALL;
  // Compressed offscreen surfaces follow the same whole-block size rule as textures (Wine device.c
  // test_surface_blocks / create_size_checked).
  if (block_size(format) && ((width & 3) || (height & 3)))
    return D3DERR_INVALIDCALL;

  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = dxgi_format;
  desc.SampleDesc.Count = 1;
  const bool cpu_pool = pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH;
  if (cpu_pool) {
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
  } else {
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  }
  ComPtr<ID3D11Texture2D> texture;
  HRESULT hr = d3d11_device_->CreateTexture2D(&desc, nullptr, &texture);
  if (FAILED(hr) && pool == D3DPOOL_DEFAULT) {
    // the format takes no render target view: the surface is a copy's source or destination, not a fill's
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    hr = d3d11_device_->CreateTexture2D(&desc, nullptr, &texture);
    if (FAILED(hr)) {
      desc.BindFlags = 0;
      hr = d3d11_device_->CreateTexture2D(&desc, nullptr, &texture);
    }
  }
  if (FAILED(hr))
    return D3DERR_INVALIDCALL;

  // DEFAULT offscreen plains are lockable on Windows/Wine (test_resource_access SURFACE_PLAIN): keep a staging twin
  // for LockRect while the DEFAULT texture remains ColorFill/StretchRect-capable.
  ComPtr<ID3D11Texture2D> staging;
  if (pool == D3DPOOL_DEFAULT) {
    D3D11_TEXTURE2D_DESC staging_desc = desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
    staging_desc.MiscFlags = 0;
    if (FAILED(d3d11_device_->CreateTexture2D(&staging_desc, nullptr, &staging)))
      return D3DERR_INVALIDCALL;
  }

  ComPtr<MTLD3D9Surface> result;
  result.Attach(new MTLD3D9Surface(static_cast<IDirect3DDevice9 *>(this), texture, format, pool, cpu_pool, context_, 0,
                                   false, nullptr, 0, nullptr, 0,
                                   user_memory ? reinterpret_cast<void *>(*shared_handle) : nullptr,
                                   user_memory ? (format == D3DFMT_L8 ? width : width * 4) : 0, false, true));
  if (staging)
    result->set_render_target_staging(staging);
  *surface = result.Detach();
  return S_OK;
}


HRESULT
create_device(IDirect3D9 *direct3d, IDXGIFactory2 *factory, IDXGIAdapter1 *adapter, UINT adapter_ordinal,
              const D3DCAPS9 &caps, HWND window, DWORD behavior_flags, D3DPRESENT_PARAMETERS present,
              IDirect3DDevice9 **device, bool extended) {
  if (!device || !factory || !adapter)
    return D3DERR_INVALIDCALL;
  *device = nullptr;

  // Wine device.c test_swapchain_parameters: swap effect 0, Ex-only effects on non-Ex, COPY with >1 back buffer,
  // and more than three back buffers are refused at CreateDevice (and Reset).
  const DWORD effect = (DWORD)present.SwapEffect;
  if (effect == 0 || effect == 4 /* OVERLAY */ || effect > 5 /* FLIPEX + 1 */)
    return D3DERR_INVALIDCALL;
  if (!extended && effect == 5 /* D3DSWAPEFFECT_FLIPEX */)
    return D3DERR_INVALIDCALL;
  if (effect == D3DSWAPEFFECT_COPY && present.BackBufferCount > 1)
    return D3DERR_INVALIDCALL;
  if (present.BackBufferCount > (extended ? 30u : 3u))
    return D3DERR_INVALIDCALL;
  // PresentationInterval must be DEFAULT or a bit advertised in caps (Wine device.c interval loop).
  {
    const DWORD interval = present.PresentationInterval;
    const bool interval_ok =
        interval == D3DPRESENT_INTERVAL_DEFAULT ||
        ((interval == D3DPRESENT_INTERVAL_ONE || interval == D3DPRESENT_INTERVAL_TWO ||
          interval == D3DPRESENT_INTERVAL_THREE || interval == D3DPRESENT_INTERVAL_FOUR ||
          interval == D3DPRESENT_INTERVAL_IMMEDIATE) &&
         (caps.PresentationIntervals & interval) != 0);
    if (!interval_ok)
      return D3DERR_INVALIDCALL;
  }

  // D3D9's windowed defaults: D3DFMT_UNKNOWN is the desktop format, and a zero width or height is the window's client
  // area. Resolve both before the device validates the chain, as the test harness and ordinary games give these values.
  D3DPRESENT_PARAMETERS resolved = present;
  if (resolved.BackBufferFormat == D3DFMT_UNKNOWN)
    resolved.BackBufferFormat = D3DFMT_X8R8G8B8;
  // CreateDevice(NULL focus) / hDeviceWindow NULL still creates a device (Wine visual.c / d3d9ex.c test_desktop_window).
  // Bind presentation to a private popup of the requested back-buffer size when no usable HWND is given.
  HWND focus = window;
  bool owns_focus = false;
  if (!focus || !IsWindow(focus)) {
    if (!resolved.BackBufferWidth)
      resolved.BackBufferWidth = 640;
    if (!resolved.BackBufferHeight)
      resolved.BackBufferHeight = 480;
    static ATOM null_hwnd_class = 0;
    if (!null_hwnd_class) {
      WNDCLASSEXW wc{};
      wc.cbSize = sizeof(wc);
      wc.lpfnWndProc = DefWindowProcW;
      wc.hInstance = GetModuleHandleW(nullptr);
      wc.lpszClassName = L"MullionD3D9NullFocus";
      null_hwnd_class = RegisterClassExW(&wc);
    }
    RECT client{0, 0, (LONG)resolved.BackBufferWidth, (LONG)resolved.BackBufferHeight};
    AdjustWindowRectEx(&client, WS_POPUP, FALSE, 0);
    focus = CreateWindowExW(0, L"MullionD3D9NullFocus", L"", WS_POPUP, 0, 0, client.right - client.left,
                            client.bottom - client.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!focus)
      return D3DERR_INVALIDCALL;
    owns_focus = true;
  }
  if ((!resolved.BackBufferWidth || !resolved.BackBufferHeight) && focus) {
    RECT client{};
    if (GetClientRect(focus, &client)) {
      if (!resolved.BackBufferWidth)
        resolved.BackBufferWidth = (UINT)(client.right - client.left);
      if (!resolved.BackBufferHeight)
        resolved.BackBufferHeight = (UINT)(client.bottom - client.top);
    }
  }

  ComPtr<MTLD3D9Device> result;
  result.Attach(new MTLD3D9Device(direct3d, ComPtr<IDXGIAdapter1>(adapter), adapter_ordinal, caps, focus,
                                  behavior_flags, resolved, extended));
  if (owns_focus)
    result->adopt_swapchain_window(focus);
  HRESULT hr = result->init(factory);
  if (FAILED(hr)) {
    ERR("D3D9: create_device's device did not start: 0x", std::hex, hr);
    return hr;
  }
  *device = result.Detach();
  return S_OK;
}

HRESULT
MTLD3D9Device::SetCursorProperties(UINT hotspot_x, UINT hotspot_y, IDirect3DSurface9 *cursor_bitmap) {
  if (!cursor_bitmap)
    return D3DERR_INVALIDCALL;
  ComPtr<IMTLD3D9Surface> ours;
  if (FAILED(cursor_bitmap->QueryInterface(IID_IMTLD3D9Surface, reinterpret_cast<void **>(ours.GetAddressOf()))))
    return D3DERR_INVALIDCALL;
  D3DSURFACE_DESC desc{};
  if (FAILED(cursor_bitmap->GetDesc(&desc)) || !desc.Width || !desc.Height)
    return D3DERR_INVALIDCALL;
  // cursor dimensions must be powers of two (Wine's test_cursor; Microsoft Learn, SetCursorProperties)
  auto power_of_two = [](UINT value) { return value && !(value & (value - 1)); };
  if (!power_of_two(desc.Width) || !power_of_two(desc.Height))
    return D3DERR_INVALIDCALL;
  // Size limit follows IDirect3D9::GetAdapterDisplayMode (desktop on Metal), not the exclusive back-buffer
  // size from IDirect3DDevice9::GetDisplayMode (Wine device.c test_cursor uses GetAdapterDisplayMode).
  D3DDISPLAYMODE mode{};
  if (FAILED(direct3d_->GetAdapterDisplayMode(adapter_ordinal_, &mode)) || desc.Width > mode.Width ||
      desc.Height > mode.Height)
    return D3DERR_INVALIDCALL;

  HCURSOR built = nullptr;
  D3DLOCKED_RECT locked{};
  if (SUCCEEDED(cursor_bitmap->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = (LONG)desc.Width;
    bi.bV5Height = -(LONG)desc.Height;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    ICONINFO info{};
    info.fIcon = FALSE;
    info.xHotspot = hotspot_x;
    info.yHotspot = hotspot_y;
    void *dib_bits = nullptr;
    info.hbmColor = CreateDIBSection(nullptr, reinterpret_cast<BITMAPINFO *>(&bi), DIB_RGB_COLORS, &dib_bits, nullptr, 0);
    if (info.hbmColor && dib_bits) {
      const UINT pitch = ((desc.Width * 32 + 31) / 32) * 4;
      for (UINT row = 0; row < desc.Height; row++) {
        memcpy(static_cast<char *>(dib_bits) + (size_t)row * pitch,
               static_cast<const char *>(locked.pBits) + (size_t)row * locked.Pitch,
               std::min<size_t>(pitch, (size_t)desc.Width * 4));
      }
      info.hbmMask = CreateBitmap((int)desc.Width, (int)desc.Height, 1, 1, nullptr);
      built = CreateIconIndirect(&info);
      if (info.hbmMask)
        DeleteObject(info.hbmMask);
    }
    if (info.hbmColor)
      DeleteObject(info.hbmColor);
    cursor_bitmap->UnlockRect();
  }
  // Host cursor creation is best-effort under Wine; D3D9 still accepts the properties (Wine device.c test_cursor).
  if (cursor_handle_) {
    DestroyCursor(cursor_handle_);
    cursor_handle_ = nullptr;
  }
  cursor_handle_ = built;
  cursor_set_ = true;
  cursor_hotspot_x_ = hotspot_x;
  cursor_hotspot_y_ = hotspot_y;
  if (cursor_visible_ && cursor_handle_)
    SetCursor(cursor_handle_);
  return S_OK;
}

void
MTLD3D9Device::SetCursorPosition(int x, int y, DWORD) {
  cursor_x_ = x;
  cursor_y_ = y;
  SetCursorPos(x, y);
}

WINBOOL
MTLD3D9Device::ShowCursor(WINBOOL show) {
  const WINBOOL previous = cursor_visible_ ? TRUE : FALSE;
  // without a surface SetCursorProperties accepted, the cursor stays hidden (Wine's test_cursor)
  if (cursor_set_) {
    cursor_visible_ = show != FALSE;
    SetCursor(cursor_visible_ ? cursor_handle_ : nullptr);
  }
  return previous;
}

} // namespace mullion::d3d9
