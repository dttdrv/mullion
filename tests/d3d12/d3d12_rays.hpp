// shared by the ray tracing tests: a scene, as the inputs of its acceleration structures and as a CPU ray caster that
// follows the DXR functional spec ("TraceRay control flow", "Ray extents", D3D12_RAYTRACING_INSTANCE_FLAGS and the ray
// flags). rays run along +z, so what a ray hits is decided in x and y.
#pragma once
#include "d3d12_test.hpp"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <functional>

enum {
  RAY_FORCE_OPAQUE = 0x1,
  RAY_FORCE_NON_OPAQUE = 0x2,
  RAY_ACCEPT_FIRST_HIT = 0x4,
  RAY_SKIP_CLOSEST_HIT = 0x8,
  RAY_CULL_BACK = 0x10,
  RAY_CULL_FRONT = 0x20,
  RAY_CULL_OPAQUE = 0x40,
  RAY_CULL_NON_OPAQUE = 0x80,
  RAY_SKIP_TRIANGLES = 0x100,
  RAY_SKIP_PROCEDURAL = 0x200,
};

struct Triangle {
  float v[3][3];
  UINT geometry, primitive;
  bool opaque;
};
struct Box {
  float min[3], max[3];
  UINT geometry, primitive;
  bool opaque;
};
struct Structure {
  std::vector<Triangle> triangles;
  std::vector<Box> boxes;
};
struct Instance {
  const Structure *structure; // null for an instance of no structure
  float translation[3];
  UINT id, mask, contribution, flags;
};

// the closest hit a ray commits: nothing (0), a triangle (1) or a procedural primitive (2), and where
struct Hit {
  UINT status, instance, geometry, primitive, front;
  float t, bary[2], origin[3]; // the ray's origin in the instance's space
};

// what the application decides while a ray is traced: whether a triangle that is not opaque is a hit (it is, without
// a function), and whether a procedural primitive is one at its box's near face (it is not, without one). `opaque` is
// the primitive's opacity after the instance's and the ray's flags
struct Decisions {
  std::function<bool(UINT instance, const Triangle &)> triangle;
  std::function<bool(UINT instance, const Box &, bool opaque)> box;
};

// whether a primitive is opaque to a ray: its geometry's flag, then its instance's FORCE flags, then the ray's
inline bool
opaque_to(bool geometry, UINT instance_flags, UINT ray_flags) {
  bool is = geometry;
  if (instance_flags & D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE)
    is = true;
  if (instance_flags & D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE)
    is = false;
  if (ray_flags & RAY_FORCE_OPAQUE)
    is = true;
  if (ray_flags & RAY_FORCE_NON_OPAQUE)
    is = false;
  return is;
}

