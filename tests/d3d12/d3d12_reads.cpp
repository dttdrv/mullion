// contract: every stage reads floats from buffers and textures as it is written to. a typed buffer and a
// structured buffer give the element asked for (ld and ld_structured, D3D11.3 22.4.6 and 22.4.12, both in all
// stages); a texture gives the texel of the mip asked for by Load; SampleLevel and SampleGrad pick their mip
// without a pixel's derivatives, so every stage has them (22.4.18 sample_l: "LOD is provided directly by the
// application", "available in all progammable Shader stages"; 22.4.17 sample_d: "Derivatives are supplied by" the
// shader); SampleBias's bias "is added to the computed LOD on a per-pixel basis" (22.4.16), GetDimensions is
// resinfo (22.4.14), and a pixel shader may store to an unordered access texture (22.4.9). a wave's lanes read each other's floats
// (WaveReadLaneFirst, WaveReadLaneAt: DirectX-Specs, HLSL Shader Model 6.0, Wave Intrinsics).
// these are the reads of a scene kept on the GPU, and each is in the game's shaders in a stage no other test has
// it in: Unreal's vertex shaders read instances and primitives from Buffer<float4> and structured buffers and
// sample height and offset textures by level; its pixel shaders sample with a bias, ask a texture for its size and
// store feedback.
// a point for each of ELEMENTS elements (the column) and each kind of read (the row); the first rows are read by
// the vertex shader and handed down, the others by the pixel shader. every element reads a mip and a texel of its
// own; the sampler takes the nearest texel of the nearest mip, and the texels are at their centres, so what is
// sampled is one texel exactly. the texture's texels hold their place and mip. every pixel of the target, the
// stored texture and the compute shaders' results are held to the data.
#include "d3d12_test.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cfloat>

static const char hlsl[] = R"hlsl(
Buffer<float4> rows : register(t0);
StructuredBuffer<float4> records : register(t1);
Texture2D<float4> mips : register(t2);
RWTexture2D<float4> stored : register(u0);
RWStructuredBuffer<float4> result : register(u1);
SamplerState nearest : register(s0);
// the mip element `i` reads, its texel there, and the texel's centre
uint lod_of(uint i) { return i % MIPS; }
uint2 texel_of(uint i) { uint size = SIZE >> lod_of(i); return uint2(i % size, i / 2 % size); }
float2 centre_of(uint i) { return (texel_of(i) + 0.5) / (SIZE >> lod_of(i)); }

struct V { float4 pos : SV_Position; nointerpolation float4 got : GOT; nointerpolation uint2 at : AT; };
V vs(uint row : SV_VertexID, uint i : SV_InstanceID) {
  V v;
  v.pos = float4((i + 0.5) * 2 / ELEMENTS - 1, 1 - (row + 0.5) * 2 / KINDS, 0, 1);
  v.at = uint2(i, row);
  v.got = 0;
  // a texel of the mip a pixel wide and high: the mip SampleGrad is to take
  float across = float(1u << lod_of(i)) / SIZE;
  switch (row) {
  case 0: v.got = rows[i]; break;
  case 1: v.got = records[i]; break;
  case 2: v.got = mips.Load(int3(texel_of(i), lod_of(i))); break;
  case 3: v.got = mips.SampleLevel(nearest, centre_of(i), lod_of(i)); break;
  case 4: v.got = mips.SampleGrad(nearest, centre_of(i), float2(across, 0), float2(0, across)); break;
  }
  return v;
}
float4 ps(V v) : SV_Target {
  uint i = v.at.x, w, h, levels;
  // a texel of the first mip a pixel: the bias alone names the mip
  float4 biased = mips.SampleBias(nearest, v.pos.xy / SIZE, lod_of(i));
  mips.GetDimensions(lod_of(i), w, h, levels);
  switch (v.at.y) {
  case BY_VERTEX + 0: return rows[i];
  case BY_VERTEX + 1: return records[i];
  case BY_VERTEX + 2: return mips.Load(int3(texel_of(i), lod_of(i)));
  case BY_VERTEX + 3: return biased;
  case BY_VERTEX + 4: return float4(w, h, levels, i);
  case BY_VERTEX + 5: stored[uint2(i, 0)] = rows[i] + records[i]; return records[i].wzyx;
  }
  return v.got;
}
[numthreads(ELEMENTS, 1, 1)] void cs(uint3 id : SV_DispatchThreadID) { result[id.x] = rows[id.x] + 2 * records[id.x]; }
)hlsl";

