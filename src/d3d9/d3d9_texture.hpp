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

#include "d3d9_surface.hpp"
#include "com/com_pointer.hpp"
#include <vector>

namespace mullion {

class MTLD3D9Device;

struct D3D9_TEXTURE_DESC {
  D3DRESOURCETYPE Type;
  UINT Width;
  UINT Height;
  UINT Depth;
  UINT Levels;
  DWORD Usage;
  D3DFORMAT Format;
  D3DPOOL Pool;
};

class MTLD3D9Volume final : public MTLD3D9PrivateDataObject<IDirect3DVolume9> {
public:
  MTLD3D9Volume(MTLD3D9Device *pDevice, const D3DVOLUME_DESC &Desc, Rc<Texture> Texture, IUnknown *pContainer,
                bool Lockable, uint32_t Level);

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final;
  ULONG STDMETHODCALLTYPE AddRef() final;
  ULONG STDMETHODCALLTYPE Release() final;
  HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void **ppContainer) final;
  HRESULT STDMETHODCALLTYPE GetDesc(D3DVOLUME_DESC *pDesc) final;
  HRESULT STDMETHODCALLTYPE LockBox(D3DLOCKED_BOX *pLockedVolume, const D3DBOX *pBox, DWORD Flags) final;
  HRESULT STDMETHODCALLTYPE UnlockBox() final;

  const D3DVOLUME_DESC &
  Desc() const {
    return desc_;
  }

  uint32_t
  level() const {
    return level_;
  }

  const D3D9_FORMAT_LAYOUT &
  layout() const {
    return layout_;
  }

  bool
  IsLocked() const {
    return locked_;
  }

  Rc<StagingResource> EnsureShadow();
  D3D9Shadow
  Shadow() {
    auto shadow = EnsureShadow();
    return {shadow, 0, shadow->bytesPerRow, shadow->bytesPerImage};
  }

private:
  MTLD3D9Device *parent_;
  D3DVOLUME_DESC desc_;
  Rc<Texture> texture_;
  IUnknown *container_;
  bool lockable_;
  uint32_t level_;
  D3D9_FORMAT_LAYOUT layout_ = {0, 1};
  Rc<StagingResource> shadow_;
  bool locked_ = false;
  DWORD lock_flags_ = 0;
  D3DBOX lock_box_ = {};
};

// shared state of 2D, cube and volume textures; Base is the D3D9 interface.
template <typename Base> class MTLD3D9TextureBase : public MTLD3D9Resource<Base> {
public:
  MTLD3D9TextureBase(MTLD3D9Device *pDevice, const D3D9_TEXTURE_DESC &Desc, Rc<Texture> &&Storage);

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final;
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() final;
  DWORD STDMETHODCALLTYPE SetLOD(DWORD LODNew) final;
  DWORD STDMETHODCALLTYPE GetLOD() final;
  DWORD STDMETHODCALLTYPE GetLevelCount() final;
  HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE FilterType) final;
  D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() final;
  void STDMETHODCALLTYPE GenerateMipSubLevels() final;

  const D3D9_TEXTURE_DESC &
  Desc() const {
    return desc_;
  }

  Rc<Texture>
  texture() const {
    return texture_;
  }

  uint32_t
  faces() const {
    return desc_.Type == D3DRTYPE_CUBETEXTURE ? 6 : 1;
  }

  MTLD3D9Surface *
  surface(uint32_t Face, uint32_t Level) {
    uint32_t index = Face * desc_.Levels + Level;
    return Level < desc_.Levels && index < surfaces_.size() ? surfaces_[index].ptr() : nullptr;
  }

  MTLD3D9Volume *
  volume(uint32_t Level) {
    return Level < volumes_.size() ? volumes_[Level].ptr() : nullptr;
  }

  // D3DUSAGE_AUTOGENMIPMAP: writes to level 0 mark the chain dirty, and it's regenerated before the next draw
  void MarkMipsDirty();
  void FlushMips();
  // D3DPOOL_MANAGED: the system memory copy is authoritative and its dirty region uploads before a draw
  void FlushManaged();

  // UpdateTexture copies only the dirty region (level 0 coordinates) of a source texture, then cleans it.
  void AddDirtyRegion(const D3DBOX &Box);
  void AddDirtyRegion(uint32_t Level, const D3DBOX &Box);
  bool
  Dirty() const {
    return dirty_;
  }
  D3DBOX DirtyRegion(uint32_t Level) const;
  void
  CleanDirtyRegion() {
    dirty_ = false;
  }

  // view used for sampling: the whole mip chain with the D3D9 format's swizzle, optionally as sRGB.
  TextureViewKey SampleView(bool Srgb);

