// contract: what a ray meets follows from the shaders its hit groups have (DXR functional spec, "TraceRay control
// flow", "Hit groups", D3D12_RAYTRACING_INSTANCE_FLAGS and RAY_FLAG,
// https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html): a triangle that is not opaque is a hit unless its
// group's any hit shader ignores it; a procedural primitive is a hit only where its group's intersection shader
// reports one, and never where its record is a null identifier ("a hit group with no shaders"). the ray flags and the
// instances' flags force opacity and cull by facing, by opacity and by kind of geometry, and the instance mask selects
// instances, whichever shaders there are. a ray that takes its first hit (RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH)
// hits or misses as the closest hit does, but may take any of its hits. the closest hit shader sees the hit's
// instance, geometry, primitive, kind, T, attributes and the ray's origin in the instance's space; the miss shader sees
// the ray's end.
// the scene and the CPU ray caster are d3d12_rays.hpp's. every case traces its grid of rays with four state objects:
// - closest hit shaders alone, the boxes' records null;
// - the same, with a hit group no record names that has an any hit shader;
// - an any hit shader for the quad that is not opaque, which ignores a triangle whose primitive index and instance ID
//   sum to an odd number;
// - an intersection shader for the boxes, which reports a hit at a box's near face, and no any hit shader.
#include "d3d12_rays.hpp"

static const char hlsl[] = R"hlsl(
RaytracingAccelerationStructure scene : register(t0);
struct Result { uint status, instance, geometry, primitive, kind; float t, u, v, object_x, tcurrent; };
RWStructuredBuffer<Result> results : register(u0);
cbuffer Case : register(b0) { uint flags; uint mask; float tmin; float tmax; };
struct Payload { Result r; };
struct BoxAttributes { float2 at; uint primitive; };
// the boxes: x and y bounds and the near face's z
static const float box_bounds[BOXES][5] = { BOX_LIST };

[shader("raygeneration")]
void raygen() {
  uint2 index = DispatchRaysIndex().xy;
  RayDesc ray = { float3(index * STEP + float2(X0, Y0), Z0), tmin, float3(0, 0, 1), tmax };
  Payload p;
  p.r = (Result)0;
  p.r.status = UNTOUCHED;
  TraceRay(scene, flags, mask, 0, 1, 0, ray, p);
  results[index.y * DispatchRaysDimensions().x + index.x] = p.r;
}
[shader("miss")]
void miss(inout Payload p) {
  p.r.status = 0;
  p.r.tcurrent = RayTCurrent();
}
void hit(inout Payload p, uint status, float2 attributes) {
  p.r.status = status;
  p.r.instance = InstanceIndex();
  p.r.geometry = GeometryIndex();
  p.r.primitive = PrimitiveIndex();
  p.r.kind = HitKind();
  p.r.t = RayTCurrent();
  p.r.u = attributes.x;
  p.r.v = attributes.y;
  p.r.object_x = ObjectRayOrigin().x;
  p.r.tcurrent = RayTCurrent();
}
[shader("closesthit")]
void closest(inout Payload p, in BuiltInTriangleIntersectionAttributes a) { hit(p, 1, a.barycentrics); }
[shader("closesthit")]
void closest_box(inout Payload p, in BoxAttributes a) { hit(p, 2, a.at + float2(0, a.primitive)); }
[shader("anyhit")]
void unused_any_hit(inout Payload p, in BuiltInTriangleIntersectionAttributes a) { IgnoreHit(); }
[shader("anyhit")]
void any_hit(inout Payload p, in BuiltInTriangleIntersectionAttributes a) {
  if ((PrimitiveIndex() + InstanceID()) % 2)
    IgnoreHit();
}
[shader("intersection")]
void intersection() {
  float3 o = ObjectRayOrigin();
  float b[5] = box_bounds[PrimitiveIndex()];
  BoxAttributes a = { o.xy, PrimitiveIndex() };
  if (o.x > b[0] && o.x < b[1] && o.y > b[2] && o.y < b[3])
    ReportHit((b[4] - o.z) / ObjectRayDirection().z, KIND + PrimitiveIndex(), a);
}
)hlsl";

