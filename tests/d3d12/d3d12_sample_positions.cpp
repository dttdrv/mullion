// contract: out-of-range and unbound sample positions are zero (D3D11.3 22.4.22, quoted in sample_positions.hpp).
// direct and indirect dispatch query each advertised sample count through both shader front ends.
#include "d3d12_test.hpp"
#include "../sample_positions.hpp"

int
main(int argc, char **argv)
{
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto code = compiler.compile(sample_positions::hlsl, "cs", "cs");
  if (!expect(!code.empty(), "sample position HLSL compiled"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1};
  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[0].DescriptorTable = {1, &range};
  auto root = root_signature(device.Get(), {2, params});
  if (!expect(!!root, "sample position root signature"))
    return verdict();
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{root.Get(), bytecode(code)};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)));
  ComPtr<ID3D12DescriptorHeap> views;
  D3D12_DESCRIPTOR_HEAP_DESC views_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1,
                                        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&views_desc, IID_PPV_ARGS(&views)));
  auto output = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, sample_positions::bytes,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback =
      buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, sample_positions::bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  auto arguments =
      buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(D3D12_DISPATCH_ARGUMENTS), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto poison =
      buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sample_positions::bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(output && readback && arguments && poison, "sample position buffers"))
    return verdict();
  void *mapped;
  CHECK(arguments->Map(0, nullptr, &mapped));
  const D3D12_DISPATCH_ARGUMENTS dispatch{1, 1, 1};
  memcpy(mapped, &dispatch, sizeof(dispatch));
  arguments->Unmap(0, nullptr);
  CHECK(poison->Map(0, nullptr, &mapped));
  memset(mapped, 0xff, sample_positions::bytes);
  poison->Unmap(0, nullptr);
  D3D12_INDIRECT_ARGUMENT_DESC argument{D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH};
  D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(dispatch), 1, &argument};
  ComPtr<ID3D12CommandSignature> signature;
  CHECK(device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signature)));
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  for (UINT count = 0; count <= D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT; count = count ? count * 2 : 1) {
    ComPtr<ID3D12Resource> texture;
    if (count) {
      D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels{DXGI_FORMAT_R32_FLOAT, count};
      CHECK(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &levels, sizeof(levels)));
      if (!levels.NumQualityLevels) {
        printf("not supported: %ux R32_FLOAT\n", count);
        continue;
      }
      D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
      D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                               0,
                               1,
                               1,
                               1,
                               1,
                               DXGI_FORMAT_R32_FLOAT,
                               {count, 0},
                               D3D12_TEXTURE_LAYOUT_UNKNOWN,
                               D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
      CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                            IID_PPV_ARGS(&texture)));
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{DXGI_FORMAT_R32_FLOAT, D3D12_SRV_DIMENSION_TEXTURE2DMS,
                                        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
    device->CreateShaderResourceView(texture.Get(), &srv, views->GetCPUDescriptorHandleForHeapStart());
    for (bool indirect : {false, true}) {
      step("samplepos %ux, %s dispatch", count, indirect ? "indirect" : "direct");
      CHECK(forget(readback.Get()));
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), pso.Get()));
      transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
      list->CopyBufferRegion(output.Get(), 0, poison.Get(), 0, sample_positions::bytes);
      transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      list->SetComputeRootSignature(root.Get());
      list->SetDescriptorHeaps(1, views.GetAddressOf());
      list->SetComputeRootDescriptorTable(0, views->GetGPUDescriptorHandleForHeapStart());
      list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
      if (indirect)
        list->ExecuteIndirect(signature.Get(), 1, arguments.Get(), 0, nullptr, 0);
      else
        list->Dispatch(1, 1, 1);
      transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyResource(readback.Get(), output.Get());
      transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      CHECK(readback->Map(0, nullptr, &mapped));
      sample_positions::check(static_cast<uint32_t *>(mapped), count);
      readback->Unmap(0, nullptr);
    }
  }
  return verdict();
}
