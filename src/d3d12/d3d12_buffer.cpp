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

#include "d3d12_device.hpp"
#include "d3d12_pageable.hpp"
#include "com/com_pointer.hpp"
#include "mullion_format.hpp"

namespace mullion {

class MTLD3D12Buffer : public MTLD3D12Pageable<MTLD3D12Resource> {
  D3D12_RESOURCE_DESC desc_;
  D3D12_HEAP_PROPERTIES heap_props_;
  D3D12_HEAP_FLAGS heap_flags_;
  bool reserved_ = false;
  // the heap a placed buffer is on, which it keeps: the heap's residency is the buffer's
  // ("A placed resource object holds a reference on the heap it is created on", ID3D12Device::CreateHeap)
  Com<MTLD3D12Heap, false> heap_;

public:
  MTLD3D12Buffer(MTLD3D12Device *pDevice) : MTLD3D12Pageable<MTLD3D12Resource>(pDevice) {}

  HRESULT
  Initialize(
      const D3D12_HEAP_PROPERTIES *pHeapProps, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC *pDesc,
      const D3D12_CLEAR_VALUE *OptimizedClearValue, MTLD3D12Heap *pHeap, UINT64 Offset, bool Reserved = false
  ) {
    if (OptimizedClearValue)
      return FAIL(E_INVALIDARG);

    // TODO: validate and normalize
    desc_ = *pDesc;
    reserved_ = Reserved;
    heap_props_ = *pHeapProps;
    heap_flags_ = HeapFlags;
    heap_ = pHeap;

    if (desc_.Alignment) {
      if (desc_.Alignment != D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT)
        return FAIL(E_INVALIDARG);
    } else {
      desc_.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    }

    if (pHeap && !pHeap->memory) {
      auto size_and_align = device_->GetMTLDevice().heapBufferSizeAndAlign(desc_.Width, {});
      // the Metal heap is the description's size rounded up to its alignment (MTLD3D12Heap)
      auto heap_desc = pHeap->GetDesc();
      if (size_and_align.size + Offset > align(heap_desc.SizeInBytes, heap_desc.Alignment))
        return FAIL(E_INVALIDARG);
    }

    std::shared_ptr<void> memory;
    uint64_t length = desc_.Width;
    if (pHeap && pHeap->memory) {
      auto size = pHeap->GetDesc().SizeInBytes;
      if (Offset % desc_.Alignment || Offset > size || length > size - Offset)
        return FAIL(E_INVALIDARG);
      // the memory the heap's owner made ends where the heap does: the rest of a 64KB granule past it is not ours to
      // wrap
      length = std::min<uint64_t>(align(length, desc_.Alignment), size - Offset);
      memory = std::shared_ptr<void>(pHeap->memory, (char *)pHeap->memory.get() + Offset);
    } else if (HeapFlags & D3D12_HEAP_FLAG_ALLOW_WRITE_WATCH) {
      SYSTEM_INFO system;
      GetSystemInfo(&system);
      length = align(length, system.dwAllocationGranularity);
      memory = std::shared_ptr<void>(
          VirtualAlloc(nullptr, length, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE),
          [](void *address) { if (address) VirtualFree(address, 0, MEM_RELEASE); }
      );
      if (!memory)
        return FAIL(E_OUTOFMEMORY);
    }
    buffer = new Buffer(length, device_->GetMTLDevice());

    Flags<BufferAllocationFlag> flags;
    if (Reserved)
      buffer->rename(buffer->allocate(flags, device_->GetSparsePageSize()));
    else if (memory)
      buffer->rename(buffer->allocate(flags, 0, std::move(memory)));
    else if (pHeap)
      buffer->rename(buffer->allocate(flags, pHeap->heap, Offset));
    else
      buffer->rename(buffer->allocate(flags));
    if (!buffer->current()->buffer())
      return FAIL(E_OUTOFMEMORY);
    device_->RegisterResidencyAndVA(buffer->current());

    return S_OK;
  };

