#pragma once

#include "airconv_public.h"
#include "d3dcommon.h"
#include <cstdint>
#include <utility>

namespace dxmt {

// a geometry draw's object threadgroups (SM50GeometryWarp): the vertices each reads, and how far the next one
// starts. `registers` are the vertex shader's output registers
inline std::pair<uint32_t, uint32_t>
get_gs_vertex_count(D3D_PRIMITIVE_TOPOLOGY topology, uint32_t registers) {
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
  auto warp = SM50GeometryWarp(per_primitive, strip, registers);
  return {warp.threads, warp.vertices};
}

} // namespace dxmt
