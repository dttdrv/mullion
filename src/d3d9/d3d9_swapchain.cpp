#include "d3d9_swapchain.hpp"
#include "d3d9_surface.hpp"

namespace mullion::d3d9 {

const IID IID_IMTLD3D9PresentSource = {0x9d0f1c62, 0x3b57, 0x4c8e, {0x8a, 0x41, 0x2c, 0xd5, 0x6e, 0x90, 0x14, 0x7b}};

void
FrontBuffer9::capture(ID3D11Texture2D *shown) {
  if (!shown)
    return;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  shown->GetDevice(&device);
  if (!device)
    return;
  device->GetImmediateContext(&context);
  D3D11_TEXTURE2D_DESC desc{};
  shown->GetDesc(&desc);
  if (copy_) {
    D3D11_TEXTURE2D_DESC have{};
    copy_->GetDesc(&have);
    if (have.Width != desc.Width || have.Height != desc.Height || have.Format != desc.Format)
      copy_.Reset();
  }
  if (!copy_) {
    D3D11_TEXTURE2D_DESC copy_desc{};
    copy_desc.Width = desc.Width;
    copy_desc.Height = desc.Height;
    copy_desc.MipLevels = 1;
    copy_desc.ArraySize = 1;
    copy_desc.Format = desc.Format;
    copy_desc.SampleDesc.Count = 1;
    copy_desc.Usage = D3D11_USAGE_DEFAULT;
    if (FAILED(device->CreateTexture2D(&copy_desc, nullptr, &copy_)))
      return;
  }
  context->CopyResource(copy_.Get(), shown);
}

HRESULT
FrontBuffer9::read(ID3D11DeviceContext *context, D3DFORMAT format, IDirect3DSurface9 *destination) {
  if (!copy_ || !destination || !context)
    return D3DERR_INVALIDCALL;
  ComPtr<IMTLD3D9Surface> target;
  if (FAILED(destination->QueryInterface(IID_IMTLD3D9Surface, reinterpret_cast<void **>(target.GetAddressOf()))) ||
      !target->lockable() || target->depth_stencil())
    return D3DERR_INVALIDCALL;
  D3D11_TEXTURE2D_DESC source_desc{}, target_desc{};
  copy_->GetDesc(&source_desc);
  target->texture()->GetDesc(&target_desc);
  if (source_desc.Format != target_desc.Format)
    return D3DERR_INVALIDCALL;
  // the display is larger than a window's chain, or smaller: the rows and columns both have are the ones copied
  const D3D11_BOX box{0, 0, 0, std::min(source_desc.Width, target_desc.Width),
                      std::min(source_desc.Height, target_desc.Height), 1};
  context->CopySubresourceRegion(target->texture(), 0, 0, 0, 0, copy_.Get(), 0, &box);

  // an X8R8G8B8 frame carries no alpha, and the display's front buffer is opaque: the surface asked for is A8R8G8B8
  D3DSURFACE_DESC destination_desc{};
  if (format == D3DFMT_X8R8G8B8 && SUCCEEDED(destination->GetDesc(&destination_desc)) &&
      destination_desc.Format == D3DFMT_A8R8G8B8) {
    D3DLOCKED_RECT locked{};
    if (SUCCEEDED(destination->LockRect(&locked, nullptr, 0))) {
      for (UINT y = 0; y < box.bottom; y++) {
        unsigned char *row = static_cast<unsigned char *>(locked.pBits) + (size_t)y * locked.Pitch;
        for (UINT x = 0; x < box.right; x++)
          row[x * 4 + 3] = 0xFF;
      }
      destination->UnlockRect();
    }
  }
  return S_OK;
}

MTLD3D9AdditionalPresent::~MTLD3D9AdditionalPresent() {
  for (auto &buffer : buffers_) {
    if (buffer) {
      buffer->destroy_implicit();
      buffer.Detach();
    }
  }
}

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
  // D3D9's count is the number of back buffers the application may address; a flip chain needs at least two to turn
  // between, so a count of one is a chain of two whose second buffer is not the application's
  const UINT count = std::max<UINT>(parameters.BackBufferCount, 1);
  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = width;
  desc.Height = height;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  // a DXGI flip chain keeps 2 to 16 buffers; D3D9's own count stays what the application asked for
  desc.BufferCount = std::min<UINT>(std::max<UINT>(count, 2), 16);
  desc.Scaling = DXGI_SCALING_STRETCH;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  HWND present_window = window;
  HRESULT hr = factory->CreateSwapChainForHwnd(device, present_window, &desc, nullptr, nullptr, &chain_);
  if (FAILED(hr)) {
    // A window may already own a flip-model chain (the device's implicit one). D3D9 still allows an additional
    // swapchain on that window; a bitblt chain is the second presentation path DXGI offers there, and it keeps as
    // many buffers as the application asked for.
    ERR_ONCE("D3D9: additional flip swapchain creation failed: 0x", std::hex, hr);
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    desc.BufferCount = std::min<UINT>(count, 16);
    hr = factory->CreateSwapChainForHwnd(device, present_window, &desc, nullptr, nullptr, &chain_);
  }
  if (FAILED(hr)) {
    // DXGI: one swap chain per HWND. D3D9 CreateAdditionalSwapChain still succeeds on the device window: bind a
    // private proxy of the same client size (same approach as the implicit chain when the focus HWND is taken).
    static ATOM proxy_class = 0;
    if (!proxy_class) {
      WNDCLASSEXW wc{};
      wc.cbSize = sizeof(wc);
      wc.lpfnWndProc = DefWindowProcW;
      wc.hInstance = GetModuleHandleW(nullptr);
      wc.lpszClassName = L"MullionD3D9AdditionalSwapProxy";
      proxy_class = RegisterClassExW(&wc);
    }
    RECT client{0, 0, (LONG)desc.Width, (LONG)desc.Height};
    AdjustWindowRectEx(&client, WS_POPUP, FALSE, 0);
    HWND proxy = CreateWindowExW(
        0, L"MullionD3D9AdditionalSwapProxy", L"", WS_POPUP, 0, 0, client.right - client.left,
        client.bottom - client.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr
    );
    if (!proxy) {
      ERR_ONCE("D3D9: additional swapchain creation failed: 0x", std::hex, hr);
      return D3DERR_NOTAVAILABLE;
    }
    present_window = proxy;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.BufferCount = std::min<UINT>(std::max<UINT>(count, 2), 16);
    hr = factory->CreateSwapChainForHwnd(device, present_window, &desc, nullptr, nullptr, &chain_);
    if (FAILED(hr)) {
      desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
      desc.BufferCount = std::min<UINT>(count, 16);
      hr = factory->CreateSwapChainForHwnd(device, present_window, &desc, nullptr, nullptr, &chain_);
    }
    if (FAILED(hr)) {
      DestroyWindow(proxy);
      ERR_ONCE("D3D9: additional swapchain creation failed: 0x", std::hex, hr);
      return D3DERR_NOTAVAILABLE;
    }
  }
  DXGI_SWAP_CHAIN_DESC1 created{};
  chain_->GetDesc1(&created);
  parameters.BackBufferWidth = created.Width;
  parameters.BackBufferHeight = created.Height;
  // the present parameters D3D9 reports are the application's count (at least one), not the two buffers a flip chain
  // keeps (Wine's device test: a chain made with 0 reports 1)
  parameters.BackBufferCount = count;
  parameters.BackBufferFormat =
      parameters.BackBufferFormat == D3DFMT_UNKNOWN ? D3DFMT_X8R8G8B8 : parameters.BackBufferFormat;
  parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
  context_ = context;
  window_ = present_window;
  owner_ = owner;
  format_ = parameters.BackBufferFormat;
  D3D11_TEXTURE2D_DESC buffer_desc{};
  buffer_desc.Width = created.Width;
  buffer_desc.Height = created.Height;
  buffer_desc.MipLevels = 1;
  buffer_desc.ArraySize = 1;
  buffer_desc.Format = created.Format;
  buffer_desc.SampleDesc.Count = 1;
  buffer_desc.Usage = D3D11_USAGE_DEFAULT;
  buffer_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  buffers_.resize(count);
  for (UINT at = 0; at < count; at++) {
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&buffer_desc, nullptr, &texture)))
      return D3DERR_INVALIDCALL;
    buffers_[at].Attach(new MTLD3D9Surface(
        owner, texture, parameters.BackBufferFormat, D3DPOOL_DEFAULT, false, context, D3DUSAGE_RENDERTARGET
    ));
  }
  return S_OK;
}

