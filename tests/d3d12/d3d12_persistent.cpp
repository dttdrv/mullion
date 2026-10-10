// contract: a compute kernel whose threadgroups stay in a loop and feed each other through a queue in a buffer runs
// to its end and visits every node of a tree once, in a time a GPU has for a pass. this is the shape of Unreal's
// Nanite culling ("persistent threads": Karis, "Nanite, a deep dive", SIGGRAPH 2021), built from what Direct3D
// defines and nothing of Unreal's:
// - a group of GROUP threads holds a batch of GROUP / FANOUT queue slots, FANOUT threads a slot, one for each child
//   of the slot's node. the group's first thread claims the batch with InterlockedAdd on the queue's read counter;
// - a slot whose node is not written yet is tried again in the next turn of the loop, so groups wait on each other:
//   only the root is in the queue at the start;
// - the children a wave of threads has are pushed with one InterlockedAdd of the wave's sum on the write counter by
//   its first lane (WaveIsFirstLane, WaveActiveSum, WavePrefixSum, WaveReadLaneFirst: HLSL "Wave Intrinsics"), and
//   written with plain stores. the queue and the counters are globallycoherent, which "causes memory barriers and
//   syncs to flush data across the entire GPU such that other groups can see writes" (HLSL, RWStructuredBuffer),
//   and every turn starts with AllMemoryBarrierWithGroupSync: a store of one turn is there for every group's loads
//   of a later one;
//   DXBC reserves each child's slot with its own atomic, since it has no wave operations;
// - GroupMemoryBarrierWithGroupSync ("blocks execution of all threads in a group until all group shared accesses
//   have been completed and all threads in the group have reached this call") stands between the turn's phases,
//   inside the loop, and the groupshared batch, node table and done mask (InterlockedOr) pass between them;
// - the loop ends when the count of pending nodes, kept with InterlockedAdd, is zero, which depends on other groups.
// HLSL asks a barrier of the group that stores, not of the group that loads, so the kernel runs with its device
// barrier in each PLACEMENT: 0 at the start of every turn, before the turn's loads; 1 once a turn after its loads;
// 2 only in the threads that stored, after their stores, so that a group that waits passes none. two more are
// not run unless asked for (PLACEMENT=n): 3 has no device barrier at all, and 4 has as many barriers a turn as
// Unreal's kernel: two with device and group memory and sync, two with device memory and sync, one of device memory
// alone, six of group memory with sync.
// a node without children is a leaf, and a leaf is appended to an output: its place comes from an InterlockedAdd on
// the output's first word, as a culling kernel hands out the places of what it found visible. every leaf is there
// once.
// the tree is the complete FANOUT-ary one of NODES nodes in heap order: node n has the children n * FANOUT + 1 and
// on that are less than NODES, so every node has one parent and the reference is a count of one a node.
// what is held to: every node visited once, nothing pending, and the dispatch done within SECONDS, for each number
// of groups (1440 is what a 1440p view's culling dispatches, 4096 more than a GPU runs at a time). a group that
// waits turns its loop far faster than a group that works, so a count of turns says nothing about time: ITERATIONS
// only keeps a kernel that never ends from keeping the GPU, and is far more turns than SECONDS has.
// when a run fails, what each group saw says which shared value did not reach it:
// - lag: every turn a group's first thread loads the count of all turns with a plain load, then adds its own turn
//   with InterlockedAdd, which returns the count as it is. the difference is how far behind the plain load was;
// - at its end a group loads the pending count and each of its slots both ways. a plain load that is not the
//   atomic one, in a group that is still waiting, is the value the group did not see.
// a GPU keeps only so many threadgroups of a kernel running side by side, fewer the more each thread has to keep
// across a barrier, and takes turns among the rest: a group that waits for one other group then waits a whole round.
// HEAVY=n gives every thread n words of its own that live across the loop's barriers (and are summed at the end, so
// they have to be kept, and the sum is checked); OWNID=1 has a group take its number from a counter in place of
// SV_GroupID, as a kernel does that a translator may dispatch in parts. TICKS_MOST ends a kernel that has had that
// many turns over all its groups, which bounds it where a turn is slow, and MS is the time it may take in place of
// SECONDS.
// argv[2] and on are NAME=VALUE defines in place of the defaults, for finding what a slow run is slow at.
#include "d3d12_test.hpp"
#include <algorithm>
#include <map>

