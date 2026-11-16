// contract: root arguments are state of the command list. they stay as set until the application sets them again or
// sets another root signature, whatever the list records between the dispatches that read them, and
// SetDescriptorHeaps changes the heaps alone (Using a Root Signature,
// https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-a-root-signature, and
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-setdescriptorheaps).
// each dispatch adds its root constant to a counter in a buffer a root UAV names, and a copy of the counter to a
// readback buffer after each dispatch ends the pass the dispatch ran in. the constant is 5 for two dispatches, the
// second after the heaps are set again, then 100 for a third, then 5 again after a dispatch through another root
// signature, whose constant is 3000 and which the first root signature's arguments must not reach: the counter reads
// 5, 10, 110, 3110 and 3115. a dispatch that read an earlier pass's arguments, or none, adds another number.
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
cbuffer constants : register(b0) { uint amount; };
RWStructuredBuffer<uint> counter : register(u0);
[numthreads(1, 1, 1)] void cs() { counter[0] += amount; }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto cs = compiler.compile(hlsl, "cs", "cs");
  if (!expect(!cs.empty(), "the shader did not compile"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));

  // the second root signature has a parameter before the first's two, so its arguments lie elsewhere
  D3D12_ROOT_PARAMETER params[3] = {
      {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS},
      {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS},
      {D3D12_ROOT_PARAMETER_TYPE_UAV}
  };
  params[0].Constants = {1, 0, 4};
  params[1].Constants = {0, 0, 1};
  auto first = root_signature(device.Get(), {2, params + 1}), second = root_signature(device.Get(), {3, params});
  if (!expect(first && second, "the root signatures"))
    return verdict();
  ComPtr<ID3D12PipelineState> pso[2];
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{first.Get(), bytecode(cs)};
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso[0])));
  pso_desc.pRootSignature = second.Get();
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso[1])));

  const UINT expected[] = {5, 10, 110, 3110, 3115};
  const UINT steps = std::size(expected);
  auto counter = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, sizeof(UINT), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, steps * sizeof(UINT), D3D12_RESOURCE_STATE_COPY_DEST);
  auto zero = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(UINT), D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(counter && readback && zero, "the buffers"))
    return verdict();
  void *mapped;
  CHECK(zero->Map(0, nullptr, &mapped));
  memset(mapped, 0, sizeof(UINT));
  zero->Unmap(0, nullptr);
  ComPtr<ID3D12DescriptorHeap> views;
  D3D12_DESCRIPTOR_HEAP_DESC views_desc{
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
  };
  CHECK(device->CreateDescriptorHeap(&views_desc, IID_PPV_ARGS(&views)));

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(
      device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso[0].Get(), IID_PPV_ARGS(&list))
  );

  step("dispatch five times, with a copy between");
  ID3D12DescriptorHeap *heaps[] = {views.Get()};
  UINT at = 0;
  auto dispatch = [&] {
    list->Dispatch(1, 1, 1);
    transition(list.Get(), counter.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), at++ * sizeof(UINT), counter.Get(), 0, sizeof(UINT));
    transition(list.Get(), counter.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  };
  transition(list.Get(), counter.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
  list->CopyBufferRegion(counter.Get(), 0, zero.Get(), 0, sizeof(UINT));
  transition(list.Get(), counter.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(first.Get());
  list->SetComputeRoot32BitConstant(0, 5, 0);
  list->SetComputeRootUnorderedAccessView(1, counter->GetGPUVirtualAddress());
  dispatch();
  list->SetDescriptorHeaps(1, heaps);
  dispatch();
  list->SetComputeRoot32BitConstant(0, 100, 0);
  dispatch();
  list->SetPipelineState(pso[1].Get());
  list->SetComputeRootSignature(second.Get());
  list->SetComputeRoot32BitConstant(0, 7, 0);
  list->SetComputeRoot32BitConstant(1, 3000, 0);
  list->SetComputeRootUnorderedAccessView(2, counter->GetGPUVirtualAddress());
  dispatch();
  list->SetPipelineState(pso[0].Get());
  list->SetComputeRootSignature(first.Get());
  list->SetComputeRoot32BitConstant(0, 5, 0);
  list->SetComputeRootUnorderedAccessView(1, counter->GetGPUVirtualAddress());
  dispatch();
  CHECK(forget(readback.Get()));
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  const UINT *read;
  CHECK(readback->Map(0, nullptr, (void **)&read));
  for (UINT i = 0; i < steps; i++)
    expect(read[i] == expected[i], "after dispatch %u the counter is %u, want %u", i, read[i], expected[i]);
  readback->Unmap(0, nullptr);
  return verdict();
}
