// contract: one stage may supply a pipeline's embedded root signature; signatures supplied by other stages must
// agree, and all resources must be covered. an explicit signature overrides the embedded ones and must cover the
// shaders. Microsoft Learn, Creating a Root Signature, "Root Signature in Pipeline State Objects": "they must
// match", "for compatibility", and "This will override any root
// signature already in the shaders." vkd3d-proton's test_root_signature_embedded witnesses that stages without a
// signature can use another stage's. all combinations below derive their HRESULT from agreement and coverage.
#include "d3d12_test.hpp"
#include <array>
#include "../../libs/DXBCParser/BlobContainer.h"
#include "../../src/airconv/airconv_public.h"

static decltype(&SM50GetErrorMessage) original_error;

static size_t
check_error(sm50_error_t error, char *message, size_t size) {
  const sm50_error_t none{};
  expect(memcmp(&error, &none, sizeof(error)), "the compiler faulted instead of rejecting the signature");
  return original_error(error, message, size);
}

// a recovered unix compiler fault has no error handle (SM50GetErrorMessage in airconv_thunks.c).
static bool
check_errors() {
  auto bridge = GetModuleHandleA("winemetal.dll");
  if (!bridge || !GetProcAddress(bridge, "SM50GetErrorMessage"))
    return true;
  auto module = (BYTE *)GetModuleHandleA("d3d12core.dll");
  if (!expect(module != nullptr, "no d3d12core module"))
    return false;
  auto dos = (IMAGE_DOS_HEADER *)module;
  auto nt = (IMAGE_NT_HEADERS *)(module + dos->e_lfanew);
  auto imports =
      (IMAGE_IMPORT_DESCRIPTOR *)(module +
                                  nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
  for (; imports->Name; imports++) {
    auto names = (IMAGE_THUNK_DATA *)(module + imports->OriginalFirstThunk);
    auto functions = (IMAGE_THUNK_DATA *)(module + imports->FirstThunk);
    for (; names->u1.AddressOfData; names++, functions++) {
      if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
        continue;
      auto name = (IMAGE_IMPORT_BY_NAME *)(module + names->u1.AddressOfData);
      if (strcmp((const char *)name->Name, "SM50GetErrorMessage"))
        continue;
      DWORD protection;
      if (!expect(
              VirtualProtect(&functions->u1.Function, sizeof(functions->u1.Function), PAGE_READWRITE, &protection),
              "cannot inspect compiler errors"
          ))
        return false;
      original_error = reinterpret_cast<decltype(original_error)>(functions->u1.Function);
      functions->u1.Function = uintptr_t(check_error);
      return expect(
          VirtualProtect(&functions->u1.Function, sizeof(functions->u1.Function), protection, &protection),
          "cannot restore import protection"
      );
    }
  }
  return expect(false, "no SM50GetErrorMessage import");
}

static const char hlsl[] = R"hlsl(
#define JOIN_(a, b) a##b
#define JOIN(a, b) JOIN_(a, b)
RWStructuredBuffer<uint> first : register(u0, JOIN(space, SPACE));
RWStructuredBuffer<uint> second : register(u1, JOIN(space, SPACE));
struct V { float4 pos : SV_Position; };
#if EMBED
[RootSignature(ROOT)]
#endif
V vs() { first[0] = 1; second[0] = 2; V v; v.pos = float4(0, 0, 0, 1); return v; }
#if EMBED
[RootSignature(ROOT)]
#endif
[maxvertexcount(1)]
void gs(point V vertices[1], inout PointStream<V> stream) {
  first[0] = 3; second[0] = 4; stream.Append(vertices[0]);
}
#if EMBED
[RootSignature(ROOT)]
#endif
[maxvertexcount(1)]
void gs_empty(point V vertices[1]) { first[0] = 3; second[0] = 4; }
[maxvertexcount(1)]
void gs_triangle(triangle V vertices[3], inout PointStream<V> stream) {
  first[0] = 3; second[0] = 4; stream.Append(vertices[0]);
}
#if EMBED
[RootSignature(ROOT)]
#endif
void ps() { first[0] = 5; second[0] = 6; }
#if EMBED
[RootSignature(ROOT)]
#endif
[numthreads(1, 1, 1)]
void cs() { first[0] = 7; second[0] = 8; }
struct CP { float4 pos : POSITION; };
struct Factors { float edges[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };
Factors factors(InputPatch<V, 1> vertices) {
  Factors f; f.edges[0] = f.edges[1] = f.edges[2] = f.inside = 1; return f;
}
#if EMBED
[RootSignature(ROOT)]
#endif
[domain("tri")] [partitioning("integer")] [outputtopology("triangle_cw")]
[outputcontrolpoints(1)] [patchconstantfunc("factors")]
CP hs(InputPatch<V, 1> vertices, uint id : SV_OutputControlPointID) {
  first[0] = 9; second[0] = 10; CP v; v.pos = vertices[id].pos; return v;
}
#if EMBED
[RootSignature(ROOT)]
#endif
[domain("tri")]
V ds(Factors f, float3 location : SV_DomainLocation, const OutputPatch<CP, 1> vertices) {
  first[0] = 11; second[0] = 12; V v; v.pos = vertices[0].pos; return v;
}
)hlsl";

static const char mesh_hlsl[] = R"hlsl(
#define JOIN_(a, b) a##b
#define JOIN(a, b) JOIN_(a, b)
RWStructuredBuffer<uint> first : register(u0, JOIN(space, SPACE));
RWStructuredBuffer<uint> second : register(u1, JOIN(space, SPACE));
struct V { float4 pos : SV_Position; };
struct Payload { float4 pos; };
#if EMBED
[RootSignature(ROOT)]
#endif
[numthreads(1, 1, 1)]
void as() {
  first[0] = 13; second[0] = 14;
  Payload payload; payload.pos = float4(0, 0, 0, 1); DispatchMesh(1, 1, 1, payload);
}
#if EMBED
[RootSignature(ROOT)]
#endif
[numthreads(1, 1, 1)] [outputtopology("triangle")]
void ms(in payload Payload payload, out vertices V verticesOut[3], out indices uint3 indicesOut[1]) {
  first[0] = 15; second[0] = 16; SetMeshOutputCounts(3, 1);
  for (uint id = 0; id < 3; id++) verticesOut[id].pos = payload.pos;
  indicesOut[0] = uint3(0, 1, 2);
}
#if EMBED
[RootSignature(ROOT)]
#endif
[numthreads(1, 1, 1)] [outputtopology("triangle")]
void ms_plain(out vertices V verticesOut[3], out indices uint3 indicesOut[1]) {
  first[0] = 15; second[0] = 16; SetMeshOutputCounts(3, 1);
  for (uint id = 0; id < 3; id++) verticesOut[id].pos = float4(0, 0, 0, 1);
  indicesOut[0] = uint3(0, 1, 2);
}
)hlsl";

