// contract: an object grid fits both its group limit and the payload and mesh allocation budget. Metal Feature
// Set Tables, footnote 10: "Mesh shaders can use up to 4 GB of payload and mesh geometry per draw for devices in
// the Apple7 and Apple8 GPU families." WWDC22 10162, 8:50: "payload size can't exceed the limit of 16 kilobytes."
// and 11:54: "The total size of metal::mesh cannot exceed 16 kilobytes." each object reserves one payload and a
// mesh for each geometry instance and each of the primitives a mesh threadgroup takes (one, or the several of a
// pipeline whose mesh threadgroups run several geometry shader invocations); if that allocation fits once, at least
// one group must fit.
#include "../../src/mullion/mullion_geometry.hpp"
#include <cstdio>
#include <vector>

int
main() {
  constexpr uint64_t payload_bytes = 16 * 1024, mesh_bytes = 16 * 1024, draw_bytes = uint64_t(1) << 32;
  std::vector<D3D_PRIMITIVE_TOPOLOGY> topologies = {
      D3D_PRIMITIVE_TOPOLOGY_POINTLIST, D3D_PRIMITIVE_TOPOLOGY_LINELIST, D3D_PRIMITIVE_TOPOLOGY_LINESTRIP,
      D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP,
      D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ, D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ,
      D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ,
  };
  for (uint32_t t = D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST;
       t <= D3D_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST; t++)
    topologies.push_back(D3D_PRIMITIVE_TOPOLOGY(t));
  unsigned wrong = 0, checked = 0;
  for (auto topology : topologies)
    // all register counts through a payload full of float4s cross every warp sizing boundary, including no room
    for (uint32_t registers = 0; registers <= SM50_GEOMETRY_PAYLOAD_SIZE / sizeof(float[4]) + 1; registers++)
      for (uint32_t instances : {0u, 1u, 32u})
      for (uint32_t invocations : {1u, 2u, 3u, 32u}) {
        auto warp = mullion::get_gs_warp(topology, registers);
        uint64_t meshes = (uint64_t(warp.primitives) + invocations - 1) / invocations;
        uint64_t allocation = payload_bytes + meshes * instances * mesh_bytes;
        for (uint64_t budget : {uint64_t(0), allocation - 1, allocation, allocation + 1, draw_bytes})
          for (uint64_t grid : {uint64_t(1), uint64_t(UINT32_MAX)}) {
            auto groups =
                mullion::get_gs_max_object_threadgroups(topology, registers, instances, grid, budget, invocations);
            checked++;
            if (groups > grid || groups * allocation > budget || (budget >= allocation && !groups)) {
              if (wrong++ < 16)
                printf("wrong: topology=%u registers=%u instances=%u invocations=%u grid=%llu budget=%llu "
                       "allocation=%llu groups=%llu\n",
                       unsigned(topology), registers, instances, invocations, (unsigned long long)grid,
                       (unsigned long long)budget,
                       (unsigned long long)allocation, (unsigned long long)groups);
            }
          }
      }
  printf("%s: %u wrong results of %u grids\n", wrong ? "failed" : "passed", wrong, checked);
  return wrong != 0;
}
