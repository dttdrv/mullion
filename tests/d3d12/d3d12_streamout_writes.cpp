// contract: a draw with stream output runs each shader invocation once (D3D11.3 14: stream output takes the
// vertices a stage has output, it does not run the stage again). what a geometry shader, or a vertex shader streamed
// without one, writes to unordered access views is there once: an atomic add on a buffer and on a texture, a plain
// read-modify-write of each, and a compare-exchange. the stream gets what the invocation itself saw: the counter
// before its own add, each value once, and the compare-exchange's original value.
// the geometry shader's draw runs first, then the vertex shader's on the same views, so the second sees the first's.
#include "d3d12_test.hpp"
#include <algorithm>
#include <iterator>

static const char hlsl[] = R"hlsl(
RWStructuredBuffer<uint> counters : register(u0);
RWTexture2D<uint> texels : register(u1);
struct O { float4 pos : SV_Position; uint3 value : VALUE; };
// point `id` of a draw: counters[0] counts invocations, counters[1 + id] and row 1 of the texture take a plain
// add, row 0 an atomic one, and counters[1 + POINTS + id] is exchanged from zero
O invocation(uint id) {
  uint before, original;
  InterlockedAdd(counters[0], 1, before);
  counters[1 + id] += PLAIN;
  InterlockedAdd(texels[uint2(id, 0)], ATOMIC);
  texels[uint2(id, 1)] = texels[uint2(id, 1)] + PLAIN;
  InterlockedCompareExchange(counters[1 + POINTS + id], 0, id + 1, original);
  O o;
  o.pos = float4(0, 0, 0, 1);
  o.value = uint3(id, before, original);
  return o;
}
struct V { uint id : ID; };
V vs(uint id : SV_VertexID) { V v; v.id = id; return v; }
[maxvertexcount(1)]
void gs(point V v[1], inout PointStream<O> s) { s.Append(invocation(v[0].id)); }
O vs_alone(uint id : SV_VertexID) { return invocation(id); }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  // more points than one object threadgroup takes
  const UINT points = 100, plain = 5, atomic = 2, draws = 2;
  std::vector<std::string> defines = {
      "POINTS=" + std::to_string(points), "PLAIN=" + std::to_string(plain), "ATOMIC=" + std::to_string(atomic)
  };
  auto vs = compiler.compile(hlsl, "vs", "vs", defines), gs = compiler.compile(hlsl, "gs", "gs", defines),
       vs_alone = compiler.compile(hlsl, "vs_alone", "vs", defines);
  if (vs.empty() || gs.empty() || vs_alone.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 0};
  D3D12_ROOT_PARAMETER parameter{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE};
  parameter.DescriptorTable = {1, &range};
  auto rs = root_signature(device.Get(), {1, &parameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT});

  const D3D12_SO_DECLARATION_ENTRY entries[] = {{0, "VALUE", 0, 0, 3, 0}};
  const UINT stride = 3 * sizeof(uint32_t);
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs)};
  desc.GS = bytecode(gs);
  desc.StreamOutput = {entries, 1, &stride, 1, D3D12_SO_NO_RASTERIZED_STREAM};
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> gs_pso, alone_pso;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&gs_pso)));
  desc.VS = bytecode(vs_alone);
  desc.GS = {};
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&alone_pso)));

  const UINT counter_count = 1 + 2 * points;
  const UINT64 counter_bytes = counter_count * sizeof(uint32_t), stream_bytes = (UINT64)draws * points * stride,
               filled_at = (stream_bytes + 7) & ~7ull, row = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT *
                                                             ((points * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) /
                                                              D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
  // where the readback keeps each: the counters, the stream with its filled size, the texels
  const UINT64 stream_at = counter_bytes, texels_at = (stream_at + filled_at + 8 + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
                                                      ~(UINT64)(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
  auto counters = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, counter_bytes, D3D12_RESOURCE_STATE_COPY_DEST,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto stream = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, filled_at + 8, D3D12_RESOURCE_STATE_COPY_DEST);
  // an upload buffer starts zeroed: everything starts as a copy of it
  auto zeros = buffer(
      device.Get(), D3D12_HEAP_TYPE_UPLOAD, std::max({counter_bytes, filled_at + 8, 2 * row}), D3D12_RESOURCE_STATE_GENERIC_READ
  );
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, texels_at + 2 * row, D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12Resource> texels;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC texel_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, points, 2, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0},
                                 D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &texel_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texels)
  ));
  ComPtr<ID3D12DescriptorHeap> views;
  D3D12_DESCRIPTOR_HEAP_DESC views_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&views_desc, IID_PPV_ARGS(&views)));
  auto handle = views->GetCPUDescriptorHandleForHeapStart();
  D3D12_UNORDERED_ACCESS_VIEW_DESC counters_view{DXGI_FORMAT_UNKNOWN, D3D12_UAV_DIMENSION_BUFFER};
  counters_view.Buffer = {0, counter_count, sizeof(uint32_t)};
  device->CreateUnorderedAccessView(counters.Get(), nullptr, &counters_view, handle);
  handle.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  device->CreateUnorderedAccessView(texels.Get(), nullptr, nullptr, handle);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), gs_pso.Get(), IID_PPV_ARGS(&list)));
  D3D12_TEXTURE_COPY_LOCATION placed{zeros.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
      texture{texels.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  placed.PlacedFootprint = {0, {DXGI_FORMAT_R32_UINT, points, 2, 1, (UINT)row}};
  list->CopyBufferRegion(stream.Get(), 0, zeros.Get(), 0, filled_at + 8);
  list->CopyBufferRegion(counters.Get(), 0, zeros.Get(), 0, counter_bytes);
  list->CopyTextureRegion(&texture, 0, 0, 0, &placed, nullptr);
  transition(list.Get(), stream.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
  transition(list.Get(), counters.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  transition(list.Get(), texels.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ID3D12DescriptorHeap *heaps[] = {views.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetGraphicsRootSignature(rs.Get());
  list->SetGraphicsRootDescriptorTable(0, views->GetGPUDescriptorHandleForHeapStart());
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
  auto base = stream->GetGPUVirtualAddress();
  D3D12_STREAM_OUTPUT_BUFFER_VIEW view{base, stream_bytes, base + filled_at};
  list->SOSetTargets(0, 1, &view);
  list->DrawInstanced(points, 1, 0, 0);
  list->SetPipelineState(alone_pso.Get());
  list->DrawInstanced(points, 1, 0, 0);
  transition(list.Get(), stream.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(list.Get(), counters.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(list.Get(), texels.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(), 0, counters.Get(), 0, counter_bytes);
  list->CopyBufferRegion(readback.Get(), stream_at, stream.Get(), 0, filled_at + 8);
  placed.pResource = readback.Get();
  placed.PlacedFootprint.Offset = texels_at;
  list->CopyTextureRegion(&placed, 0, 0, 0, &texture, nullptr);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  uint8_t *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));

  unsigned failures = 0;
  auto expect = [&](const char *what, UINT index, uint64_t value, uint64_t want) {
    if (value != want && failures++ < 12)
      printf("%s %u: %llu, want %llu\n", what, index, (unsigned long long)value, (unsigned long long)want);
  };
  auto word = [&](UINT64 at) { return *reinterpret_cast<const uint32_t *>(got + at); };
  expect("invocations", 0, word(0), draws * points);
  for (UINT id = 0; id < points; id++) {
    expect("plain buffer add", id, word(4 * (1 + id)), draws * plain);
    expect("exchanged", id, word(4 * (1 + points + id)), id + 1);
    expect("atomic texel add", id, word(texels_at + 4 * id), draws * atomic);
    expect("plain texel add", id, word(texels_at + row + 4 * id), draws * plain);
  }
  expect("filled size", 0, *reinterpret_cast<const uint64_t *>(got + stream_at + filled_at), stream_bytes);
  for (UINT draw = 0; draw < draws; draw++) {
    // each invocation of a draw saw the counter at another value of the draw's range
    std::vector<uint32_t> seen;
    for (UINT id = 0; id < points; id++) {
      UINT64 at = stream_at + (UINT64)(draw * points + id) * stride;
      expect("streamed point", draw * points + id, word(at), id);
      seen.push_back(word(at + 4));
      // the first draw found the word zero and stored id + 1, which the second found
      expect("original value", draw * points + id, word(at + 8), draw ? id + 1 : 0);
    }
    std::sort(seen.begin(), seen.end());
    for (UINT i = 0; i < points; i++)
      expect("counter seen", draw * points + i, seen[i], draw * points + i);
  }
  if (failures) {
    printf("failed: %u wrong values\n", failures);
    return 1;
  }
  printf("passed: %u invocations, each one's writes once\n", draws * points);
  return 0;
}