// DXR functional specification, D3D12_GLOBAL_ROOT_SIGNATURE: "The combination of global and/or local root
// signatures associated with any given shader function must define all resource bindings declared by the shader".
static const char ray_hlsl[] = R"hlsl(
RWStructuredBuffer<uint> result : register(u0);
cbuffer Record : register(b0) { uint value; };
[shader("raygeneration")] void raygen() { result[0] = value; }
)hlsl";

static const char resources_hlsl[] = R"hlsl(
Texture2D<float> image : register(t0);
SamplerState state : register(s0);
cbuffer Values : register(b0) { uint value; };
RWStructuredBuffer<uint> result : register(u0);
[numthreads(1, 1, 1)]
void cs() { result[0] = value + uint(image.SampleLevel(state, float2(0, 0), 0)); }
)hlsl";

template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename T> struct alignas(void *) Subobject {
  D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
  T data{};
};

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  // a stage's variants: naked or embedded, in each of two distinct register spaces
  const std::array stages = {"vs", "gs", "ps", "cs", "hs", "ds", "as_6_5", "ms_6_5"};
  std::array<std::array<std::string, 4>, stages.size()> code;
  decltype(code)::value_type plain_mesh;
  for (size_t stage = 0; stage < stages.size(); stage++)
    for (size_t variant = 0; variant < code[stage].size(); variant++) {
      if (stage >= 6 && !compiler.dxc)
        continue;
      const auto space = std::to_string(variant % 2);
      std::vector<std::string> defines = {
          "EMBED=" + std::to_string(variant / 2), "SPACE=" + space,
          "ROOT=\"UAV(u0, space=" + space + "), UAV(u1, space=" + space + ")\""
      };
      code[stage][variant] = stage < 6 ? compiler.compile(hlsl, stages[stage], stages[stage], defines)
                                     : compiler.compile(mesh_hlsl, stage == 6 ? "as" : "ms", stages[stage], defines);
      if (stage == 7)
        plain_mesh[variant] = compiler.compile(mesh_hlsl, "ms_plain", stages[stage], defines);
      if (code[stage][variant].empty() || (stage == 7 && plain_mesh[variant].empty())) {
        printf("failed: %s variant %zu HLSL did not compile\n", stages[stage], variant);
        return 1;
      }
    }
  auto longer = compiler.compile(hlsl, "ps", "ps", {"EMBED=1", "SPACE=0", "ROOT=\"UAV(u0), UAV(u1), UAV(u2)\""});
  auto swapped = compiler.compile(hlsl, "ps", "ps", {"EMBED=1", "SPACE=0", "ROOT=\"UAV(u1), UAV(u0)\""});
  auto empty_geometry = compiler.compile(hlsl, "gs_empty", "gs", {"EMBED=1", "SPACE=0", "ROOT=\"UAV(u0), UAV(u1)\""});
  auto resources = compiler.compile(resources_hlsl, "cs", "cs");
  auto triangle_geometry = compiler.compile(hlsl, "gs_triangle", "gs", {"EMBED=0", "SPACE=0"});
  auto ray = compiler.dxc ? compiler.compile(ray_hlsl, "", "lib_6_3") : std::string{};
  if (longer.empty() || swapped.empty() || empty_geometry.empty() || triangle_geometry.empty() || resources.empty() ||
      (compiler.dxc && ray.empty())) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12Device2> device2;
  CHECK(device.As(&device2));
  if (!check_errors())
    return verdict();
  std::array<ComPtr<ID3D12RootSignature>, 2> roots;
  for (UINT space = 0; space < roots.size(); space++) {
    D3D12_ROOT_PARAMETER parameters[2] = {{D3D12_ROOT_PARAMETER_TYPE_UAV}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
    for (UINT i = 0; i < std::size(parameters); i++)
      parameters[i].Descriptor = {i, space};
    roots[space] = root_signature(device.Get(), {(UINT)std::size(parameters), parameters});
    CHECK(roots[space] ? S_OK : E_FAIL);
  }
  for (auto root : std::array<ID3D12RootSignature *, 2>{nullptr, roots[0].Get()})
    for (auto length : {size_t(0), sizeof(microsoft::DXBCHeader) - 1, sizeof(microsoft::DXBCHeader)}) {
      step("null compute bytecode, length=%zu explicit=%d", length, root != nullptr);
      D3D12_COMPUTE_PIPELINE_STATE_DESC desc{root, {nullptr, length}};
      ComPtr<ID3D12PipelineState> pipeline;
      HRESULT hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline));
      expect(hr == E_INVALIDARG && !pipeline, "HRESULT %08lx, want E_INVALIDARG; pipeline=%d", hr, !!pipeline);
    }
  auto padded_compute = code[3][2] + '\0';
  for (auto root : std::array<ID3D12RootSignature *, 2>{nullptr, roots[0].Get()})
    for (size_t length : {sizeof(microsoft::DXBCHeader) - 1, sizeof(microsoft::DXBCHeader), code[3][2].size() - 1,
                         code[3][2].size(), padded_compute.size()}) {
      step("compute container length=%zu actual=%zu explicit=%d", length, code[3][2].size(), root != nullptr);
      D3D12_COMPUTE_PIPELINE_STATE_DESC desc{root, {padded_compute.data(), length}};
      ComPtr<ID3D12PipelineState> pipeline;
      HRESULT hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline));
      const HRESULT want = length == code[3][2].size() ? S_OK : E_INVALIDARG;
      expect(
          hr == want && !!pipeline == (want == S_OK), "HRESULT %08lx, want %08lx; pipeline=%d", hr, want, !!pipeline
      );
    }
  for (size_t stage = 4; stage < stages.size(); stage++) {
    if (stage >= 6 && !compiler.dxc)
      continue;
    for (int explicit_root = 0; explicit_root < 2; explicit_root++)
      for (size_t variant = 0; variant < code[stage].size(); variant++)
        for (int other = 0; other < 4; other++)
          for (int alternate = 0; alternate < (stage == 6 ? 1 : 2); alternate++) {
            step("supplier=%s variant=%zu other=%d explicit=%d alternate=%d", stages[stage], variant, other,
                 explicit_root, alternate);
            D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = explicit_root ? roots[0].Get() : nullptr;
            desc.VS = bytecode(code[0][other == 1 ? 2 : 0]);
            desc.HS = bytecode(code[4][stage == 4 ? variant : 0]);
            desc.DS = bytecode(code[5][stage == 5 ? variant : 0]);
            desc.GS = stage < 6 && alternate ? bytecode(triangle_geometry) : D3D12_SHADER_BYTECODE{};
            desc.PS =
                bytecode(other >= 2 ? (other == 2 ? longer : swapped) : code[2][stage >= 6 && other == 1 ? 2 : 0]);
            desc.SampleMask = ~0u;
            desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
            desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
            desc.SampleDesc = {1, 0};
            ComPtr<ID3D12PipelineState> pipeline;
            HRESULT hr;
            if (stage < 6) {
              hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
            } else {
              struct {
                Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> root;
                Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, D3D12_SHADER_BYTECODE> amplification;
                Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> mesh;
                Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> pixel;
                Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> raster;
                Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC> depth;
              } stream;
              stream.root.data = desc.pRootSignature;
              stream.amplification.data =
                  alternate ? D3D12_SHADER_BYTECODE{} : bytecode(code[6][stage == 6 ? variant : 0]);
              stream.mesh.data = bytecode(alternate ? plain_mesh[variant] : code[7][stage == 7 ? variant : 0]);
              stream.pixel.data = desc.PS;
              stream.raster.data = desc.RasterizerState;
              D3D12_PIPELINE_STATE_STREAM_DESC stream_desc{sizeof(stream), &stream};
              hr = device2->CreatePipelineState(&stream_desc, IID_PPV_ARGS(&pipeline));
            }
            const bool compatible = variant % 2 == 0 && (explicit_root || other || variant >= 2) &&
                                    (explicit_root || other < 2 || variant < 2);
            const HRESULT want = compatible ? S_OK : E_INVALIDARG;
            expect(
                hr == want && !!pipeline == compatible, "HRESULT %08lx, want %08lx; pipeline=%d", hr, want, !!pipeline
            );
          }
  }
  for (int geometry = 0; geometry < 2; geometry++) {
    step("tessellated vertex resource outside the explicit signature, geometry=%d", geometry);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = roots[0].Get();
    desc.VS = bytecode(code[0][1]);
    desc.HS = bytecode(code[4][0]);
    desc.DS = bytecode(code[5][0]);
    desc.GS = geometry ? bytecode(triangle_geometry) : D3D12_SHADER_BYTECODE{};
    desc.PS = bytecode(code[2][0]);
    desc.SampleMask = ~0u;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    desc.SampleDesc = {1, 0};
    ComPtr<ID3D12PipelineState> pipeline;
    HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
    expect(hr == E_INVALIDARG && !pipeline, "HRESULT %08lx, want E_INVALIDARG; pipeline=%d", hr, !!pipeline);
  }
  step("a geometry shader that emits nothing supplies the root signature");
  D3D12_GRAPHICS_PIPELINE_STATE_DESC empty_desc{};
  empty_desc.VS = bytecode(code[0][0]);
  empty_desc.GS = bytecode(empty_geometry);
  empty_desc.PS = bytecode(code[2][0]);
  empty_desc.SampleMask = ~0u;
  empty_desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  empty_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
  empty_desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> empty_pipeline;
  HRESULT empty_hr = device->CreateGraphicsPipelineState(&empty_desc, IID_PPV_ARGS(&empty_pipeline));
  expect(empty_hr == S_OK && empty_pipeline, "HRESULT %08lx, want S_OK", empty_hr);
  for (auto other : {&longer, &swapped})
    for (int explicit_root = 0; explicit_root < 2; explicit_root++) {
      step("different embedded signatures, longer=%d explicit=%d", other == &longer, explicit_root);
      D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
      desc.pRootSignature = explicit_root ? roots[0].Get() : nullptr;
      desc.VS = bytecode(code[0][2]);
      desc.PS = bytecode(*other);
      desc.SampleMask = ~0u;
      desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
      desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
      desc.SampleDesc = {1, 0};
      ComPtr<ID3D12PipelineState> pipeline;
      HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
      HRESULT want = explicit_root ? S_OK : E_INVALIDARG;
      expect(hr == want, "HRESULT %08lx, want %08lx", hr, want);
    }
  for (int explicit_space = -1; explicit_space < (int)roots.size(); explicit_space++)
    for (size_t vs = 0; vs < code[0].size(); vs++)
      for (size_t gs = 0; gs <= code[1].size(); gs++)
        for (size_t ps = 0; ps < code[2].size(); ps++) {
          const bool geometry = gs < code[1].size();
          int selected = explicit_space;
          bool compatible = true;
          if (explicit_space < 0)
            for (auto variant : {vs, geometry ? gs : ps, ps})
              if (variant >= 2) {
                if (selected >= 0 && selected != (int)(variant % 2))
                  compatible = false;
                selected = variant % 2;
              }
          compatible &= selected >= 0 && selected == (int)(vs % 2) && selected == (int)(ps % 2) &&
                        (!geometry || selected == (int)(gs % 2));
          const HRESULT want = compatible ? S_OK : E_INVALIDARG;
          D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
          desc.pRootSignature = explicit_space < 0 ? nullptr : roots[explicit_space].Get();
          desc.VS = bytecode(code[0][vs]);
          desc.GS = geometry ? bytecode(code[1][gs]) : D3D12_SHADER_BYTECODE{};
          desc.PS = bytecode(code[2][ps]);
          desc.SampleMask = ~0u;
          desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
          desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
          desc.SampleDesc = {1, 0};
          struct {
            Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> root;
            Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE> vertex;
            Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS, D3D12_SHADER_BYTECODE> geometry;
            Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> pixel;
            Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> rasterizer;
            Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC> depth;
            Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE> topology;
          } stream;
          stream.root.data = desc.pRootSignature;
          stream.vertex.data = desc.VS;
          stream.geometry.data = desc.GS;
          stream.pixel.data = desc.PS;
          stream.rasterizer.data = desc.RasterizerState;
          stream.topology.data = desc.PrimitiveTopologyType;
          D3D12_PIPELINE_STATE_STREAM_DESC stream_desc{sizeof(stream), &stream};
          for (int api = 0; api < 2; api++) {
            step("graphics api=%d explicit=%d vs=%zu gs=%zu ps=%zu", api, explicit_space, vs, gs, ps);
            ComPtr<ID3D12PipelineState> pipeline;
            HRESULT hr = api ? device2->CreatePipelineState(&stream_desc, IID_PPV_ARGS(&pipeline))
                             : device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
            expect(
                hr == want && !!pipeline == compatible, "HRESULT %08lx, want %08lx; pipeline=%d", hr, want, !!pipeline
            );
          }
        }
  for (int explicit_space = -1; explicit_space < (int)roots.size(); explicit_space++)
    for (size_t variant = 0; variant < code[3].size(); variant++) {
      step("compute explicit=%d variant=%zu", explicit_space, variant);
      const bool compatible = explicit_space < 0 ? variant >= 2 : explicit_space == (int)(variant % 2);
      const HRESULT want = compatible ? S_OK : E_INVALIDARG;
      D3D12_COMPUTE_PIPELINE_STATE_DESC desc{
          explicit_space < 0 ? nullptr : roots[explicit_space].Get(), bytecode(code[3][variant])
      };
      ComPtr<ID3D12PipelineState> pipeline;
      HRESULT hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline));
      expect(hr == want && !!pipeline == compatible, "HRESULT %08lx, want %08lx; pipeline=%d", hr, want, !!pipeline);
    }
  constexpr UINT kinds = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER - D3D12_DESCRIPTOR_RANGE_TYPE_SRV + 1;
  for (UINT sampler = 0; sampler < 3; sampler++)
    for (UINT omitted = 0; omitted <= kinds; omitted++) {
      step(
          "compute resource coverage, omitted=%u (none=%u), sampler=%u (table, static, wrong space)", omitted, kinds,
          sampler
      );
      std::array<D3D12_DESCRIPTOR_RANGE, kinds> ranges{};
      std::vector<D3D12_ROOT_PARAMETER> parameters;
      for (UINT i = 0; i < ranges.size(); i++) {
        ranges[i] = {D3D12_DESCRIPTOR_RANGE_TYPE(D3D12_DESCRIPTOR_RANGE_TYPE_SRV + i), 1};
        if (i != omitted && !(sampler && ranges[i].RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER)) {
          D3D12_ROOT_PARAMETER parameter{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE};
          parameter.DescriptorTable = {1, &ranges[i]};
          parameters.push_back(parameter);
        }
      }
      D3D12_STATIC_SAMPLER_DESC state{};
      state.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
      state.AddressU = state.AddressV = state.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
      state.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
      state.MaxLOD = D3D12_FLOAT32_MAX;
      state.RegisterSpace = sampler == 2;
      auto root = root_signature(
          device.Get(), {(UINT)parameters.size(), parameters.data(), sampler ? 1u : 0u, sampler ? &state : nullptr}
      );
      CHECK(root ? S_OK : E_FAIL);
      D3D12_COMPUTE_PIPELINE_STATE_DESC desc{root.Get(), bytecode(resources)};
      ComPtr<ID3D12PipelineState> pipeline;
      HRESULT hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline));
      const bool compatible =
          sampler != 2 && (omitted == kinds || (sampler == 1 && omitted == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER));
      const HRESULT want = compatible ? S_OK : E_INVALIDARG;
      expect(hr == want, "HRESULT %08lx, want %08lx", hr, want);
    }
  if (compiler.dxc) {
    if (auto bridge = GetModuleHandleA("winemetal.dll")) {
      auto initialize = reinterpret_cast<decltype(&SM50Initialize)>(GetProcAddress(bridge, "SM50Initialize"));
      auto compile = reinterpret_cast<decltype(&SM50Compile)>(GetProcAddress(bridge, "SM50Compile"));
      auto destroy = reinterpret_cast<decltype(&SM50Destroy)>(GetProcAddress(bridge, "SM50Destroy"));
      auto free_error = reinterpret_cast<decltype(&SM50FreeError)>(GetProcAddress(bridge, "SM50FreeError"));
      auto free_bitcode = reinterpret_cast<decltype(&SM50DestroyBitcode)>(GetProcAddress(bridge, "SM50DestroyBitcode"));
      auto get_shader = reinterpret_cast<decltype(&SM50GetRayShader)>(GetProcAddress(bridge, "SM50GetRayShader"));
      CHECK(initialize && compile && destroy && free_error && free_bitcode && get_shader ? S_OK : E_FAIL);
      sm50_shader_t shader{};
      sm50_error_t error{};
      CHECK(initialize(ray.data(), ray.size(), &shader, nullptr, &error) == 0 ? S_OK : E_FAIL);
      char name[sizeof(trace::doing)];
      SM50_RAY_SHADER_INFO info{};
      CHECK(get_shader(shader, 0, name, sizeof(name), &info) ? S_OK : E_FAIL);
      SM50_SHADER_RAY_SHADER_DATA stage{nullptr, SM50_SHADER_RAY_SHADER, name};
      for (auto type : {SM50_SHADER_ROOT_SIGNATURE, SM50_SHADER_ROOT_SIGNATURE2}) {
        step("ray converter rejects a container without a signature, argument=%u", (UINT)type);
        SM50_SHADER_ROOT_SIGNATURE_DATA root{&stage, type, code[3][0].data(), code[3][0].size()};
        sm50_bitcode_t bitcode{};
        error = {};
        auto result =
            compile(shader, (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&root, "invalid_signature", &bitcode, &error);
        const sm50_error_t none{};
        const bool has_error = memcmp(&error, &none, sizeof(error));
        expect(result != 0 && has_error, "compiler returned %d, defined error=%d", result, has_error);
        if (has_error) {
          char message[sizeof(trace::doing)];
          auto length = original_error(error, message, sizeof(message));
          printf("compiler: %.*s\n", (int)length, message);
          expect(length != 0, "the compiler supplied no diagnostic");
          free_error(error);
        }
        if (bitcode)
          free_bitcode(bitcode);
      }
      destroy(shader);
    }
    step("ray generation bindings split between global UAV and local constant");
    ComPtr<ID3D12Device5> ray_device;
    CHECK(device.As(&ray_device));
    D3D12_ROOT_PARAMETER global_parameter{D3D12_ROOT_PARAMETER_TYPE_UAV};
    D3D12_ROOT_PARAMETER local_parameter{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
    local_parameter.Constants.Num32BitValues = 1;
    auto global_root = root_signature(device.Get(), {1, &global_parameter});
    auto local_root = root_signature(
        device.Get(), {1, &local_parameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE}
    );
    CHECK(global_root && local_root ? S_OK : E_FAIL);
    D3D12_DXIL_LIBRARY_DESC library{bytecode(ray)};
    D3D12_GLOBAL_ROOT_SIGNATURE global{global_root.Get()};
    D3D12_LOCAL_ROOT_SIGNATURE local{local_root.Get()};
    D3D12_RAYTRACING_SHADER_CONFIG shader_config{};
    D3D12_RAYTRACING_PIPELINE_CONFIG pipeline_config{1};
    D3D12_STATE_SUBOBJECT subobjects[] = {
        {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library},
        {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global},
        {D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &local},
        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader_config},
        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipeline_config}
    };
    D3D12_STATE_OBJECT_DESC desc{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, (UINT)std::size(subobjects), subobjects};
    ComPtr<ID3D12StateObject> pipeline;
    HRESULT hr = ray_device->CreateStateObject(&desc, IID_PPV_ARGS(&pipeline));
    expect(hr == S_OK && pipeline, "HRESULT %08lx, want S_OK; pipeline=%d", hr, !!pipeline);
    if (SUCCEEDED(hr) && pipeline) {
      ComPtr<ID3D12StateObjectProperties> properties;
      CHECK(pipeline.As(&properties));
      auto identifier = properties->GetShaderIdentifier(L"raygen");
      CHECK(identifier ? S_OK : E_FAIL);
      struct alignas(D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT) Record {
        BYTE identifier[D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES];
        UINT value;
      } record{};
      memcpy(record.identifier, identifier, sizeof(record.identifier));
      record.value = 37;
      auto table = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(record), D3D12_RESOURCE_STATE_GENERIC_READ);
      auto output = buffer(
          device.Get(), D3D12_HEAP_TYPE_DEFAULT, sizeof(record.value), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
      );
      auto readback =
          buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, sizeof(record.value), D3D12_RESOURCE_STATE_COPY_DEST);
      CHECK(table && output && readback ? S_OK : E_FAIL);
      void *mapped;
      CHECK(table->Map(0, nullptr, &mapped));
      memcpy(mapped, &record, sizeof(record));
      table->Unmap(0, nullptr);
      CHECK(forget(readback.Get()));
      ComPtr<ID3D12CommandQueue> queue;
      ComPtr<ID3D12CommandAllocator> allocator;
      ComPtr<ID3D12GraphicsCommandList4> list;
      D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
      CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
      CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
      CHECK(device->CreateCommandList(
          0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)
      ));
      list->SetComputeRootSignature(global_root.Get());
      list->SetComputeRootUnorderedAccessView(0, output->GetGPUVirtualAddress());
      list->SetPipelineState1(pipeline.Get());
      D3D12_DISPATCH_RAYS_DESC rays{};
      rays.RayGenerationShaderRecord = {table->GetGPUVirtualAddress(), sizeof(record)};
      rays.Width = rays.Height = rays.Depth = 1;
      list->DispatchRays(&rays);
      transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyResource(readback.Get(), output.Get());
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      CHECK(readback->Map(0, nullptr, &mapped));
      expect(*(UINT *)mapped == record.value, "ray wrote %u, want local constant %u", *(UINT *)mapped, record.value);
      readback->Unmap(0, nullptr);
    }
  }
  return verdict();
}
