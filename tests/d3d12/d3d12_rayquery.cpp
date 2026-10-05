// contract: inline ray tracing (DXR functional spec, "Inline raytracing", "RayQuery", "TraceRay control flow" and
// BuildRaytracingAccelerationStructure). a compute shader sends a grid of parallel rays into a scene of triangle and
// procedural geometry instanced by a top-level structure, drives each RayQuery to its end and records what it
// committed; a CPU ray caster over the same scene, following the spec's rules, says what that must be:
// - the closest hit within (TMin, TMax) among opaque triangles, the non-opaque triangles the shader commits, and
//   the procedural primitives it commits at their boxes' near faces;
// - opacity from the geometry's flag, the instance's FORCE flags, then the ray's; facing from the winding, clockwise
//   being the front, as the instance's flags have it; and the ray flags that cull by opacity, facing and geometry type;
// - instance masks, a null instance, the instance and geometry and primitive numbers, barycentrics, T, the object
//   space ray and both transforms, InstanceContributionToHitGroupIndex and RayFlags with a query's own flags.
// the structure is read as a root SRV and as an SRV in a descriptor table, by a compute shader and by a pixel shader
// that traces the ray of its pixel. afterwards the triangle structure is compacted to a copy and the box structure
// cloned, and a scene of the copies gives the same results; then the first structure is updated in place with a
// moved triangle, and the scene shows the move. a null structure, an address of 0 at either binding, misses every
// ray ("Additional SRV type"), as does a top-level structure of no instances.
// structures are also given back ("CopyRaytracingAccelerationStructure", "CheckDriverMatchingIdentifier" and the
// post-build information): a serialized structure has the size its post-build information says, starts with a header
// the device recognizes and, for a top-level structure, the addresses of its instances' structures; decoded for
// tools it is its build's inputs, "its decoded input data ... self-contained in the destination buffer". what is
// given back is what was built, whatever the input buffers hold since. deserialized into new memory, bottom-level
// structures from bytes a copy in the same list brings and the scene with its pointers naming them there, the
// structures trace as the ones serialized did, also after the originals have changed. a deserialized structure is
// there from its copy's place in the order of execution: lists recorded before that copy has run serialize it again,
// ask for its post-build information and update it, and a build after a deserializing copy to the same memory is
// what the memory then holds. what a copy gives of a structure is what the structure holds when the copy runs: a list
// recorded before a list that updates the structure and run after it serializes the update, and the other way
// around what was there before. a scene may be deserialized before the structures its pointers name are
// ("D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_SERIALIZATION_DESC": they need not be there "until use").
// a geometry's elements are a stride apart, of which only the low 32 bits count ("D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE"):
// positions of a format with an ignored fourth component packed at their own size up to their buffer's end
// (VertexFormat), and at a stride of zero, which boxes may have and vertices are not denied, one element as many times
// as the count says: boxes that are each a candidate of a ray through them, and a triangle of one point that nothing hits.
#include "d3d12_rays.hpp"

static const char hlsl[] = R"hlsl(
RaytracingAccelerationStructure scene : register(t0);
RaytracingAccelerationStructure table_scene : register(t1);
cbuffer Case : register(b0) { uint flags; uint mask; float tmin; float tmax; uint from_table; uint own_flags; };
struct Result {
  uint status, instance, id, geometry, primitive, front, contribution, flags;
  float t; float2 bary; float object_x, to_world_x, to_object_x, tmin, world_z;
};
RWStructuredBuffer<Result> results : register(u0);
// the boxes of the procedural geometry: x and y bounds and the near face's z
static const float boxes[BOXES][5] = { BOX_LIST };

#define TRACE(q)                                                                                                       \
  if (from_table) q.TraceRayInline(table_scene, flags, mask, ray); else q.TraceRayInline(scene, flags, mask, ray);    \
  for (uint steps = 0; steps < MAX_STEPS && q.Proceed(); steps++) {                                                   \
    if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {                                                         \
      if ((q.CandidatePrimitiveIndex() + q.CandidateInstanceID()) % 2 == 0) q.CommitNonOpaqueTriangleHit();           \
    } else {                                                                                                          \
      float3 o = q.CandidateObjectRayOrigin();                                                                        \
      float b[5] = boxes[q.CandidatePrimitiveIndex()];                                                                \
      float t = (b[4] - o.z) / q.CandidateObjectRayDirection().z;                                                     \
      bool inside = o.x > b[0] && o.x < b[1] && o.y > b[2] && o.y < b[3];                                             \
      float limit = q.CommittedStatus() == COMMITTED_NOTHING ? tmax : q.CommittedRayT();                              \
      if (inside && t >= q.RayTMin() && t <= limit) q.CommitProceduralPrimitiveHit(t);                                \
    }                                                                                                                 \
  }                                                                                                                   \
  r.status = q.CommittedStatus();                                                                                     \
  r.flags = q.RayFlags();                                                                                             \
  r.tmin = q.RayTMin();                                                                                               \
  r.world_z = q.WorldRayDirection().z;                                                                                \
  if (r.status != COMMITTED_NOTHING) {                                                                                \
    r.instance = q.CommittedInstanceIndex();                                                                          \
    r.id = q.CommittedInstanceID();                                                                                   \
    r.geometry = q.CommittedGeometryIndex();                                                                          \
    r.primitive = q.CommittedPrimitiveIndex();                                                                        \
    r.contribution = q.CommittedInstanceContributionToHitGroupIndex();                                                \
    r.t = q.CommittedRayT();                                                                                          \
    r.object_x = q.CommittedObjectRayOrigin().x;                                                                      \
    r.to_world_x = q.CommittedObjectToWorld3x4()[0][3];                                                               \
    r.to_object_x = q.CommittedWorldToObject3x4()[0][3];                                                              \
  }                                                                                                                   \
  if (r.status == COMMITTED_TRIANGLE_HIT) {                                                                           \
    r.front = q.CommittedTriangleFrontFace();                                                                         \
    r.bary = q.CommittedTriangleBarycentrics();                                                                       \
  }

Result shoot(uint2 id) {
  RayDesc ray;
  ray.Origin = float3(id.x * STEP + X0, id.y * STEP + Y0, Z0);
  ray.Direction = float3(0, 0, 1);
  ray.TMin = tmin;
  ray.TMax = tmax;
  Result r = (Result)0;
  if (own_flags) {
    RayQuery<OWN_FLAGS> own;
    TRACE(own)
  } else {
    RayQuery<RAY_FLAG_NONE> q;
    TRACE(q)
  }
  return r;
}

[numthreads(1, 1, 1)]
void cs(uint3 id : SV_DispatchThreadID) { results[id.y * SIZE + id.x] = shoot(id.xy); }

// a triangle over the target, and a pixel's ray
float4 vs(uint id : SV_VertexID) : SV_Position { return float4(float2(id & 1, id >> 1) * 4 - 1, 0, 1); }
float4 ps(float4 position : SV_Position) : SV_Target {
  uint2 id = position.xy;
  results[id.y * SIZE + id.x] = shoot(id);
  return 1;
}
)hlsl";