// `margin` is how far the ray stays from an edge, where rounding would decide
inline Hit
cast(
    const std::vector<Instance> &scene, UINT flags, UINT mask, float tmin, float tmax, float x, float y, float z,
    const Decisions &decide, float &margin
) {
  Hit best{};
  float closest = tmax;
  for (UINT i = 0; i < scene.size(); i++) {
    auto &instance = scene[i];
    if (!instance.structure || !(instance.mask & mask & 0xff))
      continue;
    float o[3] = {x - instance.translation[0], y - instance.translation[1], z - instance.translation[2]};
    auto opaque = [&](bool geometry) { return opaque_to(geometry, instance.flags, flags); };
    auto culled = [&](bool is_opaque) { return flags & (is_opaque ? RAY_CULL_OPAQUE : RAY_CULL_NON_OPAQUE); };
    for (auto &tri : instance.structure->triangles) {
      if (flags & RAY_SKIP_TRIANGLES)
        break;
      float e1[3], e2[3];
      for (int k = 0; k < 3; k++) {
        e1[k] = tri.v[1][k] - tri.v[0][k];
        e2[k] = tri.v[2][k] - tri.v[0][k];
      }
      // the ray's point as v0 + u e1 + v e2 in x and y: the barycentrics are the weights of the second and third
      // vertices
      float det = e1[0] * e2[1] - e1[1] * e2[0], px = o[0] - tri.v[0][0], py = o[1] - tri.v[0][1];
      float u = (px * e2[1] - py * e2[0]) / det, v = (e1[0] * py - e1[1] * px) / det;
      margin = std::min({margin, std::abs(u), std::abs(v), std::abs(1 - u - v)});
      if (u < 0 || v < 0 || u + v > 1)
        continue;
      // clockwise from the ray's origin is the front, unless the instance says counterclockwise: in a left-handed
      // space that is a normal against the ray
      bool front = (det < 0) != bool(instance.flags & D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE);
      if (!(instance.flags & D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE) &&
          (flags & (front ? RAY_CULL_FRONT : RAY_CULL_BACK)))
        continue;
      bool is_opaque = opaque(tri.opaque);
      float t = tri.v[0][2] + u * e1[2] + v * e2[2] - o[2];
      // a triangle is hit strictly inside the ray's extent
      if (culled(is_opaque) || !(t > tmin && t < closest) || (!is_opaque && decide.triangle && !decide.triangle(i, tri)))
        continue;
      closest = t;
      best = {1, i, tri.geometry, tri.primitive, front, t, {u, v}, {o[0], o[1], o[2]}};
    }
    for (auto &box : instance.structure->boxes) {
      if (flags & RAY_SKIP_PROCEDURAL)
        break;
      for (int k = 0; k < 2; k++)
        margin = std::min({margin, std::abs(o[k] - box.min[k]), std::abs(o[k] - box.max[k])});
      bool is_opaque = opaque(box.opaque);
      float t = box.min[2] - o[2];
      // a procedural primitive may be hit at the ends of the extent too
      if (o[0] < box.min[0] || o[0] > box.max[0] || o[1] < box.min[1] || o[1] > box.max[1] || culled(is_opaque) ||
          !(t >= tmin && t <= closest) || !decide.box || !decide.box(i, box, is_opaque))
        continue;
      closest = t;
      best = {2, i, box.geometry, box.primitive, 0, t, {0, 0}, {o[0], o[1], o[2]}};
    }
  }
  return best;
}

