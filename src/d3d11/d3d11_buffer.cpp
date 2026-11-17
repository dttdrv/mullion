/*
 * Copyright 2026 Feifan He for CodeWeavers
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
#include "mullion_buffer.hpp"
#include "mullion_dynamic.hpp"
#include "mullion_format.hpp"
#include "d3d11_resource.hpp"
#include "util_env.hpp"
#include <random>

namespace mullion {

#pragma region Shared Buffer

// A buffer two devices share, in one process or two, is one file mapping: its identity in the first granule and
// its bytes in the granules after, which each device wraps in a Metal buffer of its own (a Metal buffer does not
// cross processes; the pages under it do). The mapping is named after the share's id, so a legacy handle, which is
// that id and no kernel handle, resolves in any process of the session for as long as a buffer of the share lives
// anywhere: "If no resource objects exist on any devices that refer to this resource, the handle is no longer
// valid" (Microsoft Learn, IDXGIResource::GetSharedHandle). Shared textures work the same way
// (d3d11_texture_device.cpp) and their handles carry the same marker bit.
//
// Only a VR runtime needs this (SteamVR's compositor and its applications pass each other a frame's bookkeeping in
// shared constant buffers), so it is on only with MULLION_METALVR (env::metalVR). Without it a buffer created
// with D3D11_RESOURCE_MISC_SHARED has no shared handle, as before.
struct SharedBufferData {
  char tag[16];
  uint64_t share_id;
  D3D11_BUFFER_DESC desc;
};

static constexpr char shared_buffer_tag[16] = "MULLION_buffer";
static constexpr uint64_t shared_buffer_handle_marker = uint64_t(1) << 30;
// a view of a mapping starts on a multiple of the allocation granularity (MapViewOfFile, dwFileOffsetLow), which
// is also a multiple of every page size a Metal buffer over the view has to be aligned to
static constexpr uint64_t shared_buffer_granule = 65536;

static void
SharedBufferName(uint64_t id, char (&out)[64]) {
  snprintf(out, 64, "MULLION_shared_buffer_%016llx", (unsigned long long)id);
}

// the share's bytes in this process, kept mapped and its mapping kept open for as long as the Metal buffer over
// them lives
static std::shared_ptr<void>
MapSharedBufferContents(HANDLE mapping, uint64_t length) {
  void *contents = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, DWORD(shared_buffer_granule), SIZE_T(length));
  if (!contents) {
    CloseHandle(mapping);
    return {};
  }
  return std::shared_ptr<void>(contents, [mapping](void *view) {
    UnmapViewOfFile(view);
    CloseHandle(mapping);
  });
}

#pragma endregion

struct BufferViewInfo {
  BufferViewKey viewKey;
  uint32_t viewElementOffset;
  uint32_t viewElementWidth;
  uint32_t byteOffset;
  uint32_t byteWidth;
};

class D3D11Buffer : public TResourceBase<tag_buffer> {
private:
#ifdef MULLION_DEBUG
  std::string debug_name;
#endif
  bool structured;
  bool allow_raw_view;
  // the share this buffer belongs to, which its legacy handle names; 0 for a buffer of its own
  uint64_t share_id_ = 0;

  Rc<DynamicBuffer> dynamic_;

  using SRVBase = TResourceViewBase<tag_shader_resource_view<D3D11Buffer>>;

  class TBufferSRV : public SRVBase {

  public:
    TBufferSRV(
        const tag_shader_resource_view<>::DESC1 *pDesc, D3D11Buffer *pResource, MTLD3D11Device *pDevice,
        BufferViewInfo const &info
    ) :
        SRVBase(pDesc, pResource, pDevice) {
      buffer_ = pResource->buffer_.ptr();
      view_id_ = info.viewKey;
      slice_ = {info.byteOffset, info.byteWidth, info.viewElementOffset, info.viewElementWidth };
      subset_ = ResourceSubsetState(info.byteOffset, info.byteWidth);
    }

    ~TBufferSRV() {}
  };

  using UAVBase = TResourceViewBase<tag_unordered_access_view<D3D11Buffer>>;

  class UAVWithCounter : public UAVBase {
  public:
    UAVWithCounter(
        const tag_unordered_access_view<>::DESC1 *pDesc, D3D11Buffer *pResource, MTLD3D11Device *pDevice,
        BufferViewInfo const &info, Rc<Counter>&& counter
    ) :
        UAVBase(pDesc, pResource, pDevice) {
      buffer_ = pResource->buffer_.ptr();
      view_id_ = info.viewKey;
      slice_ = {info.byteOffset, info.byteWidth, info.viewElementOffset, info.viewElementWidth };
      counter_ = std::move(counter);
      subset_ = ResourceSubsetState(info.byteOffset, info.byteWidth);
    }
  };

  class BufferRTV : public TResourceViewBase<tag_render_target_view<D3D11Buffer>> {
    Rc<Texture> attachment_;

  public:
    BufferRTV(const D3D11_RENDER_TARGET_VIEW_DESC1 *pDesc, D3D11Buffer *pResource, MTLD3D11Device *pDevice,
              const MTL_DXGI_FORMAT_DESC &format) :
        TResourceViewBase(pDesc, pResource, pDevice) {
      WMTTextureInfo info{};
      info.type = WMTTextureType2D;
      info.pixel_format = format.PixelFormat;
      info.width = pDesc->Buffer.NumElements;
      info.height = info.depth = info.array_length = info.mipmap_level_count = info.sample_count = 1;
      info.usage = WMTTextureUsageRenderTarget;
      // Metal's linear-texture alignment cannot represent every D3D11.3 5.2.1 buffer view offset
      attachment_ = new Texture(info, pDevice->GetMTLDevice());
      Flags<TextureAllocationFlag> flags;
      flags.set(TextureAllocationFlag::GpuPrivate);
      attachment_->rename(attachment_->allocate(flags));
      attachment_->render_buffer = pResource->buffer_;
      attachment_->render_buffer_offset = pDesc->Buffer.FirstElement * format.BytesPerTexel;
      texture_ = attachment_.ptr();
      view_id_ = texture_->fullView;
      format_ = format.PixelFormat;
      pass_desc_ = {0, info.sample_count, 0, info.width, info.height};
      subset_ =
          ResourceSubsetState(attachment_->render_buffer_offset, pDesc->Buffer.NumElements * format.BytesPerTexel);
    }
  };

public:
  D3D11Buffer(const tag_buffer::DESC1 *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData, MTLD3D11Device *device) :
      TResourceBase<tag_buffer>(*pDesc, device) {
    buffer_ = new Buffer(pDesc->ByteWidth, device->GetMTLDevice());
    Flags<BufferAllocationFlag> flags;
    // if (pDesc->Usage != D3D11_USAGE_DEFAULT)
    if (pDesc->Usage == D3D11_USAGE_IMMUTABLE)
      flags.set(BufferAllocationFlag::GpuReadonly);
    if (pDesc->Usage == D3D11_USAGE_DYNAMIC ||
        (pDesc->Usage == D3D11_USAGE_DEFAULT && !(pDesc->BindFlags & kD3D11OutputBindFlags))) {
      if (!m_parent->IsTraced())
        flags.set(BufferAllocationFlag::CpuWriteCombined);
      flags.set(BufferAllocationFlag::SuballocateFromOnePage);
#ifdef __i386__
      flags.set(BufferAllocationFlag::CpuPlaced);
#endif
    } else if (pInitialData || !(pDesc->BindFlags & kD3D11OutputBindFlags)) {
      flags.set(BufferAllocationFlag::GpuManaged);
    } else {
      flags.set(BufferAllocationFlag::GpuPrivate);
    }
    auto allocation = buffer_->allocate(flags);
    auto &initializer = device->GetMullionDevice().queue().initializer;
    if (pInitialData) {
      allocation->updateContents(0, pInitialData->pSysMem, pDesc->ByteWidth);
    } else if (flags.test(BufferAllocationFlag::GpuPrivate)) {
      initializer.initWithZero(allocation.ptr(), 0, buffer_->length());
    }
    auto _ = buffer_->rename(std::move(allocation));
    D3D11_ASSERT(_.ptr() == nullptr);
    structured = pDesc->MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    allow_raw_view = pDesc->MiscFlags & D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if (!(desc.BindFlags & kD3D11OutputBindFlags)) {
      dynamic_ = new DynamicBuffer(buffer_.ptr(), flags);
    }
  }

  // a buffer over another API's Metal buffer (WrapBuffer) or over a share's pages (`share`, its id), whose
  // contents it keeps: UpdateSubresource copies on the GPU
  D3D11Buffer(const tag_buffer::DESC1 *pDesc, Rc<Buffer> &&buffer, MTLD3D11Device *device, uint64_t share = 0) :
      TResourceBase<tag_buffer>(*pDesc, device),
      share_id_(share) {
    buffer_ = std::move(buffer);
    structured = pDesc->MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    allow_raw_view = pDesc->MiscFlags & D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) override {
    if (ppvObject == nullptr)
      return E_POINTER;

    return TResourceBase<tag_buffer>::QueryInterface(riid, ppvObject);
  }

  HRESULT
  GetSharedHandle(HANDLE *pSharedHandle) override {
    if (!share_id_)
      return TResourceBase<tag_buffer>::GetSharedHandle(pSharedHandle);
    if (!pSharedHandle)
      return E_INVALIDARG;
    *pSharedHandle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(share_id_ | shared_buffer_handle_marker));
    return S_OK;
  }

  Rc<StagingResource>
  staging(UINT Subresource) final {
    return nullptr;
  }
  Rc<DynamicBuffer>
  dynamicBuffer(UINT *pBufferLength, UINT *pBindFlags) override {
    *pBufferLength = desc.ByteWidth;
    *pBindFlags = desc.BindFlags;
    return dynamic_;
  }
  Rc<DynamicLinearTexture>
  dynamicLinearTexture(UINT *, UINT *) final {
    return {};
  };
  Rc<DynamicBuffer>
  dynamicTexture(UINT , UINT *, UINT *) final {
    return {};
  };

  HRESULT
  STDMETHODCALLTYPE
  CreateRenderTargetView(const D3D11_RENDER_TARGET_VIEW_DESC1 *pDesc, ID3D11RenderTargetView1 **ppView) override {
    D3D11_RENDER_TARGET_VIEW_DESC1 finalDesc;
    if (FAILED(ExtractEntireResourceViewDescription(m_parent, &desc, pDesc, &finalDesc, dxgi_usage_)) ||
        finalDesc.ViewDimension != D3D11_RTV_DIMENSION_BUFFER)
      return E_INVALIDARG;
    MTL_DXGI_FORMAT_DESC format;
    UINT support = 0;
    constexpr UINT required = D3D11_FORMAT_SUPPORT_BUFFER | D3D11_FORMAT_SUPPORT_RENDER_TARGET;
    if (FAILED(MTLQueryDXGIFormat(m_parent->GetMTLDevice(), finalDesc.Format, format)) || !format.BytesPerTexel ||
        (format.Flag & (MTL_DXGI_FORMAT_TYPELESS | MTL_DXGI_FORMAT_VIDEO_PLANAR)) ||
        FAILED(m_parent->CheckFormatSupport(finalDesc.Format, &support)) ||
        (support & required) != required)
      return E_INVALIDARG;
    const UINT elements = desc.ByteWidth / format.BytesPerTexel;
    const UINT max_width = m_parent->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0 ? D3D11_REQ_TEXTURE1D_U_DIMENSION
                                                                            : D3D10_REQ_TEXTURE1D_U_DIMENSION;
    if (finalDesc.Buffer.FirstElement >= elements || !finalDesc.Buffer.NumElements ||
        finalDesc.Buffer.NumElements > elements - finalDesc.Buffer.FirstElement ||
        finalDesc.Buffer.NumElements > max_width)
      return E_INVALIDARG;
    if (!ppView)
      return S_FALSE;
    auto view = ref(new BufferRTV(&finalDesc, this, m_parent, format));
    if (!view->texture()->current()->texture()) {
      view->Release();
      return E_OUTOFMEMORY;
    }
    *ppView = view;
    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  CreateShaderResourceView(const D3D11_SHADER_RESOURCE_VIEW_DESC1 *pDesc, ID3D11ShaderResourceView1 **ppView) override {
    D3D11_SHADER_RESOURCE_VIEW_DESC1 finalDesc;
    if (FAILED(
            ExtractEntireResourceViewDescription(this->m_parent, &this->desc, pDesc, &finalDesc, this->dxgi_usage_))) {
      return E_INVALIDARG;
    }
    if (finalDesc.ViewDimension != D3D11_SRV_DIMENSION_BUFFER &&
        finalDesc.ViewDimension != D3D11_SRV_DIMENSION_BUFFEREX) {
      return E_FAIL;
    }
    WMTPixelFormat view_format = WMTPixelFormatInvalid;
    uint32_t offset, size, viewElementOffset, viewElementWidth;
    if (structured) {
      if (finalDesc.Format != DXGI_FORMAT_UNKNOWN) {
        return E_INVALIDARG;
      }
      // StructuredBuffer
      CalculateBufferViewOffsetAndSize(
          this->desc, desc.StructureByteStride, finalDesc.Buffer.FirstElement, finalDesc.Buffer.NumElements, offset,
          size
      );
      view_format = WMTPixelFormatR32Uint;
      viewElementOffset = finalDesc.Buffer.FirstElement * (desc.StructureByteStride >> 2);
      viewElementWidth = finalDesc.Buffer.NumElements * (desc.StructureByteStride >> 2);
    } else if (finalDesc.ViewDimension == D3D11_SRV_DIMENSION_BUFFEREX && finalDesc.BufferEx.Flags & D3D11_BUFFEREX_SRV_FLAG_RAW) {
      if (!allow_raw_view)
        return E_INVALIDARG;
      if (finalDesc.Format != DXGI_FORMAT_R32_TYPELESS)
        return E_INVALIDARG;
      CalculateBufferViewOffsetAndSize(
          this->desc, sizeof(uint32_t), finalDesc.Buffer.FirstElement, finalDesc.Buffer.NumElements, offset, size
      );
      view_format = WMTPixelFormatR32Uint;
      viewElementOffset = finalDesc.Buffer.FirstElement;
      viewElementWidth = finalDesc.Buffer.NumElements;
    } else {
      MTL_DXGI_FORMAT_DESC format;
      if (FAILED(MTLQueryDXGIFormat(m_parent->GetMTLDevice(), finalDesc.Format, format))) {
        return E_FAIL;
      }
      if (!format.BytesPerTexel || (format.Flag & MTL_DXGI_FORMAT_VIDEO_PLANAR)) {
        ERR("D3D11Buffer::CreateShaderResourceView: not an ordinary or packed format: ", finalDesc.Format);
        return E_FAIL;
      }

      UINT support = 0;
      if (FAILED(m_parent->CheckFormatSupport(finalDesc.Format, &support)) || !(support & D3D11_FORMAT_SUPPORT_BUFFER))
        return E_INVALIDARG;
      view_format = format.BufferPixelFormat;
      offset = finalDesc.Buffer.FirstElement * format.BytesPerTexel;
      size = finalDesc.Buffer.NumElements * format.BytesPerTexel;
      viewElementOffset = finalDesc.Buffer.FirstElement * (format.BytesPerTexel / MTLGetTexelSize(view_format));
      viewElementWidth = finalDesc.Buffer.NumElements |
                         (format.Flag & MTL_DXGI_FORMAT_EMULATED_RGB32 ? SM50_TEXEL_BUFFER_METADATA_RGB32 : 0);
    }

    if (!ppView) {
      return S_FALSE;
    }

    auto viewId = buffer_->createView({.format = view_format});

    auto srv = ref(new TBufferSRV(
        &finalDesc, this, m_parent,
        {
            .viewKey = viewId,
            .viewElementOffset = viewElementOffset,
            .viewElementWidth = viewElementWidth,
            .byteOffset = offset,
            .byteWidth = size,
        }
    ));
    *ppView = srv;
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  CreateUnorderedAccessView(const D3D11_UNORDERED_ACCESS_VIEW_DESC1 *pDesc, ID3D11UnorderedAccessView1 **ppView)
      override {
    D3D11_UNORDERED_ACCESS_VIEW_DESC1 finalDesc;
    if (FAILED(
            ExtractEntireResourceViewDescription(this->m_parent, &this->desc, pDesc, &finalDesc, this->dxgi_usage_))) {
      return E_INVALIDARG;
    }
    if (finalDesc.ViewDimension != D3D11_UAV_DIMENSION_BUFFER) {
      return E_FAIL;
    }
    WMTPixelFormat view_format = WMTPixelFormatInvalid;
    uint32_t offset, size, viewElementOffset, viewElementWidth;
    Rc<Counter> counter = {};
    if (structured) {
      if (finalDesc.Format != DXGI_FORMAT_UNKNOWN) {
        return E_INVALIDARG;
      }
      // StructuredBuffer
      CalculateBufferViewOffsetAndSize(
          this->desc, desc.StructureByteStride, finalDesc.Buffer.FirstElement, finalDesc.Buffer.NumElements, offset,
          size
      );
      view_format = WMTPixelFormatR32Uint;
      // when structured buffer is interpreted as typed buffer for any reason
      viewElementOffset = finalDesc.Buffer.FirstElement * (desc.StructureByteStride >> 2);
      viewElementWidth = finalDesc.Buffer.NumElements * (desc.StructureByteStride >> 2);
      if (finalDesc.Buffer.Flags & (D3D11_BUFFER_UAV_FLAG_APPEND | D3D11_BUFFER_UAV_FLAG_COUNTER)) {
        counter = new Counter();
      }
    } else if (finalDesc.Buffer.Flags & D3D11_BUFFER_UAV_FLAG_RAW) {
      if (!allow_raw_view)
        return E_INVALIDARG;
      if (finalDesc.Format != DXGI_FORMAT_R32_TYPELESS)
        return E_INVALIDARG;
      CalculateBufferViewOffsetAndSize(
          this->desc, sizeof(uint32_t), finalDesc.Buffer.FirstElement, finalDesc.Buffer.NumElements, offset, size
      );
      view_format = WMTPixelFormatR32Uint;
      viewElementOffset = finalDesc.Buffer.FirstElement;
      viewElementWidth = finalDesc.Buffer.NumElements;
    } else {
      MTL_DXGI_FORMAT_DESC format;
      if (FAILED(MTLQueryDXGIFormat(m_parent->GetMTLDevice(), finalDesc.Format, format))) {
        return E_FAIL;
      }
      if (!format.BytesPerTexel || (format.Flag & MTL_DXGI_FORMAT_VIDEO_PLANAR)) {
        ERR("D3D11Buffer::CreateUnorderedAccessView: not an ordinary or packed format: ", finalDesc.Format);
        return E_FAIL;
      }
      // R32G32B32 has no typed unordered access (Format support for Direct3D Feature Level 11.0 hardware)
      if (format.Flag & MTL_DXGI_FORMAT_EMULATED_RGB32)
        return E_INVALIDARG;

      UINT support = 0;
      if (FAILED(m_parent->CheckFormatSupport(finalDesc.Format, &support)) || !(support & D3D11_FORMAT_SUPPORT_BUFFER) ||
          !(support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW))
        return E_INVALIDARG;
      view_format = format.BufferPixelFormat;
      offset = finalDesc.Buffer.FirstElement * format.BytesPerTexel;
      size = finalDesc.Buffer.NumElements * format.BytesPerTexel;
      viewElementOffset = finalDesc.Buffer.FirstElement;
      viewElementWidth = finalDesc.Buffer.NumElements;
    }

    if (!ppView) {
      return S_FALSE;
    }

    auto viewId = buffer_->createView({.format = view_format, .write = true});

    auto srv = ref(new UAVWithCounter(
        &finalDesc, this, m_parent,
        {
            .viewKey = viewId,
            .viewElementOffset = viewElementOffset,
            .viewElementWidth = viewElementWidth,
            .byteOffset = offset,
            .byteWidth = size,
        },
        std::move(counter)
    ));
    *ppView = srv;
    return S_OK;
  };

  void
  OnSetDebugObjectName(LPCSTR Name) override {
    if (!Name) {
      return;
    }
#ifdef MULLION_DEBUG
    debug_name = std::string(Name);
#endif
  }
};

// the Metal buffer of this device over a share's bytes
static Rc<Buffer>
WrapSharedBuffer(MTLD3D11Device *pDevice, HANDLE mapping, const D3D11_BUFFER_DESC &desc) {
  const uint64_t length = align(uint64_t(desc.ByteWidth), shared_buffer_granule);
  auto contents = MapSharedBufferContents(mapping, length);
  if (!contents)
    return nullptr;
  auto buffer = Rc<Buffer>(new Buffer(length, pDevice->GetMTLDevice()));
  auto allocation = buffer->allocate({}, 0, std::move(contents));
  if (!allocation->buffer())
    return nullptr;
  buffer->rename(std::move(allocation));
  return buffer;
}

static HRESULT
CreateSharedBuffer(
    MTLD3D11Device *pDevice, const D3D11_BUFFER_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData,
    ID3D11Buffer **ppBuffer
) {
  const uint64_t length = align(uint64_t(pDesc->ByteWidth), shared_buffer_granule);
  const uint64_t size = shared_buffer_granule + length;
  std::random_device rd;
  std::mt19937_64 gen(rd());
  HANDLE mapping = nullptr;
  uint64_t id = 0;
  // an id is 30 random bits: one already in use anywhere in the session is drawn again
  for (int attempt = 0; attempt < 8 && !mapping; attempt++) {
    id = gen() & (shared_buffer_handle_marker - 1);
    char name[64];
    SharedBufferName(id, name);
    mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, DWORD(size >> 32), DWORD(size), name);
    if (mapping && GetLastError() == ERROR_ALREADY_EXISTS) {
      CloseHandle(mapping);
      mapping = nullptr;
    }
  }
  if (!mapping) {
    ERR("D3D11Buffer: Failed to create a shared buffer's mapping: ", GetLastError());
    return E_OUTOFMEMORY;
  }
  auto *data = static_cast<SharedBufferData *>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(SharedBufferData)));
  if (!data) {
    ERR("D3D11Buffer: Failed to write a shared buffer's identity: ", GetLastError());
    CloseHandle(mapping);
    return E_OUTOFMEMORY;
  }
  memcpy(data->tag, shared_buffer_tag, sizeof(shared_buffer_tag));
  data->share_id = id;
  data->desc = *pDesc;
  UnmapViewOfFile(data);

  // the mapping is the buffer's from here on: its contents' view closes it
  auto buffer = WrapSharedBuffer(pDevice, mapping, *pDesc);
  if (!buffer) {
    ERR("D3D11Buffer: Failed to make a shared buffer of ", pDesc->ByteWidth, " bytes");
    return E_OUTOFMEMORY;
  }
  // a new mapping's pages are zero, as a buffer without initial data is
  if (pInitialData && pInitialData->pSysMem)
    buffer->current()->updateContents(0, pInitialData->pSysMem, pDesc->ByteWidth);
  *ppBuffer = reinterpret_cast<ID3D11Buffer *>(ref(new D3D11Buffer(pDesc, std::move(buffer), pDevice, id)));
  return S_OK;
}

HRESULT
CreateBuffer(
    MTLD3D11Device *pDevice, const D3D11_BUFFER_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData,
    ID3D11Buffer **ppBuffer
) {
  // a default buffer alone: a share's pages are written where they are, and a dynamic buffer's are replaced
  if ((pDesc->MiscFlags & (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)) &&
      pDesc->Usage == D3D11_USAGE_DEFAULT && env::metalVR())
    return CreateSharedBuffer(pDevice, pDesc, pInitialData, ppBuffer);
  *ppBuffer = reinterpret_cast<ID3D11Buffer *>(ref(new D3D11Buffer(pDesc, pInitialData, pDevice)));
  return S_OK;
}

HRESULT
ImportSharedBuffer(MTLD3D11Device *pDevice, HANDLE hResource, REFIID riid, void **ppBuffer) {
  if (!env::metalVR() || !(uint64_t(uintptr_t(hResource)) & shared_buffer_handle_marker))
    return DXGI_ERROR_NOT_FOUND;
  const uint64_t id = uint64_t(uintptr_t(hResource)) & (shared_buffer_handle_marker - 1);
  char name[64];
  SharedBufferName(id, name);
  HANDLE mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
  if (!mapping)
    return DXGI_ERROR_NOT_FOUND;
  InitReturnPtr(ppBuffer);
  if (!ppBuffer) {
    CloseHandle(mapping);
    return S_FALSE;
  }
  SharedBufferData data = {};
  void *view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(SharedBufferData));
  if (view) {
    memcpy(&data, view, sizeof(data));
    UnmapViewOfFile(view);
  }
  if (!view || memcmp(data.tag, shared_buffer_tag, sizeof(shared_buffer_tag)) || data.share_id != id ||
      !data.desc.ByteWidth) {
    WARN("ImportSharedBuffer: handle is not a shared buffer");
    CloseHandle(mapping);
    return E_INVALIDARG;
  }
  auto buffer = WrapSharedBuffer(pDevice, mapping, data.desc);
  if (!buffer) {
    ERR("ImportSharedBuffer: Failed to open a shared buffer of ", data.desc.ByteWidth, " bytes");
    return E_OUTOFMEMORY;
  }
  Com<D3D11Buffer> opened = new D3D11Buffer(&data.desc, std::move(buffer), pDevice, id);
  return opened->QueryInterface(riid, ppBuffer);
}

HRESULT
WrapBuffer(
    MTLD3D11Device *pDevice, const D3D11_BUFFER_DESC *pDesc, WMT::Reference<WMT::Buffer> &&Metal,
    const MullionMetalResource &Info, REFIID riid, void **ppBuffer
) {
  InitReturnPtr(ppBuffer);
  if (!pDesc->ByteWidth || pDesc->Usage != D3D11_USAGE_DEFAULT)
    return E_INVALIDARG;
  auto buffer = Rc<Buffer>(new Buffer(pDesc->ByteWidth, pDevice->GetMTLDevice()));
  // a default buffer of CPU access only D3D11On12 makes, over a heap its caller checked the CPU reaches
  auto checked = *pDesc;
  checked.CPUAccessFlags = 0;
  if (FAILED(pDevice->CreateBuffer(&checked, nullptr, nullptr)))
    return E_INVALIDARG;
  auto allocation = buffer->wrap(std::move(Metal), Info.buffer_info, Info.flags);
  if (!allocation)
    return E_INVALIDARG;
  buffer->rename(std::move(allocation));
  Com<D3D11Buffer> wrapped = new D3D11Buffer(pDesc, std::move(buffer), pDevice);
  return wrapped->QueryInterface(riid, ppBuffer);
}

} // namespace mullion