// for DXC alone: the other compiler has no waves
static const char waves_hlsl[] = R"hlsl(
Buffer<float4> rows : register(t0);
RWStructuredBuffer<float4> result : register(u1);
// what the wave's first lane has, read both ways, the element that lane is at, and the thread's own
[numthreads(ELEMENTS, 1, 1)] void cs_waves(uint3 id : SV_DispatchThreadID) {
  float mine = rows[id.x].x;
  result[ELEMENTS + id.x] = float4(WaveReadLaneFirst(mine), WaveReadLaneAt(mine, WaveReadLaneFirst(WaveGetLaneIndex())),
                                   WaveReadLaneFirst(id.x), mine);
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  using Float4 = std::array<float, 4>;
  // the kinds of read: BY_VERTEX by the vertex shader, then the pixel shader's
  const UINT elements = 16, by_vertex = 5, by_pixel = 6, kinds = by_vertex + by_pixel, size = 8, mips = std::bit_width(size);
  const std::vector<std::string> defines = {"ELEMENTS=" + std::to_string(elements), "KINDS=" + std::to_string(kinds), "SIZE=" + std::to_string(size),
                                            "MIPS=" + std::to_string(mips), "BY_VERTEX=" + std::to_string(by_vertex)};
  auto vs = compiler.compile(hlsl, "vs", "vs", defines), ps = compiler.compile(hlsl, "ps", "ps", defines), cs = compiler.compile(hlsl, "cs", "cs", defines),
       cs_waves = compiler.compile(waves_hlsl, "cs_waves", "cs_6_0", defines);
  if (vs.empty() || ps.empty() || cs.empty() || (compiler.dxc && cs_waves.empty())) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  // the data: what an element of each buffer holds, and a texel of a mip
  auto row = [](UINT i) { return Float4{float(i), 0.25f * i, -float(i), 100.f + i}; };
  auto record = [](UINT i) { return Float4{1000.f + i, 0.5f * i, float(i * i), -1}; };
  auto texel = [](UINT x, UINT y, UINT mip) { return Float4{float(x), float(y), float(mip), 0.5f}; };
  // as the shader has them
  auto lod_of = [=](UINT i) { return i % mips; };
  auto texel_of = [=](UINT i) { return texel(i % (size >> lod_of(i)), i / 2 % (size >> lod_of(i)), lod_of(i)); };

  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_DESCRIPTOR_RANGE read_range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3}, stored_range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2};
  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}, {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}};
  params[0].DescriptorTable = {1, &read_range};
  params[1].DescriptorTable = {1, &stored_range};
  D3D12_STATIC_SAMPLER_DESC sampler{D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                    D3D12_TEXTURE_ADDRESS_MODE_CLAMP};
  sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER, sampler.MaxLOD = FLT_MAX;
  auto rs = root_signature(device.Get(), {(UINT)std::size(params), params, 1, &sampler});
  const DXGI_FORMAT format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = rs.Get();
  desc.VS = bytecode(vs), desc.PS = bytecode(ps);
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
  desc.NumRenderTargets = 1, desc.RTVFormats[0] = format;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> draws, dispatches, waves;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&draws)));
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc{rs.Get(), bytecode(cs)};
  CHECK(device->CreateComputePipelineState(&compute_desc, IID_PPV_ARGS(&dispatches)));
  compute_desc.CS = bytecode(cs_waves);
  if (compiler.dxc)
    CHECK(device->CreateComputePipelineState(&compute_desc, IID_PPV_ARGS(&waves)));

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), draws.Get(), IID_PPV_ARGS(&list)));

  // the two buffers' elements, then the texture's mips, staged in one buffer
  const D3D12_HEAP_PROPERTIES gpu{D3D12_HEAP_TYPE_DEFAULT};
  const UINT64 buffer_bytes = elements * sizeof(Float4);
  D3D12_RESOURCE_DESC texture_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, (UINT16)mips, format, {1, 0}};
  std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> placed(mips);
  UINT64 texture_bytes;
  device->GetCopyableFootprints(&texture_desc, 0, mips, 2 * buffer_bytes, placed.data(), nullptr, nullptr, &texture_bytes);
  auto staged = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 2 * buffer_bytes + texture_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  char *bytes;
  CHECK(staged->Map(0, nullptr, (void **)&bytes));
  for (UINT i = 0; i < elements; i++) {
    const Float4 values[2] = {row(i), record(i)};
    for (UINT which = 0; which < 2; which++)
      memcpy(bytes + which * buffer_bytes + i * sizeof(Float4), &values[which], sizeof(Float4));
  }
  for (UINT mip = 0; mip < mips; mip++)
    for (UINT y = 0; y < size >> mip; y++)
      for (UINT x = 0; x < size >> mip; x++) {
        const Float4 value = texel(x, y, mip);
        memcpy(bytes + placed[mip].Offset + y * placed[mip].Footprint.RowPitch + x * sizeof(Float4), &value, sizeof(Float4));
      }
  const auto reading = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, storing = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  ComPtr<ID3D12Resource> read_buffers[2], texture, stored, target;
  CHECK(device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &texture_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)));
  for (UINT which = 0; which < 2; which++) {
    read_buffers[which] = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, buffer_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyBufferRegion(read_buffers[which].Get(), 0, staged.Get(), which * buffer_bytes, buffer_bytes);
    transition(list.Get(), read_buffers[which].Get(), D3D12_RESOURCE_STATE_COPY_DEST, reading);
  }
  for (UINT mip = 0; mip < mips; mip++) {
    D3D12_TEXTURE_COPY_LOCATION to{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = mip}},
        from{staged.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = placed[mip]}};
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  }
  transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, reading);
  // the target, a row a kind of read, and the texture the pixel shader stores to, one texel an element
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, elements, kinds, 1, 1, format, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  D3D12_RESOURCE_DESC stored_desc = target_desc;
  stored_desc.Height = 1, stored_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  CHECK(device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)));
  CHECK(device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &stored_desc, storing, nullptr, IID_PPV_ARGS(&stored)));
  const UINT64 result_bytes = 2 * buffer_bytes;
  auto result = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, result_bytes, storing, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

  ComPtr<ID3D12DescriptorHeap> views, rtv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC views_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 5, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE},
      rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&views_desc, IID_PPV_ARGS(&views)));
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)));
  const UINT apart = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  auto view_at = [&](UINT slot) {
    auto handle = views->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += slot * apart;
    return handle;
  };
  D3D12_SHADER_RESOURCE_VIEW_DESC typed{format, D3D12_SRV_DIMENSION_BUFFER, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING},
      structured{DXGI_FORMAT_UNKNOWN, D3D12_SRV_DIMENSION_BUFFER, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
  typed.Buffer = {0, elements}, structured.Buffer = {0, elements, sizeof(Float4)};
  D3D12_UNORDERED_ACCESS_VIEW_DESC result_view{DXGI_FORMAT_UNKNOWN, D3D12_UAV_DIMENSION_BUFFER};
  result_view.Buffer = {0, 2 * elements, sizeof(Float4)};
  device->CreateShaderResourceView(read_buffers[0].Get(), &typed, view_at(0));
  device->CreateShaderResourceView(read_buffers[1].Get(), &structured, view_at(1));
  device->CreateShaderResourceView(texture.Get(), nullptr, view_at(2));
  device->CreateUnorderedAccessView(stored.Get(), nullptr, nullptr, view_at(3));
  device->CreateUnorderedAccessView(result.Get(), nullptr, &result_view, view_at(4));
  auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);

  // the target, the stored texture and the results, read back one after another
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT target_placed, stored_placed;
  UINT64 target_bytes, stored_bytes;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &target_placed, nullptr, nullptr, &target_bytes);
  const UINT64 stored_at = (target_bytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) / D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
  device->GetCopyableFootprints(&stored_desc, 0, 1, stored_at, &stored_placed, nullptr, nullptr, &stored_bytes);
  const UINT64 result_at = stored_at + stored_bytes;
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, result_at + result_bytes, D3D12_RESOURCE_STATE_COPY_DEST);

  step("%u elements read %u ways by a vertex shader and %u by a pixel shader, and by compute shaders", elements, by_vertex, by_pixel);
  // a value no read gives, where nothing is drawn
  const float untouched[4] = {-7, -7, -7, -7};
  const D3D12_VIEWPORT viewport{0, 0, (float)elements, (float)kinds, 0, 1};
  const D3D12_RECT scissor{0, 0, (LONG)elements, (LONG)kinds};
  ID3D12DescriptorHeap *bound[] = {views.Get()};
  auto read_table = views->GetGPUDescriptorHandleForHeapStart(), stored_table = read_table;
  stored_table.ptr += 3 * apart;
  list->ClearRenderTargetView(rtv, untouched, 0, nullptr);
  list->SetDescriptorHeaps(1, bound);
  list->SetGraphicsRootSignature(rs.Get());
  list->SetGraphicsRootDescriptorTable(0, read_table);
  list->SetGraphicsRootDescriptorTable(1, stored_table);
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
  list->DrawInstanced(kinds, elements, 0, 0);
  list->SetComputeRootSignature(rs.Get());
  list->SetComputeRootDescriptorTable(0, read_table);
  list->SetComputeRootDescriptorTable(1, stored_table);
  for (auto pipeline : {dispatches.Get(), waves.Get()})
    if (pipeline) {
      list->SetPipelineState(pipeline);
      list->Dispatch(1, 1, 1);
    }
  transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(list.Get(), stored.Get(), storing, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(list.Get(), result.Get(), storing, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION from_target{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}, from_stored{stored.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
      to_target{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = target_placed}},
      to_stored{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = stored_placed}};
  list->CopyTextureRegion(&to_target, 0, 0, 0, &from_target, nullptr);
  list->CopyTextureRegion(&to_stored, 0, 0, 0, &from_stored, nullptr);
  list->CopyBufferRegion(readback.Get(), result_at, result.Get(), 0, result_bytes);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  const char *out;
  CHECK(readback->Map(0, nullptr, (void **)&out));

  auto sum = [](Float4 a, Float4 b, float scale) { return Float4{a[0] + scale * b[0], a[1] + scale * b[1], a[2] + scale * b[2], a[3] + scale * b[3]}; };
  auto holds = [&](const void *at, Float4 want, const char *what, UINT i) {
    Float4 got;
    memcpy(&got, at, sizeof(got));
    expect(got == want, "%s, element %u: %g %g %g %g, want %g %g %g %g", what, i, got[0], got[1], got[2], got[3], want[0], want[1], want[2], want[3]);
  };
  const char *names[] = {"a vertex shader's typed buffer",    "a vertex shader's structured buffer", "a vertex shader's Load",
                         "a vertex shader's SampleLevel",     "a vertex shader's SampleGrad",        "a pixel shader's typed buffer",
                         "a pixel shader's structured buffer", "a pixel shader's Load",               "a pixel shader's SampleBias",
                         "a pixel shader's GetDimensions",    "a pixel shader's return beside its store"};
  for (UINT kind = 0; kind < kinds; kind++)
    for (UINT i = 0; i < elements; i++) {
      const UINT lod = lod_of(i), mip_size = size >> lod;
      // the texel a pixel's own place names in the mip, held to the edge: the pixel shader samples there
      auto clamped = [&](UINT pixel) { return std::min(UINT((pixel + 0.5f) / size * mip_size), mip_size - 1); };
      const Float4 r = row(i), s = record(i);
      const Float4 want[] = {r, s, texel_of(i), texel_of(i), texel_of(i), r, s, texel_of(i), texel(clamped(i), clamped(kind), lod),
                             {float(mip_size), float(mip_size), float(mips), float(i)}, {s[3], s[2], s[1], s[0]}};
      holds(out + target_placed.Offset + kind * target_placed.Footprint.RowPitch + i * sizeof(Float4), want[kind], names[kind], i);
    }
  for (UINT i = 0; i < elements; i++) {
    holds(out + stored_placed.Offset + i * sizeof(Float4), sum(row(i), record(i), 1), "what a pixel shader stored", i);
    holds(out + result_at + i * sizeof(Float4), sum(row(i), record(i), 2), "a compute shader's buffers", i);
    if (!waves)
      continue;
    // the first lane's element, which the shader says, has the value both reads gave
    Float4 got;
    memcpy(&got, out + result_at + (elements + i) * sizeof(Float4), sizeof(got));
    const UINT first = UINT(got[2]);
    expect(first < elements && got[0] == row(first)[0] && got[1] == got[0] && got[3] == row(i)[0],
           "a wave's reads, element %u: %g by WaveReadLaneFirst and %g by WaveReadLaneAt of the lane at element %g, own %g", i, got[0], got[1],
           got[2], got[3]);
  }
  if (!waves)
    printf("not run with this front end: a wave's reads\n");
  return verdict();
}
