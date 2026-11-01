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

namespace mullion {

class MTLD3D12HeapImpl : public MTLD3D12Pageable<MTLD3D12Heap> {

  D3D12_HEAP_DESC desc_;

public:
  MTLD3D12HeapImpl(MTLD3D12Device *pDevice) : MTLD3D12Pageable<MTLD3D12Heap>(pDevice) {}

  ~MTLD3D12HeapImpl() {
    if (heap)
      device_->UnregisterResidency(heap);
    heap = {};
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return FAIL(E_POINTER);

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12Heap)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12Resource), riid)) {
      WARN("D3D12Heap: Unknown interface query ", str::format(riid));
    }

    return MTLD3D12Pageable::QueryInterface(riid, ppvObject);
  }

  HRESULT
  Initialize(const D3D12_HEAP_DESC *pDesc) {
    if (pDesc->Flags & D3D12_HEAP_FLAG_ALLOW_DISPLAY)
      return FAIL(E_INVALIDARG); // must be committed resource

    desc_ = *pDesc;
    desc_.Properties.CreationNodeMask = 1;
    desc_.Properties.VisibleNodeMask = 1;


    switch (pDesc->Alignment) {
    case 0:
      desc_.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
      [[fallthrough]];
    case D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT:
    case D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT:
      break;
    default:
      return FAIL(E_INVALIDARG);
    }

    auto size_aligned = align(pDesc->SizeInBytes, desc_.Alignment);
    if (!size_aligned)
      return FAIL(E_INVALIDARG);

    if (pDesc->Flags & D3D12_HEAP_FLAG_ALLOW_WRITE_WATCH) {
      SYSTEM_INFO system;
      GetSystemInfo(&system);
      memory = std::shared_ptr<void>(
          VirtualAlloc(nullptr, align(size_aligned, system.dwAllocationGranularity),
                       MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE),
          [](void *address) { if (address) VirtualFree(address, 0, MEM_RELEASE); }
      );
      return memory ? S_OK : FAIL(E_OUTOFMEMORY);
    }

    WMTHeapInfo info;
    info.options = {}; // FIXME: ensure this agrees with {Buffer|Texture}AllocationFlag?
    // render targets and depth buffers placed in a heap that holds no buffers and that the CPU never reaches are
    // private, as committed ones are: Metal keeps a private texture compressed from the start (MTLD3D12Texture)
    private_storage = (pDesc->Flags & D3D12_HEAP_FLAG_DENY_BUFFERS) && !(pDesc->Flags & D3D12_HEAP_FLAG_SHARED) &&
                      !IsCpuVisibleHeap(&desc_.Properties);
    if (private_storage)
      info.options = WMTResourceStorageModePrivate;
    info.size = size_aligned;
    // the tiles of reserved resources can live in any heap
    info.sparse_page_size = device_->GetSparsePageSize();
    info.type = WMTHeapTypePlacement;

    heap = device_->GetMTLDevice().newHeap(info);
    // Metal makes no heap of a size it cannot give
    if (!heap)
      return FAIL(E_OUTOFMEMORY);

    device_->RegisterResidency(heap);

    return S_OK;
  }

  HRESULT
  Import(const void *Address, HANDLE Mapping) {
    void *address = Mapping ? MapViewOfFile(Mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0) : const_cast<void *>(Address);
    memory = std::shared_ptr<void>(address, [Mapping](void *pointer) { if (Mapping && pointer) UnmapViewOfFile(pointer); });
    MEMORY_BASIC_INFORMATION region, next;
    if (!address || !VirtualQuery(address, &region, sizeof(region)) || region.AllocationBase != address ||
        region.BaseAddress != address || region.State != MEM_COMMIT || region.Protect & (PAGE_NOACCESS | PAGE_GUARD))
      return FAIL(E_INVALIDARG);
    if (VirtualQuery((char *)address + region.RegionSize, &next, sizeof(next)) && next.AllocationBase == address)
      return FAIL(E_INVALIDARG);
    desc_ = {region.RegionSize,
             {D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK, D3D12_MEMORY_POOL_L0, 1, 1},
             D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,
             D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS | D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER};
    return S_OK;
  }

  virtual D3D12_HEAP_DESC *STDMETHODCALLTYPE
  GetDesc(D3D12_HEAP_DESC *__ret) {
    *__ret = desc_;
    return __ret;
  };
};

HRESULT
OpenHeap(MTLD3D12Device *pDevice, const void *Address, HANDLE Mapping, REFIID riid, void **ppHeap) {
  InitReturnPtr(ppHeap);
  auto heap = Com(new MTLD3D12HeapImpl(pDevice));
  HRESULT hr = heap->Import(Address, Mapping);
  if (FAILED(hr))
    return hr;
  return ppHeap ? heap->QueryInterface(riid, ppHeap) : S_FALSE;
}

HRESULT
CreateHeap(MTLD3D12Device *pDevice, const D3D12_HEAP_DESC *pDesc, REFIID riid, void **ppHeap) {
  InitReturnPtr(ppHeap);
  auto heap = Com(new MTLD3D12HeapImpl(pDevice));
  HRESULT hr = heap->Initialize(pDesc);
  if (FAILED(hr))
    return hr;
  if (!ppHeap)
    return S_FALSE;
  return heap->QueryInterface(riid, ppHeap);
}

}; // namespace mullion