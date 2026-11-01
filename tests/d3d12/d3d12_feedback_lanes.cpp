// contract: sampler feedback (DirectX-Specs, Sampler Feedback) is what each sample wants, whatever the other lanes of
// its wave want: a map's region holds the mips every sample wanted there, and nothing that no sample wanted
// ("General interpretation and usage"; d3d12_feedback.cpp follows a sample alone). here the lanes of a wave sample
// close together, so many of them want the same regions, of different mips and slices:
//  - a compute shader of 8 by 8 threads samples an array with WriteSamplerFeedbackLevel, each lane at its own LOD and,
//    by pairs of rows, in the other slice, some lanes not at all and the rest one to three times, per sampler;
//  - samples near the corners of the array, alone: a border sampler's first texel of an axis is the border there, and
//    its second is in the same region as the first;
//  - lanes write two maps through NonUniformResourceIndex: each pair of lanes samples at one place, one lane into each
//    map, so each map holds what its own lanes wanted;
//  - a pixel shader writes WriteSamplerFeedback in a quad of which one pixel is covered. its other lanes are helper
//    lanes, which write nothing ("Helper lanes" in the spec: they take part in derivatives only), though they sample
//    in the same regions.
#include "d3d12_feedback.hpp"
#include <cfloat>

static const char hlsl[] = R"hlsl(
Texture2DArray<float4> paired : register(t0);
Texture2D<float4> flat : register(t1);
SamplerState s : register(s0);
FeedbackTexture2DArray<SAMPLER_FEEDBACK_MIN_MIP> min_mip : register(u0);
FeedbackTexture2DArray<SAMPLER_FEEDBACK_MIP_REGION_USED> used : register(u1);
FeedbackTexture2D<SAMPLER_FEEDBACK_MIP_REGION_USED> pair[2] : register(u2);
FeedbackTexture2D<SAMPLER_FEEDBACK_MIN_MIP> drawn : register(u4);

// a lane's place, slice and LOD; whether it samples at all, and how many times
float3 dense_coord(uint2 t) { return float3((t + 0.5) / float2(GRID_X, GRID_Y) * SPAN + ORIGIN, (t.y / 2) % SLICES); }
float dense_lod(uint2 t) { return (t.x % 8) * 0.5 + (t.y % 3) * 0.25; }
bool skips(uint2 t) { return (t.x * 7 + t.y * 3) % 5 == 0; }
uint times(uint2 t) { return t.x % 3 + 1; }
[numthreads(8, 8, 1)]
void cs_dense(uint3 id : SV_DispatchThreadID) {
  if (skips(id.xy))
    return;
  float3 c = dense_coord(id.xy);
  for (uint i = 0; i < times(id.xy); i++) {
    float3 at = float3(c.xy + i * HOP, c.z);
    min_mip.WriteSamplerFeedbackLevel(paired, s, at, dense_lod(id.xy));
    used.WriteSamplerFeedbackLevel(paired, s, at, dense_lod(id.xy));
  }
}

// near each corner of the array's first mip, 0.3 of a texel in: a linear filter's first texel along an axis is
// then the border, or the same texel as its second, which is in the same region
float3 edge_coord(uint t) { return float3(t & 1 ? 1 - EDGE : EDGE, t & 2 ? 1 - EDGE : EDGE, t / 4); }
[numthreads(8, 1, 1)]
void cs_edge(uint3 id : SV_DispatchThreadID) {
  min_mip.WriteSamplerFeedbackLevel(paired, s, edge_coord(id.x), 0);
  used.WriteSamplerFeedbackLevel(paired, s, edge_coord(id.x), 0);
}

// lanes 2n and 2n + 1 sample at one place and LOD, into maps 0 and 1
float2 pair_coord(uint2 t) { return (float2(t.x / 2, t.y) + 0.5) / float2(GRID_X / 2, GRID_Y) * SPAN + ORIGIN; }
[numthreads(8, 8, 1)]
void cs_pair(uint3 id : SV_DispatchThreadID) {
  pair[NonUniformResourceIndex(id.x & 1)].WriteSamplerFeedbackLevel(flat, s, pair_coord(id.xy), dense_lod(uint2(id.x / 2, id.y)));
}