  ~MTLD3D12Buffer() {
    if (buffer && buffer->current()->buffer())
      device_->UnregisterResidencyAndVA(buffer->current());
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return FAIL(E_POINTER);

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12Resource) || riid == __uuidof(ID3D12Resource1) ||
        riid == __uuidof(ID3D12Resource2)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12Resource), riid)) {
      WARN("D3D12Buffer: Unknown interface query ", str::format(riid));
    }

    return MTLD3D12Pageable::QueryInterface(riid, ppvObject);
  }

  virtual HRESULT STDMETHODCALLTYPE
  Map(UINT Subresource, const D3D12_RANGE *pReadRange, void **ppData) {
    if (Subresource)
      return FAIL(E_INVALIDARG);
    if (!IsCpuVisibleHeap(&heap_props_))
      return FAIL(E_INVALIDARG);
    if (ppData)
      *ppData = buffer->current()->mappedMemory(0);
    return S_OK;
  };

  virtual void STDMETHODCALLTYPE Unmap(UINT Subresource, const D3D12_RANGE *pWrittenRange) {
    // no-op
  };

  virtual D3D12_RESOURCE_DESC *STDMETHODCALLTYPE
  GetDesc(D3D12_RESOURCE_DESC *__ret) {
    *__ret = desc_;
    return __ret;
  };

  virtual D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE
  GetGPUVirtualAddress() {
    return buffer->current()->gpuAddress();
  };

  virtual HRESULT STDMETHODCALLTYPE
  WriteToSubresource(
      UINT DstSubresource, const D3D12_BOX *pDstBox, const void *pSrcData, UINT SrcRowPitch, UINT SrcSlicePitch
  ) {
    return FAIL(E_INVALIDARG);
  };

  virtual HRESULT STDMETHODCALLTYPE
  ReadFromSubresource(
      void *pDstData, UINT DstRowPitch, UINT DstSlicePitch, UINT SrcSubresource, const D3D12_BOX *pSrcBox
  ) {
    return FAIL(E_INVALIDARG);
  };

  virtual HRESULT STDMETHODCALLTYPE
  GetHeapProperties(D3D12_HEAP_PROPERTIES *pHeapProps, D3D12_HEAP_FLAGS *pFlags) {
    if (reserved_)
      return FAIL(E_INVALIDARG);
    if (pHeapProps)
      *pHeapProps = heap_props_;
    if (pFlags)
      *pFlags = heap_flags_;
    return S_OK;
  };

  virtual HRESULT STDMETHODCALLTYPE
  CreateShaderResourceView(const D3D12_SHADER_RESOURCE_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor) {
    HRESULT hr;
    D3D12_SHADER_RESOURCE_VIEW_DESC ViewDesc;
    if (!pDesc) {
      hr = ExtractEntireResourceViewDescription(desc_, &ViewDesc);
      if (FAILED(hr))
        return hr;
    } else {
      ViewDesc = *pDesc;
    }

    if (ViewDesc.ViewDimension != D3D12_SRV_DIMENSION_BUFFER)
      return FAIL(E_INVALIDARG);

    auto [Heap, Index] = GetShaderVisibleDescriptorHeap(device_, Descriptor);
    BufferSlice Slice;

    if (ViewDesc.Format == DXGI_FORMAT_UNKNOWN || ViewDesc.Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) {
      UINT Stride = (ViewDesc.Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) ? 4 : ViewDesc.Buffer.StructureByteStride;
      Slice.firstElement = ViewDesc.Buffer.FirstElement;
      Slice.elementCount = ViewDesc.Buffer.NumElements;
      Slice.byteOffset = Slice.firstElement * Stride;
      Slice.byteLength = Slice.elementCount * Stride;
      return Heap->AddShaderResourceView(Index, buffer.ptr(), Slice);
    }

    MTL_DXGI_FORMAT_DESC Format;
    if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), ViewDesc.Format, Format)) ||
        (Format.Flag & MTL_DXGI_FORMAT_VIDEO_PLANAR)) {
      ERR("D3D12Buffer::CreateShaderResourceView: not an ordinary or packed format: ", ViewDesc.Format);
      return E_FAIL;
    }
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{ViewDesc.Format};
    if (FAILED(device_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
        !(support.Support1 & D3D12_FORMAT_SUPPORT1_BUFFER))
      return FAIL(E_INVALIDARG);
    BufferViewDescriptor view_descriptor{Format.BufferPixelFormat};
    Slice.firstElement = ViewDesc.Buffer.FirstElement * (Format.BytesPerTexel / MTLGetTexelSize(Format.BufferPixelFormat));
    Slice.elementCount = ViewDesc.Buffer.NumElements |
                         (Format.Flag & MTL_DXGI_FORMAT_EMULATED_RGB32 ? SM50_TEXEL_BUFFER_METADATA_RGB32 : 0);
    Slice.byteOffset = Format.BytesPerTexel * ViewDesc.Buffer.FirstElement;
    Slice.byteLength = Format.BytesPerTexel * ViewDesc.Buffer.NumElements;

    auto view = buffer->createView(view_descriptor);
    return Heap->AddShaderResourceView(Index, buffer.ptr(), view, Slice);
  };

  virtual HRESULT STDMETHODCALLTYPE
  CreateUnorderedAccessView(
      ID3D12Resource *pCounter, const D3D12_UNORDERED_ACCESS_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor
  ) {
    HRESULT hr;
    D3D12_UNORDERED_ACCESS_VIEW_DESC ViewDesc;
    if (!pDesc) {
      hr = ExtractEntireResourceViewDescription(desc_, &ViewDesc);
      if (FAILED(hr))
        return hr;
    } else {
      ViewDesc = *pDesc;
    }

    if (ViewDesc.ViewDimension != D3D12_UAV_DIMENSION_BUFFER)
      return FAIL(E_INVALIDARG);

    auto [Heap, Index] = GetShaderVisibleDescriptorHeap(device_, Descriptor);
    BufferSlice Slice;

    if (ViewDesc.Format == DXGI_FORMAT_UNKNOWN || ViewDesc.Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW) {
      UINT Stride = (ViewDesc.Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW) ? 4 : ViewDesc.Buffer.StructureByteStride;
      Slice.firstElement = ViewDesc.Buffer.FirstElement;
      Slice.elementCount = ViewDesc.Buffer.NumElements;
      Slice.byteOffset = Slice.firstElement * Stride;
      Slice.byteLength = Slice.elementCount * Stride;
      if (!pCounter)
        return Heap->AddUnorderedAccessView(Index, buffer.ptr(), Slice, nullptr, 0);

      auto Counter = static_cast<MTLD3D12Buffer *>(pCounter);
      return Heap->AddUnorderedAccessView(
          Index, buffer.ptr(), Slice, Counter->buffer.ptr(), ViewDesc.Buffer.CounterOffsetInBytes
      );
    }

    MTL_DXGI_FORMAT_DESC Format;
    if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), ViewDesc.Format, Format)) ||
        (Format.Flag & MTL_DXGI_FORMAT_VIDEO_PLANAR)) {
      ERR("D3D12Buffer::CreateUnorderedAccessView: not an ordinary or packed format: ", ViewDesc.Format);
      return E_FAIL;
    }
    // R32G32B32 has no typed unordered access (Format support for Direct3D Feature Level 11.0 hardware)
    if (Format.Flag & MTL_DXGI_FORMAT_EMULATED_RGB32)
      return FAIL(E_INVALIDARG);
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{ViewDesc.Format};
    if (FAILED(device_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
        !(support.Support1 & D3D12_FORMAT_SUPPORT1_BUFFER) ||
        !(support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW))
      return FAIL(E_INVALIDARG);
    BufferViewDescriptor view_descriptor{Format.BufferPixelFormat, true};
    Slice.firstElement = ViewDesc.Buffer.FirstElement;
    Slice.elementCount = ViewDesc.Buffer.NumElements;
    Slice.byteOffset = Format.BytesPerTexel * ViewDesc.Buffer.FirstElement;
    Slice.byteLength = Format.BytesPerTexel * ViewDesc.Buffer.NumElements;

    auto view = buffer->createView(view_descriptor);
    return Heap->AddUnorderedAccessView(Index, buffer.ptr(), view, Slice);
  };

  virtual HRESULT STDMETHODCALLTYPE
  CreateRenderTargetView(const D3D12_RENDER_TARGET_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor) {
    // Metal renders to textures
    ERR("CreateRenderTargetView: a buffer cannot be a render target");
    return E_INVALIDARG;
  };

  virtual HRESULT STDMETHODCALLTYPE
  CreateDepthStencilView(const D3D12_DEPTH_STENCIL_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor) {
    // Direct3D 12 has no depth stencil view of a buffer
    return FAIL(E_INVALIDARG);
  };

  // a buffer's tiles are 64 KB runs of its bytes, all in its one subresource
  virtual void STDMETHODCALLTYPE GetResourceTiling(
      UINT *TotalTileCount, D3D12_PACKED_MIP_INFO *PackedMipInfo, D3D12_TILE_SHAPE *StandardTileShape,
      UINT *SubresourceTilingCount, UINT FirstSubresourceTiling, D3D12_SUBRESOURCE_TILING *SubresourceTilings
  ) {
    UINT tile = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
    UINT tiles = UINT((desc_.Width + tile - 1) / tile);
    if (TotalTileCount)
      *TotalTileCount = tiles;
    if (PackedMipInfo)
      *PackedMipInfo = {};
    if (StandardTileShape)
      *StandardTileShape = {tile, 1, 1};
    if (!SubresourceTilingCount)
      return;
    *SubresourceTilingCount = *SubresourceTilingCount && !FirstSubresourceTiling;
    if (*SubresourceTilingCount)
      SubresourceTilings[0] = {tiles, 1, 1, 0};
  };
};

