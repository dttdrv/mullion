/*
 * Copyright 2026 David Skoland
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "d3d9_surface.hpp"
#include <cstring>
#include "d3d9_device.hpp"
#include "d3d9_texture.hpp"
#include "d3d9_format.hpp"
#include "log/log.hpp"
#include "util_string.hpp"
#include <algorithm>

namespace {

struct D3DKMT_CREATEDCFROMMEMORY {
  void *pMemory;
  D3DFORMAT Format;
  UINT Width;
  UINT Height;
  UINT Pitch;
  HDC hDeviceDc;
  PALETTEENTRY *pColorTable;
  HDC hDc;
  HANDLE hBitmap;
};

struct D3DKMT_DESTROYDCFROMMEMORY {
  HDC hDc;
  HANDLE hBitmap;
};

} // namespace

extern "C" {
DECLSPEC_IMPORT LONG WINAPI D3DKMTCreateDCFromMemory(D3DKMT_CREATEDCFROMMEMORY *desc);
DECLSPEC_IMPORT LONG WINAPI D3DKMTDestroyDCFromMemory(const D3DKMT_DESTROYDCFROMMEMORY *desc);
}

namespace mullion {

MTLD3D9Surface::MTLD3D9Surface(
    MTLD3D9Device *pDevice, const D3DSURFACE_DESC &Desc, Rc<Texture> &&pTexture, IUnknown *pContainer,
    bool Lockable, uint32_t Level, uint32_t Slice
) :
    // a texture keeps the priority, not its levels
    MTLD3D9Resource(pDevice, Desc.Pool, false),
    parent_(pDevice),
    desc_(Desc),
    texture_(std::move(pTexture)),
    container_(pContainer),
    lockable_(Lockable || Desc.Pool != D3DPOOL_DEFAULT),
    level_(Level),
    slice_(Slice) {
  D3D9GetFormatLayout(pDevice->GetMTLDevice(), Desc.Format, layout_);

  if (texture_) {
    bool whole = texture_->miplevelCount() == 1 && texture_->arrayLength() == 1 &&
                 (texture_->textureType() == WMTTextureType2D || texture_->textureType() == WMTTextureType2DMultisample);
    if (whole) {
      view_ = texture_->fullView;
    } else {
      view_ = texture_->createView({
          .format = texture_->pixelFormat(),
          .type = WMTTextureType2D,
          .firstMiplevel = Level,
          .miplevelCount = 1,
          .firstArraySlice = Slice,
          .arraySize = 1,
      });
    }
  }

  if ((Desc.Pool == D3DPOOL_SYSTEMMEM || Desc.Pool == D3DPOOL_SCRATCH) && !pContainer)
    EnsureShadow();
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Surface::QueryInterface(REFIID riid, void **ppvObject) {
  if (!ppvObject)
    return E_POINTER;
  *ppvObject = nullptr;

  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DResource9) || riid == __uuidof(IDirect3DSurface9)) {
    *ppvObject = ref(this);
    return S_OK;
  }

  WARN("IDirect3DSurface9::QueryInterface: Unknown interface query ", str::format(riid));
  return E_NOINTERFACE;
}

// surfaces owned by a swapchain or texture share the container's lifetime.
ULONG STDMETHODCALLTYPE
MTLD3D9Surface::AddRef() {
  if (container_ && forward_refs_)
    return container_->AddRef();
  return MTLD3D9Resource::AddRef();
}

ULONG STDMETHODCALLTYPE
MTLD3D9Surface::Release() {
  if (container_ && forward_refs_)
    return container_->Release();
  return MTLD3D9Resource::Release();
}

D3DRESOURCETYPE STDMETHODCALLTYPE
MTLD3D9Surface::GetType() {
  return D3DRTYPE_SURFACE;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Surface::GetContainer(REFIID riid, void **ppContainer) {
  if (!ppContainer)
    return D3DERR_INVALIDCALL;
  if (container_)
    return container_->QueryInterface(riid, ppContainer);
  return device_->QueryInterface(riid, ppContainer);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Surface::GetDesc(D3DSURFACE_DESC *pDesc) {
  if (!pDesc)
    return D3DERR_INVALIDCALL;
  *pDesc = desc_;
  return D3D_OK;
}

Rc<StagingResource>
MTLD3D9Surface::EnsureShadow() {
  if (!shadow_ && layout_.BlockBytes) {
    uint32_t pitch = layout_.Pitch(desc_.Width);
    uint32_t image = pitch * layout_.Rows(desc_.Height);
    shadow_ = new StagingResource(parent_->GetMTLDevice(), image, pitch, image);
  }
  return shadow_;
}

void
MTLD3D9Surface::SyncFromUserMemory() {
  uint32_t pitch = UserPitch(), image = pitch * layout_.Rows(desc_.Height);
  if (!shadow_)
    shadow_ = new StagingResource(parent_->GetMTLDevice(), image, pitch, image);
  void *data = nullptr;
  if (FAILED(parent_->MapStaging(shadow_, false, true, false, &data)))
    return;
  std::memcpy(data, user_memory_, image);
  shadow_->unmap();
}

void
MTLD3D9Surface::SyncToUserMemory() {
  if (!user_memory_ || !shadow_)
    return;
  parent_->WaitUntilGPUIdle();
  void *data = nullptr;
  if (FAILED(parent_->MapStaging(shadow_, true, false, false, &data)))
    return;
  std::memcpy(user_memory_, data, shadow_->length);
  shadow_->unmap();
}

D3D9Shadow
MTLD3D9Surface::Shadow() {
  if (shared_)
    return {shared_->resource, shadow_offset_, shadow_pitch_, shadow_image_};
  if (user_memory_) {
    SyncFromUserMemory();
    return {shadow_, 0, shadow_->bytesPerRow, shadow_->bytesPerImage};
  }
  auto shadow = EnsureShadow();
  return {shadow, 0, shadow->bytesPerRow, shadow->bytesPerImage};
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Surface::LockRect(D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) {
  // a failing call leaves *pLockedRect untouched
  if (!pLockedRect)
    return D3DERR_INVALIDCALL;

  if (!lockable_ || locked_ || map_state_->dc || !layout_.BlockBytes)
    return D3DERR_INVALIDCALL;

  RECT rect = {0, 0, (LONG)desc_.Width, (LONG)desc_.Height};
  // DEFAULT pool surfaces refuse invalid and block-misaligned rects. The others accept them, pointing at the rect's
  // corner as Windows XP did (C&C3 relies on it), and count the whole surface as written for an invalid one.
  RECT origin = rect;
  if (pRect) {
    origin = *pRect;
    bool valid = pRect->left >= 0 && pRect->top >= 0 && pRect->right <= rect.right && pRect->bottom <= rect.bottom &&
                 pRect->left < pRect->right && pRect->top < pRect->bottom;
    uint32_t mask = layout_.BlockSize - 1;
    bool aligned = !(pRect->left & mask) && !(pRect->top & mask) &&
                   ((pRect->right & mask) == 0 || pRect->right == rect.right) &&
                   ((pRect->bottom & mask) == 0 || pRect->bottom == rect.bottom);
    // YUY2/UYVY pixels come in horizontal pairs (Wine's test_surface_blocks)
    if (desc_.Format == D3DFMT_YUY2 || desc_.Format == D3DFMT_UYVY)
      aligned = aligned && !(pRect->left & 1) && (!(pRect->right & 1) || pRect->right == rect.right);
    if (desc_.Pool == D3DPOOL_DEFAULT && (!valid || !aligned))
      return D3DERR_INVALIDCALL;
    if (valid)
      rect = *pRect;
  }

  void *data = nullptr;
  uint32_t pitch;
  if (user_memory_) {
    data = user_memory_;
    pitch = UserPitch();
  } else if (shared_) {
    // levels of one texture map their shared memory together
    if (!shared_->maps) {
      HRESULT hr = parent_->MapStaging(shared_->resource, true, true, Flags & D3DLOCK_DONOTWAIT, &shared_->data);
      if (FAILED(hr))
        return hr;
    }
    shared_->maps++;
    data = (char *)shared_->data + shadow_offset_;
    pitch = shadow_pitch_;
  } else {
    auto shadow = EnsureShadow();
    bool read_back = texture_ && desc_.Pool == D3DPOOL_DEFAULT && !(Flags & D3DLOCK_DISCARD);
    if (read_back)
      parent_->CopyTextureToBuffer(texture_, level_, slice_, {0, 0, desc_.Width, desc_.Height, 0, 1}, Shadow(), layout_);
    HRESULT hr = parent_->MapStaging(
        shadow, !(Flags & D3DLOCK_DISCARD), !(Flags & D3DLOCK_READONLY), Flags & D3DLOCK_DONOTWAIT, &data
    );
    if (FAILED(hr))
      return hr;
    pitch = shadow->bytesPerRow;
  }

  pLockedRect->Pitch = pitch;
  pLockedRect->pBits = (char *)data + (ptrdiff_t)(origin.top / (LONG)layout_.BlockSize) * pitch +
                       (ptrdiff_t)(origin.left / (LONG)layout_.BlockSize) * layout_.BlockBytes;
  locked_ = true;
  map_state_->maps++;
  lock_flags_ = Flags;
  lock_rect_ = rect;
  if (container_ && !(Flags & (D3DLOCK_READONLY | D3DLOCK_NO_DIRTY_UPDATE))) {
    IDirect3DBaseTexture9 *texture = nullptr;
    if (SUCCEEDED(container_->QueryInterface(__uuidof(IDirect3DBaseTexture9), (void **)&texture))) {
      D3D9AddDirtyRegion(texture, level_, {(UINT)rect.left, (UINT)rect.top, (UINT)rect.right, (UINT)rect.bottom, 0, 1});
      texture->Release();
    }
  }
  return D3D_OK;
}

void
MTLD3D9Surface::MarkWritten() {
  if (level_ != 0 || !container_)
    return;
  IDirect3DBaseTexture9 *texture = nullptr;
  if (FAILED(container_->QueryInterface(__uuidof(IDirect3DBaseTexture9), (void **)&texture)))
    return;
  D3D9MarkMipsDirty(texture);
  texture->Release();
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Surface::UnlockRect() {
  if (!locked_) {
    // unlocking an unlocked 2D texture level succeeds, unlike other surfaces, and so does any level while a DC is out
    if (map_state_->dc)
      return D3D_OK;
    IDirect3DTexture9 *texture = nullptr;
    if (container_ && SUCCEEDED(container_->QueryInterface(__uuidof(IDirect3DTexture9), (void **)&texture))) {
      texture->Release();
      return D3D_OK;
    }
    return D3DERR_INVALIDCALL;
  }
  locked_ = false;
  map_state_->maps--;
  if (!(lock_flags_ & D3DLOCK_READONLY))
    MarkWritten();
  if (shared_) {
    if (!--shared_->maps)
      shared_->resource->unmap();
  } else if (!user_memory_) {
    shadow_->unmap();
  }

  // managed texture levels upload their container's dirty region before the next draw instead
  bool deferred = desc_.Pool == D3DPOOL_MANAGED && container_;
  if (texture_ && !deferred && !(lock_flags_ & D3DLOCK_READONLY)) {
    uint32_t mask = layout_.BlockSize - 1;
    D3DBOX box = {
        (UINT)lock_rect_.left, (UINT)lock_rect_.top, std::min((UINT)(lock_rect_.right + mask) & ~mask, desc_.Width),
        std::min((UINT)(lock_rect_.bottom + mask) & ~mask, desc_.Height), 0, 1
    };
    parent_->CopyBufferToTexture(Shadow(), layout_, texture_, level_, slice_, box);
  }
  return D3D_OK;
}

static UINT
DCBitsPerPixel(D3DFORMAT Format) {
  switch (Format) {
  case D3DFMT_R5G6B5:
  case D3DFMT_X1R5G5B5:
  case D3DFMT_A1R5G5B5:
    return 16;
  case D3DFMT_R8G8B8:
    return 24;
  case D3DFMT_X8R8G8B8:
  case D3DFMT_A8R8G8B8:
    return 32;
  default:
    return 0;
  }
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Surface::GetDC(HDC *phdc) {
  // a failing call leaves *phdc untouched
  if (!phdc)
    return D3DERR_INVALIDCALL;
  UINT bpp = DCBitsPerPixel(desc_.Format);
  // only lockable back buffers are GDI compatible (Wine's test_lockable_backbuffer)
  if (!bpp || map_state_->maps || map_state_->dc || (back_buffer_ && !lockable_))
    return D3DERR_INVALIDCALL;

  // a DC is allowed on surfaces the app itself can't lock
  bool lockable = lockable_;
  lockable_ = true;
  HRESULT hr = LockRect(&dc_lock_, nullptr, 0);
  lockable_ = lockable;
  if (FAILED(hr))
    return hr;

  D3DKMT_CREATEDCFROMMEMORY desc = {};
  desc.pMemory = dc_lock_.pBits;
  desc.Format = desc_.Format;
  desc.Width = desc_.Width;
  desc.Height = desc_.Height;
  desc.Pitch = dc_lock_.Pitch;
  desc.hDeviceDc = CreateCompatibleDC(nullptr);
  LONG status = D3DKMTCreateDCFromMemory(&desc);
  DeleteDC(desc.hDeviceDc);
  if (status) {
    UnlockRect();
    return D3DERR_INVALIDCALL;
  }
  dc_ = desc.hDc;
  dc_bitmap_ = desc.hBitmap;
  map_state_->dc = true;
  *phdc = dc_;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Surface::ReleaseDC(HDC hdc) {
  if (!dc_ || hdc != dc_)
    return D3DERR_INVALIDCALL;
  GdiFlush();
  D3DKMT_DESTROYDCFROMMEMORY desc = {dc_, dc_bitmap_};
  D3DKMTDestroyDCFromMemory(&desc);
  dc_ = nullptr;
  dc_bitmap_ = nullptr;
  map_state_->dc = false;
  return UnlockRect();
}

HRESULT
CreateD3D9SurfaceTexture(MTLD3D9Device *pDevice, const D3DSURFACE_DESC &Desc, Rc<Texture> &OutTexture) {
  D3D9_TEXTURE_DESC desc = {D3DRTYPE_SURFACE, Desc.Width, Desc.Height, 1, 1, Desc.Usage, Desc.Format, Desc.Pool};
  uint32_t samples = Desc.MultiSampleType >= D3DMULTISAMPLE_2_SAMPLES ? Desc.MultiSampleType : 1;
  return CreateD3D9TextureStorage(pDevice, desc, 1, samples, OutTexture);
}

} // namespace mullion
