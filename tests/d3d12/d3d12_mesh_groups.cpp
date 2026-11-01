// contract: DispatchMesh runs every group of its grid once, with its own SV_GroupID, up to the largest grid D3D12
// allows: "ThreadGroupCountX*ThreadGroupCountY*ThreadGroupCountZ must not exceed 2^22" (D3D12 Mesh Shader
// specification, "DispatchMesh API"), more than one Metal mesh grid holds; an amplification shader's groups are
// launched the same way. what an amplification shader sends is "the same input values for the entire Mesh shader
// thread group" ("Amplification shader output payload"): every byte its group wrote, up to the 16 KB a payload may
// have, after "an implied GroupMemoryBarrierWithGroupSync()" ("DispatchMesh intrinsic"), whether its threads wrote
// the payload's fields apart, read each other's through a barrier of their own, or added to them atomically. a mesh
// shader's group outputs the counts of "the first active thread" ("SetMeshOutputCounts"), whatever the others pass,
// with or without cull distances.
// grids and payloads are checked through a buffer each group writes to; output counts through the pixels a group's
// triangles cover, the rest of the target untouched.
#include "d3d12_test.hpp"
#include <map>
#include <vector>

static const char hlsl[] = R"hlsl(
RWByteAddressBuffer results : register(u0);
cbuffer Constants : register(b0) { uint3 grid; uint region; };

// a group of a grid: its bit in the region's bitmap, and the count of groups that ran; a group past the grid is wrong
void mark(uint3 g) {
  uint at = region * REGION;
  if (any(g >= grid)) {
    results.InterlockedAdd(at + 4, 1);
    return;
  }
  uint id = (g.z * grid.y + g.y) * grid.x + g.x;
  results.InterlockedAdd(at, 1);
  results.InterlockedOr(at + 8 + id / 32 * 4, 1u << (id % 32));
}
struct Nothing { float4 pos : SV_Position; };
[numthreads(1, 1, 1)] [outputtopology("triangle")]
void ms_grid(uint3 g : SV_GroupID, out vertices Nothing v[3], out indices uint3 i[1]) {
  SetMeshOutputCounts(0, 0);
  mark(g);
}
// the same through an amplification shader whose group launches one mesh group, which gets the group's ID
struct Group { uint3 id; };
groupshared Group sent;
[numthreads(1, 1, 1)] void as_grid(uint3 g : SV_GroupID) {
  sent.id = g;
  DispatchMesh(1, 1, 1, sent);
}
[numthreads(1, 1, 1)] [outputtopology("triangle")]
void ms_sent(in payload Group p, out vertices Nothing v[3], out indices uint3 i[1]) {
  SetMeshOutputCounts(0, 0);
  mark(p.id);
}

// meshlet culling: amplification group a's thread t keeps meshlet 32a + t where a hash says so, compacted into the
// payload with wave operations; the threads fill the rest of a 16 KB payload apart. the mesh group of each kept
// meshlet writes its number plus one where every value it checks is the one the amplification group wrote
struct Compact { uint ids[32]; float4 extra[EXTRA]; };
groupshared Compact compact;
bool kept(uint id) { return (id * 2654435761u >> 28) < 6; }
float4 extra(uint a, uint e) { return float4(e, a, e * 3 + a, 7); }
[numthreads(32, 1, 1)] void as_compact(uint t : SV_GroupThreadID, uint a : SV_GroupID) {
  uint id = a * 32 + t;
  bool keep = kept(id);
  if (keep)
    compact.ids[WavePrefixCountBits(keep)] = id;
  for (uint e = t; e < EXTRA; e += 32)
    compact.extra[e] = extra(a, e);
  DispatchMesh(WaveActiveCountBits(keep), 1, 1, compact);
}
[numthreads(32, 1, 1)] [outputtopology("triangle")]
void ms_compact(uint t : SV_GroupThreadID, uint g : SV_GroupID, in payload Compact p, out vertices Nothing v[3],
                out indices uint3 i[1]) {
  SetMeshOutputCounts(0, 0);
  uint id = p.ids[g], a = id / 32;
  bool right = kept(id);
  for (uint e = t; e < EXTRA; e += 32)
    right = right && all(p.extra[e] == extra(a, e));
  if (!right)
    results.InterlockedAdd(region * REGION + 4, 1);
  if (t == 0)
    results.Store(region * REGION + 8 + id * 4, id + 1);
}

