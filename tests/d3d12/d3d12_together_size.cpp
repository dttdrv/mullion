// contract: a dispatch of a kernel whose groups wait for one another (a loop reads globally coherent memory) goes to
// the GPU in parts of as many groups as its cores keep at once (d3d12_together): the room a part fills is the
// cores', so it holds as many threads whatever the kernel's own group size. what a kernel sees of it: every group
// counts itself in at its start and then looks, turn after turn, whether `goal` groups are in. every group of the
// dispatch sees that many exactly when the first part holds `goal` groups, so the most `goal` for which all do is a
// part's size. the part's size is not set here (MULLION_D3D12_THREADS_AT_ONCE): it is what the library takes from
// the GPU
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
cbuffer goals : register(b0) { uint goal; };
RWStructuredBuffer<uint> meeting : register(u0);               // 0: groups in, 1: groups that saw `goal` in
globallycoherent RWStructuredBuffer<uint> coherent : register(u1);
[numthreads(THREADS, 1, 1)] void cs(uint thread : SV_GroupIndex) {
  uint kept = 0;
  if (thread)
    return;
  uint in_now;
  InterlockedAdd(meeting[0], 1, in_now);
  bool met = false;
  for (uint turn = 0; turn < TURNS && !met; turn++) {
    InterlockedOr(meeting[0], 0, in_now);
    met = in_now >= goal;
    kept += coherent[0];
  }
  if (met)
    InterlockedAdd(meeting[1], 1);
  // what was read is used
  InterlockedAdd(meeting[2], kept);
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  // a dispatch of more threads than the GPU runs at once, whose waiting groups turn long enough for the others of
  // their part to start
  const UINT dispatched = 1 << 18, turns = 1 << 16, words = 3;
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER parameters[3] = {{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}, {D3D12_ROOT_PARAMETER_TYPE_UAV},
                                        {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  parameters[0].Constants = {0, 0, 1};
  parameters[2].Descriptor = {1, 0};
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

  // the most groups of a dispatch of groups of `threads` that all see each other in: a part's
  auto part = [&](UINT threads) -> UINT {
    auto cs = compiler.compile(hlsl, "cs", "cs", {"THREADS=" + std::to_string(threads), "TURNS=" + std::to_string(turns)});
    ComPtr<ID3D12PipelineState> pso;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{rs.Get(), bytecode(cs)};
    if (!expect(!cs.empty() && device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)) == S_OK, "the pipeline"))
      return 0;
    const UINT groups = dispatched / threads;
    auto all_meet = [&](UINT goal) -> bool {
      list->CopyBufferRegion(meeting.Get(), 0, zeros.Get(), 0, bytes);
      transition(list.Get(), meeting.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      list->SetPipelineState(pso.Get());
      list->SetComputeRootSignature(rs.Get());
      list->SetComputeRoot32BitConstant(0, goal, 0);
      list->SetComputeRootUnorderedAccessView(1, meeting->GetGPUVirtualAddress());
      list->SetComputeRootUnorderedAccessView(2, shared->GetGPUVirtualAddress());
      list->Dispatch(groups, 1, 1);
      transition(list.Get(), meeting.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyResource(readback.Get(), meeting.Get());
      transition(list.Get(), meeting.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
      const UINT *got;
      if (!expect(SUCCEEDED(submit(device.Get(), queue.Get(), list.Get())) && SUCCEEDED(allocator->Reset()) &&
                      SUCCEEDED(list->Reset(allocator.Get(), nullptr)) &&
                      SUCCEEDED(readback->Map(0, nullptr, (void **)&got)),
                  "the dispatch did not run"))
        return false;
      bool all = got[0] == groups && got[1] == groups;
      expect(got[0] == groups, "%u of %u groups ran", got[0], groups);
      readback->Unmap(0, nullptr);
      return all;
    };
    // one group always meets itself: the most goal they all meet, between one that they meet and one they do not
    UINT met = 1, missed = groups + 1;
    expect(all_meet(met), "the groups did not all count themselves in");
    while (missed - met > 1) {
      UINT goal = met + (missed - met) / 2;
      (all_meet(goal) ? met : missed) = goal;
    }
    return met;
  };

  step("a part's threads, of kernels of two group sizes");
  const UINT small = 64, large = 256;
  UINT part_small = part(small), part_large = part(large);
  printf("parts of %u groups of %u threads, %u of %u\n", part_small, small, part_large, large);
  expect(part_small < dispatched / small, "a dispatch of %u threads was not in parts", dispatched);
  expect(part_small * small == part_large * large, "a part held %u threads of groups of %u, %u of groups of %u",
         part_small * small, small, part_large * large, large);
  return verdict();
}