// a triangle around the center of the last pixel of each quad of the target, which covers that pixel alone
float4 vs_dots(uint id : SV_VertexID) : SV_Position {
  uint quad = id / 3, corner = id % 3;
  float2 center = float2(quad % QUADS, quad / QUADS) * 2 + 1.5;
  float2 at = center + (corner == 0 ? float2(-0.3, -0.3) : corner == 1 ? float2(0.3, -0.3) : float2(0, 0.3));
  return float4(at / TARGET * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps_dots(float4 pos : SV_Position) : SV_Target {
  drawn.WriteSamplerFeedback(flat, s, pos.xy / TARGET * DOT_SCALE);
  return 0;
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  // the array's regions are 8 texels, so a lane's texels at the first mip are a few lanes apart
  const Paired array{64, 64, 2, 7, 8, 8}, flat{32, 32, 1, 6, 4, 4};
  const UINT grid_x = 32, grid_y = 16, quads = 8, target_size = 2 * quads;
  const float span = 1.3529f, origin = -0.1983f, hop = 0.0531f, dot_scale = 0.93f, edge = 0.0047f;
  std::vector<std::string> defines = {
      "GRID_X=" + std::to_string(grid_x), "GRID_Y=" + std::to_string(grid_y), "SPAN=" + std::to_string(span),
      "ORIGIN=" + std::to_string(origin), "HOP=" + std::to_string(hop),       "SLICES=" + std::to_string(array.slices),
      "QUADS=" + std::to_string(quads),   "TARGET=" + std::to_string(target_size),
      "DOT_SCALE=" + std::to_string(dot_scale), "EDGE=" + std::to_string(edge),
  };
  auto cs_dense = compiler.compile(hlsl, "cs_dense", "cs_6_5", defines),
       cs_pair = compiler.compile(hlsl, "cs_pair", "cs_6_5", defines),
       cs_edge = compiler.compile(hlsl, "cs_edge", "cs_6_5", defines),
       vs_dots = compiler.compile(hlsl, "vs_dots", "vs_6_5", defines),
       ps_dots = compiler.compile(hlsl, "ps_dots", "ps_6_5", defines);
  if (cs_dense.empty() || cs_pair.empty() || cs_edge.empty() || vs_dots.empty() || ps_dots.empty()) {
    printf(compiler.dxc ? "failed: HLSL did not compile\n" : "skipped: sampler feedback is shader model 6.5\n");
    return compiler.dxc ? 1 : 77;
  }
  ComPtr<ID3D12Device8> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS7 options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options, sizeof(options)));
  if (options.SamplerFeedbackTier != D3D12_SAMPLER_FEEDBACK_TIER_1_0) {
    printf("failed: sampler feedback tier %d\n", options.SamplerFeedbackTier);
    return 1;
  }

  const D3D12_TEXTURE_ADDRESS_MODE W = D3D12_TEXTURE_ADDRESS_MODE_WRAP, M = D3D12_TEXTURE_ADDRESS_MODE_MIRROR,
                                   C = D3D12_TEXTURE_ADDRESS_MODE_CLAMP, B = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
  const D3D12_FILTER linear = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  const Sampler runs[] = {
      {{C, C}, 0, true, linear},
      {{B, B}, 0, true, linear},
      {{W, M}, 0, true, D3D12_FILTER_MIN_MAG_MIP_POINT},
      {{M, W}, 1, true, D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR},
      {{B, C}, 0, true, D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR},
  };
  const UINT run_count = std::size(runs);
  const View whole{0, array.mips, 0, array.slices, 0}, flat_view{0, flat.mips, 0, flat.slices, 0};

  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  auto texture = [&](const D3D12_RESOURCE_DESC1 &desc, D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource> &out) {
    return device->CreateCommittedResource2(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, nullptr, IID_PPV_ARGS(&out));
  };
  auto desc = [](UINT width, UINT height, UINT slices, UINT mips, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags) {
    return D3D12_RESOURCE_DESC1{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, (UINT16)slices, (UINT16)mips, format,
                                {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, flags};
  };
  // the maps: the array's MinMip and MipRegionUsed, the flat texture's two MipRegionUsed and its MinMip
  enum { ARRAY_MIN_MIP, ARRAY_USED, PAIR_0, PAIR_1, DRAWN, MAPS };
  const Paired *of[MAPS] = {&array, &array, &flat, &flat, &flat};
  const bool used_kind[MAPS] = {false, true, true, true, false};
  ComPtr<ID3D12Resource> textures[2], maps[MAPS], decoded[MAPS], target;
  for (UINT t = 0; t < 2; t++) {
    auto &p = t ? flat : array;
    CHECK(texture(
        desc(p.width, p.height, p.slices, p.mips, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, textures[t]
    ));
  }
  for (UINT m = 0; m < MAPS; m++) {
    auto &p = *of[m];
    auto d = desc(p.width, p.height, p.slices, p.mips, used_kind[m] ? MIP_REGION_USED : MIN_MIP,
                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    d.SamplerFeedbackMipRegion = {p.region_width, p.region_height, 0};
    CHECK(texture(d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, maps[m]));
    // a decoded MipRegionUsed map has its paired texture's mips
    CHECK(texture(
        used_kind[m] ? desc(p.width, p.height, p.slices, p.mips, DXGI_FORMAT_R8_UINT, D3D12_RESOURCE_FLAG_NONE)
                     : desc(p.grid(0), p.grid(1), p.slices, 1, DXGI_FORMAT_R8_UINT, D3D12_RESOURCE_FLAG_NONE),
        D3D12_RESOURCE_STATE_RESOLVE_DEST, decoded[m]
    ));
  }
  CHECK(texture(
      desc(target_size, target_size, 1, 1, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
      D3D12_RESOURCE_STATE_RENDER_TARGET, target
  ));

  // descriptors: the maps, then the textures
  enum { UAVS = 0, SRVS = MAPS, DESCRIPTORS = MAPS + 2 };
  ComPtr<ID3D12DescriptorHeap> views, samplers, rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, DESCRIPTORS, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&views)));
  heap_desc = {D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, run_count, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&samplers)));
  heap_desc = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtvs)));
  auto view_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  auto sampler_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  auto cpu = [](ID3D12DescriptorHeap *h, UINT step, UINT i) {
    auto handle = h->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += step * i;
    return handle;
  };
  auto gpu = [](ID3D12DescriptorHeap *h, UINT step, UINT i) {
    auto handle = h->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += step * i;
    return handle;
  };
  for (UINT t = 0; t < 2; t++)
    device->CreateShaderResourceView(textures[t].Get(), nullptr, cpu(views.Get(), view_step, SRVS + t));
  for (UINT m = 0; m < MAPS; m++)
    device->CreateSamplerFeedbackUnorderedAccessView(
        textures[of[m] == &flat].Get(), maps[m].Get(), cpu(views.Get(), view_step, UAVS + m)
    );
  for (UINT r = 0; r < run_count; r++) {
    D3D12_SAMPLER_DESC sampler{runs[r].filter, runs[r].modes[0], runs[r].modes[1], D3D12_TEXTURE_ADDRESS_MODE_CLAMP};
    sampler.MinLOD = runs[r].min_lod;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    device->CreateSampler(&sampler, cpu(samplers.Get(), sampler_step, r));
  }
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);

  D3D12_DESCRIPTOR_RANGE uav_range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, MAPS}, srv_range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2},
      sampler_range{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1};
  D3D12_ROOT_PARAMETER params[3]{};
  params[0].DescriptorTable = {1, &uav_range};
  params[1].DescriptorTable = {1, &srv_range};
  params[2].DescriptorTable = {1, &sampler_range};
  auto rs = root_signature(device.Get(), {3, params});
  if (!rs) {
    printf("failed: root signature\n");
    return 1;
  }
  ComPtr<ID3D12PipelineState> dense_pso, pair_pso, edge_pso, dots_pso;
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute{rs.Get(), bytecode(cs_dense)};
  CHECK(device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&dense_pso)));
  compute.CS = bytecode(cs_pair);
  CHECK(device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&pair_pso)));
  compute.CS = bytecode(cs_edge);
  CHECK(device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&edge_pso)));
  D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{rs.Get(), bytecode(vs_dots), bytecode(ps_dots)};
  graphics.SampleMask = ~0u;
  graphics.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  graphics.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  graphics.NumRenderTargets = 1;
  graphics.RTVFormats[0] = DXGI_FORMAT_R8_UNORM;
  graphics.SampleDesc.Count = 1;
  CHECK(device->CreateGraphicsPipelineState(&graphics, IID_PPV_ARGS(&dots_pso)));

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList1> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));

  // what was decoded is read back a subresource per slot, each large enough for a first mip
  const UINT64 row = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, slot = row * array.height;
  const UINT slots = 2 * array.slices * (1 + array.mips);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, slot * slots, D3D12_RESOURCE_STATE_COPY_DEST);
  uint8_t *out;
  CHECK(readback->Map(0, nullptr, (void **)&out));
  UINT next_slot = 0;
  auto clear = [&](UINT m) {
    const UINT values[4] = {7, 7, 7, 7}; // a map clears to "no mip wanted", whatever the values
    list->ClearUnorderedAccessViewUint(
        gpu(views.Get(), view_step, UAVS + m), cpu(views.Get(), view_step, UAVS + m), maps[m].Get(), values, 0, nullptr
    );
  };
  // decodes the maps and reads them back: MinMip per slice, MipRegionUsed per slice and mip, in the order given
  auto decode = [&](std::initializer_list<UINT> which) {
    next_slot = 0;
    for (auto m : which) {
      auto &p = *of[m];
      transition(list.Get(), maps[m].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
      list->ResolveSubresourceRegion(
          decoded[m].Get(), UINT_MAX, 0, 0, maps[m].Get(), UINT_MAX, nullptr, DXGI_FORMAT_R8_UINT,
          D3D12_RESOLVE_MODE_DECODE_SAMPLER_FEEDBACK
      );
      transition(list.Get(), maps[m].Get(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      transition(list.Get(), decoded[m].Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
      for (UINT slice = 0; slice < p.slices; slice++)
        for (UINT level = 0; level < (used_kind[m] ? p.mips : 1); level++) {
          D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
              src{decoded[m].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
          UINT width = used_kind[m] ? p.size(0, level) : p.grid(0), height = used_kind[m] ? p.size(1, level) : p.grid(1);
          dst.PlacedFootprint = {slot * next_slot++, {DXGI_FORMAT_R8_UINT, width, height, 1, (UINT)row}};
          src.SubresourceIndex = slice * (used_kind[m] ? p.mips : 1) + level;
          list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
      transition(list.Get(), decoded[m].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RESOLVE_DEST);
    }
  };
  unsigned failures = 0, wanted_regions = 0;
  // compares what decode read back, from slot `first` on, with what the samples want of map `m`
  auto compare = [&](const char *what, UINT m, UINT first, const Wanted &wanted) {
    auto &p = *of[m];
    UINT s = first;
    for (UINT slice = 0; slice < p.slices; slice++)
      for (UINT level = 0; level < (used_kind[m] ? p.mips : 1); level++, s++)
        for (UINT y = 0; y < p.regions(1, level); y++)
          for (UINT x = 0; x < p.regions(0, level); x++) {
            uint8_t value = out[slot * s + row * y + x];
            uint8_t expected = used_kind[m] ? decoded_used(wanted, slice, level, x, y) : decoded_min_mip(wanted, p, slice, x, y);
            wanted_regions += expected != (used_kind[m] ? 0 : 0xff);
            if (value != expected && failures++ < 12)
              printf(
                  "%s: %s slice %u mip %u region %u,%u: %#x, want %#x\n", what, used_kind[m] ? "MipRegionUsed" : "MinMip",
                  slice, level, x, y, value, expected
              );
          }
    return s;
  };
  auto empty = [](const Paired &p) { return Wanted(p.slices, Wanted::value_type(p.mips)); };
  auto bind = [&](bool graphics, UINT sampler) {
    ID3D12DescriptorHeap *heaps[] = {views.Get(), samplers.Get()};
    list->SetDescriptorHeaps(2, heaps);
    D3D12_GPU_DESCRIPTOR_HANDLE tables[] = {
        gpu(views.Get(), view_step, UAVS), gpu(views.Get(), view_step, SRVS), gpu(samplers.Get(), sampler_step, sampler)
    };
    graphics ? list->SetGraphicsRootSignature(rs.Get()) : list->SetComputeRootSignature(rs.Get());
    for (UINT i = 0; i < 3; i++)
      graphics ? list->SetGraphicsRootDescriptorTable(i, tables[i]) : list->SetComputeRootDescriptorTable(i, tables[i]);
  };
  auto run = [&]() -> HRESULT {
    HRESULT hr = submit(device.Get(), queue.Get(), list.Get());
    if (SUCCEEDED(hr) && SUCCEEDED(hr = allocator->Reset()))
      hr = list->Reset(allocator.Get(), nullptr);
    return hr;
  };
  // the lane's coordinates and LOD, as the shaders work them out
  auto lane_coord = [&](UINT x, UINT y, UINT columns, float &u, float &v) {
    u = (float(x) + 0.5f) / float(columns) * span + origin;
    v = (float(y) + 0.5f) / float(grid_y) * span + origin;
  };
  auto lane_lod = [](UINT x, UINT y) { return float(x % 8) * 0.5f + float(y % 3) * 0.25f; };

  float margin = 1;
  for (UINT r = 0; r < run_count; r++) {
    auto &test = runs[r];
    char what[32];
    // the dense pass, both of the array's maps
    clear(ARRAY_MIN_MIP);
    clear(ARRAY_USED);
    clear(PAIR_0);
    clear(PAIR_1);
    bind(false, r);
    list->SetPipelineState(dense_pso.Get());
    list->Dispatch(grid_x / 8, grid_y / 8, 1);
    list->SetPipelineState(pair_pso.Get());
    list->Dispatch(grid_x / 8, grid_y / 8, 1);
    decode({ARRAY_MIN_MIP, ARRAY_USED, PAIR_0, PAIR_1});
    CHECK(run());
    // a MinMip map takes the LOD before the sampler's clamp, a MipRegionUsed map after
    Wanted dense[2] = {empty(array), empty(array)}, paired_maps = empty(flat);
    for (UINT y = 0; y < grid_y; y++)
      for (UINT x = 0; x < grid_x; x++) {
        float u, v, lod = lane_lod(x, y);
        lane_coord(x, y, grid_x, u, v);
        if ((x * 7 + y * 3) % 5)
          for (UINT i = 0; i < x % 3 + 1; i++)
            for (UINT kind = 0; kind < 2; kind++)
              sample(
                  dense[kind], array, whole, u + i * hop, v + i * hop, (y / 2) % array.slices,
                  kind ? std::max(lod, test.min_lod) : lod, test, nullptr, &margin, !kind
              );
        lane_coord(x / 2, y, grid_x / 2, u, v);
        lod = lane_lod(x / 2, y);
        sample(paired_maps, flat, flat_view, u, v, 0, std::max(lod, test.min_lod), test, nullptr, &margin, false);
      }
    snprintf(what, sizeof(what), "run %u", r);
    UINT at = compare(what, ARRAY_MIN_MIP, 0, dense[0]);
    at = compare(what, ARRAY_USED, at, dense[1]);
    snprintf(what, sizeof(what), "run %u, map 0", r);
    at = compare(what, PAIR_0, at, paired_maps);
    snprintf(what, sizeof(what), "run %u, map 1", r);
    compare(what, PAIR_1, at, paired_maps);

    // the samples near the corners alone, so no other sample wants their regions
    clear(ARRAY_MIN_MIP);
    clear(ARRAY_USED);
    bind(false, r);
    list->SetPipelineState(edge_pso.Get());
    list->Dispatch(1, 1, 1);
    decode({ARRAY_MIN_MIP, ARRAY_USED});
    CHECK(run());
    Wanted edges[2] = {empty(array), empty(array)};
    for (UINT i = 0; i < 8; i++)
      for (UINT kind = 0; kind < 2; kind++)
        sample(
            edges[kind], array, whole, i & 1 ? 1 - edge : edge, i & 2 ? 1 - edge : edge, i / 4,
            kind ? test.min_lod : 0, test, nullptr, nullptr, !kind
        );
    snprintf(what, sizeof(what), "run %u, corners", r);
    compare(what, ARRAY_USED, compare(what, ARRAY_MIN_MIP, 0, edges[0]), edges[1]);
  }

  // the pixel shader with the first sampler: a pixel's step is DOT_SCALE texels of the target along each axis
  clear(DRAWN);
  bind(true, 0);
  list->SetPipelineState(dots_pso.Get());
  D3D12_VIEWPORT viewport{0, 0, (float)target_size, (float)target_size, 0, 1};
  D3D12_RECT scissor{0, 0, (LONG)target_size, (LONG)target_size};
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list->DrawInstanced(3 * quads * quads, 1, 0, 0);
  decode({DRAWN});
  CHECK(run());
  const float pixel_d = dot_scale * flat.width / target_size, dx[2] = {pixel_d, 0}, dy[2] = {0, pixel_d};
  auto f = footprint(dx, dy, runs[0]);
  Wanted dots = empty(flat);
  for (UINT qy = 0; qy < quads; qy++)
    for (UINT qx = 0; qx < quads; qx++)
      sample(
          dots, flat, flat_view, (2 * qx + 1.5f) / target_size * dot_scale, (2 * qy + 1.5f) / target_size * dot_scale, 0,
          f.lod, runs[0], &f, &margin, true
      );
  compare("covered pixels", DRAWN, 0, dots);

  // the expectations hold only while no sample sits where rounding decides its texels or mips: 64 units in the last
  // place of the largest texel coordinate away
  if (margin < 64 * FLT_EPSILON * array.width) {
    printf("failed: a sample is %g from a texel or mip boundary\n", margin);
    return 1;
  }
  if (failures) {
    printf("failed: %u wrong regions\n", failures);
    return 1;
  }
  printf("passed: %u regions wanted\n", wanted_regions);
  return 0;
}
