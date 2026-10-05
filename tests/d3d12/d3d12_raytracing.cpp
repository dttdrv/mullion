// contract: ray tracing pipelines (DXR functional spec: "TraceRay control flow", "Addressing calculations within shader
// tables", "Shader record", state objects and DispatchRays). a ray generation shader traces a ray per thread into the
// scene of d3d12_rays.hpp and records what the ray's shaders left in its payload; the CPU ray caster of that header,
// told which hit group each geometry has, says what that must be:
// - the hit group record is RayContribution + Multiplier * GeometryIndex + the instance's contribution, and the miss
//   shader is the record MissShaderIndex names; each shader reads its own record's local root arguments (two root
//   constants next to each other, then a root CBV), and samples a row of texels past its end with the two static
//   samplers of its local root signature ("Local root signatures and shader tables"): the first texel through the
//   one that wraps, the last through the one that clamps;
// - a triangle that is not opaque is a hit if its group has no any hit shader or that shader does not ignore it; a
//   procedural primitive is hit where its group's intersection shader reports it and, if it is not opaque, its any hit
//   shader agrees, and not at all without an intersection shader; a group that is a null identifier has no shaders;
// - the closest hit shader sees the hit's instance, geometry, primitive, kind, T, attributes and transforms, and
//   traces a second ray whose result it adds to its own; RAY_FLAG_SKIP_CLOSEST_HIT_SHADER leaves the payload alone;
// - a hit that ends the search stops the intersection shader that reported it ("Intersection shaders - procedural
//   primitive geometry"): the shader marks its ray after ReportHit, with whether the hit was accepted. a ray crosses
//   one box at most, so the mark is that box's: none where the ray takes its first hit or the box's any hit shader
//   ends the search, and "accepted" where the box is the ray's hit without either;
// - a callable shader, chosen by the thread, changes its parameter with its record's arguments.
// the same pipeline made from a collection gives the same results, and a pipeline grown with AddToStateObject runs
// the miss shader it was grown with while the identifiers it had stay valid.
// a library can define the subobjects itself ("Subobjects in DXIL libraries"): root signatures, hit groups, configs
// and associations. a state object made of such a library alone traces the same; a second local root signature, with
// the records' two constants the other way round, shows which association a shader got ("Subobject association
// behavior"): the library's own association, an association the state object makes with a library's subobject, which
// overrides it for the exports it names, and a root signature the state object declares, which overrides it for all.
// with a list of exports a library gives only the subobjects the list names.
// a shader may call functions its library does not inline, and functions another library of its state object's
// description defines, with that library's resources ("Subobjects" and D3D12_STATE_OBJECT_FLAGS): the miss shader a
// pipeline is grown with calls one of each, the second from a library beside its own or from a collection that lets
// its definitions be depended on. a collection that lets its exports depend on outside definitions has no root
// signatures of its own, and traces with the ones of the pipeline that takes it. such a collection's own associations
// are the pipeline's to resolve ("Subobject association behavior"): one with a subobject that only the pipeline's
// library defines ("Explicit associations": neither end has "to even be visible yet"), one that names an export
// the pipeline takes under another name (D3D12_EXPORT_DESC), and a library's of the collection, which a root
// signature the pipeline declares does not override as it would its own library's ("do not override any existing
// associations in contained collections") and an association the pipeline makes with the same shader conflicts
// with; where the collection gave a shader only a default, the pipeline's association with the shader stands, also
// when the subobject it names is in another collection's library ("can be in any scope in the object").
// a pipeline's stack size starts as "Default pipeline stack size" makes it of its shaders' sizes and its recursion
// depth, wherever the pipeline config comes from, and a grown pipeline starts with the size of the one it grew from.
#include "d3d12_rays.hpp"

static const char hlsl[] = R"hlsl(
RaytracingAccelerationStructure scene : register(t0);
struct Result {
  uint status, instance, id, geometry, primitive, kind, record, local, flags, miss, called, secondary;
  float t; float2 attributes; float object_x, to_world_x, tcurrent; float2 sampled;
};
RWStructuredBuffer<Result> results : register(u0);
// what an intersection shader leaves a ray after ReportHit: two for each thread, its ray's and the ray's that a
// closest hit shader traces
RWStructuredBuffer<uint> marks : register(u1);
Texture2D<float> row : register(t1);
// the local root signature's
SamplerState wrapping : register(s0);
SamplerState clamping : register(s1);
cbuffer Case : register(b0) {
  uint flags; uint mask; float tmin; float tmax; uint ray_contribution; uint multiplier; uint miss_index;
};
// a shader record's arguments
cbuffer RecordFirst : register(b1) { uint record_first; };
cbuffer RecordSecond : register(b2) { uint record_second; };
cbuffer RecordBuffer : register(b3) { uint record_buffer; };

struct Payload { Result r; uint depth; };
struct Call { uint value; };
struct BoxAttributes { float2 at; uint primitive; };
// the boxes of the procedural geometry: x and y bounds and the near face's z
static const float box_bounds[BOXES][5] = { BOX_LIST };

void record(inout Result r) {
  r.record = record_first;
  r.local = record_second + record_buffer;
  r.flags = RayFlags();
  r.sampled = float2(row.SampleLevel(wrapping, float2(BEYOND, 0.5), 0), row.SampleLevel(clamping, float2(BEYOND, 0.5), 0));
}

[shader("raygeneration")]
void raygen() {
  uint2 index = DispatchRaysIndex().xy;
  RayDesc ray;
  ray.Origin = float3(index * STEP + float2(X0, Y0), Z0);
  ray.Direction = float3(0, 0, 1);
  ray.TMin = tmin;
  ray.TMax = tmax;
  Payload p = (Payload)0;
  p.r.status = UNTOUCHED;
  uint thread = index.y * DispatchRaysDimensions().x + index.x;
  marks[2 * thread] = marks[2 * thread + 1] = 0;
  TraceRay(scene, flags, mask, ray_contribution, multiplier, miss_index, ray, p);
  Call c = { index.x };
  CallShader((index.x + index.y) % CALLABLES, c);
  p.r.called = c.value;
  results[thread] = p.r;
}

void missed(inout Payload p, uint which) {
  p.r.status = 0;
  p.r.miss = which;
  p.r.tcurrent = RayTCurrent();
  record(p.r);
}
[shader("miss")] void miss(inout Payload p) { missed(p, 1); }
[shader("miss")] void miss_other(inout Payload p) { missed(p, 2); }

