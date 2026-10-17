#pragma once

#include "airconv_public.h"
#include "d3dcommon.h"
#include "log/log.hpp"
#include <cstdint>
#include <algorithm>
#include <optional>
#include <utility>

namespace dxmt {

// a geometry draw's object threadgroup (SM50GeometryWarp) for `topology`, from a vertex shader with `registers`
// output registers
inline SM50_GEOMETRY_WARP
get_gs_warp(D3D_PRIMITIVE_TOPOLOGY topology, uint32_t registers) {
  uint32_t per_primitive;
  switch (topology) {
  case D3D_PRIMITIVE_TOPOLOGY_POINTLIST:
    per_primitive = 1;
    break;
  case D3D_PRIMITIVE_TOPOLOGY_LINELIST:
  case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:
    per_primitive = 2;
    break;
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
    per_primitive = 3;
    break;
  case D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ:
  case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ:
    per_primitive = 4;
    break;
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ:
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ:
    per_primitive = 6;
    break;
  default:
    // a patch list's control points
    per_primitive = uint32_t(topology) - uint32_t(D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST) + 1;
    break;
  }
  bool strip = topology == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP || topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP ||
               topology == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ || topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ;
  return SM50GeometryWarp(per_primitive, strip, registers);
}

// a geometry draw's object threadgroups: the vertices each reads, and how far the next one starts
inline std::pair<uint32_t, uint32_t>
get_gs_vertex_count(D3D_PRIMITIVE_TOPOLOGY topology, uint32_t registers) {
  auto warp = get_gs_warp(topology, registers);
  return {warp.threads, warp.vertices};
}

// the most object threadgroups a geometry draw may have: each starts a mesh threadgroup for each of its primitives
// and geometry instances, each mesh threadgroup takes 2 KB at least, and the draw's mesh threadgroups have no more
// than `mesh_budget` bytes in all. a mesh threadgroup of over 2 KB takes more, which this does not count
inline uint64_t
get_gs_max_object_threadgroups(
    D3D_PRIMITIVE_TOPOLOGY topology, uint32_t registers, uint32_t instances, uint64_t max_object_threadgroups,
    uint64_t mesh_budget
) {
  uint64_t per_object = uint64_t(get_gs_warp(topology, registers).primitives) * instances;
  if (!per_object)
    return max_object_threadgroups;
  return std::min(max_object_threadgroups, mesh_budget / per_object / 2048);
}

// the most object threadgroups each Metal draw of a geometry draw of `warps` x `instances` may have, when it has more
// than `max` (wmtcmd_render_dxmt_geometry_draw: 0 is one draw), or nothing when not one object threadgroup fits
inline std::optional<uint32_t>
geometry_draw_parts(uint32_t warps, uint32_t instances, uint64_t max) {
  if (!max) {
    WARN("Omitted mesh draw (GS): not one object threadgroup fits the mesh memory of a draw (", warps, "x", instances, ")");
    return std::nullopt;
  }
  return uint64_t(warps) * instances <= max ? 0 : uint32_t(std::min<uint64_t>(max, UINT32_MAX));
}

// how many Metal draws an indirect geometry draw is encoded as, the GS dispatch marshal filling each one's grid and
// origin: one when a draw may have any number of object threadgroups (`max` of 2^32 or more), otherwise enough for
// `bound` object threadgroups, or kIndirectGeometryParts when the draw has no known bound
constexpr uint32_t kIndirectGeometryParts = 4;

inline uint32_t
indirect_geometry_parts(uint64_t max, uint64_t bound) {
  if (max >= (1ull << 32))
    return 1;
  if (!bound)
    return kIndirectGeometryParts;
  return uint32_t(std::clamp<uint64_t>((bound + std::max<uint64_t>(max, 1) - 1) / std::max<uint64_t>(max, 1), 1, 16));
}

} // namespace dxmt