// threads of both SIMD-groups write values, then read others' through a barrier of their own
struct Exchange { uint values[64]; uint sums[64]; };
groupshared Exchange exchange;
[numthreads(64, 1, 1)] void as_exchange(uint t : SV_GroupThreadID, uint a : SV_GroupID) {
  exchange.values[t] = 3 * t + a;
  GroupMemoryBarrierWithGroupSync();
  exchange.sums[t] = exchange.values[t] + exchange.values[63 - t];
  DispatchMesh(1, 1, 1, exchange);
}
[numthreads(64, 1, 1)] [outputtopology("triangle")]
void ms_exchange(uint t : SV_GroupThreadID, in payload Exchange p, out vertices Nothing v[3], out indices uint3 i[1]) {
  SetMeshOutputCounts(0, 0);
  uint a = p.values[0];
  if (p.values[t] != 3 * t + a || p.sums[t] != 3 * 63 + 2 * a)
    results.InterlockedAdd(region * REGION + 4, 1);
  if (t == 0)
    results.Store(region * REGION + 8 + a * 4, a + 1);
}

// threads add to the payload atomically
struct Counted { uint count; uint group; };
groupshared Counted counted;
[numthreads(64, 1, 1)] void as_atomic(uint t : SV_GroupThreadID, uint a : SV_GroupID) {
  if (t == 0) {
    counted.count = 0;
    counted.group = a;
  }
  GroupMemoryBarrierWithGroupSync();
  InterlockedAdd(counted.count, t + 1);
  DispatchMesh(1, 1, 1, counted);
}
[numthreads(1, 1, 1)] [outputtopology("triangle")]
void ms_atomic(in payload Counted p, out vertices Nothing v[3], out indices uint3 i[1]) {
  SetMeshOutputCounts(0, 0);
  if (p.count != 64 * 65 / 2)
    results.InterlockedAdd(region * REGION + 4, 1);
  results.Store(region * REGION + 8 + p.group * 4, p.group + 1);
}

// the output counts of the first thread of 64, one triangle more than every other thread's: triangle n of group g
// covers pixel (g, ROW + 2n) only, and the group writes the COUNTED triangles thread 0 asks for
float4 corner(uint x, uint y, uint c) {
  float2 at = float2(x, y) + float2(c == 1 ? 1.4 : 0, c == 2 ? 1.4 : 0);
  return float4(at / SIZE * float2(2, -2) + float2(-1, 1), 0, 1);
}
struct Value {
  float4 pos : SV_Position;
  nointerpolation uint value : VALUE;
#if DISTANCES
  float cull : SV_CullDistance;
#endif
};
[numthreads(64, 1, 1)] [outputtopology("triangle")]
void ms_counts(uint t : SV_GroupThreadID, uint g : SV_GroupID, out vertices Value v[3 * TRIANGLES],
               out indices uint3 i[TRIANGLES]) {
  uint n = t == 0 ? COUNTED : COUNTED - 1;
  SetMeshOutputCounts(3 * n, n);
  if (t < COUNTED) {
    for (uint c = 0; c < 3; c++) {
      v[3 * t + c].pos = corner(g, ROW + 2 * t, c);
      v[3 * t + c].value = 10 * g + t + 1;
#if DISTANCES
      v[3 * t + c].cull = 1;
#endif
    }
    i[t] = uint3(3 * t, 3 * t + 1, 3 * t + 2);
  }
}
uint ps_value(Value v) : SV_Target { return v.value; }
)hlsl";

