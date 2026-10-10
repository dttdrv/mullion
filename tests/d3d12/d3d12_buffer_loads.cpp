// contract: raw reads return zero for each component past the view, and structured indices cannot wrap into it.
// "Out of bounds addressing on u#/t# of any given 32-bit component returns 0 for that component." (D3D11.3 22.4.10,
// 22.4.12). DXIL's native 16-bit and 64-bit values are checked as bytes too, including partial 64-bit raw reads.
#include "d3d12_test.hpp"
#include "../buffer_loads.hpp"

int
main(int argc, char **argv) {
  using namespace buffer_loads;
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  std::vector<Case> cases;
  std::vector<std::string> shaders;
  for (UINT bits : {sizeof(uint16_t) * CHAR_BIT, sizeof(uint32_t) * CHAR_BIT, sizeof(uint64_t) * CHAR_BIT}) {
    if (!compiler.dxc && bits != sizeof(UINT) * CHAR_BIT)
      continue;
    for (UINT width = 1; width <= components; width++)
      for (UINT padding : {0u, components}) {
        cases.emplace_back(width + padding, bits, width, padding, D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT);
        shaders.push_back(
            compiler.compile(hlsl, "cs", bits == sizeof(UINT) * CHAR_BIT ? "cs" : "cs_6_2", cases.back().defines(),
                             bits < sizeof(UINT) * CHAR_BIT ? std::vector<std::wstring>{L"-enable-16bit-types"}
                                                            : std::vector<std::wstring>{}));
        if (shaders.back().empty()) {
          printf("failed: HLSL did not compile\n");
          return 1;
        }
      }
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  if (compiler.dxc) {
    D3D12_FEATURE_DATA_D3D12_OPTIONS4 options{};
    CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &options, sizeof(options)));
    if (!options.Native16BitShaderOpsSupported) {
      printf("skipped: native 16-bit shader operations unavailable\n");
      return 77;
    }
  }
  D3D12_DESCRIPTOR_RANGE ranges[] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 5}};
  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
                                    {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}};
  params[0].DescriptorTable = {1, &ranges[0]};
  params[1].DescriptorTable = {1, &ranges[1]};
  auto signature = root_signature(device.Get(), {UINT(std::size(params)), params});
  if (!signature) {
    printf("failed: root signature\n");
    return 1;
  }
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  ComPtr<ID3D12DescriptorHeap> heap;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8,
                                       D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)));
  const UINT apart = device->GetDescriptorHandleIncrementSize(heap_desc.Type);
  auto view_at = [&](UINT slot) {
    auto handle = heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += slot * apart;
    return handle;
  };
  for (size_t variant = 0; variant < cases.size(); variant++) {
    auto &test = cases[variant];
    step("seed %u, %u-bit width %u, padding %u, first %u, count %u", test.seed, test.bits, test.width, test.padding,
         test.first, test.count);
    ComPtr<ID3D12PipelineState> pipeline;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{signature.Get(), bytecode(shaders[variant])};
    CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pipeline)));
    CHECK(allocator->Reset());
    ComPtr<ID3D12GraphicsCommandList> list;
    CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&list)));
    auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD,
                         test.data.size() + test.addresses.size() * sizeof(Address), D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!upload) {
      printf("failed: upload buffer\n");
      return 1;
    }
    uint8_t *bytes;
    CHECK(upload->Map(0, nullptr, reinterpret_cast<void **>(&bytes)));
    memcpy(bytes, test.data.data(), test.data.size());
    memcpy(bytes + test.data.size(), test.addresses.data(), test.addresses.size() * sizeof(Address));
    upload->Unmap(0, nullptr);
    ComPtr<ID3D12Resource> sources[6];
    for (UINT kind = 0; kind < std::size(sources); kind++) {
      const bool structured = kind % 2;
      const auto state =
          kind < 2 ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
      sources[kind] = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, test.data.size(), D3D12_RESOURCE_STATE_COPY_DEST,
                             kind < 2 ? D3D12_RESOURCE_FLAG_NONE : D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
      if (!sources[kind]) {
        printf("failed: source buffer\n");
        return 1;
      }
      list->CopyBufferRegion(sources[kind].Get(), 0, upload.Get(), 0, test.data.size());
      transition(list.Get(), sources[kind].Get(), D3D12_RESOURCE_STATE_COPY_DEST, state);
      const UINT first = structured ? test.first : test.raw_first;
      const UINT count = structured ? test.count : test.raw_bytes / sizeof(UINT);
      const auto format = structured ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_TYPELESS;
      if (kind < 2) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{format, D3D12_SRV_DIMENSION_BUFFER,
                                             D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
        view.Buffer = {first, count, structured ? test.stride : 0,
                       structured ? D3D12_BUFFER_SRV_FLAG_NONE : D3D12_BUFFER_SRV_FLAG_RAW};
        device->CreateShaderResourceView(sources[kind].Get(), &view, view_at(kind));
      } else {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{format, D3D12_UAV_DIMENSION_BUFFER};
        view.Buffer = {first, count, structured ? test.stride : 0, 0,
                       structured ? D3D12_BUFFER_UAV_FLAG_NONE : D3D12_BUFFER_UAV_FLAG_RAW};
        device->CreateUnorderedAccessView(sources[kind].Get(), nullptr, &view, view_at(kind + 1));
      }
    }
    auto addresses = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, test.addresses.size() * sizeof(Address),
                            D3D12_RESOURCE_STATE_COPY_DEST);
    const UINT64 result_bytes = test.expected.size() * sizeof(Result);
    auto result = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, result_bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, result_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!addresses || !result || !readback) {
      printf("failed: address, result or readback buffer\n");
      return 1;
    }
    list->CopyBufferRegion(addresses.Get(), 0, upload.Get(), test.data.size(), test.addresses.size() * sizeof(Address));
    transition(list.Get(), addresses.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    D3D12_SHADER_RESOURCE_VIEW_DESC address_view{DXGI_FORMAT_UNKNOWN, D3D12_SRV_DIMENSION_BUFFER,
                                                 D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
    address_view.Buffer = {0, UINT(test.addresses.size()), sizeof(Address)};
    device->CreateShaderResourceView(addresses.Get(), &address_view, view_at(2));
    D3D12_UNORDERED_ACCESS_VIEW_DESC result_view{DXGI_FORMAT_UNKNOWN, D3D12_UAV_DIMENSION_BUFFER};
    result_view.Buffer = {0, UINT(test.expected.size()), sizeof(Result)};
    device->CreateUnorderedAccessView(result.Get(), nullptr, &result_view, view_at(heap_desc.NumDescriptors - 1));
    CHECK(forget(readback.Get()));
    ID3D12DescriptorHeap *heaps[] = {heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(signature.Get());
    auto reads = heap->GetGPUDescriptorHandleForHeapStart(), writes = reads;
    writes.ptr += ranges[0].NumDescriptors * apart;
    list->SetComputeRootDescriptorTable(0, reads);
    list->SetComputeRootDescriptorTable(1, writes);
    list->Dispatch(1, 1, 1);
    transition(list.Get(), result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(readback.Get(), result.Get());
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    void *got;
    CHECK(readback->Map(0, nullptr, &got));
    test.check(got);
    readback->Unmap(0, nullptr);
  }
  return verdict();
}
