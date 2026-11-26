// contract: NVAPI's ray tracing entry points (nvapi.h,
// NvAPI_D3D12_GetRaytracingCaps,
// NvAPI_D3D12_GetRaytracingAccelerationStructurePrebuildInfoEx and
// NvAPI_D3D12_BuildRaytracingAccelerationStructureEx).
// - the caps of each type are a value of its enum, and none of NVIDIA's
// extensions is there: each is its CAP_NONE.
//   "dataSize ... Must match the size of the caps data requested", an unknown
//   type is INVALID_ARGUMENT and a null pointer INVALID_POINTER.
// - the extended prebuild information and build are "an extension of"
// ID3D12Device5's and ID3D12GraphicsCommandList4's
//   with "additional input types": of the inputs Direct3D has, triangles and
//   boxes in an array of NVAPI's geometry descriptions a stride apart or by
//   pointers, and a top-level structure, the sizes are Direct3D's, and
//   structures built through it trace as the scene they hold says, which a
//   compute shader's ray queries and a CPU ray caster compare (d3d12_rays.hpp).
//   its post-build information is the one Direct3D's build writes: a compacted
//   size, equal to that of the same structure built through Direct3D.
// - what Mullion does not have is refused, as its caps say: a geometry of
// opacity or displacement micromaps, spheres
//   or linear swept spheres and a build flag of micromaps or data access are
//   NOT_SUPPORTED. post-build descriptions counted and not given are
//   INVALID_COMBINATION, and a structure of another version
//   INCOMPATIBLE_STRUCT_VERSION.
// NVAPI is there with NVEXT on (MULLION_ENABLE_NVEXT=1, which the test's
// environment sets).
#include "../d3d12/d3d12_rays.hpp"
#define __NVAPI_EMPTY_SAL
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-attributes"
#include "nvapi.h"
#pragma clang diagnostic pop
#include "nvapi_interface.h"
#undef __NVAPI_EMPTY_SAL

static const char hlsl[] = R"hlsl(
RaytracingAccelerationStructure scene : register(t0);
RWStructuredBuffer<uint4> results : register(u0);
// the boxes: x and y bounds and the near face's z
static const float boxes[BOXES][5] = { BOX_LIST };
[numthreads(8, 8, 1)]
void cs(uint2 id : SV_DispatchThreadID) {
  if (id.x >= SIZE || id.y >= SIZE)
    return;
  RayDesc ray;
  ray.Origin = float3(id.x * STEP + X0, id.y * STEP + Y0, Z0);
  ray.Direction = float3(0, 0, 1);
  ray.TMin = 0;
  ray.TMax = 100;
  RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
  q.TraceRayInline(scene, RAY_FLAG_NONE, 0xff, ray);
  // every triangle is opaque: a candidate is a box, hit at its near face
  while (q.Proceed()) {
    float3 o = q.CandidateObjectRayOrigin();
    float b[5] = boxes[q.CandidatePrimitiveIndex()];
    float t = (b[4] - o.z) / q.CandidateObjectRayDirection().z;
    float limit = q.CommittedStatus() == COMMITTED_NOTHING ? ray.TMax : q.CommittedRayT();
    if (o.x > b[0] && o.x < b[1] && o.y > b[2] && o.y < b[3] && t >= ray.TMin && t <= limit)
      q.CommitProceduralPrimitiveHit(t);
  }
  uint4 r = uint4(q.CommittedStatus(), 0, 0, 0);
  if (r.x != COMMITTED_NOTHING)
    r.yzw = uint3(q.CommittedInstanceIndex(), q.CommittedGeometryIndex(), q.CommittedPrimitiveIndex());
  results[id.y * SIZE + id.x] = r;
}
)hlsl";

// a function by its name in NVIDIA's table
template <typename F> static F function(FARPROC query, const char *name) {
  for (auto &entry : nvapi_interface_table)
    if (!strcmp(entry.func, name))
      return (F)((void *(__cdecl *)(NvU32))query)(entry.id);
  return nullptr;
}

