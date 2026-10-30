// a Direct3D 9 device's own object: the D3D11 device and the window's implicit swap chain it presents through, the back
// buffer a frame is drawn into, and the CPU surfaces a frame is read back into. the render states, targets and
// depth-stencil surfaces are d3d9_device_targets.cpp's, the geometry a draw reads d3d9_device_draw.cpp's, and the
// class itself d3d9_device.hpp's.
#include "d3d9_device.hpp"
#include <algorithm>
#include <dxgi1_4.h>
#include <memory>

namespace mullion::d3d9 {

const IID IID_IMTLD3D9DeviceLifetime = {0x7f2b0a61, 0x4c3d, 0x4c8e, {0x9e, 0x2a, 0x51, 0x3b, 0x77, 0x0f, 0x6d, 0x12}};
const IID IID_IMTLD3D9DeviceResources = {0x2c8e5a94, 0x1b76, 0x4f0d, {0x83, 0x5a, 0x6d, 0x1c, 0x02, 0x9e, 0x47, 0x51}};

void
MTLD3D9Device::release_device_children() {
  for (UINT stream = 0; stream < kMaxStreams; stream++)
    clear_stream(stream);
  indices_.Reset();
  index_buffer_.Reset();
  vertex_shader_.Reset();
  pixel_shader_.Reset();
  declaration_.Reset();
  declaration_inputs_.Reset();
  for (auto &texture : textures_)
    texture.Reset();
  for (auto &texture : texture_sources_)
    texture.Reset();
  depth_stencil_.Reset();
  depth_stencil_view_.Reset();
  current_target_.Reset();
  current_target_view_.Reset();
  back_buffer_surface_.Reset();
  render_target_.Reset();
  back_buffer_texture_.Reset();
  swap_chain_.Reset();
  up_scratch_.Reset();
  up_index_scratch_.Reset();
  up_scratch_size_ = 0;
  up_index_scratch_size_ = 0;
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
  indices_.Reset();
  index_buffer_.Reset();
  up_scratch_.Reset();
  up_index_scratch_.Reset();
  up_scratch_size_ = 0;
  up_index_scratch_size_ = 0;
  vertex_shader_.Reset();
  pixel_shader_.Reset();
  declaration_.Reset();
  declaration_inputs_.Reset();
  fvf_ = 0;
  for (auto &texture : textures_)
    texture.Reset();
  for (auto &texture : texture_sources_)
    texture.Reset();
  for (auto &sampler : sampler_states_)
    sampler = SamplerState9();
  texture_stages_[0] = TextureStageState9();
  texture_stages_[1] = TextureStageState9(D3DTOP_DISABLE);
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
  if (parameters->MultiSampleType != D3DMULTISAMPLE_NONE) {
    ERR_ONCE("D3D9: Reset with a multisampled chain is not implemented yet.");
    return D3DERR_INVALIDCALL;
  }
  // the flip model chain this device makes is D3DSWAPEFFECT_DISCARD's; a COPY chain's Present leaves the back buffer
  // as it was, which this device's does not, so it is refused rather than presented as a discard's
  if (parameters->SwapEffect != D3DSWAPEFFECT_DISCARD) {
    ERR_ONCE("D3D9: Reset to a swap effect other than D3DSWAPEFFECT_DISCARD is not implemented yet.");
    return D3DERR_INVALIDCALL;
  }
  // the window a windowed chain presents to: the one the parameters name, or the one the device was made with. A window
  // that is not one is refused, as D3D9 refuses it (IDirect3DDevice9::Reset: "If Windowed is TRUE ... hDeviceWindow must
  // be a valid window"); presenting to a second window is not this build's yet
  HWND target_window = parameters->hDeviceWindow ? parameters->hDeviceWindow : window_;
  if (!IsWindow(target_window)) {
    ERR_ONCE("D3D9: Reset names a window that is not one.");
    return D3DERR_INVALIDCALL;
  }
  // a different window is a new DXGI chain: the old chain belongs to the HWND it was made for
  const bool new_window = target_window != window_;
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

  // a width or height of zero is the window's client area, as CreateDevice's is
  UINT width = parameters->BackBufferWidth, height = parameters->BackBufferHeight;
  if (!width || !height) {
    RECT client{};
    if (!window_ || !GetClientRect(window_, &client))
      return D3DERR_INVALIDCALL;
    if (!width)
      width = (UINT)(client.right - client.left);
    if (!height)
      height = (UINT)(client.bottom - client.top);
  }
  if (!width || !height)
    return D3DERR_INVALIDCALL;

  // A device whose frames failed is the one a Reset replaces, so nothing it holds is the application's to have released
  // first: the surfaces of the old device are gone with it. A live one keeps the references it has, and an application
  // reference above them would be left addressing storage the resize replaces
  const bool lost = FAILED(lost_reason());
  if (!lost) {
    const ULONG device_back_buffer_references =
        back_buffer_surface_
            ? 1 + (current_target_.Get() == static_cast<IDirect3DSurface9 *>(back_buffer_surface_.Get()) ? 1 : 0)
            : 0;
    if (back_buffer_surface_ && back_buffer_surface_->public_references() > device_back_buffer_references) {
      ERR_ONCE("D3D9: Reset needs every application reference to the implicit swap chain's back buffer released first.");
      return D3DERR_INVALIDCALL;
    }
    if (depth_stencil_ && depth_stencil_->public_references() > 1) {
      ERR_ONCE("D3D9: Reset needs every application reference to a depth-stencil surface released first.");
      return D3DERR_INVALIDCALL;
    }
  }

  if (lost) {
    // the chain the parameters describe, over the device the Reset makes: the fields the application left to the window
    // are the sizes it asked for, and a chain the device already had keeps its format
    present_ = *parameters;
    present_.hDeviceWindow = window_;
    present_.BackBufferWidth = width;
    present_.BackBufferHeight = height;
    present_.BackBufferFormat = format;
    present_.MultiSampleType = D3DMULTISAMPLE_NONE;
    present_.SwapEffect = D3DSWAPEFFECT_DISCARD;
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
  current_target_.Reset();
  current_target_view_.Reset();
  depth_stencil_.Reset();
  depth_stencil_view_.Reset();
  invalidate_back_buffer();

  const UINT buffer_count = std::max<UINT>(parameters->BackBufferCount, 2);
  HRESULT hr = S_OK;
  if (new_window) {
    swapchain_.Reset();
    window_ = target_window;
    present_ = *parameters;
    present_.hDeviceWindow = window_;
    present_.BackBufferWidth = width;
    present_.BackBufferHeight = height;
    present_.BackBufferFormat = format;
    present_.MultiSampleType = D3DMULTISAMPLE_NONE;
    present_.SwapEffect = D3DSWAPEFFECT_DISCARD;
    hr = create_chain(factory_.Get());
    if (FAILED(hr))
      return hr;
  } else {
    if (!parameters->Windowed)
      swapchain_->SetFullscreenState(TRUE, nullptr);
    else if (!present_.Windowed)
      swapchain_->SetFullscreenState(FALSE, nullptr);
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
    present_.hDeviceWindow = window_;
    present_.BackBufferWidth = created.Width;
    present_.BackBufferHeight = created.Height;
    present_.BackBufferCount = created.BufferCount;
    present_.BackBufferFormat = format;
    present_.MultiSampleType = D3DMULTISAMPLE_NONE;
    present_.SwapEffect = D3DSWAPEFFECT_DISCARD;
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
  reset_state();
  viewport_ = D3DVIEWPORT9{0, 0, width_, height_, 0.0f, 1.0f};
  scissor_rect_ = RECT{0, 0, (LONG)width_, (LONG)height_};
  if (present_.EnableAutoDepthStencil) {
    ComPtr<IDirect3DSurface9> auto_depth;
    HRESULT hr = CreateDepthStencilSurface(
        width_, height_, present_.AutoDepthStencilFormat, D3DMULTISAMPLE_NONE, 0, TRUE, &auto_depth, nullptr
    );
    if (SUCCEEDED(hr))
      hr = SetDepthStencilSurface(auto_depth.Get());
    if (FAILED(hr)) {
      ERR("D3D9: no automatic depth-stencil surface of format {} for the chain a Reset made: {}",
          (uint32_t)present_.AutoDepthStencilFormat, (uint32_t)hr);
      return D3DERR_INVALIDCALL;
    }
    state_.render().set(D3DRS_ZENABLE, TRUE);
  }
  parameters->BackBufferWidth = 0;
  parameters->BackBufferHeight = 0;
  parameters->BackBufferCount = 0;
  parameters->BackBufferFormat = D3DFMT_UNKNOWN;
  return S_OK;
}

// The chain a device presents through, over the device it has now. A DXGI chain belongs to the device that made it, so a
// Reset that recreates the device makes a new chain; the chain object an application may hold is the same one, and it
// presents this device's frames through the device it asks (IMTLD3D9PresentSource).
HRESULT
MTLD3D9Device::create_chain(IDXGIFactory2 *factory) {
  const DXGI_FORMAT format = to_dxgi_format(present_.BackBufferFormat);
  if (format == DXGI_FORMAT_UNKNOWN || !window_ || !IsWindow(window_) || !factory)
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
  desc.BufferCount = std::max<UINT>(present_.BackBufferCount, 2);
  desc.Scaling = DXGI_SCALING_STRETCH;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  desc.Flags = 0;

  DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen{};
  fullscreen.Windowed = present_.Windowed ? TRUE : FALSE;
  fullscreen.RefreshRate.Numerator = 0;
  fullscreen.RefreshRate.Denominator = 0;
  fullscreen.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
  fullscreen.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
  HRESULT hr = factory->CreateSwapChainForHwnd(
      d3d11_device_.Get(), window_, &desc, present_.Windowed ? nullptr : &fullscreen, nullptr, &swapchain_
  );
  if (FAILED(hr)) {
    ERR("D3D9: no swap chain for the window: 0x", std::hex, hr);
    return D3DERR_NOTAVAILABLE;
  }

  DXGI_SWAP_CHAIN_DESC1 created{};
  swapchain_->GetDesc1(&created);
  width_ = created.Width;
  height_ = created.Height;
  present_.BackBufferWidth = created.Width;
  present_.BackBufferHeight = created.Height;
  present_.BackBufferCount = created.BufferCount;
  present_.SwapEffect = D3DSWAPEFFECT_DISCARD;

  if (swap_chain_)
    swap_chain_->set_present_parameters(present_);
  else
    swap_chain_.Attach(new MTLD3D9SwapChain(
        static_cast<IDirect3DDevice9 *>(this), static_cast<IMTLD3D9PresentSource *>(this), present_
    ));
  return S_OK;
}

HRESULT
MTLD3D9Device::init(IDXGIFactory2 *factory) {
  if (to_dxgi_format(present_.BackBufferFormat) == DXGI_FORMAT_UNKNOWN || !window_ || !IsWindow(window_))
    return D3DERR_INVALIDCALL;
  if (present_.MultiSampleType != D3DMULTISAMPLE_NONE)
    return D3DERR_NOTAVAILABLE;

  factory_ = factory;
  HRESULT hr = D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                 nullptr, 0, D3D11_SDK_VERSION, &d3d11_device_, nullptr, &context_);
  if (FAILED(hr)) {
    ERR("D3D9: no D3D11 device over the adapter: 0x", std::hex, hr);
    return D3DERR_NOTAVAILABLE;
  }
  // the same context as the one that clears a part of a view; a context that is not the 11.1 one keeps every clear
  // whole, which is what the clears below fall back to
  context_.As(&context1_);

  // the fixed-function path a device draws through, made once with the device: a device without it can clear and
  // present, but not draw, and this milestone's device draws
  hr = fixed_function_.initialize(d3d11_device_.Get());
  if (FAILED(hr))
    return D3DERR_NOTAVAILABLE;

  hr = create_chain(factory);
  if (FAILED(hr))
    return hr;

  // the viewport of a device that has drawn nothing yet is its chain's whole back buffer, as D3D9's is
  viewport_ = D3DVIEWPORT9{0, 0, width_, height_, 0.0f, 1.0f};

  // a chain made with an automatic depth-stencil surface gets one, so a game that asked for depth has it from its
  // first frame rather than drawing every pixel in the order it drew them
  if (present_.EnableAutoDepthStencil) {
    ComPtr<IDirect3DSurface9> auto_depth;
    HRESULT depth_hr = CreateDepthStencilSurface(width_, height_, present_.AutoDepthStencilFormat,
                                                 D3DMULTISAMPLE_NONE, 0, TRUE, &auto_depth, nullptr);
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
  depth_stencil_.Reset();
  depth_stencil_view_.Reset();
  current_target_.Reset();
  current_target_view_.Reset();
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
  up_scratch_.Reset();
  up_index_scratch_.Reset();
  up_scratch_size_ = 0;
  up_index_scratch_size_ = 0;
  for (UINT stream = 0; stream < kMaxStreams; stream++)
    clear_stream(stream);
  indices_.Reset();
  index_buffer_.Reset();
  vertex_shader_.Reset();
  pixel_shader_.Reset();
  declaration_.Reset();
  declaration_inputs_.Reset();
  for (auto &texture : textures_)
    texture.Reset();
  for (auto &texture : texture_sources_)
    texture.Reset();
  fixed_function_.release_device();
  swapchain_.Reset();
  context1_.Reset();
  context_.Reset();
  d3d11_device_.Reset();

  HRESULT hr = D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                 nullptr, 0, D3D11_SDK_VERSION, &d3d11_device_, nullptr, &context_);
  if (FAILED(hr)) {
    ERR("D3D9: no Direct3D 11 device to reset onto: 0x", std::hex, hr);
    return D3DERR_NOTAVAILABLE;
  }
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
  HRESULT hr = swapchain_->GetBuffer(0, IID_PPV_ARGS(&buffer));
  if (FAILED(hr)) {
    ERR("D3D9: the swap chain has no buffer 0: 0x", std::hex, hr);
    return D3DERR_INVALIDCALL;
  }
  ComPtr<ID3D11RenderTargetView> view;
  hr = d3d11_device_->CreateRenderTargetView(buffer.Get(), nullptr, &view);
  if (FAILED(hr)) {
    ERR("D3D9: the back buffer takes no render target view: 0x", std::hex, hr);
    return D3DERR_INVALIDCALL;
  }

  back_buffer_texture_ = buffer;
  render_target_ = view;
  back_buffer_surface_.Attach(new MTLD3D9Surface(
      static_cast<IDirect3DDevice9 *>(this), back_buffer_texture_, present_.BackBufferFormat, D3DPOOL_DEFAULT, false,
      context_, 0, false, nullptr, 0,
      swap_chain_ ? static_cast<IDirect3DSwapChain9 *>(swap_chain_.Get()) : nullptr
  ));
  return S_OK;
}

// what a frame is drawn into: the surface SetRenderTarget named, or the back buffer when it named none. a target of
// the app's is a surface of this device's, so it outlives the reference the app holds.
HRESULT
MTLD3D9Device::ensure_render_target() {
  if (current_target_view_)
    return S_OK;
  HRESULT hr = ensure_back_buffer();
  if (FAILED(hr))
    return hr;
  current_target_ = back_buffer_surface_;
  current_target_view_ = render_target_;
  target_width_ = width_;
  target_height_ = height_;
  // D3D9's scissor starts as the whole target
  if (!scissor_rect_.right && !scissor_rect_.bottom)
    scissor_rect_ = RECT{0, 0, (LONG)width_, (LONG)height_};
  return S_OK;
}

// D3D9's lost device is the removal of the D3D11 device under it: the frames after a failure are not ones to draw or
// present, and the calls that do answer D3DERR_DEVICELOST until the application resets the device
// (IDirect3DDevice9::TestCooperativeLevel)
HRESULT
MTLD3D9Device::lost_reason() const {
  if (!d3d11_device_)
    return S_OK;
  return FAILED(d3d11_device_->GetDeviceRemovedReason()) ? D3DERR_DEVICELOST : S_OK;
}

HRESULT
MTLD3D9Device::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS *parameters, IDirect3DSwapChain9 **chain) {
  if (!parameters || !chain)
    return D3DERR_INVALIDCALL;
  *chain = nullptr;
  if (HRESULT lost = lost_reason(); FAILED(lost))
    return lost;
  HWND window = parameters->hDeviceWindow ? parameters->hDeviceWindow : window_;
  auto extra = std::make_unique<MTLD3D9AdditionalPresent>();
  HRESULT hr = extra->init(d3d11_device_.Get(), context_.Get(), factory_.Get(), window, *parameters,
                           static_cast<IDirect3DDevice9 *>(this));
  if (FAILED(hr)) {
    ERR_ONCE("D3D9: CreateAdditionalSwapChain needs a windowed DISCARD chain without MSAA.");
    return hr;
  }
  ComPtr<MTLD3D9SwapChain> result;
  result.Attach(new MTLD3D9SwapChain(
      static_cast<IDirect3DDevice9 *>(this), nullptr, *parameters, std::move(extra)
  ));
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
  // this buffer the next buffer's storage, so this device keeps its view and surface of buffer 0 across a Present
  return S_OK;
}

HRESULT
MTLD3D9Device::back_buffer(UINT index, IDirect3DSurface9 **surface) {
  if (!surface)
    return D3DERR_INVALIDCALL;
  *surface = nullptr;
  if (index != 0)
    return D3DERR_INVALIDCALL;
  HRESULT hr = ensure_back_buffer();
  if (FAILED(hr))
    return hr;
  *surface = back_buffer_surface_.Get();
  (*surface)->AddRef();
  return S_OK;
}

HRESULT
MTLD3D9Device::display_mode(D3DDISPLAYMODE *mode) {
  if (!mode)
    return D3DERR_INVALIDCALL;
  return direct3d_->GetAdapterDisplayMode(adapter_ordinal_, mode);
}

// the working set the Metal device recommends less what its textures have already taken (IDXGIAdapter3::QueryVideoMemoryInfo,
// which is the same answer D3D12's drivers give): D3D9's own drivers answer this question with the memory their
// allocations may still take, which is what a title reads to choose its texture sizes
UINT
MTLD3D9Device::GetAvailableTextureMem() {
  ComPtr<IDXGIAdapter3> adapter3;
  DXGI_QUERY_VIDEO_MEMORY_INFO info{};
  if (FAILED(adapter_.As(&adapter3)) ||
      FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
    return 0;
  const uint64_t free = info.Budget > info.CurrentUsage ? info.Budget - info.CurrentUsage : 0;
  return (UINT)std::min<uint64_t>(free, UINT_MAX);
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
  if (!surface || shared_handle)
    return D3DERR_INVALIDCALL;
  *surface = nullptr;
  // a system-memory surface is the CPU's, and a default one is the device's: a render target ColorFill fills, or the
  // destination StretchRect and UpdateSurface write. A default surface takes a render target view when its format can,
  // which is what ColorFill needs (an offscreen surface of a compressed format has none and is only a copy's source).
  if (pool != D3DPOOL_SYSTEMMEM && pool != D3DPOOL_DEFAULT)
    return D3DERR_INVALIDCALL;
  // a default-pool surface is the device's, and the device the frames failed on is replaced by a Reset
  if (pool == D3DPOOL_DEFAULT) {
    if (HRESULT lost = lost_reason(); FAILED(lost))
      return lost;
  }
  const DXGI_FORMAT dxgi_format = to_dxgi_format(format);
  if (dxgi_format == DXGI_FORMAT_UNKNOWN || !width || !height)
    return D3DERR_INVALIDCALL;

  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = dxgi_format;
  desc.SampleDesc.Count = 1;
  if (pool == D3DPOOL_SYSTEMMEM) {
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

  ComPtr<MTLD3D9Surface> result;
  result.Attach(new MTLD3D9Surface(static_cast<IDirect3DDevice9 *>(this), texture, format, pool,
                                   pool == D3DPOOL_SYSTEMMEM, context_, 0));
  *surface = result.Detach();
  return S_OK;
}


HRESULT
create_device(IDirect3D9 *direct3d, IDXGIFactory2 *factory, IDXGIAdapter1 *adapter, UINT adapter_ordinal,
              const D3DCAPS9 &caps, HWND window, DWORD behavior_flags, D3DPRESENT_PARAMETERS present,
              IDirect3DDevice9 **device) {
  if (!device || !factory || !adapter)
    return D3DERR_INVALIDCALL;
  *device = nullptr;

  ComPtr<MTLD3D9Device> result;
  result.Attach(new MTLD3D9Device(direct3d, ComPtr<IDXGIAdapter1>(adapter), adapter_ordinal, caps, window,
                                  behavior_flags, present));
  HRESULT hr = result->init(factory);
  if (FAILED(hr))
    return hr;

  *device = result.Detach();
  return S_OK;
}

} // namespace mullion::d3d9
