#include "d3d9_swapchain.hpp"

namespace mullion::d3d9 {

const IID IID_IMTLD3D9PresentSource = {0x9d0f1c62, 0x3b57, 0x4c8e, {0x8a, 0x41, 0x2c, 0xd5, 0x6e, 0x90, 0x14, 0x7b}};

MTLD3D9SwapChain::MTLD3D9SwapChain(IDirect3DDevice9 *device, IMTLD3D9PresentSource *source,
                                   const D3DPRESENT_PARAMETERS &parameters)
    : device_(device), device_ref_(device), source_(source), parameters_(parameters) {}

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
