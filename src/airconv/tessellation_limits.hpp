// what a tessellation pipeline's stages are sized for: how air_tessellation.metal cuts a patch of the largest factor
#pragma once
#include "DXBCParser/d3d12tokenizedprogramformat.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace dxmt::dxbc {

// the most segments an edge is cut into by factors up to `factor`
inline uint32_t
get_integer_factor(float factor, microsoft::D3D11_SB_TESSELLATOR_PARTITIONING partitioning) {
  uint32_t integer_factor = 0;
  switch (partitioning) {
  case microsoft::D3D11_SB_TESSELLATOR_PARTITIONING_UNDEFINED:
    return 0;
  case microsoft::D3D11_SB_TESSELLATOR_PARTITIONING_INTEGER:
  case microsoft::D3D11_SB_TESSELLATOR_PARTITIONING_POW2: {
    factor = std::clamp<float>(factor, 1.0, 64.0);
    integer_factor = std::ceil(factor);
    break;
  }
  case microsoft::D3D11_SB_TESSELLATOR_PARTITIONING_FRACTIONAL_ODD: {
    factor = std::clamp<float>(factor, 1.0, 63.0);
    integer_factor = std::ceil(factor);
    integer_factor = integer_factor & 1 ? integer_factor : integer_factor + 1;
    break;
  }
  case microsoft::D3D11_SB_TESSELLATOR_PARTITIONING_FRACTIONAL_EVEN: {
    factor = std::clamp<float>(factor, 2.0, 64.0);
    integer_factor = std::ceil(factor);
    integer_factor = integer_factor & 1 ? integer_factor + 1 : integer_factor;
    break;
  }
  }
  return integer_factor;
}

// the workloads air_tessellation.metal's emit_workloads writes for a patch of `sides` sides, whose inside has
// `points` points along its shorter axis and `long_points` along its longer
inline uint32_t
workload_count(uint32_t sides, uint32_t points, uint32_t long_points) {
  uint32_t rings = (points - 1) >> 1, long_rings = (long_points - 1) >> 1, count = 0;
  bool last = !(points & 1);
  for (uint32_t i = 0; i < rings && 2 * i < long_rings; i++) {
    bool middle = 2 * i + 1 == long_rings;
    count += middle ? (sides + 1) / 2 : sides;
    last = last && !(middle && (sides & 1));
  }
  return count + last;
}

inline uint32_t
get_max_potential_workload_count(
    uint32_t max_tess_factor, microsoft::D3D11_SB_TESSELLATOR_DOMAIN domain,
    microsoft::D3D11_SB_TESSELLATOR_PARTITIONING partitioning
) {
  using namespace microsoft;
  if (domain == D3D11_SB_TESSELLATOR_DOMAIN_UNDEFINED)
    return 0;
  // two lines a workload
  if (domain == D3D11_SB_TESSELLATOR_DOMAIN_ISOLINE)
    return (max_tess_factor + 1) / 2;
  bool quad = domain == D3D11_SB_TESSELLATOR_DOMAIN_QUAD;
  // the inside's points along an axis: a fractional partitioning's segments are all odd or all even
  uint32_t step = partitioning == D3D11_SB_TESSELLATOR_PARTITIONING_FRACTIONAL_ODD ||
                          partitioning == D3D11_SB_TESSELLATOR_PARTITIONING_FRACTIONAL_EVEN
                      ? 2
                      : 1;
  // a patch whose factors are all 1 is one workload
  uint32_t most = 1;
  for (uint32_t long_points = max_tess_factor + 1; long_points >= 3; long_points -= step)
    // a triangle's inside has one factor
    for (uint32_t points = long_points; points >= (quad ? 3 : long_points); points -= step)
      most = std::max(most, workload_count(quad ? 4 : 3, points, long_points));
  return most;
}

// the most vertices a workload has: two lines, or two frames, which have at most four more points than two of the
// longest edges
inline uint32_t
get_max_workload_vertices(uint32_t max_tess_factor, microsoft::D3D11_SB_TESSELLATOR_DOMAIN domain) {
  return 2 * (max_tess_factor + 1) + (domain == microsoft::D3D11_SB_TESSELLATOR_DOMAIN_ISOLINE ? 0 : 4);
}