// counts what a ray is offered: which procedural primitives, how often, and what it committed of triangles, which
// are opaque here
static const char count_hlsl[] = R"hlsl(
RaytracingAccelerationStructure scene : register(t0);
RWStructuredBuffer<uint4> counted : register(u0);
[numthreads(1, 1, 1)]
void cs(uint3 id : SV_DispatchThreadID) {
  RayDesc ray;
  ray.Origin = float3(id.x * STEP + X0, id.y * STEP + Y0, Z0);
  ray.Direction = float3(0, 0, 1);
  ray.TMin = 0;
  ray.TMax = 100;
  RayQuery<RAY_FLAG_NONE> q;
  q.TraceRayInline(scene, 0, 0xff, ray);
  uint offered = 0, times = 0;
  for (uint steps = 0; steps < MAX_STEPS && q.Proceed(); steps++)
    if (q.CandidateType() == CANDIDATE_PROCEDURAL_PRIMITIVE) {
      offered |= 1u << q.CandidatePrimitiveIndex();
      times++;
    }
  bool hit = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
  counted[id.y * SIZE + id.x] = uint4(offered, times, hit, hit ? q.CommittedInstanceID() : 0);
}
)hlsl";

struct Result {
  UINT status, instance, id, geometry, primitive, front, contribution, flags;
  float t, bary[2], object_x, to_world_x, to_object_x, tmin, world_z;
};

// the half float of a value that has one exactly
static uint16_t
half(float value) {
  if (!value)
    return 0;
  int exponent;
  float mantissa = std::frexp(value, &exponent); // in [0.5, 1)
  return uint16_t((exponent - 1 + 15) << 10 | (uint32_t)((mantissa * 2 - 1) * 1024));
}

