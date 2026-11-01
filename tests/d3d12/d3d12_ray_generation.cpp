// contract: DispatchRays runs the ray generation shader that its RayGenerationShaderRecord names, with that record's
// local root arguments, once for each index of its Width x Height x Depth grid (DXR functional spec, "DispatchRays",
// "Shader record" and "Local root signatures and shader tables",
// https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html). shader tables are GPU memory, which the
// application writes from the CPU or with GPU commands ("Shader tables"), and the GPU reads the record when the
// dispatch runs: what the record holds when the list is recorded does not matter. ExecuteIndirect's
// D3D12_DISPATCH_RAYS_DESC names its record the same way ("CreateCommandSignature Structures").
// a state object with three ray generation shaders, each tracing a ray per index into one quad, runs (the third's
// misses take a miss shader that traces once more, which needs a recursion depth of 2 ("Ray recursion limit"), beside
// the plain miss shader of the others and of that second ray):
// - a record in an upload heap;
// - a record rewritten, to name another shader, after the list was recorded and before it runs;
// - a record a copy in the same list writes into a default heap, over a record of another shader;
// - two dispatches in one list naming different shaders, then two indirect dispatches doing the same;
// each with a grid that is no whole number of SIMD groups in x, and a depth of 2. a shader writes its own code, its
// record's root constant and what its ray met, at its index; every index starts with a value none writes.
#include "d3d12_test.hpp"
#include <algorithm>

static const char hlsl[] = R"hlsl(
RaytracingAccelerationStructure scene : register(t0);
RWStructuredBuffer<uint> results : register(u0);
cbuffer Record : register(b0, space1) { uint tag; };
struct Payload { uint met; };
uint traced(uint miss) {
  uint3 at = DispatchRaysIndex();
  RayDesc ray = { float3(at.x + 0.5, at.y + 0.5, -1), 0, float3(0, 0, 1), 10 };
  Payload p = { 0 };
  TraceRay(scene, RAY_FLAG_NONE, 0xff, 0, 1, miss, ray, p);
  return p.met;
}
uint index() {
  uint3 at = DispatchRaysIndex(), size = DispatchRaysDimensions();
  return (at.z * size.y + at.y) * size.x + at.x;
}
[shader("raygeneration")] void first() { results[index()] = 0x1000000 | tag << 8 | traced(0); }
[shader("raygeneration")] void second() { results[index()] = 0x2000000 | tag << 8 | traced(0); }
[shader("raygeneration")] void third() { results[index()] = 0x3000000 | tag << 8 | traced(1); }
[shader("miss")] void missed(inout Payload p) { p.met = 1; }
// back along the ray from z = 5, missing the quad again
[shader("miss")] void missed_again(inout Payload p) {
  RayDesc ray = { float3(WorldRayOrigin().xy, 5), 0, float3(0, 0, -1), 10 };
  Payload q = { 0 };
  TraceRay(scene, RAY_FLAG_NONE, 0xff, 0, 1, 0, ray, q);
  p.met = 16 + q.met;
}
[shader("closesthit")]
void hit(inout Payload p, in BuiltInTriangleIntersectionAttributes a) { p.met = 2 + uint(RayTCurrent()); }
)hlsl";

// the grid, and the quad: x from 0 to `quad_x`, y from -1 to 10, at z = 2, which a ray from z = -1 meets at t = 3.
// rays pass through pixel centres, clear of its edges and of the diagonal between its triangles
static constexpr UINT width = 37, height = 3, depth = 2, rays = width * height * depth;
static constexpr float quad_x = 20;

