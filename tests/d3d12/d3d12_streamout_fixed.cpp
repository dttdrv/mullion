// contract: stream output writes every primitive that reaches it, in the API's order (draw instance, input
// primitive, geometry instance, then emission), as a list: a strip's lines and triangles one by one, odd triangles as
// (n, n + 2, n + 1), and a cut or RestartStrip starting a new strip. each element goes to its slot at its offset in
// the vertex, at the slot's stride, gaps and the rest of the stride untouched, from the buffer's BufferFilledSize,
// which moves on by what was written, so the next draw appends. a stream stops at the first primitive that does not
// fit whole in all its buffers; a statistics query counts the primitives written and those that would have been
// (D3D11.3 functional specification, 14.5, 14.7: https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm).
// the draws here are those whose primitives the draw alone decides: no index cuts a strip, and a geometry shader, if
// any, emits the same on every invocation (Mullion then writes them in one pass, a vertex shader's from the thread
// that ran each vertex). without a geometry shader: points from a start vertex and a start instance, line strips over
// several object threadgroups and three instances, line and triangle lists with a vertex left over, a triangle strip
// over several object threadgroups, an indexed strip whose 0xFFFF is a vertex without a cut value, and with one
// (IBStripCutValue) a cut, and a draw that stops when its second slot is full. with one: a triangle strip of two
// strips per invocation from two geometry instances, two point streams from three, the second filling its buffer,
// and points from a line strip's lines. a stream output draw that rasterizes nothing, followed in its pass by one
// that rasterizes without attachments, must leave that one every pixel of its viewport (D3D12 allows a pipeline
// without render targets; its pixel shader writes an unordered access view). every case runs as a direct draw and
// through ExecuteIndirect, and each list runs twice. the expectations are the same shaders and input assembly run on
// the CPU.
#include "d3d12_test.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>

static const char hlsl[] = R"hlsl(
struct In { uint v : V; uint inst : INST; };
struct O { float4 pos : SV_Position; uint4 value : VALUE; uint tag : TAG; };
O vs(In i) {
  O o;
  o.pos = float4(0, 0, 0, 1);
  o.value = uint4(i.v, i.inst, i.v ^ i.inst, 0x5a);
  o.tag = i.v * 3 + 1;
  return o;
}
O emitted(uint4 value) { O o; o.pos = float4(0, 0, 0, 1); o.value = value; o.tag = value.x ^ (value.w << 24); return o; }
// every invocation emits the same, without a branch: a strip of two triangles, a cut, and a strip of one
[maxvertexcount(7)] [instance(2)]
void gs_cut(triangle O v[3], uint i : SV_GSInstanceID, inout TriangleStream<O> s) {
  s.Append(emitted(uint4(v[0].value.x, v[0].value.y, i, 0)));
  s.Append(emitted(uint4(v[1].value.x, v[0].value.y, i, 1)));
  s.Append(emitted(uint4(v[2].value.x, v[0].value.y, i, 2)));
  s.Append(emitted(uint4(v[0].value.x, v[0].value.y, i, 3)));
  s.RestartStrip();
  s.Append(emitted(uint4(v[1].value.x, v[0].value.y, i, 4)));
  s.Append(emitted(uint4(v[2].value.x, v[0].value.y, i, 5)));
  s.Append(emitted(uint4(v[0].value.x, v[0].value.y, i, 6)));
}
// three points on stream 0 and two on stream 1, interleaved
[maxvertexcount(5)] [instance(3)]
void gs_streams(point O v[1], uint i : SV_GSInstanceID, inout PointStream<O> s0, inout PointStream<O> s1) {
  s0.Append(emitted(uint4(v[0].value.x, v[0].value.y, i, 0)));
  s1.Append(emitted(uint4(v[0].value.x, v[0].value.y, i, 10)));
  s0.Append(emitted(uint4(v[0].value.x, v[0].value.y, i, 1)));
  s1.Append(emitted(uint4(v[0].value.x, v[0].value.y, i, 11)));
  s0.Append(emitted(uint4(v[0].value.x, v[0].value.y, i, 2)));
}
// two points from each line
[maxvertexcount(2)]
void gs_lines(line O v[2], inout PointStream<O> s) {
  s.Append(emitted(uint4(v[0].value.x, v[1].value.x, v[0].value.y, 20)));
  s.Append(emitted(uint4(v[1].value.x, v[0].value.x, v[1].value.y, 21)));
}
// a triangle over the viewport, and a pixel shader that counts each pixel it runs for
RWStructuredBuffer<uint> covered : register(u0);
float4 vs_cover(uint id : SV_VertexID) : SV_Position { return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0, 1); }
void ps_cover(float4 at : SV_Position) { InterlockedAdd(covered[uint(at.y) * SIZE + uint(at.x)], 1); }
)hlsl";