// Direct3D's geometry descriptions as NVAPI's
static std::vector<NVAPI_D3D12_RAYTRACING_GEOMETRY_DESC_EX>
extended(const D3D12_RAYTRACING_GEOMETRY_DESC *geometries, UINT count) {
  std::vector<NVAPI_D3D12_RAYTRACING_GEOMETRY_DESC_EX> out(count);
  for (UINT i = 0; i < count; i++) {
    out[i] = {};
    out[i].flags = geometries[i].Flags;
    if (geometries[i].Type == D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES) {
      out[i].type = NVAPI_D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES_EX;
      out[i].triangles = geometries[i].Triangles;
    } else {
      out[i].type =
          NVAPI_D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS_EX;
      out[i].aabbs = geometries[i].AABBs;
    }
  }
  return out;
}

static NVAPI_D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS_EX
extended(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS &in,
         const NVAPI_D3D12_RAYTRACING_GEOMETRY_DESC_EX *geometries) {
  NVAPI_D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS_EX out{};
  out.type = in.Type;
  out.flags =
      NVAPI_D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS_EX(in.Flags);
  out.numDescs = in.NumDescs;
  out.descsLayout = in.DescsLayout;
  out.geometryDescStrideInBytes =
      sizeof(NVAPI_D3D12_RAYTRACING_GEOMETRY_DESC_EX);
  if (in.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL)
    out.instanceDescs = in.InstanceDescs;
  else
    out.pGeometryDescs = geometries;
  return out;
}

