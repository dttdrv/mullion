// contract: what one thread of a group stores in groupshared memory, the group's other threads read after a barrier
// with group sync ("blocks execution of all threads in a group until all memory accesses have been completed and all
// threads in the group have reached this call", HLSL AllMemoryBarrierWithGroupSync), round after round, however the
// one thread's branch reads and writes groupshared words itself. Unreal's Nanite culling has a group's first thread
// publish the group's candidates so: it reserves their slots in a queue with InterlockedAdd, whose old value it
// stores in a groupshared word, then adds the groupshared count to the queue's count of pending work, in a branch
// the group's other threads do not take. a translator that puts a barrier of its own between the two, where the
// other threads of the SIMD group never come, stops the GPU.
// each round every thread counts itself, the first thread reserves the group's slots, and every thread marks its own
#include "d3d12_test.hpp"
#include <cstdint>

static const char hlsl[] = R"hlsl(
globallycoherent RWStructuredBuffer<uint> queue : register(u0);
globallycoherent RWStructuredBuffer<uint> slots : register(u1);
groupshared uint candidates, offset;
[numthreads(GROUP, 1, 1)]
void cs(uint i : SV_GroupIndex) {
  for (uint round = 0; round < ROUNDS; round++) {
    if (i == 0)
      candidates = 0;
    GroupMemoryBarrierWithGroupSync();
    InterlockedAdd(candidates, 1);
    GroupMemoryBarrierWithGroupSync();
    if (i == 0) {
      InterlockedAdd(queue[0], candidates, offset);
      InterlockedAdd(queue[1], candidates);
    }
    AllMemoryBarrierWithGroupSync();
    InterlockedAdd(slots[offset + i], 1);
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
  const UINT group = 64, groups = 1440, rounds = 16, slots = groups * group * rounds;
  auto cs = compiler.compile(hlsl, "cs", "cs", {"GROUP=" + std::to_string(group), "ROUNDS=" + std::to_string(rounds)});
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
  // the queue's two counts, then a mark for every slot, all zero
  const UINT64 bytes = UINT64(2 + slots) * sizeof(uint32_t);
  auto memory = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto zeros = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  void *mapped;
  CHECK(zeros->Map(0, nullptr, &mapped));
  memset(mapped, 0, bytes);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  list->CopyBufferRegion(memory.Get(), 0, zeros.Get(), 0, bytes);
  transition(list.Get(), memory.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  list->SetComputeRootSignature(rs.Get());
  list->SetComputeRootUnorderedAccessView(0, memory->GetGPUVirtualAddress());
  list->SetComputeRootUnorderedAccessView(1, memory->GetGPUVirtualAddress() + 2 * sizeof(uint32_t));
  list->Dispatch(groups, 1, 1);
  transition(list.Get(), memory.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(), 0, memory.Get(), 0, bytes);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  uint32_t *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));

  // every group reserved its threads' slots each round, and every slot was marked once
  unsigned failures = 0;
  for (UINT count = 0; count < 2; count++)
    if (got[count] != slots && failures++ < 8)
      printf("the queue's count %u: %u, want %u\n", count, got[count], slots);
  for (UINT slot = 0; slot < slots; slot++)
    if (got[2 + slot] != 1 && failures++ < 8)
      printf("slot %u was marked %u times\n", slot, got[2 + slot]);
  if (failures) {
    printf("failed: %u wrong results\n", failures);
    return 1;
  }
  printf("passed: %u groups of %u threads, %u rounds, %u slots\n", groups, group, rounds, slots);
  return 0;
}
