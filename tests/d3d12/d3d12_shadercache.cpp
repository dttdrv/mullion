// contract: a stored conversion must preserve the shader's pixels and bytes and avoid conversion in another
// process; a rejected library must be converted again. "The absence of modifiers just moves data without altering
// bits." (D3D11.3 functional specification, 22.9.1 mov). each stage forwards DATA, and the pixel and compute
// shaders write that value plus a root constant. stream output expands strips (14.1: "Just before Streaming Out,
// all topologies are always expanded to lists"). dual source blending uses "outputs o0 and o1 simultaneously as
// input sources to a blending operation with the single RenderTarget at slot 0." (17.6). the reference follows
// those operations. ordinary stages map registers directly: "a value written to o3 always goes to v3 in the
// subsequent Stage." (4.4.3.2). the reordered pixel shader therefore reads EXTRA as DATA and DATA as EXTRA.x.
#include "d3d12_test.hpp"
#include <array>
#include <fstream>
#include <iterator>
#include <limits>
#include <climits>
#include <thread>
#include "../../src/airconv/airconv_public.h"

static const char hlsl[] = R"hlsl(
cbuffer constants : register(b0) { uint number; };
RWStructuredBuffer<uint> outputValues : register(u0);
Texture2D<float> texels : register(t0);
SamplerState clamping : register(s0);
SamplerState wrapping : register(s1);
struct V { float4 pos : SV_Position; nointerpolation uint data : DATA; nointerpolation uint4 extra : EXTRA; };
V vertex(uint id : SV_VertexID, uint data : DATA, uint other : OTHER) {
  V result;
  result.pos = float4(float2(id & 1, id >> 1) * 4 - 1, 0, 1);
  result.data = data / 16 + other / 256 + BIAS;
  result.extra = EXTRA_VALUE;
  return result;
}
V vertex_points(uint id : SV_VertexID, uint data : DATA) {
  V result;
  result.pos = float4(2 * (id + 0.5) / TARGET_SIZE - 1, 1 - 1.0 / TARGET_SIZE, 0, 1);
  result.data = data / 16 + BIAS;
  result.extra = EXTRA_VALUE;
  return result;
}
float4 pixel(V value) : SV_Target { return float4((value.data + number) / MAX_BYTE, 0, 0, 1); }
struct PixelInput { float4 pos : SV_Position; nointerpolation uint4 extra : EXTRA; nointerpolation uint data : DATA; };
float4 reordered(PixelInput value) : SV_Target {
  return float4((value.data + number + (value.extra.x & 1)) / MAX_BYTE, 0, 0, 1);
}
struct Colors { float4 firstColor : SV_Target0; float4 secondColor : SV_Target1; };
Colors dual(V value) {
  Colors result;
  result.firstColor = float4(16 / MAX_BYTE, 0, 0, 1);
  result.secondColor = float4(0.5, 0, 0, 1);
  return result;
}
[maxvertexcount(3)]
void geometry(triangle V inputVertices[3], inout TriangleStream<V> outputVertices) {
  for (uint i = 0; i < 3; i++) {
    V value = inputVertices[i];
    value.data += BIAS;
    outputVertices.Append(value);
  }
}
struct Factors { float edges[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };
Factors factors(InputPatch<V, 3> vertices) {
  Factors result;
  result.edges[0] = result.edges[1] = result.edges[2] = result.inside = 1;
  return result;
}
[domain("tri")] [partitioning("integer")] [outputtopology("triangle_cw")]
[outputcontrolpoints(3)] [patchconstantfunc("factors")]
V hull(InputPatch<V, 3> vertices, uint id : SV_OutputControlPointID) {
  V result = vertices[id];
  result.data += BIAS;
  return result;
}
[domain("tri")]
V domain(Factors patchFactors, float3 uv : SV_DomainLocation, const OutputPatch<V, 3> vertices) {
  V result;
  result.pos = vertices[0].pos * uv.x + vertices[1].pos * uv.y + vertices[2].pos * uv.z;
  result.data = vertices[0].data + BIAS;
  result.extra = vertices[0].extra;
  return result;
}
struct Wide {
  float4 pos : SV_Position;
  nointerpolation uint data : DATA;
  nointerpolation uint4 extra : EXTRA;
  nointerpolation uint4 spare[SPARE_REGISTERS] : SPARE;
};
[domain("tri")]
Wide domain_wide(Factors patchFactors, float3 uv : SV_DomainLocation, const OutputPatch<V, 3> vertices) {
  V value = domain(patchFactors, uv, vertices);
  Wide result;
  result.pos = value.pos;
  result.data = value.data;
  result.extra = value.extra;
  for (uint i = 0; i < SPARE_REGISTERS; i++) result.spare[i] = value.data + i;
  return result;
}
[numthreads(1, 1, 1)]
void compute(uint id : SV_DispatchThreadID) { outputValues[id] = number + id + BIAS; }
[numthreads(1, 1, 1)]
void sampled(uint id : SV_DispatchThreadID) {
  outputValues[id] = (uint)texels.SampleLevel(clamping, float2(1.25, 0.5), 0) +
                    3 * (uint)texels.SampleLevel(wrapping, float2(1.25, 0.5), 0);
}
struct W { float4 pos : SV_Position; nointerpolation uint4 data : DATA; nointerpolation uint4 other : OTHER; };
W stream_vertex(uint id : SV_VertexID) {
  W result;
  uint corner = id % 3;
  result.pos = float4(float2(corner & 1, corner >> 1) * 4 - 1, 0, 1);
  result.data = uint4(id + BIAS, id * 3, number, ~id);
  result.other = result.data + 100;
  return result;
}
float4 stream_pixel(W value) : SV_Target { return float4((value.data.x % 64 + number) / MAX_BYTE, 0, 0, 1); }
[maxvertexcount(3)]
void stream_geometry(triangle W inputVertices[3], inout TriangleStream<W> outputVertices) {
  for (uint i = 0; i < 3; i++) outputVertices.Append(inputVertices[i]);
}
[maxvertexcount(2)]
void stream_multiple(point W inputVertices[1], inout PointStream<W> firstStream, inout PointStream<W> secondStream) {
  W value = inputVertices[0];
  firstStream.Append(value);
  value.data.x += 3;
  secondStream.Append(value);
}
)hlsl";

