/*
 * Copyright 2026 Feifan He for CodeWeavers
 * Copyright 2026 Marc-Aurel Zent for CodeWeavers
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

#include "com/com_pointer.hpp"
#include "d3d11_device.hpp"
#include "d3d11_enumerable.hpp"
#include "d3d11_view.hpp"
#include "mullion_dynamic.hpp"
#include "mullion_staging.hpp"
#include "mullion_texture.hpp"
#include "d3d11_resource.hpp"
#include "util_win32_compat.h"

#include <limits>
#include <mutex>
#include <random>
#include <unordered_map>

namespace mullion {

#pragma region Shared Texture

// a shared texture's handle is an ordinary file mapping whose bytes say which shared texture it belongs to: the
// carrier pattern of d3d11_fence.cpp with a tag of its own, so "a shared texture still opens as a texture, and
// cannot open as a fence" (tests/d3d11/d3d11_shared_fence.cpp). the texture itself travels as the mach port of
// its Metal allocation under a unique bootstrap name; a planar texture registers its chroma plane under
// "<name>.uv", and a keyed mutex carries its own name (tests/d3d11/d3d11_shared_resource.cpp)
struct SharedTextureData {
  char tag[16];
  uint64_t share_id;
  char mach_port_name[54];
  char mutex_name[54];
  D3D11_RESOURCE_DIMENSION dimension;
  union {
    D3D11_TEXTURE1D_DESC desc1d;
    D3D11_TEXTURE2D_DESC1 desc2d;
    D3D11_TEXTURE3D_DESC1 desc3d;
  } desc;
};

static constexpr char shared_texture_tag[16] = "MULLION_texture";

struct TextureShare;

static std::mutex share_registry_mutex;
static std::unordered_map<uint64_t, TextureShare *> share_registry;

// the share's own mapping is named after its id, so a marshalled legacy handle resolves in any process of the
// session: "You can also marshal this handle to another process to share a resource with a device in another
// process" (Microsoft Learn, IDXGIResource::GetSharedHandle). the name also gives the handle the lifetime that
// page spells out: "If no resource objects exist on any devices that refer to this resource, the handle is no
// longer valid. To extend the lifetime of the handle and video memory, you must open the shared resource on a
// device" - every live resource keeps the share's mapping open, and the name dies with the last of them
static void
ShareMappingName(uint64_t id, char (&out)[64]) {
  snprintf(out, 64, "MULLION_shared_texture_%016llx", (unsigned long long)id);
}

static uint64_t
MakeShareId() {
  std::random_device rd;
  std::mt19937_64 gen(rd());
  std::uniform_int_distribution<uint64_t> dis;
  // the marker bit names a legacy handle and is never part of an id
  return dis(gen) & ~(uint64_t(1) << 62);
}

// the live shared textures of this process. a legacy handle is a plain identity value and not a kernel handle
// (CloseHandle and DuplicateHandle must fail with ERROR_INVALID_HANDLE)
struct TextureShare {
  void incRef() {
    refcount.fetch_add(1u, std::memory_order_acquire);
  }

  void decRef() {
    if (refcount.fetch_sub(1u, std::memory_order_release) == 1u) {
      {
        std::lock_guard<std::mutex> lock(share_registry_mutex);
        share_registry.erase(data.share_id);
      }
      if (named_mapping)
        CloseHandle(named_mapping);
      delete this;
    }
  }

  HANDLE legacyHandle() const {
    return reinterpret_cast<HANDLE>(data.share_id | (uintptr_t(1) << 62));
  }

  SharedTextureData data;
  HANDLE named_mapping = nullptr;
  std::atomic<uint32_t> refcount = {0u};
  // the device whose initializer still owes this share its initial contents, and null once they have landed.
  // its queue is the only one that can order against the upload, so whoever first makes the share visible from
  // this process pays the wait (PublishShare). every resource that keeps a share alive keeps its device alive
  // with it, so the device outlives the debt
  MTLD3D11Device *unpublished_device = nullptr;
};

static bool
IsLegacySharedHandle(HANDLE hResource) {
  return reinterpret_cast<uintptr_t>(hResource) & (uintptr_t(1) << 62);
}

// a share's initial contents are written by its creator's upload queue, which another device or process cannot
// order against (that is what a keyed mutex is for): they must have landed before the share is visible anywhere.
// creation records the debt and pays nothing, so a texture nobody shares never stalls its creator; the first
// handle made from the share, or an import that reaches it, pays it
static void
PublishShare(TextureShare *share) {
  MTLD3D11Device *device;
  {
    std::lock_guard<std::mutex> lock(share_registry_mutex);
    device = share->unpublished_device;
  }
  if (!device)
    return;
  auto &initializer = device->GetMullionDevice().queue().initializer;
  if (auto seq = initializer.flushToWait())
    initializer.wait(seq);
  std::lock_guard<std::mutex> lock(share_registry_mutex);
  if (share->unpublished_device == device)
    share->unpublished_device = nullptr;
}

// a share this process already holds, by the identity its handle carries, without opening its mapping
static Rc<TextureShare>
FindShare(uint64_t id) {
  std::lock_guard<std::mutex> lock(share_registry_mutex);
  auto it = share_registry.find(id);
  if (it == share_registry.end())
    return {};
  return Rc<TextureShare>(it->second);
}

// one share is one entry per process however its handle arrives; the entry keeps the share's mapping open while
// any resource of the share lives here
static Rc<TextureShare>
JoinShare(const SharedTextureData &data) {
  std::lock_guard<std::mutex> lock(share_registry_mutex);
  auto it = share_registry.find(data.share_id);
  if (it != share_registry.end())
    return Rc<TextureShare>(it->second);
  auto share = Rc<TextureShare>(new TextureShare());
  share->data = data;
  char mapping_name[64];
  ShareMappingName(data.share_id, mapping_name);
  share->named_mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, mapping_name);
  share_registry.emplace(data.share_id, share.ptr());
  return share;
}

#pragma endregion

#pragma region DeviceTexture

template <typename tag_texture>
class DeviceTexture : public TResourceBase<tag_texture, IMTLMinLODClampable> {
private:
  Rc<RenamableTexturePool> renamable_;
  float min_lod = 0.0;
  Rc<TextureShare> share_;

  using SRVBase =
      TResourceViewBase<tag_shader_resource_view<DeviceTexture<tag_texture>>>;
  class TextureSRV : public SRVBase {
  public:
    TextureSRV(const TextureViewDescriptor &descriptor,
               const tag_shader_resource_view<>::DESC1 *pDesc,
               DeviceTexture *pResource, MTLD3D11Device *pDevice)
        : SRVBase(pDesc, pResource, pDevice) {
      this->texture_ = pResource->viewTexture(pDesc->Format);
      this->view_id_ = this->texture_->createView(descriptor);
      this->subset_ = ResourceSubsetState(
        &descriptor,
        this->texture_->miplevelCount(),
        this->texture_->arrayLength()
      );
    }

  };

  using UAVBase =
      TResourceViewBase<tag_unordered_access_view<DeviceTexture<tag_texture>>>;
  class TextureUAV : public UAVBase {
  public:
    TextureUAV(const TextureViewDescriptor &descriptor,
               const tag_unordered_access_view<>::DESC1 *pDesc,
               DeviceTexture *pResource, MTLD3D11Device *pDevice)
        : UAVBase(pDesc, pResource, pDevice) {
      this->texture_ = pResource->viewTexture(pDesc->Format);
      this->view_id_ = this->texture_->createView(descriptor);
      if constexpr (std::is_same_v<typename tag_texture::DESC1, D3D11_TEXTURE3D_DESC1>)
        this->w_range_ =
            (uint64_t(pDesc->Texture3D.FirstWSlice) << std::numeric_limits<decltype(pDesc->Texture3D.WSize)>::digits) |
            pDesc->Texture3D.WSize;
      this->subset_ = ResourceSubsetState(
        &descriptor,
        this->texture_->miplevelCount(),
        this->texture_->arrayLength()
      );
    }
  };

  using RTVBase =
      TResourceViewBase<tag_render_target_view<DeviceTexture<tag_texture>>>;
  class TextureRTV : public RTVBase {
  public:
    TextureRTV(
        const TextureViewDescriptor &descriptor, WMTPixelFormat view_format, const tag_render_target_view<>::DESC1 *pDesc,
        DeviceTexture *pResource, MTLD3D11Device *pDevice, const MTL_RENDER_PASS_ATTACHMENT_DESC &mtl_rtv_desc
    ) :
        RTVBase(pDesc, pResource, pDevice) {
      this->texture_ = pResource->viewTexture(pDesc->Format);
      this->view_id_ = this->texture_->createView(descriptor);
      this->format_ = view_format;
      this->pass_desc_ = mtl_rtv_desc;
      this->subset_ = ResourceSubsetState(
        &descriptor,
        this->texture_->miplevelCount(),
        this->texture_->arrayLength()
      );
    }
  };

  using DSVBase =
      TResourceViewBase<tag_depth_stencil_view<DeviceTexture<tag_texture>>>;
  class TextureDSV : public DSVBase {
  public:
    TextureDSV(
        const TextureViewDescriptor &descriptor, WMTPixelFormat view_format, const tag_depth_stencil_view<>::DESC1 *pDesc,
        DeviceTexture *pResource, MTLD3D11Device *pDevice, const MTL_RENDER_PASS_ATTACHMENT_DESC &attachment_desc
    ) :
        DSVBase(pDesc, pResource, pDevice) {
      this->texture_ = pResource->texture_.ptr();
      this->view_id_ = this->texture_->createView(descriptor);
      this->format_ = view_format;
      this->pass_desc_ = attachment_desc;
      this->renamable_ = pResource->renamable_.ptr();
      this->readonly_flags_ = this->desc.Flags & 0b11;
      this->subset_ = ResourceSubsetState(
        &descriptor,
        this->texture_->miplevelCount(),
        this->texture_->arrayLength(),
        this->readonly_flags_
      );
    }
  };

  Texture *
  viewTexture(DXGI_FORMAT format) {
    if (!this->texture_->chroma)
      return this->texture_.ptr();
    switch (format) {
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_UINT:
      return this->texture_.ptr();
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_UINT:
      return this->texture_->chroma.ptr();
    default:
      return nullptr;
    }
  }

public:
  DeviceTexture(const tag_texture::DESC1 *pDesc, Rc<Texture> &&u_texture, MTLD3D11Device *pDevice) :
      TResourceBase<tag_texture, IMTLMinLODClampable>(*pDesc, pDevice) {
        this->texture_ = std::move(u_texture);
      }

  DeviceTexture(
      const tag_texture::DESC1 *pDesc, Rc<Texture> &&u_texture, Rc<RenamableTexturePool> &&renamable,
      MTLD3D11Device *pDevice
  ) :
      TResourceBase<tag_texture, IMTLMinLODClampable>(*pDesc, pDevice),
      renamable_(std::move(renamable)) {
        this->texture_ = std::move(u_texture);
      }

  DeviceTexture(
      const tag_texture::DESC1 *pDesc, Rc<Texture> &&u_texture, Rc<TextureShare> &&share,
      Rc<KeyedMutex> &&keyed_mutex, MTLD3D11Device *pDevice
  ) :
      TResourceBase<tag_texture, IMTLMinLODClampable>(*pDesc, pDevice), share_(std::move(share)) {
        this->texture_ = std::move(u_texture);
        this->keyed_mutex_ = std::move(keyed_mutex);
      }

  Rc<StagingResource> staging(UINT) final { return nullptr; }
  Rc<DynamicBuffer> dynamicBuffer(UINT*, UINT*) final { return {}; }
  Rc<DynamicLinearTexture> dynamicLinearTexture(UINT*, UINT*) final { return {}; };
  Rc<DynamicBuffer> dynamicTexture(UINT , UINT *, UINT *) final { return {}; };

  HRESULT STDMETHODCALLTYPE CreateRenderTargetView(const D3D11_RENDER_TARGET_VIEW_DESC1 *pDesc,
                                 ID3D11RenderTargetView1 **ppView) override {
    D3D11_RENDER_TARGET_VIEW_DESC1 finalDesc;
    if (FAILED(
            ExtractEntireResourceViewDescription(this->m_parent, &this->desc, pDesc, &finalDesc, this->dxgi_usage_))) {
      return E_INVALIDARG;
    }
    auto texture = viewTexture(finalDesc.Format);
    if (!texture)
      return E_INVALIDARG;
    TextureViewDescriptor descriptor;
    uint32_t arraySize;
    if constexpr (std::is_same_v<typename tag_texture::DESC1, D3D11_TEXTURE3D_DESC1>) {
      arraySize = this->desc.Depth;
    } else {
      arraySize = this->desc.ArraySize;
    }
    MTL_RENDER_PASS_ATTACHMENT_DESC attachment_desc;
    auto hr = InitializeAndNormalizeViewDescriptor(
        this->m_parent, this->desc.MipLevels, arraySize, texture, finalDesc, attachment_desc, descriptor
    );
    if (FAILED(hr)) {
      return hr;
    }
    if (!ppView) {
      return S_FALSE;
    }
    *ppView = ref(new TextureRTV(descriptor, descriptor.format, &finalDesc, this, this->m_parent, attachment_desc));
    return S_OK;
  };

  HRESULT STDMETHODCALLTYPE CreateDepthStencilView(const D3D11_DEPTH_STENCIL_VIEW_DESC *pDesc,
                                 ID3D11DepthStencilView **ppView) override {
    if (this->texture_->chroma)
      return E_INVALIDARG;
    D3D11_DEPTH_STENCIL_VIEW_DESC finalDesc;
    if (FAILED(
            ExtractEntireResourceViewDescription(this->m_parent, &this->desc, pDesc, &finalDesc, this->dxgi_usage_))) {
      return E_INVALIDARG;
    }
    TextureViewDescriptor descriptor;
    uint32_t arraySize;
    if constexpr (std::is_same_v<typename tag_texture::DESC1, D3D11_TEXTURE3D_DESC1>) {
      arraySize = this->desc.Depth;
    } else {
      arraySize = this->desc.ArraySize;
    }
    MTL_RENDER_PASS_ATTACHMENT_DESC attachment_desc;
    auto hr = InitializeAndNormalizeViewDescriptor(
        this->m_parent, this->desc.MipLevels, arraySize, this->texture_.ptr(), finalDesc, attachment_desc, descriptor
    );
    if (FAILED(hr)) {
      return hr;
    }
    if (!ppView) {
      return S_FALSE;
    }
    *ppView = ref(new TextureDSV(descriptor, descriptor.format, &finalDesc, this, this->m_parent, attachment_desc));
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  CreateShaderResourceView(const D3D11_SHADER_RESOURCE_VIEW_DESC1 *pDesc,
                           ID3D11ShaderResourceView1 **ppView) override {
    D3D11_SHADER_RESOURCE_VIEW_DESC1 finalDesc;
    if (FAILED(
            ExtractEntireResourceViewDescription(this->m_parent, &this->desc, pDesc, &finalDesc, this->dxgi_usage_))) {
      ERR("DeviceTexture: Failed to create SRV descriptor");
      return E_INVALIDARG;
    }
    auto texture = viewTexture(finalDesc.Format);
    if (!texture)
      return E_INVALIDARG;
    TextureViewDescriptor descriptor;
    uint32_t arraySize;
    if constexpr (std::is_same_v<typename tag_texture::DESC1, D3D11_TEXTURE3D_DESC1>) {
      arraySize = this->desc.Depth;
    } else {
      arraySize = this->desc.ArraySize;
    }
    auto hr = InitializeAndNormalizeViewDescriptor(
        this->m_parent, this->desc.MipLevels, arraySize, texture, finalDesc, descriptor
    );
    if (FAILED(hr)) {
      ERR("DeviceTexture: Failed to create texture SRV");
      return hr;
    }
    if (!ppView) {
      return S_FALSE;
    }
    *ppView = ref(new TextureSRV(descriptor, &finalDesc, this, this->m_parent));
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  CreateUnorderedAccessView(const D3D11_UNORDERED_ACCESS_VIEW_DESC1 *pDesc,
                            ID3D11UnorderedAccessView1 **ppView) override {
    D3D11_UNORDERED_ACCESS_VIEW_DESC1 finalDesc;
    if (FAILED(
            ExtractEntireResourceViewDescription(this->m_parent, &this->desc, pDesc, &finalDesc, this->dxgi_usage_))) {
      return E_INVALIDARG;
    }
    auto texture = viewTexture(finalDesc.Format);
    if (!texture)
      return E_INVALIDARG;
    TextureViewDescriptor descriptor;
    uint32_t arraySize;
    if constexpr (std::is_same_v<typename tag_texture::DESC1, D3D11_TEXTURE3D_DESC1>) {
      arraySize = this->desc.Depth;
    } else {
      arraySize = this->desc.ArraySize;
    }
    auto hr = InitializeAndNormalizeViewDescriptor(
        this->m_parent, this->desc.MipLevels, arraySize, texture, finalDesc, descriptor
    );
    if (FAILED(hr)) {
      ERR("DeviceTexture: Failed to create texture UAV");
      return hr;
    }
    if (!ppView) {
      return S_FALSE;
    }
    *ppView = ref(new TextureUAV(descriptor, &finalDesc, this, this->m_parent));
    return S_OK;
  };

  virtual HRESULT
  GetSharedHandle(HANDLE *pSharedHandle) override {
    if (pSharedHandle == nullptr || (this->desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE)) {
      return E_INVALIDARG;
    }

    if (!(this->desc.MiscFlags & (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX))) {
      *pSharedHandle = NULL;
      return S_OK;
    }

    if (!share_) {
      return E_INVALIDARG;
    }

    // the handle is the first sight another device or process has of this share, so its initial contents land
    // before this returns
    PublishShare(share_.ptr());
    *pSharedHandle = share_->legacyHandle();
    return S_OK;
  }

  virtual HRESULT
  CreateSharedHandle(const SECURITY_ATTRIBUTES *Attributes, DWORD Access, const WCHAR *pName, HANDLE *pNTHandle)
      override {
    InitReturnPtr(pNTHandle);
    if (!pNTHandle || !share_ || !(this->desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE))
      return E_INVALIDARG;
    // "The name is limited to MAX_PATH characters." (Microsoft Learn, ID3D11Fence::CreateSharedHandle)
    if (pName && wcslen(pName) > MAX_PATH)
      return DXGI_ERROR_INVALID_CALL;

    // the handle is the first sight another device or process has of this share, so its initial contents land
    // before it exists
    PublishShare(share_.ptr());

    // the handle is a handle to the share's own mapping, which carries its identity: "The duplicate handle
    // refers to the same object as the original handle" (Microsoft Learn, DuplicateHandle). Access is not
    // gated: the tests pass DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE and GENERIC_ALL alike
    BOOL inherit = Attributes ? Attributes->bInheritHandle : FALSE;
    if (!DuplicateHandle(GetCurrentProcess(), share_->named_mapping, GetCurrentProcess(), pNTHandle, 0, inherit,
                         DUPLICATE_SAME_ACCESS)) {
      ERR("DeviceTexture: Failed to create shared handle: ", GetLastError());
      return E_FAIL;
    }
    return S_OK;
  }

  void SetMinLOD(float MinLod) override { min_lod = MinLod; }

  float GetMinLOD() override { return min_lod; }
};

template <typename tag>
HRESULT CreateDeviceTextureInternal(MTLD3D11Device *pDevice,
                                    const typename tag::DESC1 *pDesc,
                                    const D3D11_SUBRESOURCE_DATA *pInitialData,
                                    typename tag::COM_IMPL **ppTexture) {
  WMTTextureInfo info{};
  typename tag::DESC1 finalDesc;
  if (FAILED(CreateMTLTextureDescriptor(pDevice, pDesc, &finalDesc, &info))) {
    return E_INVALIDARG;
  }
  bool single_subresource = info.mipmap_level_count == 1 && info.array_length == 1 &&
                            !(finalDesc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE);
  auto texture = Rc<Texture>(new Texture(info, pDevice->GetMTLDevice()));

  MTL_DXGI_FORMAT_DESC format_desc;
  MTLQueryDXGIFormat(pDevice->GetMTLDevice(), finalDesc.Format, format_desc);

  if (format_desc.Flag & MTL_DXGI_FORMAT_VIDEO_PLANAR) {
    MTL_DXGI_FORMAT_DESC plane;
    MTLQueryDXGIFormat(pDevice->GetMTLDevice(), finalDesc.Format, plane, 1);
    auto chroma_info = info;
    chroma_info.pixel_format = plane.PixelFormat;
    chroma_info.width /= plane.SubsampleWidth;
    chroma_info.height /= plane.SubsampleHeight;
    texture->chroma = new Texture(chroma_info, pDevice->GetMTLDevice());
  }

  auto &initializer = pDevice->GetMullionDevice().queue().initializer;

  auto initialize = [&](Rc<TextureAllocation> &&allocation) -> HRESULT {
    if (!allocation->texture())
      return E_OUTOFMEMORY;
    texture->rename(std::move(allocation));
    if (texture->chroma) {
      auto chroma = texture->chroma;
      chroma->rename(chroma->allocate(texture->current()->flags()));
      if (!chroma->current()->texture())
        return E_OUTOFMEMORY;
      for (auto sub : EnumerateSubresources(finalDesc)) {
        if (!pInitialData) {
          initializer.initWithZero(chroma.ptr(), chroma->current(), sub.ArraySlice, sub.MipLevel);
        } else {
          auto &data = pInitialData[sub.SubresourceId];
          initializer.initWithData(
              chroma.ptr(), chroma->current(), sub.ArraySlice, sub.MipLevel,
              ptr_add(data.pSysMem, std::uintptr_t(data.SysMemPitch) * texture->height()), data.SysMemPitch, 0, 0
          );
        }
      }
    }
    if (!pInitialData) {
      for (auto sub : EnumerateSubresources(finalDesc)) {
        initializer.initWithZero(texture.ptr(), texture->current(), sub.ArraySlice, sub.MipLevel);
      }
    } else {
      for (auto sub : EnumerateSubresources(finalDesc)) {
        auto &data = pInitialData[sub.SubresourceId];
        initializer.initWithData(
            texture.ptr(), texture->current(), sub.ArraySlice, sub.MipLevel, data.pSysMem, data.SysMemPitch,
            data.SysMemSlicePitch, format_desc.Flag
        );
      }
    }
    return S_OK;
  };

  auto shared_flag =
      D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
  if (finalDesc.MiscFlags & shared_flag) {
    // use a dedicated path for now, because there are other works for private storage mode

    Flags<TextureAllocationFlag> flags;
    flags.set(TextureAllocationFlag::GpuPrivate);
    if (finalDesc.Usage == D3D11_USAGE_IMMUTABLE)
      flags.set(TextureAllocationFlag::GpuReadonly);
    if (!(finalDesc.BindFlags & (D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)))
      flags.set(TextureAllocationFlag::ShaderReadonly);
    flags.set(TextureAllocationFlag::Shared);
    auto allocation = texture->allocate(flags);

    mach_port_t mach_port = allocation->machPort;
    if (!mach_port) {
      ERR("DeviceTexture: Failed to get mach port for shared texture");
      return E_FAIL;
    }
    HRESULT hr = initialize(std::move(allocation));
    if (FAILED(hr))
      return hr;
    // a shared allocation is observed from other devices and processes, which cannot order against this queue:
    // the initial contents must land in it before any handle to the share exists. creation has no handle to give
    // out yet, so it records the wait it owes and leaves it to be paid where the share first becomes visible
    Rc<TextureShare> share(new TextureShare());
    memcpy(share->data.tag, shared_texture_tag, sizeof(shared_texture_tag));
    share->data.share_id = MakeShareId();
    share->unpublished_device = pDevice;
    MakeUniqueSharedName(share->data.mach_port_name);
    share->data.dimension = tag::dimension;
    memcpy(&share->data.desc, pDesc, sizeof(typename tag::DESC1));
    if (!WMTBootstrapRegister(share->data.mach_port_name, mach_port)) {
      ERR("DeviceTexture: Failed to register mach port for shared texture");
      return E_FAIL;
    }
    if (texture->chroma &&
        !WMTBootstrapRegister((std::string(share->data.mach_port_name) + ".uv").c_str(),
                              texture->chroma->current()->machPort))
      return E_FAIL;

    Rc<KeyedMutex> keyed_mutex = {};
    if (finalDesc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) {
      keyed_mutex = KeyedMutex::create(pDevice->GetMTLDevice());
      if (!keyed_mutex)
        return E_FAIL;
      snprintf(share->data.mutex_name, sizeof(share->data.mutex_name), "%s", keyed_mutex->name());
    }

    {
      char mapping_name[64];
      ShareMappingName(share->data.share_id, mapping_name);
      share->named_mapping =
          CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SharedTextureData), mapping_name);
      if (!share->named_mapping) {
        ERR("DeviceTexture: Failed to create the share's mapping: ", GetLastError());
        return E_FAIL;
      }
      if (GetLastError() == ERROR_ALREADY_EXISTS) {
        ERR("DeviceTexture: Shared texture id collision");
        return E_FAIL;
      }
      void *view = MapViewOfFile(share->named_mapping, FILE_MAP_WRITE, 0, 0, sizeof(SharedTextureData));
      if (!view) {
        ERR("DeviceTexture: Failed to write the share's mapping: ", GetLastError());
        return E_FAIL;
      }
      memcpy(view, &share->data, sizeof(SharedTextureData));
      UnmapViewOfFile(view);

      std::lock_guard<std::mutex> lock(share_registry_mutex);
      share_registry.emplace(share->data.share_id, share.ptr());
    }

    *ppTexture = reinterpret_cast<typename tag::COM_IMPL *>(
        ref(new DeviceTexture<tag>(&finalDesc, std::move(texture), std::move(share), std::move(keyed_mutex), pDevice))
    );
    return S_OK;
  }

  Flags<TextureAllocationFlag> flags;
  flags.set(finalDesc.CPUAccessFlags ? TextureAllocationFlag::GpuManaged : TextureAllocationFlag::GpuPrivate);
  if (!(finalDesc.BindFlags & (D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)))
    flags.set(TextureAllocationFlag::ShaderReadonly);
  if (finalDesc.Usage == D3D11_USAGE_IMMUTABLE)
    flags.set(TextureAllocationFlag::GpuReadonly);
  if (single_subresource && (finalDesc.BindFlags & D3D11_BIND_DEPTH_STENCIL)) {
    Rc<RenamableTexturePool> renamable = new RenamableTexturePool(texture.ptr(), 32, flags);
    HRESULT hr = initialize(renamable->getNext(0));
    if (FAILED(hr))
      return hr;
    *ppTexture = reinterpret_cast<typename tag::COM_IMPL *>(
        ref(new DeviceTexture<tag>(&finalDesc, std::move(texture), std::move(renamable), pDevice)));
  } else {
    HRESULT hr = initialize(texture->allocate(flags));
    if (FAILED(hr))
      return hr;
    *ppTexture = reinterpret_cast<typename tag::COM_IMPL *>(
        ref(new DeviceTexture<tag>(&finalDesc, std::move(texture), pDevice)));
  }
  return S_OK;
}

HRESULT
CreateDeviceTexture1D(MTLD3D11Device *pDevice,
                      const D3D11_TEXTURE1D_DESC *pDesc,
                      const D3D11_SUBRESOURCE_DATA *pInitialData,
                      ID3D11Texture1D **ppTexture) {
  return CreateDeviceTextureInternal<tag_texture_1d>(pDevice, pDesc,
                                                     pInitialData, ppTexture);
}

HRESULT
CreateDeviceTexture2D(MTLD3D11Device *pDevice,
                      const D3D11_TEXTURE2D_DESC1 *pDesc,
                      const D3D11_SUBRESOURCE_DATA *pInitialData,
                      ID3D11Texture2D1 **ppTexture) {
  return CreateDeviceTextureInternal<tag_texture_2d>(pDevice, pDesc,
                                                     pInitialData, ppTexture);
}

HRESULT
CreateDeviceTexture3D(MTLD3D11Device *pDevice,
                      const D3D11_TEXTURE3D_DESC1 *pDesc,
                      const D3D11_SUBRESOURCE_DATA *pInitialData,
                      ID3D11Texture3D1 **ppTexture) {
  return CreateDeviceTextureInternal<tag_texture_3d>(pDevice, pDesc,
                                                     pInitialData, ppTexture);
}

template <typename tag>
HRESULT
ImportSharedTextureInternal(
    MTLD3D11Device *pDevice, const Rc<TextureShare> &share, const typename tag::DESC1 *pDescUnchecked, REFIID riid,
    void **ppTexture
) {
  // a share of this process may still owe its initial contents: the mapping is named from creation, so a share
  // can be reached by name before any handle exists, and an import can then come first
  PublishShare(share.ptr());

  WMTTextureInfo info{};
  typename tag::DESC1 finalDesc;
  if (FAILED(CreateMTLTextureDescriptor(pDevice, pDescUnchecked, &finalDesc, &info)))
    return E_INVALIDARG;

  auto texture = Rc<Texture>(new Texture(info, pDevice->GetMTLDevice()));
  mach_port_t mach_port = 0;
  if (!WMTBootstrapLookUp(share->data.mach_port_name, &mach_port)) {
    ERR("ImportSharedTexture: Failed to look up mach port");
    return E_INVALIDARG;
  }
  auto allocation = texture->import(mach_port);
  if (!allocation)
    return E_FAIL;
  texture->rename(std::move(allocation));
  MTL_DXGI_FORMAT_DESC format;
  MTLQueryDXGIFormat(pDevice->GetMTLDevice(), finalDesc.Format, format);
  if (format.Flag & MTL_DXGI_FORMAT_VIDEO_PLANAR) {
    mach_port_t chroma_port;
    if (!WMTBootstrapLookUp((std::string(share->data.mach_port_name) + ".uv").c_str(), &chroma_port))
      return E_FAIL;
    MTLQueryDXGIFormat(pDevice->GetMTLDevice(), finalDesc.Format, format, 1);
    info.pixel_format = format.PixelFormat;
    info.width /= format.SubsampleWidth;
    info.height /= format.SubsampleHeight;
    texture->chroma = new Texture(info, pDevice->GetMTLDevice());
    auto chroma = texture->chroma->import(chroma_port);
    if (!chroma)
      return E_FAIL;
    texture->chroma->rename(std::move(chroma));
  }

  Rc<KeyedMutex> keyed_mutex;
  if (finalDesc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) {
    keyed_mutex = KeyedMutex::import(pDevice->GetMTLDevice(), share->data.mutex_name);
    if (!keyed_mutex)
      return E_FAIL;
  }

  Com<DeviceTexture<tag>> device_texture =
      new DeviceTexture<tag>(&finalDesc, std::move(texture), Rc<TextureShare>(share), std::move(keyed_mutex), pDevice);
  return device_texture->QueryInterface(riid, ppTexture);
}

HRESULT
ImportSharedTexture(MTLD3D11Device *pDevice, HANDLE hResource, REFIID riid, void **ppTexture) {
  InitReturnPtr(ppTexture);

  if (!IsLegacySharedHandle(hResource)) {
    WARN("ImportSharedTexture: Invalid shared handle type");
    return E_INVALIDARG;
  }

  if (ppTexture == nullptr)
    return S_FALSE;

  // the narrow path first: a handle that names a share of this process resolves by the identity it carries,
  // which needs no mapping. the named mapping stays the fallback, so a handle marshalled from another process of
  // the session resolves exactly as it always has
  const uint64_t share_id = reinterpret_cast<uintptr_t>(hResource) & ~(uintptr_t(1) << 62);
  Rc<TextureShare> share = FindShare(share_id);
  if (!share) {
    // the marshalled handle names its share's mapping, so it resolves in this process as it did in the one that
    // made it, for as long as any device anywhere still holds a resource of the share
    char mapping_name[64];
    ShareMappingName(share_id, mapping_name);
    HANDLE mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, mapping_name);
    if (!mapping) {
      // "released sharing chain retained its handle": no resource of the share exists anywhere any more
      WARN("ImportSharedTexture: shared handle is no longer valid");
      return E_INVALIDARG;
    }
    SharedTextureData data = {};
    void *view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(SharedTextureData));
    if (!view) {
      WARN("ImportSharedTexture: shared handle is unreadable: ", GetLastError());
      CloseHandle(mapping);
      return E_INVALIDARG;
    }
    memcpy(&data, view, sizeof(data));
    UnmapViewOfFile(view);
    CloseHandle(mapping);
    if (memcmp(data.tag, shared_texture_tag, sizeof(shared_texture_tag)) ||
        data.mach_port_name[sizeof(data.mach_port_name) - 1] || data.mutex_name[sizeof(data.mutex_name) - 1]) {
      WARN("ImportSharedTexture: handle is not a shared texture");
      return E_INVALIDARG;
    }
    share = JoinShare(data);
  }

  switch (share->data.dimension) {
  case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
    return ImportSharedTextureInternal<tag_texture_1d>(pDevice, share, &share->data.desc.desc1d, riid, ppTexture);
  case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
    return ImportSharedTextureInternal<tag_texture_2d>(pDevice, share, &share->data.desc.desc2d, riid, ppTexture);
  case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
    return ImportSharedTextureInternal<tag_texture_3d>(pDevice, share, &share->data.desc.desc3d, riid, ppTexture);
  default:
    ERR("ImportSharedTexture: Unsupported resource dimension");
    return E_INVALIDARG;
  }
}

HRESULT
ImportSharedTextureFromNtHandle(MTLD3D11Device *pDevice, HANDLE hResource, REFIID riid, void **ppTexture) {
  InitReturnPtr(ppTexture);

  if (IsLegacySharedHandle(hResource)) {
    WARN("ImportSharedTextureFromNtHandle: Invalid shared handle type");
    return E_INVALIDARG;
  }

  if (ppTexture == nullptr)
    return S_FALSE;

  SharedTextureData data = {};
  void *view = hResource ? MapViewOfFile(hResource, FILE_MAP_READ, 0, 0, sizeof(SharedTextureData)) : nullptr;
  if (!view) {
    WARN("ImportSharedTextureFromNtHandle: handle is not a shared texture");
    return E_INVALIDARG;
  }
  memcpy(&data, view, sizeof(data));
  UnmapViewOfFile(view);
  if (memcmp(data.tag, shared_texture_tag, sizeof(shared_texture_tag)) ||
      data.mach_port_name[sizeof(data.mach_port_name) - 1] || data.mutex_name[sizeof(data.mutex_name) - 1]) {
    WARN("ImportSharedTextureFromNtHandle: handle is not a shared texture");
    return E_INVALIDARG;
  }

  auto share = JoinShare(data);

  switch (data.dimension) {
  case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
    return ImportSharedTextureInternal<tag_texture_1d>(pDevice, share, &share->data.desc.desc1d, riid, ppTexture);
  case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
    return ImportSharedTextureInternal<tag_texture_2d>(pDevice, share, &share->data.desc.desc2d, riid, ppTexture);
  case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
    return ImportSharedTextureInternal<tag_texture_3d>(pDevice, share, &share->data.desc.desc3d, riid, ppTexture);
  default:
    ERR("ImportSharedTextureFromNtHandle: Unsupported resource dimension");
    return E_INVALIDARG;
  }
}

HRESULT
ImportSharedTextureByName(
    MTLD3D11Device *pDevice, LPCWSTR lpName, DWORD dwDesiredAccess, REFIID riid, void **ppTexture
) {
  // handles made here carry the share's identity in their bytes instead of a name, so a named open resolves
  // only when some producer registered one in the session namespace
  HANDLE mapping = OpenFileMappingW(dwDesiredAccess, FALSE, lpName);
  if (!mapping) {
    WARN("ImportSharedTextureByName: Failed to open shared texture by name");
    return E_INVALIDARG;
  }

  HRESULT res = ImportSharedTextureFromNtHandle(pDevice, mapping, riid, ppTexture);
  CloseHandle(mapping);
  return res;
}

#pragma endregion

} // namespace mullion