template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename T> struct alignas(void *) Subobject {
  D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
  T data{};
};

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  // a region per case: the count of groups that ran, the count of wrong ones, then a bitmap of 2^22 groups or a word
  // per group
  const UINT region = 8 + (1u << 22) / 8, extra = 1016, size = 64, triangles = 3, groups = 4;
  struct Grid {
    const char *name;
    UINT x, y, z;
    bool amplified;
  };
  // past 2^20 - 1 groups, Apple9's largest mesh grid (an M4 runs none of a grid of 1024 x 1025): by rows, by rows of
  // each of several slices, by slices; and 65535 x 17 amplification groups, the widest rows D3D12 has
  const Grid grids[] = {
      {"rows", 1024, 1025, 1, false},
      {"rows of slices", 1024, 1024, 4, false},
      {"slices", 512, 512, 16, false},
      {"amplified", 65535, 17, 1, true},
  };
  const UINT compact_groups = 2048, exchange_groups = 300, atomic_groups = 300;
  const UINT regions = std::size(grids) + 3;
  std::vector<std::string> defines = {
      "REGION=" + std::to_string(region), "EXTRA=" + std::to_string(extra), "SIZE=" + std::to_string(size),
      "TRIANGLES=" + std::to_string(triangles), "ROW=0", "DISTANCES=0", "COUNTED=2",
  };
  auto compile = [&](const char *entry, const char *stage, std::vector<std::string> more = {}) {
    auto all = defines;
    for (auto &d : more)
      for (auto &e : all)
        if (e.substr(0, e.find('=')) == d.substr(0, d.find('=')))
          e = d;
    return compiler.compile(hlsl, entry, stage, all);
  };
  struct Case {
    const char *name;
    std::string as, ms, ps;
    ComPtr<ID3D12PipelineState> pso;
  };
  std::vector<Case> cases = {
      {"grid", {}, compile("ms_grid", "ms_6_5"), {}},
      {"amplified grid", compile("as_grid", "as_6_5"), compile("ms_sent", "ms_6_5"), {}},
      {"compact", compile("as_compact", "as_6_5"), compile("ms_compact", "ms_6_5"), {}},
      {"exchange", compile("as_exchange", "as_6_5"), compile("ms_exchange", "ms_6_5"), {}},
      {"atomic", compile("as_atomic", "as_6_5"), compile("ms_atomic", "ms_6_5"), {}},
  };
  // output counts: thread 0 asks for 2 of the 3 triangles, then 1, then with cull distances 2 again
  struct Counts {
    UINT counted, row;
    bool distances;
  } counts[] = {{2, 0, false}, {1, 8, false}, {2, 16, true}};
  for (auto &c : counts)
    cases.push_back(
        {"counts", {},
         compile("ms_counts", "ms_6_5",
                 {"COUNTED=" + std::to_string(c.counted), "ROW=" + std::to_string(c.row),
                  "DISTANCES=" + std::to_string(c.distances)}),
         compile("ps_value", "ps_6_5",
                 {"DISTANCES=" + std::to_string(c.distances)})}
    );
  for (auto &c : cases)
    if (c.ms.empty() || (c.name == std::string("counts") && c.ps.empty())) {
      printf(compiler.dxc ? "failed: HLSL did not compile\n" : "skipped: mesh shaders are shader model 6.5\n");
      return compiler.dxc ? 1 : 77;
    }

  ComPtr<ID3D12Device2> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS7 options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options, sizeof(options)));
  if (options.MeshShaderTier == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED) {
    printf("failed: mesh shaders are not supported\n");
    return 1;
  }
  D3D12_ROOT_PARAMETER parameters[2] = {{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  parameters[0].Constants = {0, 0, 4};
  parameters[1].Descriptor = {0, 0};
  auto rs = root_signature(device.Get(), {2, parameters});

  struct {
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> rs;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, D3D12_SHADER_BYTECODE> as;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> ms;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> raster;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC> depth;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> targets;
  } stream;
  stream.rs.data = rs.Get();
  stream.raster.data = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  stream.raster.data.DepthClipEnable = TRUE;
  stream.targets.data.NumRenderTargets = 1;
  stream.targets.data.RTFormats[0] = DXGI_FORMAT_R32_UINT;
  D3D12_PIPELINE_STATE_STREAM_DESC stream_desc{sizeof(stream), &stream};
  for (auto &c : cases) {
    stream.as.data = c.as.empty() ? D3D12_SHADER_BYTECODE{} : bytecode(c.as);
    stream.ms.data = bytecode(c.ms);
    stream.ps.data = c.ps.empty() ? D3D12_SHADER_BYTECODE{} : bytecode(c.ps);
    if (FAILED(device->CreatePipelineState(&stream_desc, IID_PPV_ARGS(&c.pso)))) {
      printf("failed: no pipeline for the %s case\n", c.name);
      return 1;
    }
  }

  const UINT64 bytes = UINT64(region) * regions;
  auto results = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COMMON,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto results_readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  auto zeros = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  void *mapped;
  CHECK(zeros->Map(0, nullptr, &mapped));
  memset(mapped, 0, bytes);
  zeros->Unmap(0, nullptr);
  CHECK(forget(results_readback.Get()));

  const UINT64 row = (size * sizeof(UINT) + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) /
                     D3D12_TEXTURE_DATA_PITCH_ALIGNMENT * D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
  ComPtr<ID3D12Resource> target;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  auto pixels_readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, row * size, D3D12_RESOURCE_STATE_COPY_DEST);
  CHECK(forget(pixels_readback.Get()));
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList6> commands;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)));
  transition(commands.Get(), results.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
  commands->CopyBufferRegion(results.Get(), 0, zeros.Get(), 0, bytes);
  transition(commands.Get(), results.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  const UINT untouched = ~0u;
  const float clear[4] = {(float)untouched, 0, 0, 0};
  commands->ClearRenderTargetView(rtv, clear, 0, nullptr);
  commands->SetGraphicsRootSignature(rs.Get());
  commands->SetGraphicsRootUnorderedAccessView(1, results->GetGPUVirtualAddress());
  D3D12_VIEWPORT viewport{0, 0, (float)size, (float)size, 0, 1};
  D3D12_RECT scissor{0, 0, (LONG)size, (LONG)size};
  commands->RSSetViewports(1, &viewport);
  commands->RSSetScissorRects(1, &scissor);
  commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  auto dispatch = [&](Case &c, UINT index, UINT x, UINT y, UINT z) {
    const UINT constants[4] = {x, y, z, index};
    commands->SetPipelineState(c.pso.Get());
    commands->SetGraphicsRoot32BitConstants(0, 4, constants, 0);
    commands->DispatchMesh(x, y, z);
  };
  for (UINT i = 0; i < std::size(grids); i++) {
    auto &g = grids[i];
    dispatch(cases[g.amplified ? 1 : 0], i, g.x, g.y, g.z);
  }
  const UINT compact_region = std::size(grids), exchange_region = compact_region + 1, atomic_region = compact_region + 2;
  dispatch(cases[2], compact_region, compact_groups, 1, 1);
  dispatch(cases[3], exchange_region, exchange_groups, 1, 1);
  dispatch(cases[4], atomic_region, atomic_groups, 1, 1);
  for (UINT k = 0; k < std::size(counts); k++)
    dispatch(cases[5 + k], 0, groups, 1, 1);
  transition(commands.Get(), results.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  commands->CopyBufferRegion(results_readback.Get(), 0, results.Get(), 0, bytes);
  transition(commands.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION dst{pixels_readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
      src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  dst.PlacedFootprint = {0, {DXGI_FORMAT_R32_UINT, size, size, 1, (UINT)row}};
  commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  if (!expect(SUCCEEDED(submit(device.Get(), queue.Get(), commands.Get())), "the list ran"))
    return verdict();

  const uint8_t *data;
  CHECK(results_readback->Map(0, nullptr, (void **)&data));
  auto word = [&](UINT r, UINT64 at) { return reinterpret_cast<const UINT *>(data + UINT64(region) * r)[at]; };
  for (UINT i = 0; i < std::size(grids); i++) {
    auto &g = grids[i];
    step("DispatchMesh(%u, %u, %u)%s", g.x, g.y, g.z, g.amplified ? " of amplification groups" : "");
    UINT total = g.x * g.y * g.z, missing = 0, first = ~0u;
    expect(word(i, 0) == total, "%u groups ran, want %u", word(i, 0), total);
    expect(word(i, 1) == 0, "%u groups past the grid", word(i, 1));
    for (UINT id = 0; id < (1u << 22); id++) {
      bool set = word(i, 2 + id / 32) >> (id % 32) & 1;
      if (set != (id < total)) {
        missing++;
        first = std::min(first, id);
      }
    }
    expect(!missing, "%u groups ran or not wrongly, the first %u", missing, first);
  }
  step("a 16 KB payload of compacted meshlets");
  UINT wrong = 0;
  for (UINT id = 0; id < compact_groups * 32; id++) {
    bool kept = (id * 2654435761u >> 28) < 6;
    wrong += word(compact_region, 2 + id) != (kept ? id + 1 : 0);
  }
  expect(!wrong, "%u meshlets wrong", wrong);
  expect(word(compact_region, 1) == 0, "%u mesh groups read a wrong payload", word(compact_region, 1));
  step("a payload whose threads read each other's values through a barrier");
  wrong = 0;
  for (UINT a = 0; a < exchange_groups; a++)
    wrong += word(exchange_region, 2 + a) != a + 1;
  expect(!wrong, "%u groups missing", wrong);
  expect(word(exchange_region, 1) == 0, "%u mesh threads read a wrong payload", word(exchange_region, 1));
  step("a payload added to atomically");
  wrong = 0;
  for (UINT a = 0; a < atomic_groups; a++)
    wrong += word(atomic_region, 2 + a) != a + 1;
  expect(!wrong, "%u groups missing", wrong);
  expect(word(atomic_region, 1) == 0, "%u mesh groups read a wrong count", word(atomic_region, 1));
  results_readback->Unmap(0, nullptr);

  std::map<std::pair<UINT, UINT>, UINT> want;
  for (auto &c : counts)
    for (UINT g = 0; g < groups; g++)
      for (UINT t = 0; t < c.counted; t++)
        want[{g, c.row + 2 * t}] = 10 * g + t + 1;
  CHECK(pixels_readback->Map(0, nullptr, (void **)&data));
  step("the first thread's output counts");
  for (UINT y = 0; y < size; y++)
    for (UINT x = 0; x < size; x++) {
      auto value = reinterpret_cast<const UINT *>(data + row * y)[x];
      auto it = want.find({x, y});
      auto expected = it == want.end() ? untouched : it->second;
      expect(value == expected, "pixel %u,%u: %u, want %u", x, y, value, expected);
    }
  return verdict();
}
