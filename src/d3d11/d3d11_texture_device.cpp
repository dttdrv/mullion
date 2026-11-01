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
#include "mullion_fence.hpp"
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
// "<name>.uv", and a keyed mutex's shared event carries its own name (tests/d3d11/d3d11_shared_resource.cpp)
struct SharedTextureData {
  char tag[16];
  uint64_t share_id;
  // the carrier's own name, so an opener that holds nothing but the share's identity - a name that resolved to it,
  // or a handle of it - opens the object that holds the state
  char mapping_name[64];
  // the name the share was published under, if one was: every resource of the share holds that name open for as
  // long as it lives, since "a name can be used by only one resource at a time" and the name belongs to the
  // resource and not to the process that happened to create it
  WCHAR published_name[MAX_PATH + 1];
  // the access this share was published with: "dwAccess ... one or more of the following values: DXGI_SHARED_RESOURCE_
  // READ 0x80000000, DXGI_SHARED_RESOURCE_WRITE 0x1" (Microsoft Learn, IDXGIResource1::CreateSharedHandle), which a
  // later OpenSharedResourceByName is checked against
  DWORD granted_access;
  char mach_port_name[54];
  char mutex_name[54];
  D3D11_RESOURCE_DIMENSION dimension;
  union {
    D3D11_TEXTURE1D_DESC desc1d;
    D3D11_TEXTURE2D_DESC1 desc2d;
    D3D11_TEXTURE3D_DESC1 desc3d;
  } desc;
};

// the carrier holds the identity and, past it, the keyed mutex's state, so one object carries both: a handle of
// this share is a handle to this object - "The duplicate handle refers to the same object as the original handle"
// (Microsoft Learn, DuplicateHandle) - and so is a handle to the name the share was published under, which is why
// a handle kept after every resource of the share is released still opens the mutex the share was made with
struct SharedTextureCarrier {
  SharedTextureData data;
  SharedMutexState mutex;
};
static constexpr size_t shared_texture_mutex_offset = offsetof(SharedTextureCarrier, mutex);

static constexpr char shared_texture_tag[16] = "MULLION_texture";

// "dwDesiredAccess ... accepts the same flags as dxgi.IDXGIResource1::CreateSharedHandle's dwAccess" (Microsoft
// Learn, IDXGIResource1::OpenSharedResourceByName)
static bool
WriteRequested(DWORD access) {
  return (access & (DXGI_SHARED_RESOURCE_WRITE | GENERIC_WRITE | GENERIC_ALL)) != 0;
}

// the carrier a name resolves to is a file mapping, so the DXGI access an open asks for is the file mapping
// access it means: a reader opens the mapping for reading, and a caller that asked for writing opens it for both
static DWORD
MappingAccess(DWORD access) {
  return WriteRequested(access) ? FILE_MAP_ALL_ACCESS : FILE_MAP_READ;
}

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

// the marker bit names a legacy handle and is never part of an id. a 32-bit HANDLE and a 64-bit HANDLE of the
// same share must be the same integer: the marker is bit 30, which both widths carry, so a truncated 64-bit
// GetSharedHandle is still a legacy handle a 32-bit OpenSharedResource can name (not a width-local bit 62)
static constexpr uint64_t legacy_handle_marker = uint64_t(1) << 30;
static constexpr uint64_t share_id_range = uint64_t(1) << 30;

static uint64_t
MakeShareId() {
  std::random_device rd;
  std::mt19937_64 gen(rd());
  std::uniform_int_distribution<uint64_t> dis;
  return dis(gen) & (share_id_range - 1);
}

// the live shared textures of this process. a legacy handle is a plain identity value and not a kernel handle
// (CloseHandle and DuplicateHandle must fail with ERROR_INVALID_HANDLE)
struct TextureShare {
  void incRef() {
    refcount.fetch_add(1u, std::memory_order_acquire);
  }

