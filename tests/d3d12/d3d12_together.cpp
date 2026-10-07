// contract: all the threadgroups of a dispatch run as one dispatch unless the kernel is one whose groups wait for
// each other through globally coherent memory. Direct3D starts every group of a Dispatch together as far as the GPU
// has room; Mullion sends the dispatch of a kernel in which a loop reads globally coherent memory in parts, one
// after another (an Apple GPU takes turns among more groups than it keeps running, and a group that waits for
// another then waits a round). that changes what a kernel can see, so it must happen for those kernels only.
// what a kernel sees of it: every group counts itself in at its start and then looks, turn after turn, whether all
// GROUPS groups are in. in one dispatch they all are, at once; in parts the groups of every part but the last
// give up at TURNS. GROUPS is more groups than a part ever has (parts are of at most 16384 threads) and fewer than
// the GPU runs side by side. the counting and looking go through atomics on a buffer that is not globally coherent,
// so they are the same in every case; the cases differ in what else the kernel does with a globally coherent buffer:
// - reads it in the loop, or does an atomic on it in the loop: its groups work together, the dispatch goes in parts;
// - reads it before the loop only, only stores to it in the loop, reads it inside a switch with breaks and no loop
//   around it, or has it and never touches it: one dispatch.
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
RWStructuredBuffer<uint> meeting : register(u0);               // 0: groups in, 1: groups that saw all in
globallycoherent RWStructuredBuffer<uint> coherent : register(u1);
[numthreads(64, 1, 1)] void cs(uint thread : SV_GroupIndex) {
  uint kept = 0;
#if CASE == 1
  kept = coherent[0];
#elif CASE == 3
  switch (meeting[2]) {
  case 0: kept = coherent[0]; break;
  case 1: kept = coherent[1]; break;
  default: break;
  }
#endif
  if (thread)
    return;
  uint in_now;
  InterlockedAdd(meeting[0], 1, in_now);
  bool met = false;
  for (uint turn = 0; turn < TURNS && !met; turn++) {
    InterlockedOr(meeting[0], 0, in_now);
    met = in_now == GROUPS;
#if CASE == 0
    kept += coherent[0];
#elif CASE == 2
    coherent[1] = turn;
#elif CASE == 5
    InterlockedAdd(coherent[2], 0);
#endif
  }
  if (met)
    InterlockedAdd(meeting[1], 1);
  // what was read is used
  InterlockedAdd(meeting[3], kept);
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const UINT groups = 320, turns = 1 << 16, words = 4;
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER parameters[2] = {{D3D12_ROOT_PARAMETER_TYPE_UAV}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  parameters[1].Descriptor = {1, 0};
  auto rs = root_signature(device.Get(), {(UINT)std::size(parameters), parameters});
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  const UINT64 bytes = words * sizeof(UINT);
  auto zeros = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto meeting = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto shared = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  void *start;
  CHECK(zeros->Map(0, nullptr, &start));
  memset(start, 0, bytes);

  struct Case {
    const char *what;
    bool together;
  };
  const Case cases[] = {
      {"reads globally coherent memory in its loop", true},
      {"reads globally coherent memory before its loop only", false},
      {"only stores to globally coherent memory in its loop", false},
      {"reads globally coherent memory in a switch with breaks, outside its loop", false},
      {"has a globally coherent buffer and does not touch it", false},
      {"does an atomic on globally coherent memory in its loop", true},
  };
  for (UINT number = 0; number < std::size(cases); number++) {
    step("a kernel that %s", cases[number].what);
    auto cs = compiler.compile(hlsl, "cs", "cs", {"CASE=" + std::to_string(number), "GROUPS=" + std::to_string(groups), "TURNS=" + std::to_string(turns)});
    if (!expect(!cs.empty(), "the HLSL did not compile"))
      continue;
    ComPtr<ID3D12PipelineState> pso;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{rs.Get(), bytecode(cs)};
    HRESULT made = device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso));
    if (!expect(made == S_OK, "the pipeline: %08lx", made))
      continue;
    list->CopyBufferRegion(meeting.Get(), 0, zeros.Get(), 0, bytes);
    transition(list.Get(), meeting.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetPipelineState(pso.Get());
    list->SetComputeRootSignature(rs.Get());
    list->SetComputeRootUnorderedAccessView(0, meeting->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(1, shared->GetGPUVirtualAddress());
    list->Dispatch(groups, 1, 1);
    transition(list.Get(), meeting.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(readback.Get(), meeting.Get());
    transition(list.Get(), meeting.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), nullptr));
    const UINT *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    expect(got[0] == groups, "%u of %u groups ran", got[0], groups);
    if (cases[number].together)
      expect(got[1] < groups, "all %u groups saw each other: the dispatch was not in parts", groups);
    else
      expect(got[1] == groups, "%u of %u groups saw all the others in: the dispatch was in parts", got[1], groups);
    printf("a kernel that %s: %u of %u groups saw all in\n", cases[number].what, got[1], groups);
    readback->Unmap(0, nullptr);
  }
  return verdict();
}