static const char hlsl[] = R"hlsl(
#define EMPTY 0xffffffffu
#define BATCH (GROUP / FANOUT)
// the queue and its counters: in a structured buffer, in a typed one (TYPED=1), which is a texture buffer to Metal,
// or in a 2D texture of ROW words a row (TYPED=2), as an engine keeps its page tables and visibility buffers
#if TYPED == 2
#define SHARED RWTexture2D<uint>
#define AT(buffer, i) buffer[uint2((i) % ROW, (i) / ROW)]
#else
#define AT(buffer, i) buffer[i]
#if TYPED
#define SHARED RWBuffer<uint>
#else
#define SHARED RWStructuredBuffer<uint>
#endif
#endif
globallycoherent SHARED state : register(u0);
globallycoherent SHARED queue : register(u1);
RWStructuredBuffer<uint> visits : register(u2);
// what a group saw: the count of all turns at its first and last turn, the nodes it found in its slots, its turns,
// the furthest a plain load of the count of all turns was behind, the pending count at its end by a plain load and
// by an atomic one, and how many of its slots the two loads disagreed about
struct Seen { uint first, last, found, turns, lag, pending, pending_now, unseen, worked, left, batch, again, heavy, half; };
RWStructuredBuffer<Seen> seen : register(u3);
// the leaves found: how many, then each
RWStructuredBuffer<uint> leaves_out : register(u4);
// HEAVY: the words every thread keeps
StructuredBuffer<uint> seeds : register(t0);
groupshared uint batch;        // the first slot of the group's batch
groupshared uint done;         // bit n: the batch's node n is done
groupshared uint finished;     // nothing is pending anywhere
groupshared uint late;         // the kernel has had its turns
groupshared uint me;           // OWNID: the group's number, from a counter
groupshared uint nodes[BATCH]; // the nodes of the batch's slots that are there to do, or EMPTY
groupshared uint candidates;   // ALLOCATE: the children the group's waves have this turn
groupshared uint offset;       // ALLOCATE: where they go in the queue

// what a place of the queue or the state holds: a plain load, or with LOADS=1 an atomic one that changes nothing,
// to tell a load that does not see another group's store from the rest
uint load(SHARED buffer, uint at) {
#if FENCE_BEFORE
  DeviceMemoryBarrier();
#endif
#if LOADS
  uint value;
  InterlockedOr(AT(buffer, at), 0, value);
  return value;
#else
  return AT(buffer, at);
#endif
}

// adds what the wave's lanes have to a counter at once, and gives each lane its place in what the counter was
uint claim(uint counter, uint has) {
#if WAVES
  uint place = WavePrefixSum(has), sum = WaveActiveSum(has), was = 0;
  if (WaveIsFirstLane() && sum) {
    InterlockedAdd(AT(state, counter), sum, was);
    // how often an add leaves the pending count at zero: once, the last node's, if every add is in the count
    if (counter == PENDING && was + sum == 0)
      InterlockedAdd(AT(state, ZEROS), 1);
  }
  return WaveReadLaneFirst(was) + place;
#else
  uint was = 0;
  if (has) {
    InterlockedAdd(AT(state, counter), has, was);
    if (counter == PENDING && was + has == 0)
      InterlockedAdd(AT(state, ZEROS), 1);
  }
  return was;
#endif
}

