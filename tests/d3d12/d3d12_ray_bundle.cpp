// contract: a bundle's SetPipelineState1 and DispatchRays run on the list that executes it. "Can be called from
// graphics or compute command lists and bundles" (DXR spec, SetPipelineState1 and DispatchRays), and a bundle's calls
// run when a list calls ExecuteBundle (Microsoft Learn, Creating and recording command lists and bundles).
// https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html#dispatchrays
// https://learn.microsoft.com/en-us/windows/win32/direct3d12/recording-command-lists-and-bundles
// a ray generation shader writes 100 + DispatchRaysIndex().x through its local root signature's root UAV. each of two
// bundles sets the state object and dispatches 64 rays, the second through a record whose UAV starts past the first
// 64 results. the list that executes the bundles sets no state object and dispatches nothing itself, so every result
// is a bundle's.
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
RWStructuredBuffer<uint> results : register(u0);
[shader("raygeneration")] void generate() {
  results[DispatchRaysIndex().x] = 100 + DispatchRaysIndex().x;
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler) || !compiler.dxc) {
    printf("skipped: ray tracing libraries need DXC\n");
    return 77;
  }
  auto library = compiler.compile(hlsl, "", "lib_6_3");
  if (!expect(!library.empty(), "the library does not compile"))
    return verdict();
  ComPtr<ID3D12Device5> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options)));
  if (options.RaytracingTier < D3D12_RAYTRACING_TIER_1_0) {
    printf("skipped: ray tracing is not supported\n");
    return 77;
  }

  D3D12_ROOT_PARAMETER parameter{D3D12_ROOT_PARAMETER_TYPE_UAV};
  auto local = root_signature(device.Get(), {1, &parameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE});
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
  if (!expect(SUCCEEDED(hr), "the state object is refused: %08lx", hr))
    return verdict();
  ComPtr<ID3D12StateObjectProperties> properties;
  CHECK(state.As(&properties));
  auto identifier = properties->GetShaderIdentifier(generate);
  if (!expect(!!identifier, "the ray generation shader has no identifier"))
    return verdict();

  constexpr UINT rays = 64, dispatches = 2;
  auto results = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, dispatches * rays * 4,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, dispatches * rays * 4, D3D12_RESOURCE_STATE_COPY_DEST);
  // a record for each dispatch: the identifier, then the local root argument, the UAV's address
  constexpr UINT stride = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;
  constexpr UINT record_size = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES + sizeof(UINT64);
  auto table = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, dispatches * stride, D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(results && readback && table, "the buffers are refused"))
    return verdict();
  BYTE *records;
  CHECK(table->Map(0, nullptr, (void **)&records));
  for (UINT i = 0; i < dispatches; i++) {
    memcpy(records + i * stride, identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    const UINT64 argument = results->GetGPUVirtualAddress() + i * rays * 4;
    memcpy(records + i * stride + D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, &argument, sizeof(argument));
  }
  table->Unmap(0, nullptr);

  ComPtr<ID3D12CommandQueue> queue;
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator, bundle_allocator;
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&bundle_allocator)));
  ComPtr<ID3D12GraphicsCommandList4> list, bundles[dispatches];
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  for (UINT i = 0; i < dispatches; i++) {
    CHECK(device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bundle_allocator.Get(), nullptr, IID_PPV_ARGS(&bundles[i])
    ));
    D3D12_DISPATCH_RAYS_DESC dispatch{};
    dispatch.RayGenerationShaderRecord = {table->GetGPUVirtualAddress() + i * stride, record_size};
    dispatch.Width = rays;
    dispatch.Height = dispatch.Depth = 1;
    bundles[i]->SetPipelineState1(state.Get());
    bundles[i]->DispatchRays(&dispatch);
    CHECK(bundles[i]->Close());
  }
  for (auto &bundle : bundles)
    list->ExecuteBundle(bundle.Get());
  transition(list.Get(), results.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(), 0, results.Get(), 0, dispatches * rays * 4);
  CHECK(list->Close());
  CHECK(execute(device.Get(), queue.Get(), list.Get()));

  UINT *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  UINT wrong = 0;
  for (UINT i = 0; i < dispatches * rays; i++)
    if (got[i] != 100 + i % rays && !wrong++)
      printf("ray %u of the %s bundle wrote %u, want %u\n", i % rays, i < rays ? "first" : "second", got[i],
             100 + i % rays);
  readback->Unmap(0, nullptr);
  expect(!wrong, "%u wrong results", wrong);
  return verdict();
}
