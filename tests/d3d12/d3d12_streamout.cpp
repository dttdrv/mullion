// contract: stream output writes every primitive a geometry shader completes, in the API's order (draw instance, input
// primitive, geometry instance, then emission), as a list: a triangle strip's odd triangles as (n, n + 2, n + 1), and
// RestartStrip starting a new strip. each element goes to its slot at its offset in the vertex, at the slot's stride,
// gaps and the rest of the stride untouched, starting at the buffer's BufferFilledSize, which moves on by what was
// written, so the next draw appends. a stream stops at the first primitive that does not fit whole in all its
// buffers (D3D11.3 functional spec, 14.5). streams count apart: a point geometry shader emits to two streams, each to
// its own buffer. without a geometry shader, the vertex shader's primitives go out as lists: a triangle strip, which
// also rasterizes over the whole target, and points over several object threadgroups and two instances. the points
// span several object threadgroups and the draws two instances, and the expectations are the same shaders run on the
// CPU. a statistics query per stream counts the primitives written and those that would have been. the stream points
// and the vertex shader's points, drawn again from many vertices, span thousands of object threadgroups.
#include "d3d12_test.hpp"
#include <array>
#include <cstdint>

static const char hlsl[] = R"hlsl(
struct V { uint id : ID; };
V vs(uint id : SV_VertexID, uint instance : SV_InstanceID) { V v; v.id = id + instance * 1000; return v; }
struct O { float4 pos : SV_Position; uint4 value : VALUE; };
O vertex(uint4 value) { O o; o.pos = float4(0, 0, 0, 1); o.value = value; return o; }
// (p + i) % 9 vertices from point p, geometry instance i, the strip restarting after four (so a strip has an odd
// triangle)
[maxvertexcount(8)] [instance(INSTANCES)]
void gs_strips(point V v[1], uint i : SV_GSInstanceID, inout TriangleStream<O> s) {
  uint n = (v[0].id + i) % 9;
  for (uint j = 0; j < n; j++) {
    if (j == 4)
      s.RestartStrip();
    s.Append(vertex(uint4(v[0].id, i, j, 7)));
  }
}
// a point per input triangle: its vertices and primitive ID
[maxvertexcount(1)]
void gs_triangles(triangle V v[3], uint primitive : SV_PrimitiveID, inout PointStream<O> s) {
  s.Append(vertex(uint4(v[0].id, v[1].id, v[2].id, primitive)));
}
// the vertex shader alone: a strip of quads across the target, and points
struct S { float4 pos : SV_Position; uint4 value : VALUE; };
S vs_alone(uint id : SV_VertexID, uint instance : SV_InstanceID) {
  S o;
  o.value = uint4(id, instance, id * 3, 9);
  o.pos = float4((id >> 1) - 1.0, (id & 1) * 2.0 - 1, 0, 1);
  return o;
}
uint ps() : SV_Target { return 1; }
struct P { float4 pos : SV_Position; uint value : VALUE; };
// stream 0 gets point p once, stream 1 gets it p % 3 times
[maxvertexcount(3)]
void gs_streams(point V v[1], inout PointStream<P> s0, inout PointStream<P> s1) {
  P o;
  o.pos = float4(0, 0, 0, 1);
  o.value = v[0].id;
  s0.Append(o);
  for (uint j = 0; j < v[0].id % 3; j++) {
    o.value = v[0].id * 10 + j;
    s1.Append(o);
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
  const UINT points = 100, instances = 2, gs_instances = 2;
  std::vector<std::string> defines = {"INSTANCES=" + std::to_string(gs_instances)};
  auto vs = compiler.compile(hlsl, "vs", "vs", defines), gs_strips = compiler.compile(hlsl, "gs_strips", "gs", defines),
       gs_streams = compiler.compile(hlsl, "gs_streams", "gs", defines), vs_alone = compiler.compile(hlsl, "vs_alone", "vs", defines),
       ps = compiler.compile(hlsl, "ps", "ps", defines);
  if (vs.empty() || gs_strips.empty() || gs_streams.empty() || vs_alone.empty() || ps.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  auto rs = root_signature(device.Get(), {0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT});

  // slot 0: VALUE.xy, a gap, VALUE.w, in 16 of a 20-byte stride; slot 1: VALUE.z in 4 of 8
  const D3D12_SO_DECLARATION_ENTRY strip_entries[] = {
      {0, "VALUE", 0, 0, 2, 0}, {0, nullptr, 0, 0, 1, 0}, {0, "VALUE", 0, 3, 1, 0}, {0, "VALUE", 0, 2, 1, 1}
  };
  const UINT strip_strides[] = {20, 8};
  const D3D12_SO_DECLARATION_ENTRY stream_entries[] = {{0, "VALUE", 0, 0, 1, 0}, {1, "VALUE", 0, 0, 1, 1}};
  const UINT stream_strides[] = {4, 4};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs)};
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> strips_pso, streams_pso;
  desc.GS = bytecode(gs_strips);
  desc.StreamOutput = {strip_entries, (UINT)std::size(strip_entries), strip_strides, 2, D3D12_SO_NO_RASTERIZED_STREAM};
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&strips_pso)));
  desc.GS = bytecode(gs_streams);
  desc.StreamOutput = {stream_entries, (UINT)std::size(stream_entries), stream_strides, 2, D3D12_SO_NO_RASTERIZED_STREAM};
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&streams_pso)));
  // no geometry shader: the strip rasterizes stream 0 as well, the points do not
  const D3D12_SO_DECLARATION_ENTRY alone_entries[] = {{0, "VALUE", 0, 0, 4, 0}};
  const UINT alone_strides[] = {16}, size = 4;
  ComPtr<ID3D12PipelineState> alone_strip_pso, alone_points_pso;
  desc.VS = bytecode(vs_alone);
  desc.GS = {};
  desc.StreamOutput = {alone_entries, 1, alone_strides, 1, D3D12_SO_NO_RASTERIZED_STREAM};
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&alone_points_pso)));
  desc.StreamOutput.RasterizedStream = 0;
  desc.PS = bytecode(ps);
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&alone_strip_pso)));

  // the CPU's geometry shaders, in the API's order: each primitive's vertices' VALUE
  using Vertex = std::array<uint32_t, 4>;
  std::vector<std::array<Vertex, 3>> triangles;
  for (UINT instance = 0; instance < instances; instance++)
    for (UINT p = 0; p < points; p++)
      for (UINT i = 0; i < gs_instances; i++) {
        uint32_t id = p + instance * 1000, n = (id + i) % 9;
        for (uint32_t first = 0; first < n; first += 4) {
          uint32_t strip = std::min(n - first, 4u);
          for (uint32_t t = 0; t + 2 < strip; t++) {
            uint32_t odd = t & 1, at[3] = {t, t + 1 + odd, t + 2 - odd};
            std::array<Vertex, 3> triangle;
            for (int k = 0; k < 3; k++)
              triangle[k] = {id, i, first + at[k], 7};
            triangles.push_back(triangle);
          }
        }
      }
  // the vertex shader alone: the strip's triangles (the first four cover the target; long enough that a thread of an
  // object threadgroup sums several earlier ones), then each point of each instance
  const uint32_t strip_vertices = 1300;
  std::vector<Vertex> alone;
  for (uint32_t t = 0; t + 2 < strip_vertices; t++)
    for (uint32_t odd = t & 1, k = 0; k < 3; k++) {
      uint32_t id = k == 0 ? t : k == 1 ? t + 1 + odd : t + 2 - odd;
      alone.push_back({id, 0, id * 3, 9});
    }
  for (UINT instance = 0; instance < instances; instance++)
    for (UINT p = 0; p < points; p++)
      alone.push_back({p, instance, p * 3, 9});
  std::vector<uint32_t> stream0, stream1;
  for (UINT instance = 0; instance < instances; instance++)
    for (UINT p = 0; p < points; p++) {
      uint32_t id = p + instance * 1000;
      stream0.push_back(id);
      for (uint32_t j = 0; j < id % 3; j++)
        stream1.push_back(id * 10 + j);
    }
  // the same from many points, and the vertex shader's points from as many
  const UINT many_points = 40000;
  std::vector<uint32_t> many0, many1;
  std::vector<Vertex> many_alone;
  for (UINT instance = 0; instance < instances; instance++)
    for (UINT p = 0; p < many_points; p++) {
      uint32_t id = p + instance * 1000;
      many0.push_back(id);
      for (uint32_t j = 0; j < id % 3; j++)
        many1.push_back(id * 10 + j);
      many_alone.push_back({p, instance, p * 3, 9});
    }

  // one buffer: slot 0, room for three draws after a triangle already there; slot 1, room for two; the third draw's
  // small slot 1, room for `fits` triangles and a little more; the two stream buffers; the vertex shader's; the many
  // points'; the filled sizes
  const UINT64 sentinel = 0xa5a5a5a5u, start0 = 3 * strip_strides[0], fits = 37;
  const UINT64 slot0_bytes = start0 + 3 * strip_strides[0] * (triangles.size() * 2 + fits),
               slot1_bytes = 3 * strip_strides[1] * triangles.size() * 2,
               small_bytes = 3 * strip_strides[1] * fits + 4, stream_bytes = 4 * std::max(stream0.size(), stream1.size());
  const UINT64 slot0_at = 0, slot1_at = slot0_at + slot0_bytes, small_at = slot1_at + slot1_bytes,
               stream0_at = small_at + small_bytes, stream1_at = stream0_at + stream_bytes,
               alone_at = stream1_at + stream_bytes, alone_bytes = 16 * alone.size(),
               many0_at = alone_at + alone_bytes, many1_at = many0_at + 4 * many0.size(),
               many_alone_at = many1_at + 4 * many1.size(), many_alone_bytes = 16 * many_alone.size(),
               filled_at = (many_alone_at + many_alone_bytes + 7) & ~7ull, total = filled_at + 8 * 9;
  auto out = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, total, D3D12_RESOURCE_STATE_COPY_DEST);
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
  uint8_t *init;
  CHECK(upload->Map(0, nullptr, (void **)&init));
  std::fill_n(reinterpret_cast<uint32_t *>(init), filled_at / 4, sentinel);
  // filled sizes: the strip slots (slot 0 past a triangle), the small slot, the two streams, the vertex shader's, the
  // many points'
  const UINT64 filled[9] = {start0};
  memcpy(init + filled_at, filled, sizeof(filled));

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), strips_pso.Get(), IID_PPV_ARGS(&list)));
  list->CopyBufferRegion(out.Get(), 0, upload.Get(), 0, total);
  transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
  auto base = out->GetGPUVirtualAddress();
  auto view = [&](UINT64 at, UINT64 bytes, UINT filled_index) {
    return D3D12_STREAM_OUTPUT_BUFFER_VIEW{base + at, bytes, base + filled_at + 8 * filled_index};
  };
  ComPtr<ID3D12QueryHeap> statistics;
  D3D12_QUERY_HEAP_DESC statistics_desc{D3D12_QUERY_HEAP_TYPE_SO_STATISTICS, 2};
  CHECK(device->CreateQueryHeap(&statistics_desc, IID_PPV_ARGS(&statistics)));
  auto statistics_out = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 2 * sizeof(D3D12_QUERY_DATA_SO_STATISTICS), D3D12_RESOURCE_STATE_COPY_DEST);
  list->SetGraphicsRootSignature(rs.Get());
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
  list->BeginQuery(statistics.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0);
  // two draws append; the third, slot 0 still appending, overflows the small slot 1 after `fits` triangles
  D3D12_STREAM_OUTPUT_BUFFER_VIEW strip_views[] = {view(slot0_at, slot0_bytes, 0), view(slot1_at, slot1_bytes, 1)};
  list->SOSetTargets(0, 2, strip_views);
  list->DrawInstanced(points, instances, 0, 0);
  list->DrawInstanced(points, instances, 0, 0);
  D3D12_STREAM_OUTPUT_BUFFER_VIEW small_views[] = {view(slot0_at, slot0_bytes, 0), view(small_at, small_bytes, 2)};
  list->SOSetTargets(0, 2, small_views);
  list->DrawInstanced(points, instances, 0, 0);
  list->EndQuery(statistics.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0);
  list->SetPipelineState(streams_pso.Get());
  D3D12_STREAM_OUTPUT_BUFFER_VIEW stream_views[] = {view(stream0_at, stream_bytes, 3), view(stream1_at, stream_bytes, 4)};
  list->SOSetTargets(0, 2, stream_views);
  list->BeginQuery(statistics.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM1, 1);
  list->DrawInstanced(points, instances, 0, 0);
  list->EndQuery(statistics.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM1, 1);
  D3D12_STREAM_OUTPUT_BUFFER_VIEW many_views[] = {view(many0_at, 4 * many0.size(), 6), view(many1_at, 4 * many1.size(), 7)};
  list->SOSetTargets(0, 2, many_views);
  list->DrawInstanced(many_points, instances, 0, 0);
  list->ResolveQueryData(statistics.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0, 2, statistics_out.Get(), 0);
  // the vertex shader alone, into one buffer: the strip, drawn too, then the points
  ComPtr<ID3D12Resource> target;
  D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  CHECK(device->CreateCommittedResource(
      &default_heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  const float clear[4] = {};
  list->ClearRenderTargetView(rtv, clear, 0, nullptr);
  D3D12_VIEWPORT viewport{0, 0, (float)size, (float)size, 0, 1};
  D3D12_RECT scissor{0, 0, (LONG)size, (LONG)size};
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  D3D12_STREAM_OUTPUT_BUFFER_VIEW alone_view = view(alone_at, alone_bytes, 5);
  list->SOSetTargets(0, 1, &alone_view);
  list->SetPipelineState(alone_strip_pso.Get());
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  list->DrawInstanced(strip_vertices, 1, 0, 0);
  list->SetPipelineState(alone_points_pso.Get());
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
  list->DrawInstanced(points, instances, 0, 0);
  D3D12_STREAM_OUTPUT_BUFFER_VIEW many_alone_view = view(many_alone_at, many_alone_bytes, 8);
  list->SOSetTargets(0, 1, &many_alone_view);
  list->DrawInstanced(many_points, instances, 0, 0);
  transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(), 0, out.Get(), 0, total);
  transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  auto pixels = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT * size, D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_TEXTURE_COPY_LOCATION pixels_dst{pixels.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
      pixels_src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  pixels_dst.PlacedFootprint = {0, {DXGI_FORMAT_R32_UINT, size, size, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT}};
  list->CopyTextureRegion(&pixels_dst, 0, 0, 0, &pixels_src, nullptr);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  uint8_t *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));

  unsigned failures = 0;
  auto expect = [&](const char *what, uint64_t value, uint64_t want) {
    if (value != want && failures++ < 12)
      printf("%s: %#llx, want %#llx\n", what, (unsigned long long)value, (unsigned long long)want);
  };
  auto word = [&](UINT64 at) { return *reinterpret_cast<const uint32_t *>(got + at); };
  auto filled_size = [&](UINT index) { return *reinterpret_cast<const uint64_t *>(got + filled_at + 8 * index); };
  // the strip slots: the first two draws' triangles twice after the one already there, then the third draw's
  // `fits` (its slot 1 was full after them, which stops slot 0 too)
  const UINT64 written = triangles.size() * 2 + fits;
  for (UINT64 v = 0; v < 3 * (written + 1); v++) {
    UINT64 n = v / 3 - 1;
    bool before = v < 3;
    auto &want = triangles[before ? 0 : n % triangles.size()][v % 3];
    UINT64 at = slot0_at + v * strip_strides[0];
    expect("slot 0 x", word(at), before ? sentinel : want[0]);
    expect("slot 0 y", word(at + 4), before ? sentinel : want[1]);
    expect("slot 0 gap", word(at + 8), sentinel);
    expect("slot 0 w", word(at + 12), before ? sentinel : want[3]);
    expect("slot 0 past the elements", word(at + 16), sentinel);
  }
  for (UINT64 v = 0; v < 3 * triangles.size() * 2; v++) {
    expect("slot 1 z", word(slot1_at + v * strip_strides[1]), triangles[v / 3 % triangles.size()][v % 3][2]);
    expect("slot 1 past the element", word(slot1_at + v * strip_strides[1] + 4), sentinel);
  }
  for (UINT64 v = 0; v < 3 * fits; v++)
    expect("small slot z", word(small_at + v * strip_strides[1]), triangles[v / 3][v % 3][2]);
  expect("small slot past the fit", word(small_at + 3 * fits * strip_strides[1]), sentinel);
  expect("slot 0 filled", filled_size(0), start0 + 3 * strip_strides[0] * written);
  expect("slot 1 filled", filled_size(1), 3 * strip_strides[1] * triangles.size() * 2);
  expect("small slot filled", filled_size(2), 3 * strip_strides[1] * fits);
  for (size_t v = 0; v < stream0.size(); v++)
    expect("stream 0", word(stream0_at + 4 * v), stream0[v]);
  for (size_t v = 0; v < stream1.size(); v++)
    expect("stream 1", word(stream1_at + 4 * v), stream1[v]);
  expect("stream 0 filled", filled_size(3), 4 * stream0.size());
  expect("stream 1 filled", filled_size(4), 4 * stream1.size());
  for (size_t v = 0; v < alone.size(); v++)
    for (int c = 0; c < 4; c++)
      expect("vertex shader alone", word(alone_at + 16 * v + 4 * c), alone[v][c]);
  expect("vertex shader alone filled", filled_size(5), alone_bytes);
  for (size_t v = 0; v < many0.size(); v++)
    expect("many points, stream 0", word(many0_at + 4 * v), many0[v]);
  for (size_t v = 0; v < many1.size(); v++)
    expect("many points, stream 1", word(many1_at + 4 * v), many1[v]);
  for (size_t v = 0; v < many_alone.size(); v++)
    for (int c = 0; c < 4; c++)
      expect("many points, vertex shader alone", word(many_alone_at + 16 * v + 4 * c), many_alone[v][c]);
  expect("many points, stream 0 filled", filled_size(6), 4 * many0.size());
  expect("many points, stream 1 filled", filled_size(7), 4 * many1.size());
  expect("many points, vertex shader alone filled", filled_size(8), many_alone_bytes);
  D3D12_QUERY_DATA_SO_STATISTICS *counted;
  CHECK(statistics_out->Map(0, nullptr, (void **)&counted));
  expect("stream 0 primitives written", counted[0].NumPrimitivesWritten, written);
  expect("stream 0 primitives needed", counted[0].PrimitivesStorageNeeded, 3 * triangles.size());
  expect("stream 1 primitives written", counted[1].NumPrimitivesWritten, stream1.size());
  expect("stream 1 primitives needed", counted[1].PrimitivesStorageNeeded, stream1.size());
  uint8_t *drawn;
  CHECK(pixels->Map(0, nullptr, (void **)&drawn));
  for (UINT y = 0; y < size; y++)
    for (UINT x = 0; x < size; x++)
      expect("drawn pixel", reinterpret_cast<const uint32_t *>(drawn + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT * y)[x], 1);
  if (failures) {
    printf("failed: %u wrong values\n", failures);
    return 1;
  }
  printf("passed: %zu triangles per draw, %zu + %zu stream points, %zu vertices without a geometry shader\n",
         triangles.size(), stream0.size(), stream1.size(), alone.size());
  return 0;
}