protected:
  MTLD3D9Device *parent_;
  D3D9_TEXTURE_DESC desc_;
  Rc<Texture> texture_;
  std::vector<Com<MTLD3D9Surface, false>> surfaces_;
  std::vector<Com<MTLD3D9Volume, false>> volumes_;
  DWORD lod_ = 0;
  bool mips_dirty_ = false;
  bool dirty_ = true;
  uint64_t managed_epoch_ = 0;
  D3DBOX dirty_box_ = {0, 0, ~0u, ~0u, 0, ~0u};
  D3DTEXTUREFILTERTYPE autogen_filter_ = D3DTEXF_LINEAR;
  TextureViewKey sample_views_[2] = {};
  bool sample_view_valid_[2] = {};
};

class MTLD3D9Texture final : public MTLD3D9TextureBase<IDirect3DTexture9> {
public:
  using MTLD3D9TextureBase::MTLD3D9TextureBase;

  HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc) final;
  HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT Level, IDirect3DSurface9 **ppSurfaceLevel) final;
  HRESULT STDMETHODCALLTYPE LockRect(UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) final;
  HRESULT STDMETHODCALLTYPE UnlockRect(UINT Level) final;
  HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT *pDirtyRect) final;
};

class MTLD3D9CubeTexture final : public MTLD3D9TextureBase<IDirect3DCubeTexture9> {
public:
  using MTLD3D9TextureBase::MTLD3D9TextureBase;

  HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc) final;
  HRESULT STDMETHODCALLTYPE
  GetCubeMapSurface(D3DCUBEMAP_FACES FaceType, UINT Level, IDirect3DSurface9 **ppCubeMapSurface) final;
  HRESULT STDMETHODCALLTYPE
  LockRect(D3DCUBEMAP_FACES FaceType, UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) final;
  HRESULT STDMETHODCALLTYPE UnlockRect(D3DCUBEMAP_FACES FaceType, UINT Level) final;
  HRESULT STDMETHODCALLTYPE AddDirtyRect(D3DCUBEMAP_FACES FaceType, const RECT *pDirtyRect) final;
};

class MTLD3D9VolumeTexture final : public MTLD3D9TextureBase<IDirect3DVolumeTexture9> {
public:
  using MTLD3D9TextureBase::MTLD3D9TextureBase;

  HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT Level, D3DVOLUME_DESC *pDesc) final;
  HRESULT STDMETHODCALLTYPE GetVolumeLevel(UINT Level, IDirect3DVolume9 **ppVolumeLevel) final;
  HRESULT STDMETHODCALLTYPE LockBox(UINT Level, D3DLOCKED_BOX *pLockedVolume, const D3DBOX *pBox, DWORD Flags) final;
  HRESULT STDMETHODCALLTYPE UnlockBox(UINT Level) final;
  HRESULT STDMETHODCALLTYPE AddDirtyBox(const D3DBOX *pDirtyBox) final;
};

HRESULT CreateD3D9TextureStorage(MTLD3D9Device *pDevice, const D3D9_TEXTURE_DESC &Desc, UINT StorageLevels,
                                 uint32_t SampleCount, Rc<Texture> &OutTexture);

HRESULT CreateD3D9Texture(MTLD3D9Device *pDevice, const D3D9_TEXTURE_DESC &Desc, IDirect3DBaseTexture9 **ppTexture);

void D3D9MarkMipsDirty(IDirect3DBaseTexture9 *pTexture);
void D3D9AddDirtyRegion(IDirect3DBaseTexture9 *pTexture, uint32_t Level, const D3DBOX &Box);

} // namespace mullion
