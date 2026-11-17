// contract: a descriptor heap's memory is that of the descriptors written in it. a heap of as many descriptors as a
// shader-visible heap may have on every tier (D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1), a handle
// increment each (GetDescriptorHandleIncrementSize), adds to what the process has resident (WorkingSetSize) less than
// an eighth of their increments while none is written, shader-visible or not; once every descriptor of the
// non-visible one is written, as a constant buffer view, and copied into the visible one, the process has grown by at
// least that eighth, so the measure sees descriptors' memory
#include "d3d12_test.hpp"
#include <psapi.h>

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler))
    return 77;
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  auto resident = [] {
    PROCESS_MEMORY_COUNTERS counters{sizeof(counters)};
    GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters));
    return (INT64)counters.WorkingSetSize;
  };
  const UINT count = D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1;
  const INT64 eighth = INT64(count) * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) / 8;
  auto cbuffer = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,
                        D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!cbuffer) {
    printf("failed: constant buffer\n");
    return 1;
  }
  ComPtr<ID3D12DescriptorHeap> heaps[2];
  for (UINT visible = 0; visible < 2; visible++) {
    step(visible ? "a shader-visible heap" : "a heap");
    D3D12_DESCRIPTOR_HEAP_DESC desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, count,
                                    visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE};
    INT64 before = resident();
    CHECK(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heaps[visible])));
    INT64 grown = resident() - before;
    expect(grown < eighth, "%u unwritten descriptors take %lld bytes, an eighth of their increments is %lld", count,
           grown, eighth);
  }
  step("every descriptor written and copied");
  INT64 before = resident();
  D3D12_CONSTANT_BUFFER_VIEW_DESC view{cbuffer->GetGPUVirtualAddress(), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT};
  auto increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  auto at = heaps[0]->GetCPUDescriptorHandleForHeapStart();
  for (UINT i = 0; i < count; i++, at.ptr += increment)
    device->CreateConstantBufferView(&view, at);
  device->CopyDescriptorsSimple(count, heaps[1]->GetCPUDescriptorHandleForHeapStart(),
                                heaps[0]->GetCPUDescriptorHandleForHeapStart(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  INT64 grown = resident() - before;
  expect(grown >= eighth, "%u written descriptors take %lld bytes, less than an eighth of their increments (%lld)",
         count, grown, eighth);
  return verdict();
}