struct Case {
  UINT flags, mask;
  float tmin, tmax;
  UINT from_table, own_flags;
};

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  RayScene scene;
  const UINT size = scene.size, max_steps = 64, own_flags = RAY_SKIP_PROCEDURAL;
  const float z0 = scene.z0, quad_shift = scene.quad_shift;
  // the boxes of the shader: x and y bounds and the near face's z
  std::string box_list;
  for (auto &b : scene.aabbs)
    box_list += (box_list.empty() ? "{" : ", {") + std::to_string(b.MinX) + ", " + std::to_string(b.MaxX) + ", " +
                std::to_string(b.MinY) + ", " + std::to_string(b.MaxY) + ", " + std::to_string(b.MinZ) + "}";
  const float distant = 100;
  const Case cases[] = {
      {0, 0xff, 0, distant},
      {0, 0xff, 0, distant, 1},
      {0, 0xff, 0, distant, 0, 1},
      {RAY_FORCE_OPAQUE, 0xff, 0, distant},
      {RAY_FORCE_NON_OPAQUE, 0xff, 0, distant},
      {RAY_CULL_BACK, 0xff, 0, distant},
      {RAY_CULL_FRONT, 0xff, 0, distant},
      {RAY_CULL_OPAQUE, 0xff, 0, distant},
      {RAY_CULL_NON_OPAQUE, 0xff, 0, distant},
      {RAY_SKIP_TRIANGLES, 0xff, 0, distant},
      {RAY_SKIP_PROCEDURAL, 0xff, 0, distant, 1},
      {RAY_ACCEPT_FIRST_HIT, 0xff, 0, distant},
      {0, 0x0b, 0, distant},
      {0, 0xff, -z0 + quad_shift / 2, -z0 + 0.5f},
      {0, 0xff, -z0 + 0.5f, distant},
  };
  const UINT case_count = std::size(cases), rays = size * size;

  std::vector<std::string> defines = {
      "SIZE=" + std::to_string(size), "STEP=" + std::to_string(scene.step), "X0=" + std::to_string(scene.x0),
      "Y0=" + std::to_string(scene.y0), "Z0=" + std::to_string(z0), "MAX_STEPS=" + std::to_string(max_steps),
      "BOXES=" + std::to_string(std::size(scene.aabbs)), "BOX_LIST=" + box_list, "OWN_FLAGS=" + std::to_string(own_flags),
  };
  auto cs = compiler.compile(hlsl, "cs", "cs_6_5", defines), vs = compiler.compile(hlsl, "vs", "vs_6_5", defines),
       ps = compiler.compile(hlsl, "ps", "ps_6_5", defines);
  if (cs.empty() || vs.empty() || ps.empty()) {
    printf("skipped: RayQuery is shader model 6.5\n");
    return 77;
  }
  ComPtr<ID3D12Device5> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));

  D3D12_ROOT_PARAMETER params[4]{};
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  params[1].DescriptorTable = {1, &range};
  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[2].Constants = {0, 0, sizeof(Case) / sizeof(UINT)};
  params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  auto rs = root_signature(device.Get(), {4, params});
  if (!rs) {
    printf("failed: root signature\n");
    return 1;
  }
  ComPtr<ID3D12PipelineState> pso, pixel_pso;
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute{rs.Get(), bytecode(cs)};
  CHECK(device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&pso)));
  // the pixel shader's target, a pixel for each ray
  const DXGI_FORMAT target_format = DXGI_FORMAT_R8_UNORM;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{rs.Get(), bytecode(vs), bytecode(ps)};
  graphics.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  graphics.SampleMask = ~0u;
  graphics.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  graphics.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  graphics.NumRenderTargets = 1;
  graphics.RTVFormats[0] = target_format;
  graphics.SampleDesc = {1, 0};
  CHECK(device->CreateGraphicsPipelineState(&graphics, IID_PPV_ARGS(&pixel_pso)));
  ComPtr<ID3D12Resource> target;
  D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1, target_format, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  CHECK(device->CreateCommittedResource(
      &default_heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)));
  auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList4> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  auto run = [&]() -> HRESULT {
    HRESULT hr = submit(device.Get(), queue.Get(), list.Get());
    if (SUCCEEDED(hr) && SUCCEEDED(hr = allocator->Reset()))
      hr = list->Reset(allocator.Get(), nullptr);
    return hr;
  };

  CHECK(scene.create(device.Get()));
  // post-build information: the triangle structure's compacted and current sizes
  auto sizes = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, 2 * sizeof(UINT64), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto results = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, rays * sizeof(Result), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto readback = buffer(
      device.Get(), D3D12_HEAP_TYPE_READBACK, case_count * rays * sizeof(Result) + 2 * sizeof(UINT64),
      D3D12_RESOURCE_STATE_COPY_DEST
  );
  const UINT64 sizes_at = case_count * rays * sizeof(Result);
  const uint8_t *out;
  CHECK(readback->Map(0, nullptr, (void **)&out));
  ComPtr<ID3D12DescriptorHeap> views;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&views)));

  // traces every case through the structure at an address, from a compute shader or a pixel shader, and reads the
  // results back
  auto trace_at = [&](D3D12_GPU_VIRTUAL_ADDRESS address, bool pixel) {
    D3D12_SHADER_RESOURCE_VIEW_DESC view{DXGI_FORMAT_UNKNOWN, D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE,
                                         D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
    view.RaytracingAccelerationStructure.Location = address;
    device->CreateShaderResourceView(nullptr, &view, views->GetCPUDescriptorHandleForHeapStart());
    ID3D12DescriptorHeap *heaps[] = {views.Get()};
    list->SetDescriptorHeaps(1, heaps);
    if (pixel) {
      D3D12_VIEWPORT viewport{0, 0, (float)size, (float)size, 0, 1};
      D3D12_RECT scissor{0, 0, (LONG)size, (LONG)size};
      list->SetPipelineState(pixel_pso.Get());
      list->SetGraphicsRootSignature(rs.Get());
      list->SetGraphicsRootShaderResourceView(0, address);
      list->SetGraphicsRootDescriptorTable(1, views->GetGPUDescriptorHandleForHeapStart());
      list->SetGraphicsRootUnorderedAccessView(3, results->GetGPUVirtualAddress());
      list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
      list->RSSetViewports(1, &viewport);
      list->RSSetScissorRects(1, &scissor);
      list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    } else {
      list->SetPipelineState(pso.Get());
      list->SetComputeRootSignature(rs.Get());
      list->SetComputeRootShaderResourceView(0, address);
      list->SetComputeRootDescriptorTable(1, views->GetGPUDescriptorHandleForHeapStart());
      list->SetComputeRootUnorderedAccessView(3, results->GetGPUVirtualAddress());
    }
    for (UINT c = 0; c < case_count; c++) {
      if (pixel) {
        list->SetGraphicsRoot32BitConstants(2, sizeof(Case) / sizeof(UINT), &cases[c], 0);
        list->DrawInstanced(3, 1, 0, 0);
      } else {
        list->SetComputeRoot32BitConstants(2, sizeof(Case) / sizeof(UINT), &cases[c], 0);
        list->Dispatch(size, size, 1);
      }
      transition(list.Get(), results.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyBufferRegion(readback.Get(), c * rays * sizeof(Result), results.Get(), 0, rays * sizeof(Result));
      transition(list.Get(), results.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    transition(list.Get(), sizes.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), sizes_at, sizes.Get(), 0, 2 * sizeof(UINT64));
    transition(list.Get(), sizes.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return run();
  };
  // builds the scene over the given structures first
  auto trace = [&](ID3D12Resource *triangle_structure, ID3D12Resource *box_structure, bool pixel = false) {
    scene.build_scene(list.Get(), triangle_structure, box_structure);
    return trace_at(scene.scene_memory->GetGPUVirtualAddress(), pixel);
  };
  unsigned failures = 0, hits = 0, traced = 0;
  float margin = 1;
  // `nothing`: the rays have nothing to hit
  auto compare = [&](const char *what, bool nothing = false) {
    traced += case_count * rays;
    for (UINT c = 0; c < case_count; c++)
      for (UINT i = 0; i < rays; i++) {
        Result got;
        memcpy(&got, out + (c * rays + i) * sizeof(Result), sizeof(got));
        // the shader's decisions: a triangle that is not opaque is a hit when its primitive index and its instance's ID
        // sum to an even number, and a procedural primitive at its box's near face
        auto &in = cases[c];
        UINT flags = in.flags | (in.own_flags ? own_flags : 0);
        auto hit = cast(
            scene.instances, flags, in.mask, in.tmin, in.tmax, scene.ray_x(i), scene.ray_y(i), z0,
            {[&](UINT instance, const Triangle &t) { return (t.primitive + scene.instances[instance].id) % 2 == 0; },
             [](UINT, const Box &, bool) { return true; }},
            margin
        );
        Result want{0, 0, 0, 0, 0, 0, 0, flags, 0, {0, 0}, 0, 0, 0, in.tmin, 1};
        if (hit.status && !nothing) {
          auto &instance = scene.instances[hit.instance];
          want = {hit.status, hit.instance, instance.id, hit.geometry, hit.primitive, hit.front, instance.contribution,
                  flags, hit.t, {hit.bary[0], hit.bary[1]}, hit.origin[0], instance.translation[0],
                  -instance.translation[0], in.tmin, 1};
        }
        hits += want.status != 0;
        // a ray that takes its first hit commits one of those there are
        bool same = cases[c].flags & RAY_ACCEPT_FIRST_HIT
                        ? !got.status == !want.status
                        : !memcmp(&got, &want, offsetof(Result, t)) &&
                              std::equal(&got.t, &got.world_z + 1, &want.t, [](float x, float y) {
                                return std::abs(x - y) < 1e-3f;
                              });
        if (!same && failures++ < 12)
          printf(
              "%s, case %u, ray %u,%u: status %u instance %u id %u geometry %u primitive %u front %u contribution %u "
              "flags %#x t %g bary %g,%g object %g world %g,%g tmin %g z %g; want %u %u %u %u %u %u %u %#x %g %g,%g "
              "%g %g,%g %g %g\n",
              what, c, i % size, i / size, got.status, got.instance, got.id, got.geometry, got.primitive, got.front,
              got.contribution, got.flags, got.t, got.bary[0], got.bary[1], got.object_x, got.to_world_x,
              got.to_object_x, got.tmin, got.world_z, want.status, want.instance, want.id, want.geometry,
              want.primitive, want.front, want.contribution, want.flags, want.t, want.bary[0], want.bary[1],
              want.object_x, want.to_world_x, want.to_object_x, want.tmin, want.world_z
          );
      }
  };

  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC compacted{
      sizes->GetGPUVirtualAddress(), D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE
  }, current{sizes->GetGPUVirtualAddress() + sizeof(UINT64), D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_CURRENT_SIZE};
  scene.build(list.Get(), scene.triangle_inputs, scene.triangle_memory.Get(), nullptr, 1, &compacted);
  auto triangle_address = scene.triangle_memory->GetGPUVirtualAddress();
  list->EmitRaytracingAccelerationStructurePostbuildInfo(&current, 1, &triangle_address);
  scene.build(list.Get(), scene.box_inputs, scene.box_memory.Get());
  CHECK(trace(scene.triangle_memory.Get(), scene.box_memory.Get()));
  compare("built");
  CHECK(trace(scene.triangle_memory.Get(), scene.box_memory.Get(), true));
  compare("from a pixel shader");
  UINT64 compacted_size, current_size;
  memcpy(&compacted_size, out + sizes_at, sizeof(UINT64));
  memcpy(&current_size, out + sizes_at + sizeof(UINT64), sizeof(UINT64));
  if (!compacted_size || compacted_size > current_size || current_size != scene.triangle_size) {
    printf("failed: compacted size %llu, current size %llu, prebuild size %llu\n", compacted_size, current_size, scene.triangle_size);
    return 1;
  }

  // copies: the triangles compacted into memory of the compacted size, the boxes cloned
  auto compact_memory = scene.memory(device.Get(), compacted_size), clone_memory = scene.memory(device.Get(), scene.box_size);
  list->CopyRaytracingAccelerationStructure(
      compact_memory->GetGPUVirtualAddress(), scene.triangle_memory->GetGPUVirtualAddress(),
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT
  );
  list->CopyRaytracingAccelerationStructure(
      clone_memory->GetGPUVirtualAddress(), scene.box_memory->GetGPUVirtualAddress(),
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE
  );
  D3D12_RESOURCE_BARRIER barrier{D3D12_RESOURCE_BARRIER_TYPE_UAV};
  list->ResourceBarrier(1, &barrier);
  CHECK(trace(compact_memory.Get(), clone_memory.Get()));
  compare("copied");

  // an update in place: the first triangle moves back, past the second
  const float move = 1.5f;
  for (UINT k = 0; k < 3; k++)
    scene.inputs->vertices[k][2] = scene.vertices[k][2] += move;
  scene.triangles = scene.triangle_structure();
  scene.triangle_inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
  scene.build(list.Get(), scene.triangle_inputs, scene.triangle_memory.Get(), scene.triangle_memory.Get());
  CHECK(trace(scene.triangle_memory.Get(), scene.box_memory.Get()));
  compare("updated");

  // given back. a buffer's bytes, once the list has run
  auto bytes_of = [&](ID3D12Resource *source, UINT64 size, D3D12_RESOURCE_STATES state, std::vector<uint8_t> &bytes) {
    auto to = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, size, D3D12_RESOURCE_STATE_COPY_DEST);
    transition(list.Get(), source, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(to.Get(), 0, source, 0, size);
    transition(list.Get(), source, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    HRESULT hr = run();
    void *mapped;
    if (SUCCEEDED(hr) && SUCCEEDED(hr = to->Map(0, nullptr, &mapped)))
      bytes.assign((const uint8_t *)mapped, (const uint8_t *)mapped + size);
    return hr;
  };
  auto unordered = [&](UINT64 size) {
    return buffer(
        device.Get(), D3D12_HEAP_TYPE_DEFAULT, size, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
    );
  };
  using Header = D3D12_SERIALIZED_RAYTRACING_ACCELERATION_STRUCTURE_HEADER;
  using SerializationInfo = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_SERIALIZATION_DESC;
  const auto UA = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  unsigned given_wrong = 0;
  auto expect = [&](bool ok, const char *what, const char *of, double got, double want) {
    if (!ok && given_wrong++ < 12)
      printf("%s of %s: %g, want %g\n", what, of, got, want);
  };
  // the structures of the scene over the originals, with the sizes their prebuild information gave
  scene.build_scene(list.Get(), scene.triangle_memory.Get(), scene.box_memory.Get());
  enum { Triangles, Boxes, Scene, Structures };
  const char *const structure_names[Structures] = {"the triangle structure", "the box structure", "the scene"};
  ID3D12Resource *const originals[Structures] = {scene.triangle_memory.Get(), scene.box_memory.Get(), scene.scene_memory.Get()};
  const UINT64 original_sizes[Structures] = {scene.triangle_size, scene.box_size, scene.scene_size};
  D3D12_GPU_VIRTUAL_ADDRESS original_at[Structures];
  for (UINT i = 0; i < Structures; i++)
    original_at[i] = originals[i]->GetGPUVirtualAddress();
  // a serialization entry and a decoded size for each structure
  const UINT64 info_size = Structures * (sizeof(SerializationInfo) + sizeof(UINT64));
  auto info = unordered(info_size);
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC serialization{
      info->GetGPUVirtualAddress(), D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_SERIALIZATION
  }, visualization{info->GetGPUVirtualAddress() + Structures * sizeof(SerializationInfo),
                   D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_TOOLS_VISUALIZATION};
  list->EmitRaytracingAccelerationStructurePostbuildInfo(&serialization, Structures, original_at);
  list->EmitRaytracingAccelerationStructurePostbuildInfo(&visualization, Structures, original_at);
  std::vector<uint8_t> info_bytes;
  CHECK(bytes_of(info.Get(), info_size, UA, info_bytes));
  SerializationInfo serialized_info[Structures];
  UINT64 decoded_sizes[Structures];
  memcpy(serialized_info, info_bytes.data(), sizeof(serialized_info));
  memcpy(decoded_sizes, info_bytes.data() + sizeof(serialized_info), sizeof(decoded_sizes));

  // the input buffer's vertices change now, with no build: what is given back is what was built
  const float scribble = 7;
  for (auto &vertex : scene.inputs->vertices)
    vertex[2] += scribble;
  ComPtr<ID3D12Resource> serialized[Structures], decoded[Structures];
  std::vector<uint8_t> serialized_bytes[Structures], decoded_bytes[Structures];
  for (UINT i = 0; i < Structures; i++) {
    if (!serialized_info[i].SerializedSizeInBytes || !decoded_sizes[i]) {
      printf("failed: %s serializes to %llu bytes and decodes to %llu\n", structure_names[i],
             serialized_info[i].SerializedSizeInBytes, decoded_sizes[i]);
      return 1;
    }
    serialized[i] = unordered(serialized_info[i].SerializedSizeInBytes);
    decoded[i] = unordered(decoded_sizes[i]);
    list->CopyRaytracingAccelerationStructure(
        serialized[i]->GetGPUVirtualAddress(), original_at[i], D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_SERIALIZE
    );
    list->CopyRaytracingAccelerationStructure(
        decoded[i]->GetGPUVirtualAddress(), original_at[i],
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_VISUALIZATION_DECODE_FOR_TOOLS
    );
  }
  // the original triangles change in the same list, after the copies: an update takes the changed vertices
  scene.build(list.Get(), scene.triangle_inputs, scene.triangle_memory.Get(), scene.triangle_memory.Get());
  for (UINT i = 0; i < Structures; i++) {
    CHECK(bytes_of(serialized[i].Get(), serialized_info[i].SerializedSizeInBytes, UA, serialized_bytes[i]));
    CHECK(bytes_of(decoded[i].Get(), decoded_sizes[i], UA, decoded_bytes[i]));
  }
  Header headers[Structures];
  const UINT instance_count = scene.instances.size();
  for (UINT i = 0; i < Structures; i++) {
    auto name = structure_names[i];
    memcpy(&headers[i], serialized_bytes[i].data(), sizeof(Header));
    auto &header = headers[i];
    // the identifier is this device's; another GUID, version or kind of data is not
    auto changed = header.DriverMatchingIdentifier;
    const auto structure_data = D3D12_SERIALIZED_DATA_RAYTRACING_ACCELERATION_STRUCTURE;
    expect(device->CheckDriverMatchingIdentifier(structure_data, &changed) == D3D12_DRIVER_MATCHING_IDENTIFIER_COMPATIBLE_WITH_DEVICE,
           "the identifier's status", name, device->CheckDriverMatchingIdentifier(structure_data, &changed), 0);
    changed.DriverOpaqueVersioningData[0] ^= 0xff;
    expect(device->CheckDriverMatchingIdentifier(structure_data, &changed) == D3D12_DRIVER_MATCHING_IDENTIFIER_INCOMPATIBLE_VERSION,
           "another version's status", name, device->CheckDriverMatchingIdentifier(structure_data, &changed),
           D3D12_DRIVER_MATCHING_IDENTIFIER_INCOMPATIBLE_VERSION);
    changed.DriverOpaqueGUID.Data1 ^= 1;
    expect(device->CheckDriverMatchingIdentifier(structure_data, &changed) == D3D12_DRIVER_MATCHING_IDENTIFIER_UNRECOGNIZED,
           "another GUID's status", name, device->CheckDriverMatchingIdentifier(structure_data, &changed),
           D3D12_DRIVER_MATCHING_IDENTIFIER_UNRECOGNIZED);
    auto other_data = D3D12_SERIALIZED_DATA_TYPE(structure_data + 1);
    expect(device->CheckDriverMatchingIdentifier(other_data, &header.DriverMatchingIdentifier) == D3D12_DRIVER_MATCHING_IDENTIFIER_UNSUPPORTED_TYPE,
           "another kind of data's status", name, device->CheckDriverMatchingIdentifier(other_data, &header.DriverMatchingIdentifier),
           D3D12_DRIVER_MATCHING_IDENTIFIER_UNSUPPORTED_TYPE);
    expect(header.SerializedSizeInBytesIncludingHeader == serialized_info[i].SerializedSizeInBytes, "the header's size",
           name, header.SerializedSizeInBytesIncludingHeader, serialized_info[i].SerializedSizeInBytes);
    // "less than or equal to the size of the original acceleration structure before it was serialized"
    expect(header.DeserializedSizeInBytes && header.DeserializedSizeInBytes <= original_sizes[i],
           "the deserialized size", name, header.DeserializedSizeInBytes, original_sizes[i]);
    UINT64 pointers = i == Scene ? instance_count : 0;
    expect(header.NumBottomLevelAccelerationStructurePointersAfterHeader == pointers, "the header's pointers", name,
           header.NumBottomLevelAccelerationStructurePointersAfterHeader, pointers);
    expect(serialized_info[i].NumBottomLevelAccelerationStructurePointers == pointers, "the pointers", name,
           serialized_info[i].NumBottomLevelAccelerationStructurePointers, pointers);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_TOOLS_VISUALIZATION_HEADER tools;
    memcpy(&tools, decoded_bytes[i].data(), sizeof(tools));
    const auto &inputs = i == Triangles ? scene.triangle_inputs : i == Boxes ? scene.box_inputs : scene.scene_inputs;
    expect(tools.Type == inputs.Type, "the decoded type", name, tools.Type, inputs.Type);
    expect(tools.NumDescs == inputs.NumDescs, "the decoded count", name, tools.NumDescs, inputs.NumDescs);
  }
  // the scene's pointers are its instances' structures, and its decoded instances the ones it was built from
  for (UINT n = 0; n < instance_count; n++) {
    UINT64 pointer;
    memcpy(&pointer, serialized_bytes[Scene].data() + sizeof(Header) + n * sizeof(pointer), sizeof(pointer));
    expect(pointer == scene.inputs->instances[n].AccelerationStructure, "a pointer", structure_names[Scene], pointer,
           scene.inputs->instances[n].AccelerationStructure);
  }
  const auto tools_header = sizeof(D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_TOOLS_VISUALIZATION_HEADER);
  expect(decoded_sizes[Scene] >= tools_header + instance_count * sizeof(D3D12_RAYTRACING_INSTANCE_DESC) &&
             !memcmp(decoded_bytes[Scene].data() + tools_header, scene.inputs->instances,
                     instance_count * sizeof(D3D12_RAYTRACING_INSTANCE_DESC)),
         "the decoded instances", structure_names[Scene], 0, 1);
  // a decoded geometry is the one built, and its addresses are of the data that was built from, in the destination
  auto decoded_geometry = [&](UINT structure, UINT index, const D3D12_RAYTRACING_GEOMETRY_DESC &built) {
    auto name = structure_names[structure];
    auto &bytes = decoded_bytes[structure];
    D3D12_RAYTRACING_GEOMETRY_DESC got;
    memcpy(&got, bytes.data() + tools_header + index * sizeof(got), sizeof(got));
    auto data = [&](D3D12_GPU_VIRTUAL_ADDRESS at, const void *want, UINT64 size, UINT64 alignment, const char *what) {
      UINT64 offset = at - decoded[structure]->GetGPUVirtualAddress();
      expect(at % alignment == 0, "the alignment", what, at % alignment, 0);
      expect(at && offset + size <= bytes.size() && !memcmp(bytes.data() + offset, want, size), what, name, 0, 1);
    };
    expect(got.Type == built.Type && got.Flags == built.Flags, "a decoded geometry's type and flags", name, got.Type, built.Type);
    if (built.Type == D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS) {
      expect(got.AABBs.AABBCount == built.AABBs.AABBCount && got.AABBs.AABBs.StrideInBytes == built.AABBs.AABBs.StrideInBytes,
             "the decoded boxes' count and stride", name, got.AABBs.AABBCount, built.AABBs.AABBCount);
      data(got.AABBs.AABBs.StartAddress, scene.aabbs, sizeof(scene.aabbs), D3D12_RAYTRACING_AABB_BYTE_ALIGNMENT, "the decoded boxes");
      return;
    }
    const auto &g = got.Triangles, &b = built.Triangles;
    expect(g.IndexFormat == b.IndexFormat && g.VertexFormat == b.VertexFormat && g.IndexCount == b.IndexCount &&
               g.VertexCount == b.VertexCount && g.VertexBuffer.StrideInBytes == b.VertexBuffer.StrideInBytes,
           "the decoded triangles' formats, counts and stride", name, g.VertexCount, b.VertexCount);
    // the vertices as they were built, which the scene's own copy still is
    UINT first = (b.VertexBuffer.StartAddress - scene.triangle_geometries[0].Triangles.VertexBuffer.StartAddress) /
                 sizeof(scene.vertices[0]);
    data(g.VertexBuffer.StartAddress, scene.vertices[first], b.VertexCount * sizeof(scene.vertices[0]), sizeof(float),
         "the decoded vertices");
    expect(!g.Transform3x4 == !b.Transform3x4 && !g.IndexBuffer == !b.IndexBuffer,
           "which of a transform and indices is decoded", name, !g.Transform3x4, !b.Transform3x4);
    if (b.Transform3x4)
      data(g.Transform3x4, scene.quad_transform, sizeof(scene.quad_transform), D3D12_RAYTRACING_TRANSFORM3X4_BYTE_ALIGNMENT,
           "the decoded transform");
    if (b.IndexBuffer)
      data(g.IndexBuffer, scene.quad_indices, sizeof(scene.quad_indices), sizeof(scene.quad_indices[0]), "the decoded indices");
  };
  for (UINT g = 0; g < std::size(scene.triangle_geometries); g++)
    decoded_geometry(Triangles, g, scene.triangle_geometries[g]);
  decoded_geometry(Boxes, 0, scene.box_geometry);

  // deserialized, into new memory of the sizes the headers say. the bottom-level structures' bytes get to where they
  // are read by a copy in the same list; the scene's pointers name the structures where they are now
  ComPtr<ID3D12Resource> restored[Structures], brought[Structures];
  for (UINT i = 0; i < Structures; i++)
    restored[i] = scene.memory(device.Get(), headers[i].DeserializedSizeInBytes);
  // the serialized scene with its pointers naming the bottom-level structures in other memory
  auto naming = [&](ComPtr<ID3D12Resource> *structures) {
    auto scene_bytes = serialized_bytes[Scene];
    for (UINT n = 0; n < instance_count; n++) {
      UINT64 pointer;
      auto at = scene_bytes.data() + sizeof(Header) + n * sizeof(pointer);
      memcpy(&pointer, at, sizeof(pointer));
      for (UINT i = 0; i < Scene; i++)
        if (pointer == original_at[i])
          pointer = structures[i]->GetGPUVirtualAddress();
      memcpy(at, &pointer, sizeof(pointer));
    }
    auto named = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, scene_bytes.size(), D3D12_RESOURCE_STATE_GENERIC_READ);
    void *scene_mapped;
    if (named && SUCCEEDED(named->Map(0, nullptr, &scene_mapped)))
      memcpy(scene_mapped, scene_bytes.data(), scene_bytes.size());
    return named;
  };
  brought[Scene] = naming(restored);
  for (UINT i = 0; i < Scene; i++) {
    UINT64 size = serialized_info[i].SerializedSizeInBytes;
    brought[i] = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, size, D3D12_RESOURCE_STATE_COPY_DEST);
    transition(list.Get(), serialized[i].Get(), UA, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(brought[i].Get(), 0, serialized[i].Get(), 0, size);
    transition(list.Get(), brought[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  for (UINT i = 0; i < Structures; i++)
    list->CopyRaytracingAccelerationStructure(
        restored[i]->GetGPUVirtualAddress(), brought[i]->GetGPUVirtualAddress(),
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE
    );
  list->ResourceBarrier(1, &barrier);
  // the deserialized scene, as it is: its instances are of the deserialized structures, not of the originals,
  // which have changed
  CHECK(trace_at(restored[Scene]->GetGPUVirtualAddress(), false));
  compare("deserialized");

  // the scene first, in a list that has run before the structures its pointers name are deserialized
  ComPtr<ID3D12Resource> early[Structures];
  for (UINT i = 0; i < Structures; i++)
    early[i] = scene.memory(device.Get(), headers[i].DeserializedSizeInBytes);
  auto early_scene = naming(early);
  list->CopyRaytracingAccelerationStructure(
      early[Scene]->GetGPUVirtualAddress(), early_scene->GetGPUVirtualAddress(),
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE
  );
  CHECK(run());
  for (UINT i = 0; i < Scene; i++)
    list->CopyRaytracingAccelerationStructure(
        early[i]->GetGPUVirtualAddress(), brought[i]->GetGPUVirtualAddress(),
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE
    );
  list->ResourceBarrier(1, &barrier);
  CHECK(trace_at(early[Scene]->GetGPUVirtualAddress(), false));
  compare("deserialized, the scene before its structures");

  // lists recorded before the structures they use are there: one deserializes the bottom-level structures once
  // more, and three more, each starting with a command on what the first one makes, are recorded before any runs
  ComPtr<ID3D12Resource> second[Scene];
  enum { Deserializes, Serializes, Asks, Updates, Lists };
  ComPtr<ID3D12CommandAllocator> allocators[Lists];
  ComPtr<ID3D12GraphicsCommandList4> lists[Lists];
  for (UINT i = 0; i < Lists; i++) {
    CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[i])));
    CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[i].Get(), nullptr, IID_PPV_ARGS(&lists[i])));
  }
  for (UINT i = 0; i < Scene; i++) {
    second[i] = scene.memory(device.Get(), headers[i].DeserializedSizeInBytes);
    lists[Deserializes]->CopyRaytracingAccelerationStructure(
        second[i]->GetGPUVirtualAddress(), brought[i]->GetGPUVirtualAddress(),
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE
    );
  }
  // a deserialized structure serializes to what it came from
  auto again = unordered(serialized_info[Triangles].SerializedSizeInBytes);
  lists[Serializes]->CopyRaytracingAccelerationStructure(
      again->GetGPUVirtualAddress(), second[Triangles]->GetGPUVirtualAddress(),
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_SERIALIZE
  );
  // the serialized sizes of the boxes' clone, made long before, and of the deserialized boxes
  const D3D12_GPU_VIRTUAL_ADDRESS copies[] = {clone_memory->GetGPUVirtualAddress(), second[Boxes]->GetGPUVirtualAddress()};
  lists[Asks]->EmitRaytracingAccelerationStructurePostbuildInfo(&serialization, std::size(copies), copies);
  // the deserialized triangles are updated from vertices of their own, the first triangle moved once more
  for (UINT k = 0; k < 3; k++)
    scene.vertices[k][2] += move;
  scene.triangles = scene.triangle_structure();
  auto moved = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(scene.vertices), D3D12_RESOURCE_STATE_GENERIC_READ);
  void *moved_mapped;
  CHECK(moved->Map(0, nullptr, &moved_mapped));
  memcpy(moved_mapped, scene.vertices, sizeof(scene.vertices));
  D3D12_RAYTRACING_GEOMETRY_DESC moved_geometries[std::size(scene.triangle_geometries)];
  auto moved_inputs = scene.triangle_inputs;
  for (UINT g = 0; g < std::size(moved_geometries); g++) {
    moved_geometries[g] = scene.triangle_geometries[g];
    moved_geometries[g].Triangles.VertexBuffer.StartAddress +=
        moved->GetGPUVirtualAddress() - scene.triangle_geometries[0].Triangles.VertexBuffer.StartAddress;
  }
  moved_inputs.pGeometryDescs = moved_geometries;
  scene.build(lists[Updates].Get(), moved_inputs, second[Triangles].Get(), second[Triangles].Get());
  ID3D12CommandList *together[Lists];
  for (UINT i = 0; i < Lists; i++) {
    CHECK(lists[i]->Close());
    together[i] = lists[i].Get();
  }
  queue->ExecuteCommandLists(Lists, together);
  // the scene over them, traced once they are there
  CHECK(trace(second[Triangles].Get(), second[Boxes].Get()));
  compare("deserialized, then updated by lists recorded before");
  std::vector<uint8_t> again_bytes;
  CHECK(bytes_of(again.Get(), serialized_info[Triangles].SerializedSizeInBytes, UA, again_bytes));
  expect(again_bytes == serialized_bytes[Triangles], "the bytes a deserialized structure serializes to",
         structure_names[Triangles], 0, 1);
  CHECK(bytes_of(info.Get(), std::size(copies) * sizeof(SerializationInfo), UA, info_bytes));
  for (UINT copy = 0; copy < std::size(copies); copy++)
    expect(!memcmp(info_bytes.data() + copy * sizeof(SerializationInfo), &serialized_info[Boxes], sizeof(SerializationInfo)),
           copy ? "the serialized size of a deserialized structure" : "the serialized size of a clone",
           structure_names[Boxes], 0, 1);
  // a copy gives what its source holds when the copy runs, whenever it was recorded. one list serializes the
  // deserialized triangles and one updates them, from vertices moved further each time: first the serializing list is
  // recorded first and run last, and gives the update; then it is recorded last and run first, and gives what was
  // there before. `now` is what a copy recorded and run in between gives
  ComPtr<ID3D12Resource> ordered[3], further[2];
  std::vector<uint8_t> ordered_bytes[3];
  for (auto &bytes : ordered)
    bytes = unordered(serialized_info[Triangles].SerializedSizeInBytes);
  auto serializes = [&](ID3D12GraphicsCommandList4 *to, ID3D12Resource *bytes) {
    to->CopyRaytracingAccelerationStructure(
        bytes->GetGPUVirtualAddress(), second[Triangles]->GetGPUVirtualAddress(),
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_SERIALIZE
    );
  };
  enum { RecordedFirst, Now, RecordedLast };
  float further_vertices[std::size(scene.vertices)][3];
  memcpy(further_vertices, scene.vertices, sizeof(further_vertices));
  for (UINT round = 0; round < 2; round++) {
    for (UINT k = 0; k < 3; k++)
      further_vertices[k][2] += move;
    further[round] = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(further_vertices), D3D12_RESOURCE_STATE_GENERIC_READ);
    void *further_mapped;
    CHECK(further[round]->Map(0, nullptr, &further_mapped));
    memcpy(further_mapped, further_vertices, sizeof(further_vertices));
    for (UINT g = 0; g < std::size(moved_geometries); g++) {
      moved_geometries[g] = scene.triangle_geometries[g];
      moved_geometries[g].Triangles.VertexBuffer.StartAddress +=
          further[round]->GetGPUVirtualAddress() - scene.triangle_geometries[0].Triangles.VertexBuffer.StartAddress;
    }
    for (UINT i : {(UINT)Serializes, (UINT)Updates}) {
      CHECK(allocators[i]->Reset());
      CHECK(lists[i]->Reset(allocators[i].Get(), nullptr));
    }
    bool serializes_first = round == 0;
    if (serializes_first)
      serializes(lists[Serializes].Get(), ordered[RecordedFirst].Get());
    scene.build(lists[Updates].Get(), moved_inputs, second[Triangles].Get(), second[Triangles].Get());
    if (!serializes_first)
      serializes(lists[Serializes].Get(), ordered[RecordedLast].Get());
    CHECK(lists[Serializes]->Close());
    CHECK(lists[Updates]->Close());
    // run in the order they were not recorded in
    ID3D12CommandList *pair[2] = {lists[serializes_first ? Updates : Serializes].Get(),
                                  lists[serializes_first ? Serializes : Updates].Get()};
    queue->ExecuteCommandLists(2, pair);
    if (serializes_first)
      serializes(list.Get(), ordered[Now].Get());
    CHECK(run());
  }
  for (UINT i = 0; i < std::size(ordered); i++)
    CHECK(bytes_of(ordered[i].Get(), serialized_info[Triangles].SerializedSizeInBytes, UA, ordered_bytes[i]));
  expect(ordered_bytes[Now] != again_bytes, "the bytes an updated structure serializes to, as before the update",
         structure_names[Triangles], 1, 0);
  expect(ordered_bytes[RecordedFirst] == ordered_bytes[Now],
         "the bytes of a copy recorded before an update and run after it, as of the update", structure_names[Triangles], 0, 1);
  expect(ordered_bytes[RecordedLast] == ordered_bytes[Now],
         "the bytes of a copy recorded after an update and run before it, as of before the update",
         structure_names[Triangles], 0, 1);

  // a list's commands keep their order: memory that a copy deserializes the triangles into and a build then makes
  // the boxes in holds the boxes. a copy of a structure that is nowhere does nothing
  auto reused = scene.memory(device.Get(), std::max(headers[Triangles].DeserializedSizeInBytes, scene.box_size));
  auto reused_at = reused->GetGPUVirtualAddress();
  list->CopyRaytracingAccelerationStructure(
      reused_at, brought[Triangles]->GetGPUVirtualAddress(), D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE
  );
  scene.build(list.Get(), scene.box_inputs, reused.Get());
  list->CopyRaytracingAccelerationStructure(
      again->GetGPUVirtualAddress(), info->GetGPUVirtualAddress(), D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE
  );
  CHECK(run());
  list->EmitRaytracingAccelerationStructurePostbuildInfo(&serialization, 1, &reused_at);
  CHECK(bytes_of(info.Get(), sizeof(SerializationInfo), UA, info_bytes));
  expect(!memcmp(info_bytes.data(), &serialized_info[Boxes], sizeof(SerializationInfo)),
         "the serialized size of a structure built where another was deserialized", structure_names[Boxes], 0, 1);
  // post-build information is of what the structure holds when its list runs: asked in a list recorded while the
  // memory held the boxes, and run after a list that builds the triangles there, it is the triangles'
  CHECK(allocators[Asks]->Reset());
  CHECK(lists[Asks]->Reset(allocators[Asks].Get(), nullptr));
  lists[Asks]->EmitRaytracingAccelerationStructurePostbuildInfo(&serialization, 1, &reused_at);
  CHECK(lists[Asks]->Close());
  auto rebuilt_inputs = scene.triangle_inputs;
  rebuilt_inputs.Flags &= ~D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
  scene.build(list.Get(), rebuilt_inputs, reused.Get());
  CHECK(run());
  ID3D12CommandList *asks = lists[Asks].Get();
  queue->ExecuteCommandLists(1, &asks);
  CHECK(bytes_of(info.Get(), sizeof(SerializationInfo), UA, info_bytes));
  expect(!memcmp(info_bytes.data(), &serialized_info[Triangles], sizeof(SerializationInfo)),
         "the serialized size asked before a build that runs first", structure_names[Triangles], 0, 1);
  if (given_wrong) {
    printf("failed: %u wrong answers about serialized and decoded structures\n", given_wrong);
    return 1;
  }

  // strides. three structures, each in its own corner of the rays' grid
  {
    auto count_cs = compiler.compile(count_hlsl, "cs", "cs_6_5", defines);
    ComPtr<ID3D12PipelineState> count_pso;
    D3D12_COMPUTE_PIPELINE_STATE_DESC count_desc{rs.Get(), bytecode(count_cs)};
    CHECK(device->CreateComputePipelineState(&count_desc, IID_PPV_ARGS(&count_pso)));
    // a triangle of packed half floats, the last one ending where the buffer does, with stride bits above the low
    // 32; one box three times, the memory after it other boxes far from every ray, which a stride would reach; and
    // one vertex three times, the memory after it the rest of a triangle
    const float corner = 0.5f, wide_corner = 3.625f, apart = 5, away = 1000;
    const D3D12_RAYTRACING_AABB box{1, 1, -1, 3, 3, 1};
    const UINT boxes_repeated = 3, triangle_id = 20;
    const UINT64 input_size = 4096, packed_stride = 3 * sizeof(uint16_t), packed_at = input_size - 3 * packed_stride;
    struct {
      D3D12_RAYTRACING_AABB boxes[3];
      float vertices[3][3];
      D3D12_RAYTRACING_INSTANCE_DESC instances[3];
    } placed{{box, {away, away, away, away + 1, away + 1, away + 1}, {away, away, away, away + 1, away + 1, away + 1}},
             {{corner, corner, 0}, {corner, wide_corner, 0}, {wide_corner, corner, 0}}};
    const uint16_t packed[3][3] = {{half(corner), half(corner), 0}, {half(corner), half(wide_corner), 0},
                                   {half(wide_corner), half(corner), 0}};
    auto stride_input = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, input_size, D3D12_RESOURCE_STATE_GENERIC_READ);
    char *stride_mapped;
    CHECK(stride_input->Map(0, nullptr, (void **)&stride_mapped));
    auto input_at = stride_input->GetGPUVirtualAddress();
    D3D12_RAYTRACING_GEOMETRY_DESC geometries[3]{};
    for (auto &geometry : geometries)
      geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geometries[0].Triangles = {0, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 3, 0,
                               {input_at + packed_at, packed_stride | 1ull << 32}};
    geometries[1].Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
    geometries[1].AABBs = {boxes_repeated, {input_at + offsetof(decltype(placed), boxes), 0}};
    geometries[2].Triangles = {0, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R32G32B32_FLOAT, 0, 3, 0,
                               {input_at + offsetof(decltype(placed), vertices), 0}};
    ComPtr<ID3D12Resource> structures[3];
    for (UINT i = 0; i < 3; i++) {
      D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{
          D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE, 1,
          D3D12_ELEMENTS_LAYOUT_ARRAY
      };
      in.pGeometryDescs = &geometries[i];
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
      device->GetRaytracingAccelerationStructurePrebuildInfo(&in, &prebuild);
      if (!prebuild.ResultDataMaxSizeInBytes || prebuild.ScratchDataSizeInBytes > scene.scratch_size) {
        printf("failed: a structure of strided geometry %u needs %llu bytes and %llu of scratch\n", i,
               prebuild.ResultDataMaxSizeInBytes, prebuild.ScratchDataSizeInBytes);
        return 1;
      }
      structures[i] = scene.memory(device.Get(), prebuild.ResultDataMaxSizeInBytes);
      auto &instance = placed.instances[i];
      for (UINT k = 0; k < 3; k++)
        instance.Transform[k][k] = 1;
      instance.Transform[0][3] = i == 1 ? apart : 0;
      instance.Transform[1][3] = i == 2 ? apart : 0;
      instance.InstanceID = triangle_id + i;
      instance.InstanceMask = 0xff;
      instance.AccelerationStructure = structures[i]->GetGPUVirtualAddress();
    }
    memcpy(stride_mapped, &placed, sizeof(placed));
    memcpy(stride_mapped + packed_at, packed, sizeof(packed));
    for (UINT i = 0; i < 3; i++) {
      D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{
          D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE, 1,
          D3D12_ELEMENTS_LAYOUT_ARRAY
      };
      in.pGeometryDescs = &geometries[i];
      scene.build(list.Get(), in, structures[i].Get());
    }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS top{
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE, 3,
        D3D12_ELEMENTS_LAYOUT_ARRAY
    };
    top.InstanceDescs = input_at + offsetof(decltype(placed), instances);
    scene.build(list.Get(), top, scene.scene_memory.Get());
    list->SetPipelineState(count_pso.Get());
    list->SetComputeRootSignature(rs.Get());
    list->SetComputeRootShaderResourceView(0, scene.scene_memory->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(3, results->GetGPUVirtualAddress());
    list->Dispatch(size, size, 1);
    transition(list.Get(), results.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), 0, results.Get(), 0, rays * 4 * sizeof(UINT));
    transition(list.Get(), results.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    CHECK(run());
    unsigned stride_failures = 0, in_boxes = 0, on_triangle = 0;
    for (UINT i = 0; i < rays; i++) {
      UINT got[4];
      memcpy(got, out + i * sizeof(got), sizeof(got));
      float x = scene.ray_x(i), y = scene.ray_y(i);
      // the packed triangle where it is; every repeat of the box through it; nothing of the one-point triangle
      bool triangle = x > corner && y > corner && x + y < corner + wide_corner;
      bool boxed = x - apart > box.MinX && x - apart < box.MaxX && y > box.MinY && y < box.MaxY;
      margin = std::min({margin, std::abs(x - corner), std::abs(y - corner), std::abs(x + y - corner - wide_corner),
                         std::abs(x - apart - box.MinX), std::abs(x - apart - box.MaxX), std::abs(y - box.MinY),
                         std::abs(y - box.MaxY)});
      UINT want[4] = {boxed ? (1u << boxes_repeated) - 1 : 0, got[1], triangle, triangle ? triangle_id : 0};
      on_triangle += triangle;
      in_boxes += boxed;
      // a box may be offered more than once, never less
      if ((memcmp(got, want, sizeof(got)) || (boxed && got[1] < boxes_repeated)) && stride_failures++ < 8)
        printf("strides, ray %u,%u: boxes %#x offered %u times, a triangle hit %u of instance %u; want %#x, %u of %u\n",
               i % size, i / size, got[0], got[1], got[2], got[3], want[0], want[2], want[3]);
    }
    // each structure has rays through it
    if (stride_failures || !in_boxes || !on_triangle) {
      printf("failed: %u wrong rays over strided geometry, %u in the boxes, %u on the triangle\n", stride_failures,
             in_boxes, on_triangle);
      return 1;
    }
    traced += rays;
    hits += on_triangle;
  }

  // no structure, and a structure of nothing
  for (bool pixel : {false, true}) {
    CHECK(trace_at(0, pixel));
    compare(pixel ? "a null structure, from a pixel shader" : "a null structure", true);
  }
  scene.scene_inputs.NumDescs = 0;
  scene.build(list.Get(), scene.scene_inputs, scene.scene_memory.Get());
  CHECK(trace_at(scene.scene_memory->GetGPUVirtualAddress(), false));
  compare("a structure of no instances", true);
  // which serializes to its headers alone, and deserialized is nothing again
  auto empty_at = scene.scene_memory->GetGPUVirtualAddress();
  list->EmitRaytracingAccelerationStructurePostbuildInfo(&serialization, 1, &empty_at);
  CHECK(bytes_of(info.Get(), sizeof(SerializationInfo), UA, info_bytes));
  SerializationInfo empty_info;
  memcpy(&empty_info, info_bytes.data(), sizeof(empty_info));
  if (!empty_info.SerializedSizeInBytes || empty_info.NumBottomLevelAccelerationStructurePointers) {
    printf("failed: a scene of no instances serializes to %llu bytes with %llu pointers\n",
           empty_info.SerializedSizeInBytes, empty_info.NumBottomLevelAccelerationStructurePointers);
    return 1;
  }
  auto empty_serialized = unordered(empty_info.SerializedSizeInBytes), empty_restored = scene.memory(device.Get(), scene.scene_size);
  list->CopyRaytracingAccelerationStructure(
      empty_serialized->GetGPUVirtualAddress(), empty_at, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_SERIALIZE
  );
  transition(list.Get(), empty_serialized.Get(), UA, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  list->CopyRaytracingAccelerationStructure(
      empty_restored->GetGPUVirtualAddress(), empty_serialized->GetGPUVirtualAddress(),
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE
  );
  list->ResourceBarrier(1, &barrier);
  CHECK(trace_at(empty_restored->GetGPUVirtualAddress(), false));
  compare("a structure of no instances, deserialized", true);

  // the expectations hold only while no ray passes where rounding decides what it hits
  if (!scene.clear(margin)) {
    printf("failed: a ray is %g from an edge\n", margin);
    return 1;
  }
  if (failures) {
    printf("failed: %u wrong rays\n", failures);
    return 1;
  }
  printf("passed: %u hits of %u rays\n", hits, traced);
  return 0;
}
