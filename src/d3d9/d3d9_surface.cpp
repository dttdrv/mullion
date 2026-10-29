#include "d3d9_surface.hpp"

namespace mullion::d3d9 {

const IID IID_IMTLD3D9Surface = {0x4c7b4a3e, 0x1e0f, 0x4a15, {0x9c, 0x2b, 0x77, 0xbb, 0x5f, 0x2e, 0x71, 0x31}};
const IID IID_IMTLD3D9LockTarget = {0x2d0c6f18, 0x5a73, 0x4b62, {0xa1, 0x38, 0x6e, 0x4c, 0x7b, 0x59, 0x18, 0xd4}};

MTLD3D9Surface::MTLD3D9Surface(IDirect3DDevice9 *device, const ComPtr<ID3D11Texture2D> &texture, D3DFORMAT format,
                               D3DPOOL pool, bool lockable, const ComPtr<ID3D11DeviceContext> &context, DWORD usage,
                               bool depth_stencil, IMTLD3D9LockTarget *lock_target, UINT level, IUnknown *container)
    : device_(device), device_ref_(device), container_(container), texture_(texture), context_(context),
      lockable_(lockable), depth_stencil_(depth_stencil), lock_target_(lock_target), level_(level) {
  D3D11_TEXTURE2D_DESC texture_desc;
  texture_->GetDesc(&texture_desc);
  desc_ = to_surface_desc(texture_desc, format, pool, level);
  desc_.Usage = usage;
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

HRESULT STDMETHODCALLTYPE MTLD3D9Surface::SetPrivateData(REFGUID guid, const void *data, DWORD size, DWORD flags) {
  return private_data_.set(guid, data, size, flags);
}

HRESULT STDMETHODCALLTYPE MTLD3D9Surface::GetPrivateData(REFGUID guid, void *data, DWORD *size) {
  return private_data_.get(guid, data, size);
}

HRESULT STDMETHODCALLTYPE MTLD3D9Surface::FreePrivateData(REFGUID guid) { return private_data_.free(guid); }

DWORD STDMETHODCALLTYPE MTLD3D9Surface::SetPriority(DWORD) { return 0; }

DWORD STDMETHODCALLTYPE MTLD3D9Surface::GetPriority() { return 0; }

void STDMETHODCALLTYPE MTLD3D9Surface::PreLoad() {}

D3DRESOURCETYPE STDMETHODCALLTYPE MTLD3D9Surface::GetType() { return D3DRTYPE_SURFACE; }

// A level of a texture names its texture; a back buffer names its swap chain; a standalone surface (a render target,
// a depth-stencil surface, a readback surface) has no container and says so (Microsoft Learn,
// IDirect3DSurface9::GetContainer).
HRESULT STDMETHODCALLTYPE MTLD3D9Surface::GetContainer(REFIID riid, void **container) {
  if (!container)
    return D3DERR_INVALIDCALL;
  *container = nullptr;
  if (!container_)
    return E_NOINTERFACE;
  return container_->QueryInterface(riid, container);
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
  // a level of a managed texture locks the bytes its texture holds on this side, not a staging texture's
  if (lock_target_)
    return lock_target_->lock_level(level_, rect, locked, flags);
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
  if (lock_target_)
    return lock_target_->unlock_level(level_);
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