[numthreads(GROUP, 1, 1)]
#if OWNID
void cs(uint thread : SV_GroupIndex) {
  if (thread == 0)
    InterlockedAdd(AT(state, IDS), 1, me);
  GroupMemoryBarrierWithGroupSync();
  const uint3 group = me;
#else
void cs(uint thread : SV_GroupIndex, uint3 group : SV_GroupID) {
#endif
  const uint slot = thread / FANOUT, child = thread % FANOUT;
  Seen mine = (Seen)0;
#if HEAVY
  // words of the thread's own, each from a buffer: nothing but keeping them gives their sum at the end
  uint heavy[HEAVY];
  for (uint word = 0; word < HEAVY; word++)
    heavy[word] = seeds[thread * HEAVY + word];
#endif
  uint turn;
  for (turn = 0; turn < ITERATIONS; turn++) {
#if PLACEMENT == 0 || PLACEMENT == 4
    AllMemoryBarrierWithGroupSync();
#else
    GroupMemoryBarrierWithGroupSync();
#endif
    if (thread == 0) {
      candidates = 0;
      if (turn == 0 || done == (1u << BATCH) - 1) {
        InterlockedAdd(AT(state, READ), BATCH, batch);
        done = 0;
      }
      finished = load(state, PENDING) == 0;
      // a tick a turn of any group: groups whose spans of ticks overlap ran at one time
      uint ticks = AT(state, TICKS), tick;
      InterlockedAdd(AT(state, TICKS), 1, tick);
      mine.lag = max(mine.lag, tick - ticks);
      mine.first = turn ? mine.first : tick;
      mine.last = tick;
      // the turn count at which the group saw nothing pending, and left
      if (finished) {
        mine.left = tick + 1;
        mine.again = load(state, PENDING);
      }
      late = tick >= TICKS_MOST;
    }
    GroupMemoryBarrierWithGroupSync();
    if (finished || late)
      break;
    if (child == 0) {
      nodes[slot] = done >> slot & 1 ? EMPTY : load(queue, batch + slot);
      if (nodes[slot] != EMPTY)
        InterlockedAdd(seen[group.x].found, 1);
    }
#if PLACEMENT == 1 || PLACEMENT == 4
    AllMemoryBarrierWithGroupSync();
#else
    GroupMemoryBarrierWithGroupSync();
#endif
    uint node = nodes[slot], next = node * FANOUT + 1 + child;
    bool has = node != EMPTY && next < NODES;
#if WORK
    // what a culling kernel does for a child before it knows whether to push it: WORK steps of arithmetic, kept by
    // a word of the group's
    if (has) {
      uint x = next;
      for (uint step = 0; step < WORK; step++)
        x = x * 1664525 + 1013904223;
      InterlockedXor(seen[group.x].worked, x);
    }
#endif
#if ALLOCATE
    // places given out as Unreal's kernel gives them: each wave counts its children among the lanes that have one
    // and adds the count to a word of the group's, the group's first thread then takes the group's places from the
    // queue and counts the children pending, and after a barrier each child is stored at its place
    uint index = 0;
    if (has) {
#if WAVES
      uint count = WaveActiveCountBits(true), before = 0;
      if (WaveIsFirstLane())
        InterlockedAdd(candidates, count, before);
      index = WaveReadLaneFirst(before) + WavePrefixCountBits(true);
#else
      InterlockedAdd(candidates, 1, index);
#endif
    }
    GroupMemoryBarrierWithGroupSync();
    if (thread == 0) {
      InterlockedAdd(AT(state, WRITE), candidates, offset);
      InterlockedAdd(AT(state, PENDING), candidates);
    }
#if PLACEMENT == 4
    AllMemoryBarrierWithGroupSync();
#else
    GroupMemoryBarrierWithGroupSync();
#endif
    uint at = offset + index;
#else
    uint at = claim(WRITE, has);
    // a node is pending before it is in the queue, where another group may take it and count it done at once, and
    // a node's children are pending before it is not: the count is never zero while a node is left
    claim(PENDING, has);
#endif
    if (has) {
#if STORES
      uint was;
      InterlockedExchange(AT(queue, at), next, was);
#else
      AT(queue, at) = next;
#endif
    }
#if PLACEMENT == 2
    if (has)
      DeviceMemoryBarrier();
#endif
#if PLACEMENT == 4
    DeviceMemoryBarrierWithGroupSync();
#else
    GroupMemoryBarrierWithGroupSync();
#endif
    bool leaves = node != EMPTY && child == 0;
    if (leaves) {
      InterlockedAdd(visits[node], 1);
      InterlockedOr(done, 1u << slot);
      if (node * FANOUT + 1 >= NODES) {
#if ALLOCATE && WAVES
        // as Unreal's kernel gives out the places of what a wave found: each lane's count before it, the whole from
        // the last lane that has one, one add by the first
        uint before = WavePrefixSum(1u), was = 0;
        uint4 ballot = WaveActiveBallot(true);
        uint last = ballot.y ? 32 + firstbithigh(ballot.y) : firstbithigh(ballot.x), whole = WaveReadLaneAt(before + 1, last);
        if (WaveIsFirstLane())
          InterlockedAdd(leaves_out[0], whole, was);
        uint place = WaveReadLaneFirst(was) + before;
#else
        uint place;
        InterlockedAdd(leaves_out[0], 1, place);
#endif
        leaves_out[1 + place] = node;
      }
    }
    claim(PENDING, leaves ? -1 : 0);
#if PLACEMENT == 4
    DeviceMemoryBarrier();
    DeviceMemoryBarrierWithGroupSync();
    GroupMemoryBarrierWithGroupSync();
    GroupMemoryBarrierWithGroupSync();
    GroupMemoryBarrierWithGroupSync();
    GroupMemoryBarrierWithGroupSync();
    GroupMemoryBarrierWithGroupSync();
#endif
  }
  AllMemoryBarrierWithGroupSync();
  if (child == 0 && !(done >> slot & 1)) {
    uint now;
    InterlockedOr(AT(queue, batch + slot), 0, now);
    if (AT(queue, batch + slot) != now)
      InterlockedAdd(seen[group.x].unseen, 1);
  }
#if HEAVY
  uint kept = 0;
  for (uint word = 0; word < HEAVY; word++)
    kept += heavy[word];
  InterlockedAdd(seen[group.x].heavy, kept);
#endif
  // the turns the group's other half took: a barrier keeps all of a group's threads in one turn
  if (thread == GROUP / 2)
    seen[group.x].half = late ? ITERATIONS : turn;
  if (thread == 0) {
    mine.turns = late ? ITERATIONS : turn;
    mine.pending = AT(state, PENDING);
    InterlockedOr(AT(state, PENDING), 0, mine.pending_now);
    seen[group.x].first = mine.first, seen[group.x].last = mine.last, seen[group.x].turns = mine.turns;
    seen[group.x].lag = mine.lag, seen[group.x].pending = mine.pending, seen[group.x].pending_now = mine.pending_now;
    seen[group.x].left = mine.left, seen[group.x].batch = batch, seen[group.x].again = mine.again;
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
  enum { Read, Write, Pending, Ticks, Zeros, Ids, Seed, States };
  struct Seen {
    UINT first, last, found, turns, lag, pending, pending_now, unseen, worked, left, batch, again, heavy, half;
  };
  const UINT EMPTY = ~0u;
  // the kernel's numbers: groups of 64 threads, four children a node, a tree of millions of nodes, a second for the
  // pass, and a bound on a group's turns that a waiting group needs some tenths of a second to reach. GROUPS=0 runs
  // each of the dispatches below
  std::map<std::string, UINT> numbers{{"GROUP", 64}, {"FANOUT", 4}, {"NODES", 1 << 22}, {"GROUPS", 0},
                                      {"ITERATIONS", 1 << 17}, {"LOADS", 0}, {"STORES", 0}, {"FENCE_BEFORE", 0}, {"SECONDS", 1},
                                      {"TYPED", 0}, {"REPEATS", 1}, {"WORK", 0}, {"ALLOCATE", 0}, {"HEAVY", 0}, {"OWNID", 0},
                                      {"TICKS_MOST", ~0u}, {"MS", 0}};
  // PLACEMENT=n runs that one placement of the device barrier
  std::vector<UINT> placements{0, 1, 2};
  for (int i = 2; i < argc; i++)
    if (auto eq = strchr(argv[i], '=')) {
      numbers[std::string(argv[i], eq)] = strtoul(eq + 1, nullptr, 0);
      if (std::string(argv[i], eq) == "PLACEMENT")
        placements = {numbers["PLACEMENT"]};
    }
  numbers.erase("PLACEMENT");
  const UINT group = numbers["GROUP"], fanout = numbers["FANOUT"], node_count = numbers["NODES"], turns = numbers["ITERATIONS"];
  const UINT row = 4096;
  std::vector<std::string> defines{"WAVES=" + std::to_string(bool(compiler.dxc)), "ROW=" + std::to_string(row),
                                   "READ=" + std::to_string(Read), "WRITE=" + std::to_string(Write), "PENDING=" + std::to_string(Pending),
                                   "TICKS=" + std::to_string(Ticks), "ZEROS=" + std::to_string(Zeros), "IDS=" + std::to_string(Ids), "SEED=" + std::to_string(Seed)};
  for (auto &[name, value] : numbers)
    defines.push_back(name + "=" + std::to_string(value));
  std::vector<std::string> shaders;
  for (UINT placement : placements) {
    auto placed = defines;
    placed.push_back("PLACEMENT=" + std::to_string(placement));
    shaders.push_back(compiler.compile(hlsl, "cs", "cs", placed));
    if (shaders.back().empty()) {
      printf("failed: HLSL did not compile\n");
      return 1;
    }
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  if (compiler.dxc) {
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
    CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1)));
    if (!options1.WaveOps) {
      printf("skipped: no wave operations\n");
      return 77;
    }
  }
  enum { State, Queue, Visits, Saw, Leaves, Buffers };
  // the buffers by their addresses, or, typed ones having none (a root descriptor is a raw or structured buffer:
  // D3D12_ROOT_PARAMETER_TYPE_UAV), all four in a table
  const UINT typed = numbers["TYPED"];
  const bool textured = typed == 2;
  D3D12_ROOT_PARAMETER parameters[Buffers];
  for (UINT i = 0; i < Buffers; i++) {
    parameters[i] = {D3D12_ROOT_PARAMETER_TYPE_UAV};
    parameters[i].Descriptor = {i, 0};
  }
  const D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, Buffers};
  // the threads' words by their address, after the views: parameter Buffers, or 1 beside the table
  D3D12_ROOT_PARAMETER with_seeds[Buffers + 1], table[2] = {{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}, {D3D12_ROOT_PARAMETER_TYPE_SRV}};
  std::copy_n(parameters, Buffers, with_seeds);
  with_seeds[Buffers] = {D3D12_ROOT_PARAMETER_TYPE_SRV};
  table[0].DescriptorTable = {1, &range};
  auto rs = typed ? root_signature(device.Get(), {2, table}) : root_signature(device.Get(), {Buffers + 1, with_seeds});
  const UINT heavy = numbers["HEAVY"], seed_count = group * std::max(heavy, 1u);
  auto seeds = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, seed_count * sizeof(UINT), D3D12_RESOURCE_STATE_GENERIC_READ);
  UINT *seed_words, kept = 0;
  CHECK(seeds->Map(0, nullptr, (void **)&seed_words));
  for (UINT i = 0, value = 1; i < seed_count; i++)
    kept += seed_words[i] = value = value * 1664525 + 1013904223;
  ComPtr<ID3D12DescriptorHeap> views;
  D3D12_DESCRIPTOR_HEAP_DESC views_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, Buffers, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&views_desc, IID_PPV_ARGS(&views)));

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));

  // REPEATS=n runs each dispatch n times, for a time that is not the first dispatch's
  std::vector<UINT> dispatches;
  for (UINT groups : numbers["GROUPS"] ? std::vector<UINT>{numbers["GROUPS"]} : std::vector<UINT>{1440, 4096})
    dispatches.insert(dispatches.end(), numbers["REPEATS"], groups);
  // a leaf has no child inside the tree
  const UINT first_leaf = (node_count + fanout - 2) / fanout, leaf_count = node_count - first_leaf;
  for (size_t variant = 0; variant < placements.size(); variant++)
  for (UINT groups : dispatches) {
    const UINT placement = placements[variant];
    step("placement %u: %u groups of %u threads through a tree of %u nodes, %u children a node", placement, groups, group,
         node_count, fanout);
    ComPtr<ID3D12PipelineState> pso;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{rs.Get(), bytecode(shaders[variant])};
    CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)));
    // a batch beyond the tree's last slot is claimed by a group at most once, and never done
    const UINT slots = node_count + (groups + 1) * (group / fanout);
    // the state, the queue, the visits and what the groups saw, one after another in the buffers that fill and read them
    // a texture's words fill whole rows, and each part starts where a texture's data may (D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT)
    auto words = [&](UINT64 count) { return (textured ? (count + row - 1) / row * row : count) * sizeof(UINT); };
    const UINT64 sizes[Buffers] = {words(States), words(slots), UINT64(node_count) * sizeof(UINT), UINT64(groups) * sizeof(Seen),
                                   UINT64(leaf_count + 1) * sizeof(UINT)};
    UINT64 offsets[Buffers], total = 0;
    for (UINT i = 0; i < Buffers; total = offsets[i] + sizes[i], i++)
      offsets[i] = (total + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~UINT64(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
    auto footprint = [&](UINT i) {
      return D3D12_PLACED_SUBRESOURCE_FOOTPRINT{offsets[i], {DXGI_FORMAT_R32_UINT, row, UINT(sizes[i] / sizeof(UINT) / row), 1, UINT(row * sizeof(UINT))}};
    };
    ComPtr<ID3D12Resource> buffers[Buffers];
    auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
    char *start;
    CHECK(upload->Map(0, nullptr, (void **)&start));
    // the root is in the queue's first slot and pending; no other slot has a node, and no node a visit
    const UINT state[States] = {0, 1, 1};
    memset(start, 0, total);
    memcpy(start + offsets[State], state, sizeof(state));
    memset(start + offsets[Queue], 0xff, sizes[Queue]);
    memset(start + offsets[Queue], 0, sizeof(UINT));
    for (UINT i = 0; i < Buffers; i++) {
      if (textured && i < Visits) {
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, row, footprint(i).Footprint.Height, 1, 1, DXGI_FORMAT_R32_UINT,
                                 {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
        CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&buffers[i])));
        D3D12_TEXTURE_COPY_LOCATION to{buffers[i].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
            from{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint(i)}};
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
      } else {
        buffers[i] = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, sizes[i], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        list->CopyBufferRegion(buffers[i].Get(), 0, upload.Get(), offsets[i], sizes[i]);
      }
      transition(list.Get(), buffers[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), pso.Get()));
    list->SetComputeRootSignature(rs.Get());
    for (UINT i = 0; i < Buffers; i++) {
      // the state and the queue as buffers of R32_UINT, the others as structures
      D3D12_UNORDERED_ACCESS_VIEW_DESC view{i < Visits ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_UNKNOWN, D3D12_UAV_DIMENSION_BUFFER};
      const UINT stride = i == Saw ? sizeof(Seen) : sizeof(UINT);
      view.Buffer = {0, UINT(sizes[i] / stride), i < Visits ? 0 : stride};
      if (textured && i < Visits) {
        view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        view.Texture2D = {0, 0};
      }
      auto at = views->GetCPUDescriptorHandleForHeapStart();
      at.ptr += i * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      if (typed)
        device->CreateUnorderedAccessView(buffers[i].Get(), nullptr, &view, at);
      else
        list->SetComputeRootUnorderedAccessView(i, buffers[i]->GetGPUVirtualAddress());
    }
    if (typed) {
      ID3D12DescriptorHeap *heaps[] = {views.Get()};
      list->SetDescriptorHeaps(1, heaps);
      list->SetComputeRootDescriptorTable(0, views->GetGPUDescriptorHandleForHeapStart());
    }
    list->SetComputeRootShaderResourceView(typed ? 1 : Buffers, seeds->GetGPUVirtualAddress());
    list->Dispatch(groups, 1, 1);
    CHECK(list->Close());
    LARGE_INTEGER before, after, frequency;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&before);
    CHECK(execute(device.Get(), queue.Get(), list.Get()));
    QueryPerformanceCounter(&after);
    double seconds = double(after.QuadPart - before.QuadPart) / frequency.QuadPart;

    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), nullptr));
    for (UINT i = 0; i < Buffers; i++) {
      transition(list.Get(), buffers[i].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      if (textured && i < Visits) {
        D3D12_TEXTURE_COPY_LOCATION from{buffers[i].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
            to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint(i)}};
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
      } else {
        list->CopyBufferRegion(readback.Get(), offsets[i], buffers[i].Get(), 0, sizes[i]);
      }
    }
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    const char *read;
    CHECK(readback->Map(0, nullptr, (void **)&read));
    auto got = (const UINT *)(read + offsets[State]);
    const unsigned wrong_before = trace::wrong;
    const double limit = numbers["MS"] ? numbers["MS"] / 1000.0 : numbers["SECONDS"];
    expect(seconds <= limit, "the dispatch took %.3f s, more than %g s", seconds, limit);
    expect(got[Pending] == 0 && got[Write] == node_count, "%u nodes pending and %u pushed of %u", got[Pending], got[Write], node_count);
    auto visits = (const UINT *)(read + offsets[Visits]);
    UINT unvisited = std::count(visits, visits + node_count, 0u), once = std::count(visits, visits + node_count, 1u);
    expect(once == node_count, "%u of %u nodes visited once, %u never, %u more than once", once, node_count, unvisited,
           node_count - once - unvisited);
    auto saw = (const Seen *)(read + offsets[Saw]);
    // the output: as many as there are leaves, each leaf once
    auto appended = (const UINT *)(read + offsets[Leaves]);
    std::vector<bool> there(node_count);
    UINT strangers = 0;
    for (UINT i = 0; i < std::min(appended[0], leaf_count); i++) {
      UINT leaf = appended[1 + i];
      if (leaf < first_leaf || leaf >= node_count || there[leaf])
        strangers++;
      else
        there[leaf] = true;
    }
    expect(appended[0] == leaf_count && !strangers, "%u leaves appended of %u, %u of them not a leaf or there twice", appended[0],
           leaf_count, strangers);
    UINT waiting = 0, behind = 0, unseen = 0, blind = 0, longest = 0, lag = 0, lagging = 0;
    std::vector<int> change(got[Ticks] + 2);
    for (UINT g = 0; g < groups; g++) {
      auto &s = saw[g];
      bool gave_up = s.turns == turns;
      waiting += gave_up, blind += !s.found, longest = std::max(longest, s.turns), lag = std::max(lag, s.lag);
      // a plain load may miss what another group adds while it is made: one turn of every group, not more
      lagging += s.lag > groups;
      behind += s.pending != s.pending_now, unseen += s.unseen != 0;
      change[s.first]++, change[s.last + 1]--;
      if (gave_up && (s.pending != s.pending_now || s.unseen) && behind + unseen <= 4)
        printf("group %u, still waiting after %u turns: its plain load of the pending count gives %u, its atomic one %u; "
               "%u of its slots hold a node that its plain load does not show\n", g, s.turns, s.pending, s.pending_now, s.unseen);
    }
    expect(!waiting, "%u groups were still in the loop after %u turns, or when the kernel had had its %u", waiting, turns,
           numbers["TICKS_MOST"]);
    UINT apart = 0;
    for (UINT g = 0; g < groups; g++)
      apart += saw[g].half != saw[g].turns;
    expect(!apart, "in %u groups thread %u left the loop at another turn than thread 0", apart, group / 2);
    // what a thread kept across the barriers: the sum of its words, summed over a group's threads
    UINT forgetful = 0;
    for (UINT g = 0; g < groups; g++)
      forgetful += heavy && saw[g].heavy != kept;
    expect(!forgetful, "%u groups' threads did not keep their %u words across the loop", forgetful, heavy);
    if (trace::wrong != wrong_before) {
      // where it went wrong: a slot the write counter gave out and no child was stored in, a node stored twice, or a
      // node that is in the queue and was never taken from it
      auto queued = (const UINT *)(read + offsets[Queue]);
      std::vector<UINT> times(node_count);
      UINT empty = 0, twice = 0, untaken = 0, first_empty = EMPTY, first_untaken = EMPTY;
      for (UINT slot = 0; slot < std::min(got[Write], slots); slot++) {
        if (queued[slot] >= node_count) {
          if (!empty++)
            first_empty = slot;
        } else if (times[queued[slot]]++) {
          twice++;
        } else if (!visits[queued[slot]] && !untaken++) {
          first_untaken = slot;
        }
      }
      printf("of the %u slots given out, %u hold no node (the first is slot %u), %u hold a node another slot has, and %u hold "
             "a node that was never visited (the first is slot %u); the read counter is at %u\n",
             got[Write], empty, first_empty, twice, untaken, first_untaken, got[Read]);
      // the groups that left before the bound, and the group whose batch has the first node nobody took
      UINT early = 0, saw_zero = 0, no_turn = 0, earliest = ~0u, latest = 0, not_again = 0;
      for (UINT g = 0; g < groups; g++)
        if (saw[g].turns != turns) {
          early++, saw_zero += saw[g].left != 0, no_turn += !saw[g].turns, not_again += saw[g].left && saw[g].again;
          earliest = std::min(earliest, saw[g].last), latest = std::max(latest, saw[g].last);
        }
      printf("%u groups left before the bound: %u of them had seen the pending count at zero, %u took no turn; their last "
             "turns were at counts %u to %u of %u\n", early, saw_zero, no_turn, earliest, latest, got[Ticks]);
      printf("an add left the pending count at zero %u times; %u of the groups that saw zero loaded it again at once and it was "
             "not zero\n", got[Zeros], not_again);
      for (UINT g = 0; g < groups && first_untaken != EMPTY; g++)
        if (saw[g].batch <= first_untaken && first_untaken < saw[g].batch + group / fanout)
          printf("slot %u is in the batch of group %u, which took %u turns, the last at count %u, found %u nodes, and %s\n",
                 first_untaken, g, saw[g].turns, saw[g].last, saw[g].found,
                 saw[g].left ? "left when it saw the pending count at zero" : saw[g].turns == turns ? "was at the bound" : "left otherwise");
    }
    // how many groups were in the loop at one time: the most spans of ticks that hold one tick
    int together = 0, most = 0;
    for (int c : change)
      most = std::max(most, together += c);
    printf("placement %u, %u groups: the dispatch took %.5f s; the group that stayed longest took %u turns, all groups %u; at "
           "most %d groups were in the loop at one time, and %u never found a node in their slots\n", placement, groups,
           seconds, longest, got[Ticks], most, blind);
    printf("%u groups: a plain load of the count of all turns was at most %u behind, in %u groups more than %u behind; at "
           "their end %u groups' plain load of the pending count was not the atomic one, and %u groups had a slot whose "
           "plain load was not the atomic one\n", groups, lag, lagging, groups, behind, unseen);
    readback->Unmap(0, nullptr);
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), nullptr));
  }
  return verdict();
}