static const char mesh_hlsl[] = R"hlsl(
cbuffer constants : register(b0) { uint number; };
struct V { float4 pos : SV_Position; nointerpolation uint data : DATA; nointerpolation uint4 extra : EXTRA; };
struct Payload { uint data; };
[numthreads(1, 1, 1)]
void amplify() { Payload payload; payload.data = BIAS; DispatchMesh(1, 1, 1, payload); }
[numthreads(1, 1, 1)] [outputtopology("triangle")]
void mesh(out vertices V verticesOut[3], out indices uint3 indicesOut[1]) {
  SetMeshOutputCounts(3, 1);
  for (uint id = 0; id < 3; id++) {
    verticesOut[id].pos = float4(float2(id & 1, id >> 1) * 4 - 1, 0, 1);
    verticesOut[id].data = BIAS;
    verticesOut[id].extra = EXTRA_VALUE;
  }
  indicesOut[0] = uint3(0, 1, 2);
}
)hlsl";

template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename T> struct alignas(void *) Subobject {
  D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
  T data{};
};

// the existing converter imports are counted on both the uncached and cached library.
alignas(sizeof(LONG64)) static volatile LONG64 conversions;
static decltype(&SM50Compile) original_compile;
static std::array<decltype(&SM50CompileGeometryPipelineVertex), 4> original_pair;

static int
count_compile(
    sm50_shader_t shader, SM50_SHADER_COMPILATION_ARGUMENT_DATA *args, const char *name, sm50_bitcode_t *bitcode,
    sm50_error_t *error
) {
  InterlockedIncrement64(&conversions);
  return original_compile(shader, args, name, bitcode, error);
}

template <size_t I>
static int
count_pair(
    sm50_shader_t first, sm50_shader_t second, SM50_SHADER_COMPILATION_ARGUMENT_DATA *args, const char *name,
    sm50_bitcode_t *bitcode, sm50_error_t *error
) {
  InterlockedIncrement64(&conversions);
  return original_pair[I](first, second, args, name, bitcode, error);
}

