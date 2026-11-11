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

#pragma once

#include "d3d9_device_child.hpp"
#include "d3d9_format.hpp"
#include "mullion_staging.hpp"
#include "mullion_texture.hpp"
#include <memory>

namespace mullion {

class MTLD3D9Device;

// system memory of a subresource: a staging buffer, possibly shared by every level of a texture.
struct D3D9Shadow {
  Rc<StagingResource> resource;
  uint64_t offset;
  uint32_t pitch;
  uint32_t image;
};

// Windows lays out the levels (face by face) of SYSTEMMEM, SCRATCH and MANAGED textures contiguously, and apps write
// all of them through level 0's pointer (Wine's test_miptree_layout, test_mipmap_upload).
struct D3D9SharedShadow {
  Rc<StagingResource> resource;
  uint32_t maps = 0;
  void *data = nullptr;
};

// one mip level of one face. SYSTEMMEM/SCRATCH surfaces have no texture and live in the staging shadow only.
class MTLD3D9Surface final : public MTLD3D9Resource<IDirect3DSurface9> {
public:
  MTLD3D9Surface(
      MTLD3D9Device *pDevice, const D3DSURFACE_DESC &Desc, Rc<Texture> &&pTexture, IUnknown *pContainer = nullptr,
      bool Lockable = false, uint32_t Level = 0, uint32_t Slice = 0
  );

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final;
  ULONG STDMETHODCALLTYPE AddRef() final;
  ULONG STDMETHODCALLTYPE Release() final;

  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() final;
  HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void **ppContainer) final;
  HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC *pDesc) final;
  HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) final;
  HRESULT STDMETHODCALLTYPE UnlockRect() final;
  HRESULT STDMETHODCALLTYPE GetDC(HDC *phdc) final;
  HRESULT STDMETHODCALLTYPE ReleaseDC(HDC hdc) final;

  const D3DSURFACE_DESC &
  Desc() const {
    return desc_;
  }

  Rc<Texture>
  texture() const {
    return texture_;
  }

  TextureViewKey
  viewKey() const {
    return view_;
  }

  uint32_t
  level() const {
    return level_;
  }

  uint32_t
  slice() const {
    return slice_;
  }

  const D3D9_FORMAT_LAYOUT &
  layout() const {
    return layout_;
  }

  // tells an autogen-mipmap container that level 0 changed.
  void MarkWritten();

  bool
  IsDepthStencil() const {
    return desc_.Usage & D3DUSAGE_DEPTHSTENCIL;
  }

  bool
  IsLocked() const {
    return locked_;
  }

  Rc<StagingResource> EnsureShadow();
  D3D9Shadow Shadow();
  void
  UseSharedShadow(std::shared_ptr<D3D9SharedShadow> Shared, uint64_t Offset, uint32_t Pitch, uint32_t Image) {
    shared_ = std::move(Shared);
    shadow_offset_ = Offset;
    shadow_pitch_ = Pitch;
    shadow_image_ = Image;
  }

  // locks and DCs are counted per texture: a DC excludes locks and other DCs on every level and face
  struct MapState {
    uint32_t maps = 0;
    bool dc = false;
  };
  void
  ShareMapState(std::shared_ptr<MapState> State) {
    map_state_ = std::move(State);
  }

  // swap chain back buffers keep their own refcount; texture levels forward theirs to the texture
  void
  KeepOwnRefcount() {
    forward_refs_ = false;
  }

  // flip chains rotate contents under unchanged surface pointers
  void
  SwapStorage(MTLD3D9Surface &Other) {
    std::swap(texture_, Other.texture_);
    std::swap(view_, Other.view_);
  }

  void
  MarkBackBuffer() {
    back_buffer_ = true;
  }
  bool
  IsBackBuffer() const {
    return back_buffer_;
  }

  void
  DetachFromContainer() {
    container_ = nullptr;
  }

  // D3D9Ex system memory resources created on app memory (pSharedHandle): locks return it with a tight pitch,
  // and the staging copy the GPU uses is refreshed from it and written back after readbacks
  void
  UseUserMemory(void *Memory) {
    user_memory_ = Memory;
    shared_ = nullptr;
  }
  bool
  HasUserMemory() const {
    return user_memory_;
  }
  void SyncToUserMemory();

private:
  MTLD3D9Device *parent_;
  D3DSURFACE_DESC desc_;
  Rc<Texture> texture_;
  TextureViewKey view_;
  IUnknown *container_;
  bool forward_refs_ = true;
  bool back_buffer_ = false;
  bool lockable_;
  uint32_t level_;
  uint32_t slice_;
  D3D9_FORMAT_LAYOUT layout_ = {0, 1};
  Rc<StagingResource> shadow_;
  std::shared_ptr<D3D9SharedShadow> shared_;
  void *user_memory_ = nullptr;
  uint32_t
  UserPitch() const {
    return (desc_.Width + layout_.BlockSize - 1) / layout_.BlockSize * layout_.BlockBytes;
  }
  void SyncFromUserMemory();
  uint64_t shadow_offset_ = 0;
  uint32_t shadow_pitch_ = 0;
  uint32_t shadow_image_ = 0;
  bool locked_ = false;
  std::shared_ptr<MapState> map_state_ = std::make_shared<MapState>();
  HDC dc_ = nullptr;
  HANDLE dc_bitmap_ = nullptr;
  D3DLOCKED_RECT dc_lock_ = {};
  DWORD lock_flags_ = 0;
  RECT lock_rect_ = {};
};

HRESULT CreateD3D9SurfaceTexture(MTLD3D9Device *pDevice, const D3DSURFACE_DESC &Desc, Rc<Texture> &OutTexture);

} // namespace mullion