// a mesh's size as Metal counts it, which makes no mesh pipeline of more than 32768 bytes ("Total mesh size ...
// exceeds the maximum mesh size allowed (32768)"): its vertices, a multiple of four of them, and what its primitives
// carry, here each its patch's number. measured on an M5 Pro, 2026-10-05
constexpr uint32_t mesh_size_limit = 32768;

inline uint32_t
mesh_size(uint32_t vertices, uint32_t vertex_size, uint32_t primitives) {
  return (vertices + 3) / 4 * 4 * vertex_size + primitives * sizeof(uint32_t);
}

// the vertices of a primitive the tessellator outputs
inline uint32_t
get_primitive_vertices(microsoft::D3D11_SB_TESSELLATOR_OUTPUT_PRIMITIVE primitive) {
  switch (primitive) {
  case microsoft::D3D11_SB_TESSELLATOR_OUTPUT_POINT:
    return 1;
  case microsoft::D3D11_SB_TESSELLATOR_OUTPUT_LINE:
    return 2;
  default:
    return 3;
  }
}

// how the mesh threadgroups of a workload share its primitives, a vertex of the domain shader's taking `vertex_size`
// bytes. where a mesh has room for the workload's vertices, one threadgroup makes them and all the primitives
// (`primitives` 0). else each makes `primitives` of them, from vertices of their own, and `count` threadgroups make
// the most a workload has, which do not outnumber its vertices
struct tess_pieces {
  uint32_t count, primitives;
};

inline tess_pieces
tessellation_pieces(
    uint32_t factor, microsoft::D3D11_SB_TESSELLATOR_DOMAIN domain,
    microsoft::D3D11_SB_TESSELLATOR_OUTPUT_PRIMITIVE primitive, uint32_t vertex_size
) {
  uint32_t corners = get_primitive_vertices(primitive), vertices = get_max_workload_vertices(factor, domain);
  // a point has its size too
  vertex_size += corners == 1 ? sizeof(float) : 0;
  if (mesh_size(vertices, vertex_size, vertices) <= mesh_size_limit)
    return {1, 0};
  uint32_t primitives = mesh_size_limit / (corners * vertex_size);
  while (mesh_size(primitives * corners, vertex_size, primitives) > mesh_size_limit)
    primitives--;
  return {(vertices + primitives - 1) / primitives, primitives};
}

// the largest factor a pipeline tessellates at, and the segments it cuts an edge into at most: the hull shader's
// largest factor, as far as `most_segments`
inline std::pair<float, uint32_t>
get_final_factor(float factor, microsoft::D3D11_SB_TESSELLATOR_PARTITIONING partitioning, uint32_t most_segments) {
  uint32_t segments = get_integer_factor(factor, partitioning);
  if (segments <= most_segments)
    return {factor, segments};
  factor = std::min(factor, (float)most_segments);
  do {
    segments = get_integer_factor(factor, partitioning);
    if (segments <= most_segments)
      return {factor, segments};
    factor = factor - 1.0f;
  } while (factor > 1.0f);
  segments = get_integer_factor(1.0f, partitioning);
  return {segments, segments};
}

// how the mesh threadgroups of an object threadgroup's patches are shared between grids, each the grid of one object
// threadgroup: the patches have `workloads` workloads at most, a workload takes `primitives` mesh threadgroups (a
// geometry shader's primitives each take one) for each of the geometry shader's `instances` (1 without one), and a
// grid has no more than `limit` (0: not known, and one grid has them all). a grid takes whole workloads, as many as
// fit, and where not even one fits with all its instances, as many of the instances as fit
struct tess_grids {
  uint32_t workloads, instances; // of each grid
  uint32_t instance_parts;       // the grids that share a workload's instances
  uint32_t parts;                // the grids of an object threadgroup's patches
};

inline tess_grids
tessellation_grids(uint32_t workloads, uint32_t primitives, uint32_t instances, uint32_t limit) {
  tess_grids grids{workloads, instances};
  if (limit) {
    grids.instances = std::clamp(limit / primitives, 1u, instances);
    grids.workloads = std::clamp(limit / (primitives * grids.instances), 1u, workloads);
  }
  grids.instance_parts = (instances + grids.instances - 1) / grids.instances;
  grids.parts = (workloads + grids.workloads - 1) / grids.workloads * grids.instance_parts;
  return grids;
}

} // namespace dxmt::dxbc
