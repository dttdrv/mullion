// contract: wave intrinsics count the lanes that are active where they are called ("active lanes are those that are
// executing the same basic block", HLSL wave intrinsics; DXIL.rst "Wave Intrinsics"), also in a branch only some
// lanes take, inside a loop, between GroupMemoryBarrierWithGroupSync calls, in a threadgroup of more than one wave.
// Unreal's Nanite hands out queue slots so: each wave's first lane adds the wave's count of candidates to a
// groupshared count, and every candidate takes that count's old value from the first lane plus its place among the
// wave's candidates. a wrong first lane, count or place gives two candidates one slot, and the work of one is lost.
// here every round's candidates (a hash of group, thread and round) write their thread's number to the slot they
// get; a round's slots must hold each candidate once and the count their number
#include "d3d12_test.hpp"
#include <algorithm>
#include <cstdint>

static const char hlsl[] = R"hlsl(
RWStructuredBuffer<uint> slots : register(u0);
RWStructuredBuffer<uint> counts : register(u1);
groupshared uint count;
bool candidate(uint g, uint i, uint round) { return ((g * GROUP + i) * ROUNDS + round + 1) * MULTIPLIER >> 31; }
[numthreads(GROUP, 1, 1)]
void cs(uint i : SV_GroupIndex, uint3 group : SV_GroupID) {
  uint g = group.x;
  for (uint round = 0; round < ROUNDS; round++) {
    if (i == 0)
      count = 0;
    GroupMemoryBarrierWithGroupSync();
    uint slot = 0;
    if (candidate(g, i, round)) {
      uint candidates = WaveActiveCountBits(true), before = 0;
      if (WaveIsFirstLane())
        InterlockedAdd(count, candidates, before);
      slot = WaveReadLaneFirst(before) + WavePrefixCountBits(true);
    }
    GroupMemoryBarrierWithGroupSync();
    if (candidate(g, i, round))
      slots[(g * ROUNDS + round) * GROUP + slot] = i + 1;
    if (i == 0)
      counts[g * ROUNDS + round] = count;
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
  if (!compiler.dxc) {
    printf("skipped: wave intrinsics are DXIL only\n");
    return 77;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1)));
  if (!options1.WaveOps) {
    printf("skipped: no wave operations\n");
    return 77;
  }
  // a group of at least two waves, whatever the lane count
  const UINT group = std::clamp<UINT>(2 * options1.WaveLaneCountMax, 64, D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP),
             groups = 256, rounds = 32;
  const uint32_t multiplier = 0x9e3779b9;
  auto cs = compiler.compile(
      hlsl, "cs", "cs_6_0",
      {"GROUP=" + std::to_string(group), "ROUNDS=" + std::to_string(rounds), "MULTIPLIER=" + std::to_string(multiplier) + "u"}
  );
  if (cs.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_UAV}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[1].Descriptor.ShaderRegister = 1;
  auto rs = root_signature(device.Get(), {2, params});
  D3D12_COMPUTE_PIPELINE_STATE_DESC cs_desc{rs.Get(), bytecode(cs)};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateComputePipelineState(&cs_desc, IID_PPV_ARGS(&pso)));
  const UINT64 count_bytes = UINT64(groups) * rounds * sizeof(uint32_t), slot_bytes = count_bytes * group;
  auto slots = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, slot_bytes, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto counts = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, count_bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  // slots no candidate takes stay zero, which no thread writes
  auto zeros = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, slot_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  void *mapped;
  CHECK(zeros->Map(0, nullptr, &mapped));
  memset(mapped, 0, slot_bytes);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, slot_bytes + count_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  list->CopyBufferRegion(slots.Get(), 0, zeros.Get(), 0, slot_bytes);
  transition(list.Get(), slots.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  list->SetComputeRootSignature(rs.Get());
  list->SetComputeRootUnorderedAccessView(0, slots->GetGPUVirtualAddress());
  list->SetComputeRootUnorderedAccessView(1, counts->GetGPUVirtualAddress());
  list->Dispatch(groups, 1, 1);
  transition(list.Get(), slots.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(list.Get(), counts.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(), 0, slots.Get(), 0, slot_bytes);
  list->CopyBufferRegion(readback.Get(), slot_bytes, counts.Get(), 0, count_bytes);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  uint32_t *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  const uint32_t *counted = got + slot_bytes / sizeof(uint32_t);

  unsigned failures = 0;
  for (UINT g = 0; g < groups; g++)
    for (UINT round = 0; round < rounds; round++) {
      std::vector<uint32_t> want, taken(got + (g * rounds + round) * group, got + (g * rounds + round + 1) * group);
      for (UINT i = 0; i < group; i++)
        if (((g * group + i) * rounds + round + 1) * multiplier >> 31)
          want.push_back(i + 1);
      // the candidates' slots are the first ones, in any order
      std::sort(taken.begin(), taken.begin() + want.size());
      want.resize(group);
      if (counted[g * rounds + round] != std::count_if(want.begin(), want.end(), [](auto v) { return v; }) && failures++ < 6)
        printf("group %u, round %u: counted %u candidates\n", g, round, counted[g * rounds + round]);
      if (taken != want && failures++ < 6) {
        printf("group %u, round %u: slots", g, round);
        for (auto v : taken)
          printf(" %u", v);
        printf("\n");
      }
    }
  if (failures) {
    printf("failed: %u wrong results\n", failures);
    return 1;
  }
  printf("passed: %u groups of %u threads, %u rounds\n", groups, group, rounds);
  return 0;
}
