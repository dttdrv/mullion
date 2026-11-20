// a Direct3D 9 surface over a D3D11 texture: the swap chain's buffer as a
// render target, or a staging texture that a frame is read back through. only a
// staging texture's bytes are on the CPU, so only it locks.
#pragma once
#include "d3d9_common.hpp"

namespace mullion::d3d9 {

// what this build's own code asks a surface for: the texture under it, whether
// its bytes are on the CPU, and whether it is a depth-stencil surface (which a
// draw tests against and never draws into). reached through QueryInterface, so a
// surface of someone else's is refused rather than misread.
struct IMTLD3D9Surface {
  virtual ULONG STDMETHODCALLTYPE AddRef() = 0;
  virtual ULONG STDMETHODCALLTYPE Release() = 0;
  // a reference only this build holds, for the device's binding of a target: it does not change the count the
  // application sees, and it keeps an application-released target alive while the device draws into it
  virtual void add_private_ref() = 0;
  virtual void release_private_ref() = 0;
  // make this a D3D9 implicit object (the automatic depth-stencil surface): reference count 0, freed by the device
  virtual void set_implicit_surface() = 0;
  // true for the device/additional-chain back buffers and the automatic depth-stencil (Object9::implicit)
  virtual bool implicit_surface() const = 0;
  virtual ID3D11Texture2D *texture() = 0;
  virtual bool lockable() = 0;
  virtual bool depth_stencil() = 0;
  // true when this surface is a GetSurfaceLevel / cube-face level of a texture (StretchRect refuses these as dest)
  virtual bool texture_surface() = 0;
  // which level of the texture under it this surface is: a read back of it copies that level, not level 0
  virtual UINT level() = 0;
  // which array slice of a cube (or 2D array) this surface is: a 2D texture's only slice is 0
  virtual UINT array_slice() = 0;
};

// the interface id of IMTLD3D9Surface, compared in QueryInterface
extern const IID IID_IMTLD3D9Surface;

// what a surface of a texture whose bytes the CPU writes locks through: its texture's own storage (d3d9_texture.hpp),
// so that a level locked through the surface and through its texture is the same lock of the same bytes
struct IMTLD3D9LockTarget {
  virtual HRESULT lock_level(UINT level, const RECT *rect, D3DLOCKED_RECT *locked, DWORD flags) = 0;
  virtual HRESULT unlock_level(UINT level) = 0;
  // CPU shadow without taking a lock (UpdateTexture while a LockRect is held).
  virtual HRESULT peek_level(UINT level, D3DLOCKED_RECT *locked) { (void)level; (void)locked; return D3DERR_INVALIDCALL; }
  // GetDC exclusion across every level/face of the parent texture (Wine WINED3D_TEXTURE_DC_IN_USE).
  virtual void acquire_dc() {}
  virtual void release_dc() {}
  // true while any level/face is LockRect'd or a GetDC is outstanding — GetDC of another face must fail then
  // (Wine device.c test_getdc: LockRect(surface2) then GetDC(surface) → INVALIDCALL)
  virtual bool texture_busy() const { return false; }
};

// the interface id of IMTLD3D9LockTarget, compared in QueryInterface
extern const IID IID_IMTLD3D9LockTarget;

class MTLD3D9Surface final : private Object9, public IDirect3DSurface9, public IMTLD3D9Surface {
public:
  // `usage` is what the surface's description says it is for (a render target, a depth-stencil surface, or nothing),
  // which is the usage the resource it came from was made with
  MTLD3D9Surface(IDirect3DDevice9 *device, const ComPtr<ID3D11Texture2D> &texture, D3DFORMAT format, D3DPOOL pool,
                 bool lockable, const ComPtr<ID3D11DeviceContext> &context, DWORD usage = 0,
                 bool depth_stencil = false, IMTLD3D9LockTarget *lock_target = nullptr, UINT level = 0,
                 IUnknown *container = nullptr, UINT array_slice = 0, void *user_memory = nullptr,
                 UINT user_pitch = 0, bool container_borrowed = false, bool counted_device = false);

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override;
  // a texture or cube level shares the parent's public reference count (Wine's test_refcount: GetSurfaceLevel
  // AddRefs the texture, and AddRef/Release of the surface move the texture's count)
  ULONG STDMETHODCALLTYPE AddRef() override {
    return share_parent_ ? share_parent_->AddRef() : Object9::AddRef();
  }
  ULONG STDMETHODCALLTYPE Release() override {
    return share_parent_ ? share_parent_->Release() : Object9::Release();
  }

  // the texture or cube whose level this is, when the public counts are shared
  void set_share_parent(IUnknown *parent) { share_parent_ = parent; }

  // IMTLD3D9Surface: binding a level surface must pin the parent texture (Wine/D3D9 shared public count). A private
  // ref on the surface object alone does not keep the texture alive once the application's public count hits 0, so
  // SetRenderTarget/SetDepthStencil of GetSurfaceLevel results forward private refs to the container.
  void add_private_ref() override;
  void release_private_ref() override;
  ID3D11Texture2D *texture() override { return texture_.Get(); }
  bool lockable() override { return lockable_; }
  bool depth_stencil() override { return depth_stencil_; }
  bool texture_surface() override { return lock_target_ != nullptr || share_parent_ != nullptr; }
  UINT level() override { return level_; }
  UINT array_slice() override { return array_slice_; }

