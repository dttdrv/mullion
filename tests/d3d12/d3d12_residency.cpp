// contract: a kernel whose threadgroups hand work to one another finishes in a time that follows its work, however
// many groups are dispatched. each group claims a place in a line (InterlockedAdd on a counter) and loops, with a
// group barrier each turn, until the item of its place is released; it then does the item, releases the next one
// and claims a new place. CHAIN items go down the line; a group that sees the last one done leaves. Direct3D runs
// such a kernel as a desktop GPU does: the groups that start first pass the items among themselves and the rest
// find nothing to do. this is the hand-over at the heart of Unreal's Nanite culling (a node slot or a batch of
// clusters is claimed by one group and waited for; Karis, "Nanite, a deep dive", SIGGRAPH 2021), reduced to it.
// every thread keeps LIVE words of its own across the loop's barriers (a large kernel has that much state), read
// from a buffer and changed every turn, and adds their sum to a count at the end.
// what is held to, for each number of groups: all CHAIN items are done, the kernel says so, and the dispatch takes
// no longer than MS. a GPU that keeps only some of the groups running side by side and takes turns among the rest
// makes every hand-over to a group that is not running wait a whole round, which is seconds here. TURNS bounds a
// group's loop: a kernel that reaches it has not done its items and fails.
// the line and its counters are globallycoherent, with no device barrier: a group barrier with sync is what flushes
// them for other groups ("causes memory barriers and syncs to flush data across the entire GPU", HLSL).
// argv[2] and on are NAME=VALUE in place of the defaults.
#include "d3d12_test.hpp"
#include <map>

