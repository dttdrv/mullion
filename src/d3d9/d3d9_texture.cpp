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

#include "d3d9_texture.hpp"
#include "d3d9_device.hpp"
#include "log/log.hpp"
#include "util_string.hpp"
#include <algorithm>

namespace mullion {

static bool
IsRenderableColor(MTLD3D9Device *pDevice, D3DFORMAT Format) {
  D3D9_FORMAT_DESC desc;
  if (!D3D9QueryFormat(Format, desc) || (desc.Flags & (D3D9_FORMAT_DEPTH | D3D9_FORMAT_BC)))
    return false;
  return int(pDevice->GetAdapter()->GetFormatCapability(Format)) & int(FormatCapability::Color);
}

HRESULT
CreateD3D9TextureStorage(
    MTLD3D9Device *pDevice, const D3D9_TEXTURE_DESC &Desc, UINT StorageLevels, uint32_t SampleCount,
    Rc<Texture> &OutTexture
) {
  WMTPixelFormat pixel_format = D3D9GetPixelFormat(pDevice->GetMTLDevice(), Desc.Format);
  if (pixel_format == WMTPixelFormatInvalid) {
    ERR("D3D9: unsupported texture format ", Desc.Format);
    return D3DERR_INVALIDCALL;
  }
  if (SampleCount > 1 && !pDevice->GetMTLDevice().supportsTextureSampleCount(SampleCount)) {
    ERR("D3D9: unsupported sample count ", SampleCount);
    return D3DERR_INVALIDCALL;
  }

  WMTTextureInfo info = {};
  info.pixel_format = (WMTPixelFormat)ORIGINAL_FORMAT(pixel_format);
  switch (Desc.Type) {
  case D3DRTYPE_CUBETEXTURE:
    info.type = WMTTextureTypeCube;
    break;
  case D3DRTYPE_VOLUMETEXTURE:
    info.type = WMTTextureType3D;
    break;
  default:
    info.type = SampleCount > 1 ? WMTTextureType2DMultisample : WMTTextureType2D;
    break;
  }
  info.width = Desc.Width;
  info.height = Desc.Height;
  info.depth = Desc.Type == D3DRTYPE_VOLUMETEXTURE ? Desc.Depth : 1;
  info.array_length = 1;
  info.mipmap_level_count = StorageLevels;
  info.sample_count = SampleCount;

  bool depth = Desc.Usage & D3DUSAGE_DEPTHSTENCIL;
  info.usage = WMTTextureUsageShaderRead;
  if (!depth)
    info.usage = (WMTTextureUsage)(info.usage | WMTTextureUsagePixelFormatView);
  if (depth || (Desc.Usage & D3DUSAGE_RENDERTARGET) ||
      (Desc.Type != D3DRTYPE_VOLUMETEXTURE && IsRenderableColor(pDevice, Desc.Format)))
    info.usage = (WMTTextureUsage)(info.usage | WMTTextureUsageRenderTarget);

  OutTexture = new Texture(info, pDevice->GetMTLDevice());

  Flags<TextureAllocationFlag> flags;
  flags.set(TextureAllocationFlag::GpuPrivate);
  OutTexture->rename(OutTexture->allocate(flags));
  for (uint32_t slice = 0; slice < OutTexture->arrayLength(); slice++)
    for (uint32_t level = 0; level < StorageLevels; level++)
      pDevice->queue().initializer.initWithZero(OutTexture.ptr(), OutTexture->current(), slice, level);
  return D3D_OK;
}

MTLD3D9Volume::MTLD3D9Volume(
    MTLD3D9Device *pDevice, const D3DVOLUME_DESC &Desc, Rc<Texture> Texture, IUnknown *pContainer, bool Lockable,
    uint32_t Level
) :
    MTLD3D9PrivateDataObject(pDevice),
    parent_(pDevice),
    desc_(Desc),
    texture_(std::move(Texture)),
    container_(pContainer),
    lockable_(Lockable || Desc.Pool != D3DPOOL_DEFAULT),
    level_(Level) {
  D3D9GetFormatLayout(pDevice->GetMTLDevice(), Desc.Format, layout_);
  if (Desc.Pool == D3DPOOL_SYSTEMMEM || Desc.Pool == D3DPOOL_SCRATCH)
    EnsureShadow();
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Volume::QueryInterface(REFIID riid, void **ppvObject) {
  if (!ppvObject)
    return E_POINTER;
  *ppvObject = nullptr;
  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DVolume9)) {
    *ppvObject = ref(this);
    return S_OK;
  }
  WARN("IDirect3DVolume9::QueryInterface: Unknown interface query ", str::format(riid));
  return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE
MTLD3D9Volume::AddRef() {
  return container_->AddRef();
}

ULONG STDMETHODCALLTYPE
MTLD3D9Volume::Release() {
  return container_->Release();
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Volume::GetContainer(REFIID riid, void **ppContainer) {
  if (!ppContainer)
    return D3DERR_INVALIDCALL;
  return container_->QueryInterface(riid, ppContainer);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Volume::GetDesc(D3DVOLUME_DESC *pDesc) {
  if (!pDesc)
    return D3DERR_INVALIDCALL;
  *pDesc = desc_;
  return D3D_OK;
}

Rc<StagingResource>
MTLD3D9Volume::EnsureShadow() {
  if (!shadow_ && layout_.BlockBytes) {
    uint32_t pitch = layout_.Pitch(desc_.Width);
    uint32_t image = pitch * layout_.Rows(desc_.Height);
    shadow_ = new StagingResource(parent_->GetMTLDevice(), (uint64_t)image * desc_.Depth, pitch, image);
  }
  return shadow_;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Volume::LockBox(D3DLOCKED_BOX *pLockedVolume, const D3DBOX *pBox, DWORD Flags) {
  if (!pLockedVolume)
    return D3DERR_INVALIDCALL;
  pLockedVolume->pBits = nullptr;
  pLockedVolume->RowPitch = 0;
  pLockedVolume->SlicePitch = 0;

  if (!lockable_ || locked_ || !layout_.BlockBytes)
    return D3DERR_INVALIDCALL;

  D3DBOX box = {0, 0, desc_.Width, desc_.Height, 0, desc_.Depth};
  if (pBox) {
    if (pBox->Left >= pBox->Right || pBox->Top >= pBox->Bottom || pBox->Front >= pBox->Back ||
        pBox->Right > desc_.Width || pBox->Bottom > desc_.Height || pBox->Back > desc_.Depth)
      return D3DERR_INVALIDCALL;
    // volumes enforce block-aligned locks in every pool, unlike surfaces (Wine's test_volume_blocks)
    UINT bw = 1, bh = 1;
    if (D3D9IsDXTn(desc_.Format) || desc_.Format == (D3DFORMAT)MAKEFOURCC('A', 'T', 'I', '1') ||
        desc_.Format == (D3DFORMAT)MAKEFOURCC('A', 'T', 'I', '2'))
      bw = bh = 4;
    else if (desc_.Format == D3DFMT_YUY2 || desc_.Format == D3DFMT_UYVY)
      bw = 2;
    if ((pBox->Left % bw) || (pBox->Top % bh) || ((pBox->Right % bw) && pBox->Right != desc_.Width) ||
        ((pBox->Bottom % bh) && pBox->Bottom != desc_.Height))
      return D3DERR_INVALIDCALL;
    box = *pBox;
  }

  auto shadow = EnsureShadow();
  if (texture_ && desc_.Pool == D3DPOOL_DEFAULT && !(Flags & D3DLOCK_DISCARD))
    parent_->CopyTextureToBuffer(
        texture_, level_, 0, {0, 0, desc_.Width, desc_.Height, 0, desc_.Depth}, Shadow(), layout_
    );

  void *data = nullptr;
  HRESULT hr = parent_->MapStaging(
      shadow, !(Flags & D3DLOCK_DISCARD), !(Flags & D3DLOCK_READONLY), Flags & D3DLOCK_DONOTWAIT, &data
  );
  if (FAILED(hr))
    return hr;

  pLockedVolume->RowPitch = shadow->bytesPerRow;
  pLockedVolume->SlicePitch = shadow->bytesPerImage;
  pLockedVolume->pBits = (char *)data + (uint64_t)box.Front * shadow->bytesPerImage +
                         (box.Top / layout_.BlockSize) * shadow->bytesPerRow +
                         (box.Left / layout_.BlockSize) * layout_.BlockBytes;
  locked_ = true;
  lock_flags_ = Flags;
  lock_box_ = box;
  if (container_ && !(Flags & (D3DLOCK_READONLY | D3DLOCK_NO_DIRTY_UPDATE)))
    D3D9AddDirtyRegion(static_cast<IDirect3DBaseTexture9 *>(static_cast<IDirect3DVolumeTexture9 *>(container_)), level_, box);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Volume::UnlockBox() {
  if (!locked_)
    return D3DERR_INVALIDCALL;
  locked_ = false;
  shadow_->unmap();
  if (texture_ && desc_.Pool != D3DPOOL_MANAGED && !(lock_flags_ & D3DLOCK_READONLY)) {
    uint32_t mask = layout_.BlockSize - 1;
    D3DBOX box = lock_box_;
    box.Right = std::min((box.Right + mask) & ~mask, desc_.Width);
    box.Bottom = std::min((box.Bottom + mask) & ~mask, desc_.Height);
    parent_->CopyBufferToTexture(Shadow(), layout_, texture_, level_, 0, box);
  }
  return D3D_OK;
}

template <typename Base>
MTLD3D9TextureBase<Base>::MTLD3D9TextureBase(
    MTLD3D9Device *pDevice, const D3D9_TEXTURE_DESC &Desc, Rc<Texture> &&Storage
) :
    MTLD3D9Resource<Base>(pDevice, Desc.Pool),
    parent_(pDevice),
    desc_(Desc),
    texture_(std::move(Storage)) {
  // DEFAULT pool ATI2N textures lock like dynamic ones (Wine's test_resource_access)
  bool lockable = (Desc.Usage & D3DUSAGE_DYNAMIC) || Desc.Format == (D3DFORMAT)MAKEFOURCC('A', 'T', 'I', '2');
  if (Desc.Type == D3DRTYPE_VOLUMETEXTURE) {
    for (uint32_t level = 0; level < Desc.Levels; level++) {
      D3DVOLUME_DESC vd = {
          Desc.Format,
          D3DRTYPE_VOLUME,
          Desc.Usage,
          Desc.Pool,
          std::max(1u, Desc.Width >> level),
          std::max(1u, Desc.Height >> level),
          std::max(1u, Desc.Depth >> level)
      };
      volumes_.emplace_back(new MTLD3D9Volume(pDevice, vd, texture_, this, lockable, level));
    }
    return;
  }

  auto map_state = std::make_shared<MTLD3D9Surface::MapState>();
  for (uint32_t face = 0; face < faces(); face++) {
    for (uint32_t level = 0; level < Desc.Levels; level++) {
      D3DSURFACE_DESC sd = {};
      sd.Format = Desc.Format;
      sd.Type = D3DRTYPE_SURFACE;
      sd.Usage = Desc.Usage;
      sd.Pool = Desc.Pool;
      sd.MultiSampleType = D3DMULTISAMPLE_NONE;
      sd.Width = std::max(1u, Desc.Width >> level);
      sd.Height = std::max(1u, Desc.Height >> level);
      surfaces_.emplace_back(new MTLD3D9Surface(pDevice, sd, Rc(texture_), this, lockable, level, face));
      surfaces_.back()->ShareMapState(map_state);
    }
  }

  if (Desc.Pool == D3DPOOL_DEFAULT)
    return;
  // each level takes height * pitch bytes, which over-counts block rows for compressed formats as Windows does, and
  // each cube face starts 16-byte aligned
  auto face_start = [&](uint64_t offset, MTLD3D9Surface *s) { return s->level() ? offset : (offset + 15) & ~15ull; };
  uint64_t total = 0;
  for (auto &s : surfaces_)
    total = face_start(total, s.ptr()) + (uint64_t)s->Desc().Height * s->layout().Pitch(s->Desc().Width);
  if (!total || !surfaces_[0]->layout().BlockBytes)
    return;
  auto shared = std::make_shared<D3D9SharedShadow>();
  shared->resource = new StagingResource(pDevice->GetMTLDevice(), total, 0, 0);
  uint64_t offset = 0;
  for (auto &s : surfaces_) {
    uint32_t pitch = s->layout().Pitch(s->Desc().Width);
    offset = face_start(offset, s.ptr());
    s->UseSharedShadow(shared, offset, pitch, pitch * s->layout().Rows(s->Desc().Height));
    offset += (uint64_t)s->Desc().Height * pitch;
  }
}

template <typename Base>
HRESULT STDMETHODCALLTYPE
MTLD3D9TextureBase<Base>::QueryInterface(REFIID riid, void **ppvObject) {
  if (!ppvObject)
    return E_POINTER;
  *ppvObject = nullptr;
  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DResource9) ||
      riid == __uuidof(IDirect3DBaseTexture9) || riid == __uuidof(Base)) {
    *ppvObject = ref(this);
    return S_OK;
  }
  WARN("IDirect3DBaseTexture9::QueryInterface: Unknown interface query ", str::format(riid));
  return E_NOINTERFACE;
}

template <typename Base>
D3DRESOURCETYPE STDMETHODCALLTYPE
MTLD3D9TextureBase<Base>::GetType() {
  return desc_.Type;
}

template <typename Base>
DWORD STDMETHODCALLTYPE
MTLD3D9TextureBase<Base>::SetLOD(DWORD LODNew) {
  if (desc_.Pool != D3DPOOL_MANAGED)
    return 0;
  DWORD lod = std::min<DWORD>(LODNew, desc_.Levels - 1);
  if (lod != lod_)
    sample_view_valid_[0] = sample_view_valid_[1] = false;
  return std::exchange(lod_, lod);
}

template <typename Base>
DWORD STDMETHODCALLTYPE
MTLD3D9TextureBase<Base>::GetLOD() {
  return lod_;
}

template <typename Base>
DWORD STDMETHODCALLTYPE
MTLD3D9TextureBase<Base>::GetLevelCount() {
  return desc_.Levels;
}

template <typename Base>
HRESULT STDMETHODCALLTYPE
MTLD3D9TextureBase<Base>::SetAutoGenFilterType(D3DTEXTUREFILTERTYPE FilterType) {
  if (FilterType == D3DTEXF_NONE)
    return D3DERR_INVALIDCALL;
  autogen_filter_ = FilterType;
  return D3D_OK;
}

template <typename Base>
D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE
MTLD3D9TextureBase<Base>::GetAutoGenFilterType() {
  return autogen_filter_;
}

template <typename Base>
void STDMETHODCALLTYPE
MTLD3D9TextureBase<Base>::GenerateMipSubLevels() {
  if (!(desc_.Usage & D3DUSAGE_AUTOGENMIPMAP) || !texture_ || texture_->miplevelCount() < 2)
    return;
  parent_->EmitBlit([texture = texture_](ArgumentEncodingContext &enc) {
    auto &cmd = enc.encodeBlitCommand<wmtcmd_blit_generate_mipmaps>();
    cmd.type = WMTBlitCommandGenerateMipmaps;
    // every level and face is written, and each has its own fence tracker
    cmd.texture = enc.access(texture, texture->fullView, ResourceAccess::ReadWrite).texture;
  });
}

template <typename Base>
void
MTLD3D9TextureBase<Base>::MarkMipsDirty() {
  if (desc_.Usage & D3DUSAGE_AUTOGENMIPMAP)
    mips_dirty_ = true;
}

template <typename Base>
void
MTLD3D9TextureBase<Base>::FlushManaged() {
  // drawing from a SYSTEMMEM texture samples its current memory whatever was marked dirty; it gets GPU storage on
  // first use and uploads whole, and its dirty region stays for UpdateTexture
  bool sysmem = desc_.Pool == D3DPOOL_SYSTEMMEM;
  if (sysmem) {
    if (!texture_ && FAILED(CreateD3D9TextureStorage(parent_, desc_, desc_.Levels, 1, texture_)))
      return;
  } else {
    if (desc_.Pool != D3DPOOL_MANAGED || !texture_)
      return;
    // an eviction drops the GPU copy, so everything uploads again, including NO_DIRTY_UPDATE writes
    if (managed_epoch_ != parent_->ManagedEpoch()) {
      managed_epoch_ = parent_->ManagedEpoch();
      AddDirtyRegion({0, 0, ~0u, ~0u, 0, ~0u});
    }
    if (!dirty_)
      return;
  }
  for (uint32_t level = 0; level < desc_.Levels; level++) {
    D3DBOX box = DirtyRegion(level);
    if (sysmem)
      box = {0, 0, std::max(1u, desc_.Width >> level), std::max(1u, desc_.Height >> level), 0,
             std::max(1u, desc_.Depth >> level)};
    if (auto v = volume(level)) {
      parent_->CopyBufferToTexture(v->Shadow(), v->layout(), texture_, level, 0, box);
      continue;
    }
    for (uint32_t face = 0; face < faces(); face++) {
      auto s = surface(face, level);
      if (!s)
        continue;
      uint32_t mask = s->layout().BlockSize - 1;
      D3DBOX aligned = box;
      aligned.Left &= ~mask;
      aligned.Top &= ~mask;
      aligned.Right = std::min((aligned.Right + mask) & ~mask, s->Desc().Width);
      aligned.Bottom = std::min((aligned.Bottom + mask) & ~mask, s->Desc().Height);
      parent_->CopyBufferToTexture(s->Shadow(), s->layout(), texture_, level, face, aligned);
    }
  }
  if (sysmem)
    return;
  dirty_ = false;
  MarkMipsDirty();
}

template <typename Base>
void
MTLD3D9TextureBase<Base>::FlushMips() {
  if (!mips_dirty_)
    return;
  mips_dirty_ = false;
  GenerateMipSubLevels();
}

template <typename Base>
void
MTLD3D9TextureBase<Base>::AddDirtyRegion(const D3DBOX &Box) {
  if (!dirty_) {
    dirty_box_ = Box;
    dirty_ = true;
    return;
  }
  dirty_box_.Left = std::min(dirty_box_.Left, Box.Left);
  dirty_box_.Top = std::min(dirty_box_.Top, Box.Top);
  dirty_box_.Front = std::min(dirty_box_.Front, Box.Front);
  dirty_box_.Right = std::max(dirty_box_.Right, Box.Right);
  dirty_box_.Bottom = std::max(dirty_box_.Bottom, Box.Bottom);
  dirty_box_.Back = std::max(dirty_box_.Back, Box.Back);
}

template <typename Base>
void
MTLD3D9TextureBase<Base>::AddDirtyRegion(uint32_t Level, const D3DBOX &Box) {
  auto scale = [&](UINT v) { return v == ~0u ? v : v << Level; };
  AddDirtyRegion({scale(Box.Left), scale(Box.Top), scale(Box.Right), scale(Box.Bottom), scale(Box.Front), scale(Box.Back)});
}

template <typename Base>
D3DBOX
MTLD3D9TextureBase<Base>::DirtyRegion(uint32_t Level) const {
  UINT width = std::max(1u, desc_.Width >> Level), height = std::max(1u, desc_.Height >> Level);
  UINT depth = std::max(1u, desc_.Depth >> Level);
  auto down = [&](UINT v) { return v >> Level; };
  auto up = [&](UINT v, UINT size) { return v == ~0u ? size : std::min(size, (v + (1u << Level) - 1) >> Level); };
  D3DBOX box = {
      std::min(down(dirty_box_.Left), width - 1), std::min(down(dirty_box_.Top), height - 1),
      up(dirty_box_.Right, width), up(dirty_box_.Bottom, height),
      std::min(down(dirty_box_.Front), depth - 1), up(dirty_box_.Back, depth),
  };
  box.Right = std::max(box.Right, box.Left + 1);
  box.Bottom = std::max(box.Bottom, box.Top + 1);
  box.Back = std::max(box.Back, box.Front + 1);
  return box;
}

void
D3D9AddDirtyRegion(IDirect3DBaseTexture9 *pTexture, uint32_t Level, const D3DBOX &Box) {
  switch (pTexture->GetType()) {
  case D3DRTYPE_TEXTURE:
    return static_cast<MTLD3D9Texture *>(pTexture)->AddDirtyRegion(Level, Box);
  case D3DRTYPE_CUBETEXTURE:
    return static_cast<MTLD3D9CubeTexture *>(pTexture)->AddDirtyRegion(Level, Box);
  case D3DRTYPE_VOLUMETEXTURE:
    return static_cast<MTLD3D9VolumeTexture *>(pTexture)->AddDirtyRegion(Level, Box);
  default:
    return;
  }
}

void
D3D9MarkMipsDirty(IDirect3DBaseTexture9 *pTexture) {
  switch (pTexture->GetType()) {
  case D3DRTYPE_TEXTURE:
    return static_cast<MTLD3D9Texture *>(pTexture)->MarkMipsDirty();
  case D3DRTYPE_CUBETEXTURE:
    return static_cast<MTLD3D9CubeTexture *>(pTexture)->MarkMipsDirty();
  case D3DRTYPE_VOLUMETEXTURE:
    return static_cast<MTLD3D9VolumeTexture *>(pTexture)->MarkMipsDirty();
  default:
    return;
  }
}

template <typename Base>
TextureViewKey
MTLD3D9TextureBase<Base>::SampleView(bool Srgb) {
  if (sample_view_valid_[Srgb])
    return sample_views_[Srgb];
  WMTPixelFormat format = D3D9GetPixelFormat(parent_->GetMTLDevice(), desc_.Format);
  if (Srgb) {
    auto swizzle = format & WMTPixelFormatCustomSwizzle;
    format = (WMTPixelFormat)(Recall_sRGB((WMTPixelFormat)ORIGINAL_FORMAT(format)) | swizzle);
  }
  sample_views_[Srgb] = texture_->createView({
      .format = format,
      .type = texture_->textureType(),
      // SetLOD makes a managed texture's level LOD its most detailed one
      .firstMiplevel = lod_,
      .miplevelCount = texture_->miplevelCount() - lod_,
      .firstArraySlice = 0,
      .arraySize = texture_->arrayLength(),
  });
  sample_view_valid_[Srgb] = true;
  return sample_views_[Srgb];
}

template class MTLD3D9TextureBase<IDirect3DTexture9>;
template class MTLD3D9TextureBase<IDirect3DCubeTexture9>;
template class MTLD3D9TextureBase<IDirect3DVolumeTexture9>;

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc) {
  auto s = surface(0, Level);
  return s ? s->GetDesc(pDesc) : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::GetSurfaceLevel(UINT Level, IDirect3DSurface9 **ppSurfaceLevel) {
  InitReturnPtr(ppSurfaceLevel);
  auto s = surface(0, Level);
  if (!s || !ppSurfaceLevel)
    return D3DERR_INVALIDCALL;
  *ppSurfaceLevel = ref(s);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::LockRect(UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) {
  auto s = surface(0, Level);
  return s ? s->LockRect(pLockedRect, pRect, Flags) : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::UnlockRect(UINT Level) {
  auto s = surface(0, Level);
  if (!s)
    return D3DERR_INVALIDCALL;
  HRESULT hr = s->UnlockRect();
  if (SUCCEEDED(hr) && Level == 0)
    MarkMipsDirty();
  return hr;
}

static bool
ValidDirtyRect(const RECT &Rect, MTLD3D9Surface *pLevel0) {
  auto &desc = pLevel0->Desc();
  return Rect.left >= 0 && Rect.top >= 0 && Rect.left < Rect.right && Rect.top < Rect.bottom &&
         Rect.right <= (LONG)desc.Width && Rect.bottom <= (LONG)desc.Height;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::AddDirtyRect(const RECT *pDirtyRect) {
  if (pDirtyRect && !ValidDirtyRect(*pDirtyRect, surface(0, 0)))
    return D3DERR_INVALIDCALL;
  if (!pDirtyRect)
    AddDirtyRegion({0, 0, ~0u, ~0u, 0, ~0u});
  else
    AddDirtyRegion({(UINT)pDirtyRect->left, (UINT)pDirtyRect->top, (UINT)pDirtyRect->right, (UINT)pDirtyRect->bottom, 0, 1});
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9CubeTexture::GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc) {
  auto s = surface(0, Level);
  return s ? s->GetDesc(pDesc) : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9CubeTexture::GetCubeMapSurface(D3DCUBEMAP_FACES FaceType, UINT Level, IDirect3DSurface9 **ppCubeMapSurface) {
  InitReturnPtr(ppCubeMapSurface);
  auto s = (uint32_t)FaceType < 6 ? surface(FaceType, Level) : nullptr;
  if (!s || !ppCubeMapSurface)
    return D3DERR_INVALIDCALL;
  *ppCubeMapSurface = ref(s);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9CubeTexture::LockRect(
    D3DCUBEMAP_FACES FaceType, UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags
) {
  auto s = (uint32_t)FaceType < 6 ? surface(FaceType, Level) : nullptr;
  return s ? s->LockRect(pLockedRect, pRect, Flags) : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9CubeTexture::UnlockRect(D3DCUBEMAP_FACES FaceType, UINT Level) {
  auto s = (uint32_t)FaceType < 6 ? surface(FaceType, Level) : nullptr;
  if (!s)
    return D3DERR_INVALIDCALL;
  HRESULT hr = s->UnlockRect();
  if (SUCCEEDED(hr) && Level == 0)
    MarkMipsDirty();
  return hr;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9CubeTexture::AddDirtyRect(D3DCUBEMAP_FACES FaceType, const RECT *pDirtyRect) {
  if ((uint32_t)FaceType >= 6 || (pDirtyRect && !ValidDirtyRect(*pDirtyRect, surface(FaceType, 0))))
    return D3DERR_INVALIDCALL;
  if (!pDirtyRect)
    AddDirtyRegion({0, 0, ~0u, ~0u, 0, ~0u});
  else
    AddDirtyRegion({(UINT)pDirtyRect->left, (UINT)pDirtyRect->top, (UINT)pDirtyRect->right, (UINT)pDirtyRect->bottom, 0, 1});
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::GetLevelDesc(UINT Level, D3DVOLUME_DESC *pDesc) {
  auto v = volume(Level);
  return v ? v->GetDesc(pDesc) : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::GetVolumeLevel(UINT Level, IDirect3DVolume9 **ppVolumeLevel) {
  InitReturnPtr(ppVolumeLevel);
  auto v = volume(Level);
  if (!v || !ppVolumeLevel)
    return D3DERR_INVALIDCALL;
  *ppVolumeLevel = ref(v);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::LockBox(UINT Level, D3DLOCKED_BOX *pLockedVolume, const D3DBOX *pBox, DWORD Flags) {
  auto v = volume(Level);
  return v ? v->LockBox(pLockedVolume, pBox, Flags) : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::UnlockBox(UINT Level) {
  auto v = volume(Level);
  if (!v)
    return D3DERR_INVALIDCALL;
  HRESULT hr = v->UnlockBox();
  if (SUCCEEDED(hr) && Level == 0)
    MarkMipsDirty();
  return hr;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::AddDirtyBox(const D3DBOX *pDirtyBox) {
  AddDirtyRegion(pDirtyBox ? *pDirtyBox : D3DBOX{0, 0, ~0u, ~0u, 0, ~0u});
  return D3D_OK;
}

static UINT
MaxLevels(UINT Width, UINT Height, UINT Depth) {
  UINT size = std::max({Width, Height, Depth});
  UINT levels = 1;
  while (size > 1) {
    size >>= 1;
    levels++;
  }
  return levels;
}

HRESULT
CreateD3D9Texture(MTLD3D9Device *pDevice, const D3D9_TEXTURE_DESC &Desc, IDirect3DBaseTexture9 **ppTexture) {
  InitReturnPtr(ppTexture);
  if (!ppTexture || !Desc.Width || !Desc.Height || !Desc.Depth || !D3D9SizeInWholeBlocks(Desc.Format, Desc.Width, Desc.Height))
    return D3DERR_INVALIDCALL;

  D3D9_FORMAT_DESC format;
  if (!D3D9QueryFormat(Desc.Format, format) || (format.Flags & D3D9_FORMAT_NULL_RT && Desc.Type != D3DRTYPE_TEXTURE))
    return D3DERR_INVALIDCALL;
  if ((format.Flags & D3D9_FORMAT_CPU_ONLY) && Desc.Pool != D3DPOOL_SCRATCH)
    return D3DERR_INVALIDCALL;

  bool gpu_output = Desc.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL);
  if (gpu_output && Desc.Pool != D3DPOOL_DEFAULT)
    return D3DERR_INVALIDCALL;
  if ((Desc.Usage & D3DUSAGE_DYNAMIC) && (Desc.Pool == D3DPOOL_MANAGED || Desc.Pool == D3DPOOL_SCRATCH))
    return D3DERR_INVALIDCALL;
  if ((Desc.Usage & D3DUSAGE_DEPTHSTENCIL) && !(format.Flags & D3D9_FORMAT_DEPTH))
    return D3DERR_INVALIDCALL;
  if (Desc.Usage & D3DUSAGE_WRITEONLY)
    return D3DERR_INVALIDCALL;
  // D3D9Ex has no managed pool
  if (Desc.Pool == D3DPOOL_MANAGED && pDevice->IsExtended())
    return D3DERR_INVALIDCALL;
  // dynamic GPU outputs and depth render targets don't exist (Wine's test_resource_access)
  if ((Desc.Usage & D3DUSAGE_DYNAMIC) && gpu_output)
    return D3DERR_INVALIDCALL;
  if ((Desc.Usage & D3DUSAGE_RENDERTARGET) && (format.Flags & D3D9_FORMAT_DEPTH))
    return D3DERR_INVALIDCALL;
  if (Desc.Type == D3DRTYPE_VOLUMETEXTURE && (gpu_output || (format.Flags & D3D9_FORMAT_DEPTH)))
    return D3DERR_INVALIDCALL;

  D3D9_TEXTURE_DESC desc = Desc;
  UINT max_levels = MaxLevels(desc.Width, desc.Height, desc.Type == D3DRTYPE_VOLUMETEXTURE ? desc.Depth : 1);
  UINT storage_levels;
  if (desc.Usage & D3DUSAGE_AUTOGENMIPMAP) {
    if (desc.Levels > 1 || desc.Pool == D3DPOOL_SYSTEMMEM || desc.Type == D3DRTYPE_VOLUMETEXTURE)
      return D3DERR_INVALIDCALL;
    desc.Levels = 1;
    storage_levels = max_levels;
  } else {
    if (desc.Levels > max_levels)
      return D3DERR_INVALIDCALL;
    if (!desc.Levels)
      desc.Levels = max_levels;
    storage_levels = desc.Levels;
  }

  Rc<Texture> storage;
  if (desc.Pool != D3DPOOL_SYSTEMMEM && desc.Pool != D3DPOOL_SCRATCH && !(format.Flags & D3D9_FORMAT_NULL_RT)) {
    HRESULT hr = CreateD3D9TextureStorage(pDevice, desc, storage_levels, 1, storage);
    if (FAILED(hr))
      return hr;
  }

  switch (desc.Type) {
  case D3DRTYPE_TEXTURE:
    *ppTexture = ref(new MTLD3D9Texture(pDevice, desc, std::move(storage)));
    return D3D_OK;
  case D3DRTYPE_CUBETEXTURE:
    *ppTexture = ref(new MTLD3D9CubeTexture(pDevice, desc, std::move(storage)));
    return D3D_OK;
  case D3DRTYPE_VOLUMETEXTURE:
    *ppTexture = ref(new MTLD3D9VolumeTexture(pDevice, desc, std::move(storage)));
    return D3D_OK;
  default:
    return D3DERR_INVALIDCALL;
  }
}

} // namespace mullion
