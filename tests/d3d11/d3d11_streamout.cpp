// contract: stream output writes every primitive the stage before it completes, in the API's order (draw instance,
// input primitive, geometry instance, then emission), as a list: a triangle strip's odd triangles as (n, n + 2, n + 1),
// and RestartStrip starting a new strip. each element goes to its slot at its offset in the vertex, at the slot's
// stride, gaps and the rest of the stride untouched. SOSetTargets starts a buffer at its offset, and -1 appends after
// what earlier draws filled. a stream stops at the first primitive that does not fit whole in all its buffers (D3D11.3
// functional spec, 14.5). streams count apart: a point geometry shader emits to two streams, each to its own buffer.
// a geometry shader fed a long triangle strip streams out a point per triangle, in the strip's order.
// CreateGeometryShaderWithStreamOutput with a vertex shader streams that shader's primitives out as lists: a long
// triangle strip, which also rasterizes over the whole target, and points over two instances. the draws span several
// object threadgroups, and the expectations are the same shaders run on the CPU. the queries count per stream the
// primitives written and those that needed room, between their Begin and End, overlapping ones included; an overflow
// predicate is whether the two differ, the unnumbered one for any stream (D3D11.3 functional spec, 20.4).
// DrawInstancedIndirect and DrawIndexedInstancedIndirect take the same arguments from a buffer (8.7, 8.8): the strips
// and the vertex shader's points stream out from them as from the direct draws, the strips a second time indexed.
// the stream points and the vertex shader's points, drawn again from many vertices, span thousands of object
// threadgroups, as many draws of a game do.
#include "d3d11_test.hpp"
#include <array>

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
main() {
  const UINT points = 100, instances = 2, gs_instances = 2;
  std::vector<std::string> defines = {"INSTANCES=" + std::to_string(gs_instances)};
  auto vs = compile(hlsl, "vs", "vs", defines), gs_strips = compile(hlsl, "gs_strips", "gs", defines),
       gs_triangles = compile(hlsl, "gs_triangles", "gs", defines),
       gs_streams = compile(hlsl, "gs_streams", "gs", defines), vs_alone = compile(hlsl, "vs_alone", "vs", defines),
       ps = compile(hlsl, "ps", "ps", defines);
  if (!vs || !gs_strips || !gs_triangles || !gs_streams || !vs_alone || !ps) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context));

  // slot 0: VALUE.xy, a gap, VALUE.w, in 16 of a 20-byte stride; slot 1: VALUE.z in 4 of 8
  const D3D11_SO_DECLARATION_ENTRY strip_entries[] = {
      {0, "VALUE", 0, 0, 2, 0}, {0, nullptr, 0, 0, 1, 0}, {0, "VALUE", 0, 3, 1, 0}, {0, "VALUE", 0, 2, 1, 1}
  };
  const UINT strip_strides[] = {20, 8};
  const D3D11_SO_DECLARATION_ENTRY stream_entries[] = {{0, "VALUE", 0, 0, 1, 0}, {1, "VALUE", 0, 0, 1, 1}};
  const UINT stream_strides[] = {4, 4};
  const D3D11_SO_DECLARATION_ENTRY alone_entries[] = {{0, "VALUE", 0, 0, 4, 0}};
  const UINT alone_strides[] = {16}, size = 4;
  ComPtr<ID3D11VertexShader> vertex, vertex_alone;
  ComPtr<ID3D11GeometryShader> strips, triangles_out, streams, alone_points, alone_strip;
  ComPtr<ID3D11PixelShader> pixel;
  CHECK(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex));
  CHECK(device->CreateVertexShader(vs_alone->GetBufferPointer(), vs_alone->GetBufferSize(), nullptr, &vertex_alone));
  CHECK(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel));
  CHECK(device->CreateGeometryShaderWithStreamOutput(
      gs_strips->GetBufferPointer(), gs_strips->GetBufferSize(), strip_entries, std::size(strip_entries), strip_strides,
      2, D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &strips
  ));
  CHECK(device->CreateGeometryShaderWithStreamOutput(
      gs_streams->GetBufferPointer(), gs_streams->GetBufferSize(), stream_entries, std::size(stream_entries),
      stream_strides, 2, D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &streams
  ));
  CHECK(device->CreateGeometryShaderWithStreamOutput(
      gs_triangles->GetBufferPointer(), gs_triangles->GetBufferSize(), alone_entries, 1, alone_strides, 1,
      D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &triangles_out
  ));
  // no geometry shader: the strip rasterizes stream 0 as well, the points do not
  CHECK(device->CreateGeometryShaderWithStreamOutput(
      vs_alone->GetBufferPointer(), vs_alone->GetBufferSize(), alone_entries, 1, alone_strides, 1,
      D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &alone_points
  ));
  CHECK(device->CreateGeometryShaderWithStreamOutput(
      vs_alone->GetBufferPointer(), vs_alone->GetBufferSize(), alone_entries, 1, alone_strides, 1, 0, nullptr,
      &alone_strip
  ));

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
  // the strip into a geometry shader: a point per triangle
  std::vector<Vertex> from_strip;
  for (uint32_t t = 0; t + 2 < strip_vertices; t++)
    from_strip.push_back({t, t + 1 + (t & 1), t + 2 - (t & 1), t});
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

  // slot 0, room for three draws after a triangle's worth it starts past; slot 1, room for two; the third draw's small
  // slot 1, room for `fits` triangles and a little more; the two stream buffers; the vertex shader's
  const UINT sentinel = 0xa5a5a5a5u, start0 = 3 * strip_strides[0], fits = 37;
  const UINT stream_bytes = 4 * std::max(stream0.size(), stream1.size());
  auto target = [&](UINT bytes) { return buffer(device.Get(), bytes, D3D11_BIND_STREAM_OUTPUT, sentinel); };
  auto slot0 = target(start0 + 3 * strip_strides[0] * (triangles.size() * 2 + fits) + 4),
       slot1 = target(3 * strip_strides[1] * triangles.size() * 2 + 4), small = target(3 * strip_strides[1] * fits + 4),
       out0 = target(stream_bytes + 4), out1 = target(stream_bytes + 4), out_alone = target(16 * alone.size() + 4),
       out_strip = target(16 * from_strip.size() + 4);

  D3D11_TEXTURE2D_DESC texture_desc{size, size, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
  ComPtr<ID3D11Texture2D> texture, texture_read;
  ComPtr<ID3D11RenderTargetView> rtv;
  CHECK(device->CreateTexture2D(&texture_desc, nullptr, &texture));
  CHECK(device->CreateRenderTargetView(texture.Get(), nullptr, &rtv));
  const float clear[4] = {};
  context->ClearRenderTargetView(rtv.Get(), clear);
  D3D11_VIEWPORT viewport{0, 0, (float)size, (float)size, 0, 1};
  context->RSSetViewports(1, &viewport);
  D3D11_RASTERIZER_DESC raster_desc{D3D11_FILL_SOLID, D3D11_CULL_NONE};
  raster_desc.DepthClipEnable = TRUE;
  ComPtr<ID3D11RasterizerState> raster;
  CHECK(device->CreateRasterizerState(&raster_desc, &raster));
  context->RSSetState(raster.Get());

  const UINT append = ~0u;
  auto targets = [&](ID3D11Buffer *a, UINT a_offset, ID3D11Buffer *b = nullptr, UINT b_offset = 0) {
    ID3D11Buffer *buffers[] = {a, b};
    UINT offsets[] = {a_offset, b_offset};
    context->SOSetTargets(b ? 2 : 1, buffers, offsets);
  };
  context->VSSetShader(vertex.Get(), nullptr, 0);
  context->GSSetShader(strips.Get(), nullptr, 0);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
  auto query = [&](D3D11_QUERY type) {
    ComPtr<ID3D11Query> q;
    D3D11_QUERY_DESC desc{type};
    device->CreateQuery(&desc, &q);
    return q;
  };
  auto strip_statistics = query(D3D11_QUERY_SO_STATISTICS), fitting = query(D3D11_QUERY_SO_OVERFLOW_PREDICATE),
       overflowing = query(D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0),
       stream1_statistics = query(D3D11_QUERY_SO_STATISTICS_STREAM1),
       other_stream = query(D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM1);
  // two draws, the second appending; the third, slot 0 still appending, overflows the small slot 1 after `fits`
  context->Begin(strip_statistics.Get());
  context->Begin(other_stream.Get());
  context->Begin(fitting.Get());
  targets(slot0.Get(), start0, slot1.Get(), 0);
  context->DrawInstanced(points, instances, 0, 0);
  context->End(fitting.Get());
  targets(slot0.Get(), append, slot1.Get(), append);
  context->DrawInstanced(points, instances, 0, 0);
  context->Begin(overflowing.Get());
  targets(slot0.Get(), append, small.Get(), 0);
  context->DrawInstanced(points, instances, 0, 0);
  context->End(overflowing.Get());
  context->End(other_stream.Get());
  context->End(strip_statistics.Get());
  context->GSSetShader(streams.Get(), nullptr, 0);
  targets(out0.Get(), 0, out1.Get(), 0);
  context->Begin(stream1_statistics.Get());
  context->DrawInstanced(points, instances, 0, 0);
  context->End(stream1_statistics.Get());
  auto out_many0 = target(4 * many0.size() + 4), out_many1 = target(4 * many1.size() + 4),
       out_many_alone = target(16 * many_alone.size() + 4);
  targets(out_many0.Get(), 0, out_many1.Get(), 0);
  context->DrawInstanced(many_points, instances, 0, 0);
  context->GSSetShader(triangles_out.Get(), nullptr, 0);
  targets(out_strip.Get(), 0);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  context->Draw(strip_vertices, 0);
  // the vertex shader alone, into one buffer: the strip, drawn too, then the points
  context->VSSetShader(vertex_alone.Get(), nullptr, 0);
  context->PSSetShader(pixel.Get(), nullptr, 0);
  context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
  context->GSSetShader(alone_strip.Get(), nullptr, 0);
  targets(out_alone.Get(), 0);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  context->Draw(strip_vertices, 0);
  context->GSSetShader(alone_points.Get(), nullptr, 0);
  targets(out_alone.Get(), append);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
  context->DrawInstanced(points, instances, 0, 0);
  targets(out_many_alone.Get(), 0);
  context->DrawInstanced(many_points, instances, 0, 0);
  // the points and the strips again, from arguments in a buffer: a draw's, then an indexed draw's that appends
  const UINT argument_words[] = {points, instances, 0, 0, points, instances, 0, 0, 0};
  D3D11_BUFFER_DESC arguments_desc{
      sizeof(argument_words), D3D11_USAGE_DEFAULT, 0, 0, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS
  };
  D3D11_SUBRESOURCE_DATA arguments_data{argument_words};
  ComPtr<ID3D11Buffer> arguments, indices;
  CHECK(device->CreateBuffer(&arguments_desc, &arguments_data, &arguments));
  std::vector<UINT16> index_words(points);
  for (UINT p = 0; p < points; p++)
    index_words[p] = p;
  D3D11_BUFFER_DESC index_desc{UINT(points * sizeof(UINT16)), D3D11_USAGE_DEFAULT, D3D11_BIND_INDEX_BUFFER};
  D3D11_SUBRESOURCE_DATA index_data{index_words.data()};
  CHECK(device->CreateBuffer(&index_desc, &index_data, &indices));
  auto indirect0 = target(3 * strip_strides[0] * triangles.size() * 2 + 4),
       indirect1 = target(3 * strip_strides[1] * triangles.size() * 2 + 4),
       indirect_alone = target(16 * instances * points + 4);
  auto indirect_statistics = query(D3D11_QUERY_SO_STATISTICS);
  targets(indirect_alone.Get(), 0);
  context->DrawInstancedIndirect(arguments.Get(), 0);
  context->VSSetShader(vertex.Get(), nullptr, 0);
  context->GSSetShader(strips.Get(), nullptr, 0);
  context->IASetIndexBuffer(indices.Get(), DXGI_FORMAT_R16_UINT, 0);
  context->Begin(indirect_statistics.Get());
  targets(indirect0.Get(), 0, indirect1.Get(), 0);
  context->DrawInstancedIndirect(arguments.Get(), 0);
  targets(indirect0.Get(), append, indirect1.Get(), append);
  context->DrawIndexedInstancedIndirect(arguments.Get(), 4 * sizeof(UINT));
  context->End(indirect_statistics.Get());
  context->SOSetTargets(0, nullptr, nullptr);

  // the queries from a deferred context's list: a draw that fits both its buffers, and one past its second
  ComPtr<ID3D11DeviceContext> deferred;
  CHECK(device->CreateDeferredContext(0, &deferred));
  auto listed_statistics = query(D3D11_QUERY_SO_STATISTICS), listed_fitting = query(D3D11_QUERY_SO_OVERFLOW_PREDICATE),
       listed_overflowing = query(D3D11_QUERY_SO_OVERFLOW_PREDICATE);
  auto listed0 = target(3 * strip_strides[0] * triangles.size() * 2 + 4),
       listed1 = target(3 * strip_strides[1] * triangles.size() + 4), listed_small = target(3 * strip_strides[1] * fits + 4);
  auto listed_targets = [&](ID3D11Buffer *a, UINT a_offset, ID3D11Buffer *b) {
    ID3D11Buffer *buffers[] = {a, b};
    UINT offsets[] = {a_offset, 0};
    deferred->SOSetTargets(2, buffers, offsets);
  };
  deferred->VSSetShader(vertex.Get(), nullptr, 0);
  deferred->GSSetShader(strips.Get(), nullptr, 0);
  deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
  deferred->Begin(listed_statistics.Get());
  deferred->Begin(listed_fitting.Get());
  listed_targets(listed0.Get(), 0, listed1.Get());
  deferred->DrawInstanced(points, instances, 0, 0);
  deferred->End(listed_fitting.Get());
  deferred->Begin(listed_overflowing.Get());
  listed_targets(listed0.Get(), append, listed_small.Get());
  deferred->DrawInstanced(points, instances, 0, 0);
  deferred->End(listed_overflowing.Get());
  deferred->End(listed_statistics.Get());
  ComPtr<ID3D11CommandList> list;
  CHECK(deferred->FinishCommandList(FALSE, &list));
  // a call the immediate context refuses before the list has run: Wine's test_deferred_context_queries records
  // DXGI_ERROR_INVALID_CALL from Windows between FinishCommandList and ExecuteCommandList
  D3D11_QUERY_DATA_SO_STATISTICS early{};
  HRESULT before_list = context->GetData(listed_statistics.Get(), &early, sizeof(early), D3D11_ASYNC_GETDATA_DONOTFLUSH);
  context->ExecuteCommandList(list.Get(), FALSE);

  unsigned failures = 0;
  auto expect = [&](const char *what, uint64_t value, uint64_t want) {
    if (value != want && failures++ < 12)
      printf("%s: %#llx, want %#llx\n", what, (unsigned long long)value, (unsigned long long)want);
  };
  auto got0 = read(device.Get(), context.Get(), slot0.Get()), got1 = read(device.Get(), context.Get(), slot1.Get()),
       got_small = read(device.Get(), context.Get(), small.Get()), got_s0 = read(device.Get(), context.Get(), out0.Get()),
       got_s1 = read(device.Get(), context.Get(), out1.Get()), got_alone = read(device.Get(), context.Get(), out_alone.Get()),
       got_strip = read(device.Get(), context.Get(), out_strip.Get());
  // the strip slots: the first two draws' triangles twice after the start, then the third draw's `fits` (its slot 1
  // was full after them, which stops slot 0 too)
  const UINT64 written = triangles.size() * 2 + fits;
  for (UINT64 v = 0; v < 3 * (written + 1); v++) {
    bool before = v < 3;
    auto &want = triangles[before ? 0 : (v / 3 - 1) % triangles.size()][v % 3];
    auto at = v * strip_strides[0] / 4;
    expect("slot 0 x", got0[at], before ? sentinel : want[0]);
    expect("slot 0 y", got0[at + 1], before ? sentinel : want[1]);
    expect("slot 0 gap", got0[at + 2], sentinel);
    expect("slot 0 w", got0[at + 3], before ? sentinel : want[3]);
    expect("slot 0 past the elements", got0[at + 4], sentinel);
  }
  expect("slot 0 past the third draw", got0[3 * (written + 1) * strip_strides[0] / 4], sentinel);
  for (UINT64 v = 0; v < 3 * triangles.size() * 2; v++) {
    expect("slot 1 z", got1[v * strip_strides[1] / 4], triangles[v / 3 % triangles.size()][v % 3][2]);
    expect("slot 1 past the element", got1[v * strip_strides[1] / 4 + 1], sentinel);
  }
  expect("slot 1 past the second draw", got1[3 * triangles.size() * 2 * strip_strides[1] / 4], sentinel);
  for (UINT64 v = 0; v < 3 * fits; v++)
    expect("small slot z", got_small[v * strip_strides[1] / 4], triangles[v / 3][v % 3][2]);
  expect("small slot past the fit", got_small[3 * fits * strip_strides[1] / 4], sentinel);
  for (size_t v = 0; v < stream0.size(); v++)
    expect("stream 0", got_s0[v], stream0[v]);
  expect("stream 0 past the draw", got_s0[stream0.size()], sentinel);
  for (size_t v = 0; v < stream1.size(); v++)
    expect("stream 1", got_s1[v], stream1[v]);
  expect("stream 1 past the draw", got_s1[stream1.size()], sentinel);
  for (size_t v = 0; v < from_strip.size(); v++)
    for (int c = 0; c < 4; c++)
      expect("a strip's triangles", got_strip[4 * v + c], from_strip[v][c]);
  expect("a strip's triangles past the draw", got_strip[4 * from_strip.size()], sentinel);
  for (size_t v = 0; v < alone.size(); v++)
    for (int c = 0; c < 4; c++)
      expect("vertex shader alone", got_alone[4 * v + c], alone[v][c]);
  expect("vertex shader alone past the draws", got_alone[4 * alone.size()], sentinel);
  auto got_many0 = read(device.Get(), context.Get(), out_many0.Get()),
       got_many1 = read(device.Get(), context.Get(), out_many1.Get()),
       got_many_alone = read(device.Get(), context.Get(), out_many_alone.Get());
  for (size_t v = 0; v < many0.size(); v++)
    expect("many points, stream 0", got_many0[v], many0[v]);
  expect("many points, stream 0 past the draw", got_many0[many0.size()], sentinel);
  for (size_t v = 0; v < many1.size(); v++)
    expect("many points, stream 1", got_many1[v], many1[v]);
  expect("many points, stream 1 past the draw", got_many1[many1.size()], sentinel);
  for (size_t v = 0; v < many_alone.size(); v++)
    for (int c = 0; c < 4; c++)
      expect("many points, vertex shader alone", got_many_alone[4 * v + c], many_alone[v][c]);
  expect("many points, vertex shader alone past the draw", got_many_alone[4 * many_alone.size()], sentinel);
  // from arguments: the strips twice, and the vertex shader's points, the last of `alone`
  auto got_indirect0 = read(device.Get(), context.Get(), indirect0.Get()),
       got_indirect1 = read(device.Get(), context.Get(), indirect1.Get()),
       got_indirect_alone = read(device.Get(), context.Get(), indirect_alone.Get());
  for (UINT64 v = 0; v < 3 * triangles.size() * 2; v++) {
    auto &want = triangles[v / 3 % triangles.size()][v % 3];
    auto at = v * strip_strides[0] / 4;
    expect("indirect slot 0 x", got_indirect0[at], want[0]);
    expect("indirect slot 0 y", got_indirect0[at + 1], want[1]);
    expect("indirect slot 0 w", got_indirect0[at + 3], want[3]);
    expect("indirect slot 1 z", got_indirect1[v * strip_strides[1] / 4], want[2]);
  }
  expect("indirect slot 0 past the draws", got_indirect0[3 * triangles.size() * 2 * strip_strides[0] / 4], sentinel);
  expect("indirect slot 1 past the draws", got_indirect1[3 * triangles.size() * 2 * strip_strides[1] / 4], sentinel);
  const size_t first_point = alone.size() - instances * points;
  for (size_t v = 0; v < instances * points; v++)
    for (int c = 0; c < 4; c++)
      expect("indirect vertex shader alone", got_indirect_alone[4 * v + c], alone[first_point + v][c]);
  expect("indirect vertex shader alone past the draw", got_indirect_alone[4 * instances * points], sentinel);

  auto result = [&](ID3D11Query *q, void *data, UINT size) {
    HRESULT hr;
    for (int tries = 0; (hr = context->GetData(q, data, size, 0)) == S_FALSE && tries < 1000; tries++)
      Sleep(1);
    if (hr != S_OK && failures++ < 12)
      printf("query result: %#lx\n", (unsigned long)hr);
  };
  D3D11_QUERY_DATA_SO_STATISTICS counted{};
  BOOL overflowed = 2;
  result(strip_statistics.Get(), &counted, sizeof(counted));
  expect("stream 0 primitives written", counted.NumPrimitivesWritten, written);
  expect("stream 0 primitives needed", counted.PrimitivesStorageNeeded, 3 * triangles.size());
  result(stream1_statistics.Get(), &counted, sizeof(counted));
  expect("stream 1 primitives written", counted.NumPrimitivesWritten, stream1.size());
  expect("stream 1 primitives needed", counted.PrimitivesStorageNeeded, stream1.size());
  result(fitting.Get(), &overflowed, sizeof(overflowed));
  expect("a draw that fits overflowed", overflowed, FALSE);
  result(overflowing.Get(), &overflowed, sizeof(overflowed));
  expect("a draw past its buffer overflowed", overflowed, TRUE);
  result(other_stream.Get(), &overflowed, sizeof(overflowed));
  expect("a stream without primitives overflowed", overflowed, FALSE);
  expect("the result of a query whose list has not run", before_list, DXGI_ERROR_INVALID_CALL);
  result(listed_statistics.Get(), &counted, sizeof(counted));
  expect("primitives written from a list", counted.NumPrimitivesWritten, triangles.size() + fits);
  expect("primitives needed from a list", counted.PrimitivesStorageNeeded, 2 * triangles.size());
  result(listed_fitting.Get(), &overflowed, sizeof(overflowed));
  expect("a list's draw that fits overflowed", overflowed, FALSE);
  result(listed_overflowing.Get(), &overflowed, sizeof(overflowed));
  expect("a list's draw past its buffer overflowed", overflowed, TRUE);
  result(indirect_statistics.Get(), &counted, sizeof(counted));
  expect("primitives written from arguments", counted.NumPrimitivesWritten, 2 * triangles.size());
  expect("primitives needed from arguments", counted.PrimitivesStorageNeeded, 2 * triangles.size());

  texture_desc.Usage = D3D11_USAGE_STAGING;
  texture_desc.BindFlags = 0;
  texture_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  CHECK(device->CreateTexture2D(&texture_desc, nullptr, &texture_read));
  context->CopyResource(texture_read.Get(), texture.Get());
  D3D11_MAPPED_SUBRESOURCE mapped;
  CHECK(context->Map(texture_read.Get(), 0, D3D11_MAP_READ, 0, &mapped));
  for (UINT y = 0; y < size; y++)
    for (UINT x = 0; x < size; x++)
      expect("drawn pixel", reinterpret_cast<const uint32_t *>((const char *)mapped.pData + mapped.RowPitch * y)[x], 1);
  if (failures) {
    printf("failed: %u wrong values\n", failures);
    return 1;
  }
  printf("passed: %zu triangles per draw, %zu + %zu stream points, %zu vertices without a geometry shader\n",
         triangles.size(), stream0.size(), stream1.size(), alone.size());
  return 0;
}