HRESULT
CreateCommittedBuffer(
    MTLD3D12Device *pDevice, const D3D12_HEAP_PROPERTIES *pHeapProps, D3D12_HEAP_FLAGS HeapFlags,
    const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *OptimizedClearValue,
    REFIID riid, void **ppResource
) {
  auto buffer = Com(new MTLD3D12Buffer(pDevice));
  HRESULT hr = buffer->Initialize(pHeapProps, HeapFlags, pDesc, OptimizedClearValue, nullptr, 0);
  if (FAILED(hr))
    return hr;
  if (!ppResource)
    return S_FALSE;
  return buffer->QueryInterface(riid, ppResource);
}

HRESULT
CreateReservedBuffer(
    MTLD3D12Device *pDevice, const D3D12_RESOURCE_DESC *pDesc, REFIID riid, void **ppResource
) {
  auto buffer = Com(new MTLD3D12Buffer(pDevice));
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  HRESULT hr = buffer->Initialize(&heap, D3D12_HEAP_FLAG_NONE, pDesc, nullptr, nullptr, 0, true);
  if (FAILED(hr))
    return hr;
  if (!ppResource)
    return S_FALSE;
  return buffer->QueryInterface(riid, ppResource);
}

HRESULT
CreatePlacedBuffer(
    MTLD3D12Device *pDevice, MTLD3D12Heap *pHeap, UINT64 Offset, const D3D12_RESOURCE_DESC *pDesc,
    D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *OptimizedClearValue, REFIID riid, void **ppResource
) {
  auto buffer = Com(new MTLD3D12Buffer(pDevice));
  D3D12_HEAP_DESC heap_desc = pHeap->GetDesc();

  if (heap_desc.Flags & D3D12_HEAP_FLAG_DENY_BUFFERS)
    return FAIL(E_INVALIDARG);

  HRESULT hr = buffer->Initialize(&heap_desc.Properties, heap_desc.Flags, pDesc, OptimizedClearValue, pHeap, Offset);
  if (FAILED(hr))
    return hr;
  if (!ppResource)
    return S_FALSE;
  return buffer->QueryInterface(riid, ppResource);
}

} // namespace mullion