  // JoinShare takes its reference under the same lock, so a count that reaches 0 under it is final. dropped before
  // the lock, another thread could find the share, take it up and release it, and delete it before this one looks
  void decRef() {
    std::lock_guard<std::mutex> lock(share_registry_mutex);
    if (refcount.fetch_sub(1u, std::memory_order_acq_rel) != 1u)
      return;
    share_registry.erase(data.share_id);
    if (carrier)
      UnmapViewOfFile(carrier);
    if (named_mapping)
      CloseHandle(named_mapping);
    if (published_name)
      CloseHandle(published_name);
    delete this;
  }

  HANDLE legacyHandle() const {
    return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(data.share_id | legacy_handle_marker));
  }

  const char *
  mappingName() const {
    return data.mapping_name;
  }

  SharedTextureData data;
  // this process's view of the share's own object: the identity in `data` and the keyed mutex's state past it
  SharedTextureCarrier *carrier = nullptr;
  HANDLE named_mapping = nullptr;      // the carrier, opened by its name or taken from a handle of the share
  HANDLE published_name = nullptr;     // the name this share was published under, if a name was published
  std::atomic<uint32_t> refcount = {0u};
};

static bool
IsLegacySharedHandle(HANDLE hResource) {
  return (uint64_t(uintptr_t(hResource)) & legacy_handle_marker) != 0;
}

// one share is one entry per process however its handle arrives; the entry holds the share's object open while any
// resource of the share lives here. the object is opened by the name its identity records - its id - and that name
// lives for as long as the object does, so a handle of the share left after every resource went is itself one of
// the object's references and still resolves
static Rc<TextureShare>
JoinShare(const SharedTextureData &data, HANDLE carried = nullptr) {
  std::lock_guard<std::mutex> lock(share_registry_mutex);
  auto it = share_registry.find(data.share_id);
  if (it != share_registry.end())
    return Rc<TextureShare>(it->second);
  auto share = Rc<TextureShare>(new TextureShare());
  share->data = data;
  share->named_mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, share->mappingName());
  if (!share->named_mapping && carried) {
    // the caller's own handle keeps the object alive even where no name resolved it
    HANDLE duplicate = nullptr;
    if (DuplicateHandle(GetCurrentProcess(), carried, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS))
      share->named_mapping = duplicate;
  }
  if (share->named_mapping) {
    share->carrier = static_cast<SharedTextureCarrier *>(
        MapViewOfFile(share->named_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedTextureCarrier))
    );
    // the object has to be this share's carrier and not some other mapping of that shape
    if (share->carrier && (memcmp(share->carrier->data.tag, shared_texture_tag, sizeof(shared_texture_tag)) ||
                           share->carrier->data.share_id != data.share_id)) {
      UnmapViewOfFile(share->carrier);
      share->carrier = nullptr;
    }
    // a share that was published under a name holds that name open while any resource of the share is alive in
    // this process, so the name outlives the process that published it, as the resource it belongs to does
    if (share->carrier && share->carrier->data.published_name[0])
      share->published_name = OpenFileMappingW(FILE_MAP_READ, FALSE, share->carrier->data.published_name);
  }
  share_registry.emplace(data.share_id, share.ptr());
  return share;
}

#pragma endregion

#pragma region DeviceTexture