struct Result {
  UINT status, instance, geometry, primitive, kind;
  float t, u, v, object_x, tcurrent;
};
struct Case {
  UINT flags, mask;
  float tmin, tmax;
};
static constexpr UINT untouched = 0xabc, kind = 0x10;

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler) || !compiler.dxc) {
    printf("skipped: ray tracing libraries need DXC\n");
    return 77;
  }
  RayScene scene;
  auto number = [](float f) { return std::to_string(f); };
  std::string box_list;
  for (auto &b : scene.aabbs)
    box_list += (box_list.empty() ? "{" : ", {") + number(b.MinX) + ", " + number(b.MaxX) + ", " + number(b.MinY) + ", " +
                number(b.MaxY) + ", " + number(b.MinZ) + "}";
  step("compile the library");
  auto library = compiler.compile(
      hlsl, "", "lib_6_3",
      {"STEP=" + number(scene.step), "X0=" + number(scene.x0), "Y0=" + number(scene.y0), "Z0=" + number(scene.z0),
       "UNTOUCHED=" + std::to_string(untouched), "KIND=" + std::to_string(kind),
       "BOXES=" + std::to_string(std::size(scene.aabbs)), "BOX_LIST=" + box_list}
  );
  if (library.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device5> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
  device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5));
  if (options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_0) {
    printf("skipped: no ray tracing\n");
    return 77;
  }
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList4> list;
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));

  step("build the scene");
  CHECK(scene.create(device.Get()));
  scene.build(list.Get(), scene.triangle_inputs, scene.triangle_memory.Get());
  scene.build(list.Get(), scene.box_inputs, scene.box_memory.Get());
  scene.build_scene(list.Get(), scene.triangle_memory.Get(), scene.box_memory.Get());
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  step("create the state objects");
  D3D12_ROOT_PARAMETER parameters[3]{{D3D12_ROOT_PARAMETER_TYPE_SRV}, {D3D12_ROOT_PARAMETER_TYPE_UAV},
                                    {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}};
  parameters[2].Constants = {0, 0, sizeof(Case) / sizeof(UINT)};
  auto global_rs = root_signature(device.Get(), {3, parameters});
  if (!expect(!!global_rs, "root signature"))
    return verdict();
  D3D12_RAYTRACING_SHADER_CONFIG shader_config{sizeof(Result), sizeof(float) * 2 + sizeof(UINT)};
  D3D12_RAYTRACING_PIPELINE_CONFIG pipeline_config{1};
  D3D12_GLOBAL_ROOT_SIGNATURE global{global_rs.Get()};
  // the groups: of the opaque triangles and the quad, and of the boxes (null where a state object has none)
  D3D12_HIT_GROUP_DESC group{L"group", D3D12_HIT_GROUP_TYPE_TRIANGLES, nullptr, L"closest"};
  D3D12_HIT_GROUP_DESC unused{L"unused", D3D12_HIT_GROUP_TYPE_TRIANGLES, L"unused_any_hit", L"closest"};
  D3D12_HIT_GROUP_DESC quad{L"quad", D3D12_HIT_GROUP_TYPE_TRIANGLES, L"any_hit", L"closest"};
  D3D12_HIT_GROUP_DESC boxes{L"boxes", D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE, nullptr, L"closest_box", L"intersection"};
  enum { ClosestOnly, Unused, AnyHit, Intersection, States };
  const char *state_names[] = {"closest hit shaders alone", "an unused any hit shader", "an any hit shader",
                               "an intersection shader"};
  struct {
    std::vector<D3D12_EXPORT_DESC> exports;
    D3D12_HIT_GROUP_DESC *extra;
    const wchar_t *quad_group, *box_group;
  } states_desc[States] = {
      {{{L"raygen"}, {L"miss"}, {L"closest"}}, nullptr, L"group", nullptr},
      {{{L"raygen"}, {L"miss"}, {L"closest"}, {L"unused_any_hit"}}, &unused, L"group", nullptr},
      {{{L"raygen"}, {L"miss"}, {L"closest"}, {L"any_hit"}}, &quad, L"quad", nullptr},
      {{{L"raygen"}, {L"miss"}, {L"closest"}, {L"closest_box"}, {L"intersection"}}, &boxes, L"group", L"boxes"},
  };
  ComPtr<ID3D12StateObject> states[States];
  for (UINT s = 0; s < States; s++) {
    auto &d = states_desc[s];
    D3D12_DXIL_LIBRARY_DESC code{bytecode(library), (UINT)d.exports.size(), d.exports.data()};
    std::vector<D3D12_STATE_SUBOBJECT> subobjects = {
        {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &code},
        {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &group},
        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader_config},
        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipeline_config},
        {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global},
    };
    if (d.extra)
      subobjects.push_back({D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, d.extra});
    D3D12_STATE_OBJECT_DESC desc{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, (UINT)subobjects.size(), subobjects.data()};
    step("create the state object with %s", state_names[s]);
    CHECK(device->CreateStateObject(&desc, IID_PPV_ARGS(&states[s])));
  }

  // the tables of each state object: the ray generation and miss records, then a hit group record for each geometry
  // of each instance (its contribution is twice its index): the opaque triangles', the quad's and the boxes' group
  const UINT stride = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT, hit_records = 2 * (UINT)scene.instances.size();
  const UINT records = 2 + hit_records;
  auto tables = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, States * records * stride, D3D12_RESOURCE_STATE_GENERIC_READ);
  const UINT rays = RayScene::size * RayScene::size;
  Case cases[] = {
      {0, 0xff, 0, 100},
      {RAY_FORCE_OPAQUE, 0xff, 0, 100},
      {RAY_FORCE_NON_OPAQUE, 0xff, 0, 100},
      {RAY_CULL_BACK, 0xff, 0, 100},
      {RAY_CULL_FRONT, 0xff, 0, 100},
      {RAY_CULL_OPAQUE, 0xff, 0, 100},
      {RAY_CULL_NON_OPAQUE, 0xff, 0, 100},
      {RAY_FORCE_NON_OPAQUE | RAY_CULL_NON_OPAQUE, 0xff, 0, 100},
      {RAY_FORCE_OPAQUE | RAY_CULL_OPAQUE, 0xff, 0, 100},
      {RAY_SKIP_TRIANGLES, 0xff, 0, 100},
      {RAY_SKIP_PROCEDURAL, 0xff, 0, 100},
      {RAY_SKIP_CLOSEST_HIT, 0xff, 0, 100},
      {RAY_ACCEPT_FIRST_HIT, 0xff, 0, 100},
      {0, 0x0b, 0, 100},
      // an extent that ends between the quad and the triangles behind it
      {0, 0xff, -scene.z0 + scene.quad_shift / 2, -scene.z0 + 0.5f},
  };
  const UINT case_count = std::size(cases);
  auto results = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, rays * sizeof(Result), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, States * case_count * rays * sizeof(Result),
                         D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(tables && results && readback, "buffers"))
    return verdict();
  uint8_t *table;
  CHECK(tables->Map(0, nullptr, (void **)&table));
  memset(table, 0, States * records * stride);
  for (UINT s = 0; s < States; s++) {
    ComPtr<ID3D12StateObjectProperties> properties;
    CHECK(states[s].As(&properties));
    auto put = [&](UINT record, const wchar_t *name) {
      auto identifier = properties->GetShaderIdentifier(name);
      if (expect(!!identifier, "%s: identifier of %ls", state_names[s], name))
        memcpy(table + (s * records + record) * stride, identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    };
    put(0, L"raygen");
    put(1, L"miss");
    for (UINT i = 0; i < scene.instances.size(); i++) {
      if (scene.instances[i].structure == &scene.triangles) {
        put(2 + 2 * i, L"group");
        put(2 + 2 * i + 1, states_desc[s].quad_group);
      } else if (scene.instances[i].structure == &scene.boxes && states_desc[s].box_group) {
        put(2 + 2 * i, states_desc[s].box_group);
      }
    }
  }
  Result *out;
  CHECK(readback->Map(0, nullptr, (void **)&out));
  memset(out, 0xff, States * case_count * rays * sizeof(Result));

  step("trace");
  CHECK(allocator->Reset());
  CHECK(list->Reset(allocator.Get(), nullptr));
  list->SetComputeRootSignature(global_rs.Get());
  list->SetComputeRootShaderResourceView(0, scene.scene_memory->GetGPUVirtualAddress());
  list->SetComputeRootUnorderedAccessView(1, results->GetGPUVirtualAddress());
  for (UINT s = 0; s < States; s++) {
    list->SetPipelineState1(states[s].Get());
    auto at = [&](UINT record) { return tables->GetGPUVirtualAddress() + (s * records + record) * stride; };
    D3D12_DISPATCH_RAYS_DESC dispatch{{at(0), stride},       {at(1), stride, stride}, {at(2), hit_records * stride, stride},
                                      {},                    RayScene::size,          RayScene::size,
                                      1};
    for (UINT c = 0; c < case_count; c++) {
      list->SetComputeRoot32BitConstants(2, sizeof(Case) / sizeof(UINT), &cases[c], 0);
      list->DispatchRays(&dispatch);
      transition(list.Get(), results.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyBufferRegion(readback.Get(), (s * case_count + c) * rays * sizeof(Result), results.Get(), 0,
                             rays * sizeof(Result));
      transition(list.Get(), results.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
  }
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  step("compare");
  UINT wrong = 0, hits[3] = {};
  for (UINT s = 0; s < States; s++) {
    // what the state object's shaders decide: the quad's any hit shader, and the boxes' intersection shader
    Decisions decide;
    if (s == AnyHit)
      decide.triangle = [&](UINT instance, const Triangle &t) {
        return t.geometry == 0 || (t.primitive + scene.instances[instance].id) % 2 == 0;
      };
    if (s == Intersection)
      decide.box = [](UINT, const Box &, bool) { return true; };
    for (UINT c = 0; c < case_count; c++)
      for (UINT i = 0; i < rays; i++) {
        auto &k = cases[c];
        float margin = FLT_MAX, x = scene.ray_x(i), y = scene.ray_y(i);
        auto hit = cast(scene.instances, k.flags, k.mask, k.tmin, k.tmax, x, y, scene.z0, decide, margin);
        if (!expect(RayScene::clear(margin), "case %u, ray %u: the ray passes too close to an edge", c, i))
          return verdict();
        Result want{untouched};
        if (!hit.status) {
          want.status = 0;
          want.tcurrent = k.tmax;
        } else if (!(k.flags & RAY_SKIP_CLOSEST_HIT)) {
          bool box = hit.status == 2;
          want = {hit.status,
                  hit.instance,
                  hit.geometry,
                  hit.primitive,
                  box ? kind + hit.primitive : hit.front ? 0xfeu : 0xffu,
                  hit.t,
                  box ? hit.origin[0] : hit.bary[0],
                  box ? hit.origin[1] + hit.primitive : hit.bary[1],
                  hit.origin[0],
                  hit.t};
        }
        hits[want.status < 3 ? want.status : 0] += want.status == 1 || want.status == 2;
        auto &got = out[(s * case_count + c) * rays + i];
        auto close = [](float a, float b) { return std::abs(a - b) < 1e-3f; };
        bool same = k.flags & RAY_ACCEPT_FIRST_HIT
                        ? (got.status == 0) == (want.status == 0) && got.status != untouched
                        : got.status == want.status && got.instance == want.instance && got.geometry == want.geometry &&
                              got.primitive == want.primitive && got.kind == want.kind && close(got.t, want.t) &&
                              close(got.u, want.u) && close(got.v, want.v) && close(got.object_x, want.object_x) &&
                              close(got.tcurrent, want.tcurrent);
        if (!same && wrong++ < 12)
          expect(false,
                 "%s, case %u (flags %#x mask %#x), ray %u,%u: status %#x instance %u geometry %u primitive %u kind %#x "
                 "t %g attributes %g,%g object x %g tcurrent %g; want %#x %u %u %u %#x %g %g,%g %g %g",
                 state_names[s], c, k.flags, k.mask, i % RayScene::size, i / RayScene::size, got.status, got.instance,
                 got.geometry, got.primitive, got.kind, got.t, got.u, got.v, got.object_x, got.tcurrent, want.status,
                 want.instance, want.geometry, want.primitive, want.kind, want.t, want.u, want.v, want.object_x,
                 want.tcurrent);
      }
  }
  if (wrong > 12)
    expect(false, "%u more wrong rays", wrong - 12);
  printf("%u triangle and %u procedural hits of %u rays\n", hits[1], hits[2], States * case_count * rays);
  return verdict();
}
