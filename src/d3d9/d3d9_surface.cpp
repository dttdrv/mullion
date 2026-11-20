#include "d3d9_surface.hpp"
#include "d3d9_texture.hpp"
#include "util_d3dkmt.h"
#include <algorithm>

namespace mullion::d3d9 {

const IID IID_IMTLD3D9Surface = {0x4c7b4a3e, 0x1e0f, 0x4a15, {0x9c, 0x2b, 0x77, 0xbb, 0x5f, 0x2e, 0x71, 0x31}};
const IID IID_IMTLD3D9LockTarget = {0x2d0c6f18, 0x5a73, 0x4b62, {0xa1, 0x38, 0x6e, 0x4c, 0x7b, 0x59, 0x18, 0xd4}};

MTLD3D9Surface::MTLD3D9Surface(IDirect3DDevice9 *device, const ComPtr<ID3D11Texture2D> &texture, D3DFORMAT format,
                               D3DPOOL pool, bool lockable, const ComPtr<ID3D11DeviceContext> &context, DWORD usage,
                               bool depth_stencil, IMTLD3D9LockTarget *lock_target, UINT level, IUnknown *container,
                               UINT array_slice, void *user_memory, UINT user_pitch, bool container_borrowed,
                               bool counted_device)
    : device_(device), device_ref_(device, counted_device), container_(container_borrowed ? nullptr : container),
      texture_(texture),
      context_(context), lockable_(lockable), depth_stencil_(depth_stencil), lock_target_(lock_target), level_(level),
      array_slice_(array_slice), user_memory_(user_memory), user_pitch_(user_pitch),
      container_borrowed_(container_borrowed ? container : nullptr) {
  D3D11_TEXTURE2D_DESC texture_desc;
  texture_->GetDesc(&texture_desc);
  desc_ = to_surface_desc(texture_desc, format, pool, level);
  desc_.Usage = usage;
  // Dedicated CreateDepthStencilSurface results (no texture lock target) always report DEPTHSTENCIL on DEFAULT.
  // Texture level surfaces keep CreateTexture's usage verbatim so Wine test_resource_access can distinguish
  // FORMAT_DEPTH + usage 0 from FORMAT_DEPTH + D3DUSAGE_DEPTHSTENCIL.
  if (depth_stencil_ && !lock_target_) {
    desc_.Usage = D3DUSAGE_DEPTHSTENCIL | (usage & ~(DWORD)D3DUSAGE_DEPTHSTENCIL);
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
  if (*device)
    (*device)->AddRef();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE MTLD3D9Surface::SetPrivateData(REFGUID guid, const void *data, DWORD size, DWORD flags) {
  return private_data_.set(guid, data, size, flags);
}

HRESULT STDMETHODCALLTYPE MTLD3D9Surface::GetPrivateData(REFGUID guid, void *data, DWORD *size) {
  return private_data_.get(guid, data, size);
}

HRESULT STDMETHODCALLTYPE MTLD3D9Surface::FreePrivateData(REFGUID guid) {
  return private_data_.free(guid);
}

DWORD STDMETHODCALLTYPE MTLD3D9Surface::SetPriority(DWORD priority) {
  return 0;
}

DWORD STDMETHODCALLTYPE MTLD3D9Surface::GetPriority() {
  return 0;
}

void STDMETHODCALLTYPE MTLD3D9Surface::PreLoad() {}

D3DRESOURCETYPE STDMETHODCALLTYPE MTLD3D9Surface::GetType() {
  return D3DRTYPE_SURFACE;
}

HRESULT STDMETHODCALLTYPE MTLD3D9Surface::GetContainer(REFIID riid, void **container) {
  if (!container)
    return D3DERR_INVALIDCALL;
  *container = nullptr;
  IUnknown *holder = container_borrowed_ ? container_borrowed_ : container_.Get();
  if (!holder)
    return E_NOINTERFACE;
  return holder->QueryInterface(riid, container);
}

HRESULT STDMETHODCALLTYPE MTLD3D9Surface::GetDesc(D3DSURFACE_DESC *desc) {
  if (!desc)
    return D3DERR_INVALIDCALL;
  *desc = desc_;
  return S_OK;
}

void
MTLD3D9Surface::add_private_ref() {
  if (share_parent_) {
    if (IMTLD3D9Texture *parent = lookup_base_texture(static_cast<IDirect3DBaseTexture9 *>(share_parent_)))
      parent->add_private_ref();
    return;
  }
  Object9::AddRefPrivate();
}

void
MTLD3D9Surface::release_private_ref() {
  if (share_parent_) {
    if (IMTLD3D9Texture *parent = lookup_base_texture(static_cast<IDirect3DBaseTexture9 *>(share_parent_)))
      parent->release_private_ref();
    return;
  }
  Object9::ReleasePrivate();
}

// a surface's bytes: the bytes of a staging texture are the CPU's, so locking
// it maps the texture. a rect locks the part of it the rect names, as D3D9's
// does.
HRESULT STDMETHODCALLTYPE MTLD3D9Surface::LockRect(D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags) {
  if (!locked)
    return D3DERR_INVALIDCALL;
  // A second LockRect while locked must leave the caller's pBits/Pitch unchanged (Wine device.c lock tests).
  // GetDC holds the surface mapped until ReleaseDC (same as Windows / wined3d map_count).
  if (mapped_valid_ || gdi_dc_)
    return D3DERR_INVALIDCALL;
  // Texture-level locks go through the parent first so a refused double-lock does not wipe the caller's outputs.
  // SCRATCH/SYSTEMMEM/MANAGED pool lock rules (including "invalid" rects that still succeed) live in the storage.
  if (lock_target_) {
    D3D11_TEXTURE2D_DESC texture_desc{};
    texture_->GetDesc(&texture_desc);
    return lock_target_->lock_level(D3D11CalcSubresource(level_, array_slice_, texture_desc.MipLevels), rect, locked,
                                    flags);
  }
  locked->Pitch = 0;
  locked->pBits = nullptr;
  if (user_memory_) {
    if (level_ != 0 || !user_pitch_)
      return D3DERR_INVALIDCALL;
    const LONG left = rect ? rect->left : 0;
    const LONG top = rect ? rect->top : 0;
    locked->Pitch = (INT)user_pitch_;
    locked->pBits = static_cast<char *>(user_memory_) + (size_t)top * user_pitch_ + (size_t)left * 4;
    return S_OK;
  }
  if (rt_staging_) {
    // a lockable render target: the pixels drawn are copied to the CPU's copy, which is what the lock maps. A lock that
    // discards (D3DLOCK_DISCARD) promises to overwrite what it locks, so the GPU's pixels need not come first
    if (mapped_valid_)
      return D3DERR_INVALIDCALL;
    if (rect && (rect->left < 0 || rect->top < 0 || rect->right > (LONG)desc_.Width || rect->bottom > (LONG)desc_.Height ||
                 rect->right <= rect->left || rect->bottom <= rect->top))
      return D3DERR_INVALIDCALL;
    if (!(flags & D3DLOCK_DISCARD))
      context_->CopyResource(rt_staging_.Get(), texture_.Get());
    const D3D11_MAP map = (flags & D3DLOCK_READONLY) ? D3D11_MAP_READ : D3D11_MAP_READ_WRITE;
    if (FAILED(context_->Map(rt_staging_.Get(), 0, map, 0, &mapped_)))
      return D3DERR_INVALIDCALL;
    mapped_valid_ = true;
    rt_lock_writable_ = !(flags & D3DLOCK_READONLY);
    locked->Pitch = (INT)mapped_.RowPitch;
    const LONG left = rect ? rect->left : 0;
    const LONG top = rect ? rect->top : 0;
    locked->pBits = static_cast<char *>(mapped_.pData) + (size_t)top * mapped_.RowPitch +
                    (size_t)left * pixel_size(desc_.Format);
    return S_OK;
  }
  if (!lockable_ || mapped_valid_)
    return D3DERR_INVALIDCALL;

  // Offscreen SCRATCH/SYSTEMMEM surfaces: do not reject "invalid" rects here — Wine device.c expects many of them
  // to succeed on those pools (test_lockrect_invalid / test_pitch).
  const D3D11_MAP map = (flags & D3DLOCK_READONLY) ? D3D11_MAP_READ : D3D11_MAP_READ_WRITE;
  if (FAILED(context_->Map(texture_.Get(), 0, map, 0, &mapped_)))
    return D3DERR_INVALIDCALL;
  mapped_valid_ = true;

  const UINT blocks = block_size(desc_.Format);
  // Compressed SCRATCH/SYSTEMMEM plains: D3D9 pitches and LockRect offsets are in whole blocks (Wine device.c
  // test_lockrect_offset). Report the D3D9 pitch rather than the staging Map's padded RowPitch when they differ.
  if (blocks) {
    const UINT d3d_pitch = ((desc_.Width + 3) / 4) * blocks;
    locked->Pitch = (INT)d3d_pitch;
    const LONG left = rect ? rect->left : 0;
    const LONG top = rect ? rect->top : 0;
    locked->pBits = static_cast<char *>(mapped_.pData) + (size_t)(top / 4) * d3d_pitch +
                    (size_t)(left / 4) * blocks;
    return S_OK;
  }
  // Uncompressed SYSTEMMEM/SCRATCH plains: D3D9 pitch is DWORD-aligned width*bpp, not the staging RowPitch
  // (Wine device.c test_pitch: 5x5 R5G6B5 → Pitch 12).
  {
    const UINT bpp = pixel_size(desc_.Format);
    const UINT d3d_pitch = bpp ? ((desc_.Width * bpp + 3u) & ~3u) : (UINT)mapped_.RowPitch;
    locked->Pitch = (INT)d3d_pitch;
    const LONG left = rect ? rect->left : 0;
    const LONG top = rect ? rect->top : 0;
    if (d3d_pitch == mapped_.RowPitch) {
      locked->pBits = static_cast<char *>(mapped_.pData) + (size_t)top * mapped_.RowPitch + (size_t)left * bpp;
    } else {
      // Present a tightly packed view: copy mapped rows into a D3D9-pitch shadow for the lock duration.
      const UINT rows = desc_.Height;
      pitch_shadow_.assign((size_t)d3d_pitch * rows, 0);
      for (UINT row = 0; row < rows; row++) {
        const size_t copy = std::min<size_t>(d3d_pitch, mapped_.RowPitch);
        memcpy(pitch_shadow_.data() + (size_t)row * d3d_pitch,
               static_cast<const char *>(mapped_.pData) + (size_t)row * mapped_.RowPitch, copy);
      }
      pitch_shadow_active_ = true;
      locked->pBits = pitch_shadow_.data() + (size_t)top * d3d_pitch + (size_t)left * bpp;
    }
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE MTLD3D9Surface::UnlockRect() {
  if (gdi_dc_)
    return D3DERR_INVALIDCALL;
  if (user_memory_)
    return S_OK;
  if (lock_target_) {
    D3D11_TEXTURE2D_DESC texture_desc{};
    texture_->GetDesc(&texture_desc);
    return lock_target_->unlock_level(D3D11CalcSubresource(level_, array_slice_, texture_desc.MipLevels));
  }
  if (!mapped_valid_)
    return D3DERR_INVALIDCALL;
  if (pitch_shadow_active_) {
    const UINT bpp = pixel_size(desc_.Format);
    const UINT d3d_pitch = bpp ? ((desc_.Width * bpp + 3u) & ~3u) : (UINT)mapped_.RowPitch;
    for (UINT row = 0; row < desc_.Height; row++) {
      const size_t copy = std::min<size_t>(d3d_pitch, mapped_.RowPitch);
      memcpy(static_cast<char *>(mapped_.pData) + (size_t)row * mapped_.RowPitch,
             pitch_shadow_.data() + (size_t)row * d3d_pitch, copy);
    }
    pitch_shadow_active_ = false;
    pitch_shadow_.clear();
  }
  if (rt_staging_) {
    // what the application wrote becomes the render target's pixels
    context_->Unmap(rt_staging_.Get(), 0);
    if (rt_lock_writable_)
      context_->CopyResource(texture_.Get(), rt_staging_.Get());
    rt_lock_writable_ = false;
    mapped_ = {};
    mapped_valid_ = false;
    return S_OK;
  }
  context_->Unmap(texture_.Get(), 0);
  mapped_ = {};
  mapped_valid_ = false;
  return S_OK;
}

namespace {

D3DDDIFORMAT getdc_ddi_format(D3DFORMAT format) {
  switch (format) {
  case D3DFMT_A8R8G8B8:
    return D3DDDIFMT_A8R8G8B8;
  case D3DFMT_X8R8G8B8:
    return D3DDDIFMT_X8R8G8B8;
  case D3DFMT_R8G8B8:
    return D3DDDIFMT_R8G8B8;
  case D3DFMT_R5G6B5:
    return D3DDDIFMT_R5G6B5;
  case D3DFMT_X1R5G5B5:
    return D3DDDIFMT_X1R5G5B5;
  case D3DFMT_A1R5G5B5:
    return D3DDDIFMT_A1R5G5B5;
  default:
    return D3DDDIFMT_UNKNOWN;
  }
}

using CreateDCFromMemoryFn = NTSTATUS(WINAPI *)(D3DKMT_CREATEDCFROMMEMORY *);
using DestroyDCFromMemoryFn = NTSTATUS(WINAPI *)(const D3DKMT_DESTROYDCFROMMEMORY *);

CreateDCFromMemoryFn create_dc_from_memory() {
  static CreateDCFromMemoryFn fn = []() -> CreateDCFromMemoryFn {
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (!gdi)
      gdi = LoadLibraryW(L"gdi32.dll");
    return gdi ? reinterpret_cast<CreateDCFromMemoryFn>(GetProcAddress(gdi, "D3DKMTCreateDCFromMemory")) : nullptr;
  }();
  return fn;
}

DestroyDCFromMemoryFn destroy_dc_from_memory() {
  static DestroyDCFromMemoryFn fn = []() -> DestroyDCFromMemoryFn {
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (!gdi)
      gdi = LoadLibraryW(L"gdi32.dll");
    return gdi ? reinterpret_cast<DestroyDCFromMemoryFn>(GetProcAddress(gdi, "D3DKMTDestroyDCFromMemory")) : nullptr;
  }();
  return fn;
}

} // namespace

HRESULT STDMETHODCALLTYPE MTLD3D9Surface::GetDC(HDC *dc) {
  if (!dc)
    return D3DERR_INVALIDCALL;
  // On failure Windows leaves *dc untouched (Wine device.c test_getdc sentinel 0x1234).
  const D3DDDIFORMAT ddi = getdc_ddi_format(desc_.Format);
  // Texture/cube level surfaces are created with lockable_=false; locking goes through lock_target_ (Wine
  // GetSurfaceLevel / GetCubeMapSurface). GetDC must still succeed on those when the parent storage locks.
  if (ddi == D3DDDIFMT_UNKNOWN || depth_stencil_ || gdi_dc_ || mapped_valid_ || (!lockable_ && !lock_target_))
    return D3DERR_INVALIDCALL;
  // GetDC is exclusive against any LockRect on the parent texture (Wine test_getdc), even of another face/level.
  if (lock_target_ && lock_target_->texture_busy())
    return D3DERR_INVALIDCALL;

  auto create = create_dc_from_memory();
  if (!create)
    return D3DERR_INVALIDCALL;

  // Hold the surface mapped for the DC's lifetime (wined3d increments map_count). Sibling levels/faces then refuse
  // LockRect/GetDC through the shared lock_target one-level lock.
  D3DLOCKED_RECT locked{};
  HRESULT hr = LockRect(&locked, nullptr, 0);
  if (FAILED(hr))
    return hr;

  D3DKMT_CREATEDCFROMMEMORY desc{};
  desc.pMemory = locked.pBits;
  desc.Format = ddi;
  desc.Width = desc_.Width;
  desc.Height = desc_.Height;
  desc.Pitch = (UINT)locked.Pitch;
  desc.hDeviceDc = CreateCompatibleDC(nullptr);
  desc.pColorTable = nullptr;
  const NTSTATUS status = create(&desc);
  if (desc.hDeviceDc)
    DeleteDC(desc.hDeviceDc);
  gdi_surface_bits_ = locked.pBits;
  gdi_surface_pitch_ = (UINT)locked.Pitch;
  if (lock_target_)
    lock_target_->acquire_dc();

  if (!status && desc.hDc) {
    gdi_dc_ = desc.hDc;
    gdi_bitmap_ = (HBITMAP)desc.hBitmap;
    gdi_old_ = nullptr;
    gdi_bits_ = nullptr;
    gdi_from_memory_ = true;
    *dc = desc.hDc;
    return D3D_OK;
  }

  // Fallback when D3DKMTCreateDCFromMemory refuses the locked layout (some cube/level pitches): a bottom-up
  // DIB section that mirrors the locked bits for the DC's lifetime.
  UINT bit_count = 0;
  DWORD mask_r = 0, mask_g = 0, mask_b = 0;
  bool bitfields = false;
  switch (desc_.Format) {
  case D3DFMT_A8R8G8B8:
  case D3DFMT_X8R8G8B8:
    bit_count = 32;
    break;
  case D3DFMT_R8G8B8:
    bit_count = 24;
    break;
  case D3DFMT_R5G6B5:
    bit_count = 16;
    mask_r = 0x0000f800;
    mask_g = 0x000007e0;
    mask_b = 0x0000001f;
    bitfields = true;
    break;
  case D3DFMT_X1R5G5B5:
  case D3DFMT_A1R5G5B5:
    bit_count = 16;
    mask_r = 0x00007c00;
    mask_g = 0x000003e0;
    mask_b = 0x0000001f;
    bitfields = true;
    break;
  default:
    if (lock_target_)
      lock_target_->release_dc();
    gdi_surface_bits_ = nullptr;
    gdi_surface_pitch_ = 0;
    UnlockRect();
    return D3DERR_INVALIDCALL;
  }
  struct {
    BITMAPINFOHEADER bmiHeader;
    DWORD masks[3];
  } info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = (LONG)desc_.Width;
  info.bmiHeader.biHeight = (LONG)desc_.Height;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = (WORD)bit_count;
  info.bmiHeader.biCompression = bitfields ? BI_BITFIELDS : BI_RGB;
  info.masks[0] = mask_r;
  info.masks[1] = mask_g;
  info.masks[2] = mask_b;
  HDC mem = CreateCompatibleDC(nullptr);
  void *bits = nullptr;
  HBITMAP bitmap = mem ? CreateDIBSection(mem, reinterpret_cast<BITMAPINFO *>(&info), DIB_RGB_COLORS, &bits, nullptr, 0)
                       : nullptr;
  if (!bitmap || !bits) {
    if (bitmap)
      DeleteObject(bitmap);
    if (mem)
      DeleteDC(mem);
    if (lock_target_)
      lock_target_->release_dc();
    gdi_surface_bits_ = nullptr;
    gdi_surface_pitch_ = 0;
    UnlockRect();
    return D3DERR_OUTOFVIDEOMEMORY;
  }
  const UINT dib_pitch = ((desc_.Width * bit_count + 31) >> 3) & ~3u;
  const UINT copy = std::min<UINT>(dib_pitch, (UINT)locked.Pitch);
  for (UINT y = 0; y < desc_.Height; y++)
    memcpy(static_cast<char *>(bits) + (size_t)(desc_.Height - 1 - y) * dib_pitch,
           static_cast<char *>(locked.pBits) + (size_t)y * locked.Pitch, copy);
  gdi_old_ = SelectObject(mem, bitmap);
  gdi_dc_ = mem;
  gdi_bitmap_ = bitmap;
  gdi_bits_ = bits;
  gdi_from_memory_ = false;
  *dc = mem;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE MTLD3D9Surface::ReleaseDC(HDC dc) {
  if (!gdi_dc_ || dc != gdi_dc_)
    return D3DERR_INVALIDCALL;

  const bool from_memory = gdi_from_memory_;
  void *surface_bits = gdi_surface_bits_;
  const UINT surface_pitch = gdi_surface_pitch_;
  void *dib_bits = gdi_bits_;

  if (from_memory) {
    if (auto destroy = destroy_dc_from_memory()) {
      D3DKMT_DESTROYDCFROMMEMORY desc{};
      desc.hDc = gdi_dc_;
      desc.hBitmap = gdi_bitmap_;
      destroy(&desc);
    }
  } else {
    GdiFlush();
    if (surface_bits && dib_bits) {
      UINT bit_count = 32;
      if (desc_.Format == D3DFMT_R5G6B5 || desc_.Format == D3DFMT_X1R5G5B5 || desc_.Format == D3DFMT_A1R5G5B5)
        bit_count = 16;
      else if (desc_.Format == D3DFMT_R8G8B8)
        bit_count = 24;
      const UINT dib_pitch = ((desc_.Width * bit_count + 31) >> 3) & ~3u;
      const UINT copy = std::min<UINT>(dib_pitch, surface_pitch);
      for (UINT y = 0; y < desc_.Height; y++)
        memcpy(static_cast<char *>(surface_bits) + (size_t)y * surface_pitch,
               static_cast<char *>(dib_bits) + (size_t)(desc_.Height - 1 - y) * dib_pitch, copy);
    }
    SelectObject(gdi_dc_, gdi_old_);
    DeleteObject(gdi_bitmap_);
    DeleteDC(gdi_dc_);
  }
  gdi_dc_ = nullptr;
  gdi_bitmap_ = nullptr;
  gdi_old_ = nullptr;
  gdi_bits_ = nullptr;
  gdi_surface_bits_ = nullptr;
  gdi_surface_pitch_ = 0;
  gdi_from_memory_ = false;
  if (lock_target_)
    lock_target_->release_dc();
  return UnlockRect();
}

} // namespace mullion::d3d9