template <typename DESC>
bool
centered(const DESC &desc) {
  if constexpr (requires { desc.SampleDesc; })
    return desc.SampleDesc.Quality == D3D11_CENTER_MULTISAMPLE_PATTERN;
  return false;
}

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
        this->texture_->centered = centered(*pDesc);
      }

  DeviceTexture(
      const tag_texture::DESC1 *pDesc, Rc<Texture> &&u_texture, Rc<RenamableTexturePool> &&renamable,
      MTLD3D11Device *pDevice
  ) :
      TResourceBase<tag_texture, IMTLMinLODClampable>(*pDesc, pDevice),
      renamable_(std::move(renamable)) {
        this->texture_ = std::move(u_texture);
        this->texture_->centered = centered(*pDesc);
      }

  DeviceTexture(
      const tag_texture::DESC1 *pDesc, Rc<Texture> &&u_texture, Rc<TextureShare> &&share,
      Rc<KeyedMutex> &&keyed_mutex, MTLD3D11Device *pDevice
  ) :
      TResourceBase<tag_texture, IMTLMinLODClampable>(*pDesc, pDevice), share_(std::move(share)) {
        this->texture_ = std::move(u_texture);
        this->texture_->centered = centered(*pDesc);
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
    // a swizzled view is read only: an attachment stores the channels as they are (RenderTargetChannels)
    auto format = descriptor.format;
    descriptor.format = WMTPixelFormat(ORIGINAL_FORMAT(format));
    *ppView = ref(new TextureRTV(descriptor, format, &finalDesc, this, this->m_parent, attachment_desc));
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

    *pSharedHandle = share_->legacyHandle();
    return S_OK;
  }

  virtual HRESULT
  CreateSharedHandle(const SECURITY_ATTRIBUTES *Attributes, DWORD Access, const WCHAR *pName, HANDLE *pNTHandle)
      override {
    if (this->wrapped_resource)
      return TResourceBase<tag_texture, IMTLMinLODClampable>::CreateSharedHandle(Attributes, Access, pName, pNTHandle);
    InitReturnPtr(pNTHandle);
    if (!pNTHandle || !share_ || !(this->desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE))
      return E_INVALIDARG;
    // "The name is limited to MAX_PATH characters." (Microsoft Learn, ID3D11Fence::CreateSharedHandle)
    if (pName && wcslen(pName) > MAX_PATH)
      return DXGI_ERROR_INVALID_CALL;
    if (!share_->carrier)
      return E_FAIL;

    // the access this handle grants is what a later open of the share by name is checked against
    share_->data.granted_access = Access;
    share_->carrier->data.granted_access = Access;

    // "the name is in the same namespace as the name of any other object that can be opened by a process, such as a
    // file, a mutex, or an event, and a name can be used by only one resource at a time" and "Creation fails if the
    // name is already used" with DXGI_ERROR_NAME_ALREADY_EXISTS (Microsoft Learn, IDXGIResource1::CreateSharedHandle)
    if (pName && !share_->published_name) {
      // the carrier is named after the share's id, and one object has one name, so the caller's name is published
      // as a mapping of the same identity: an open by that name finds the identity, whose `mapping_name` is the
      // carrier that holds the share's state. every resource of the share then holds this name open, its creator
      // included, so the name lives as long as a resource of the share does anywhere
      swprintf(share_->carrier->data.published_name, MAX_PATH + 1, L"%s", pName);
      swprintf(share_->data.published_name, MAX_PATH + 1, L"%s", pName);
      SECURITY_ATTRIBUTES internal = {sizeof(internal), nullptr, FALSE};
      if (Attributes)
        internal = *Attributes;
      // the caller's inheritance belongs to the handle at the end of this call; this object's own handle is not the
      // caller's, so it is never inherited, while the security descriptor is the object's and is handed to it
      internal.bInheritHandle = FALSE;
      HANDLE published = CreateFileMappingW(
          INVALID_HANDLE_VALUE, &internal, PAGE_READWRITE, 0, sizeof(SharedTextureData), pName
      );
      if (!published) {
        ERR("DeviceTexture: Failed to publish the shared texture's name: ", GetLastError());
        return E_FAIL;
      }
      if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(published);
        WARN("DeviceTexture: the shared texture's name is already in use");
        return DXGI_ERROR_NAME_ALREADY_EXISTS;
      }
      void *view = MapViewOfFile(published, FILE_MAP_WRITE, 0, 0, sizeof(SharedTextureData));
      if (!view) {
        ERR("DeviceTexture: Failed to write the shared texture's name: ", GetLastError());
        CloseHandle(published);
        return E_FAIL;
      }
      memcpy(view, &share_->data, sizeof(SharedTextureData));
      UnmapViewOfFile(view);
      share_->published_name = published;
    }

    // the caller gets a handle to the share's own object: "The duplicate handle refers to the same object as the
    // original handle" (Microsoft Learn, DuplicateHandle), which is what makes a kept handle outlive every resource
    // of the share, inheriting or being duplicated into another process as the caller asked
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
                                    typename tag::COM_IMPL **ppTexture,
                                    WMTTextureUsage usage = WMTTextureUsageUnknown) {
  WMTTextureInfo info{};
  typename tag::DESC1 finalDesc;
  if (FAILED(CreateMTLTextureDescriptor(pDevice, pDesc, &finalDesc, &info))) {
    return E_INVALIDARG;
  }
  info.usage = WMTTextureUsage(info.usage | usage);
  // a renamed texture would leave the Direct3D 12 resource Direct3D 11 on 12 makes of it
  bool single_subresource = info.mipmap_level_count == 1 && info.array_length == 1 &&
                            !(finalDesc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) && !pDevice->shares_with_d3d12;
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
        auto data = pInitialData[sub.SubresourceId];
        // a Texture1D's pitches have no meaning (D3D11_SUBRESOURCE_DATA) and may be anything, 0 among them; its Metal
        // texture is a 2D one, one row high, whose row is as wide as the level
        if constexpr (tag::dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D) {
          uint32_t row, image, length;
          if (FAILED(GetLinearTextureLayout(pDevice, finalDesc, sub.MipLevel, row, image, length, false)))
            return E_FAIL;
          data.SysMemPitch = data.SysMemSlicePitch = row;
        }
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
    // the initial contents must land in it before any handle to the share exists, so the initializer's batched
    // uploads are flushed and waited for here
    if (auto seq = initializer.flushToWait())
      initializer.wait(seq);
    Rc<TextureShare> share(new TextureShare());
    memcpy(share->data.tag, shared_texture_tag, sizeof(shared_texture_tag));
    share->data.share_id = MakeShareId();
    MakeUniqueSharedName(share->data.mach_port_name);
    share->data.dimension = tag::dimension;
    memcpy(&share->data.desc, pDesc, sizeof(typename tag::DESC1));
    if (!WMTBootstrapRegister(share->data.mach_port_name, texture->current()->texture().handle, mach_port)) {
      ERR("DeviceTexture: Failed to register mach port for shared texture");
      return E_FAIL;
    }
    if (texture->chroma &&
        !WMTBootstrapRegister((std::string(share->data.mach_port_name) + ".uv").c_str(),
                              texture->chroma->current()->texture().handle, texture->chroma->current()->machPort))
      return E_FAIL;

    // the share's object: the carrier holds the identity and the keyed mutex's state, so one handle of this share
    // says everything the share is. it stays mapped here for as long as the share lives, since the state is the
    // share's own bytes and the mutex that owns them maps the same object
    {
      char mapping_name[64];
      ShareMappingName(share->data.share_id, mapping_name);
      snprintf(share->data.mapping_name, sizeof(share->data.mapping_name), "%s", mapping_name);
      share->named_mapping = CreateFileMappingA(
          INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SharedTextureCarrier), mapping_name
      );
      if (!share->named_mapping) {
        ERR("DeviceTexture: Failed to create the share's mapping: ", GetLastError());
        return E_FAIL;
      }
      if (GetLastError() == ERROR_ALREADY_EXISTS) {
        ERR("DeviceTexture: Shared texture id collision");
        return E_FAIL;
      }
      share->carrier = static_cast<SharedTextureCarrier *>(
          MapViewOfFile(share->named_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedTextureCarrier))
      );
      if (!share->carrier) {
        ERR("DeviceTexture: Failed to map the share's mapping: ", GetLastError());
        return E_FAIL;
      }
      memset(share->carrier, 0, sizeof(SharedTextureCarrier));
      share->carrier->data = share->data;
    }

    Rc<KeyedMutex> keyed_mutex = {};
    if (finalDesc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) {
      // the mutex's state lives in the share's carrier, so every handle of the share carries it: a handle kept
      // after the resources of the share are released still names the key, its owner and its abandonment
      keyed_mutex = KeyedMutex::create(pDevice->GetMTLDevice(), share->named_mapping, shared_texture_mutex_offset);
      if (!keyed_mutex)
        return E_FAIL;
      snprintf(share->carrier->data.mutex_name, sizeof(share->data.mutex_name), "%s", keyed_mutex->name());
      // the identity the share hands out is the carrier's, which now names the mutex's shared event too
      share->data = share->carrier->data;
    }

    {
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
                      ID3D11Texture2D1 **ppTexture,
                      WMTTextureUsage usage) {
  return CreateDeviceTextureInternal<tag_texture_2d>(pDevice, pDesc,
                                                     pInitialData, ppTexture, usage);
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
  WMT::SharedNameHold hold(share->data.mach_port_name);
  auto allocation = texture->import(mach_port);
  if (!allocation)
    return E_FAIL;
  hold.keep(allocation->texture().handle);
  texture->rename(std::move(allocation));
  MTL_DXGI_FORMAT_DESC format;
  MTLQueryDXGIFormat(pDevice->GetMTLDevice(), finalDesc.Format, format);
  if (format.Flag & MTL_DXGI_FORMAT_VIDEO_PLANAR) {
    mach_port_t chroma_port;
    if (!WMTBootstrapLookUp((std::string(share->data.mach_port_name) + ".uv").c_str(), &chroma_port))
      return E_FAIL;
    WMT::SharedNameHold chroma_hold(std::string(share->data.mach_port_name) + ".uv");
    MTLQueryDXGIFormat(pDevice->GetMTLDevice(), finalDesc.Format, format, 1);
    info.pixel_format = format.PixelFormat;
    info.width /= format.SubsampleWidth;
    info.height /= format.SubsampleHeight;
    texture->chroma = new Texture(info, pDevice->GetMTLDevice());
    auto chroma = texture->chroma->import(chroma_port);
    if (!chroma)
      return E_FAIL;
    chroma_hold.keep(chroma->texture().handle);
    texture->chroma->rename(std::move(chroma));
  }

  Rc<KeyedMutex> keyed_mutex;
  if (finalDesc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) {
    if (!share->carrier || !share->named_mapping) {
      ERR("ImportSharedTexture: the share's keyed mutex state is gone");
      return E_FAIL;
    }
    keyed_mutex = KeyedMutex::import(
        pDevice->GetMTLDevice(), share->named_mapping, shared_texture_mutex_offset, share->data.mutex_name
    );
    if (!keyed_mutex)
      return E_FAIL;
  }

  Com<DeviceTexture<tag>> device_texture =
      new DeviceTexture<tag>(&finalDesc, std::move(texture), Rc<TextureShare>(share), std::move(keyed_mutex), pDevice);
  return device_texture->QueryInterface(riid, ppTexture);
}

