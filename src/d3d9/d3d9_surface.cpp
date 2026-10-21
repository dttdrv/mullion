#include "d3d9_surface.hpp"

namespace mullion::d3d9 {

const IID IID_IMTLD3D9Surface = {0x4c7b4a3e, 0x1e0f, 0x4a15, {0x9c, 0x2b, 0x77, 0xbb, 0x5f, 0x2e, 0x71, 0x31}};

MTLD3D9Surface::MTLD3D9Surface(IDirect3DDevice9 *device, const ComPtr<ID3D11Texture2D> &texture, D3DFORMAT format,
                               bool lockable, const ComPtr<ID3D11DeviceContext> &context, bool depth_stencil)
    : device_(device), texture_(texture), context_(context), lockable_(lockable), depth_stencil_(depth_stencil) {
  D3D11_TEXTURE2D_DESC texture_desc;
  texture_->GetDesc(&texture_desc);
  desc_ = to_surface_desc(texture_desc, format, lockable);
  if (depth_stencil_) {
    // a depth-stencil surface is a device's, never the CPU's
    desc_.Usage = D3DUSAGE_DEPTHSTENCIL;
    desc_.Pool = D3DPOOL_DEFAULT;
  }
}

HRESULT STDMETHODCALLTYPE MTLD3D9Surface::QueryInterface(REFIID riid, void **object) {
  if (!object)
    return E_POINTER;
  *object = nullptr;
  if (riid == IID_IUnknown || riid == __uuidof(IDirect3DResource9) || riid == __uuidof(IDirect3DSurface9)) {
    *object = static_cast<IDirect3DSurface9 *>(this);
  } else if (riid == IID_IMTLD3D9Surface) {
    *object = static_cast<IMTLD3D9Surface *>(this);
  } else {
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE MTLD3D9Surface::GetDevice(IDirect3DDevice9 **device) {
  if (!device)
    return D3DERR_INVALIDCALL;
  *device = device_;
  if (device_)
    device_->AddRef();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE MTLD3D9Surface::SetPrivateData(REFGUID, const void *, DWORD, DWORD) {
  return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE MTLD3D9Surface::GetPrivateData(REFGUID, void *, DWORD *) { return D3DERR_INVALIDCALL; }

HRESULT STDMETHODCALLTYPE MTLD3D9Surface::FreePrivateData(REFGUID) { return D3DERR_INVALIDCALL; }

DWORD STDMETHODCALLTYPE MTLD3D9Surface::SetPriority(DWORD) { return 0; }

DWORD STDMETHODCALLTYPE MTLD3D9Surface::GetPriority() { return 0; }

void STDMETHODCALLTYPE MTLD3D9Surface::PreLoad() {}

D3DRESOURCETYPE STDMETHODCALLTYPE MTLD3D9Surface::GetType() { return D3DRTYPE_SURFACE; }

HRESULT STDMETHODCALLTYPE MTLD3D9Surface::GetContainer(REFIID, void **container) {
  if (!container)
    return D3DERR_INVALIDCALL;
  *container = nullptr;
  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE MTLD3D9Surface::GetDesc(D3DSURFACE_DESC *desc) {
  if (!desc)
    return D3DERR_INVALIDCALL;
  *desc = desc_;
  return S_OK;
}

// a surface's bytes: the bytes of a staging texture are the CPU's, so locking
// it maps the texture. a rect locks the part of it the rect names, as D3D9's
// does.
HRESULT STDMETHODCALLTYPE MTLD3D9Surface::LockRect(D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags) {
  if (!locked)
    return D3DERR_INVALIDCALL;
  locked->Pitch = 0;
  locked->pBits = nullptr;
  if (!lockable_ || mapped_valid_)
    return D3DERR_INVALIDCALL;

  const D3D11_MAP map = (flags & D3DLOCK_READONLY) ? D3D11_MAP_READ : D3D11_MAP_READ_WRITE;
  if (FAILED(context_->Map(texture_.Get(), 0, map, 0, &mapped_)))
    return D3DERR_INVALIDCALL;
  mapped_valid_ = true;

  locked->Pitch = (INT)mapped_.RowPitch;
  const LONG left = rect ? rect->left : 0;
  const LONG top = rect ? rect->top : 0;
  locked->pBits =
      static_cast<char *>(mapped_.pData) + (size_t)top * mapped_.RowPitch + (size_t)left * pixel_size(desc_.Format);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE MTLD3D9Surface::UnlockRect() {
  if (!mapped_valid_)
    return D3DERR_INVALIDCALL;
  context_->Unmap(texture_.Get(), 0);
  mapped_ = {};
  mapped_valid_ = false;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE MTLD3D9Surface::GetDC(HDC *) { return D3DERR_INVALIDCALL; }

HRESULT STDMETHODCALLTYPE MTLD3D9Surface::ReleaseDC(HDC) { return D3DERR_INVALIDCALL; }

} // namespace mullion::d3d9
