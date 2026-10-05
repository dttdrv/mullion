// shared by d3d11_assembly and d3d12_assembly: the shader, the topologies and the CPU's assembly of their primitives.
// contract: without a geometry shader, a primitive is its own vertices, whatever the topology brings beside them.
// - topologies with adjacency draw and stream out their primitives without the adjacent vertices, which only a
//   geometry shader sees (D3D11.3 8.15); a primitive's leading vertex, whose values an uninterpolated attribute
//   takes, is its first own vertex (8.14), and a triangle strip's odd triangles go out as (n, n + 2, n + 1) (14.5);
// - a primitive is discarded when all its vertices are behind one cull distance, negative or NaN, and drawn whole
//   when one is not (15.4.2, 15.4.3). cull distances do not reach stream output, which comes first (15.4.3).
// every topology draws six primitives, each over one pixel of a row of its own, with the leading vertex's number;
// adjacent vertices lie on the row below, where a primitive made with one would show. the vertex shader runs with
// and without cull distances, and each time alone and with its own stream output, which also rasterizes. last in
// each pass, a topology turns into its form with adjacency while everything else stays bound.
#pragma once
#include <array>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

namespace assembly {

static const char hlsl[] = R"hlsl(
// the case: its row; the own vertices of a primitive (1 to 3), found among a list's `stride` vertices, or along a
// strip's, as every `every`-th from `first`; a strip's last own vertex
cbuffer Case : register(b0) { uint row, corners, strip, stride, first, every, last, nan_bits; };
struct O {
  float4 pos : SV_Position;
  nointerpolation uint value : VALUE;
#if CULL
  float2 cull : SV_CullDistance;
#endif
};
float4 at(float2 pixels) { return float4(pixels / float2(WIDTH, HEIGHT) * float2(2, -2) + float2(-1, 1), 0, 1); }
O vs(uint id : SV_VertexID) {
  // n: the vertex's place in its strip, or its primitive in a list, where it is corner c
  uint k = strip ? id : id % stride, c = (k - first) / every, n = strip ? c : id / stride;
  bool own = k >= first && (k - first) % every == 0 && (strip ? id <= last : c < corners);
  float2 pixels;
  if (!own)
    pixels = float2(id + 0.5, row + 1.5);
  else if (corners == 1)
    pixels = float2(n + 0.5, row + 0.5);
  // a line from a quarter into a pixel to a quarter into the next covers the first
  else if (corners == 2)
    pixels = float2(n + (strip ? 0 : c) + 0.25, row + 0.5);
  // a strip's triangle n has its tip over the center of pixel n + 1
  else if (strip)
    pixels = float2(n + 0.5, row + (n % 2 ? 0.9 : 0.1));
  else
    pixels = float2(n + (c == 1 ? 1.4 : 0), row + (c == 2 ? 1.4 : 0));
  O o;
  o.pos = at(pixels);
  o.value = id + 1;
#if CULL
  // behind the first distance: a strip's vertices of STRIP_OUT, a list's corners of LIST_OUT's three bits for their
  // primitive, as -1 or NaN; the others at 0 or 1. and primitive SECOND of a list behind the second distance
  bool out_ = own && (strip ? STRIP_OUT >> n & 1 : LIST_OUT >> (3 * n + c) & 1);
  o.cull.x = out_ ? (n % 2 ? asfloat(nan_bits) : -1.0) : (float)(c % 2);
  o.cull.y = own && !strip && n == SECOND ? -1 : 1;
#endif
  return o;
}
uint ps(O o) : SV_Target { return o.value; }
)hlsl";

// a strip's vertices 1 to 4 are behind the first distance; a list's primitives 1 and 3 whole, and corner 1 of
// primitive 2 and corners 0 and 2 of primitive 4; primitive 5 is behind the second distance
const uint32_t primitives = 6, strip_out = 0x1e, list_out = 0b000'101'111'010'111'000, second = 5;

