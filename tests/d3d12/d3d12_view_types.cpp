// contract: a shader resource view reads the same texels whatever texture type the shader declares it with, as long as
// the types agree but for being an array (D3D11.3 22.3.12): a Texture2D view read as a Texture2DArray reads its one
// layer, and a one-layer Texture2DArray view read as a Texture2D reads that layer, each at every mip the view holds. a
// texture of MIPS mips, each mip holding 10 plus its number in every texel, is read through both views at each mip;
// each read must return its own mip's value.
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
Texture2DArray<uint> plain_as_array : register(t0);
Texture2D<uint> array_as_plain : register(t1);
RWStructuredBuffer<uint> results : register(u0);
[numthreads(1, 1, 1)]
void cs() {
  for (uint m = 0; m < MIPS; m++) {
    results[m] = plain_as_array.Load(int4(0, 0, 0, m));
    results[MIPS + m] = array_as_plain.Load(int3(0, 0, m));
  }
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const UINT mips = 4, side = 1 << (mips - 1), base = 10;
  auto cs = compiler.compile(hlsl, "cs", "cs", {"MIPS=" + std::to_string(mips)});
  if (cs.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0};
  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[0].DescriptorTable = {1, &range};
  auto rs = root_signature(device.Get(), {2, params});
  D3D12_COMPUTE_PIPELINE_STATE_DESC cs_desc{rs.Get(), bytecode(cs)};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateComputePipelineState(&cs_desc, IID_PPV_ARGS(&pso)));

  ComPtr<ID3D12Resource> texture;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC texture_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, side, side, 1, (UINT16)mips, DXGI_FORMAT_R32_UINT,
                                   {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &texture_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)
  ));
  // every texel of mip m holds base + m
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[mips];
  UINT rows[mips];
  UINT64 upload_bytes;
  device->GetCopyableFootprints(&texture_desc, 0, mips, 0, footprints, rows, nullptr, &upload_bytes);
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, upload_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  char *mapped;
  CHECK(upload->Map(0, nullptr, (void **)&mapped));
  for (UINT m = 0; m < mips; m++)
    for (UINT y = 0; y < rows[m]; y++)
      for (UINT x = 0; x < footprints[m].Footprint.Width; x++)
        reinterpret_cast<UINT *>(mapped + footprints[m].Offset + y * footprints[m].Footprint.RowPitch)[x] = base + m;
  upload->Unmap(0, nullptr);

  // the texture as Texture2D (read as an array) and as a one-layer Texture2DArray (read as a Texture2D)
  ComPtr<ID3D12DescriptorHeap> srvs;
  D3D12_DESCRIPTOR_HEAP_DESC srv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&srvs)));
  auto handle = srvs->GetCPUDescriptorHandleForHeapStart();
  D3D12_SHADER_RESOURCE_VIEW_DESC plain{DXGI_FORMAT_R32_UINT, D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
  plain.Texture2D = {0, mips};
  device->CreateShaderResourceView(texture.Get(), &plain, handle);
  handle.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_SHADER_RESOURCE_VIEW_DESC array{DXGI_FORMAT_R32_UINT, D3D12_SRV_DIMENSION_TEXTURE2DARRAY, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
  array.Texture2DArray = {0, mips, 0, 1};
  device->CreateShaderResourceView(texture.Get(), &array, handle);

  const UINT64 results_bytes = 2 * mips * sizeof(UINT);
  auto results = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, results_bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, results_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  CHECK(forget(readback.Get()));

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  for (UINT m = 0; m < mips; m++) {
    D3D12_TEXTURE_COPY_LOCATION dst{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = m}},
        src{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprints[m]}};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  list->SetDescriptorHeaps(1, srvs.GetAddressOf());
  list->SetComputeRootSignature(rs.Get());
  list->SetComputeRootDescriptorTable(0, srvs->GetGPUDescriptorHandleForHeapStart());
  list->SetComputeRootUnorderedAccessView(1, results->GetGPUVirtualAddress());
  list->Dispatch(1, 1, 1);
  transition(list.Get(), results.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(), 0, results.Get(), 0, results_bytes);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  UINT *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  const char *views[2] = {"Texture2D view read as Texture2DArray", "one-layer Texture2DArray view read as Texture2D"};
  unsigned failures = 0;
  for (UINT v = 0; v < 2; v++)
    for (UINT m = 0; m < mips; m++)
      if (got[v * mips + m] != base + m && failures++ < 12)
        printf("%s, mip %u: %u, want %u\n", views[v], m, got[v * mips + m], base + m);
  readback->Unmap(0, nullptr);
  printf("%s: %u wrong results of %u reads\n", failures ? "failed" : "passed", failures, 2 * mips);
  return failures != 0;
}
