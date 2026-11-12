// contract: a load from a shader resource view whose ResourceMinLODClamp is past the view's last mip reads zeros,
// and a view whose clamp is at or before that mip reads its mips ("A value to clamp sample LOD values to",
// D3D12_TEX2D_SRV; the clamp is in the resource's mip numbers). Windows counts the clamp in 256ths, rounded to the
// nearest: vkd3d-proton's test_view_min_lod has each of these loads from Windows. it also has zeros for a sample
// through such a view, which is not held here: a sample reads the view's last mip.
// a texture of four mips, mip m of the byte (m + 1) * 0x11 in every channel; a compute shader loads and samples
// the view at a lod of the view's own numbering. zeros are what no mip has.
#include "d3d12_test.hpp"
#include <cmath>

static const char hlsl[] = R"hlsl(
Texture2D<float4> t : register(t0);
SamplerState s : register(s0);
cbuffer C : register(b0) { uint lod; };
RWStructuredBuffer<float4> o : register(u0);
[numthreads(1, 1, 1)] void cs() {
  o[0] = t.Load(int3(0, 0, lod));
  o[1] = t.SampleLevel(s, float2(0.5, 0.5), lod);
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto cs = compiler.compile(hlsl, "cs", "cs");
  if (!expect(!cs.empty(), "HLSL did not compile"))
    return verdict();
  constexpr UINT kSide = 8, kMips = 4;
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0};
  D3D12_ROOT_PARAMETER params[3] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable = {1, &range};
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants = {0, 0, 1};
  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  D3D12_STATIC_SAMPLER_DESC sampler{D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                    D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP};
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  auto rs = root_signature(device.Get(), {3, params, 1, &sampler});
  CHECK(rs ? S_OK : E_FAIL);
  ComPtr<ID3D12PipelineState> pso;
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{rs.Get(), bytecode(cs)};
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)));

  D3D12_RESOURCE_DESC texture_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, kSide, kSide, 1, kMips,
                                   DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}};
  D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
  ComPtr<ID3D12Resource> texture;
  CHECK(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &texture_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                        nullptr, IID_PPV_ARGS(&texture)));
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[kMips];
  UINT rows[kMips];
  UINT64 bytes;
  device->GetCopyableFootprints(&texture_desc, 0, kMips, 0, footprints, rows, nullptr, &bytes);
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto output = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, 2 * 4 * sizeof(float), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 2 * 4 * sizeof(float), D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(upload && output && readback, "buffers were not made"))
    return verdict();
  uint8_t *mapped;
  CHECK(upload->Map(0, nullptr, (void **)&mapped));
  for (UINT m = 0; m < kMips; m++)
    for (UINT y = 0; y < rows[m]; y++)
      memset(mapped + footprints[m].Offset + y * footprints[m].Footprint.RowPitch, (m + 1) * 0x11,
             footprints[m].Footprint.Width * 4);
  upload->Unmap(0, nullptr);
  ComPtr<ID3D12DescriptorHeap> heap;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)));

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  for (UINT m = 0; m < kMips; m++) {
    D3D12_TEXTURE_COPY_LOCATION to{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = m}};
    D3D12_TEXTURE_COPY_LOCATION from{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprints[m]}};
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  }
  transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  // the view's first mip and count (0: all the rest), its clamp, the lod asked for in the view's numbering, and
  // the resource's mip a load and a sample give: -1 for zeros, -2 where the answer is not held
  struct Case {
    UINT first, count;
    float clamp;
    UINT lod;
    int load, sample;
  };
  const Case cases[] = {
      {0, 1, 0.0f, 0, 0, 0},
      {0, 1, 0.1f, 0, -1, -2},
      {0, 1, 1.0f / 256, 0, -1, -2},
      {0, 1, 1.0f / 512, 0, -1, -2},
      {0, 1, 1.5f / 1024, 0, 0, 0},
      {0, 1, 1.0f / 1024, 0, 0, 0},
      {0, 2, 1.0f, 1, 1, 1},
      {0, 2, 1.1f, 1, -1, -2},
      {0, 2, 1.0f + 1.0f / 256, 1, -1, -2},
      {0, 2, 1.0f + 1.0f / 512, 1, -1, -2},
      {0, 2, 1.0f + 1.0f / 1024, 1, 1, 1},
      {1, 0, 1.0f, 0, 1, 1},
      {1, 0, 3.0f, 2, 3, 3},
      {1, 0, 9.0f, 0, -1, -2},
      {1, 0, 9.0f, 2, -1, -2},
  };
  for (auto &c : cases) {
    step("mips from %u, %u of them, clamp %.6f, lod %u", c.first, c.count, c.clamp, c.lod);
    D3D12_SHADER_RESOURCE_VIEW_DESC view{DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_SRV_DIMENSION_TEXTURE2D,
                                         D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
    view.Texture2D = {c.first, c.count ? c.count : ~0u, 0, c.clamp};
    device->CreateShaderResourceView(texture.Get(), &view, heap->GetCPUDescriptorHandleForHeapStart());
    CHECK(forget(readback.Get()));
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), nullptr));
    ID3D12DescriptorHeap *heaps[] = {heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetPipelineState(pso.Get());
    list->SetComputeRootSignature(rs.Get());
    list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRoot32BitConstant(1, c.lod, 0);
    list->SetComputeRootUnorderedAccessView(2, output->GetGPUVirtualAddress());
    list->Dispatch(1, 1, 1);
    transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(readback.Get(), output.Get());
    transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    const float *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    const std::pair<const char *, int> reads[] = {{"Load", c.load}, {"SampleLevel", c.sample}};
    for (UINT r = 0; r < 2; r++) {
      if (reads[r].second == -2)
        continue;
      float want = reads[r].second < 0 ? 0.0f : (reads[r].second + 1) * 0x11 / 255.0f;
      bool same = true;
      for (UINT channel = 0; channel < 4; channel++)
        same &= std::fabs(got[r * 4 + channel] - want) < 0.5f / 255;
      expect(same, "%s gives %g %g %g %g, want %g (%s)", reads[r].first, got[r * 4], got[r * 4 + 1], got[r * 4 + 2],
             got[r * 4 + 3], want, reads[r].second < 0 ? "zeros" : "the mip");
    }
    readback->Unmap(0, nullptr);
  }
  return verdict();
}