namespace {
// VALUE, then TAG
using V4 = std::array<uint32_t, 5>;
// the vertex buffer's value of vertex k, and the instance buffer's of instance k
uint32_t vertex_value(uint64_t k) { return 0x10000u + uint32_t(k) * 7u; }
uint32_t instance_value(uint64_t k) { return 0x7000000u + uint32_t(k) * 0x100u; }
// what the geometry shaders emit
V4 emit(uint32_t x, uint32_t y, uint32_t z, uint32_t w) { return {x, y, z, w, x ^ (w << 24)}; }
// a declaration entry, in its slot's order: a component of VALUE (TAG is 4), or a gap (-1)
struct Element {
  uint32_t stream, slot;
  int component;
};
enum Kind { Alone, Cut, Streams, Lines };
} // namespace

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const UINT size = 64;
  std::vector<std::string> defines = {"SIZE=" + std::to_string(size)};
  auto vs = compiler.compile(hlsl, "vs", "vs", defines), gs_cut = compiler.compile(hlsl, "gs_cut", "gs", defines),
       gs_streams = compiler.compile(hlsl, "gs_streams", "gs", defines),
       gs_lines = compiler.compile(hlsl, "gs_lines", "gs", defines),
       vs_cover = compiler.compile(hlsl, "vs_cover", "vs", defines), ps_cover = compiler.compile(hlsl, "ps_cover", "ps", defines);
  if (vs.empty() || gs_cut.empty() || gs_streams.empty() || gs_lines.empty() || vs_cover.empty() || ps_cover.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER uav{D3D12_ROOT_PARAMETER_TYPE_UAV};
  auto rs = root_signature(
      device.Get(), {1, &uav, 0, nullptr,
                     D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT}
  );

  // slot 0: VALUE and a gap, in 20 bytes; slot 1: TAG in 4. the point streams: VALUE in 16 bytes each
  const std::vector<Element> one_stream = {{0, 0, 0}, {0, 0, 1}, {0, 0, 2}, {0, 0, 3}, {0, 0, -1}, {0, 1, 4}},
                             two_streams = {{0, 0, 0}, {0, 0, 1}, {0, 0, 2}, {0, 0, 3}, {1, 1, 0}, {1, 1, 1}, {1, 1, 2}, {1, 1, 3}};
  const D3D12_SO_DECLARATION_ENTRY one_entries[] = {{0, "VALUE", 0, 0, 4, 0}, {0, nullptr, 0, 0, 1, 0}, {0, "TAG", 0, 0, 1, 1}},
                                   two_entries[] = {{0, "VALUE", 0, 0, 4, 0}, {1, "VALUE", 0, 0, 4, 1}};
  const UINT one_strides[] = {20, 4}, two_strides[] = {16, 16};
  const D3D12_INPUT_ELEMENT_DESC inputs[] = {
      {"V", 0, DXGI_FORMAT_R32_UINT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"INST", 0, DXGI_FORMAT_R32_UINT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
  };
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs)};
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.InputLayout = {inputs, (UINT)std::size(inputs)};
  desc.SampleDesc = {1, 0};
  auto pipeline = [&](D3D12_PRIMITIVE_TOPOLOGY_TYPE type, D3D12_SHADER_BYTECODE gs, bool two,
                      D3D12_INDEX_BUFFER_STRIP_CUT_VALUE cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED) {
    auto d = desc;
    d.PrimitiveTopologyType = type;
    d.GS = gs;
    d.IBStripCutValue = cut;
    d.StreamOutput = two ? D3D12_STREAM_OUTPUT_DESC{two_entries, (UINT)std::size(two_entries), two_strides, 2,
                                                    D3D12_SO_NO_RASTERIZED_STREAM}
                         : D3D12_STREAM_OUTPUT_DESC{one_entries, (UINT)std::size(one_entries), one_strides, 2,
                                                    D3D12_SO_NO_RASTERIZED_STREAM};
    ComPtr<ID3D12PipelineState> pso;
    device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
    return pso;
  };
  auto points_pso = pipeline(D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT, {}, false),
       lines_pso = pipeline(D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE, {}, false),
       triangles_pso = pipeline(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, {}, false),
       cutting_pso = pipeline(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, {}, false, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF),
       cut_pso = pipeline(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, bytecode(gs_cut), false),
       streams_pso = pipeline(D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT, bytecode(gs_streams), true),
       lines_gs_pso = pipeline(D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE, bytecode(gs_lines), false);
  ComPtr<ID3D12PipelineState> cover_pso;
  {
    auto d = desc;
    d.VS = bytecode(vs_cover);
    d.PS = bytecode(ps_cover);
    d.InputLayout = {};
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&cover_pso));
  }
  const std::pair<const char *, ID3D12PipelineState *> made[] = {
      {"points", points_pso.Get()}, {"lines", lines_pso.Get()}, {"triangles", triangles_pso.Get()},
      {"cut triangles", cutting_pso.Get()}, {"two strips", cut_pso.Get()}, {"two streams", streams_pso.Get()},
      {"lines to points", lines_gs_pso.Get()}, {"cover", cover_pso.Get()}};
  for (auto &[name, pso] : made)
    expect(pso != nullptr, "the %s pipeline was not created", name);
  if (trace::wrong)
    return verdict();

  // the draws, in order, and what they write to: an output region per slot, which two draws share to append
  struct Draw {
    const char *name;
    ID3D12PipelineState *pso;
    D3D_PRIMITIVE_TOPOLOGY topology;
    Kind kind;
    bool indexed, cuts;
    UINT count, instances, start, base, start_instance;
    int regions[2];
    int query; // the statistics query's stream, or -1
  };
  // the indexed strips' indices: 0xFFFF in the middle, a vertex or a cut
  std::vector<uint16_t> indices;
  for (uint16_t i = 0; i < 21; i++)
    indices.push_back(i == 10 ? 0xffff : i);
  const UINT base_vertex = 2, indexed_start = 0;
  enum { PointsA, PointsB, StripA, StripB, ListA, ListB, TriStripA, TriStripB, IndexedA, IndexedB, CutA, CutB, FullA,
         FullB, GsCutA, GsCutB, Stream0, Stream1, GsLinesA, GsLinesB, Regions };
  const Draw draws[] = {
      {"points", points_pso.Get(), D3D_PRIMITIVE_TOPOLOGY_POINTLIST, Alone, false, false, 77, 2, 5, 0, 1, {PointsA, PointsB}, -1},
      {"line strip", lines_pso.Get(), D3D_PRIMITIVE_TOPOLOGY_LINESTRIP, Alone, false, false, 70, 3, 0, 0, 0, {StripA, StripB}, -1},
      {"line list, appended", lines_pso.Get(), D3D_PRIMITIVE_TOPOLOGY_LINELIST, Alone, false, false, 71, 2, 3, 0, 2, {StripA, StripB}, -1},
      {"triangle list", triangles_pso.Get(), D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, Alone, false, false, 100, 2, 0, 0, 0, {ListA, ListB}, -1},
      {"triangle strip", triangles_pso.Get(), D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, Alone, false, false, 75, 2, 1, 0, 0, {TriStripA, TriStripB}, -1},
      {"indexed strip, no cut value", triangles_pso.Get(), D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, Alone, true, false, 21, 2, indexed_start, base_vertex, 0, {IndexedA, IndexedB}, -1},
      {"indexed strip, cut", cutting_pso.Get(), D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, Alone, true, true, 21, 2, indexed_start, base_vertex, 0, {CutA, CutB}, -1},
      {"triangle list, slot 1 full", triangles_pso.Get(), D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, Alone, false, false, 100, 2, 0, 0, 0, {FullA, FullB}, 0},
      {"geometry shader, two strips", cut_pso.Get(), D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, Cut, false, false, 100, 2, 0, 0, 0, {GsCutA, GsCutB}, 0},
      {"geometry shader, two streams", streams_pso.Get(), D3D_PRIMITIVE_TOPOLOGY_POINTLIST, Streams, false, false, 50, 2, 0, 0, 0, {Stream0, Stream1}, 1},
      {"geometry shader, lines", lines_gs_pso.Get(), D3D_PRIMITIVE_TOPOLOGY_LINESTRIP, Lines, false, false, 70, 2, 0, 0, 0, {GsLinesA, GsLinesB}, -1},
  };

  // the CPU's draws: per draw and stream, each primitive's vertices
  std::vector<uint32_t> vb(65600), ib(8);
  for (size_t k = 0; k < vb.size(); k++)
    vb[k] = vertex_value(k);
  for (size_t k = 0; k < ib.size(); k++)
    ib[k] = instance_value(k);
  auto assemble = [&](const Draw &d, UINT instance) {
    // the input primitives' vertices, as each vertex shader output them
    std::vector<V4> vertices;
    std::vector<bool> cut;
    for (UINT p = 0; p < d.count; p++) {
      uint64_t index = d.indexed ? indices[d.start + p] : d.start + p;
      cut.push_back(d.cuts && index == 0xffff);
      uint64_t fetched = d.indexed ? index + d.base : index;
      uint32_t v = vb[fetched], inst = ib[d.start_instance + instance];
      vertices.push_back({v, inst, v ^ inst, 0x5a, v * 3 + 1});
    }
    std::vector<std::vector<V4>> primitives;
    bool strip = d.topology == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP || d.topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    UINT n = d.topology == D3D_PRIMITIVE_TOPOLOGY_POINTLIST                                                       ? 1
             : d.topology == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP || d.topology == D3D_PRIMITIVE_TOPOLOGY_LINELIST ? 2
                                                                                                                : 3;
    if (!strip) {
      for (UINT p = 0; p + n <= d.count; p += n)
        primitives.push_back(std::vector<V4>(vertices.begin() + p, vertices.begin() + p + n));
      return primitives;
    }
    for (UINT first = 0; first < d.count;) {
      UINT end = first;
      while (end < d.count && !cut[end])
        end++;
      for (UINT t = 0; first + t + n <= end; t++) {
        UINT odd = n == 3 ? t & 1 : 0, at = first + t;
        primitives.push_back(n == 3 ? std::vector<V4>{vertices[at], vertices[at + 1 + odd], vertices[at + 2 - odd]}
                                    : std::vector<V4>{vertices[at], vertices[at + 1]});
      }
      first = end + 1;
    }
    return primitives;
  };
  // per stream, the primitives in order
  auto run = [&](const Draw &d) {
    std::array<std::vector<std::vector<V4>>, 2> streams;
    for (UINT instance = 0; instance < d.instances; instance++)
      for (auto &in : assemble(d, instance)) {
        if (d.kind == Alone) {
          streams[0].push_back(in);
        } else if (d.kind == Cut) {
          for (uint32_t i = 0; i < 2; i++) {
            V4 e[7];
            for (uint32_t j = 0; j < 7; j++)
              e[j] = emit(in[j % 3][0], in[0][1], i, j);
            streams[0].push_back({e[0], e[1], e[2]});
            streams[0].push_back({e[1], e[3], e[2]});
            streams[0].push_back({e[4], e[5], e[6]});
          }
        } else if (d.kind == Streams) {
          for (uint32_t i = 0; i < 3; i++) {
            for (uint32_t j = 0; j < 3; j++)
              streams[0].push_back({emit(in[0][0], in[0][1], i, j)});
            for (uint32_t j = 10; j < 12; j++)
              streams[1].push_back({emit(in[0][0], in[0][1], i, j)});
          }
        } else {
          streams[0].push_back({emit(in[0][0], in[1][0], in[0][1], 20)});
          streams[0].push_back({emit(in[1][0], in[0][0], in[1][1], 21)});
        }
      }
    return streams;
  };

  // the regions: each its room, where it starts filled, and the words the draws leave in it
  const uint32_t sentinel = 0xa5a5a5a5u;
  struct Region {
    UINT64 at = 0, bytes = 0, start = 0;
    std::vector<uint32_t> words;
    UINT64 filled = 0;
  };
  std::array<Region, Regions> regions;
  struct Counted {
    UINT64 written = 0, needed = 0;
  };
  std::vector<Counted> counted(std::size(draws));
  {
    // the room each draw needs, and the cases that start part-filled or run out
    std::vector<std::array<std::vector<std::vector<V4>>, 2>> outputs;
    for (auto &d : draws)
      outputs.push_back(run(d));
    for (size_t i = 0; i < std::size(draws); i++) {
      auto &d = draws[i];
      auto &elements = d.kind == Streams ? two_streams : one_stream;
      auto strides = d.kind == Streams ? two_strides : one_strides;
      for (uint32_t s = 0; s < 2; s++)
        for (uint32_t slot = 0; slot < 2; slot++) {
          bool used = std::any_of(elements.begin(), elements.end(), [&](auto &e) { return e.stream == s && e.slot == slot; });
          if (!used)
            continue;
          UINT64 n = 0;
          for (auto &p : outputs[i][s])
            n += p.size();
          regions[d.regions[slot]].bytes += n * strides[slot];
        }
    }
    // points start after three vertices; the full case's slot 1 has room for 40 triangles and two words more; the
    // second point stream's for 450 points and a word
    regions[PointsA].start = 3 * one_strides[0];
    regions[PointsA].bytes += regions[PointsA].start;
    regions[PointsB].start = 2 * one_strides[1];
    regions[PointsB].bytes += regions[PointsB].start;
    regions[FullB].bytes = 40 * 3 * one_strides[1] + 8;
    regions[Stream1].bytes = 450 * two_strides[1] + 4;
    UINT64 at = 0;
    for (auto &r : regions) {
      r.at = at;
      at = (at + r.bytes + 15) & ~15ull;
      r.words.assign(r.bytes / 4, sentinel);
      r.filled = r.start;
    }
    // the CPU's stream output, draw by draw
    for (size_t i = 0; i < std::size(draws); i++) {
      auto &d = draws[i];
      auto &elements = d.kind == Streams ? two_streams : one_stream;
      auto strides = d.kind == Streams ? two_strides : one_strides;
      for (uint32_t s = 0; s < 2; s++) {
        auto &primitives = outputs[i][s];
        if (primitives.empty())
          continue;
        UINT64 per = primitives[0].size(), written = 0;
        std::vector<uint32_t> slots;
        for (uint32_t slot = 0; slot < 2; slot++)
          if (std::any_of(elements.begin(), elements.end(), [&](auto &e) { return e.stream == s && e.slot == slot; }))
            slots.push_back(slot);
        for (auto &p : primitives) {
          bool fits = std::all_of(slots.begin(), slots.end(), [&](uint32_t slot) {
            auto &r = regions[d.regions[slot]];
            return r.filled + per * strides[slot] <= r.bytes;
          });
          if (!fits)
            break;
          for (uint32_t slot : slots) {
            auto &r = regions[d.regions[slot]];
            for (auto &v : p) {
              UINT64 offset = r.filled;
              for (auto &e : elements)
                if (e.slot == slot) {
                  if (e.component >= 0)
                    r.words[offset / 4] = v[e.component];
                  offset += 4;
                }
              r.filled += strides[slot];
            }
          }
          written++;
        }
        if (d.query == int(s))
          counted[i] = {written, primitives.size()};
      }
    }
  }
  const UINT64 filled_at = (regions.back().at + regions.back().bytes + 7) & ~7ull, total = filled_at + 8 * Regions,
               cover_bytes = 4 * size * size;

  // the buffers: what the draws write and their filled sizes (the word after each a guard), the vertices and
  // instances, the indices, and the indirect arguments
  auto out = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, total, D3D12_RESOURCE_STATE_COPY_DEST);
  auto covered = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, cover_bytes, D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto cover_readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, cover_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
  const UINT64 vb_bytes = 4 * vb.size(), ib_at = vb_bytes, ib_bytes = 4 * ib.size(), indices_at = ib_at + ib_bytes,
               args_at = (indices_at + 2 * indices.size() + 15) & ~15ull, args_bytes = 32 * std::size(draws);
  auto zeros = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, cover_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto inputs_buffer = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, args_at + args_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  ComPtr<ID3D12QueryHeap> statistics;
  D3D12_QUERY_HEAP_DESC statistics_desc{D3D12_QUERY_HEAP_TYPE_SO_STATISTICS, (UINT)std::size(draws)};
  CHECK(device->CreateQueryHeap(&statistics_desc, IID_PPV_ARGS(&statistics)));
  auto statistics_out = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, std::size(draws) * sizeof(D3D12_QUERY_DATA_SO_STATISTICS),
                               D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(out && covered && cover_readback && upload && readback && inputs_buffer && statistics_out,
              "a buffer was not created"))
    return verdict();
  {
    uint8_t *init;
    CHECK(upload->Map(0, nullptr, (void **)&init));
    std::fill_n(reinterpret_cast<uint32_t *>(init), filled_at / 4, sentinel);
    for (UINT r = 0; r < Regions; r++) {
      uint32_t filled[2] = {uint32_t(regions[r].start), sentinel};
      memcpy(init + filled_at + 8 * r, filled, sizeof(filled));
    }
    upload->Unmap(0, nullptr);
    void *zero;
    CHECK(zeros->Map(0, nullptr, &zero));
    memset(zero, 0, cover_bytes);
    zeros->Unmap(0, nullptr);
    uint8_t *in;
    CHECK(inputs_buffer->Map(0, nullptr, (void **)&in));
    memcpy(in, vb.data(), vb_bytes);
    memcpy(in + ib_at, ib.data(), ib_bytes);
    memcpy(in + indices_at, indices.data(), 2 * indices.size());
    for (size_t i = 0; i < std::size(draws); i++) {
      auto &d = draws[i];
      if (d.indexed) {
        D3D12_DRAW_INDEXED_ARGUMENTS a{d.count, d.instances, d.start, INT(d.base), d.start_instance};
        memcpy(in + args_at + 32 * i, &a, sizeof(a));
      } else {
        D3D12_DRAW_ARGUMENTS a{d.count, d.instances, d.start, d.start_instance};
        memcpy(in + args_at + 32 * i, &a, sizeof(a));
      }
    }
    inputs_buffer->Unmap(0, nullptr);
  }
  ComPtr<ID3D12CommandSignature> draw_signature, indexed_signature;
  {
    D3D12_INDIRECT_ARGUMENT_DESC operation{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW};
    D3D12_COMMAND_SIGNATURE_DESC sig{32, 1, &operation};
    CHECK(device->CreateCommandSignature(&sig, nullptr, IID_PPV_ARGS(&draw_signature)));
    operation.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    CHECK(device->CreateCommandSignature(&sig, nullptr, IID_PPV_ARGS(&indexed_signature)));
  }

  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  for (bool indirect : {false, true}) {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
    list->CopyBufferRegion(out.Get(), 0, upload.Get(), 0, total);
    transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
    // the pixel counts start at zero: the upload's words past the filled sizes
    list->CopyBufferRegion(covered.Get(), 0, zeros.Get(), 0, cover_bytes);
    transition(list.Get(), covered.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto base = out->GetGPUVirtualAddress(), inputs_base = inputs_buffer->GetGPUVirtualAddress();
    D3D12_VERTEX_BUFFER_VIEW vbvs[] = {{inputs_base, UINT(vb_bytes), 4}, {inputs_base + ib_at, UINT(ib_bytes), 4}};
    D3D12_INDEX_BUFFER_VIEW ibv{inputs_base + indices_at, UINT(2 * indices.size()), DXGI_FORMAT_R16_UINT};
    list->SetGraphicsRootSignature(rs.Get());
    list->SetGraphicsRootUnorderedAccessView(0, covered->GetGPUVirtualAddress());
    list->IASetVertexBuffers(0, 2, vbvs);
    list->IASetIndexBuffer(&ibv);
    const D3D12_VIEWPORT viewport{0, 0, (float)size, (float)size, 0, 1};
    const D3D12_RECT scissor{0, 0, (LONG)size, (LONG)size};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    for (size_t i = 0; i < std::size(draws); i++) {
      auto &d = draws[i];
      D3D12_STREAM_OUTPUT_BUFFER_VIEW views[2];
      for (int slot = 0; slot < 2; slot++) {
        auto &r = regions[d.regions[slot]];
        views[slot] = {base + r.at, r.bytes, base + filled_at + 8 * d.regions[slot]};
      }
      list->SOSetTargets(0, 2, views);
      list->SetPipelineState(d.pso);
      list->IASetPrimitiveTopology(d.topology);
      if (d.query >= 0)
        list->BeginQuery(statistics.Get(), D3D12_QUERY_TYPE(D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 + d.query), UINT(i));
      if (indirect)
        list->ExecuteIndirect(d.indexed ? indexed_signature.Get() : draw_signature.Get(), 1, inputs_buffer.Get(), args_at + 32 * i, nullptr, 0);
      else if (d.indexed)
        list->DrawIndexedInstanced(d.count, d.instances, d.start, INT(d.base), d.start_instance);
      else
        list->DrawInstanced(d.count, d.instances, d.start, d.start_instance);
      if (d.query >= 0)
        list->EndQuery(statistics.Get(), D3D12_QUERY_TYPE(D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 + d.query), UINT(i));
      // the first draw, which rasterizes nothing, is followed by one that rasterizes without attachments
      if (!i) {
        list->SetPipelineState(cover_pso.Get());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
      }
    }
    list->SOSetTargets(0, 0, nullptr);
    for (size_t i = 0; i < std::size(draws); i++)
      if (draws[i].query >= 0)
        list->ResolveQueryData(statistics.Get(), D3D12_QUERY_TYPE(D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 + draws[i].query),
                               UINT(i), 1, statistics_out.Get(), i * sizeof(D3D12_QUERY_DATA_SO_STATISTICS));
    transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), 0, out.Get(), 0, total);
    transition(list.Get(), covered.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(cover_readback.Get(), 0, covered.Get(), 0, cover_bytes);
    CHECK(list->Close());
    for (UINT replay = 0; replay < 2; replay++) {
      step("%s draws, run %u", indirect ? "indirect" : "direct", replay);
      CHECK(forget(readback.Get()));
      CHECK(forget(cover_readback.Get()));
      CHECK(forget(statistics_out.Get()));
      CHECK(execute(device.Get(), queue.Get(), list.Get()));
      const uint8_t *got;
      CHECK(readback->Map(0, nullptr, (void **)&got));
      auto word = [&](UINT64 at) { return *reinterpret_cast<const uint32_t *>(got + at); };
      for (size_t i = 0; i < std::size(draws); i++) {
        auto &d = draws[i];
        step("%s draws, run %u, %s", indirect ? "indirect" : "direct", replay, d.name);
        for (int slot = 0; slot < 2; slot++) {
          int r = d.regions[slot];
          // a region shared with an earlier draw was checked with it
          if (std::any_of(draws, draws + i, [&](auto &e) { return e.regions[0] == r || e.regions[1] == r; }))
            continue;
          unsigned wrong = 0;
          for (size_t w = 0; w < regions[r].words.size(); w++) {
            uint32_t value = word(regions[r].at + 4 * w), want = regions[r].words[w];
            if (value != want && wrong++ < 6)
              expect(false, "slot %d byte %zu is %#x, want %#x", slot, 4 * w, value, want);
          }
          if (wrong > 6)
            expect(false, "slot %d: %u more wrong words", slot, wrong - 6);
          expect(word(filled_at + 8 * r) == regions[r].filled, "slot %d filled size %u, want %llu", slot,
                 word(filled_at + 8 * r), (unsigned long long)regions[r].filled);
          expect(word(filled_at + 8 * r + 4) == sentinel, "slot %d: the word after the filled size is %#x", slot,
                 word(filled_at + 8 * r + 4));
        }
      }
      readback->Unmap(0, nullptr);
      step("%s draws, run %u, pixels of the draw after stream output", indirect ? "indirect" : "direct", replay);
      const uint32_t *runs;
      CHECK(cover_readback->Map(0, nullptr, (void **)&runs));
      unsigned missed = 0;
      for (UINT p = 0; p < size * size; p++)
        if (runs[p] != 1 && missed++ < 6)
          expect(false, "pixel (%u, %u) ran %u times, want 1", p % size, p / size, runs[p]);
      if (missed > 6)
        expect(false, "%u more pixels wrong", missed - 6);
      cover_readback->Unmap(0, nullptr);
      const D3D12_QUERY_DATA_SO_STATISTICS *got_counts;
      CHECK(statistics_out->Map(0, nullptr, (void **)&got_counts));
      for (size_t i = 0; i < std::size(draws); i++)
        if (draws[i].query >= 0) {
          step("%s draws, run %u, %s, statistics", indirect ? "indirect" : "direct", replay, draws[i].name);
          expect(got_counts[i].NumPrimitivesWritten == counted[i].written, "%llu primitives written, want %llu",
                 (unsigned long long)got_counts[i].NumPrimitivesWritten, (unsigned long long)counted[i].written);
          expect(got_counts[i].PrimitivesStorageNeeded == counted[i].needed, "%llu primitives needed, want %llu",
                 (unsigned long long)got_counts[i].PrimitivesStorageNeeded, (unsigned long long)counted[i].needed);
        }
      statistics_out->Unmap(0, nullptr);
    }
  }
  return verdict();
}
