#pragma once

#include "../airconv/airconv_public.h"
#include "d3dcommon.h"
#include "../util/log/log.hpp"
#include <cstdint>
#include <algorithm>
#include <optional>
#include <utility>

namespace mullion {

// the vertices of the first primitive and of each following one (D3D11.3 8, primitive topologies)
inline std::pair<uint32_t, uint32_t> get_primitive_vertex_count(D3D_PRIMITIVE_TOPOLOGY topology) {
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
  return {per_primitive, strip ? (topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ ? 2u : 1u) : per_primitive};
}

// a geometry draw's object threadgroup (SM50GeometryWarp) for `topology`, from a vertex shader with `registers`
// output registers
inline SM50_GEOMETRY_WARP
get_gs_warp(D3D_PRIMITIVE_TOPOLOGY topology, uint32_t registers) {
  auto [per_primitive, step] = get_primitive_vertex_count(topology);
  return SM50GeometryWarp(per_primitive, step != per_primitive, registers);
}

// a geometry draw's object threadgroups: the vertices each reads, and how far the next one starts
inline std::pair<uint32_t, uint32_t>
get_gs_vertex_count(D3D_PRIMITIVE_TOPOLOGY topology, uint32_t registers) {
  auto warp = get_gs_warp(topology, registers);
  return {warp.threads, warp.vertices};
}

// Apple7/8 share 4 GB between payload and mesh geometry (Metal Feature Set Tables, footnote 10). allocation
// rounding is not public: bound the payload by the tables' 16 KB cap and each mesh by its 16 KB cap
// (WWDC22 10162, 11:47). an object threadgroup starts a mesh threadgroup for each `mesh_invocations` of its primitives
// (SM50GeometryMeshInvocations), for each instance
inline uint64_t
get_gs_max_object_threadgroups(
    D3D_PRIMITIVE_TOPOLOGY topology, uint32_t registers, uint32_t instances, uint64_t max_object_threadgroups,
    uint64_t mesh_budget, uint32_t mesh_invocations = 1
) {
  constexpr uint32_t max_payload_bytes = 16 * 1024;
  constexpr uint32_t max_mesh_output_bytes = 16 * 1024;
  uint32_t each = mesh_invocations ? mesh_invocations : 1;
  uint64_t meshes = (uint64_t(get_gs_warp(topology, registers).primitives) + each - 1) / each;
  uint64_t per_object = max_payload_bytes + meshes * instances * max_mesh_output_bytes;
  return std::min(max_object_threadgroups, mesh_budget / per_object);
}

// the most object threadgroups each Metal draw of a geometry draw of `warps` x `instances` may have, when it has more
// than `max` (wmtcmd_render_mullion_geometry_draw: 0 is one draw), or nothing when not one object threadgroup fits
inline std::optional<uint32_t>
geometry_draw_parts(uint32_t warps, uint32_t instances, uint64_t max) {
  if (!max) {
    WARN("Omitted mesh draw (GS): not one object threadgroup fits the mesh memory of a draw (", warps, "x", instances, ")");
    return std::nullopt;
  }
  return uint64_t(warps) * instances <= max ? 0 : uint32_t(std::min<uint64_t>(max, UINT32_MAX));
}

// rectangular parts can leave nearly half their capacity unused at instance boundaries
inline uint32_t
indirect_geometry_parts(uint64_t max, uint64_t bound) {
  if (bound <= max)
    return 1;
  auto capacity = std::max<uint64_t>((max + 1) / 2, 1);
  return uint32_t(bound / capacity + (bound % capacity != 0));
}

} // namespace mullion
