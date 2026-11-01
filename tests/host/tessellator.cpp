// contract: the fixed function tessellator is Direct3D's reference tessellator in what it connects as in where it
// puts its points (D3D11.3 11.7.1, 11.7.6): a patch's triangles, each its three domain locations in clockwise order,
// are the reference's, in whatever order they come (11.7.9), and so are an isoline patch's line segments and every
// patch's points, each once. this holds whatever largest factor the hull shader declares, which caps the factors
// (11.7.10 MaxTessFactor) and sizes the stages (src/airconv/tessellation_limits.hpp): the payload has room for that
// many workloads of a patch, which is all the hull stage is given, and a workload has no more vertices than the mesh.
// this runs the tessellator's own source, src/airconv/shaders/air_tessellation.metal, built for the host
// (metal/metal_stdlib stands in for Metal's library): the hull stage's workloads, what the mesh stage makes of each,
// and what a geometry shader's threadgroup looks up for each primitive (mullion.get_domain_primitive_location). a mesh
// threadgroup may instead take a group of a patch's workloads (tess_output): the 32 threads of its SIMD group make
// together the primitives each workload's own threadgroup makes, in the same order and culled alike, from the group's
// vertices, which are each workload's but for the points of an edge it shares with the one before; a group has no
// more vertices and primitives than its limits, and the groups' primitives are the reference's. the d3d11 and d3d12
// tessellator tests check the same source as the GPU runs it, by its points and what it covers.
// listing work is the zip's attempted slots, including ones that make no triangle. D3D11.3 11.7.5 defines an epoch
// as "a power of 2 number of vertices on one half of the domain (excluding corners)"; 11.7.6.2 omits "one segment
// on each end of the inside". for H points on the larger half-edge, the smallest containing epoch E has E < 2H:
// trying both edges at both ends and two middle slots costs at most 4E + 2 <= 8H - 2. the frame has P >= 2H + 1
// edge points (the inner edge omits two end segments, the outer has at least one), so fewer than 4P slots suffice.
// each triangle advances one edge segment: a complete frame has the sum of its edges' segments in triangles.
#include "../tessellator.hpp"
#include "tessellation_limits.hpp"
#include <set>
// last: it names Metal's address spaces away
#include "metal/metal_stdlib"
#include "air_tessellation.metal"

using namespace tessellator;

// a primitive's corners as domain location bits; a triangle is the least of its turns, since it is the same
// triangle whichever corner it starts from
using Corner = std::array<uint32_t, 2>;
using Primitive = std::vector<Corner>;

static Corner
corner(float u, float v) {
  Corner c;
  memcpy(&c[0], &u, 4);
  memcpy(&c[1], &v, 4);
  return c;
}

static Primitive
primitive(Primitive p) {
  return p.size() == 3 ? std::min({p, Primitive{p[1], p[2], p[0]}, Primitive{p[2], p[0], p[1]}}) : p;
}

// the reference's primitives of `vertices` corners each
static std::multiset<Primitive>
expected(Domain domain, Partitioning partitioning, const Factors &patch, int vertices) {
  float factors[6];
  for (uint32_t i = 0; i < edges[domain]; i++)
    factors[i] = patch[i];
  for (uint32_t i = 0; i < insides[domain]; i++)
    factors[edges[domain] + i] = patch[4 + i];
  std::multiset<Primitive> out;
  if (vertices == 1) {
    for (auto &p : reference::points(domain, partitioning, factors))
      out.insert({corner(p.u, p.v)});
    return out;
  }
  auto corners = reference::corners(domain, partitioning, factors, vertices);
  for (size_t i = 0; i + vertices <= corners.size(); i += vertices) {
    Primitive p;
    for (int c = 0; c < vertices; c++)
      p.push_back(corner(corners[i + c].u, corners[i + c].v));
    out.insert(primitive(p));
  }
  return out;
}