int main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler) || !compiler.dxc) {
    printf("skipped: RayQuery needs DXC\n");
    return 77;
  }
  step("NVAPI's ray tracing functions");
  auto nvapi = LoadLibraryA("nvapi64.dll");
  auto query = nvapi ? GetProcAddress(nvapi, "nvapi_QueryInterface") : nullptr;
  if (!expect(query, "nvapi64.dll does not load: %lu", GetLastError()))
    return verdict();
  auto initialize =
      function<NvAPI_Status(__cdecl *)()>(query, "NvAPI_Initialize");
  auto caps = function<NvAPI_Status(__cdecl *)(
      ID3D12Device *, NVAPI_D3D12_RAYTRACING_CAPS_TYPE, void *, size_t)>(
      query, "NvAPI_D3D12_GetRaytracingCaps");
  auto prebuild = function<NvAPI_Status(__cdecl *)(
      ID3D12Device5 *,
      NVAPI_GET_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO_EX_PARAMS *)>(
      query, "NvAPI_D3D12_GetRaytracingAccelerationStructurePrebuildInfoEx");
  auto build = function<NvAPI_Status(__cdecl *)(
      ID3D12GraphicsCommandList4 *,
      const NVAPI_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_EX_PARAMS *)>(
      query, "NvAPI_D3D12_BuildRaytracingAccelerationStructureEx");
  expect(caps, "no NvAPI_D3D12_GetRaytracingCaps");
  expect(prebuild,
         "no NvAPI_D3D12_GetRaytracingAccelerationStructurePrebuildInfoEx");
  expect(build, "no NvAPI_D3D12_BuildRaytracingAccelerationStructureEx");
  if (!initialize || !caps || !prebuild || !build ||
      !expect(initialize() == NVAPI_OK, "NvAPI_Initialize"))
    return verdict();

  ComPtr<ID3D12Device5> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                          IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5,
                                    sizeof(options5)));
  if (options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_1) {
    printf("skipped: the device has no ray tracing tier 1.1, which ray queries "
           "need\n");
    return 77;
  }

  step("NvAPI_D3D12_GetRaytracingCaps");
  for (int type = NVAPI_D3D12_RAYTRACING_CAPS_TYPE_THREAD_REORDERING;
       type <= NVAPI_D3D12_RAYTRACING_CAPS_TYPE_LINEAR_SWEPT_SPHERES; type++) {
    NvU32 value = ~0u;
    auto status = caps(device.Get(), NVAPI_D3D12_RAYTRACING_CAPS_TYPE(type),
                       &value, sizeof(value));
    expect(status == NVAPI_OK && value == 0, "caps type %d: status %d, %#x",
           type, status, value);
    expect(caps(device.Get(), NVAPI_D3D12_RAYTRACING_CAPS_TYPE(type), &value,
                2 * sizeof(value)) == NVAPI_INVALID_ARGUMENT,
           "caps type %d of a size not its own", type);
    expect(caps(device.Get(), NVAPI_D3D12_RAYTRACING_CAPS_TYPE(type), nullptr,
                sizeof(value)) == NVAPI_INVALID_POINTER,
           "caps type %d into no memory", type);
  }
  NvU32 value;
  expect(caps(device.Get(), NVAPI_D3D12_RAYTRACING_CAPS_TYPE(7), &value,
              sizeof(value)) == NVAPI_INVALID_ARGUMENT &&
             caps(device.Get(), NVAPI_D3D12_RAYTRACING_CAPS_TYPE_INVALID,
                  &value, sizeof(value)) == NVAPI_INVALID_ARGUMENT,
         "an unknown caps type");
  expect(caps(nullptr, NVAPI_D3D12_RAYTRACING_CAPS_TYPE_OPACITY_MICROMAP,
              &value, sizeof(value)) == NVAPI_INVALID_POINTER,
         "caps of no device");

  RayScene scene;
  const UINT size = scene.size, rays = size * size;
  std::string box_list;
  for (auto &b : scene.aabbs)
    box_list += (box_list.empty() ? "{" : ", {") + std::to_string(b.MinX) +
                ", " + std::to_string(b.MaxX) + ", " + std::to_string(b.MinY) +
                ", " + std::to_string(b.MaxY) + ", " + std::to_string(b.MinZ) +
                "}";
  auto cs = compiler.compile(
      hlsl, "cs", "cs_6_5",
      {"SIZE=" + std::to_string(size), "STEP=" + std::to_string(scene.step),
       "X0=" + std::to_string(scene.x0), "Y0=" + std::to_string(scene.y0),
       "Z0=" + std::to_string(scene.z0),
       "BOXES=" + std::to_string(std::size(scene.aabbs)),
       "BOX_LIST=" + box_list});
  if (!expect(!cs.empty(), "the HLSL does not compile"))
    return verdict();
  D3D12_ROOT_PARAMETER params[2]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  auto rs = root_signature(device.Get(), {2, params});
  if (!expect(rs, "the root signature"))
    return verdict();
  ComPtr<ID3D12PipelineState> pso;
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute{rs.Get(), bytecode(cs)};
  CHECK(device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&pso)));
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList4> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                       IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                  allocator.Get(), nullptr,
                                  IID_PPV_ARGS(&list)));
  CHECK(scene.create(device.Get()));

  // the extended inputs: the triangles in an array of NVAPI's descriptions, the
  // boxes by pointer
  auto triangles = extended(scene.triangle_geometries, 2),
       boxes = extended(&scene.box_geometry, 1);
  const NVAPI_D3D12_RAYTRACING_GEOMETRY_DESC_EX *box_pointers[] = {&boxes[0]};
  auto triangle_inputs = extended(scene.triangle_inputs, triangles.data()),
       box_inputs = extended(scene.box_inputs, nullptr),
       scene_inputs = extended(scene.scene_inputs, nullptr);
  box_inputs.descsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS;
  box_inputs.ppGeometryDescs = box_pointers;

  step("NvAPI_D3D12_GetRaytracingAccelerationStructurePrebuildInfoEx");
  const std::pair<
      const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS *,
      const NVAPI_D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS_EX *>
      compared[] = {{&scene.triangle_inputs, &triangle_inputs},
                    {&scene.box_inputs, &box_inputs},
                    {&scene.scene_inputs, &scene_inputs}};
  for (auto [standard, ex] : compared) {
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO wanted{}, got{};
    device->GetRaytracingAccelerationStructurePrebuildInfo(standard, &wanted);
    NVAPI_GET_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO_EX_PARAMS info{
        NVAPI_GET_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO_EX_PARAMS_VER,
        ex, &got};
    auto status = prebuild(device.Get(), &info);
    expect(status == NVAPI_OK &&
               got.ResultDataMaxSizeInBytes ==
                   wanted.ResultDataMaxSizeInBytes &&
               got.ScratchDataSizeInBytes == wanted.ScratchDataSizeInBytes &&
               got.UpdateScratchDataSizeInBytes ==
                   wanted.UpdateScratchDataSizeInBytes,
           "the prebuild information of structure type %u: status %d, %llu "
           "%llu %llu where Direct3D's is %llu %llu %llu",
           standard->Type, status, got.ResultDataMaxSizeInBytes,
           got.ScratchDataSizeInBytes, got.UpdateScratchDataSizeInBytes,
           wanted.ResultDataMaxSizeInBytes, wanted.ScratchDataSizeInBytes,
           wanted.UpdateScratchDataSizeInBytes);
  }

  // the triangle structure built through Direct3D, beside the one built through
  // NVAPI, and their compacted sizes
  auto standard = RayScene::memory(device.Get(), scene.triangle_size),
       extended_scene = RayScene::memory(device.Get(), scene.scene_size);
  auto sizes = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, 2 * sizeof(UINT64),
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto results = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, rays * 16,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback =
      buffer(device.Get(), D3D12_HEAP_TYPE_READBACK,
             rays * 16 + 2 * sizeof(UINT64), D3D12_RESOURCE_STATE_COPY_DEST);
  CHECK(forget(readback.Get()));
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC compacted[] = {
      {sizes->GetGPUVirtualAddress(),
       D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE},
      {sizes->GetGPUVirtualAddress() + sizeof(UINT64),
       D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE},
  };
  D3D12_RESOURCE_BARRIER uav{D3D12_RESOURCE_BARRIER_TYPE_UAV};
  auto build_ex =
      [&](const NVAPI_D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS_EX
              &in,
          ID3D12Resource *to, UINT infos = 0,
          const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC
              *info = nullptr) {
        NVAPI_D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC_EX desc{
            to->GetGPUVirtualAddress(), in, 0,
            scene.scratch->GetGPUVirtualAddress()};
        NVAPI_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_EX_PARAMS params{
            NVAPI_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_EX_PARAMS_VER, &desc,
            infos, info};
        auto status = build(list.Get(), &params);
        list->ResourceBarrier(1, &uav);
        return status;
      };

  step("NvAPI_D3D12_BuildRaytracingAccelerationStructureEx");
  scene.build(list.Get(), scene.triangle_inputs, standard.Get(), nullptr, 1,
              &compacted[1]);
  auto built =
      build_ex(triangle_inputs, scene.triangle_memory.Get(), 1, &compacted[0]);
  expect(built == NVAPI_OK, "the triangle structure: %d", built);
  expect((built = build_ex(box_inputs, scene.box_memory.Get())) == NVAPI_OK,
         "the box structure: %d", built);
  // the instances of the scene over the structures NVAPI built, which
  // build_scene writes and builds through Direct3D: the scene NVAPI builds is
  // apart from it
  scene.build_scene(list.Get(), scene.triangle_memory.Get(),
                    scene.box_memory.Get());
  expect((built = build_ex(scene_inputs, extended_scene.Get())) == NVAPI_OK,
         "the scene: %d", built);
  list->SetComputeRootSignature(rs.Get());
  list->SetPipelineState(pso.Get());
  list->SetComputeRootShaderResourceView(
      0, extended_scene->GetGPUVirtualAddress());
  list->SetComputeRootUnorderedAccessView(1, results->GetGPUVirtualAddress());
  list->Dispatch((size + 7) / 8, (size + 7) / 8, 1);
  transition(list.Get(), results.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
             D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(list.Get(), sizes.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
             D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(), 0, results.Get(), 0, rays * 16);
  list->CopyBufferRegion(readback.Get(), rays * 16, sizes.Get(), 0,
                         2 * sizeof(UINT64));
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  const uint8_t *out;
  CHECK(readback->Map(0, nullptr, (void **)&out));
  UINT64 compacted_sizes[2];
  memcpy(compacted_sizes, out + rays * 16, sizeof(compacted_sizes));
  expect(compacted_sizes[0] && compacted_sizes[0] <= scene.triangle_size &&
             compacted_sizes[0] == compacted_sizes[1],
         "the compacted size NVAPI's build gives is %llu, Direct3D's %llu",
         compacted_sizes[0], compacted_sizes[1]);
  Decisions decide;
  decide.box = [](UINT, const Box &, bool) { return true; };
  UINT hits = 0, checked = 0;
  for (UINT i = 0; i < rays; i++) {
    float margin = FLT_MAX;
    auto wanted =
        cast(scene.instances, RAY_FORCE_OPAQUE, 0xff, 0, 100, scene.ray_x(i),
             scene.ray_y(i), scene.z0, decide, margin);
    if (!RayScene::clear(margin))
      continue;
    checked++;
    hits += wanted.status != 0;
    UINT got[4];
    memcpy(got, out + i * 16, sizeof(got));
    bool same = got[0] == wanted.status &&
                (!wanted.status ||
                 (got[1] == wanted.instance && got[2] == wanted.geometry &&
                  got[3] == wanted.primitive));
    if (!expect(same,
                "ray %u: hit %u of instance %u, geometry %u, primitive %u "
                "where the scene's is %u of %u, %u, %u",
                i, got[0], got[1], got[2], got[3], wanted.status,
                wanted.instance, wanted.geometry, wanted.primitive) &&
        trace::wrong > 16)
      break;
  }
  readback->Unmap(0, nullptr);
  expect(checked > rays / 2 && hits > checked / 4,
         "%u rays of %u checked, %u of them hit", checked, rays, hits);

  step("refusals");
  CHECK(allocator->Reset());
  CHECK(list->Reset(allocator.Get(), nullptr));
  for (auto type : {NVAPI_D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES_EX,
                    NVAPI_D3D12_RAYTRACING_GEOMETRY_TYPE_DMM_TRIANGLES_EX,
                    NVAPI_D3D12_RAYTRACING_GEOMETRY_TYPE_SPHERES_EX,
                    NVAPI_D3D12_RAYTRACING_GEOMETRY_TYPE_LSS_EX}) {
    auto refused = triangles;
    refused[1].type = type;
    auto in = extended(scene.triangle_inputs, refused.data());
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    NVAPI_GET_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO_EX_PARAMS params{
        NVAPI_GET_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO_EX_PARAMS_VER,
        &in, &info};
    expect(prebuild(device.Get(), &params) == NVAPI_NOT_SUPPORTED,
           "the prebuild information of geometry type %u", type);
    expect(build_ex(in, standard.Get()) == NVAPI_NOT_SUPPORTED,
           "a build of geometry type %u", type);
  }
  for (
      auto flag :
      {NVAPI_D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_OMM_UPDATE_EX,
       NVAPI_D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_DISABLE_OMMS_EX,
       NVAPI_D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_OMM_OPACITY_STATES_UPDATE_EX,
       NVAPI_D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_DATA_ACCESS_EX}) {
    auto in = triangle_inputs;
    in.flags = NVAPI_D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS_EX(
        in.flags | flag);
    expect(build_ex(in, standard.Get()) == NVAPI_NOT_SUPPORTED,
           "a build with flag %#x", flag);
  }
  expect(build_ex(triangle_inputs, standard.Get(), 1, nullptr) ==
             NVAPI_INVALID_COMBINATION,
         "post-build information counted and not given");
  NVAPI_D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC_EX desc{
      standard->GetGPUVirtualAddress(), triangle_inputs, 0,
      scene.scratch->GetGPUVirtualAddress()};
  NVAPI_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_EX_PARAMS other{
      NVAPI_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_EX_PARAMS_VER + 1, &desc};
  expect(build(list.Get(), &other) == NVAPI_INCOMPATIBLE_STRUCT_VERSION,
         "a build of another version");
  expect(build(nullptr, &other) == NVAPI_INVALID_ARGUMENT &&
             build(list.Get(), nullptr) == NVAPI_INVALID_ARGUMENT,
         "a build without its list or parameters");
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  return verdict();
}