void hit(inout Payload p, uint status, float2 attributes) {
  p.r.status = status;
  p.r.instance = InstanceIndex();
  p.r.id = InstanceID();
  p.r.geometry = GeometryIndex();
  p.r.primitive = PrimitiveIndex();
  p.r.attributes = attributes;
  // the ray of the thread SHIFT to the right, traced from the hit, after which this hit's values are still its own
  if (p.depth == 0) {
    RayDesc ray;
    ray.Origin = WorldRayOrigin() + float3(SHIFT, 0, 0);
    ray.Direction = WorldRayDirection();
    ray.TMin = 0;
    ray.TMax = FAR;
    Payload second = (Payload)0;
    second.r.status = UNTOUCHED;
    second.depth = 1;
    TraceRay(scene, RAY_FLAG_NONE, 0xff, 0, 1, 1, ray, second);
    p.r.secondary = second.r.status + 16 * second.r.instance + 256 * second.r.miss;
  }
  p.r.kind = HitKind();
  p.r.t = RayTCurrent();
  p.r.object_x = ObjectRayOrigin().x;
  p.r.to_world_x = ObjectToWorld3x4()[0][3];
  record(p.r);
}
[shader("closesthit")]
void closest(inout Payload p, in BuiltInTriangleIntersectionAttributes a) { hit(p, 1, a.barycentrics); }
[shader("closesthit")]
void closest_box(inout Payload p, in BoxAttributes a) { hit(p, 2, a.at + a.primitive); }

// a triangle is a hit when its primitive index and its instance's ID sum to an even number
[shader("anyhit")]
void any_hit(inout Payload p, in BuiltInTriangleIntersectionAttributes a) {
  if ((PrimitiveIndex() + InstanceID()) % 2)
    IgnoreHit();
}
// the first box is no hit; the second, in front of everything, ends the search
[shader("anyhit")]
void any_hit_box(inout Payload p, in BoxAttributes a) {
  if (a.primitive == 0)
    IgnoreHit();
  AcceptHitAndEndSearch();
}
// a box is hit at its near face
[shader("intersection")]
void intersection() {
  float3 o = ObjectRayOrigin();
  float b[5] = box_bounds[PrimitiveIndex()];
  BoxAttributes a = { o.xy, PrimitiveIndex() };
  if (o.x > b[0] && o.x < b[1] && o.y > b[2] && o.y < b[3]) {
    bool accepted = ReportHit((b[4] - o.z) / ObjectRayDirection().z, KIND + PrimitiveIndex(), a);
    // a second ray starts SHIFT to the right of its thread's
    uint2 index = DispatchRaysIndex().xy;
    bool second = WorldRayOrigin().x > index.x * STEP + X0 + SHIFT / 2;
    marks[2 * (index.y * DispatchRaysDimensions().x + index.x) + second] = accepted ? ACCEPTED : REJECTED;
  }
}

[shader("callable")] void call_add(inout Call c) { c.value += record_first; }
[shader("callable")] void call_scale(inout Call c) { c.value *= record_second + record_buffer; }

#ifdef ASSOCIATED
GlobalRootSignature global_rs = { GLOBAL };
#define SAMPLERS "StaticSampler(s0, filter = FILTER_MIN_MAG_MIP_POINT, addressU = TEXTURE_ADDRESS_WRAP), " \
                 "StaticSampler(s1, filter = FILTER_MIN_MAG_MIP_POINT, addressU = TEXTURE_ADDRESS_CLAMP)"
LocalRootSignature local_rs = {
  "RootConstants(num32BitConstants = 1, b1), RootConstants(num32BitConstants = 1, b2), CBV(b3), " SAMPLERS
};
LocalRootSignature swapped_rs = {
  "RootConstants(num32BitConstants = 1, b2), RootConstants(num32BitConstants = 1, b1), CBV(b3), " SAMPLERS
};
TriangleHitGroup opaque = { "", "closest" };
TriangleHitGroup alpha = { "any_hit", "closest" };
ProceduralPrimitiveHitGroup boxes = { "any_hit_box", "closest_box", "intersection" };
SubobjectToExportsAssociation association = { ASSOCIATED, "opaque;alpha;boxes;miss;miss_other;call_add;call_scale" };
RaytracingShaderConfig shader_config = { PAYLOAD, ATTRIBUTES };
RaytracingPipelineConfig pipeline_config = { 2 };
#endif
)hlsl";

// a miss shader for AddToStateObject
static const char added_hlsl[] = R"hlsl(
struct Result {
  uint status, instance, id, geometry, primitive, kind, record, local, flags, miss, called, secondary;
  float t; float2 attributes; float object_x, to_world_x, tcurrent; float2 sampled;
};
struct Payload { Result r; uint depth; };
// its record's first argument: this library's first constant buffer, as the case is the other library's
cbuffer RecordFirst : register(b1) { uint record_first; };
// defined in another library
uint beyond(uint value);
[noinline] uint half_of(uint value) { return value / 2; }
[shader("miss")]
void miss_added(inout Payload p) {
  p.r.status = 0;
  p.r.miss = beyond(half_of(2));
  p.r.record = record_first;
  p.r.tcurrent = RayTCurrent();
}
)hlsl";

// the library of the function the added miss shader calls: what it is given, past the case's miss shader index
static const char helper_hlsl[] = R"hlsl(
cbuffer Case : register(b0) {
  uint flags; uint mask; float tmin; float tmax; uint ray_contribution; uint multiplier; uint miss_index;
};
export uint beyond(uint value) { return value + miss_index; }
)hlsl";

struct Result {
  UINT status, instance, id, geometry, primitive, kind, record, local, flags, miss, called, secondary;
  float t, attributes[2], object_x, to_world_x, tcurrent, sampled[2];
};

struct Case {
  UINT flags, mask;
  float tmin, tmax;
  UINT ray_contribution, multiplier, miss_index;
};

// a shader record: an identifier, then the local root arguments
struct Record {
  uint8_t identifier[D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES];
  UINT first, second;
  D3D12_GPU_VIRTUAL_ADDRESS buffer;
  uint8_t padding[16]; // to D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT
};