HRESULT
MTLD3D9AdditionalPresent::front_buffer_data(IDirect3DSurface9 *destination) {
  return front_.read(context_.Get(), format_, destination);
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
  // The frame that is shown: the chain's current back buffer, which is buffer 0, copied into the DXGI chain's buffer 0
  // (wined3d_present presents swapchain->back_buffers[0]). Which buffers the application asked for, and in what order,
  // does not choose it. The copy is made every time rather than only when buffer 0 was named, because a flip chain's
  // DXGI buffer 0 is the next one to present
  if (!buffers_.empty() && buffers_[0]) {
    ComPtr<ID3D11Texture2D> shown;
    if (FAILED(chain_->GetBuffer(0, IID_PPV_ARGS(&shown))))
      return D3DERR_INVALIDCALL;
    context_->CopyResource(shown.Get(), buffers_[0]->texture());
    // kept for GetFrontBufferData
    front_.capture(shown.Get());
  }
  HRESULT hr = chain_->Present1(0, 0, source ? &dirty : nullptr);
  if (FAILED(hr))
    return (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) ? D3DERR_DEVICELOST : D3DERR_INVALIDCALL;
  return S_OK;
}

HRESULT
MTLD3D9AdditionalPresent::back_buffer(UINT index, IDirect3DSurface9 **surface) {
  if (!surface || index >= buffers_.size() || !buffers_[index])
    return D3DERR_INVALIDCALL;
  *surface = buffers_[index].Get();
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
                                   std::unique_ptr<MTLD3D9AdditionalPresent> additional, bool extended)
    : device_(device), device_ref_(device, additional != nullptr), source_(source),
      additional_(std::move(additional)), parameters_(parameters), extended_(extended) {
  if (additional_) {
    source_ = additional_.get();
    // an additional chain's back buffers are implicit children of the chain: they start at reference count 0, name
    // the chain as GetContainer, and take/drop one device reference across 0↔1 (Wine's test_refcount)
    for (UINT at = 0; at < additional_->buffer_count(); at++) {
      if (MTLD3D9Surface *buffer = additional_->buffer_at(at)) {
        buffer->set_container_borrowed(static_cast<IDirect3DSwapChain9 *>(this));
        buffer->set_implicit();
      }
    }
  } else {
    // the device's implicit chain starts at reference count 0, and the device frees it with itself
    set_implicit();
  }
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
  if (riid == __uuidof(IDirect3DSwapChain9Ex) && extended_) {
    *object = static_cast<IDirect3DSwapChain9Ex *>(this);
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE MTLD3D9SwapChain::Present(const RECT *source, const RECT *destination, HWND window,
                                                    const RGNDATA *dirty, DWORD flags) {
  const HRESULT hr = source_->present(source, destination, window, dirty, flags);
  if (SUCCEEDED(hr))
    present_count_++;
  return hr;
}

// what the display shows: the frame the chain presented last, copied into a system-memory surface the way
// GetRenderTargetData copies a render target (Microsoft Learn, IDirect3DSwapChain9::GetFrontBufferData). Before the
// first Present there is no frame of the chain's to show, which is D3DERR_INVALIDCALL
HRESULT STDMETHODCALLTYPE MTLD3D9SwapChain::GetFrontBufferData(IDirect3DSurface9 *destination) {
  if (!destination)
    return D3DERR_INVALIDCALL;
  return source_->front_buffer_data(destination);
}

HRESULT STDMETHODCALLTYPE MTLD3D9SwapChain::GetBackBuffer(UINT index, D3DBACKBUFFER_TYPE, IDirect3DSurface9 **back_buffer) {
  // D3D9 ignores the back buffer type: Wine's device test asks for LEFT, RIGHT and an out-of-range value and takes the
  // same surface each time
  if (!back_buffer)
    return D3DERR_INVALIDCALL;
  // a failed call leaves the caller's pointer as it was (Wine's device test passes 0xdeadbeef and expects it back)
  return source_->back_buffer(index, back_buffer);
}

// the same display measurement the device reports: every chain of a device presents on the same display, so the device's
// output-linked raster status is the chain's too. A chain whose device is not an extended one has no raster query, and
// reports the blank it cannot see as false rather than inventing a position
HRESULT STDMETHODCALLTYPE MTLD3D9SwapChain::GetRasterStatus(D3DRASTER_STATUS *status) {
  if (!status)
    return D3DERR_INVALIDCALL;
  ComPtr<IDirect3DDevice9Ex> extended;
  if (device_ && SUCCEEDED(device_->QueryInterface(__uuidof(IDirect3DDevice9Ex),
                                                   reinterpret_cast<void **>(extended.GetAddressOf()))))
    return extended->GetRasterStatus(0, status);
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

HRESULT STDMETHODCALLTYPE MTLD3D9SwapChain::GetLastPresentCount(UINT *last_present_count) {
  if (!last_present_count)
    return D3DERR_INVALIDCALL;
  *last_present_count = present_count_;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE MTLD3D9SwapChain::GetPresentStats(D3DPRESENTSTATS *stats) {
  if (!stats)
    return D3DERR_INVALIDCALL;
  *stats = {};
  stats->PresentCount = present_count_;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE MTLD3D9SwapChain::GetDisplayModeEx(D3DDISPLAYMODEEX *mode,
                                                              D3DDISPLAYROTATION *rotation) {
  if (!mode || mode->Size != sizeof(D3DDISPLAYMODEEX))
    return D3DERR_INVALIDCALL;
  D3DDISPLAYMODE current{};
  const HRESULT hr = source_->display_mode(&current);
  if (FAILED(hr))
    return hr;
  mode->Size = sizeof(D3DDISPLAYMODEEX);
  mode->Width = current.Width;
  mode->Height = current.Height;
  mode->RefreshRate = current.RefreshRate;
  mode->Format = current.Format;
  mode->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
  if (rotation)
    *rotation = D3DDISPLAYROTATION_IDENTITY;
  return S_OK;
}

} // namespace mullion::d3d9
