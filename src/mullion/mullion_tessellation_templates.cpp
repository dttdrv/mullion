// the device's tessellation templates (air_tessellation.metal's tess_template), made once from the tessellator's own
// source, built for the host as tests/host/tessellator.cpp builds it
#include "mullion_tessellation_templates.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <vector>
// first: it names Metal's address spaces away
#include "metal_stdlib"

namespace mullion::tessellation {
// of this file alone, as other files build it too
namespace {
#include "air_tessellation.metal"
}

namespace {

// a patch of `n` points a side, its factors all whole: the template of every patch whose factors have those points
template <typename Patch>
bool
uniform_patch(Patch &patch, int n) {
  float f = float(n - 1);
  bool made;
  if constexpr (Patch::sides == 4)
    made = make_patch(patch, partitioning::integer, f, f, f, f, f, f);
  else
    made = make_patch(patch, partitioning::integer, f, f, f, f);
  return made && template_points(patch) == n;
}

// the place of point i of an edge that runs along ring k's side s (or against side -1 - s), as the template keeps it
uint
point_code(const quad_patch &patch, int k, int s, const tess_edge &edge, int i) {
  if (s < 0) {
    s = -1 - s;
    i = edge.segments - i;
  }
  int n = patch.n[0], a, b;
  if (!k) {
    a = s == 0 ? 0 : s == 1 ? i : s == 2 ? n - 1 : n - 1 - i;
    b = s == 0 ? n - 1 - i : s == 1 ? 0 : s == 2 ? i : n - 1;
  } else {
    a = s == 0 ? k : s == 1 ? k + i : s == 2 ? n - 1 - k : n - 1 - k - i;
    b = s == 0 ? n - 1 - k - i : s == 1 ? k : s == 2 ? k + i : n - 1 - k;
  }
  return uint(a) | uint(b) << 8;
}

uint
point_code(const tri_patch &patch, int k, int s, const tess_edge &edge, int i) {
  if (s < 0) {
    s = -1 - s;
    i = edge.segments - i;
  }
  // a side's last point is the next side's first, which the triangle's formulas place
  int m = k ? patch.n - 1 - 2 * k : patch.ring0[s];
  if (m <= 0)
    return uint(k);
  if (i == m) {
    s = (s + 1) % 3;
    i = 0;
  }
  return uint(k) | uint(s) << 8 | uint(i) << 16;
}

uint
morton(uint x, uint y) {
  uint key = 0;
  for (int bit = 0; bit < 16; bit++)
    key |= (x >> bit & 1) << (2 * bit) | (y >> bit & 1) << (2 * bit + 1);
  return key;
}

struct built {
  std::vector<uint32_t> meshlets, vertices, triangles; // meshlets' offsets relative to the vertices and triangles
};

// the patch's triangles, as its points' codes, in meshlets: false where a decoded point is not the tessellator's own
template <typename Patch>
bool
build(const Patch &patch, built &out, int &meshlet_count) {
  std::vector<uint> codes;             // by the patch's number of each point
  std::vector<std::array<int, 3>> tris; // as numbers
  auto number_of = [&](int k, int s, const tess_edge &edge, int i) {
    int number = edge_number(patch, k, s, edge, i);
    uint code = point_code(patch, k, s, edge, i);
    float2 at = point(edge, i), decoded = template_point(patch, code);
    if (memcmp(&at, &decoded, sizeof(at)))
      return -1;
    if (codes.size() <= size_t(number))
      codes.resize(number + 1, ~0u);
    if (codes[number] != ~0u && codes[number] != code)
      return -1;
    codes[number] = code;
    return number;
  };
  for (int f = 0; f < frame_count(patch); f++) {
    tess_along along;
    tess_frame frame = nth_frame(patch, f, along);
    tess_zip zip = {frame.inner, frame.outer, 0, 0, 0};
    short inner = frame.inner.segments + 1;
    for (short3 t(0, 0, 0); zip.next(t, 0, inner);) {
      std::array<int, 3> numbers;
      for (int c = 0; c < 3; c++) {
        numbers[c] = t[c] < inner ? number_of(along.inner_k, along.inner_s, frame.inner, t[c])
                                  : number_of(along.outer_k, along.outer_s, frame.outer, t[c] - inner);
        if (numbers[c] < 0)
          return false;
      }
      tris.push_back(numbers);
    }
  }
  // near triangles first, by where their corners are, so that a meshlet's share their points
  std::vector<std::pair<uint, size_t>> order;
  for (size_t t = 0; t < tris.size(); t++) {
    uint x = 0, y = 0;
    for (int number : tris[t]) {
      float2 at = template_point(patch, codes[number]);
      x += uint(at.x * 1024);
      y += uint(at.y * 1024);
    }
    order.push_back({morton(x / 3, y / 3), t});
  }
  std::stable_sort(order.begin(), order.end(), [](auto &a, auto &b) { return a.first < b.first; });
  std::vector<int> local(codes.size(), -1);
  std::vector<int> used;
  uint32_t first_vertex = out.vertices.size(), first_triangle = out.triangles.size();
  meshlet_count = 0;
  auto close = [&] {
    if (out.triangles.size() == first_triangle)
      return;
    uint32_t vertices = out.vertices.size() - first_vertex, triangles = out.triangles.size() - first_triangle;
    out.meshlets.insert(out.meshlets.end(), {first_vertex, vertices | triangles << 16, first_triangle, 0});
    meshlet_count++;
    for (int number : used)
      local[number] = -1;
    used.clear();
    first_vertex = out.vertices.size();
    first_triangle = out.triangles.size();
  };
  for (auto [key, t] : order) {
    int added = 0;
    for (int number : tris[t])
      added += local[number] < 0;
    if (out.triangles.size() - first_triangle + 1 > uint32_t(template_primitives) ||
        out.vertices.size() - first_vertex + added > uint32_t(template_vertices))
      close();
    uint word = 0;
    for (int c = 0; c < 3; c++) {
      int number = tris[t][c];
      if (local[number] < 0) {
        local[number] = out.vertices.size() - first_vertex;
        used.push_back(number);
        out.vertices.push_back(codes[number]);
      }
      word |= uint(local[number]) << (8 * c);
    }
    out.triangles.push_back(word);
  }
  close();
  return true;
}

std::vector<uint32_t>
make() {
  built parts;
  std::vector<uint32_t> header(template_meshlets, 0);
  header[0] = template_version;
  header[1] = template_count;
  for (int quad = 0; quad < 2; quad++)
    for (int n = 2; n <= template_most_points; n++) {
      int first = parts.meshlets.size() / 4, count = 0;
      bool made;
      if (quad) {
        quad_patch patch;
        made = uniform_patch(patch, n) && build(patch, parts, count);
      } else {
        tri_patch patch;
        made = uniform_patch(patch, n) && build(patch, parts, count);
      }
      if (!made) {
        // its meshlets so far are left unnamed
        fprintf(stderr, "mullion: no tessellation template of %d points a side (%s)\n", n, quad ? "quad" : "triangle");
        continue;
      }
      int at = 2 + 2 * ((quad ? template_most_points + 1 : 0) + n);
      header[at] = first;
      header[at + 1] = count;
    }
  // the meshlets' offsets, from the buffer's start
  uint32_t vertices = template_meshlets + parts.meshlets.size(), triangles = vertices + parts.vertices.size();
  for (size_t m = 0; m < parts.meshlets.size(); m += 4) {
    parts.meshlets[m] += vertices;
    parts.meshlets[m + 2] += triangles;
  }
  std::vector<uint32_t> words = std::move(header);
  words.insert(words.end(), parts.meshlets.begin(), parts.meshlets.end());
  words.insert(words.end(), parts.vertices.begin(), parts.vertices.end());
  words.insert(words.end(), parts.triangles.begin(), parts.triangles.end());
  return words;
}

} // namespace

} // namespace mullion::tessellation

namespace mullion {

const std::vector<uint32_t> &
TessellationTemplates() {
  static std::once_flag once;
  static std::vector<uint32_t> words;
  std::call_once(once, [] { words = tessellation::make(); });
  return words;
}

} // namespace mullion