// what a hit group's shaders are, for the model
struct Group {
  const wchar_t *name;
  bool closest, any_hit, intersection;
};

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  RayScene scene;
  const UINT size = scene.size, rays = size * size, untouched = 3, kind = 5, callables = 2;
  // an intersection shader's marks
  const UINT rejected = 1, accepted = 2;
  const float shift = 5, distant = 100;
  // the row of texels, and where it is sampled: a fifth of its width past its end, inside the first texel again
  const float texels[] = {10, 20, 30, 40}, beyond = 1.2f;
  std::string box_list;
  for (auto &b : scene.aabbs)
    box_list += (box_list.empty() ? "{" : ", {") + std::to_string(b.MinX) + ", " + std::to_string(b.MaxX) + ", " +
                std::to_string(b.MinY) + ", " + std::to_string(b.MaxY) + ", " + std::to_string(b.MinZ) + "}";
  std::vector<std::string> defines = {
      "STEP=" + std::to_string(scene.step), "X0=" + std::to_string(scene.x0), "Y0=" + std::to_string(scene.y0),
      "Z0=" + std::to_string(scene.z0), "UNTOUCHED=" + std::to_string(untouched), "KIND=" + std::to_string(kind),
      "CALLABLES=" + std::to_string(callables), "SHIFT=" + std::to_string(shift), "FAR=" + std::to_string(distant),
      "BOXES=" + std::to_string(std::size(scene.aabbs)), "BOX_LIST=" + box_list, "BEYOND=" + std::to_string(beyond),
      "REJECTED=" + std::to_string(rejected), "ACCEPTED=" + std::to_string(accepted),
  };
  auto library = compiler.compile(hlsl, "", "lib_6_3", defines), added = compiler.compile(added_hlsl, "", "lib_6_3");
  // the library with its own subobjects, its association with the local root signature or the swapped one
  auto with_subobjects = [&](const char *associated) {
    auto d = defines;
    d.insert(d.end(), {std::string("ASSOCIATED=\"") + associated + "\"", "GLOBAL=\"SRV(t0), UAV(u0), RootConstants(num32BitConstants = " + std::to_string(sizeof(Case) / sizeof(UINT)) + ", b0), DescriptorTable(SRV(t1)), UAV(u1)\"",
                       "PAYLOAD=" + std::to_string(sizeof(Result) + sizeof(UINT)), "ATTRIBUTES=" + std::to_string(3 * sizeof(UINT))});
    return compiler.compile(hlsl, "", "lib_6_3", d);
  };
  auto own = with_subobjects("local_rs"), own_swapped = with_subobjects("swapped_rs");
  // a library of shader model 6.6, which annotates the handles of its resources
  auto helper = compiler.compile(helper_hlsl, "", "lib_6_6");
  if (library.empty() || added.empty() || own.empty() || own_swapped.empty() || helper.empty()) {
    printf("skipped: shader libraries are shader model 6.3\n");
    return 77;
  }
  ComPtr<ID3D12Device7> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options)));
  if (options.RaytracingTier < D3D12_RAYTRACING_TIER_1_1) {
    printf("failed: ray tracing tier %d\n", options.RaytracingTier);
    return 1;
  }

  // the global root signature: the scene, the results, the case, the row of texels and the marks; the local one: a
  // record's arguments, and the two samplers
  D3D12_ROOT_PARAMETER global_params[5]{}, local_params[3]{};
  global_params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  global_params[4].Descriptor = {1, 0};
  const D3D12_DESCRIPTOR_RANGE row_range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1};
  global_params[3].DescriptorTable = {1, &row_range};
  D3D12_STATIC_SAMPLER_DESC samplers[2]{};
  for (UINT i = 0; i < 2; i++) {
    samplers[i].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samplers[i].AddressU = i ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[i].AddressV = samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[i].ShaderRegister = i;
  }
  global_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  global_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  global_params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  global_params[2].Constants = {0, 0, sizeof(Case) / sizeof(UINT)};
  local_params[0].ParameterType = local_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  local_params[0].Constants = {1, 0, 1};
  local_params[1].Constants = {2, 0, 1};
  local_params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  local_params[2].Descriptor = {3, 0};
  auto global_rs = root_signature(device.Get(), {5, global_params});
  auto local_rs = root_signature(
      device.Get(), {3, local_params, 2, samplers, D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE}
  );
  // the records' two constants the other way round
  std::swap(local_params[0], local_params[1]);
  auto swapped_rs = root_signature(
      device.Get(), {3, local_params, 2, samplers, D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE}
  );
  if (!global_rs || !local_rs || !swapped_rs) {
    printf("failed: root signatures\n");
    return 1;
  }

  // the hit groups, and the records of the hit group table: two for each instance, for the two geometries a
  // structure may have. triangles' first geometry is opaque and its second is not; boxes have one
  const Group groups[] = {
      {L"opaque", true, false, false}, {L"alpha", true, true, false}, {L"boxes", true, true, true}, {nullptr},
  };
  enum { OPAQUE_GROUP, ALPHA_GROUP, BOX_GROUP, NO_GROUP };
  std::vector<UINT> hit_records;
  for (auto &instance : scene.instances) {
    bool boxes = instance.structure == &scene.boxes;
    hit_records.push_back(!instance.structure ? NO_GROUP : boxes ? BOX_GROUP : OPAQUE_GROUP);
    hit_records.push_back(!instance.structure || boxes ? NO_GROUP : ALPHA_GROUP);
  }
  const wchar_t *miss_names[] = {L"miss", L"miss_other", L"miss_added"}, *call_names[] = {L"call_add", L"call_scale"};

  D3D12_HIT_GROUP_DESC hit_groups[] = {
      {L"opaque", D3D12_HIT_GROUP_TYPE_TRIANGLES, nullptr, L"closest", nullptr},
      {L"alpha", D3D12_HIT_GROUP_TYPE_TRIANGLES, L"any_hit", L"closest", nullptr},
      {L"boxes", D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE, L"any_hit_box", L"closest_box", L"intersection"},
  };
  D3D12_DXIL_LIBRARY_DESC library_desc{bytecode(library)}, added_desc{bytecode(added)}, helper_desc{bytecode(helper)};
  D3D12_RAYTRACING_SHADER_CONFIG shader_config{sizeof(Result) + sizeof(UINT), 3 * sizeof(UINT)};
  D3D12_RAYTRACING_PIPELINE_CONFIG pipeline_config{2};
  D3D12_GLOBAL_ROOT_SIGNATURE global{global_rs.Get()};
  D3D12_LOCAL_ROOT_SIGNATURE local{local_rs.Get()};
  D3D12_STATE_OBJECT_CONFIG additions{D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS};
  // the local root signature is for the shaders with records' arguments: everything but the ray generation shader
  const wchar_t *with_arguments[] = {L"opaque", L"alpha", L"boxes", L"miss", L"miss_other", L"call_add", L"call_scale"};
  // the subobjects of a state object: the last two are the local root signature and its association
  std::vector<D3D12_STATE_SUBOBJECT> subobjects = {
      {D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &additions},
      {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library_desc},
      {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hit_groups[0]},
      {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hit_groups[1]},
      {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hit_groups[2]},
      {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader_config},
      {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipeline_config},
      {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global},
      {D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &local},
      {},
  };
  D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION association{&subobjects[subobjects.size() - 2], (UINT)std::size(with_arguments),
                                                     with_arguments};
  subobjects.back() = {D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &association};
  auto state_object = [&](D3D12_STATE_OBJECT_TYPE type, const std::vector<D3D12_STATE_SUBOBJECT> &of, ComPtr<ID3D12StateObject> &out) {
    D3D12_STATE_OBJECT_DESC desc{type, (UINT)of.size(), of.data()};
    return device->CreateStateObject(&desc, IID_PPV_ARGS(&out));
  };
  // the pipeline made at once; made of a collection; and grown from the first with one more miss shader
  ComPtr<ID3D12StateObject> pipeline, collection, collected, grown;
  CHECK(state_object(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, subobjects, pipeline));
  CHECK(state_object(D3D12_STATE_OBJECT_TYPE_COLLECTION, subobjects, collection));
  D3D12_EXISTING_COLLECTION_DESC existing{collection.Get()};
  CHECK(state_object(
      D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE,
      {{D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &additions},
       {D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &existing},
       {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader_config},
       {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipeline_config}},
      collected
  ));
  // the addition: the miss shader's library, and the library of the function it calls, which reads the case
  std::vector<D3D12_STATE_SUBOBJECT> addition = {
      {D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &additions},
      {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &added_desc},
      {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &helper_desc},
      {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader_config},
      {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipeline_config},
      {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global},
      {D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &local},
      {},
  };
  const wchar_t *added_names[] = {L"miss_added"};
  D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION added_association{&addition[addition.size() - 2], 1, added_names};
  addition.back() = {D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &added_association};
  D3D12_STATE_OBJECT_DESC addition_desc{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, (UINT)addition.size(), addition.data()};
  CHECK(device->AddToStateObject(&addition_desc, pipeline.Get(), IID_PPV_ARGS(&grown)));
  // the same with the function's library in a collection that lets its definitions be depended on
  ComPtr<ID3D12StateObject> helpers, grown_with_collection;
  D3D12_STATE_OBJECT_CONFIG dependable{
      D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS | D3D12_STATE_OBJECT_FLAG_ALLOW_EXTERNAL_DEPENDENCIES_ON_LOCAL_DEFINITIONS
  };
  CHECK(state_object(
      D3D12_STATE_OBJECT_TYPE_COLLECTION,
      {{D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &dependable}, {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &helper_desc}},
      helpers
  ));
  D3D12_EXISTING_COLLECTION_DESC existing_helpers{helpers.Get()};
  addition[2] = {D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &existing_helpers};
  CHECK(device->AddToStateObject(&addition_desc, pipeline.Get(), IID_PPV_ARGS(&grown_with_collection)));
  // a collection whose exports depend on the pipeline that takes it for their root signatures
  ComPtr<ID3D12StateObject> dependent, of_dependent;
  D3D12_STATE_OBJECT_CONFIG depending{
      D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS | D3D12_STATE_OBJECT_FLAG_ALLOW_LOCAL_DEPENDENCIES_ON_EXTERNAL_DEFINITIONS
  };
  CHECK(state_object(
      D3D12_STATE_OBJECT_TYPE_COLLECTION,
      {{D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &depending},
       {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library_desc},
       {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hit_groups[0]},
       {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hit_groups[1]},
       {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hit_groups[2]},
       {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader_config},
       {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipeline_config}},
      dependent
  ));
  D3D12_EXISTING_COLLECTION_DESC existing_dependent{dependent.Get()};
  auto with_dependent = subobjects;
  with_dependent[1] = {D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &existing_dependent};
  // the hit groups are the collection's
  with_dependent.erase(with_dependent.begin() + 2, with_dependent.begin() + 5);
  D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION dependent_association{&with_dependent[with_dependent.size() - 2],
                                                               (UINT)std::size(with_arguments), with_arguments};
  with_dependent.back() = {D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &dependent_association};
  CHECK(state_object(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, with_dependent, of_dependent));

  Case cases[] = {
      {0, 0xff, 0, distant, 0, 1, 0},
      {0, 0xff, 0, distant, 0, 0, 1},
      {0, 0xff, 0, distant, 1, 0, 0},
      {RAY_FORCE_OPAQUE, 0xff, 0, distant, 0, 1, 0},
      {RAY_FORCE_NON_OPAQUE, 0xff, 0, distant, 0, 1, 1},
      {RAY_CULL_BACK, 0xff, 0, distant, 0, 1, 0},
      {RAY_CULL_FRONT, 0xff, 0, distant, 0, 1, 0},
      {RAY_CULL_OPAQUE, 0xff, 0, distant, 0, 1, 0},
      {RAY_CULL_NON_OPAQUE, 0xff, 0, distant, 0, 1, 0},
      {RAY_SKIP_TRIANGLES, 0xff, 0, distant, 0, 1, 0},
      {RAY_SKIP_PROCEDURAL, 0xff, 0, distant, 0, 1, 0},
      {RAY_SKIP_CLOSEST_HIT, 0xff, 0, distant, 0, 1, 0},
      {RAY_ACCEPT_FIRST_HIT, 0xff, 0, distant, 0, 1, 0},
      {0, 0x0b, 0, distant, 0, 1, 0},
      {0, 0xff, -scene.z0 + scene.quad_shift / 2, -scene.z0 + 0.5f, 0, 1, 1},
      // an extent that starts inside the front box, whose near face is then no hit
      {0, 0xff, -scene.z0 + (scene.aabbs[1].MinZ + scene.aabbs[1].MaxZ) / 2, distant, 0, 1, 0},
  };
  const UINT case_count = std::size(cases);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList4> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  // the row of texels, in a heap
  ComPtr<ID3D12Resource> row;
  D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC row_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, (UINT)std::size(texels), 1, 1, 1, DXGI_FORMAT_R32_FLOAT, {1, 0}};
  CHECK(device->CreateCommittedResource(
      &default_heap, D3D12_HEAP_FLAG_NONE, &row_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&row)
  ));
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT row_footprint;
  UINT64 row_bytes;
  device->GetCopyableFootprints(&row_desc, 0, 1, 0, &row_footprint, nullptr, nullptr, &row_bytes);
  auto row_upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, row_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  void *row_data;
  CHECK(row_upload->Map(0, nullptr, &row_data));
  memcpy(row_data, texels, sizeof(texels));
  D3D12_TEXTURE_COPY_LOCATION row_to{row.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
      row_from{row_upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = row_footprint}};
  list->CopyTextureRegion(&row_to, 0, 0, 0, &row_from, nullptr);
  transition(list.Get(), row.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  ComPtr<ID3D12DescriptorHeap> heap;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)));
  device->CreateShaderResourceView(row.Get(), nullptr, heap->GetCPUDescriptorHandleForHeapStart());

  CHECK(scene.create(device.Get()));
  scene.build(list.Get(), scene.triangle_inputs, scene.triangle_memory.Get());
  scene.build(list.Get(), scene.box_inputs, scene.box_memory.Get());
  scene.build_scene(list.Get(), scene.triangle_memory.Get(), scene.box_memory.Get());

  // shader tables: the ray generation record, then the miss, hit group and callable tables; and the constant buffers
  // their records name, a value at each constant buffer alignment
  const UINT miss_count = std::size(miss_names), table_records = 1 + miss_count + hit_records.size() + callables;
  const UINT buffer_step = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
  auto tables = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, table_records * sizeof(Record), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto constants = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, table_records * buffer_step, D3D12_RESOURCE_STATE_GENERIC_READ);
  Record *records;
  uint8_t *constant_data;
  CHECK(tables->Map(0, nullptr, (void **)&records));
  CHECK(constants->Map(0, nullptr, (void **)&constant_data));
  // a record's arguments are derived from its place in the tables
  auto first_of = [](UINT r) { return 1000 + r; };
  auto second_of = [](UINT r) { return 2000 + r; };
  auto buffer_of = [](UINT r) { return 7000 + r; };
  const UINT miss_at = 1, hit_at = miss_at + miss_count, call_at = hit_at + hit_records.size();
  auto fill = [&](ID3D12StateObject *state) -> HRESULT {
    ComPtr<ID3D12StateObjectProperties> properties;
    if (HRESULT hr = state->QueryInterface(IID_PPV_ARGS(&properties)); FAILED(hr))
      return hr;
    for (UINT r = 0; r < table_records; r++) {
      const wchar_t *name = r < miss_at   ? L"raygen"
                            : r < hit_at  ? miss_names[r - miss_at]
                            : r < call_at ? groups[hit_records[r - hit_at]].name
                                          : call_names[r - call_at];
      auto identifier = name ? properties->GetShaderIdentifier(name) : nullptr;
      records[r] = {};
      if (identifier)
        memcpy(records[r].identifier, identifier, sizeof(records[r].identifier));
      records[r].first = first_of(r);
      records[r].second = second_of(r);
      records[r].buffer = constants->GetGPUVirtualAddress() + r * buffer_step;
      UINT value = buffer_of(r);
      memcpy(constant_data + r * buffer_step, &value, sizeof(value));
    }
    return properties->GetShaderIdentifier(L"raygen") ? S_OK : E_FAIL;
  };

  auto results = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, rays * sizeof(Result), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, case_count * rays * sizeof(Result), D3D12_RESOURCE_STATE_COPY_DEST);
  const uint8_t *out;
  CHECK(readback->Map(0, nullptr, (void **)&out));
  const UINT64 marks_size = rays * 2 * sizeof(UINT);
  auto marks = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, marks_size, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto marks_readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, case_count * marks_size, D3D12_RESOURCE_STATE_COPY_DEST);
  const UINT *marked;
  CHECK(marks_readback->Map(0, nullptr, (void **)&marked));
  // dispatches every case through a state object and reads the results back
  auto trace = [&](ID3D12StateObject *state) -> HRESULT {
    if (HRESULT hr = fill(state); FAILED(hr))
      return hr;
    auto at = [&](UINT r) { return tables->GetGPUVirtualAddress() + r * sizeof(Record); };
    D3D12_DISPATCH_RAYS_DESC dispatch{{at(0), sizeof(Record)},
                                      {at(miss_at), miss_count * sizeof(Record), sizeof(Record)},
                                      {at(hit_at), hit_records.size() * sizeof(Record), sizeof(Record)},
                                      {at(call_at), callables * sizeof(Record), sizeof(Record)},
                                      size, size, 1};
    list->SetDescriptorHeaps(1, heap.GetAddressOf());
    list->SetComputeRootSignature(global_rs.Get());
    list->SetComputeRootShaderResourceView(0, scene.scene_memory->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(1, results->GetGPUVirtualAddress());
    list->SetComputeRootDescriptorTable(3, heap->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRootUnorderedAccessView(4, marks->GetGPUVirtualAddress());
    list->SetPipelineState1(state);
    for (UINT c = 0; c < case_count; c++) {
      list->SetComputeRoot32BitConstants(2, sizeof(Case) / sizeof(UINT), &cases[c], 0);
      list->DispatchRays(&dispatch);
      transition(list.Get(), results.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyBufferRegion(readback.Get(), c * rays * sizeof(Result), results.Get(), 0, rays * sizeof(Result));
      transition(list.Get(), results.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      transition(list.Get(), marks.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyBufferRegion(marks_readback.Get(), c * marks_size, marks.Get(), 0, marks_size);
      transition(list.Get(), marks.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    HRESULT hr = submit(device.Get(), queue.Get(), list.Get());
    if (SUCCEEDED(hr) && SUCCEEDED(hr = allocator->Reset()))
      hr = list->Reset(allocator.Get(), nullptr);
    return hr;
  };

  unsigned failures = 0, hits = 0, marks_checked = 0;
  float margin = 1;
  // whether the shaders of an export read their record's two constants the other way round
  std::function<bool(const std::wstring &)> swapped = [](const std::wstring &) { return false; };
  auto swaps = [&](UINT table_record) {
    return swapped(
        table_record < hit_at    ? miss_names[table_record - miss_at]
        : table_record < call_at ? groups[hit_records[table_record - hit_at]].name
                                 : call_names[table_record - call_at]
    );
  };
  // a ray's result, as its shaders leave it. `miss_shaders` is how many miss shaders the state object has
  auto model = [&](const Case &c, float x, float y, UINT miss_shaders) {
    auto record_of = [&](UINT instance, UINT geometry) {
      return c.ray_contribution + c.multiplier * geometry + scene.instances[instance].contribution;
    };
    auto group_of = [&](UINT instance, UINT geometry) -> const Group & {
      return groups[hit_records[record_of(instance, geometry)]];
    };
    auto hit = cast(
        scene.instances, c.flags, c.mask, c.tmin, c.tmax, x, y, scene.z0,
        {[&](UINT instance, const Triangle &t) {
           return !group_of(instance, t.geometry).any_hit || (t.primitive + scene.instances[instance].id) % 2 == 0;
         },
         [&](UINT instance, const Box &b, bool opaque) {
           auto &group = group_of(instance, b.geometry);
           return group.intersection && (opaque || !group.any_hit || b.primitive != 0);
         }},
        margin
    );
    Result r{};
    r.status = untouched;
    auto arguments = [&](UINT table_record) {
      bool swap = swaps(table_record);
      r.record = swap ? second_of(table_record) : first_of(table_record);
      r.local = (swap ? first_of(table_record) : second_of(table_record)) + buffer_of(table_record);
      r.flags = c.flags;
      // a point beyond the row's end by less than a texel
      r.sampled[0] = texels[0];
      r.sampled[1] = std::end(texels)[-1];
    };
    if (!hit.status) {
      // the added miss shader reads its record's first argument alone
      if (c.miss_index < miss_shaders) {
        r.status = 0;
        r.miss = c.miss_index + 1;
        r.tcurrent = c.tmax;
        if (c.miss_index < 2)
          arguments(miss_at + c.miss_index);
        else
          r.record = first_of(miss_at + c.miss_index);
      }
      return r;
    }
    auto &group = group_of(hit.instance, hit.geometry);
    if (!group.closest || (c.flags & RAY_SKIP_CLOSEST_HIT))
      return r;
    auto &instance = scene.instances[hit.instance];
    r.status = hit.status;
    r.instance = hit.instance;
    r.id = instance.id;
    r.geometry = hit.geometry;
    r.primitive = hit.primitive;
    // D3D12_HIT_KIND_TRIANGLE_FRONT_FACE and _BACK_FACE, or what the intersection shader reported
    r.kind = hit.status == 2 ? kind + hit.primitive : hit.front ? 0xfe : 0xff;
    r.t = hit.t;
    r.attributes[0] = hit.status == 2 ? hit.origin[0] + hit.primitive : hit.bary[0];
    r.attributes[1] = hit.status == 2 ? hit.origin[1] + hit.primitive : hit.bary[1];
    r.object_x = hit.origin[0];
    r.to_world_x = instance.translation[0];
    arguments(hit_at + record_of(hit.instance, hit.geometry));
    return r;
  };
  auto compare = [&](const char *what, UINT miss_shaders, UINT first_case, UINT count) {
    for (UINT c = first_case; c < first_case + count; c++)
      for (UINT i = 0; i < rays; i++) {
        Result got;
        memcpy(&got, out + (c * rays + i) * sizeof(Result), sizeof(got));
        float x = scene.ray_x(i), y = scene.ray_y(i);
        auto want = model(cases[c], x, y, miss_shaders);
        // the second ray, from a closest hit shader: with no flags, the first hit groups and the second miss shader
        if (want.status == 1 || want.status == 2) {
          auto second = model({0, 0xff, 0, distant, 0, 1, 1}, x + shift, y, miss_shaders);
          want.secondary = second.status + 16 * second.instance + 256 * second.miss;
        }
        // the callable shader the thread chooses, with its record's arguments
        UINT column = i % size, callable = (i % size + i / size) % callables, r = call_at + callable;
        UINT first = swaps(r) ? second_of(r) : first_of(r), second = swaps(r) ? first_of(r) : second_of(r);
        want.called = callable ? column * (second + buffer_of(r)) : column + first;
        hits += want.status == 1 || want.status == 2;
        // a ray that takes its first hit runs the closest hit shader for one of the hits there are
        bool same = cases[c].flags & RAY_ACCEPT_FIRST_HIT
                        ? (got.status == 0) == (want.status == 0) && got.called == want.called
                        : !memcmp(&got, &want, offsetof(Result, t)) &&
                              std::equal(&got.t, std::end(got.sampled), &want.t, [](float a, float b) {
                                return std::abs(a - b) < 1e-3f;
                              });
        // the box the ray crosses, if any, and what its intersection shader leaves after ReportHit
        for (UINT n = 0; n < scene.instances.size(); n++) {
          auto &instance = scene.instances[n];
          for (auto &box : instance.structure ? instance.structure->boxes : std::vector<Box>{}) {
            float ox = x - instance.translation[0], oy = y - instance.translation[1];
            if (ox < box.min[0] || ox > box.max[0] || oy < box.min[1] || oy > box.max[1])
              continue;
            bool opaque = opaque_to(box.opaque, instance.flags, cases[c].flags);
            auto &group = groups[hit_records[cases[c].ray_contribution + cases[c].multiplier * box.geometry + instance.contribution]];
            // the first hit a ray takes ends its search, as the boxes' any hit shader does for a hit it does not ignore
            bool ends = (cases[c].flags & RAY_ACCEPT_FIRST_HIT) || (!opaque && group.any_hit);
            bool hit = want.status == 2 && want.instance == n && want.primitive == box.primitive;
            UINT mark = marked[c * rays * 2 + 2 * i];
            if ((ends ? mark == accepted : hit && mark != accepted) && failures++ < 12)
              printf("%s, case %u, ray %u,%u: its intersection shader left %u after a hit that %s the search\n", what, c,
                     i % size, i / size, mark, ends ? "ends" : "does not end");
            marks_checked += ends || hit;
          }
        }
        if (!same && failures++ < 12)
          printf(
              "%s, case %u, ray %u,%u: status %u instance %u id %u geometry %u primitive %u kind %#x record %u local %u "
              "flags %#x miss %u called %u secondary %#x t %g attributes %g,%g object %g world %g tcurrent %g sampled "
              "%g,%g; want %u %u %u %u %u %#x %u %u %#x %u %u %#x %g %g,%g %g %g %g %g,%g\n",
              what, c, i % size, i / size, got.status, got.instance, got.id, got.geometry, got.primitive, got.kind,
              got.record, got.local, got.flags, got.miss, got.called, got.secondary, got.t, got.attributes[0],
              got.attributes[1], got.object_x, got.to_world_x, got.tcurrent, got.sampled[0], got.sampled[1], want.status, want.instance, want.id,
              want.geometry, want.primitive, want.kind, want.record, want.local, want.flags, want.miss, want.called,
              want.secondary, want.t, want.attributes[0], want.attributes[1], want.object_x, want.to_world_x,
              want.tcurrent, want.sampled[0], want.sampled[1]
          );
      }
  };

  // "Default pipeline stack size", from the state object's own shader sizes
  const UINT recursion = pipeline_config.MaxTraceRecursionDepth;
  auto stack = [&](ID3D12StateObject *state, UINT64 &now) -> UINT64 {
    ComPtr<ID3D12StateObjectProperties> properties;
    if (FAILED(state->QueryInterface(IID_PPV_ARGS(&properties))))
      return 0;
    auto most = [&](std::initializer_list<const wchar_t *> names) {
      UINT64 size = 0;
      for (auto name : names)
        size = std::max(size, properties->GetShaderStackSize(name));
      return size;
    };
    UINT64 closest = most({L"opaque::closesthit", L"alpha::closesthit", L"boxes::closesthit"}),
           miss = most({L"miss", L"miss_other"}), any = most({L"alpha::anyhit", L"boxes::anyhit"});
    now = properties->GetPipelineStackSize();
    return most({L"raygen"}) + std::max({closest, miss, most({L"boxes::intersection"}) + any}) * std::min(1u, recursion) +
           std::max(closest, miss) * (recursion ? recursion - 1 : 0) + 2 * most({L"call_add", L"call_scale"});
  };
  // a hit group has no stack size of its own, nor has a shader that is not in one a part ("GetShaderStackSize")
  auto no_size = [&](ID3D12StateObject *state) {
    ComPtr<ID3D12StateObjectProperties> properties;
    if (FAILED(state->QueryInterface(IID_PPV_ARGS(&properties))))
      return false;
    for (auto name : {L"opaque", L"boxes", L"miss::closesthit", L"raygen::anyhit", L"opaque::miss", L"nothing"})
      if (properties->GetShaderStackSize(name) != 0xffffffff) {
        printf("failed: the stack size of %ls is %llu, want none\n", name, (unsigned long long)properties->GetShaderStackSize(name));
        return false;
      }
    return true;
  };
  auto stack_is_default = [&](const char *what, ID3D12StateObject *state) {
    if (!no_size(state))
      return false;
    UINT64 now, want = stack(state, now);
    if (now != want || !want) {
      printf("failed: the pipeline stack size of %s is %llu, want %llu\n", what, (unsigned long long)now, (unsigned long long)want);
      return false;
    }
    return true;
  };
  UINT64 first_stack, grown_stack;
  stack(pipeline.Get(), first_stack);
  stack(grown.Get(), grown_stack);
  if (!stack_is_default("the pipeline", pipeline.Get()) || !stack_is_default("the pipeline of a collection", collected.Get()))
    return 1;
  if (grown_stack != first_stack) {
    printf("failed: a grown pipeline's stack size is %llu, not %llu\n", (unsigned long long)grown_stack, (unsigned long long)first_stack);
    return 1;
  }

  CHECK(trace(pipeline.Get()));
  compare("pipeline", 2, 0, case_count);
  CHECK(trace(collected.Get()));
  compare("of a collection", 2, 0, 1);
  // the grown pipeline has the added miss shader; the identifiers the first pipeline gave are still its own
  ComPtr<ID3D12StateObjectProperties> first_properties, grown_properties;
  CHECK(pipeline.As(&first_properties));
  CHECK(grown.As(&grown_properties));
  auto first_identifier = first_properties->GetShaderIdentifier(L"opaque"),
       grown_identifier = grown_properties->GetShaderIdentifier(L"opaque");
  if (!first_identifier || !grown_identifier ||
      memcmp(first_identifier, grown_identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES) ||
      first_properties->GetShaderIdentifier(L"miss_added") || !grown_properties->GetShaderIdentifier(L"miss_added")) {
    printf("failed: a grown state object's identifiers\n");
    return 1;
  }
  cases[0].miss_index = 2;
  CHECK(trace(grown.Get()));
  compare("grown", 3, 0, 1);
  CHECK(trace(grown_with_collection.Get()));
  compare("grown with a collection's function", 3, 0, 1);
  cases[0].miss_index = 0;
  if (!stack_is_default("the pipeline of a collection that depends on it", of_dependent.Get()))
    return 1;
  CHECK(trace(of_dependent.Get()));
  compare("of a collection that depends on the pipeline", 2, 0, 2);

  // state objects of a library's own subobjects
  D3D12_DXIL_LIBRARY_DESC own_desc{bytecode(own)}, own_swapped_desc{bytecode(own_swapped)}, listed_desc{bytecode(own_swapped)};
  // every shader, and the subobjects but the second local root signature and the association with it
  std::vector<D3D12_EXPORT_DESC> listed;
  for (auto name : {L"raygen", L"miss", L"miss_other", L"closest", L"closest_box", L"any_hit", L"any_hit_box",
                    L"intersection", L"call_add", L"call_scale", L"global_rs", L"local_rs", L"opaque", L"alpha",
                    L"boxes", L"shader_config", L"pipeline_config"})
    listed.push_back({name});
  listed_desc.NumExports = listed.size();
  listed_desc.pExports = listed.data();
  const wchar_t *miss_only[] = {L"miss"};
  D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION named{L"swapped_rs", 1, miss_only};
  D3D12_LOCAL_ROOT_SIGNATURE declared{swapped_rs.Get()};
  auto all = [](const std::wstring &name) { return true; };
  auto none = [](const std::wstring &name) { return false; };
  const struct {
    const char *what;
    std::vector<D3D12_STATE_SUBOBJECT> subobjects;
    std::function<bool(const std::wstring &)> swapped;
  } own_cases[] = {
      {"a library's subobjects", {{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &own_desc}}, none},
      {"a library's association", {{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &own_swapped_desc}}, all},
      {"a library's listed subobjects", {{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &listed_desc}}, none},
      {"an association with a library's subobject",
       {{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &own_desc},
        {D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &named}},
       [](const std::wstring &name) { return name == L"miss"; }},
      {"a root signature over a library's",
       {{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &own_desc}, {D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &declared}},
       all},
  };
  // a pipeline of the subobjects, traced with the first miss shader, then the second
  auto traced = [&](const char *what, const std::vector<D3D12_STATE_SUBOBJECT> &of, std::function<bool(const std::wstring &)> which) {
    ComPtr<ID3D12StateObject> state;
    if (FAILED(state_object(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, of, state))) {
      printf("failed: no state object of %s\n", what);
      return false;
    }
    if (!stack_is_default(what, state.Get()))
      return false;
    swapped = which;
    if (FAILED(trace(state.Get())))
      return false;
    compare(what, 2, 0, 2);
    return true;
  };
  for (auto &own_case : own_cases)
    if (!traced(own_case.what, own_case.subobjects, own_case.swapped))
      return 1;

  // collections that leave their shaders to the pipeline, with associations of their own. the library's subobjects
  // without its second local root signature and its association: its shaders get its first by default
  auto collection_of = [&](const std::vector<D3D12_STATE_SUBOBJECT> &of) {
    ComPtr<ID3D12StateObject> made;
    state_object(D3D12_STATE_OBJECT_TYPE_COLLECTION, of, made);
    return made;
  };
  auto miss_swapped = [](const std::wstring &name) { return name == L"miss"; };
  // an association with a subobject that only a library of the pipeline defines
  auto unresolved = collection_of({{D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &depending},
                                   {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &listed_desc},
                                   {D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &named}});
  D3D12_EXPORT_DESC swapped_only{L"swapped_rs"};
  D3D12_DXIL_LIBRARY_DESC swapped_only_desc{bytecode(own), 1, &swapped_only};
  // an association with a shader that the pipeline takes as `miss`
  auto inner_names = listed;
  for (auto &taken : inner_names)
    if (!wcscmp(taken.Name, L"miss"))
      taken = {L"miss_inner", L"miss"};
  D3D12_DXIL_LIBRARY_DESC inner_desc{bytecode(own_swapped), (UINT)inner_names.size(), inner_names.data()};
  const wchar_t *miss_inner[] = {L"miss_inner"};
  std::vector<D3D12_STATE_SUBOBJECT> renamed_of = {{D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &depending},
                                                   {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &inner_desc},
                                                   {D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &declared},
                                                   {}};
  D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION inner_association{&renamed_of[2], 1, miss_inner};
  renamed_of.back() = {D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &inner_association};
  auto renamed = collection_of(renamed_of);
  auto outer_names = listed;
  for (auto &taken : outer_names)
    if (!wcscmp(taken.Name, L"miss"))
      taken = {L"miss", L"miss_inner"};
  // a library's own association, and a library with none
  auto associated = collection_of({{D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &depending},
                                   {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &own_swapped_desc}});
  auto defaulted = collection_of({{D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &depending},
                                  {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &listed_desc}});
  // a library's subobjects alone: the second local root signature and the shader config, which the pipeline
  // associates with a shader too
  D3D12_EXPORT_DESC subobjects_only[] = {{L"swapped_rs"}, {L"shader_config"}};
  D3D12_DXIL_LIBRARY_DESC subobjects_only_desc{bytecode(own), (UINT)std::size(subobjects_only), subobjects_only};
  auto subobject_only = collection_of({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &subobjects_only_desc}});
  const wchar_t *raygen_only[] = {L"raygen"};
  D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION configured{L"shader_config", 1, raygen_only};
  D3D12_EXISTING_COLLECTION_DESC existing_subobject{subobject_only.Get()};
  if (!unresolved || !renamed || !associated || !defaulted || !subobject_only) {
    printf("failed: no collection that leaves its shaders to the pipeline\n");
    return 1;
  }
  D3D12_EXISTING_COLLECTION_DESC existing_unresolved{unresolved.Get()},
      existing_renamed{renamed.Get(), (UINT)outer_names.size(), outer_names.data()}, existing_associated{associated.Get()},
      existing_defaulted{defaulted.Get()};
  const D3D12_STATE_SUBOBJECT configs[] = {{D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader_config},
                                           {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipeline_config}};
  // the pipeline's own association with a collection's shader: of the first root signature for the conflict, of the
  // second where the collection has only a default
  auto with_association = [&](D3D12_EXISTING_COLLECTION_DESC &taken, D3D12_LOCAL_ROOT_SIGNATURE &root,
                              D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION &made) {
    std::vector<D3D12_STATE_SUBOBJECT> of = {{D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &taken}, configs[0], configs[1],
                                             {D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &root},
                                             {D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &made}};
    return of;
  };
  D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION conflicting{nullptr, 1, miss_only}, standing{nullptr, 1, miss_only};
  auto conflict_of = with_association(existing_associated, local, conflicting),
       standing_of = with_association(existing_defaulted, declared, standing);
  conflicting.pSubobjectToAssociate = &conflict_of[3];
  standing.pSubobjectToAssociate = &standing_of[3];
  if (!traced("a collection's association with a subobject of the pipeline's library",
              {{D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &existing_unresolved},
               {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &swapped_only_desc}, configs[0], configs[1]},
              miss_swapped) ||
      !traced("a collection's association with an export taken under another name",
              {{D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &existing_renamed}, configs[0], configs[1]}, miss_swapped) ||
      !traced("a collection's association under a root signature the pipeline declares",
              {{D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &existing_associated}, configs[0], configs[1],
               {D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &local}},
              all) ||
      !traced("the pipeline's association with a shader its collection gave a default", standing_of, miss_swapped) ||
      !traced("the pipeline's association with a subobject of another collection's library",
              {{D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &existing_defaulted},
               {D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &existing_subobject},
               {D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &named},
               {D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &configured}, configs[0], configs[1]},
              miss_swapped))
    return 1;
  ComPtr<ID3D12StateObject> conflicted;
  if (SUCCEEDED(state_object(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, conflict_of, conflicted))) {
    printf("failed: a pipeline whose association conflicts with its collection's was made\n");
    return 1;
  }

  if (!scene.clear(margin)) {
    printf("failed: a ray is %g from an edge\n", margin);
    return 1;
  }
  if (failures) {
    printf("failed: %u wrong rays\n", failures);
    return 1;
  }
  if (!marks_checked) {
    printf("failed: no ray crosses a box\n");
    return 1;
  }
  printf("passed: %u hits of %u rays\n", hits, (case_count + 5 + 2 * ((UINT)std::size(own_cases) + 5)) * rays);
  return 0;
}