static const char hlsl[] = R"hlsl(
globallycoherent RWStructuredBuffer<uint> state : register(u0);
globallycoherent RWStructuredBuffer<uint> released : register(u1);
StructuredBuffer<uint> seeds : register(t0);
groupshared uint place, ready, done;
[numthreads(GROUP, 1, 1)] void cs(uint thread : SV_GroupIndex) {
  uint live[LIVE];
  for (uint word = 0; word < LIVE; word++)
    live[word] = seeds[thread * LIVE + word];
  if (thread == 0)
    InterlockedAdd(state[CLAIMED], 1, place);
  for (uint turn = 0; turn < TURNS; turn++) {
    GroupMemoryBarrierWithGroupSync();
    if (thread == 0) {
      done = state[FINISHED];
      ready = place < CHAIN ? released[place] : 0;
    }
    GroupMemoryBarrierWithGroupSync();
    if (done)
      break;
    live[(turn + thread) % LIVE] += ready + turn;
    if (ready && thread == 0) {
      uint before;
      InterlockedAdd(state[DONE], 1, before);
      if (before + 1 < CHAIN)
        released[before + 1] = 1;
      else
        state[FINISHED] = 1;
      InterlockedAdd(state[CLAIMED], 1, place);
    }
  }
  uint sum = 0;
  for (uint kept = 0; kept < LIVE; kept++)
    sum += live[kept];
  InterlockedAdd(state[SUM], sum);
  if (thread == 0)
    InterlockedAdd(state[ENDED], 1);
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  enum { Claimed, Done, Finished, Sum, Ended, States };
  // groups of 64 threads with 64 words each, two thousand items, a quarter of a second, and a bound on a group's
  // turns that a kernel in trouble reaches in a few seconds. GROUPS=n runs that one number of groups
  std::map<std::string, UINT> numbers{{"GROUP", 64}, {"LIVE", 64}, {"CHAIN", 2000}, {"TURNS", 1 << 18}, {"MS", 250}, {"GROUPS", 0}};
  for (int i = 2; i < argc; i++)
    if (auto eq = strchr(argv[i], '='))
      numbers[std::string(argv[i], eq)] = strtoul(eq + 1, nullptr, 0);
  const UINT group = numbers["GROUP"], live = numbers["LIVE"], chain = numbers["CHAIN"];
  std::vector<std::string> defines{"CLAIMED=" + std::to_string(Claimed), "DONE=" + std::to_string(Done), "FINISHED=" + std::to_string(Finished),
                                   "SUM=" + std::to_string(Sum), "ENDED=" + std::to_string(Ended)};
  for (auto &[name, value] : numbers)
    defines.push_back(name + "=" + std::to_string(value));
  auto cs = compiler.compile(hlsl, "cs", "cs", defines);
  if (cs.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER parameters[3] = {{D3D12_ROOT_PARAMETER_TYPE_UAV}, {D3D12_ROOT_PARAMETER_TYPE_UAV}, {D3D12_ROOT_PARAMETER_TYPE_SRV}};
  parameters[1].Descriptor = {1, 0};
  auto rs = root_signature(device.Get(), {(UINT)std::size(parameters), parameters});
  ComPtr<ID3D12PipelineState> pso;
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{rs.Get(), bytecode(cs)};
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)));
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));

  const UINT64 state_bytes = States * sizeof(UINT), line_bytes = chain * sizeof(UINT), seed_bytes = UINT64(group) * live * sizeof(UINT);
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, state_bytes + line_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto seeds = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, seed_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto state = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, state_bytes, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto line = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, line_bytes, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, state_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  UINT *start, *words;
  CHECK(upload->Map(0, nullptr, (void **)&start));
  CHECK(seeds->Map(0, nullptr, (void **)&words));
  // nothing claimed or done, and the first item released
  memset(start, 0, state_bytes + line_bytes);
  start[States] = 1;
  for (UINT i = 0, value = 1; i < group * live; i++)
    words[i] = value = value * 1664525 + 1013904223;

  // the first dispatch of a process carries the pipeline's first use: it is run and not timed
  std::vector<UINT> dispatches{64};
  for (UINT groups : numbers["GROUPS"] ? std::vector<UINT>{numbers["GROUPS"]} : std::vector<UINT>{64, 256, 448, 1440, 4096})
    dispatches.push_back(groups);
  for (size_t run = 0; run < dispatches.size(); run++) {
    const UINT groups = dispatches[run];
    step("%u items handed down a line by %u groups of %u threads that keep %u words each", chain, groups, group, live);
    list->CopyBufferRegion(state.Get(), 0, upload.Get(), 0, state_bytes);
    list->CopyBufferRegion(line.Get(), 0, upload.Get(), state_bytes, line_bytes);
    transition(list.Get(), state.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(list.Get(), line.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), pso.Get()));
    list->SetComputeRootSignature(rs.Get());
    list->SetComputeRootUnorderedAccessView(0, state->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(1, line->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(2, seeds->GetGPUVirtualAddress());
    list->Dispatch(groups, 1, 1);
    CHECK(list->Close());
    LARGE_INTEGER before, after, second;
    QueryPerformanceFrequency(&second);
    QueryPerformanceCounter(&before);
    CHECK(execute(device.Get(), queue.Get(), list.Get()));
    QueryPerformanceCounter(&after);
    const double ms = 1000.0 * (after.QuadPart - before.QuadPart) / second.QuadPart;

    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), nullptr));
    transition(list.Get(), state.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), 0, state.Get(), 0, state_bytes);
    transition(list.Get(), state.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    transition(list.Get(), line.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), nullptr));
    const UINT *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    if (run) {
      expect(got[Done] == chain && got[Finished] == 1, "%u of %u items were done, and the kernel says %s", got[Done], chain,
             got[Finished] ? "it is finished" : "it is not finished");
      expect(got[Ended] == groups, "%u of %u groups came to their end", got[Ended], groups);
      expect(ms <= numbers["MS"], "the dispatch took %.1f ms, more than %u ms", ms, numbers["MS"]);
      printf("%u groups: %.2f ms, %u items done, %u places claimed\n", groups, ms, got[Done], got[Claimed]);
    }
    readback->Unmap(0, nullptr);
  }
  return verdict();
}
