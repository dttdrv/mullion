// contract: a patch drawn from the device's tessellation templates (air_tessellation.metal's tess_template) has the
// reference tessellator's triangles (D3D11.3 11.7), each its three domain locations in clockwise order, in whatever
// order they come (11.7.9): a patch whose factors all have the same points is tessellated alike whatever their
// fractions, which place its points, so one template of each domain and points a side draws every such patch. its
// meshlets keep the template's limits, and every corner names one of the meshlet's vertices. this runs the templates
// the device makes (src/mullion/mullion_tessellation_templates.cpp), from every uniform factor of every partitioning,
// whole and not, and checks that a patch of unequal points is drawn from none.
#include "../tessellator.hpp"
#include "mullion_tessellation_templates.hpp"
#include <cstring>
#include <set>
// last: it names Metal's address spaces away
#include "metal_stdlib"

namespace mullion::tessellation {
namespace {
#include "air_tessellation.metal"
}
}

using namespace tessellator;
using Corner = std::array<uint32_t, 2>;
using Primitive = std::vector<Corner>;

static Corner
corner(float2 at) {
  Corner c;
  memcpy(&c[0], &at.x, 4);
  memcpy(&c[1], &at.y, 4);
  return c;
}

static Primitive
primitive(Primitive p) {
  return std::min({p, Primitive{p[1], p[2], p[0]}, Primitive{p[2], p[0], p[1]}});
}

static std::multiset<Primitive>
expected(Domain domain, Partitioning partitioning, float factor) {
  float factors[6] = {factor, factor, factor, factor, factor, factor};
  std::multiset<Primitive> out;
  auto corners = reference::corners(domain, partitioning, factors, 3);
  for (size_t i = 0; i + 3 <= corners.size(); i += 3) {
    Primitive p;
    for (int c = 0; c < 3; c++) {
      Corner at;
      memcpy(&at[0], &corners[i + c].u, 4);
      memcpy(&at[1], &corners[i + c].v, 4);
      p.push_back(at);
    }
    out.insert(primitive(p));
  }
  return out;
}

int
main() {
  namespace t = mullion::tessellation;
  auto &words = mullion::TessellationTemplates();
  const uint *templates = words.data();
  unsigned failures = 0, patches = 0;
  size_t triangles = 0, vertices = 0, meshlets = 0;
  auto fail = [&](const char *what, Domain domain, Partitioning partitioning, float factor, size_t got, size_t want) {
    if (failures++ < 8)
      printf("%s, %s, factor %g: %s: %zu, want %zu\n", domain_names[domain], partitioning_names[partitioning], factor,
             what, got, want);
  };
  if (words.size() < size_t(t::template_meshlets) || words[0] != uint(t::template_version)) {
    printf("failed: no templates\n");
    return 1;
  }
  for (Domain domain : {reference::triangle, reference::quad})
    for (Partitioning partitioning :
         {reference::integer, reference::pow2, reference::fractional_odd, reference::fractional_even})
      for (int step = 4; step <= 64 * 4; step++) {
        // whole factors and fractions of them, up to 64
        float factor = step / 4.0f + (step % 4 == 3 ? 0.37f : 0.0f);
        bool quad = domain == reference::quad;
        t::quad_patch quad_patch;
        t::tri_patch tri_patch;
        auto p = t::partitioning(int(partitioning));
        bool made = quad ? t::make_patch(quad_patch, p, factor, factor, factor, factor, factor, factor)
                         : t::make_patch(tri_patch, p, factor, factor, factor, factor);
        if (!made)
          continue;
        int n = quad ? t::template_points(quad_patch) : t::template_points(tri_patch);
        t::tess_template drawn = t::template_of(templates, quad, n);
        // every such patch has a template: its factors have the same points
        if (!n || !drawn.count) {
          fail("a uniform patch drawn from no template", domain, partitioning, factor, n, 1);
          continue;
        }
        std::multiset<Primitive> got;
        for (int m = 0; m < drawn.count; m++) {
          t::tess_meshlet meshlet = t::meshlet_of(templates, drawn.first + m);
          if (meshlet.vertex_count > uint(t::template_vertices) || meshlet.triangle_count > uint(t::template_primitives))
            fail("a meshlet's size", domain, partitioning, factor, meshlet.vertex_count, t::template_vertices);
          std::vector<Corner> at;
          for (uint v = 0; v < meshlet.vertex_count; v++) {
            uint code = templates[meshlet.vertices + v];
            at.push_back(corner(quad ? t::template_point(quad_patch, code) : t::template_point(tri_patch, code)));
          }
          for (uint i = 0; i < meshlet.triangle_count; i++) {
            uint word = templates[meshlet.triangles + i];
            Primitive p;
            for (int c = 0; c < 3; c++) {
              uint v = word >> (8 * c) & 0xff;
              if (v >= at.size()) {
                fail("a corner past the meshlet's vertices", domain, partitioning, factor, v, at.size());
                v = 0;
              }
              p.push_back(at[v]);
            }
            got.insert(primitive(p));
          }
          vertices += meshlet.vertex_count;
          meshlets++;
        }
        auto want = expected(domain, partitioning, factor);
        if (got != want)
          fail("triangles", domain, partitioning, factor, got.size(), want.size());
        triangles += got.size();
        patches++;
      }
  // a patch whose edges have points unlike its inside's is drawn from no template
  t::quad_patch uneven;
  if (t::make_patch(uneven, t::partitioning::integer, 8, 8, 8, 9, 8, 8) && t::template_points(uneven))
    fail("a template of unequal points", reference::quad, reference::integer, 9, 1, 0);
  if (failures) {
    printf("failed: %u wrong\n", failures);
    return 1;
  }
  printf("passed: %zu triangles of %u patches as the reference's; %zu meshlets, %.2f vertices a triangle; %zu KB of "
         "templates\n",
         triangles, patches, meshlets, double(vertices) / triangles, words.size() * 4 / 1024);
  return 0;
}
