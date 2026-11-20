// a Direct3D 9 swap chain: what a device's implicit chain is, and the copy of
// it an additional chain would be. every one of its frames is the device's to
// show, so a swap chain asks its device for the buffer it is presenting and for
// the frame it is showing.
#pragma once
#include "d3d9_common.hpp"
#include <memory>

namespace mullion::d3d9 {

class MTLD3D9Surface;

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
  // the last frame the chain presented, copied into a system-memory surface (IDirect3DSwapChain9::GetFrontBufferData)
  virtual HRESULT front_buffer_data(IDirect3DSurface9 *destination) = 0;
};

// The frame a chain last presented. D3D9's front buffer is what the display shows, which a flip model chain does not
// keep (buffer 0 is the next frame's storage once it is shown), so every Present takes a copy of the buffer it shows;
// GetFrontBufferData reads it back the way GetRenderTargetData reads a render target. The same copy is what a
// D3DSWAPEFFECT_COPY chain's back buffer is restored from, since its Present leaves the back buffer as it was.
class FrontBuffer9 {
public:
  // the buffer that is being shown, copied; a copy of another size or format is made again
  void capture(ID3D11Texture2D *shown);
  // the copy, copied into the surface (the first of its rows and columns that both have), or an error without one or
  // when the surface is not a system-memory surface of the buffer's own format
  HRESULT read(ID3D11DeviceContext *context, D3DFORMAT format, IDirect3DSurface9 *destination);
  ID3D11Texture2D *copy() const { return copy_.Get(); }
  void reset() { copy_.Reset(); }

private:
  ComPtr<ID3D11Texture2D> copy_;
};

// a second windowed chain: its own DXGI swap chain and back buffer, presented independently of the device's implicit
// one (Microsoft Learn, IDirect3DDevice9::CreateAdditionalSwapChain)
class MTLD3D9AdditionalPresent final : public IMTLD3D9PresentSource {
public:
  ~MTLD3D9AdditionalPresent();
  HRESULT init(ID3D11Device *device, ID3D11DeviceContext *context, IDXGIFactory2 *factory, HWND window,
               D3DPRESENT_PARAMETERS &parameters, IDirect3DDevice9 *owner);
  HRESULT present(const RECT *source, const RECT *destination, HWND window, const RGNDATA *dirty,
                  DWORD flags) override;
  HRESULT back_buffer(UINT index, IDirect3DSurface9 **surface) override;
  HRESULT display_mode(D3DDISPLAYMODE *mode) override;
  HRESULT front_buffer_data(IDirect3DSurface9 *destination) override;
  UINT buffer_count() const { return (UINT)buffers_.size(); }
  MTLD3D9Surface *buffer_at(UINT index) {
    return index < buffers_.size() ? buffers_[index].Get() : nullptr;
  }

private:
  FrontBuffer9 front_;
  D3DFORMAT format_ = D3DFMT_X8R8G8B8;
  ComPtr<IDXGISwapChain1> chain_;
  ComPtr<ID3D11DeviceContext> context_;
  // the buffers the application asked for (at least one), each a render-target texture of the device's: D3D9 indexes
  // them 0..count-1 and every index is a surface of the chain's (Wine's device test asks a chain made with a count of 2
  // for buffer 1). Buffer 0 is the chain's current back buffer and the one Present shows, as wined3d presents
  // swapchain->back_buffers[0]; asking for another buffer hands out a reference to it and changes nothing about what is
  // presented. The chain's own buffer 0 is where Present copies that back buffer, since a DXGI chain need not expose as
  // many buffers as D3D9's count names
  std::vector<ComPtr<MTLD3D9Surface>> buffers_;
  IDirect3DDevice9 *owner_ = nullptr;
  HWND window_ = nullptr;
};

class MTLD3D9SwapChain final : private Object9, public IDirect3DSwapChain9Ex {
public:
  MTLD3D9SwapChain(IDirect3DDevice9 *device, IMTLD3D9PresentSource *source, const D3DPRESENT_PARAMETERS &parameters,
                   std::unique_ptr<MTLD3D9AdditionalPresent> additional = {}, bool extended = false);

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
  // IDirect3DSwapChain9Ex: only an Ex-created device's chains answer these, through QueryInterface
  HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT *last_present_count) override;
  HRESULT STDMETHODCALLTYPE GetPresentStats(D3DPRESENTSTATS *stats) override;
  HRESULT STDMETHODCALLTYPE GetDisplayModeEx(D3DDISPLAYMODEEX *mode, D3DDISPLAYROTATION *rotation) override;

  // the parameters a Reset of the device's implicit chain leaves it with: the same object answers them, since an
  // application may keep the chain it asked for
  void set_present_parameters(const D3DPRESENT_PARAMETERS &parameters) {
    parameters_ = parameters;
  }

  // the implicit chain is made at reference count 0 and freed with its device (Object9)
  void destroy_implicit() { Object9::destroy_implicit(); }

private:
  // the implicit chain's public count crossed 0->1 or 1->0: its one device reference is taken or given back
  void on_implicit_acquire() override {
    if (device_)
      device_->AddRef();
  }
  void on_implicit_release() override {
    if (device_)
      device_->Release();
  }

  // the device owns its chains, and the chain holds a private reference on it (DeviceChildRef), so a chain kept after
  // the application released its device still has one to present and answer GetDevice through
  IDirect3DDevice9 *device_ = nullptr;
  DeviceChildRef device_ref_;
  IMTLD3D9PresentSource *source_ = nullptr;
  std::unique_ptr<MTLD3D9AdditionalPresent> additional_;
  D3DPRESENT_PARAMETERS parameters_{};
  bool extended_ = false;
  UINT present_count_ = 0;
};

} // namespace mullion::d3d9