// a texture over a Metal texture another API made (D3D11On12's CreateWrappedResource): the description must make
// the same Metal texture, which its format and size check
template <typename tag>
static HRESULT
WrapTextureInternal(
    MTLD3D11Device *pDevice, const typename tag::DESC1 *pDescUnchecked, WMT::Reference<WMT::Texture> &&metal,
    const MullionMetalResource &Info, REFIID riid, void **ppTexture
) {
  WMTTextureInfo info{};
  typename tag::DESC1 finalDesc;
  if (FAILED(CreateMTLTextureDescriptor(pDevice, pDescUnchecked, &finalDesc, &info)))
    return E_INVALIDARG;
  MTL_DXGI_FORMAT_DESC format;
  MTLQueryDXGIFormat(pDevice->GetMTLDevice(), finalDesc.Format, format);
  if (format.Flag & MTL_DXGI_FORMAT_VIDEO_PLANAR) {
    ERR("WrapTexture: planar video formats are not supported");
    return E_INVALIDARG;
  }
  if (metal.pixelFormat() != info.pixel_format || metal.width() != info.width || metal.height() != info.height) {
    ERR("WrapTexture: the description does not make the texture it wraps");
    return E_INVALIDARG;
  }
  auto texture = Rc<Texture>(new Texture(Info.texture_info, pDevice->GetMTLDevice()));
  auto allocation = texture->wrap(std::move(metal), Info.texture_info, Info.flags);
  if (!allocation)
    return E_INVALIDARG;
  texture->rename(std::move(allocation));
  Com<DeviceTexture<tag>> device_texture = new DeviceTexture<tag>(&finalDesc, std::move(texture), pDevice);
  return device_texture->QueryInterface(riid, ppTexture);
}

