// a Direct3D 9 device's own object: the D3D11 device and the window's implicit swap chain it presents through, the back
// buffer a frame is drawn into, and the CPU surfaces a frame is read back into. the render states, targets and
// depth-stencil surfaces are d3d9_device_targets.cpp's, the geometry a draw reads d3d9_device_draw.cpp's, and the
// class itself d3d9_device.hpp's.
#include "d3d9_device.hpp"
#include <algorithm>

namespace mullion::d3d9 {

HRESULT
MTLD3D9Device::init(IDXGIFactory2 *factory) {
  const DXGI_FORMAT format = to_dxgi_format(present_.BackBufferFormat);
  if (format == DXGI_FORMAT_UNKNOWN || !window_ || !present_.Windowed)
    return D3DERR_INVALIDCALL;
  if (present_.MultiSampleType != D3DMULTISAMPLE_NONE)
    return D3DERR_NOTAVAILABLE;

  HRESULT hr = D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                 nullptr, 0, D3D11_SDK_VERSION, &d3d11_device_, nullptr, &context_);
  if (FAILED(hr)) {
    ERR("D3D9: no D3D11 device over the adapter: 0x", std::hex, hr);
    return D3DERR_NOTAVAILABLE;
  }

  // the fixed-function path a device draws through, made once with the device: a device without it can clear and
  // present, but not draw, and this milestone's device draws
  hr = fixed_function_.initialize(d3d11_device_.Get());
  if (FAILED(hr))
    return D3DERR_NOTAVAILABLE;

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

  hr = factory->CreateSwapChainForHwnd(d3d11_device_.Get(), window_, &desc, nullptr, nullptr, &swapchain_);
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

  swap_chain_.Attach(new MTLD3D9SwapChain(static_cast<IDirect3DDevice9 *>(this),
                                          static_cast<IMTLD3D9PresentSource *>(this), present_));

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
    render_state_.set(D3DRS_ZENABLE, TRUE);
  }
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
  back_buffer_surface_.Attach(new MTLD3D9Surface(static_cast<IDirect3DDevice9 *>(this), back_buffer_texture_,
                                                 present_.BackBufferFormat, D3DPOOL_DEFAULT, false, context_));
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
  return S_OK;
}

HRESULT
MTLD3D9Device::present(const RECT *, const RECT *, HWND, const RGNDATA *, DWORD) {
  if (!swapchain_)
    return D3DERR_INVALIDCALL;

  // what was cleared and drawn into goes to the window before the chain turns
  // over
  context_->Flush();
  HRESULT hr = swapchain_->Present(to_sync_interval(present_.PresentationInterval), 0);
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
  if (source_desc.Width != target_desc.Width || source_desc.Height != target_desc.Height ||
      source_desc.Format != target_desc.Format)
    return D3DERR_INVALIDCALL;

  context_->CopyResource(target->texture(), source->texture());
  return S_OK;
}

HRESULT
MTLD3D9Device::CreateOffscreenPlainSurface(UINT width, UINT height, D3DFORMAT format, D3DPOOL pool,
                                           IDirect3DSurface9 **surface, HANDLE *shared_handle) {
  if (!surface || shared_handle)
    return D3DERR_INVALIDCALL;
  *surface = nullptr;
  // a default pool surface is a texture a device may draw into, which this
  // milestone has none of: the surfaces a frame is read back into are the CPU's
  if (pool != D3DPOOL_SYSTEMMEM)
    return D3DERR_INVALIDCALL;
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
  desc.Usage = D3D11_USAGE_STAGING;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
  ComPtr<ID3D11Texture2D> texture;
  if (FAILED(d3d11_device_->CreateTexture2D(&desc, nullptr, &texture)))
    return D3DERR_INVALIDCALL;

  ComPtr<MTLD3D9Surface> result;
  result.Attach(
      new MTLD3D9Surface(static_cast<IDirect3DDevice9 *>(this), texture, format, D3DPOOL_SYSTEMMEM, true, context_)
  );
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
