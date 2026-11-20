// contract: "x*y*z <= 1024" and shared storage "32kB" (D3D11.3 22.3.40, 22.3.45-46).
// shader_groups.hpp quotes the memory fence and derives every word and system ID; direct and indirect dispatch
// must produce the same values at the dimension, thread-count and memory limits.
#include "d3d12_test.hpp"
#include "../shader_groups.hpp"

int
main(int argc, char **argv)
{
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  struct Case {
    shader_groups::Shape shape;
    UINT words;
    std::string code;
  };
  std::vector<Case> cases;
  for (auto shape : shader_groups::shapes(D3D12_CS_THREAD_GROUP_MAX_X, D3D12_CS_THREAD_GROUP_MAX_Z,
                                          D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP))
    for (UINT words : {D3D12_CS_TGSM_REGISTER_COUNT - 1u, UINT(D3D12_CS_TGSM_REGISTER_COUNT)}) {
      auto code = compiler.compile(shader_groups::hlsl, "cs", "cs", shader_groups::defines(shape, words));
      if (!expect(!code.empty(), "group %ux%ux%u, %u shared words compiled", shape.x, shape.y, shape.z, words))
        return verdict();
      cases.push_back({shape, words, code});
    }
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER param{D3D12_ROOT_PARAMETER_TYPE_UAV};
  auto root = root_signature(device.Get(), {1, &param});
  if (!expect(!!root, "group root signature"))
    return verdict();
  auto arguments =
      buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(D3D12_DISPATCH_ARGUMENTS), D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(!!arguments, "group dispatch arguments"))
    return verdict();
  void *mapped;
  CHECK(arguments->Map(0, nullptr, &mapped));
  const D3D12_DISPATCH_ARGUMENTS dispatch{shader_groups::groups.x, shader_groups::groups.y, shader_groups::groups.z};
  memcpy(mapped, &dispatch, sizeof(dispatch));
  arguments->Unmap(0, nullptr);
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
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  unsigned skipped = 0;
  for (auto &c : cases) {
    if (!shader_groups::runs(c.words, D3D12_CS_TGSM_REGISTER_COUNT)) {
      skipped++;
      continue;
    }
    const UINT bytes =
        shader_groups::group_count * shader_groups::rounds * c.words * shader_groups::components * sizeof(uint32_t);
    auto output = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    auto poison = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!expect(output && readback && poison, "group buffers"))
      return verdict();
    CHECK(poison->Map(0, nullptr, &mapped));
    memset(mapped, 0xff, bytes);
    poison->Unmap(0, nullptr);
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{root.Get(), bytecode(c.code)};
    ComPtr<ID3D12PipelineState> pso;
    CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)));
    for (bool indirect : {false, true}) {
      step("group %ux%ux%u, %u shared words, %s dispatch", c.shape.x, c.shape.y, c.shape.z, c.words,
           indirect ? "indirect" : "direct");
      CHECK(forget(readback.Get()));
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), pso.Get()));
      transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
      list->CopyBufferRegion(output.Get(), 0, poison.Get(), 0, bytes);
      transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      list->SetComputeRootSignature(root.Get());
      list->SetComputeRootUnorderedAccessView(0, output->GetGPUVirtualAddress());
      if (indirect)
        list->ExecuteIndirect(signature.Get(), 1, arguments.Get(), 0, nullptr, 0);
      else
        list->Dispatch(dispatch.ThreadGroupCountX, dispatch.ThreadGroupCountY, dispatch.ThreadGroupCountZ);
      transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyResource(readback.Get(), output.Get());
      transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      CHECK(readback->Map(0, nullptr, &mapped));
      shader_groups::check(static_cast<uint32_t *>(mapped), c.shape, c.words);
      readback->Unmap(0, nullptr);
    }
  }
  return shader_groups::verdict(skipped);
}