  // A lockable render target (CreateRenderTarget with Lockable TRUE): the surface stays a default-pool texture the GPU
  // draws into, and `staging` is the CPU-visible copy its lock goes through. LockRect copies the target into the staging
  // texture and maps that, and UnlockRect of a lock that was not read-only copies the staging texture back, so the
  // bytes an application locks are the pixels drawn and the bytes it writes are the pixels drawn from then on
  // (Microsoft Learn, IDirect3DDevice9::CreateRenderTarget, "Lockable").
  void set_render_target_staging(const ComPtr<ID3D11Texture2D> &staging) { rt_staging_ = staging; }

  // a swap chain's buffer after the chain turned its buffers over: the surface keeps what the application named, and
  // addresses the buffer that is now at the same index
  void set_texture(const ComPtr<ID3D11Texture2D> &texture) {
    if (texture)
      texture_ = texture;
  }

  // the references an application holds on the surface, apart from the device's: Reset needs them released
  ULONG public_references() const {
    if (share_parent_) {
      // the shared parent is a texture that uses Object9 through its IUnknown AddRef path; its public count is what
      // Reset cares about for the level
      return Object9::public_references();
    }
    return Object9::public_references();
  }

  // an additional swap chain's back buffer names the chain as its container without owning it
  void set_container_borrowed(IUnknown *container) { container_borrowed_ = container; }

  // the implicit back buffer and automatic depth-stencil surface are made at reference count 0 and freed by the device
  void set_implicit() { Object9::set_implicit(); }
  void set_implicit_surface() override { Object9::set_implicit(); }
  bool implicit_surface() const override { return Object9::implicit(); }
  void destroy_implicit() { Object9::destroy_implicit(); }
  // D3D9Ex Reset while the application still holds the back buffer: keep the surface, drop the swapchain container,
  // and name the device instead (Wine d3d9ex.c test_backbuffer_resize). `standalone` replaces any DXGI swap-chain
  // buffer so ResizeBuffers can proceed while this surface keeps the pre-Reset size/contents.
  void detach_as_orphan(const ComPtr<ID3D11Texture2D> &standalone = {}) {
    // Implicit surfaces hold one device ref while public count > 0. Leaving implicit without releasing that ref
    // leaks the device after Reset (Wine test_backbuffer_resize / test_desktop_window refcounts).
    if (Object9::implicit() && Object9::public_references() > 0)
      on_implicit_release();
    Object9::clear_implicit();
    container_borrowed_ = nullptr;
    container_ = device_;
    if (standalone)
      texture_ = standalone;
  }

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
  void on_last_reference() override {
    // Texture levels share the parent's public count; the texture unbinds itself.
    if (share_parent_ || !device_)
      return;
    IMTLD3D9DeviceResources *resources = nullptr;
    if (SUCCEEDED(device_->QueryInterface(IID_IMTLD3D9DeviceResources, reinterpret_cast<void **>(&resources)))) {
      resources->unbind_surface(static_cast<IMTLD3D9Surface *>(this));
      resources->release_device_ref();
    }
  }
  // the implicit surface's public count crossed 0->1 or 1->0: its one device reference is taken or given back
  void on_implicit_acquire() override {
    if (device_)
      device_->AddRef();
  }
  void on_implicit_release() override {
    if (device_)
      device_->Release();
  }

  // the device the surface was made by, held through a private reference (DeviceChildRef) so a surface kept after the
  // application released its device still has one to answer GetDevice with
  IDirect3DDevice9 *device_ = nullptr;
  DeviceChildRef device_ref_;
  // the texture whose level this is (or the swap chain whose back buffer it is): what GetContainer names, held so the
  // texture cannot be freed while one of its levels is alive
  ComPtr<IUnknown> container_;
  // a container this surface does not own: the implicit back buffer's swap chain is the device's, and a counted
  // reference to it would be an application reference (and so a device reference) the application never asked for
  IUnknown *container_borrowed_ = nullptr;
  // when set, AddRef/Release of this surface are the parent's (a texture level)
  IUnknown *share_parent_ = nullptr;
  ComPtr<ID3D11Texture2D> texture_;
  ComPtr<ID3D11DeviceContext> context_;
  D3DSURFACE_DESC desc_{};
  bool lockable_ = false;
  bool depth_stencil_ = false;
  PrivateData9 private_data_;
  D3D11_MAPPED_SUBRESOURCE mapped_{};
  bool mapped_valid_ = false;
  // the texture this surface is a level of, when its bytes are that texture's (not a staging texture's)
  IMTLD3D9LockTarget *lock_target_ = nullptr;
  UINT level_ = 0;
  UINT array_slice_ = 0;
  // a lockable render target's CPU-visible copy, and whether the lock held now is one that may write it back
  ComPtr<ID3D11Texture2D> rt_staging_;
  bool rt_lock_writable_ = false;
  // D3D9Ex SYSTEMMEM with a shared handle locks the caller's block directly.
  void *user_memory_ = nullptr;
  UINT user_pitch_ = 0;
  // GetDC: DC wrapping the locked surface bits until ReleaseDC (D3DKMTCreateDCFromMemory or DIB fallback).
  HDC gdi_dc_ = nullptr;
  HBITMAP gdi_bitmap_ = nullptr;
  HGDIOBJ gdi_old_ = nullptr;
  void *gdi_bits_ = nullptr;          // DIB bits when !gdi_from_memory_
  void *gdi_surface_bits_ = nullptr;  // locked surface pointer held for the DC
  UINT gdi_surface_pitch_ = 0;
  bool gdi_from_memory_ = false;
  // SYSTEMMEM/SCRATCH LockRect shadow when D3D9 DWORD pitch differs from staging RowPitch.
  std::vector<char> pitch_shadow_;
  bool pitch_shadow_active_ = false;
};

} // namespace mullion::d3d9