struct Case {
  D3D_PRIMITIVE_TOPOLOGY topology;
  uint32_t corners, strip, stride, first, every, vertices;
};
// the vertices that make six primitives (D3D11.3 8.14)
const Case cases[] = {
    {D3D_PRIMITIVE_TOPOLOGY_POINTLIST, 1, 0, 1, 0, 1, primitives},
    {D3D_PRIMITIVE_TOPOLOGY_LINELIST, 2, 0, 2, 0, 1, 2 * primitives},
    {D3D_PRIMITIVE_TOPOLOGY_LINESTRIP, 2, 1, 1, 0, 1, primitives + 1},
    {D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 3, 0, 3, 0, 1, 3 * primitives},
    {D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, 3, 1, 1, 0, 1, primitives + 2},
    {D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ, 2, 0, 4, 1, 1, 4 * primitives},
    {D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ, 2, 1, 1, 1, 1, primitives + 3},
    {D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ, 3, 0, 6, 0, 2, 6 * primitives},
    {D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ, 3, 1, 1, 0, 2, 2 * (primitives + 2)},
};
// a case takes its row and the one below; a pass draws every case and the turn, and there is one for each of the
// vertex shader's cull distances (without, with) and stream output (without, with)
const uint32_t case_count = std::size(cases), rows = 2, pass_rows = rows * (case_count + 1), passes = 4;
// the turn: with a line strip's constants, a strip of one line, then a strip with adjacency, whose lines the same
// constants put from the second pixel on
const Case &turned = cases[2];
const uint32_t turn_vertices = 2, turned_vertices = primitives + 2, turn_row = rows * case_count;
const uint32_t width = 6 * primitives + 4, height = pass_rows * passes;

inline std::vector<std::string>
defines(bool cull) {
  return {
      "WIDTH=" + std::to_string(width), "HEIGHT=" + std::to_string(height), "STRIP_OUT=" + std::to_string(strip_out),
      "LIST_OUT=" + std::to_string(list_out), "SECOND=" + std::to_string(second), "CULL=" + std::to_string(cull),
  };
}

// the shader's constants for a case drawn at `row`
struct Constants {
  uint32_t row, corners, strip, stride, first, every, last, nan_bits;
};
inline Constants
constants(const Case &c, uint32_t row) {
  // a strip's last own vertex: every vertex but those adjacent to its end
  return {row, c.corners, c.strip, c.stride, c.first, c.every, c.first + (primitives + c.corners - 2) * c.every, 0x7fc00000};
}

// the CPU's primitives: the own vertices of each, in the order they go out, and whether it is drawn
struct Primitive {
  std::array<uint32_t, 3> vertices;
  bool culled;
};
inline std::vector<Primitive>
assemble(const Case &c, bool cull) {
  std::vector<Primitive> out;
  for (uint32_t n = 0; n < primitives; n++) {
    Primitive p{};
    bool behind = true;
    for (uint32_t corner = 0; corner < c.corners; corner++) {
      // a strip's vertex n + corner, odd triangles with the last two the other way round
      uint32_t along = n + (c.corners == 3 && n % 2 && corner ? 3 - corner : corner);
      p.vertices[corner] = c.strip ? c.first + along * c.every : n * c.stride + c.first + corner * c.every;
      behind &= c.strip ? strip_out >> along & 1 : list_out >> (3 * n + corner) & 1;
    }
    p.culled = cull && (behind || (!c.strip && n == second));
    out.push_back(p);
  }
  return out;
}

// what pixel (x, y) of a case's two rows shows: the leading vertex's number plus one, or 0
inline uint32_t
pixel(const Case &c, const std::vector<Primitive> &assembled, uint32_t x, uint32_t y) {
  // primitive x of the row, a strip's triangles one pixel on
  uint32_t n = x - (c.strip && c.corners == 3);
  return !y && n < primitives && !assembled[n].culled ? assembled[n].vertices[0] + 1 : 0;
}

// what pixel (x, y) of the turn's two rows shows: the strip's one line, then the lines of the strip with adjacency,
// line x - 1 from vertex x to x + 1
inline uint32_t
turned_pixel(bool cull, uint32_t x, uint32_t y) {
  bool behind = cull && (strip_out >> x & 1) && (strip_out >> (x + 1) & 1);
  return !y && x + 3 <= turned_vertices && !behind ? x + 1 : 0;
}

// what stream output gets of a pass: every primitive's own vertices' numbers plus one, and the turn's lines
inline void
stream(std::vector<uint32_t> &out) {
  for (auto &c : cases)
    for (auto &p : assemble(c, false))
      for (uint32_t corner = 0; corner < c.corners; corner++)
        out.push_back(p.vertices[corner] + 1);
  for (uint32_t vertex : {0u, 1u})
    out.push_back(vertex + 1);
  for (uint32_t line = 0; line + 3 < turned_vertices; line++)
    for (uint32_t vertex : {line + 1, line + 2})
      out.push_back(vertex + 1);
}

} // namespace assembly
