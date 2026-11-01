// contract: a ray tracing pipeline needs no global root signature: "Different shaders can use different global root
// signatures (or none)" (DXR spec, D3D12_GLOBAL_ROOT_SIGNATURE). a ray generation shader whose resources come from its
// local root signature alone (a root SRV of the acceleration structure, a root UAV) traces an inline ray query, and a
// null acceleration structure, at address 0, misses every ray (DXR spec, "Additional SRV type"). each of 64 rays
// counts its candidates (none) and writes 100 + 10 * candidates + its committed status (COMMITTED_NOTHING, 0), once
// from DispatchRays and once, through a second record writing past the first 64, from ExecuteIndirect. every ray
// also adds 1 to a 64-bit texel of a table's UAV (SM 6.6 64-bit typed atomics), which ends at 128. Mullion keeps the
// null structure's header and the 64-bit atomics' locks in the device's lock table, which only the global root
// arguments carried.
// https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html#d3d12_global_root_signature
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
RaytracingAccelerationStructure scene : register(t0);
RWStructuredBuffer<uint> results : register(u0);
RWTexture2D<uint64_t> wide : register(u1);
[shader("raygeneration")] void generate() {
  InterlockedAdd(wide[uint2(0, 0)], (uint64_t)1);
  RayQuery<RAY_FLAG_NONE> q;
  RayDesc ray = { float3(DispatchRaysIndex().x, 0, 0), 0, float3(0, 0, 1), 100 };
  q.TraceRayInline(scene, RAY_FLAG_NONE, 0xff, ray);
  uint candidates = 0;
  while (q.Proceed()) candidates++;
  results[DispatchRaysIndex().x] = 100 + candidates * 10 + q.CommittedStatus();
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler) || !compiler.dxc) {
    printf("skipped: ray tracing libraries need DXC\n");
    return 77;
  }
  auto library = compiler.compile(hlsl, "", "lib_6_5");
  if (!expect(!library.empty(), "the library does not compile"))
    return verdict();
  ComPtr<ID3D12Device5> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options)));
  if (options.RaytracingTier < D3D12_RAYTRACING_TIER_1_1) {
    printf("skipped: ray tracing tier 1.1 is not supported\n");
    return 77;
  }

  D3D12_FEATURE_DATA_D3D12_OPTIONS9 options9{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9, &options9, sizeof(options9)));
  if (!options9.AtomicInt64OnTypedResourceSupported) {
    printf("skipped: 64-bit atomics on typed resources are not supported\n");
    return 77;
  }
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1};
  D3D12_ROOT_PARAMETER parameters[3]{{D3D12_ROOT_PARAMETER_TYPE_SRV}, {D3D12_ROOT_PARAMETER_TYPE_UAV},
                                     {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}};
  parameters[2].DescriptorTable = {1, &range};
  auto local = root_signature(device.Get(), {3, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE});
  if (!expect(!!local, "the local root signature is refused"))
    return verdict();
  D3D12_DXIL_LIBRARY_DESC code{bytecode(library)};
  D3D12_LOCAL_ROOT_SIGNATURE local_signature{local.Get()};
  const WCHAR *generate = L"generate";
  D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION association{nullptr, 1, &generate};
  D3D12_RAYTRACING_SHADER_CONFIG shader{4, 8};
  D3D12_RAYTRACING_PIPELINE_CONFIG pipeline{1};
  D3D12_STATE_SUBOBJECT subobjects[] = {
      {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &code},
      {D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &local_signature},
      {D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &association},
      {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader},
      {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipeline},
  };
  association.pSubobjectToAssociate = &subobjects[1];
  D3D12_STATE_OBJECT_DESC desc{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, (UINT)std::size(subobjects), subobjects};
  ComPtr<ID3D12StateObject> state;
  HRESULT hr = device->CreateStateObject(&desc, IID_PPV_ARGS(&state));
  if (!expect(SUCCEEDED(hr), "a pipeline with no global root signature is refused: %08lx", hr))
    return verdict();
  ComPtr<ID3D12StateObjectProperties> properties;
  CHECK(state.As(&properties));
  auto identifier = properties->GetShaderIdentifier(generate);
  if (!expect(!!identifier, "the ray generation shader has no identifier"))
    return verdict();

  constexpr UINT rays = 64;
  auto results = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, 2 * rays * 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  // the results, then the 64-bit texel's row
  constexpr UINT texel_at = 2 * rays * 4;
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, texel_at + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT,
                         D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12Resource> texture;
  const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
  const D3D12_RESOURCE_DESC texture_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 1, 1, 1, 1, DXGI_FORMAT_R32G32_UINT,
                                         {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
  CHECK(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &texture_desc,
                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)));
  auto zeros = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT,
                      D3D12_RESOURCE_STATE_GENERIC_READ);
  void *zeroed;
  CHECK(zeros->Map(0, nullptr, &zeroed));
  memset(zeroed, 0, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
  zeros->Unmap(0, nullptr);
  ComPtr<ID3D12DescriptorHeap> heap;
  const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1,
                                             D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)));
  D3D12_UNORDERED_ACCESS_VIEW_DESC texel_view{DXGI_FORMAT_R32G32_UINT, D3D12_UAV_DIMENSION_TEXTURE2D};
  device->CreateUnorderedAccessView(texture.Get(), nullptr, &texel_view, heap->GetCPUDescriptorHandleForHeapStart());
  // a record for each dispatch: the identifier, then the local root arguments, the null structure's address and the
  // UAV's. then the indirect dispatch's arguments
  constexpr UINT stride = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;
  auto table = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 3 * stride, D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(results && readback && table, "the buffers are refused"))
    return verdict();
  BYTE *records;
  CHECK(table->Map(0, nullptr, (void **)&records));
  for (UINT i = 0; i < 2; i++) {
    memcpy(records + i * stride, identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    const UINT64 arguments[3] = {0, results->GetGPUVirtualAddress() + i * rays * 4,
                                 heap->GetGPUDescriptorHandleForHeapStart().ptr};
    memcpy(records + i * stride + D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, arguments, sizeof(arguments));
  }
  constexpr UINT record_size = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES + 3 * sizeof(UINT64);
  D3D12_DISPATCH_RAYS_DESC indirect{};
  indirect.RayGenerationShaderRecord = {table->GetGPUVirtualAddress() + stride, record_size};
  indirect.Width = rays;
  indirect.Height = indirect.Depth = 1;
  memcpy(records + 2 * stride, &indirect, sizeof(indirect));
  table->Unmap(0, nullptr);
  D3D12_INDIRECT_ARGUMENT_DESC argument{D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS};
  D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(D3D12_DISPATCH_RAYS_DESC), 1, &argument};
  ComPtr<ID3D12CommandSignature> signature;
  CHECK(device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signature)));

  ComPtr<ID3D12CommandQueue> queue;
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  ComPtr<ID3D12GraphicsCommandList4> list;
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  D3D12_TEXTURE_COPY_LOCATION row{zeros.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
      texel{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  row.PlacedFootprint = {0, {DXGI_FORMAT_R32G32_UINT, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT}};
  list->CopyTextureRegion(&texel, 0, 0, 0, &row, nullptr);
  transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  list->SetDescriptorHeaps(1, heap.GetAddressOf());
  list->SetPipelineState1(state.Get());
  D3D12_DISPATCH_RAYS_DESC dispatch{};
  dispatch.RayGenerationShaderRecord = {table->GetGPUVirtualAddress(), record_size};
  dispatch.Width = rays;
  dispatch.Height = dispatch.Depth = 1;
  list->DispatchRays(&dispatch);
  list->ExecuteIndirect(signature.Get(), 1, table.Get(), 2 * stride, nullptr, 0);
  transition(list.Get(), results.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(), 0, results.Get(), 0, texel_at);
  transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  row.pResource = readback.Get();
  row.PlacedFootprint.Offset = texel_at;
  list->CopyTextureRegion(&row, 0, 0, 0, &texel, nullptr);
  CHECK(list->Close());
  CHECK(execute(device.Get(), queue.Get(), list.Get()));

  UINT *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  UINT wrong = 0;
  for (UINT i = 0; i < 2 * rays; i++)
    if (got[i] != 100 && !wrong++)
      printf("%s ray %u wrote %u, want 100 (no candidates, nothing committed)\n", i < rays ? "direct" : "indirect",
             i % rays, got[i]);
  UINT64 added;
  memcpy(&added, got + texel_at / 4, sizeof(added));
  if (added != 2 * rays && ++wrong)
    printf("the 64-bit texel is %llu, want %u\n", (unsigned long long)added, 2 * rays);
  readback->Unmap(0, nullptr);
  expect(!wrong, "%u wrong results", wrong);
  return verdict();
}
