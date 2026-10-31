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

namespace mullion {

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
  D3DKMT_HANDLE local_kmt_ = 0;
  D3DKMT_HANDLE global_kmt_ = 0;

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
      const tag_texture::DESC1 *pDesc, Rc<Texture> &&u_texture, D3DKMT_HANDLE localHandle, D3DKMT_HANDLE globalHandle,
      Rc<KeyedMutex> && keyed_mutex, MTLD3D11Device *pDevice
  ) :
      TResourceBase<tag_texture, IMTLMinLODClampable>(*pDesc, pDevice),
      local_kmt_(localHandle), global_kmt_(globalHandle) {
        this->texture_ = std::move(u_texture);
        this->texture_->centered = centered(*pDesc);
        this->keyed_mutex_ = std::move(keyed_mutex);
      }

  ~DeviceTexture() {
    if (local_kmt_) {
      D3DKMT_DESTROYALLOCATION destroy = {};
      destroy.hDevice = this->m_parent->GetLocalD3DKMT();
      destroy.hResource = local_kmt_;
      D3DKMTDestroyAllocation(&destroy);
    }
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

    if (!global_kmt_) {
      return E_INVALIDARG;
    }

    *pSharedHandle = reinterpret_cast<HANDLE>(global_kmt_);
    return S_OK;
  }

  virtual HRESULT
  CreateSharedHandle(const SECURITY_ATTRIBUTES *Attributes, DWORD Access, const WCHAR *pName, HANDLE *pNTHandle)
      override {
    if (this->wrapped_resource)
      return TResourceBase<tag_texture, IMTLMinLODClampable>::CreateSharedHandle(Attributes, Access, pName, pNTHandle);
    InitReturnPtr(pNTHandle);
    if (!local_kmt_ || !(this->desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE))
      return E_INVALIDARG;

    OBJECT_ATTRIBUTES attr = {};
    attr.Length = sizeof(attr);
    attr.SecurityDescriptor = const_cast<SECURITY_ATTRIBUTES*>(Attributes);

    WCHAR buffer[MAX_PATH];
    UNICODE_STRING name_str;
    if (pName) {
      DWORD session, len, name_len = wcslen(pName);

      ProcessIdToSessionId(GetCurrentProcessId(), &session);
      len = swprintf(buffer, ARRAYSIZE(buffer), L"\\Sessions\\%u\\BaseNamedObjects\\", session);
      memcpy(buffer + len, pName, (name_len + 1) * sizeof(WCHAR));
      name_str.MaximumLength = name_str.Length = (len + name_len) * sizeof(WCHAR);
      name_str.MaximumLength += sizeof(WCHAR);
      name_str.Buffer = buffer;

      attr.ObjectName = &name_str;
      attr.Attributes = OBJ_CASE_INSENSITIVE;
    }

    if (D3DKMTShareObjects(1, &local_kmt_, &attr, Access, pNTHandle)) {
      ERR("DeviceTexture: Failed to create shared handle");
      return E_FAIL;
    }

    return S_OK;
  }

  void SetMinLOD(float MinLod) override { min_lod = MinLod; }

  float GetMinLOD() override { return min_lod; }
};

struct SharedResourceData {
  char mach_port_name[54];
  D3D11_RESOURCE_DIMENSION dimension;
  union {
    D3D11_TEXTURE1D_DESC desc1d;
    D3D11_TEXTURE2D_DESC1 desc2d;
    D3D11_TEXTURE3D_DESC1 desc3d;
  } desc;
  D3DKMT_HANDLE mutex_handle;
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
    if (!(pDevice->GetLocalD3DKMT() & 0xc0000000)) {
      ERR("DeviceTexture: Invalid device handle");
      return E_FAIL;
    }
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
    SharedResourceData runtimeData;
    MakeUniqueSharedName(runtimeData.mach_port_name);
    if (!WMTBootstrapRegister(runtimeData.mach_port_name, texture->current()->texture().handle, mach_port)) {
      ERR("DeviceTexture: Failed to register mach port for shared texture");
      return E_FAIL;
    }
    if (texture->chroma &&
        !WMTBootstrapRegister((std::string(runtimeData.mach_port_name) + ".uv").c_str(),
                              texture->chroma->current()->texture().handle, texture->chroma->current()->machPort))
      return E_FAIL;
    runtimeData.dimension = tag::dimension;
    memcpy(&runtimeData.desc, pDesc, sizeof(typename tag::DESC1));

