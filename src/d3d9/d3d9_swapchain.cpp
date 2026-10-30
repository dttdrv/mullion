#include "d3d9_swapchain.hpp"
#include "d3d9_surface.hpp"

namespace mullion::d3d9 {

const IID IID_IMTLD3D9PresentSource = {0x9d0f1c62, 0x3b57, 0x4c8e, {0x8a, 0x41, 0x2c, 0xd5, 0x6e, 0x90, 0x14, 0x7b}};

HRESULT
MTLD3D9AdditionalPresent::init(
    ID3D11Device *device, ID3D11DeviceContext *context, IDXGIFactory2 *factory, HWND window,
    D3DPRESENT_PARAMETERS &parameters, IDirect3DDevice9 *owner
) {
  if (!device || !context || !factory || !window || !parameters.Windowed || !IsWindow(window))
    return D3DERR_INVALIDCALL;
  if (parameters.MultiSampleType != D3DMULTISAMPLE_NONE || parameters.SwapEffect != D3DSWAPEFFECT_DISCARD)
    return D3DERR_INVALIDCALL;
  const DXGI_FORMAT format = to_dxgi_format(
      parameters.BackBufferFormat == D3DFMT_UNKNOWN ? D3DFMT_X8R8G8B8 : parameters.BackBufferFormat
  );
  if (format == DXGI_FORMAT_UNKNOWN)
    return D3DERR_INVALIDCALL;
  UINT width = parameters.BackBufferWidth, height = parameters.BackBufferHeight;
  if (!width || !height) {
    RECT client{};
    if (!GetClientRect(window, &client))
      return D3DERR_INVALIDCALL;
    if (!width)
      width = (UINT)(client.right - client.left);
    if (!height)
      height = (UINT)(client.bottom - client.top);
  }
  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = width;
  desc.Height = height;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = std::max<UINT>(parameters.BackBufferCount, 2);
  desc.Scaling = DXGI_SCALING_STRETCH;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  HRESULT hr = factory->CreateSwapChainForHwnd(device, window, &desc, nullptr, nullptr, &chain_);
  if (FAILED(hr))
    return D3DERR_NOTAVAILABLE;
  DXGI_SWAP_CHAIN_DESC1 created{};
  chain_->GetDesc1(&created);
  parameters.BackBufferWidth = created.Width;
  parameters.BackBufferHeight = created.Height;
  parameters.BackBufferCount = created.BufferCount;
  parameters.BackBufferFormat =
      parameters.BackBufferFormat == D3DFMT_UNKNOWN ? D3DFMT_X8R8G8B8 : parameters.BackBufferFormat;
  parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
  ComPtr<ID3D11Texture2D> texture;
  if (FAILED(chain_->GetBuffer(0, IID_PPV_ARGS(&texture))))
    return D3DERR_INVALIDCALL;
  buffer_.Attach(new MTLD3D9Surface(
      owner, texture, parameters.BackBufferFormat, D3DPOOL_DEFAULT, false, context, D3DUSAGE_RENDERTARGET
  ));
  context_ = context;
  window_ = window;
  owner_ = owner;
  return S_OK;
}

HRESULT
MTLD3D9AdditionalPresent::present(const RECT *source, const RECT *, HWND window, const RGNDATA *, DWORD) {
  if (!chain_)
    return D3DERR_INVALIDCALL;
  if (window && window != window_ && !IsWindow(window))
    return D3DERR_INVALIDCALL;
  context_->Flush();
  DXGI_PRESENT_PARAMETERS dirty{};
  RECT rect{};
  if (source) {
    rect = *source;
    dirty.DirtyRectsCount = 1;
    dirty.pDirtyRects = &rect;
  }
  HRESULT hr = chain_->Present1(0, 0, source ? &dirty : nullptr);
  if (FAILED(hr))
    return (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) ? D3DERR_DEVICELOST : D3DERR_INVALIDCALL;
  return S_OK;
}

HRESULT
MTLD3D9AdditionalPresent::back_buffer(UINT index, IDirect3DSurface9 **surface) {
  if (!surface || index != 0)
    return D3DERR_INVALIDCALL;
  *surface = buffer_.Get();
  if (*surface)
    (*surface)->AddRef();
  return S_OK;
}

HRESULT
MTLD3D9AdditionalPresent::display_mode(D3DDISPLAYMODE *mode) {
  if (!mode || !owner_)
    return D3DERR_INVALIDCALL;
  ComPtr<IDirect3D9> d3d;
  if (FAILED(owner_->GetDirect3D(&d3d)))
    return D3DERR_INVALIDCALL;
  return d3d->GetAdapterDisplayMode(0, mode);
}

MTLD3D9SwapChain::MTLD3D9SwapChain(IDirect3DDevice9 *device, IMTLD3D9PresentSource *source,
                                   const D3DPRESENT_PARAMETERS &parameters,
                                   std::unique_ptr<MTLD3D9AdditionalPresent> additional)
    : device_(device), device_ref_(device), source_(source), additional_(std::move(additional)),
      parameters_(parameters) {
  if (additional_)
    source_ = additional_.get();
}

HRESULT STDMETHODCALLTYPE MTLD3D9SwapChain::QueryInterface(REFIID riid, void **object) {
  if (!object)
    return E_POINTER;
  *object = nullptr;
  if (riid == IID_IUnknown || riid == __uuidof(IDirect3DSwapChain9)) {
    *object = static_cast<IDirect3DSwapChain9 *>(this);
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE MTLD3D9SwapChain::Present(const RECT *source, const RECT *destination, HWND window,
                                                    const RGNDATA *dirty, DWORD flags) {
  return source_->present(source, destination, window, dirty, flags);
}

// what the display shows: a D3D9 chain's front buffer is the desktop's, and
// reading it back is not part of this milestone (src/d3d9/README says what it
// would take)
HRESULT STDMETHODCALLTYPE MTLD3D9SwapChain::GetFrontBufferData(IDirect3DSurface9 *) {
  ERR_ONCE("D3D9: GetFrontBufferData is not implemented yet.");
  return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE MTLD3D9SwapChain::GetBackBuffer(UINT index, D3DBACKBUFFER_TYPE type,
                                                          IDirect3DSurface9 **back_buffer) {
  if (type != D3DBACKBUFFER_TYPE_MONO)
    return D3DERR_INVALIDCALL;
  return source_->back_buffer(index, back_buffer);
}

HRESULT STDMETHODCALLTYPE MTLD3D9SwapChain::GetRasterStatus(D3DRASTER_STATUS *status) {
  if (!status)
    return D3DERR_INVALIDCALL;
  status->InVBlank = FALSE;
  status->ScanLine = 0;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE MTLD3D9SwapChain::GetDisplayMode(D3DDISPLAYMODE *mode) { return source_->display_mode(mode); }

HRESULT STDMETHODCALLTYPE MTLD3D9SwapChain::GetDevice(IDirect3DDevice9 **device) {
  if (!device)
    return D3DERR_INVALIDCALL;
  *device = device_;
  device_->AddRef();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE MTLD3D9SwapChain::GetPresentParameters(D3DPRESENT_PARAMETERS *parameters) {
  if (!parameters)
    return D3DERR_INVALIDCALL;
  *parameters = parameters_;
  return S_OK;
}

} // namespace mullion::d3d9