static UINT
expected(UINT shader, UINT tag, UINT i) {
  UINT x = i % width;
  return shader << 24 | tag << 8 | (x + 0.5f < quad_x ? 2 + 3 : shader == 3 ? 16 + 1 : 1);
}

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler) || !compiler.dxc) {
    printf("skipped: ray tracing libraries need DXC\n");
    return 77;
  }
  step("compile the library");
  auto library = compiler.compile(hlsl, "", "lib_6_3");
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
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList4> list;
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));

  step("build the acceleration structures");
  struct Inputs {
    float vertices[6][3];
    D3D12_RAYTRACING_INSTANCE_DESC instance;
  } *inputs;
  auto input = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(Inputs), D3D12_RESOURCE_STATE_GENERIC_READ);
  CHECK(input->Map(0, nullptr, (void **)&inputs));
  const float quad[6][3] = {{0, -1, 2}, {0, 10, 2}, {quad_x, -1, 2}, {quad_x, -1, 2}, {0, 10, 2}, {quad_x, 10, 2}};
  memcpy(inputs->vertices, quad, sizeof(quad));
  D3D12_RAYTRACING_GEOMETRY_DESC geometry{D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES, D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE};
  geometry.Triangles = {0, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R32G32B32_FLOAT, 0, 6, 0,
                        {input->GetGPUVirtualAddress() + offsetof(Inputs, vertices), sizeof(float[3])}};
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bottom{
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE,
      1, D3D12_ELEMENTS_LAYOUT_ARRAY
  };
  bottom.pGeometryDescs = &geometry;
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS top{
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE, 1,
      D3D12_ELEMENTS_LAYOUT_ARRAY
  };
  top.InstanceDescs = input->GetGPUVirtualAddress() + offsetof(Inputs, instance);
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO bottom_info{}, top_info{};
  device->GetRaytracingAccelerationStructurePrebuildInfo(&bottom, &bottom_info);
  device->GetRaytracingAccelerationStructurePrebuildInfo(&top, &top_info);
  auto structure = [&](UINT64 size) {
    return buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, size, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  };
  auto blas = structure(bottom_info.ResultDataMaxSizeInBytes), tlas = structure(top_info.ResultDataMaxSizeInBytes);
  auto scratch = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT,
                        std::max(bottom_info.ScratchDataSizeInBytes, top_info.ScratchDataSizeInBytes),
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  if (!expect(blas && tlas && scratch, "acceleration structure memory"))
    return verdict();
  inputs->instance = {};
  inputs->instance.Transform[0][0] = inputs->instance.Transform[1][1] = inputs->instance.Transform[2][2] = 1;
  inputs->instance.InstanceMask = 0xff;
  inputs->instance.AccelerationStructure = blas->GetGPUVirtualAddress();
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{blas->GetGPUVirtualAddress(), bottom, 0,
                                                           scratch->GetGPUVirtualAddress()};
  list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
  D3D12_RESOURCE_BARRIER uav{D3D12_RESOURCE_BARRIER_TYPE_UAV};
  list->ResourceBarrier(1, &uav);
  build = {tlas->GetGPUVirtualAddress(), top, 0, scratch->GetGPUVirtualAddress()};
  list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
  list->ResourceBarrier(1, &uav);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  step("create the state object");
  D3D12_ROOT_PARAMETER global_parameters[2]{{D3D12_ROOT_PARAMETER_TYPE_SRV}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  auto global_rs = root_signature(device.Get(), {2, global_parameters});
  D3D12_ROOT_PARAMETER local_parameter{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
  local_parameter.Constants = {0, 1, 1};
  auto local_rs = root_signature(device.Get(), {1, &local_parameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE});
  if (!expect(global_rs && local_rs, "root signatures"))
    return verdict();
  D3D12_DXIL_LIBRARY_DESC code{bytecode(library)};
  D3D12_HIT_GROUP_DESC group{L"group", D3D12_HIT_GROUP_TYPE_TRIANGLES, nullptr, L"hit"};
  D3D12_RAYTRACING_SHADER_CONFIG shader_config{sizeof(UINT), sizeof(float) * 2};
  D3D12_RAYTRACING_PIPELINE_CONFIG pipeline_config{2};
  D3D12_GLOBAL_ROOT_SIGNATURE global{global_rs.Get()};
  D3D12_LOCAL_ROOT_SIGNATURE local{local_rs.Get()};
  D3D12_STATE_SUBOBJECT subobjects[] = {
      {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &code},
      {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &group},
      {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader_config},
      {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipeline_config},
      {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global},
      {D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &local},
  };
  const WCHAR *generators[] = {L"first", L"second", L"third"};
  D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION association{&subobjects[5], 3, generators};
  D3D12_STATE_SUBOBJECT all[std::size(subobjects) + 1];
  std::copy(std::begin(subobjects), std::end(subobjects), all);
  all[std::size(subobjects)] = {D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &association};
  D3D12_STATE_OBJECT_DESC desc{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, (UINT)std::size(all), all};
  ComPtr<ID3D12StateObject> state;
  CHECK(device->CreateStateObject(&desc, IID_PPV_ARGS(&state)));
  ComPtr<ID3D12StateObjectProperties> properties;
  CHECK(state.As(&properties));
  const void *identifiers[3];
  for (UINT s = 0; s < 3; s++)
    if (!expect(!!(identifiers[s] = properties->GetShaderIdentifier(generators[s])), "identifier of shader %u", s + 1))
      return verdict();
  auto miss_identifier = properties->GetShaderIdentifier(L"missed"),
       again_identifier = properties->GetShaderIdentifier(L"missed_again"),
       group_identifier = properties->GetShaderIdentifier(L"group");
  if (!expect(miss_identifier && again_identifier && group_identifier, "miss and hit group identifiers"))
    return verdict();

  // the tables: four ray generation records, then the miss shaders' and the hit group's
  struct Record {
    uint8_t identifier[D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES];
    UINT tag;
    uint8_t padding[D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT - D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES - sizeof(UINT)];
  };
  enum { Generators = 4, Miss = Generators, MissAgain, Group, Records };
  auto record = [&](Record &r, const void *identifier, UINT tag) {
    r = {};
    memcpy(r.identifier, identifier, sizeof(r.identifier));
    r.tag = tag;
  };
  auto tables = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, Records * sizeof(Record), D3D12_RESOURCE_STATE_GENERIC_READ);
  // where a copy writes a record: a default heap, and the record it copies from
  auto copied = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, sizeof(Record), D3D12_RESOURCE_STATE_COPY_DEST);
  auto source = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 2 * sizeof(Record), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto results = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, 2 * rays * sizeof(UINT), D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto initial = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 2 * rays * sizeof(UINT), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 2 * rays * sizeof(UINT), D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_INDIRECT_ARGUMENT_DESC argument{D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS};
  D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(D3D12_DISPATCH_RAYS_DESC), 1, &argument};
  ComPtr<ID3D12CommandSignature> signature;
  CHECK(device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signature)));
  auto commands = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 2 * sizeof(D3D12_DISPATCH_RAYS_DESC),
                         D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(tables && copied && source && results && initial && readback && signature && commands, "buffers"))
    return verdict();
  Record *table, *sources;
  UINT *start, *out;
  D3D12_DISPATCH_RAYS_DESC *command;
  CHECK(tables->Map(0, nullptr, (void **)&table));
  CHECK(source->Map(0, nullptr, (void **)&sources));
  CHECK(initial->Map(0, nullptr, (void **)&start));
  CHECK(readback->Map(0, nullptr, (void **)&out));
  CHECK(commands->Map(0, nullptr, (void **)&command));
  for (UINT i = 0; i < 2 * rays; i++)
    start[i] = 0xdeadbeef;
  record(table[Miss], miss_identifier, 0);
  record(table[MissAgain], again_identifier, 0);
  record(table[Group], group_identifier, 0);
  auto at = [&](UINT r) { return tables->GetGPUVirtualAddress() + r * sizeof(Record); };
  auto dispatch = [&](D3D12_GPU_VIRTUAL_ADDRESS generator) {
    return D3D12_DISPATCH_RAYS_DESC{{generator, sizeof(Record)},
                                    {at(Miss), 2 * sizeof(Record), sizeof(Record)},
                                    {at(Group), sizeof(Record), sizeof(Record)},
                                    {},
                                    width,
                                    height,
                                    depth};
  };
  // records the list: the results reset, then `recorded` with the state bound, then the results read back
  auto run = [&](auto recorded, auto before_running) -> HRESULT {
    memset(out, 0xff, 2 * rays * sizeof(UINT));
    HRESULT hr;
    if (FAILED(hr = allocator->Reset()) || FAILED(hr = list->Reset(allocator.Get(), nullptr)))
      return hr;
    list->CopyResource(results.Get(), initial.Get());
    transition(list.Get(), results.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetComputeRootSignature(global_rs.Get());
    list->SetComputeRootShaderResourceView(0, tlas->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(1, results->GetGPUVirtualAddress());
    list->SetPipelineState1(state.Get());
    recorded();
    transition(list.Get(), results.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(readback.Get(), results.Get());
    if (FAILED(hr = list->Close()))
      return hr;
    before_running();
    return execute(device.Get(), queue.Get(), list.Get());
  };
  auto nothing = [] {};
  // the results of the first half, then of the second (0 for none: still as they started)
  auto check = [&](UINT shader, UINT tag, UINT second_shader = 0, UINT second_tag = 0) {
    UINT wrong = 0;
    for (UINT half = 0; half < 2; half++)
      for (UINT i = 0; i < rays; i++) {
        UINT s = half ? second_shader : shader, t = half ? second_tag : tag;
        UINT want = s ? expected(s, t, i) : 0xdeadbeef, got = out[half * rays + i];
        if (got != want && wrong++ < 8)
          expect(false, "half %u, index %u (%u, %u, %u): 0x%08x, not 0x%08x", half, i, i % width,
                 i / width % height, i / (width * height), got, want);
      }
    if (wrong > 8)
      expect(false, "%u more wrong indices", wrong - 8);
  };

  step("a record in an upload heap");
  record(table[0], identifiers[0], 0x11);
  CHECK(run([&] { auto d = dispatch(at(0)); list->DispatchRays(&d); }, nothing));
  check(1, 0x11);

  step("a record rewritten after the list was recorded");
  record(table[0], identifiers[0], 0x12);
  CHECK(run([&] { auto d = dispatch(at(0)); list->DispatchRays(&d); }, [&] { record(table[0], identifiers[1], 0x22); }));
  check(2, 0x22);

  step("a record copied in the list, over a record of another shader");
  record(sources[0], identifiers[0], 0x13);
  record(sources[1], identifiers[2], 0x33);
  for (UINT r = 0; r < 2; r++) {
    CHECK(run([&] {
      if (r)
        transition(list.Get(), copied.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
      list->CopyBufferRegion(copied.Get(), 0, source.Get(), r * sizeof(Record), sizeof(Record));
      transition(list.Get(), copied.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      auto d = dispatch(copied->GetGPUVirtualAddress());
      list->DispatchRays(&d);
    }, nothing));
    r ? check(3, 0x33) : check(1, 0x13);
  }

  step("two dispatches of different shaders in one list");
  record(table[1], identifiers[2], 0x34);
  record(table[2], identifiers[1], 0x25);
  CHECK(run([&] {
    auto d = dispatch(at(1));
    list->DispatchRays(&d);
    list->SetComputeRootUnorderedAccessView(1, results->GetGPUVirtualAddress() + rays * sizeof(UINT));
    d = dispatch(at(2));
    list->DispatchRays(&d);
  }, nothing));
  check(3, 0x34, 2, 0x25);

  step("two indirect dispatches of different shaders in one list");
  record(table[1], identifiers[1], 0x26);
  record(table[2], identifiers[0], 0x17);
  command[0] = dispatch(at(1));
  command[1] = dispatch(at(2));
  CHECK(run([&] {
    list->ExecuteIndirect(signature.Get(), 1, commands.Get(), 0, nullptr, 0);
    list->SetComputeRootUnorderedAccessView(1, results->GetGPUVirtualAddress() + rays * sizeof(UINT));
    list->ExecuteIndirect(signature.Get(), 1, commands.Get(), sizeof(D3D12_DISPATCH_RAYS_DESC), nullptr, 0);
  }, nothing));
  check(2, 0x26, 1, 0x17);
  return verdict();
}
