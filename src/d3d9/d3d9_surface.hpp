// a Direct3D 9 surface over a D3D11 texture: the swap chain's buffer as a
// render target, or a staging texture that a frame is read back through. only a
// staging texture's bytes are on the CPU, so only it locks.
#pragma once
#include "d3d9_common.hpp"

namespace mullion::d3d9 {

// what this build's own code asks a surface for: the texture under it and
// whether its bytes are on the CPU. reached through QueryInterface, so a
// surface of someone else's is refused rather than misread.
struct IMTLD3D9Surface {
  virtual ULONG AddRef() = 0;
  virtual ULONG Release() = 0;
  virtual ID3D11Texture2D *texture() = 0;
  virtual bool lockable() = 0;
};

// the interface id of IMTLD3D9Surface, compared in QueryInterface
extern const IID IID_IMTLD3D9Surface;

class MTLD3D9Surface final : private Object9, public IDirect3DSurface9, public IMTLD3D9Surface {
public:
  MTLD3D9Surface(IDirect3DDevice9 *device, const ComPtr<ID3D11Texture2D> &texture, D3DFORMAT format, bool lockable,
                 const ComPtr<ID3D11DeviceContext> &context);

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override;
  ULONG STDMETHODCALLTYPE AddRef() override { return Object9::AddRef(); }
  ULONG STDMETHODCALLTYPE Release() override { return Object9::Release(); }

  // IMTLD3D9Surface
  ID3D11Texture2D *texture() override { return texture_.Get(); }
  bool lockable() override { return lockable_; }

  // IDirect3DResource9
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **device) override;
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, const void *data, DWORD size, DWORD flags) override;
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, void *data, DWORD *size) override;
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) override;
  DWORD STDMETHODCALLTYPE SetPriority(DWORD priority) override;
  DWORD STDMETHODCALLTYPE GetPriority() override;
  void STDMETHODCALLTYPE PreLoad() override;
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override;

  // IDirect3DSurface9
  HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void **container) override;
  HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC *desc) override;
  HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags) override;
  HRESULT STDMETHODCALLTYPE UnlockRect() override;
  HRESULT STDMETHODCALLTYPE GetDC(HDC *dc) override;
  HRESULT STDMETHODCALLTYPE ReleaseDC(HDC dc) override;

private:
  // the device the surface was made by: it owns the surface, so this is not a
  // reference (a cycle), and GetDevice hands one out as D3D9's contract has it
  IDirect3DDevice9 *device_ = nullptr;
  ComPtr<ID3D11Texture2D> texture_;
  ComPtr<ID3D11DeviceContext> context_;
  D3DSURFACE_DESC desc_{};
  bool lockable_ = false;
  D3D11_MAPPED_SUBRESOURCE mapped_{};
  bool mapped_valid_ = false;
};

} // namespace mullion::d3d9