static bool
count_imports() {
  auto module = (BYTE *)GetModuleHandleA("d3d12core.dll");
  if (!expect(module != nullptr, "no d3d12core module"))
    return false;
  auto dos = (IMAGE_DOS_HEADER *)module;
  auto nt = (IMAGE_NT_HEADERS *)(module + dos->e_lfanew);
  auto imports =
      (IMAGE_IMPORT_DESCRIPTOR *)(module +
                                  nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
  const char *names[] = {
      "SM50Compile", "SM50CompileGeometryPipelineVertex", "SM50CompileGeometryPipelineGeometry",
      "SM50CompileTessellationPipelineHull", "SM50CompileTessellationPipelineDomain"
  };
  const uintptr_t hooks[] = {
      uintptr_t(count_compile), uintptr_t(count_pair<0>), uintptr_t(count_pair<1>), uintptr_t(count_pair<2>),
      uintptr_t(count_pair<3>)
  };
  UINT found = 0;
  for (; imports->Name; imports++) {
    auto names_at = (IMAGE_THUNK_DATA *)(module + imports->OriginalFirstThunk);
    auto functions = (IMAGE_THUNK_DATA *)(module + imports->FirstThunk);
    for (; names_at->u1.AddressOfData; names_at++, functions++) {
      if (IMAGE_SNAP_BY_ORDINAL(names_at->u1.Ordinal))
        continue;
      auto name = (IMAGE_IMPORT_BY_NAME *)(module + names_at->u1.AddressOfData);
      for (size_t i = 0; i < std::size(names); i++) {
        if (strcmp((const char *)name->Name, names[i]))
          continue;
        DWORD protection;
        if (!expect(
                VirtualProtect(&functions->u1.Function, sizeof(functions->u1.Function), PAGE_READWRITE, &protection),
                "cannot count %s", names[i]
            ))
          return false;
        if (i)
          original_pair[i - 1] = reinterpret_cast<decltype(original_pair)::value_type>(functions->u1.Function);
        else
          original_compile = reinterpret_cast<decltype(original_compile)>(functions->u1.Function);
        functions->u1.Function = hooks[i];
        if (!expect(
                VirtualProtect(&functions->u1.Function, sizeof(functions->u1.Function), protection, &protection),
                "cannot restore import protection"
            ))
          return false;
        found++;
      }
    }
  }
  return expect(found == std::size(names), "%u converter imports, want %zu", found, std::size(names));
}

static int processes(int argc, char **argv);

int
main(int argc, char **argv) {
  constexpr UINT size = 4, extra_value = 33;
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  std::array<std::string, 2> vs, cs, ms, amplification, gs, hs, ds;
  for (unsigned i = 0; i < vs.size(); i++) {
    std::vector<std::string> defines = {
        "BIAS=" + std::to_string(i + 1), "MAX_BYTE=" + std::to_string(std::numeric_limits<BYTE>::max()) + ".0",
        "TARGET_SIZE=" + std::to_string(size), "SPARE_REGISTERS=" + std::to_string(D3D12_DS_OUTPUT_REGISTER_COUNT / 2),
        "EXTRA_VALUE=" + std::to_string(extra_value)
    };
    vs[i] = compiler.compile(hlsl, "vertex", "vs", defines);
    cs[i] = compiler.compile(hlsl, "compute", "cs", defines);
    gs[i] = compiler.compile(hlsl, "geometry", "gs", defines);
    hs[i] = compiler.compile(hlsl, "hull", "hs", defines);
    ds[i] = compiler.compile(hlsl, "domain", "ds", defines);
    if (compiler.dxc) {
      ms[i] = compiler.compile(mesh_hlsl, "mesh", "ms_6_5", defines);
      amplification[i] = compiler.compile(mesh_hlsl, "amplify", "as_6_5", defines);
    }
  }
  std::vector<std::string> defines = {
      "BIAS=1", "MAX_BYTE=" + std::to_string(std::numeric_limits<BYTE>::max()) + ".0",
      "TARGET_SIZE=" + std::to_string(size), "SPARE_REGISTERS=" + std::to_string(D3D12_DS_OUTPUT_REGISTER_COUNT / 2),
      "EXTRA_VALUE=" + std::to_string(extra_value)
  };
  auto vs_points = compiler.compile(hlsl, "vertex_points", "vs", defines);
  if (!expect(!vs_points.empty(), "point vertex shader did not compile"))
    return verdict();
  auto ps = compiler.compile(hlsl, "pixel", "ps", defines);
  auto reordered = compiler.compile(hlsl, "reordered", "ps", defines);
  auto dual = compiler.compile(hlsl, "dual", "ps", defines);
  auto stream_ps = compiler.compile(hlsl, "stream_pixel", "ps", defines);
  if (!expect(!reordered.empty() && !dual.empty() && !stream_ps.empty(), "pixel variants did not compile"))
    return verdict();
  auto wide_ds = compiler.compile(hlsl, "domain_wide", "ds", defines);
  if (!expect(!wide_ds.empty(), "wide domain shader did not compile"))
    return verdict();
  auto sampled = compiler.compile(hlsl, "sampled", "cs", defines);
  if (!expect(!sampled.empty(), "sampler shader did not compile"))
    return verdict();
  auto stream_vs = compiler.compile(hlsl, "stream_vertex", "vs", defines);
  auto stream_gs = compiler.compile(hlsl, "stream_geometry", "gs", defines);
  auto stream_multiple = compiler.compile(hlsl, "stream_multiple", "gs", defines);
  if (!expect(
          !stream_vs.empty() && !stream_gs.empty() && !stream_multiple.empty(), "stream output shaders did not compile"
      ))
    return verdict();
  for (auto codes : {&vs, &cs, &gs, &hs, &ds})
    for (auto &code : *codes)
      if (!expect(!code.empty(), "a graphics or compute shader did not compile"))
        return verdict();
  if (!expect(!ps.empty(), "pixel shader did not compile"))
    return verdict();
  if (compiler.dxc) {
    for (auto codes : {&ms, &amplification})
      for (auto &code : *codes)
        if (!expect(!code.empty(), "a mesh or amplification shader did not compile"))
          return verdict();
  }

  ComPtr<ID3D12Device2> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  if (argc <= 3)
    return processes(argc, argv);
  if (!count_imports())
    return verdict();
  const auto before = conversions;
  const bool disabled = !strcmp(argv[2], "disabled");
  WIN32_FIND_DATAA stored_file;
  const auto store_files = std::string(argv[3]).substr(0, std::string(argv[3]).rfind('\\') + 1) + "shaders_*.db";
  auto store_exists = [&] {
    auto find = FindFirstFileA(store_files.c_str(), &stored_file);
    if (find == INVALID_HANDLE_VALUE)
      return false;
    FindClose(find);
    return true;
  };
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList6> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  D3D12_ROOT_PARAMETER parameters[3]{};
  parameters[0].ParameterType = parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants = {0, 0, 1};
  parameters[1].Constants = {1, 0, 1};
  parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  auto first_root = root_signature(
      device.Get(),
      {3, parameters, 0, nullptr,
       D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT}
  );
  std::swap(parameters[0], parameters[1]);
  auto second_root = root_signature(
      device.Get(),
      {3, parameters, 0, nullptr,
       D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT}
  );
  if (!expect(first_root && second_root, "root signatures were not made"))
    return verdict();
  const UINT elements = 2 * SM50_GEOMETRY_WARP_THREADS + 1, base = 7, data0 = 0x111, data1 = 0x211;
  const UINT vertices[][2] = {{data0, data1}, {0x311, 0x411}, {0x511, 0x611}};
  auto vertex_buffer =
      buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(vertices), D3D12_RESOURCE_STATE_GENERIC_READ);
  void *mapped;
  CHECK(vertex_buffer->Map(0, nullptr, &mapped));
  memcpy(mapped, vertices, sizeof(vertices));
  vertex_buffer->Unmap(0, nullptr);
  auto output = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, elements * sizeof(UINT), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto readback =
      buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, elements * sizeof(UINT), D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  std::vector<UINT> results;

  for (UINT shader = 0; shader < cs.size(); shader++)
    for (UINT root = 0; root < 2; root++) {
      if (shader && root)
        continue;
      step("compute shader %u, root mapping %u", shader, root);
      auto rs = root ? second_root.Get() : first_root.Get();
      D3D12_COMPUTE_PIPELINE_STATE_DESC desc{rs, bytecode(cs[shader])};
      ComPtr<ID3D12PipelineState> pipeline;
      auto made_before = conversions;
      HANDLE start = CreateEventA(nullptr, TRUE, FALSE, nullptr);
      if (!expect(start != nullptr, "no compute start event"))
        return verdict();
      std::array<ComPtr<ID3D12PipelineState>, 2> pipelines;
      std::array<HRESULT, std::size(pipelines)> statuses;
      statuses.fill(E_FAIL);
      std::thread threads[std::size(pipelines)];
      for (size_t i = 0; i < std::size(pipelines); i++)
        threads[i] = std::thread([&, i] {
          if (WaitForSingleObject(start, INFINITE) == WAIT_OBJECT_0)
            statuses[i] = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipelines[i]));
        });
      SetEvent(start);
      for (auto &thread : threads)
        thread.join();
      CloseHandle(start);
      for (auto status : statuses)
        CHECK(status);
      pipeline = pipelines[0];
      if (disabled)
        expect(conversions - made_before == pipelines.size(), "the disabled store must convert every request");
      if (!strcmp(argv[2], "cold") && store_exists())
        expect(conversions - made_before == 1, "overlapping requests must share the conversion");
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), pipeline.Get()));
      list->SetComputeRootSignature(rs);
      list->SetComputeRoot32BitConstant(0, base, 0);
      list->SetComputeRoot32BitConstant(1, base + 1, 0);
      list->SetComputeRootUnorderedAccessView(2, output->GetGPUVirtualAddress());
      list->Dispatch(elements, 1, 1);
      transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyResource(readback.Get(), output.Get());
      transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      UINT *got;
      CHECK(readback->Map(0, nullptr, (void **)&got));
      for (UINT i = 0; i < elements; i++) {
        UINT want = base + root + i + shader + 1;
        expect(got[i] == want, "compute element %u: %u, want %u", i, got[i], want);
        results.push_back(got[i]);
      }
      readback->Unmap(0, nullptr);
    }

  for (UINT kind = 0; kind < 6; kind++)
    for (UINT variant = 0; variant < 23; variant++) {
      const bool mesh = kind == 3 || kind == 4, tessellation = kind == 2 || kind == 5;
      if (kind == 0 && variant == 22)
        continue;
      const bool points = variant == 17 || variant == 18 || variant == 21;
      if ((mesh && !compiler.dxc) || (kind == 1 && variant != 0 && variant != 14) ||
          (kind == 2 && variant != 0 && variant != 15 && variant != 16 && variant != 22) ||
          (mesh && variant != 0 && variant != 2 && variant != 13) || (kind == 5 && variant != 0))
        continue;
      step("graphics kind %u, key variant %u", kind, variant);
      const UINT root = variant == 1, shader = variant == 2, offset = variant == 3 ? sizeof(UINT) : 0;
      const UINT samples = variant == 4 ? 4 : 1;
      auto rs = root ? second_root.Get() : first_root.Get();
      D3D12_INPUT_ELEMENT_DESC input{
          variant == 19 ? "OTHER" : "DATA",
          0,
          variant == 10 ? DXGI_FORMAT_R8_UINT : DXGI_FORMAT_R32_UINT,
          variant == 7 ? 1u : 0u,
          offset,
          variant == 8 || variant == 9 || variant == 18 || variant == 21 ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                                                                         : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
          variant == 9                    ? 2u
          : variant == 8 || variant == 18 ? 1u
                                          : 0u
      };
      D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{
          rs, bytecode(points ? vs_points : vs[shader]),
          bytecode(
              variant == 11 || variant == 12 ? dual
              : variant == 13                ? reordered
                                             : ps
          )
      };
      desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      if (variant == 12) {
        auto &blend = desc.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_SRC1_COLOR;
        blend.DestBlend = D3D12_BLEND_ZERO;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlendAlpha = D3D12_BLEND_ZERO;
      }
      desc.SampleMask = variant == 5 ? 0 : ~0u;
      desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
      D3D12_INPUT_ELEMENT_DESC inputs[] = {
          input,
          {variant == 19 ? "DATA" : "OTHER", 0, DXGI_FORMAT_R32_UINT, 2, sizeof(UINT),
           D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA}
      };
      desc.InputLayout = {inputs, points ? 1u : UINT(std::size(inputs))};
      desc.PrimitiveTopologyType =
          points ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      desc.NumRenderTargets = 1;
      desc.RTVFormats[0] = variant == 6 ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
      desc.SampleDesc = {samples, 0};
      if (kind == 1 || kind == 5)
        desc.GS = bytecode(gs[variant == 14]);
      if (tessellation) {
        desc.HS = bytecode(hs[variant == 15]);
        desc.DS = bytecode(variant == 22 ? wide_ds : ds[variant == 16]);
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
      }
      ComPtr<ID3D12PipelineState> pipeline;
      auto made_before = conversions;
      if (!mesh) {
        CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)));
      } else {
        struct {
          Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> rs;
          Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, D3D12_SHADER_BYTECODE> amplification;
          Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> mesh;
          Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> pixel;
          Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> raster;
          Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> blend;
          Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> mask;
          Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> samples;
          Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> targets;
        } stream;
        stream.rs.data = rs;
        stream.mesh.data = bytecode(ms[shader]);
        if (kind == 4)
          stream.amplification.data = bytecode(amplification[shader]);
        stream.pixel.data = desc.PS;
        stream.raster.data = desc.RasterizerState;
        stream.blend.data = desc.BlendState;
        stream.mask.data = desc.SampleMask;
        stream.samples.data = desc.SampleDesc;
        stream.targets.data.NumRenderTargets = 1;
        stream.targets.data.RTFormats[0] = desc.RTVFormats[0];
        D3D12_PIPELINE_STATE_STREAM_DESC stream_desc{sizeof(stream), &stream};
        CHECK(device->CreatePipelineState(&stream_desc, IID_PPV_ARGS(&pipeline)));
      }
      if (!strcmp(argv[2], "cold") && store_exists() && kind == 0) {
        if (variant == 4)
          expect(conversions == made_before, "sample count changes the Metal pipeline, not the conversion");
        if (variant == 3 || (variant >= 5 && variant <= 13) || variant == 17 || variant == 18 || variant == 19 ||
            variant == 21)
          expect(conversions - made_before == 1, "a new conversion input must have its own entry");
      }
      if (!strcmp(argv[2], "cold") && store_exists() && mesh && variant == 13)
        expect(
            conversions - made_before == (kind == 3 ? 1u : 0u),
            "pixel linkage converts the mesh shader and reuses amplification"
        );
      if (!strcmp(argv[2], "cold") && store_exists() && kind == 2 && variant == 22)
        expect(conversions - made_before == 2, "domain output size must also distinguish the hull conversion");
      if (disabled)
        expect(
            conversions - made_before >= (kind == 1 || tessellation || kind == 4 ? 3u : 2u),
            "the disabled store must convert every graphics stage"
        );
      D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
      D3D12_RESOURCE_DESC target_desc{
          D3D12_RESOURCE_DIMENSION_TEXTURE2D,
          0,
          size,
          size,
          1,
          1,
          desc.RTVFormats[0],
          {samples, 0},
          D3D12_TEXTURE_LAYOUT_UNKNOWN,
          D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
      };
      ComPtr<ID3D12Resource> target, resolved;
      CHECK(device->CreateCommittedResource(
          &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
      ));
      auto copy_desc = target_desc;
      copy_desc.SampleDesc.Count = 1;
      if (samples != 1)
        CHECK(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &copy_desc, D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr, IID_PPV_ARGS(&resolved)
        ));
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
      UINT64 bytes;
      device->GetCopyableFootprints(&copy_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
      auto pixels = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
      CHECK(forget(pixels.Get()));
      device->CreateRenderTargetView(target.Get(), nullptr, rtv);
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), pipeline.Get()));
      list->SetGraphicsRootSignature(rs);
      list->SetGraphicsRoot32BitConstant(0, base, 0);
      list->SetGraphicsRoot32BitConstant(1, base + 1, 0);
      list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
      const float clear[4] = {};
      list->ClearRenderTargetView(rtv, clear, 0, nullptr);
      D3D12_VIEWPORT viewport{0, 0, float(size), float(size), 0, 1};
      D3D12_RECT scissor{0, 0, LONG(size), LONG(size)};
      list->RSSetViewports(1, &viewport);
      list->RSSetScissorRects(1, &scissor);
      D3D12_VERTEX_BUFFER_VIEW view{vertex_buffer->GetGPUVirtualAddress(), sizeof(vertices), sizeof(vertices[0])};
      D3D12_VERTEX_BUFFER_VIEW views[] = {view, view, view};
      views[1].BufferLocation += sizeof(UINT);
      views[1].SizeInBytes -= sizeof(UINT);
      list->IASetVertexBuffers(0, std::size(views), views);
      ComPtr<ID3D12Resource> adjacency;
      if (!mesh) {
        if (variant == 20) {
          const UINT indices[] = {0, 0, 1, 0, 2, 0};
          adjacency = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(indices), D3D12_RESOURCE_STATE_GENERIC_READ);
          CHECK(adjacency->Map(0, nullptr, &mapped));
          memcpy(mapped, indices, sizeof(indices));
          adjacency->Unmap(0, nullptr);
          D3D12_INDEX_BUFFER_VIEW view{adjacency->GetGPUVirtualAddress(), sizeof(indices), DXGI_FORMAT_R32_UINT};
          list->IASetIndexBuffer(&view);
          list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ);
          list->DrawIndexedInstanced(std::size(indices), 1, 0, 0, 0);
        } else {
          list->IASetPrimitiveTopology(
              tessellation ? D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST
              : points     ? D3D_PRIMITIVE_TOPOLOGY_POINTLIST
                           : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST
          );
          list->DrawInstanced(3, 3, 0, 0);
        }
      } else {
        list->DispatchMesh(1, 1, 1);
      }
      ID3D12Resource *copied = target.Get();
      if (samples != 1) {
        transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        list->ResolveSubresource(resolved.Get(), 0, target.Get(), 0, desc.RTVFormats[0]);
        transition(list.Get(), resolved.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        copied = resolved.Get();
      } else {
        transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      }
      D3D12_TEXTURE_COPY_LOCATION source{copied, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      D3D12_TEXTURE_COPY_LOCATION destination{pixels.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {footprint}};
      list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      BYTE *got;
      CHECK(pixels->Map(0, nullptr, (void **)&got));
      UINT data = variant == 8             ? vertices[2][0]
                  : variant == 9           ? vertices[1][0]
                  : offset || variant == 7 ? data1
                  : variant == 10          ? BYTE(data0)
                                           : data0;
      const UINT stages = (kind == 1 || kind == 5 ? 1 + (variant == 14) : 0) +
                          (tessellation ? 2 + (variant == 15) + (variant == 16) : 0);
      UINT color = variant == 11 ? 16
                   : variant == 12
                       ? 8
                       : (!mesh ? (variant == 19 ? data1 / 16 + data0 / 256 : data / 16 + data1 / 256) : 0) + base +
                             root + shader + 1 + stages + (variant == 13);
      if (variant == 13 && !mesh)
        color = extra_value + base + ((data / 16 + data1 / 256 + shader + 1) & 1);
      for (UINT y = 0; y < size; y++)
        for (UINT x = 0; x < size; x++) {
          if (points)
            color = vertices
                            [variant == 18   ? std::size(vertices) - 1
                             : variant == 21 ? 0
                                             : std::min(x, UINT(std::size(vertices) - 1))][0] /
                        16 +
                    base + 1;
          UINT want = variant == 5 || (points && (y != 0 || x >= std::size(vertices)))
                          ? 0
                          : (color << (variant == 6 ? 2 * sizeof(BYTE) * CHAR_BIT : 0)) |
                                (UINT(std::numeric_limits<BYTE>::max()) << ((sizeof(UINT) - sizeof(BYTE)) * CHAR_BIT));
          UINT actual;
          memcpy(&actual, got + footprint.Offset + y * footprint.Footprint.RowPitch + x * sizeof(UINT), sizeof(actual));
          expect(actual == want, "pixel (%u, %u): %#x, want %#x", x, y, actual, want);
          results.push_back(actual);
        }
      pixels->Unmap(0, nullptr);
    }
  {
    step("static samplers with the same registers and states in another order");
    const float texels[] = {11, 23};
    D3D12_RESOURCE_DESC texture_desc{
        D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, std::size(texels), 1, 1, 1, DXGI_FORMAT_R32_FLOAT, {1, 0},
        D3D12_TEXTURE_LAYOUT_UNKNOWN
    };
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    ComPtr<ID3D12Resource> texture;
    CHECK(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &texture_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)
    ));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT64 bytes;
    device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
    CHECK(upload->Map(0, nullptr, &mapped));
    memcpy((BYTE *)mapped + footprint.Offset, texels, sizeof(texels));
    upload->Unmap(0, nullptr);
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), nullptr));
    D3D12_TEXTURE_COPY_LOCATION source{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {footprint}};
    D3D12_TEXTURE_COPY_LOCATION destination{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    transition(
        list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
    );
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    ComPtr<ID3D12DescriptorHeap> descriptors;
    D3D12_DESCRIPTOR_HEAP_DESC descriptor_desc{
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
    };
    CHECK(device->CreateDescriptorHeap(&descriptor_desc, IID_PPV_ARGS(&descriptors)));
    device->CreateShaderResourceView(texture.Get(), nullptr, descriptors->GetCPUDescriptorHandleForHeapStart());
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1};
    D3D12_ROOT_PARAMETER roots[2]{};
    roots[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    roots[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    roots[1].DescriptorTable = {1, &range};
    D3D12_STATIC_SAMPLER_DESC samplers[2]{};
    for (UINT i = 0; i < std::size(samplers); i++) {
      samplers[i].ShaderRegister = i;
      samplers[i].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
      samplers[i].AddressU = i ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
      samplers[i].AddressV = samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
      samplers[i].MaxAnisotropy = 1;
      samplers[i].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
      samplers[i].MaxLOD = std::numeric_limits<float>::max();
    }
    for (UINT order = 0; order < 2; order++) {
      auto rs = root_signature(device.Get(), {UINT(std::size(roots)), roots, UINT(std::size(samplers)), samplers});
      if (!expect(bool(rs), "no sampler root signature"))
        return verdict();
      D3D12_COMPUTE_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(sampled)};
      ComPtr<ID3D12PipelineState> pipeline;
      auto made_before = conversions;
      CHECK(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline)));
      if (disabled)
        expect(conversions - made_before == 1, "the disabled store must convert the sampler shader");
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), pipeline.Get()));
      list->SetComputeRootSignature(rs.Get());
      list->SetComputeRootUnorderedAccessView(0, output->GetGPUVirtualAddress());
      ID3D12DescriptorHeap *heaps[] = {descriptors.Get()};
      list->SetDescriptorHeaps(std::size(heaps), heaps);
      list->SetComputeRootDescriptorTable(1, descriptors->GetGPUDescriptorHandleForHeapStart());
      list->Dispatch(elements, 1, 1);
      transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyResource(readback.Get(), output.Get());
      transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      CHECK(readback->Map(0, nullptr, &mapped));
      auto got = (UINT *)mapped;
      for (UINT i = 0; i < elements; i++) {
        UINT want = UINT(texels[1]) + 3 * UINT(texels[0]);
        expect(got[i] == want, "static sampler order %u, element %u: %u, want %u", order, i, got[i], want);
        results.push_back(got[i]);
      }
      readback->Unmap(0, nullptr);
      std::swap(samplers[0], samplers[1]);
    }
  }
  for (UINT topology = 0; topology < 4; topology++)
    for (UINT variant = 0; variant < (topology < 2 ? 12u : 8u); variant++) {
      if ((topology == 0 && variant != 0 && variant != 1 && variant != 2 && variant != 3 && variant != 4 &&
           variant != 9 && variant != 10 && variant != 11) ||
          (topology == 1 && variant != 0 && variant != 8) ||
          (topology == 2 && variant != 0 && variant != 6 && variant != 7) || (topology == 3 && variant != 0))
        continue;
      step("stream output topology %u, key variant %u", topology, variant);
      const bool strip = topology == 2;
      const bool cut = variant == 6 || variant == 7;
      const UINT cut_index = variant == 7 ? ~0u : 0xffff;
      const UINT stream = variant == 10;
      const UINT slot = variant == 1 ? D3D12_SO_BUFFER_SLOT_COUNT - 1 : 0;
      const UINT component = variant == 2 ? 1 : 0, gap = variant == 3 ? 1 : 0;
      const UINT stride = (variant == 4 ? 5 : 4) * sizeof(UINT);
      D3D12_SO_DECLARATION_ENTRY entries[] = {
          {stream, nullptr, 0, 0, BYTE(gap), BYTE(slot)},
          {stream, variant == 11 ? "OTHER" : "DATA", 0, BYTE(component), 1, BYTE(slot)}
      };
      std::array<UINT, D3D12_SO_BUFFER_SLOT_COUNT> strides;
      strides.fill(stride);
      const bool rasterized = variant == 8;
      D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{first_root.Get(), bytecode(stream_vs), bytecode(stream_ps)};
      if (variant == 9 || variant == 10)
        desc.GS = bytecode(stream_multiple);
      else if (topology == 1)
        desc.GS = bytecode(stream_gs);
      desc.StreamOutput = {
          entries + !gap, gap + 1, strides.data(), UINT(strides.size()), rasterized ? 0u : D3D12_SO_NO_RASTERIZED_STREAM
      };
      desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
      desc.SampleMask = ~0u;
      desc.SampleDesc = {1, 0};
      desc.NumRenderTargets = 1;
      desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
      desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      desc.PrimitiveTopologyType =
          topology == 0 ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      desc.IBStripCutValue = variant == 6   ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF
                             : variant == 7 ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF
                                            : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
      ComPtr<ID3D12PipelineState> pipeline;
      auto made_before = conversions;
      CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)));
      if (disabled)
        expect(conversions - made_before >= 4, "the disabled store must convert both stream output passes");
      std::vector<UINT> indices;
      for (UINT i = 0; i < 3 * elements; i++) {
        indices.push_back(i);
        if (strip && i % elements == elements / 2)
          indices.push_back(cut_index);
      }
      std::vector<UINT> emitted, primitive;
      for (auto index : indices) {
        if (strip && cut && index == cut_index) {
          primitive.clear();
          continue;
        }
        primitive.push_back(index);
        if (topology == 0)
          emitted.push_back(index);
        else if (topology == 1 && primitive.size() == 3) {
          emitted.insert(emitted.end(), primitive.begin(), primitive.end());
          primitive.clear();
        } else if (topology == 2 && primitive.size() >= 3) {
          size_t n = primitive.size();
          emitted.push_back(primitive[n - 3]);
          emitted.push_back(primitive[n - ((n - 3) % 2 ? 1 : 2)]);
          emitted.push_back(primitive[n - ((n - 3) % 2 ? 2 : 1)]);
        } else if (topology == 3 && primitive.size() == 6) {
          for (size_t i = 0; i < primitive.size(); i += 2)
            emitted.push_back(primitive[i]);
          primitive.clear();
        }
      }
      D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
      D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                                      0,
                                      size,
                                      size,
                                      1,
                                      1,
                                      desc.RTVFormats[0],
                                      {1, 0},
                                      D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
      ComPtr<ID3D12Resource> target;
      CHECK(device->CreateCommittedResource(
          &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
      ));
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
      UINT64 bytes;
      device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
      auto pixels = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
      CHECK(forget(pixels.Get()));
      device->CreateRenderTargetView(target.Get(), nullptr, rtv);
      const UINT64 slot_bytes = stride * emitted.size();
      const UINT64 filled_at = (slot_bytes * strides.size() + alignof(UINT64) - 1) & ~(UINT64(alignof(UINT64)) - 1);
      const UINT64 total = filled_at + sizeof(UINT64) * strides.size();
      std::vector<BYTE> want(total, 0xff);
      memset(want.data() + filled_at, 0, sizeof(UINT64) * strides.size());
      UINT64 filled = emitted.size() * stride;
      memcpy(want.data() + filled_at + slot * sizeof(filled), &filled, sizeof(filled));
      for (size_t i = 0; i < emitted.size(); i++) {
        UINT value = component ? emitted[i] * 3 : emitted[i] + 1 + 3 * stream + (variant == 11 ? 100 : 0);
        memcpy(want.data() + slot * slot_bytes + i * stride + gap * sizeof(UINT), &value, sizeof(value));
      }
      auto stored = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, total, D3D12_RESOURCE_STATE_COPY_DEST);
      auto initial = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);
      auto observed = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
      auto index_buffer = buffer(
          device.Get(), D3D12_HEAP_TYPE_UPLOAD, indices.size() * sizeof(indices[0]), D3D12_RESOURCE_STATE_GENERIC_READ
      );
      CHECK(initial->Map(0, nullptr, &mapped));
      memset(mapped, 0xff, total);
      memset((BYTE *)mapped + filled_at, 0, sizeof(UINT64) * strides.size());
      initial->Unmap(0, nullptr);
      CHECK(index_buffer->Map(0, nullptr, &mapped));
      memcpy(mapped, indices.data(), indices.size() * sizeof(indices[0]));
      index_buffer->Unmap(0, nullptr);
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), pipeline.Get()));
      list->CopyResource(stored.Get(), initial.Get());
      transition(list.Get(), stored.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
      list->SetGraphicsRootSignature(first_root.Get());
      list->SetGraphicsRoot32BitConstant(0, base, 0);
      list->SetGraphicsRoot32BitConstant(1, base + 1, 0);
      list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
      const float clear[4] = {};
      list->ClearRenderTargetView(rtv, clear, 0, nullptr);
      D3D12_VIEWPORT viewport{0, 0, float(size), float(size), 0, 1};
      D3D12_RECT scissor{0, 0, LONG(size), LONG(size)};
      list->RSSetViewports(1, &viewport);
      list->RSSetScissorRects(1, &scissor);
      const D3D_PRIMITIVE_TOPOLOGY topologies[] = {
          D3D_PRIMITIVE_TOPOLOGY_POINTLIST, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP,
          D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ
      };
      list->IASetPrimitiveTopology(topologies[topology]);
      D3D12_INDEX_BUFFER_VIEW index_view{
          index_buffer->GetGPUVirtualAddress(), UINT(indices.size() * sizeof(indices[0])), DXGI_FORMAT_R32_UINT
      };
      list->IASetIndexBuffer(&index_view);
      std::array<D3D12_STREAM_OUTPUT_BUFFER_VIEW, D3D12_SO_BUFFER_SLOT_COUNT> views;
      for (size_t i = 0; i < views.size(); i++)
        views[i] = {
            stored->GetGPUVirtualAddress() + slot_bytes * i, slot_bytes,
            stored->GetGPUVirtualAddress() + filled_at + sizeof(UINT64) * i
        };
      list->SOSetTargets(0, views.size(), views.data());
      list->DrawIndexedInstanced(indices.size(), 1, 0, 0, 0);
      transition(list.Get(), stored.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyResource(observed.Get(), stored.Get());
      transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION source{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      D3D12_TEXTURE_COPY_LOCATION destination{pixels.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {footprint}};
      list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      CHECK(observed->Map(0, nullptr, &mapped));
      expect(!memcmp(mapped, want.data(), want.size()), "stream output bytes or filled sizes differ");
      auto got = (UINT *)mapped;
      results.insert(results.end(), got, got + total / sizeof(UINT));
      observed->Unmap(0, nullptr);
      CHECK(pixels->Map(0, nullptr, &mapped));
      const UINT color = (emitted[emitted.size() - 3] + 1) % 64 + base;
      const UINT pixel_want =
          rasterized ? color | (UINT(std::numeric_limits<BYTE>::max()) << ((sizeof(UINT) - sizeof(BYTE)) * CHAR_BIT))
                     : 0;
      for (UINT y = 0; y < size; y++)
        for (UINT x = 0; x < size; x++) {
          UINT actual;
          memcpy(
              &actual, (BYTE *)mapped + footprint.Offset + y * footprint.Footprint.RowPitch + x * sizeof(UINT),
              sizeof(actual)
          );
          expect(actual == pixel_want, "stream output pixel (%u, %u): %#x, want %#x", x, y, actual, pixel_want);
          results.push_back(actual);
        }
      pixels->Unmap(0, nullptr);
    }
  UINT64 converted = conversions - before;
  const bool warm = !strcmp(argv[2], "warm");
  expect(warm ? converted == 0 : converted > 0, "%llu conversions in %s", converted, argv[2]);
  std::ofstream file(argv[3], std::ios::binary);
  file.write(reinterpret_cast<const char *>(results.data()), results.size() * sizeof(results[0]));
  expect(bool(file), "readback results were not written");
  return verdict();
}

static int
processes(int argc, char **argv) {
  char temporary[MAX_PATH], directory[MAX_PATH], exe[MAX_PATH];
  GetTempPathA(sizeof(temporary), temporary);
  GetModuleFileNameA(nullptr, exe, sizeof(exe));
  if (!expect(GetTempFileNameA(temporary, "sc", 0, directory), "no temporary name"))
    return verdict();
  DeleteFileA(directory);
  if (!expect(CreateDirectoryA(directory, nullptr), "no temporary directory"))
    return verdict();
  auto unix_name = reinterpret_cast<char *(__cdecl *)(const WCHAR *)>(
      GetProcAddress(GetModuleHandleA("kernel32.dll"), "wine_get_unix_file_name")
  );
  if (!expect(unix_name != nullptr, "Wine's path conversion is unavailable"))
    return verdict();
  std::wstring wide(std::size(directory), L'\0');
  if (!expect(MultiByteToWideChar(CP_ACP, 0, directory, -1, wide.data(), wide.size()), "no wide temporary path"))
    return verdict();
  auto path = unix_name(wide.c_str());
  if (!expect(path != nullptr, "the store's Unix path is unavailable"))
    return verdict();
  std::string store = std::string("DXMT_SHADER_CACHE_PATH=") + path;
  HeapFree(GetProcessHeap(), 0, path);
  auto spawn = reinterpret_cast<LONG(WINAPI *)(char *const *, int)>(
      GetProcAddress(GetModuleHandleA("ntdll.dll"), "__wine_unix_spawnvp")
  );
  std::wstring loader(GetEnvironmentVariableW(L"WINELOADER", nullptr, 0), L'\0');
  if (!expect(
          spawn && !loader.empty() && GetEnvironmentVariableW(L"WINELOADER", loader.data(), loader.size()),
          "Wine's loader or Unix process launch is unavailable"
      ))
    return verdict();
  if (loader.starts_with(L"\\??\\"))
    loader.replace(0, std::size(L"\\??\\") - 1, L"\\\\?\\");
  auto loader_path = unix_name(loader.c_str());
  if (!expect(loader_path != nullptr, "the loader's Unix path is unavailable"))
    return verdict();
  std::string reference;
  auto run = [&](const char *mode, bool disabled, const char *option = "DXMT_AIRCONV_SKIP=") {
    step("process %s, store %s", mode, directory);
    std::string output = std::string(directory) + "\\" + mode + ".bin";
    const char *args[] = {
        "env",
        "-u",
        "DXMT_AIRCONV_SKIP",
        "-u",
        "DXMT_MESH_GRID_LIMIT",
        store.c_str(),
        disabled ? "DXMT_SHADER_CACHE=0" : "DXMT_SHADER_CACHE=1",
        option,
        loader_path,
        exe,
        argc > 1 ? argv[1] : "dxil",
        mode,
        output.c_str(),
        nullptr
    };
    auto status = spawn(const_cast<char *const *>(args), TRUE);
    expect(status == 0, "child ended with %ld", status);
    std::ifstream file(output, std::ios::binary);
    std::string bytes{std::istreambuf_iterator<char>(file), {}};
    expect(!bytes.empty(), "no readback results");
    if (reference.empty())
      reference = bytes;
    else
      expect(bytes == reference, "the process's pixels or buffers differ from the first process");
  };
  run("cold", false);
  run("warm", false);
  WIN32_FIND_DATAA found;
  if (!trace::wrong) {
    auto find = FindFirstFileA((std::string(directory) + "\\shaders_*.db").c_str(), &found);
    if (expect(find != INVALID_HANDLE_VALUE, "no shader store was written")) {
      FindClose(find);
      std::string db = std::string(directory) + "\\" + found.cFileName;
      std::ifstream file(db, std::ios::binary);
      std::string bytes{std::istreambuf_iterator<char>(file), {}};
      auto at = bytes.find("MTLB");
      if (expect(at != std::string::npos, "no stored Metal library")) {
        std::fstream damaged(db, std::ios::in | std::ios::out | std::ios::binary);
        damaged.seekp(at);
        damaged.put(bytes[at] ^ 1);
        expect(bool(damaged), "the stored library was not damaged");
      }
    }
    run("repair", false);
    run("warm", false);
    run("disabled", true);
    run("disabled", true);
    const char *options[] = {
        "DXMT_AIRCONV_SKIP=simdgroup-barrier", "DXMT_AIRCONV_SKIP=simdgroup-barrier,simdgroup-barrier"
    };
    LONG statuses[std::size(options)];
    std::thread writers[std::size(options)];
    step("concurrent processes write distinct conversions");
    for (size_t i = 0; i < std::size(options); i++)
      writers[i] = std::thread([&, i] {
        std::string output = std::string(directory) + "\\writers_" + std::to_string(i) + ".bin";
        const char *args[] = {
            "env", "-u", "DXMT_AIRCONV_SKIP", "-u", "DXMT_MESH_GRID_LIMIT", store.c_str(), "DXMT_SHADER_CACHE=1",
            options[i], loader_path, exe, argc > 1 ? argv[1] : "dxil", "writers", output.c_str(), nullptr
        };
        statuses[i] = spawn(const_cast<char *const *>(args), TRUE);
      });
    for (auto &writer : writers)
      writer.join();
    for (size_t i = 0; i < std::size(options); i++) {
      expect(statuses[i] == 0, "writer %zu ended with %ld", i, statuses[i]);
      std::ifstream file(std::string(directory) + "\\writers_" + std::to_string(i) + ".bin", std::ios::binary);
      std::string bytes{std::istreambuf_iterator<char>(file), {}};
      expect(bytes == reference, "writer %zu pixels or buffers differ", i);
      run("warm", false, options[i]);
    }
    run("grid", false, "DXMT_MESH_GRID_LIMIT=2");
    run("warm", false, "DXMT_MESH_GRID_LIMIT=2");
  }
  HeapFree(GetProcessHeap(), 0, loader_path);
  auto find = FindFirstFileA((std::string(directory) + "\\*").c_str(), &found);
  if (find != INVALID_HANDLE_VALUE) {
    do {
      if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        DeleteFileA((std::string(directory) + "\\" + found.cFileName).c_str());
    } while (FindNextFileA(find, &found));
    FindClose(find);
  }
  RemoveDirectoryA(directory);
  return verdict();
}
