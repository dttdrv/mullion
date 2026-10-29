// a Direct3D 9 swap chain: what a device's implicit chain is, and the copy of
// it an additional chain would be. every one of its frames is the device's to
// show, so a swap chain asks its device for the buffer it is presenting and for
// the frame it is showing.
#pragma once
#include "d3d9_common.hpp"

namespace mullion::d3d9 {

// the interface id of IMTLD3D9PresentSource, compared in QueryInterface
extern const IID IID_IMTLD3D9PresentSource;

// what a swap chain asks its device for (the device is one of these)
struct IMTLD3D9PresentSource {
  // the frame a device presents: the D3D9 Present's own arguments
  virtual HRESULT present(const RECT *source, const RECT *destination, HWND window, const RGNDATA *dirty,
                          DWORD flags) = 0;
  // the back buffer a swap chain shows a frame from: a reference of it
  virtual HRESULT back_buffer(UINT index, IDirect3DSurface9 **surface) = 0;
  // the display mode of the output the chain presents to
  virtual HRESULT display_mode(D3DDISPLAYMODE *mode) = 0;
};

class MTLD3D9SwapChain final : private Object9, public IDirect3DSwapChain9 {
public:
  MTLD3D9SwapChain(IDirect3DDevice9 *device, IMTLD3D9PresentSource *source, const D3DPRESENT_PARAMETERS &parameters);

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override;
  ULONG STDMETHODCALLTYPE AddRef() override { return Object9::AddRef(); }
  ULONG STDMETHODCALLTYPE Release() override { return Object9::Release(); }

  HRESULT STDMETHODCALLTYPE Present(const RECT *source, const RECT *destination, HWND window, const RGNDATA *dirty,
                                    DWORD flags) override;
  HRESULT STDMETHODCALLTYPE GetFrontBufferData(IDirect3DSurface9 *destination) override;
  HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT index, D3DBACKBUFFER_TYPE type,
                                          IDirect3DSurface9 **back_buffer) override;
  HRESULT STDMETHODCALLTYPE GetRasterStatus(D3DRASTER_STATUS *status) override;
  HRESULT STDMETHODCALLTYPE GetDisplayMode(D3DDISPLAYMODE *mode) override;
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **device) override;
  HRESULT STDMETHODCALLTYPE GetPresentParameters(D3DPRESENT_PARAMETERS *parameters) override;

  // the parameters a Reset of the device's implicit chain leaves it with: the same object answers them, since an
  // application may keep the chain it asked for
  void set_present_parameters(const D3DPRESENT_PARAMETERS &parameters) {
    parameters_ = parameters;
  }

private:
  // the device owns its chains, and the chain holds a private reference on it (DeviceChildRef), so a chain kept after
  // the application released its device still has one to present and answer GetDevice through
  IDirect3DDevice9 *device_ = nullptr;
  DeviceChildRef device_ref_;
  IMTLD3D9PresentSource *source_ = nullptr;
  D3DPRESENT_PARAMETERS parameters_{};
};

} // namespace mullion::d3d9
