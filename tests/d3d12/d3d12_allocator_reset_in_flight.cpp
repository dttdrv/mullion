// contract: the work a queue was given runs as recorded when the application resets the list's command allocator
// right after ExecuteCommandLists and records the next list in it, and the process goes on. Direct3D 12 leaves the
// order to the application (ID3D12CommandAllocator::Reset: "it is not recommended that you call Reset on the command
// allocator while a command list is still being executed", and the debug layer's COMMAND_ALLOCATOR_SYNC), so what
// such a reset does is not specified; Mullion keeps what the submitted lists recorded until the queues' work has
// completed (docs/ARCHITECTURE.md), which is what this test holds it to, as d3d12_release_in_flight does for
// resources. a list of dispatches writes one buffer, each dispatch a part of it from root constants of its own; the
// allocator is reset as soon as the list is given, and a second list recorded in it writes another buffer from other
// constants. each buffer has what its own list's constants give. in even rounds the queue waits for a fence before
// the first list, which is signalled only after the second is recorded: the first list's work then starts after
// the allocator has recorded over where it was, and a reset that waited for the list would not return.
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
RWStructuredBuffer<uint> dst : register(u0);
cbuffer C : register(b0) { uint base; uint seed; };
[numthreads(64, 1, 1)]
void cs(uint3 tid : SV_DispatchThreadID) {
  dst[base + tid.x] = seed + base + tid.x;
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto code = compiler.compile(hlsl, "cs", "cs");
  if (code.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_UAV}, {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}};
  params[1].Constants.Num32BitValues = 2;
  auto rs = root_signature(device.Get(), {2, params});
  if (!rs) {
    printf("failed: root signature\n");
    return 1;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{rs.Get(), bytecode(code)};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)));

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  ComPtr<ID3D12Fence> gate, done;
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)));
  auto event = CreateEventA(nullptr, FALSE, FALSE, nullptr);

  // enough dispatches that a list's commands and constants take more than one of the allocator's pieces of memory
  constexpr UINT kThreads = 64, kDispatches = 2048, kWords = kThreads * kDispatches, kLists = 2;
  constexpr UINT64 kBytes = UINT64(kWords) * sizeof(UINT);
  ComPtr<ID3D12Resource> outputs[kLists], readbacks[kLists];
  for (UINT i = 0; i < kLists; i++) {
    outputs[i] = buffer(
        device.Get(), D3D12_HEAP_TYPE_DEFAULT, kBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
    );
    readbacks[i] = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, kBytes, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!expect(outputs[i] && readbacks[i], "buffers of list %u", i))
      return verdict();
  }
  // word w of list l's buffer in round r
  auto seed = [](UINT round, UINT l) { return (round * kLists + l + 1) << 20; };
  auto record = [&](UINT round, UINT l) {
    list->SetComputeRootSignature(rs.Get());
    list->SetComputeRootUnorderedAccessView(0, outputs[l]->GetGPUVirtualAddress());
    for (UINT d = 0; d < kDispatches; d++) {
      UINT constants[2] = {d * kThreads, seed(round, l)};
      list->SetComputeRoot32BitConstants(1, 2, constants, 0);
      list->Dispatch(1, 1, 1);
    }
    transition(list.Get(), outputs[l].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(readbacks[l].Get(), outputs[l].Get());
    transition(list.Get(), outputs[l].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  };

  UINT64 gated = 0, signalled = 0;
  for (UINT round = 0; round < 8; round++) {
    const bool waits = !(round & 1);
    step("round %u, %s", round, waits ? "the first list behind a fence" : "the first list at once");
    for (auto &readback : readbacks)
      CHECK(forget(readback.Get()));
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), pso.Get()));
    record(round, 0);
    CHECK(list->Close());
    ID3D12CommandList *lists[] = {list.Get()};
    if (waits)
      CHECK(queue->Wait(gate.Get(), ++gated));
    queue->ExecuteCommandLists(1, lists);
    // reset while the list is in flight, and recorded over
    if (!expect(SUCCEEDED(allocator->Reset()), "Reset with the first list in flight"))
      return verdict();
    CHECK(list->Reset(allocator.Get(), pso.Get()));
    record(round, 1);
    CHECK(list->Close());
    if (waits)
      CHECK(gate->Signal(gated));
    queue->ExecuteCommandLists(1, lists);
    CHECK(queue->Signal(done.Get(), ++signalled));
    CHECK(done->SetEventOnCompletion(signalled, event));
    WaitForSingleObject(event, INFINITE);
    if (!expect(SUCCEEDED(device->GetDeviceRemovedReason()), "the device is removed"))
      return verdict();
    for (UINT l = 0; l < kLists; l++) {
      const UINT *got;
      CHECK(readbacks[l]->Map(0, nullptr, (void **)&got));
      unsigned wrong = 0;
      for (UINT w = 0; w < kWords; w++)
        if (got[w] != seed(round, l) + w && wrong++ < 4)
          printf("  list %u, word %u is %#x, want %#x\n", l, w, got[w], seed(round, l) + w);
      readbacks[l]->Unmap(0, nullptr);
      expect(!wrong, "round %u, list %u: %u of %u words wrong", round, l, wrong, kWords);
    }
  }
  CloseHandle(event);
  return verdict();
}
