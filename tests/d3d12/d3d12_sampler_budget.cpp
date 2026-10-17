// contract: a shader visible sampler heap takes 600 samplers that differ only in MaxLOD, all under the last mip of
// the largest texture, so none of them can share a Metal sampler. each makes two (the plain one and the cube one),
// 1200 in all, more than the 1024 distinct samplers an M1 holds: the log has to say so.
#include "d3d12_test.hpp"

int
main(int argc, char **argv) {
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  const UINT count = 600;
  ComPtr<ID3D12DescriptorHeap> heap;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, count, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)));
  const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  for (UINT i = 0; i < count; i++) {
    D3D12_SAMPLER_DESC desc{D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                            D3D12_TEXTURE_ADDRESS_MODE_WRAP};
    desc.MaxLOD = i / 64.0f;
    device->CreateSampler(&desc, {heap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(i) * increment});
  }
  printf("passed: %u samplers made\n", count);
  return 0;
}