HRESULT
WrapTexture(
    MTLD3D11Device *pDevice, D3D11_RESOURCE_DIMENSION Dimension, const void *pDesc1, WMT::Reference<WMT::Texture> &&Metal,
    const MullionMetalResource &Info, REFIID riid, void **ppTexture
) {
  InitReturnPtr(ppTexture);
  switch (Dimension) {
  case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
    return WrapTextureInternal<tag_texture_1d>(
        pDevice, static_cast<const D3D11_TEXTURE1D_DESC *>(pDesc1), std::move(Metal), Info, riid, ppTexture
    );
  case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
    return WrapTextureInternal<tag_texture_2d>(
        pDevice, static_cast<const D3D11_TEXTURE2D_DESC1 *>(pDesc1), std::move(Metal), Info, riid, ppTexture
    );
  case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
    return WrapTextureInternal<tag_texture_3d>(
        pDevice, static_cast<const D3D11_TEXTURE3D_DESC1 *>(pDesc1), std::move(Metal), Info, riid, ppTexture
    );
  default:
    return E_INVALIDARG;
  }
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

  // the marshalled handle names its share's mapping, so it resolves in this process as it did in the one that
  // made it, for as long as any device anywhere still holds a resource of the share
  char mapping_name[64];
  ShareMappingName(uint64_t(uintptr_t(hResource)) & ~legacy_handle_marker, mapping_name);
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

  auto share = JoinShare(data);

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

  // the handle carries the share's object, so this process's entry is that object even where nothing else of the
  // share is left anywhere
  auto share = JoinShare(data, hResource);

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
  // "opens a shared resource using a name of a shared resource that was created with the NT handle sharing flag"
  // (Microsoft Learn, IDXGIResource1::OpenSharedResourceByName): the name a CreateSharedHandle published resolves
  // to the share's identity, which names the carrier that holds the share's keyed mutex state
  HANDLE mapping = nullptr;
  if (FAILED(openHandleByName(lpName, MappingAccess(dwDesiredAccess), &mapping))) {
    WARN("ImportSharedTextureByName: Failed to open shared texture by name");
    return E_INVALIDARG;
  }

  SharedTextureData data = {};
  void *view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(SharedTextureData));
  if (!view) {
    WARN("ImportSharedTextureByName: the name is unreadable: ", GetLastError());
    CloseHandle(mapping);
    return E_INVALIDARG;
  }
  memcpy(&data, view, sizeof(data));
  UnmapViewOfFile(view);
  // "returns E_ACCESSDENIED if permissions to access the resource are invalid": the access the share was published
  // with is what an open that asks for more is refused with
  if (WriteRequested(dwDesiredAccess) && !WriteRequested(data.granted_access)) {
    WARN("ImportSharedTextureByName: the share was not published for writing");
    CloseHandle(mapping);
    return E_ACCESSDENIED;
  }

  HRESULT res = ImportSharedTextureFromNtHandle(pDevice, mapping, riid, ppTexture);
  CloseHandle(mapping);
  return res;
}

#pragma endregion

} // namespace mullion
