// contract: ID3D12Device::CreateHeap returns "E_OUTOFMEMORY: There is insufficient memory to create the heap"
// (Return value), and an interface pointer is set to NULL when a call that returns one fails. allocators such as
// D3D12MA make a smaller heap, or fall back to committed resources, when this call fails, so a heap that cannot be
// made must not be reported as made.
// a default heap of 16 TB, which no Mac's memory holds, is asked for: the call must return E_OUTOFMEMORY and leave
// the pointer NULL. a heap of 64 KB is then made and a buffer placed on it, which must still work.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createheap
#include "d3d12_test.hpp"

int
main() {
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));

  step("a default heap of 16 TB");
  D3D12_HEAP_DESC desc{1ull << 44, {D3D12_HEAP_TYPE_DEFAULT}, 0, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS};
  ID3D12Heap *heap = (ID3D12Heap *)(UINT_PTR)0xdeadbeef;
  HRESULT hr = device->CreateHeap(&desc, IID_PPV_ARGS(&heap));
  expect(hr == E_OUTOFMEMORY, "CreateHeap returned %08lx, want E_OUTOFMEMORY (%08lx)", hr, E_OUTOFMEMORY);
  expect(!heap || SUCCEEDED(hr), "the heap pointer is %p after a failure, want NULL", heap);
  if (SUCCEEDED(hr) && heap)
    heap->Release();

  step("a heap of 64 KB, and a buffer placed on it");
  desc.SizeInBytes = 1 << 16;
  ComPtr<ID3D12Heap> small;
  hr = device->CreateHeap(&desc, IID_PPV_ARGS(&small));
  if (expect(hr == S_OK, "CreateHeap returned %08lx, want S_OK", hr)) {
    D3D12_RESOURCE_DESC buffer_desc{D3D12_RESOURCE_DIMENSION_BUFFER, 0, 1 << 16, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                                    D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
    ComPtr<ID3D12Resource> placed;
    hr = device->CreatePlacedResource(small.Get(), 0, &buffer_desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                      IID_PPV_ARGS(&placed));
    expect(hr == S_OK, "CreatePlacedResource returned %08lx, want S_OK", hr);
  }
  return verdict();
}