int
main() {
  using namespace mullion::dxbc;
  unsigned failures = 0, patches_checked = 0;
  size_t primitives = 0, frames_checked = 0, zip_slots = 0, lanes_checked = 0;
  // the largest factors a hull shader may declare: the tessellator's, and lower ones whole and not, odd and even
  for (float declared : {64.0f, 17.5f, 8.0f, 3.0f, 1.0f})
  for (Domain domain : {reference::isoline, reference::triangle, reference::quad})
    for (Partitioning partitioning :
         {reference::integer, reference::pow2, reference::fractional_odd, reference::fractional_even}) {
      // the tokenized program's names for them follow its undefined ones
      auto sb_domain = microsoft::D3D11_SB_TESSELLATOR_DOMAIN(domain + 1);
      auto sb_partitioning = microsoft::D3D11_SB_TESSELLATOR_PARTITIONING(partitioning + 1);
      uint32_t most_segments = get_integer_factor(declared, sb_partitioning);
      // the room a patch's workloads have, and no more
      std::vector<tess_workload> workloads(get_max_potential_workload_count(most_segments, sb_domain, sb_partitioning));
      auto data = (int *)workloads.data();
      auto fail = [&](const char *what, const Factors &f, size_t got, size_t want) {
        if (failures++ < 8)
          printf(
              "%s, %s, at most %g, factors %g %g %g %g, inside %g %g: %s: %zu, want %zu\n", domain_names[domain],
              partitioning_names[partitioning], declared, f[0], f[1], f[2], f[3], f[4], f[5], what, got, want
          );
      };
      // the patches the GPU tests draw alone, then draws of mixed ones
      std::vector<Factors> all(std::begin(alone), std::end(alone));
      all.insert(all.end(), std::begin(lines_alone), std::end(lines_alone));
      for (uint32_t seed = 1; seed <= 48; seed++)
        for (auto &patch : draw_patches(domain, seed))
          all.push_back(patch);
      if (domain != reference::isoline)
        for (uint32_t factor = 1; factor <= most_segments; factor++) {
          Factors uniform{};
          uniform.fill(factor);
          all.push_back(uniform);
          // both sides of every epoch boundary, with unequal edges and inside axes and both orders of parity
          for (float inside : {1.0f, float(factor) + 0.5f, float(most_segments)}) {
            Factors mixed = {float(factor), 1, float(most_segments), float(factor) + 0.5f, inside, float(factor)};
            all.push_back(mixed);
          }
        }
      for (auto f : all) {
        // the hull stage's cap, which a NaN passes
        for (auto &factor : f)
          factor = factor > declared ? declared : factor;
        // the patch's workloads, and with groups of at most `limit` vertices those groups (none: 0)
        std::vector<int> groups(workloads.size());
        auto generate = [&](int *next, int capacity, int limit) {
          if (domain == reference::quad)
            generate_workload_quad(0, next, data, capacity, groups.data(), limit, partitioning, f[4], f[5], f[0], f[1], f[2], f[3]);
          else if (domain == reference::triangle)
            generate_workload_triangle(0, next, data, capacity, groups.data(), limit, partitioning, f[4], f[0], f[1], f[2]);
          else
            generate_workload_isoline(0, next, data, capacity, groups.data(), limit, partitioning, f[0], f[1]);
        };
        int count = 0;
        generate(&count, workloads.size(), 0);
        // one more than there is room for was not written
        if ((size_t)count > workloads.size())
          fail("workloads", f, count, workloads.size());
        count = std::min<size_t>(count, workloads.size());
        if (domain != reference::isoline)
          for (int w = 0; w < count; w++)
            for (int frame = 0; frame < 4 && workloads[w].edge[frame].segments >= 0; frame += 2) {
              tess_zip zip = {workloads[w].edge[frame], workloads[w].edge[frame + 1], 0, 0, 0};
              size_t triangles = 0, segments = zip.inner.segments + zip.outer.segments;
              short3 indices(0, 0, 0);
              while (zip.next(indices, 0, zip.inner.segments + 1))
                triangles++;
              if (triangles != segments)
                fail("triangles in a frame", f, triangles, segments);
              size_t bound = 4 * (segments + 2);
              if ((size_t)zip.slot >= bound)
                fail("zip slots in a frame", f, zip.slot, bound - 1);
              frames_checked++;
              zip_slots += zip.slot;
            }
        // by corners: the points, an isoline patch's lines, the other patches' triangles
        for (int vertices : {1, domain == reference::isoline ? 2 : 3}) {
          std::multiset<Primitive> made, looked;
          for (int w = 0; w < count; w++) {
            auto at = [&](int vertex) {
              auto location = workload_vertex(workloads[w], vertex);
              return corner(location.uv.x, location.uv.y);
            };
            int workload_vertices = 0;
            for (auto &edge : workloads[w].edge)
              workload_vertices += edge.segments + 1;
            if ((uint32_t)workload_vertices > get_max_workload_vertices(most_segments, sb_domain))
              fail("vertices in a workload", f, workload_vertices, get_max_workload_vertices(most_segments, sb_domain));
            metal::recorder recorded;
            Mesh mesh{&recorded};
            if (vertices == 3)
              generatePrimitiveTriangle(w, data, mesh, nullptr);
            else if (vertices == 2)
              generatePrimitiveLine(w, data, mesh, nullptr);
            else
              generatePrimitivePoint(w, data, mesh, nullptr);
            for (uint32_t n = 0; n < recorded.primitives; n++) {
              Primitive from_mesh, from_lookup;
              for (int c = 0; c < vertices; c++) {
                from_mesh.push_back(at(recorded.indices[vertices * n + c]));
                auto location = get_domain_primitive_location(w, n, c, vertices, 0, data);
                from_lookup.push_back(corner(location.uv.x, location.uv.y));
                if (from_lookup.back() != from_mesh.back())
                  fail("primitive numbering", f, primitive_vertex(workloads[w], n, c, vertices),
                       recorded.indices[vertices * n + c]);
              }
              made.insert(primitive(from_mesh));
              looked.insert(primitive(from_lookup));
            }
            // the lookup ends where the mesh's primitives do
            if (get_domain_primitive_location(w, recorded.primitives, 0, vertices, 0, data).active)
              fail("primitives looked up past a workload's", f, recorded.primitives + 1, recorded.primitives);
          }
          // a mesh threadgroup takes a group of workloads, of at most the fewest vertices that fit any workload or a
          // mesh's most: its primitives are those each workload's own threadgroup makes, one workload's after
          // another, of the same domain locations in the same order, and its threads together cull the same ones
          // as one thread makes each workload's. a vertex is behind cull distances of bits its location gives
          auto behind_bits = [](Corner c) { return uint((c[0] * 2654435761u) ^ (c[1] * 40503u)) >> 29; };
          // a mesh has at most 256 vertices. the pipelines' groups have as many vertices as a workload has at most
          // and twice as many primitives (tessellation_group_size), or fewer primitives where the mesh's size allows
          // no more, as few as its vertices
          constexpr int mesh_vertices = 256;
          int most = get_max_workload_vertices(most_segments, sb_domain);
          for (auto [limit, primitive_limit] :
               {std::pair{most, 2 * most}, std::pair{most, most}, std::pair{mesh_vertices, (int)mesh_primitive_limit}}) {
            int next[5] = {};
            std::fill(groups.begin(), groups.end(), -1);
            generate(next, workloads.size(), pack_group_size(limit, primitive_limit));
            if (next[0] != count)
              fail("workloads written with groups", f, next[0], count);
            std::multiset<Primitive> grouped;
            int expect_first = 0;
            for (int g = 0; g < next[1]; g++) {
              int first = groups[g] & 0xffff, end = groups[g] >> 16;
              // the groups follow each other, and together have every workload
              if (groups[g] < 0 || first != expect_first || end <= first || (g + 1 == next[1] && end != count)) {
                fail("a group's workloads", f, first, end);
                break;
              }
              expect_first = end;
              // the group's vertices, each a location once
              std::vector<Corner> at;
              while (true) {
                auto location = get_group_location(first, end, at.size(), data);
                if (!location.active)
                  break;
                at.push_back(corner(location.uv.x, location.uv.y));
              }
              if (at.size() > (size_t)limit)
                fail("vertices in a group", f, at.size(), limit);
              uint behind[mesh_vertices];
              for (size_t v = 0; v < at.size() && v < mesh_vertices; v++)
                behind[v] = behind_bits(at[v]);
              std::vector<Primitive> want_group;
              std::vector<bool> want_culled;
              for (int w = first; w < end; w++) {
                metal::recorder one;
                Mesh one_mesh{&one};
                uint one_behind[mesh_vertices];
                for (int v = 0; v < mesh_vertices; v++) {
                  auto location = workload_vertex(workloads[w], v);
                  one_behind[v] = location.active ? behind_bits(corner(location.uv.x, location.uv.y)) : 0;
                }
                if (vertices == 3)
                  generatePrimitiveTriangle(w, data, one_mesh, one_behind);
                else if (vertices == 2)
                  generatePrimitiveLine(w, data, one_mesh, one_behind);
                else
                  generatePrimitivePoint(w, data, one_mesh, one_behind);
                for (uint n = 0; n < one.primitives; n++) {
                  Primitive p;
                  for (int c = 0; c < vertices; c++) {
                    auto location = workload_vertex(workloads[w], one.indices[vertices * n + c]);
                    p.push_back(corner(location.uv.x, location.uv.y));
                  }
                  want_group.push_back(p);
                  want_culled.push_back(one.culled[n]);
                }
              }
              metal::recorder lanes;
              Mesh lanes_mesh{&lanes};
              for (int lane = 0; lane < 32; lane++)
                if (vertices == 3)
                  generateGroupTriangle(first, end, data, lanes_mesh, behind, lane);
                else if (vertices == 2)
                  generateGroupLine(first, end, data, lanes_mesh, behind, lane);
                else
                  generateGroupPoint(first, end, data, lanes_mesh, behind, lane);
              std::vector<Primitive> got_group;
              bool bad_index = lanes.indices.size() < lanes.primitives * vertices;
              for (uint n = 0; n < lanes.primitives && !bad_index; n++) {
                Primitive p;
                for (int c = 0; c < vertices; c++) {
                  uint index = lanes.indices[vertices * n + c];
                  bad_index = bad_index || index >= at.size();
                  p.push_back(bad_index ? Corner{} : at[index]);
                }
                got_group.push_back(p);
                grouped.insert(primitive(p));
              }
              lanes.culled.resize(lanes.primitives);
              if ((int)lanes.primitives > primitive_limit)
                fail("primitives in a group", f, lanes.primitives, primitive_limit);
              if (bad_index || got_group != want_group)
                fail("primitives of a group", f, got_group.size(), want_group.size());
              else if (lanes.culled != want_culled)
                fail("primitives culled in a group", f, lanes.primitives, want_culled.size());
              lanes_checked += lanes.primitives;
            }
            if (expect_first != count)
              fail("workloads in groups", f, expect_first, count);
            // reordered, the patch's primitives are still the reference's
            if (grouped != expected(domain, partitioning, f, vertices))
              fail("primitives in groups", f, grouped.size(), expected(domain, partitioning, f, vertices).size());
          }
          auto want = expected(domain, partitioning, f, vertices);
          if (made != want)
            fail(vertices == 1 ? "points" : vertices == 2 ? "lines" : "triangles", f, made.size(), want.size());
          if (looked != made)
            fail("primitives looked up", f, looked.size(), made.size());
          primitives += want.size();
        }
        patches_checked++;
      }
    }
  if (failures) {
    printf("failed: %u wrong\n", failures);
    return 1;
  }
  printf("passed: %zu primitives of %u patches as the reference's; %zu frames in %zu zip slots; %zu primitives made "
         "alike by groups' threadgroups\n",
         primitives, patches_checked, frames_checked, zip_slots, lanes_checked);
  return 0;
}