    Rc<KeyedMutex> keyed_mutex = {};
    if (finalDesc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) {
      keyed_mutex = KeyedMutex::create(pDevice->GetMTLDevice());
    }
    runtimeData.mutex_handle = keyed_mutex ? keyed_mutex->globalHandle() : 0;

    D3DKMT_CREATEALLOCATION create = {};
    create.hDevice = pDevice->GetLocalD3DKMT();
    create.pPrivateRuntimeData = &runtimeData;
    create.PrivateRuntimeDataSize = sizeof(runtimeData);
    create.Flags.StandardAllocation = 1;
    create.NumAllocations = 1;
    D3DDDI_ALLOCATIONINFO2 allocationInfo = {};
    create.pAllocationInfo2 = &allocationInfo;
    D3DKMT_CREATESTANDARDALLOCATION standardAllocation = {};
    create.pStandardAllocation = &standardAllocation;
    standardAllocation.Type = D3DKMT_STANDARDALLOCATIONTYPE_EXISTINGHEAP;
    create.Flags.ExistingSysMem = 1;
    D3DDDI_ALLOCATIONINFO systemMem;
    allocationInfo.pSystemMem = &systemMem;
    create.Flags.CreateResource = 1;
    create.Flags.CreateShared = 1;
    create.Flags.NtSecuritySharing = !!(finalDesc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE);
    if (D3DKMTCreateAllocation2(&create)) {
      ERR("DeviceTexture: Failed to create D3DKMT for shared texture");
      return E_FAIL;
    }