// the scene and its acceleration structures
struct RayScene {
  // the triangle structure: an opaque geometry of a front and a back facing triangle, and a non-opaque quad that its
  // transform moves in front of them
  static constexpr float quad_shift = -1;
  static constexpr UINT first_quad_vertex = 6;
  float vertices[10][3] = {{0, 0, 0}, {0, 4, 0}, {4, 0, 0}, {4, 4, 1}, {0, 4, 1},
                           {4, 0, 1}, {1, 1, 0}, {1, 3, 0}, {3, 1, 0}, {3, 3, 0}};
  const uint16_t quad_indices[6] = {0, 1, 2, 3, 2, 1};
  const float quad_transform[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, quad_shift}};
  // the box structure: an opaque geometry of two boxes, one behind the triangles and one in front
  const D3D12_RAYTRACING_AABB aabbs[2] = {{0.5f, 0.5f, 2, 1.5f, 1.5f, 3}, {2, 2, -3, 3.5f, 3.5f, -2}};
  Structure triangles, boxes;
  // instances: of each structure alone, with flags that change opacity, winding and culling, with distinct masks,
  // one of no structure, and the boxes again over the first triangles. each contributes twice its index to the hit
  // group index, a record for each geometry there is in a structure
  std::vector<Instance> instances;

  // the rays: a grid of `size` by `size`, apart by `step`, from z0 along +z, clear of every edge
  static constexpr UINT size = 40;
  static constexpr float step = 0.25f, x0 = 0.11f, y0 = 0.13f, z0 = -10;

  RayScene() {
    triangles = triangle_structure();
    for (UINT p = 0; p < std::size(aabbs); p++) {
      auto &b = aabbs[p];
      boxes.boxes.push_back({{b.MinX, b.MinY, b.MinZ}, {b.MaxX, b.MaxY, b.MaxZ}, 0, p, true});
    }
    const UINT front_ccw = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE,
               force_opaque = D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE,
               force_non_opaque = D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE,
               cull_disable = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
    instances = {
        {&triangles, {0, 0, 0}, 10, 0x01, 0, 0},
        {&triangles, {5, 0, 0}, 11, 0x02, 0, force_opaque | front_ccw},
        {&triangles, {0, 5, 0}, 12, 0x04, 0, force_non_opaque | cull_disable},
        {&boxes, {5, 5, 0}, 13, 0x08, 0, 0},
        {nullptr, {0, 0, 0}, 14, 0xff, 0, 0},
        {&boxes, {0, 0, 0}, 15, 0x10, 0, force_non_opaque},
    };
    for (UINT i = 0; i < instances.size(); i++)
      instances[i].contribution = 2 * i;
  }

  // of the vertices as they are now
  Structure
  triangle_structure() const {
    Structure s;
    for (UINT p = 0; p < first_quad_vertex / 3; p++) {
      Triangle t{{}, 0, p, true};
      memcpy(t.v, vertices[3 * p], sizeof(t.v));
      s.triangles.push_back(t);
    }
    for (UINT p = 0; p < std::size(quad_indices) / 3; p++) {
      Triangle t{{}, 1, p, false};
      for (UINT k = 0; k < 3; k++) {
        memcpy(t.v[k], vertices[first_quad_vertex + quad_indices[3 * p + k]], sizeof(t.v[k]));
        t.v[k][2] += quad_shift;
      }
      s.triangles.push_back(t);
    }
    return s;
  }

  // the build inputs, in one upload buffer
  struct Inputs {
    float vertices[sizeof(vertices) / sizeof(vertices[0])][3];
    uint16_t indices[sizeof(quad_indices) / sizeof(quad_indices[0])];
    float transform[3][4];
    D3D12_RAYTRACING_AABB aabbs[sizeof(aabbs) / sizeof(aabbs[0])];
    D3D12_RAYTRACING_INSTANCE_DESC instances[8];
  } *inputs = nullptr;
  ComPtr<ID3D12Resource> input, scratch, triangle_memory, box_memory, scene_memory;
  D3D12_RAYTRACING_GEOMETRY_DESC triangle_geometries[2]{}, box_geometry{};
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS triangle_inputs{}, box_inputs{}, scene_inputs{};
  UINT64 triangle_size = 0, box_size = 0, scene_size = 0, scratch_size = 0;

  static ComPtr<ID3D12Resource>
  memory(ID3D12Device *device, UINT64 bytes) {
    return buffer(
        device, D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
    );
  }

  // the inputs, and memory for the structures and their builds' scratch data as the prebuild information sizes them
  HRESULT
  create(ID3D12Device5 *device) {
    input = buffer(device, D3D12_HEAP_TYPE_UPLOAD, sizeof(*inputs), D3D12_RESOURCE_STATE_GENERIC_READ);
    if (HRESULT hr = input->Map(0, nullptr, (void **)&inputs); FAILED(hr))
      return hr;
    memcpy(inputs->vertices, vertices, sizeof(vertices));
    memcpy(inputs->indices, quad_indices, sizeof(quad_indices));
    memcpy(inputs->transform, quad_transform, sizeof(quad_transform));
    memcpy(inputs->aabbs, aabbs, sizeof(aabbs));
    auto at = [&](const void *field) {
      return input->GetGPUVirtualAddress() + ((const char *)field - (const char *)inputs);
    };
    triangle_geometries[0].Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    triangle_geometries[0].Triangles = {0, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R32G32B32_FLOAT, 0, first_quad_vertex, 0,
                                        {at(inputs->vertices), sizeof(vertices[0])}};
    triangle_geometries[1].Triangles = {at(inputs->transform), DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R32G32B32_FLOAT,
                                        (UINT)std::size(quad_indices), (UINT)std::size(vertices) - first_quad_vertex,
                                        at(inputs->indices), {at(inputs->vertices[first_quad_vertex]), sizeof(vertices[0])}};
    box_geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
    box_geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    box_geometry.AABBs = {std::size(aabbs), {at(inputs->aabbs), sizeof(aabbs[0])}};
    const auto bottom = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    triangle_inputs = {bottom,
                       D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE |
                           D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION,
                       2, D3D12_ELEMENTS_LAYOUT_ARRAY};
    triangle_inputs.pGeometryDescs = triangle_geometries;
    box_inputs = {bottom, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE, 1, D3D12_ELEMENTS_LAYOUT_ARRAY};
    box_inputs.pGeometryDescs = &box_geometry;
    scene_inputs = {D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL,
                    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE, (UINT)instances.size(),
                    D3D12_ELEMENTS_LAYOUT_ARRAY};
    scene_inputs.InstanceDescs = at(inputs->instances);
    auto prebuild = [&](const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS &in) {
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
      device->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
      scratch_size = std::max({scratch_size, info.ScratchDataSizeInBytes, info.UpdateScratchDataSizeInBytes});
      return info.ResultDataMaxSizeInBytes;
    };
    triangle_size = prebuild(triangle_inputs);
    box_size = prebuild(box_inputs);
    scene_size = prebuild(scene_inputs);
    if (!triangle_size || !box_size || !scene_size || !scratch_size)
      return E_FAIL;
    triangle_memory = memory(device, triangle_size);
    box_memory = memory(device, box_size);
    scene_memory = memory(device, scene_size);
    scratch = buffer(
        device, D3D12_HEAP_TYPE_DEFAULT, scratch_size, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
    );
    return S_OK;
  }

  // a build into `to`, or with `from` an update of it, then the barrier after which the structure can be used
  void
  build(
      ID3D12GraphicsCommandList4 *list, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS &in,
      ID3D12Resource *to, ID3D12Resource *from = nullptr, UINT infos = 0,
      const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *info = nullptr
  ) {
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{to->GetGPUVirtualAddress(), in,
                                                            from ? from->GetGPUVirtualAddress() : 0,
                                                            scratch->GetGPUVirtualAddress()};
    list->BuildRaytracingAccelerationStructure(&desc, infos, info);
    D3D12_RESOURCE_BARRIER barrier{D3D12_RESOURCE_BARRIER_TYPE_UAV};
    list->ResourceBarrier(1, &barrier);
  }

  // the top-level structure of the instances, over the given bottom-level ones
  void
  build_scene(ID3D12GraphicsCommandList4 *list, ID3D12Resource *triangle_structure, ID3D12Resource *box_structure) {
    for (UINT i = 0; i < instances.size(); i++) {
      auto &in = instances[i];
      auto &desc = inputs->instances[i] = {};
      for (UINT k = 0; k < 3; k++) {
        desc.Transform[k][k] = 1;
        desc.Transform[k][3] = in.translation[k];
      }
      desc.InstanceID = in.id;
      desc.InstanceMask = in.mask;
      desc.InstanceContributionToHitGroupIndex = in.contribution;
      desc.Flags = in.flags;
      desc.AccelerationStructure = !in.structure         ? 0
                                   : in.structure == &boxes ? box_structure->GetGPUVirtualAddress()
                                                            : triangle_structure->GetGPUVirtualAddress();
    }
    build(list, scene_inputs, scene_memory.Get());
  }

  float
  ray_x(UINT i) const {
    return (i % size) * step + x0;
  }
  float
  ray_y(UINT i) const {
    return (i / size) * step + y0;
  }
  // whether no ray passes where rounding decides what it hits: 64 units in the last place of the largest coordinate
  static bool
  clear(float margin) {
    return margin >= 64 * FLT_EPSILON * size * step;
  }
};
