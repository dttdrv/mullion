// contract: two queues that take turns through each other's fences, as an engine's graphics and async compute queues
// do every frame, run each frame in the time its work takes. Wait "queues a GPU-side wait, and returns immediately"
// and is over once the fence "reaches or exceeds the specified value" (ID3D12CommandQueue::Wait), whatever is
// signaled after. each frame the direct queue copies the number in and signals, the compute queue waits for that,
// computes the next number and signals, and the direct queue waits for that and copies the number out. the CPU stays
// two frames behind, as a game does, so a queue is given its next waits and signals while the last ones are still on
// the GPU, and every third frame the compute queue first waits for a value of the direct queue's that later signals
// have passed.
// - every frame's number is the one that order gives;
// - no frame is waited for longer than the limit: its work is two copies and two dispatches of milliseconds.
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
RWStructuredBuffer<uint> from : register(u0);
RWStructuredBuffer<uint> to : register(u1);
RWStructuredBuffer<uint> spun : register(u2);
[numthreads(1, 1, 1)] void cs() { to[0] = from[0] * 3 + 1; }
// work that keeps the GPU for a while, so that a signal is still on its way when the other queue waits for it
[numthreads(64, 1, 1)] void cs_busy(uint3 thread : SV_DispatchThreadID) {
  uint random = thread.x;
  for (uint i = 0; i < SPINS; i++)
    random = random * 1664525 + 1013904223;
  InterlockedAdd(spun[0], random);
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const UINT frames = 1500, behind = 2, slots = 4, limit_ms = 2000, spins = 1 << 12;
  std::vector<std::string> defines{"SPINS=" + std::to_string(spins)};
  auto cs = compiler.compile(hlsl, "cs", "cs", defines), cs_busy = compiler.compile(hlsl, "cs_busy", "cs", defines);
  if (cs.empty() || cs_busy.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER parameters[3] = {{D3D12_ROOT_PARAMETER_TYPE_UAV}, {D3D12_ROOT_PARAMETER_TYPE_UAV},
                                        {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  parameters[1].Descriptor = {1, 0};
  parameters[2].Descriptor = {2, 0};
  auto rs = root_signature(device.Get(), {3, parameters});
  ComPtr<ID3D12PipelineState> pso, pso_busy;
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{rs.Get(), bytecode(cs)};
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)));
  pso_desc.CS = bytecode(cs_busy);
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso_busy)));

  auto words = [&](D3D12_HEAP_TYPE heap, UINT count, D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags) {
    return buffer(device.Get(), heap, count * sizeof(UINT), state, flags);
  };
  const auto ua = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  // `in` is what the compute queue reads, `out` what it writes: the direct queue copies `out` to `in` and out
  auto in = words(D3D12_HEAP_TYPE_DEFAULT, 1, D3D12_RESOURCE_STATE_COMMON, ua);
  auto out = words(D3D12_HEAP_TYPE_DEFAULT, 1, D3D12_RESOURCE_STATE_COMMON, ua);
  auto spun = words(D3D12_HEAP_TYPE_DEFAULT, 1, D3D12_RESOURCE_STATE_COMMON, ua);
  auto readback = words(D3D12_HEAP_TYPE_READBACK, slots, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
  UINT *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));

  enum { Compute, Direct, Queues };
  const D3D12_COMMAND_LIST_TYPE types[Queues] = {D3D12_COMMAND_LIST_TYPE_COMPUTE, D3D12_COMMAND_LIST_TYPE_DIRECT};
  ComPtr<ID3D12CommandQueue> queues[Queues];
  ComPtr<ID3D12CommandAllocator> allocators[Queues];
  ComPtr<ID3D12Fence> fences[Queues];
  for (UINT q = 0; q < Queues; q++) {
    D3D12_COMMAND_QUEUE_DESC desc{types[q]};
    CHECK(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queues[q])));
    CHECK(device->CreateCommandAllocator(types[q], IID_PPV_ARGS(&allocators[q])));
    CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fences[q])));
  }
  // the frames run the same closed lists again and again; only the copy out has one a readback slot
  std::vector<ComPtr<ID3D12GraphicsCommandList>> lists;
  auto recorded = [&](UINT q, auto record) -> ID3D12CommandList * {
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandList(0, types[q], allocators[q].Get(), nullptr, IID_PPV_ARGS(&list))))
      return nullptr;
    list->SetComputeRootSignature(rs.Get());
    list->SetComputeRootUnorderedAccessView(0, in->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(2, spun->GetGPUVirtualAddress());
    record(list.Get());
    list->Close();
    lists.push_back(list);
    return list.Get();
  };
  auto dispatch = [&](ID3D12GraphicsCommandList *list, ID3D12PipelineState *state, ID3D12Resource *written, UINT groups) {
    transition(list, written, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetPipelineState(state);
    list->Dispatch(groups, 1, 1);
    transition(list, written, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
  };
  auto copy = [&](ID3D12GraphicsCommandList *list, ID3D12Resource *to, UINT at, D3D12_RESOURCE_STATES to_state) {
    transition(list, out.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (to_state != D3D12_RESOURCE_STATE_COPY_DEST)
      transition(list, to, to_state, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyBufferRegion(to, at * sizeof(UINT), out.Get(), 0, sizeof(UINT));
    if (to_state != D3D12_RESOURCE_STATE_COPY_DEST)
      transition(list, to, D3D12_RESOURCE_STATE_COPY_DEST, to_state);
    transition(list, out.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
  };
  ID3D12CommandList *copies_in = recorded(Direct, [&](auto list) {
    dispatch(list, pso_busy.Get(), spun.Get(), 16);
    copy(list, in.Get(), 0, D3D12_RESOURCE_STATE_COMMON);
  });
  ID3D12CommandList *computes = recorded(Compute, [&](auto list) {
    dispatch(list, pso_busy.Get(), spun.Get(), 16);
    dispatch(list, pso.Get(), out.Get(), 1);
  });
  ID3D12CommandList *copies_out[slots];
  for (UINT slot = 0; slot < slots; slot++)
    copies_out[slot] = recorded(Direct, [&](auto list) { copy(list, readback.Get(), slot, D3D12_RESOURCE_STATE_COPY_DEST); });
  if (!copies_in || !computes || !copies_out[slots - 1]) {
    printf("failed: no command lists\n");
    return 1;
  }

  // the direct queue's fence is at 2 * frame + 1 once the frame's number is in and at 2 * frame + 2 once it is out,
  // the compute queue's at frame + 1 once it is computed
  auto event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  UINT number = 0, slowest_ms = 0, slowest = 0;
  auto ended = [&](UINT frame) {
    DWORD began = GetTickCount();
    bool done = SUCCEEDED(fences[Direct]->SetEventOnCompletion(2 * frame + 2, event)) &&
                WaitForSingleObject(event, 60 * 1000) == WAIT_OBJECT_0;
    if (!expect(done, "frame %u has not ended after 60 s: the direct queue's fence is at %llu, the compute queue's at %llu",
                frame, fences[Direct]->GetCompletedValue(), fences[Compute]->GetCompletedValue()))
      ExitProcess(verdict());
    UINT took = GetTickCount() - began;
    if (took > slowest_ms)
      slowest_ms = took, slowest = frame;
    number = number * 3 + 1;
    expect(got[frame % slots] == number, "frame %u: %u, want %u", frame, got[frame % slots], number);
  };
  step("%u frames of turns between the direct and the compute queue, the CPU %u frames behind", frames, behind);
  for (UINT frame = 0; frame < frames; frame++) {
    queues[Direct]->ExecuteCommandLists(1, &copies_in);
    CHECK(queues[Direct]->Signal(fences[Direct].Get(), 2 * frame + 1));
    if (frame % 3 == 2) {
      CHECK(queues[Compute]->Wait(fences[Direct].Get(), 2 * frame - 1));
    }
    CHECK(queues[Compute]->Wait(fences[Direct].Get(), 2 * frame + 1));
    queues[Compute]->ExecuteCommandLists(1, &computes);
    CHECK(queues[Compute]->Signal(fences[Compute].Get(), frame + 1));
    CHECK(queues[Direct]->Wait(fences[Compute].Get(), frame + 1));
    queues[Direct]->ExecuteCommandLists(1, &copies_out[frame % slots]);
    CHECK(queues[Direct]->Signal(fences[Direct].Get(), 2 * frame + 2));
    if (frame >= behind)
      ended(frame - behind);
  }
  for (UINT frame = frames - behind; frame < frames; frame++)
    ended(frame);
  expect(slowest_ms < limit_ms, "frame %u was waited for %u ms, the limit is %u", slowest, slowest_ms, limit_ms);
  printf("the slowest frame was waited for %u ms\n", slowest_ms);
  return verdict();
}
