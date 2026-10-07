// contract: the Interlocked functions on groupshared memory are atomic among a threadgroup's threads ("performs a
// guaranteed atomic add", HLSL InterlockedAdd; groupshared is its "shared memory" case). Unreal's Nanite culling
// counts the nodes its group's threads take with InterlockedAdd and marks them with InterlockedOr in groupshared
// words, round after round; one lost step and a group goes round again for work it already has. every thread of
// every group, of two SIMD groups each, adds one ROUNDS times, sets its own bit, raises a maximum and lowers a
// minimum to hashes of its number, and takes one word by compare-exchange: one thread finds it free ("the original
// input value", InterlockedCompareExchange) and the others find that thread's number; thread 0 writes the words out
#include "d3d12_test.hpp"
#include <algorithm>
#include <cstdint>

static const char hlsl[] = R"hlsl(
RWStructuredBuffer<uint> results : register(u0);
groupshared uint count, bits[2], highest, lowest, winner, took, agreed;
uint value(uint g, uint i) { return (g * GROUP + i + 1) * MULTIPLIER; }
[numthreads(GROUP, 1, 1)]
void cs(uint i : SV_GroupIndex, uint3 group : SV_GroupID) {
  uint g = group.x;
  if (i == 0) {
    count = bits[0] = bits[1] = highest = winner = took = agreed = 0;
    lowest = ~0u;
  }
  GroupMemoryBarrierWithGroupSync();
  for (uint k = 0; k < ROUNDS; k++)
    InterlockedAdd(count, 1);
  InterlockedOr(bits[i / 32], 1u << (i % 32));
  InterlockedMax(highest, value(g, i));
  InterlockedMin(lowest, value(g, i));
  uint was;
  InterlockedCompareExchange(winner, 0, i + 1, was);
  InterlockedAdd(took, was == 0);
  GroupMemoryBarrierWithGroupSync();
  InterlockedAdd(agreed, winner == (was ? was : i + 1));
  GroupMemoryBarrierWithGroupSync();
  if (i == 0) {
    results[g * WORDS] = count;
    results[g * WORDS + 1] = bits[0];
    results[g * WORDS + 2] = bits[1];
    results[g * WORDS + 3] = highest;
    results[g * WORDS + 4] = lowest;
    results[g * WORDS + 5] = took;
    results[g * WORDS + 6] = agreed;
    results[g * WORDS + 7] = winner;
  }
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  // 64 threads are two SIMD groups of an Apple GPU, and 1440 groups are what Unreal dispatches
  const UINT group = 64, groups = 1440, rounds = 256, words = 8;
  const uint32_t multiplier = 0x9e3779b9;
  auto cs = compiler.compile(
      hlsl, "cs", "cs",
      {"GROUP=" + std::to_string(group), "ROUNDS=" + std::to_string(rounds), "WORDS=" + std::to_string(words),
       "MULTIPLIER=" + std::to_string(multiplier) + "u"}
  );
  if (cs.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  D3D12_ROOT_PARAMETER param{D3D12_ROOT_PARAMETER_TYPE_UAV};
  auto rs = root_signature(device.Get(), {1, &param});
  D3D12_COMPUTE_PIPELINE_STATE_DESC cs_desc{rs.Get(), bytecode(cs)};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateComputePipelineState(&cs_desc, IID_PPV_ARGS(&pso)));
  const UINT64 bytes = UINT64(groups) * words * sizeof(uint32_t);
  auto results = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  list->SetComputeRootSignature(rs.Get());
  list->SetComputeRootUnorderedAccessView(0, results->GetGPUVirtualAddress());
  list->Dispatch(groups, 1, 1);
  transition(list.Get(), results.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(), 0, results.Get(), 0, bytes);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  uint32_t *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));

  unsigned failures = 0;
  const char *names[] = {"the count",   "the first 32 bits",          "the other 32 bits",          "the maximum",
                         "the minimum", "the threads that took the word", "the threads that agree on it"};
  for (UINT g = 0; g < groups; g++) {
    uint32_t want[] = {group * rounds, ~0u, ~0u, 0, ~0u, 1, group};
    for (UINT i = 0; i < group; i++) {
      want[3] = std::max(want[3], (g * group + i + 1) * multiplier);
      want[4] = std::min(want[4], (g * group + i + 1) * multiplier);
    }
    for (UINT w = 0; w < std::size(want); w++)
      if (got[g * words + w] != want[w] && failures++ < 12)
        printf("group %u, %s: %#x, want %#x\n", g, names[w], got[g * words + w], want[w]);
    // one thread took the word: it holds that thread's number
    if ((got[g * words + 7] < 1 || got[g * words + 7] > group) && failures++ < 12)
      printf("group %u, the compare-exchange's word: %u, want a thread's number\n", g, got[g * words + 7]);
  }
  if (failures) {
    printf("failed: %u wrong results\n", failures);
    return 1;
  }
  printf("passed: %u groups of %u threads, %u adds each\n", groups, group, rounds);
  return 0;
}