    *ppTexture = reinterpret_cast<typename tag::COM_IMPL *>(
        ref(new DeviceTexture<tag>(&finalDesc, std::move(texture), create.hResource,
                                   create.hGlobalShare, std::move(keyed_mutex), pDevice))
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
    MTLD3D11Device *pDevice, const typename tag::DESC1 *pDescUnchecked, mach_port_t MachPort, 
    D3DKMT_HANDLE &hResource, D3DKMT_HANDLE hGlobalShare, D3DKMT_HANDLE hSharedKeyedMutex, REFIID riid,
    void **ppTexture, const char *Name, WMT::SharedNameHold &Hold
) {
  WMTTextureInfo info{};
  typename tag::DESC1 finalDesc;
  if (FAILED(CreateMTLTextureDescriptor(pDevice, pDescUnchecked, &finalDesc, &info)))
    return E_INVALIDARG;

  auto texture = Rc<Texture>(new Texture(info, pDevice->GetMTLDevice()));
  auto allocation = texture->import(MachPort);
  if (!allocation)
    return E_FAIL;
  Hold.keep(allocation->texture().handle);
  texture->rename(std::move(allocation));
  MTL_DXGI_FORMAT_DESC format;
  MTLQueryDXGIFormat(pDevice->GetMTLDevice(), finalDesc.Format, format);
  if (format.Flag & MTL_DXGI_FORMAT_VIDEO_PLANAR) {
    mach_port_t chroma_port;
    if (!WMTBootstrapLookUp((std::string(Name) + ".uv").c_str(), &chroma_port))
      return E_FAIL;
    WMT::SharedNameHold chroma_hold(std::string(Name) + ".uv");
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
  if (hSharedKeyedMutex & 0xc0000000)
    keyed_mutex = KeyedMutex::import(pDevice->GetMTLDevice(), hSharedKeyedMutex);

  Com<DeviceTexture<tag>> device_texture =
      new DeviceTexture<tag>(&finalDesc, std::move(texture), hResource, hGlobalShare, std::move(keyed_mutex), pDevice);
  hResource = 0;
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

  if (!(reinterpret_cast<uintptr_t>(hResource) & 0xc0000000)) {
    WARN("ImportSharedTexture: Invalid shared handle type");
    return E_INVALIDARG;
  }

  if (ppTexture == nullptr)
    return S_FALSE;

  struct SharedResourceData runtimeData;

  D3DKMT_QUERYRESOURCEINFO query = {};
  query.hDevice = pDevice->GetLocalD3DKMT();
  query.hGlobalShare = reinterpret_cast<uintptr_t>(hResource);
  query.pPrivateRuntimeData = &runtimeData;
  query.PrivateRuntimeDataSize = sizeof(runtimeData);

  if (D3DKMTQueryResourceInfo(&query)) {
    WARN("ImportSharedTexture: Failed to query resource: ", hResource);
    return E_INVALIDARG;
  }

  if (query.PrivateRuntimeDataSize != sizeof(runtimeData)) {
    WARN("ImportSharedTexture: Unexpected size: ", query.PrivateRuntimeDataSize);
    return E_INVALIDARG;
  } 

  D3DDDI_OPENALLOCATIONINFO2 alloc = {};
  D3DKMT_OPENRESOURCE open = {};
  open.hDevice = pDevice->GetLocalD3DKMT();
  open.hGlobalShare = reinterpret_cast<uintptr_t>(hResource);
  open.NumAllocations = 1;
  open.pOpenAllocationInfo2 = &alloc;
  open.pPrivateRuntimeData = &runtimeData;
  open.PrivateRuntimeDataSize = query.PrivateRuntimeDataSize;

  if (D3DKMTOpenResource2(&open)) {
    WARN("ImportSharedTexture: Failed to open resource: ", hResource);
    return E_INVALIDARG;
  }

  HRESULT hr = E_INVALIDARG;
  mach_port_t mach_port;
  if (!WMTBootstrapLookUp(runtimeData.mach_port_name, &mach_port)) {
    ERR("ImportSharedTexture: Failed to look up mach port");
  } else {
    WMT::SharedNameHold hold(runtimeData.mach_port_name);
    switch (runtimeData.dimension) {
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
      hr = ImportSharedTextureInternal<tag_texture_1d>(
          pDevice, &runtimeData.desc.desc1d, mach_port, open.hResource, open.hGlobalShare, runtimeData.mutex_handle,
          riid, ppTexture, runtimeData.mach_port_name, hold
      );
      break;
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
      hr = ImportSharedTextureInternal<tag_texture_2d>(
          pDevice, &runtimeData.desc.desc2d, mach_port, open.hResource, open.hGlobalShare, runtimeData.mutex_handle,
          riid, ppTexture, runtimeData.mach_port_name, hold
      );
      break;
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
      hr = ImportSharedTextureInternal<tag_texture_3d>(
          pDevice, &runtimeData.desc.desc3d, mach_port, open.hResource, open.hGlobalShare, runtimeData.mutex_handle,
          riid, ppTexture, runtimeData.mach_port_name, hold
      );
      break;
    default:
      ERR("ImportSharedTexture: Unsupported resource dimension");
    }
  }
  if (open.hResource) {
    D3DKMT_DESTROYALLOCATION destroy = {};
    destroy.hDevice = pDevice->GetLocalD3DKMT();
    destroy.hResource = open.hResource;
    D3DKMTDestroyAllocation(&destroy);
  }
  return hr;
}

HRESULT
ImportSharedTextureFromNtHandle(MTLD3D11Device *pDevice, HANDLE hResource, REFIID riid, void **ppTexture) {
  InitReturnPtr(ppTexture);

  if (reinterpret_cast<uintptr_t>(hResource) & 0xc0000000) {
    WARN("ImportSharedTextureFromNtHandle: Invalid shared handle type");
    return E_INVALIDARG;
  }

  if (ppTexture == nullptr)
    return S_FALSE;

  struct SharedResourceData runtimeData;

  D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE query = {};
  query.hDevice = pDevice->GetLocalD3DKMT();
  query.hNtHandle = hResource;
  query.pPrivateRuntimeData = &runtimeData;
  query.PrivateRuntimeDataSize = sizeof(runtimeData);

  if (D3DKMTQueryResourceInfoFromNtHandle(&query)) {
    WARN(str::format("ImportSharedTextureFromNtHandle: Failed to query resource: ", hResource));
    return E_INVALIDARG;
  }
  
  if (query.PrivateRuntimeDataSize != sizeof(runtimeData)) {
    WARN(str::format("ImportSharedTextureFromNtHandle: Unexpected size: ", query.PrivateRuntimeDataSize));
    return E_INVALIDARG;
  }

  D3DDDI_OPENALLOCATIONINFO2 alloc = {};
  D3DKMT_OPENRESOURCEFROMNTHANDLE open = {};
  char dummy;

  open.hDevice = pDevice->GetLocalD3DKMT();
  open.hNtHandle = hResource;
  open.NumAllocations = 1;
  open.pOpenAllocationInfo2 = &alloc;
  open.pPrivateRuntimeData = &runtimeData;
  open.PrivateRuntimeDataSize = query.PrivateRuntimeDataSize;
  open.pTotalPrivateDriverDataBuffer = &dummy;
  open.TotalPrivateDriverDataBufferSize = 0;

  if (D3DKMTOpenResourceFromNtHandle(&open)) {
    WARN(str::format("ImportSharedTextureFromNtHandle: Failed to open resource: ", hResource));
    return E_INVALIDARG;
  }

  if (open.hSyncObject) {
    WARN(str::format("ImportSharedTextureFromNtHandle: Ignoring bundled sync object"));
    D3DKMT_DESTROYSYNCHRONIZATIONOBJECT destroySync = {};
    destroySync.hSyncObject = open.hSyncObject;
    D3DKMTDestroySynchronizationObject(&destroySync);
  }
  if (open.hKeyedMutex) {
    WARN(str::format("ImportSharedTextureFromNtHandle: Ignoring bundled keyed mutex"));
    D3DKMT_DESTROYKEYEDMUTEX destroyMutex = {};
    destroyMutex.hKeyedMutex = open.hKeyedMutex;
    D3DKMTDestroyKeyedMutex(&destroyMutex);
  }

  HRESULT hr = E_INVALIDARG;
  mach_port_t mach_port;
  if (!WMTBootstrapLookUp(runtimeData.mach_port_name, &mach_port)) {
    ERR("ImportSharedTexture: Failed to look up mach port");
  } else {
    WMT::SharedNameHold hold(runtimeData.mach_port_name);
    switch (runtimeData.dimension) {
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
      hr = ImportSharedTextureInternal<tag_texture_1d>(
          pDevice, &runtimeData.desc.desc1d, mach_port, open.hResource, 0, runtimeData.mutex_handle, riid, ppTexture,
          runtimeData.mach_port_name, hold
      );
      break;
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
      hr = ImportSharedTextureInternal<tag_texture_2d>(
          pDevice, &runtimeData.desc.desc2d, mach_port, open.hResource, 0, runtimeData.mutex_handle, riid, ppTexture,
          runtimeData.mach_port_name, hold
      );
      break;
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
      hr = ImportSharedTextureInternal<tag_texture_3d>(
          pDevice, &runtimeData.desc.desc3d, mach_port, open.hResource, 0, runtimeData.mutex_handle, riid, ppTexture,
          runtimeData.mach_port_name, hold
      );
      break;
    default:
      ERR("ImportSharedTexture: Unsupported resource dimension");
    }
  }
  if (open.hResource) {
    D3DKMT_DESTROYALLOCATION destroy = {};
    destroy.hDevice = pDevice->GetLocalD3DKMT();
    destroy.hResource = open.hResource;
    D3DKMTDestroyAllocation(&destroy);
  }
  return hr;
}

HRESULT
ImportSharedTextureByName(
    MTLD3D11Device *pDevice, LPCWSTR lpName, DWORD dwDesiredAccess, REFIID riid, void **ppTexture
) {
  HANDLE handle;
  if (FAILED(openHandleByName(lpName, dwDesiredAccess, &handle)))
    return E_INVALIDARG;
  HRESULT res = ImportSharedTextureFromNtHandle(pDevice, handle, riid, ppTexture);
  CloseHandle(handle);
  return res;
}

#pragma endregion

} // namespace mullion
