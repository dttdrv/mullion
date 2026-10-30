// contract: a library preserves compiled named pipelines across serialization and rejects a different
// description. "If the input library blob is empty, then the initial content of the library is empty."
// (Microsoft Learn, ID3D12Device1::CreatePipelineLibrary, pLibraryBlob); "Overwriting is not supported."
// (ID3D12PipelineLibrary::StorePipeline, pName); "if the input description doesn't match the data in the library"
// gives E_INVALIDARG (LoadComputePipeline, Return value). Serialize requires "at least the size returned from
// GetSerializedSize" (Serialize, DataSizeInBytes). values and write masks are generated from seed argv[2]; the
// loaded shaders must write those values and preserve the channels outside their masks. public references may
// reach zero while a library keeps the pipeline and its private data alive (vkd3d-proton test_pipeline_library).
// "cache compiled PSOs to disk and avoid costly shader compilation during subsequent runs" (Microsoft Learn,
// D3D12 pipeline state cache sample). "The pipeline stream description is matched against the library database"
// (ID3D12PipelineLibrary1::LoadPipeline). a blob of another driver gives D3D12_ERROR_DRIVER_VERSION_MISMATCH,
// "if the provided data came from an old driver or runtime" (CreatePipelineLibrary, Return value), as vkd3d-proton
// answers, so that a game replaces its cache file. the converter import counts conversion with the disk cache disabled;
// named stream loads must preserve identity, reject AS/MS/depth bounds changes, and draw the same generated values.
// a mesh shader's payload comes from DispatchMesh: "This parameter provides the same input values for the entire
// Mesh shader thread group." (D3D12 mesh shader specification, Mesh Payload). its output arrays are another matter:
// "Each array cannot be read from" (Shared Output Arrays), yet DXC compiles such a read, to one of a parameter the
// function then keeps. the amplified shader has one, masked by a payload field that is 0: what it draws does not
// depend on what the read gives, and it must still become a pipeline.
#include "d3d12_test.hpp"
#include <array>
#include <random>
#include <limits>
#include <fstream>
#include <iterator>
#include "../../src/winemetal/winemetal.h"
#include "../../src/util/sha1/sha1_util.hpp"
#include "../../src/airconv/airconv_public.h"
#include <thread>
#include <type_traits>

static const char hlsl[] = R"hlsl(
cbuffer constants : register(b0) { uint number; };
RWStructuredBuffer<uint> result : register(u0);
float4 vs(uint id : SV_VertexID) : SV_Position { return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0, 1); }
uint4 ps() : SV_Target { return uint4(number + BIAS, number + BIAS + 1, number + BIAS + 2, number + BIAS + 3); }
[numthreads(1, 1, 1)] void cs() { result[0] = number + BIAS; }
)hlsl";

// the imported converter is observed directly; Windows has no such import.
alignas(sizeof(LONG64)) static volatile LONG64 conversions;
static decltype(&SM50Compile) original_compile;
static decltype(&MTLDevice_newRenderPipelineState) original_render;
static decltype(&DispatchData_alloc_init) original_data;
static bool fail_data;

static obj_handle_t
make_data(uint64_t data, uint64_t size) {
  return fail_data ? 0 : original_data(data, size);
}
static HANDLE pipeline_entered, operation_done;

static obj_handle_t
probe_render(obj_handle_t device, const WMTRenderPipelineInfo *info, obj_handle_t *error) {
  if (pipeline_entered) {
    SetEvent(pipeline_entered);
    expect(WaitForSingleObject(operation_done, 60000) == WAIT_OBJECT_0,
           "a cold pipeline load held the library lock through Metal pipeline creation");
  }
  return original_render(device, info, error);
}

static int
count_compile(sm50_shader_t shader, SM50_SHADER_COMPILATION_ARGUMENT_DATA *args, const char *name,
              sm50_bitcode_t *bitcode, sm50_error_t *error) {
  InterlockedIncrement64(&conversions);
  return original_compile(shader, args, name, bitcode, error);
}

static bool
count_imports() {
  auto module = (BYTE *)GetModuleHandleA("d3d12core.dll");
  if (!GetModuleHandleA("winemetal.dll") || !module)
    return false;
  auto dos = (IMAGE_DOS_HEADER *)module;
  auto nt = (IMAGE_NT_HEADERS *)(module + dos->e_lfanew);
  auto imports = (IMAGE_IMPORT_DESCRIPTOR *)(module +
      nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
  UINT hooked = 0;
  for (; imports->Name; imports++) {
    auto names = (IMAGE_THUNK_DATA *)(module + imports->OriginalFirstThunk);
    auto functions = (IMAGE_THUNK_DATA *)(module + imports->FirstThunk);
    for (; names->u1.AddressOfData; names++, functions++) {
      if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
        continue;
      auto name = (IMAGE_IMPORT_BY_NAME *)(module + names->u1.AddressOfData);
      uintptr_t hook;
      if (!strcmp((const char *)name->Name, "SM50Compile")) {
        original_compile = reinterpret_cast<decltype(original_compile)>(functions->u1.Function);
        hook = uintptr_t(count_compile);
      } else if (!strcmp((const char *)name->Name, "MTLDevice_newRenderPipelineState")) {
        original_render = reinterpret_cast<decltype(original_render)>(functions->u1.Function);
        hook = uintptr_t(probe_render);
      } else if (!strcmp((const char *)name->Name, "DispatchData_alloc_init")) {
        original_data = reinterpret_cast<decltype(original_data)>(functions->u1.Function);
        hook = uintptr_t(make_data);
      } else
        continue;
      DWORD protection;
      if (!expect(VirtualProtect(&functions->u1.Function, sizeof(functions->u1.Function), PAGE_READWRITE, &protection),
                  "cannot count conversions"))
        return false;
      functions->u1.Function = hook;
      if (!expect(VirtualProtect(&functions->u1.Function, sizeof(functions->u1.Function), protection, &protection),
                  "cannot restore import protection"))
        return false;
      hooked++;
    }
  }
  return expect(hooked == 3, "%u observed imports, want converter, pipeline creation and compiled data", hooked);
}

template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename T> struct alignas(void *) Subobject {
  D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
  T data{};
};

struct Stream {
  Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> root;
  Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE> vs;
  Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
  Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS, D3D12_SHADER_BYTECODE> cs;
  Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, D3D12_SHADER_BYTECODE> as;
  Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> ms;
  Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1, D3D12_DEPTH_STENCIL_DESC1> depth;
  Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> targets;
  Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> raster;
  Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> blend;
  Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> mask;
  Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE> topology;
  Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT, D3D12_INPUT_LAYOUT_DESC> input;
  Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> samples;
};

static const char mesh_hlsl[] = R"hlsl(
cbuffer constants : register(b0) { uint number; };
struct V { float4 pos : SV_Position; nointerpolation uint value : DATA; };
struct Payload { uint value, none; };
[numthreads(1, 1, 1)] void amplify() { Payload payload = {BIAS, 0}; DispatchMesh(1, 1, 1, payload); }
[numthreads(1, 1, 1)] [outputtopology("triangle")]
void mesh(
#ifdef AMPLIFIED
  in payload Payload incoming,
#endif
  out vertices V verticesOut[3], out indices uint3 indicesOut[1]) {
  SetMeshOutputCounts(3, 1);
  for (uint id = 0; id < 3; id++) {
    verticesOut[id].pos = float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0, 1);
    verticesOut[id].value = BIAS;
#ifdef AMPLIFIED
    verticesOut[id].value = BIAS + incoming.value + (verticesOut[id].value & incoming.none);
#endif
  }
  indicesOut[0] = uint3(0, 1, 2);
}
uint4 pixel(V value) : SV_Target {
  return uint4(number + value.value, number + value.value + 1, number + value.value + 2, number + value.value + 3);
}
)hlsl";

static void STDMETHODCALLTYPE
destroyed(void *data) {
  ++*(UINT *)data;
}

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const UINT seed = argc > 2 ? strtoul(argv[2], nullptr, 0) : 9;
  std::mt19937 random(seed);
  const UINT count = D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT + 1;
  std::array<UINT, count> bias{}, number{}, masks{};
  std::array<std::string, count> ps, cs;
  auto vs = compiler.compile(hlsl, "vs", "vs", {"BIAS=0"});
  for (UINT i = 0; i < count; i++) {
    bias[i] = random(), number[i] = random();
    masks[i] = i == 0 ? 0 : i == 1 ? D3D12_COLOR_WRITE_ENABLE_ALL : random() % (D3D12_COLOR_WRITE_ENABLE_ALL + 1);
    auto defines = std::vector<std::string>{"BIAS=" + std::to_string(bias[i]) + "u"};
    ps[i] = compiler.compile(hlsl, "ps", "ps", defines);
    cs[i] = compiler.compile(hlsl, "cs", "cs", defines);
    if (vs.empty() || ps[i].empty() || cs[i].empty()) {
      printf("failed: HLSL did not compile, seed %u variant %u\n", seed, i);
      return 1;
    }
  }

  std::array<std::string, 2> mesh, amplified_mesh, amplification;
  std::string mesh_pixel;
  if (compiler.dxc) {
    mesh_pixel = compiler.compile(mesh_hlsl, "pixel", "ps_6_5", {"BIAS=0"});
    for (UINT i = 0; i < mesh.size(); i++) {
      auto defines = std::vector<std::string>{"BIAS=" + std::to_string(bias[i]) + "u"};
      mesh[i] = compiler.compile(mesh_hlsl, "mesh", "ms_6_5", defines);
      amplification[i] = compiler.compile(mesh_hlsl, "amplify", "as_6_5", defines);
      defines.push_back("AMPLIFIED=1");
      amplified_mesh[i] = compiler.compile(mesh_hlsl, "mesh", "ms_6_5", defines);
      if (mesh_pixel.empty() || mesh[i].empty() || amplified_mesh[i].empty() || amplification[i].empty()) {
        printf("failed: mesh HLSL did not compile, seed %u variant %u\n", seed, i);
        return 1;
      }
    }
  }

  step("seed %u: create and validate an empty library", seed);
  SetEnvironmentVariableA("MULLION_SHADER_CACHE", "0");
  ComPtr<ID3D12Device2> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  const bool instrumented = count_imports();
  if (argc > 3) {
    step("seed %u: load persisted compute pipeline in another process", seed);
    std::ifstream input(argv[3], std::ios::binary);
    std::vector<char> data{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (!expect(!input.bad() && !data.empty(), "the persisted library was not read"))
      return verdict();
    ComPtr<ID3D12PipelineLibrary> persisted;
    CHECK(device->CreatePipelineLibrary(data.data(), data.size(), IID_PPV_ARGS(&persisted)));
    D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
    params[0].Constants = {0, 0, 1};
    auto rs = root_signature(device.Get(), {2, params, 0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT});
    if (!expect(rs, "the child root signature was not made"))
      return verdict();
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(cs[0])};
    ComPtr<ID3D12PipelineState> loaded;
    CHECK(persisted->LoadComputePipeline(L"concurrent", &desc, IID_PPV_ARGS(&loaded)));
    if (instrumented)
      expect(conversions == 0, "another process converted %lld shaders", conversions);
    return verdict();
  }
  if (instrumented) {
    step("seed %u: compiled-data bridge bounds", seed);
    auto module = GetModuleHandleA("winemetal.dll");
    auto make = (decltype(&DispatchData_alloc_init))GetProcAddress(module, "DispatchData_alloc_init");
    auto fetch = (decltype(&DispatchData_getBytes))GetProcAddress(module, "DispatchData_getBytes");
    auto release = (decltype(&NSObject_release))GetProcAddress(module, "NSObject_release");
    if (!expect(make && fetch && release, "the compiled-data bridge exports are missing"))
      return verdict();
    auto data = make(uint64_t(bias.data()), sizeof(bias));
    std::array<UINT, count + 1> copied;
    for (size_t length : {size_t(0), sizeof(bias) - 1, sizeof(bias), sizeof(bias) + 1}) {
      copied.fill(~0u);
      auto size = fetch(data, copied.data(), length);
      expect(size == sizeof(bias), "the compiled-data size is %llu, want %zu", size, sizeof(bias));
      for (UINT i = 0; i < copied.size(); i++) {
        auto want = length >= sizeof(bias) && i < bias.size() ? bias[i] : ~0u;
        expect(copied[i] == want, "compiled data word %u: %u, want %u", i, copied[i], want);
      }
    }
    for (size_t length : {size_t(0), sizeof(bias)})
      expect(fetch(data, nullptr, length) == sizeof(bias), "compiled data size query failed");
    release(data);
  }
  ComPtr<ID3D12PipelineLibrary1> library;
  HRESULT hr = device->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&library));
  if (!expect(hr == S_OK && library, "empty library: %08lx, want S_OK", hr))
    return verdict();
  hr = device->CreatePipelineLibrary(nullptr, 0, __uuidof(ID3D12PipelineLibrary), nullptr);
  expect(hr == S_FALSE, "validate an empty library: %08lx, want S_FALSE", hr);
  std::vector<char> empty(library->GetSerializedSize());
  CHECK(library->Serialize(empty.data(), empty.size()));
  ComPtr<ID3D12PipelineLibrary> empty_copy;
  CHECK(device->CreatePipelineLibrary(empty.data(), empty.size(), IID_PPV_ARGS(&empty_copy)));
  empty_copy.Reset();
  hr = device->CreatePipelineLibrary(nullptr, empty.size(), __uuidof(ID3D12PipelineLibrary), nullptr);
  expect(hr == E_INVALIDARG, "a null nonempty blob was accepted: %08lx", hr);
  // another driver's blob, short or long, is a version mismatch, which games answer by building a new library
  const char other_driver[] = "another driver's pipeline library, which is not this one's";
  for (SIZE_T length : {SIZE_T(4), sizeof(other_driver)}) {
    hr = device->CreatePipelineLibrary(other_driver, length, __uuidof(ID3D12PipelineLibrary), nullptr);
    expect(hr == D3D12_ERROR_DRIVER_VERSION_MISMATCH, "a foreign blob of %llu bytes: %08lx, want %08lx",
           (UINT64)length, hr, D3D12_ERROR_DRIVER_VERSION_MISMATCH);
  }
  ComPtr<ID3D12Fence> unsupported;
  hr = device->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&unsupported));
  expect(hr == E_NOINTERFACE && !unsupported, "an unsupported library interface was returned: %08lx", hr);
  D3D12_FEATURE_DATA_SHADER_CACHE support{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_CACHE, &support, sizeof(support)));
  expect(support.SupportFlags & D3D12_SHADER_CACHE_SUPPORT_LIBRARY, "the library support flag is missing");
  ComPtr<ID3D12Device1> parent;
  CHECK(library->GetDevice(IID_PPV_ARGS(&parent)));
  expect(parent.Get() == device.Get(), "GetDevice returned another device");
  parent.Reset();
  expect(library->StorePipeline(L"null", nullptr) == E_INVALIDARG, "a null pipeline was stored");
  ComPtr<ID3DDestructionNotifier> notifier;
  auto references = [](auto *object) {
    object->AddRef();
    return object->Release();
  };
  const ULONG library_refs = references(library.Get());
  CHECK(library.As(&notifier));
  expect(references(library.Get()) == library_refs + 1, "the notifier did not share the library's reference count");
  ComPtr<IUnknown> library_identity, notifier_identity;
  CHECK(library.As(&library_identity));
  CHECK(notifier.As(&notifier_identity));
  expect(library_identity.Get() == notifier_identity.Get(), "the notifier has another COM identity");
  library_identity.Reset();
  notifier_identity.Reset();
  UINT notified = 0, removed = 0, cookie;
  CHECK(notifier->RegisterDestructionCallback(destroyed, &notified, nullptr));
  CHECK(notifier->RegisterDestructionCallback(destroyed, &removed, &cookie));
  CHECK(notifier->UnregisterDestructionCallback(cookie));
  expect(notifier->UnregisterDestructionCallback(cookie) == DXGI_ERROR_NOT_FOUND, "a callback was removed twice");
  expect(notifier->RegisterDestructionCallback(nullptr, nullptr, nullptr) == DXGI_ERROR_INVALID_CALL,
         "a null callback was accepted");
  notifier.Reset();

  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[0].Constants = {0, 0, 1};
  const auto root_flags =
      D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT;
  auto rs = root_signature(device.Get(), {2, params, 0, nullptr, root_flags});
  auto equivalent = root_signature(device.Get(), {2, params, 0, nullptr, root_flags});
  if (!expect(rs && equivalent, "root signatures were not made"))
    return verdict();
  std::array<D3D12_GRAPHICS_PIPELINE_STATE_DESC, count> graphics{};
  std::array<D3D12_COMPUTE_PIPELINE_STATE_DESC, count> computes{};
  std::array<std::array<D3D12_INPUT_ELEMENT_DESC, D3D12_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT>, count> inputs{};
  std::array<std::wstring, count> names;
  for (UINT i = 0; i < count; i++) {
    step("seed %u variant %u: store and match descriptors", seed, i);
    names[i] = std::wstring(i * D3D12_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT, L'\u03bb') + std::to_wstring(i);
    auto &g = graphics[i];
    g.pRootSignature = rs.Get(), g.VS = bytecode(vs), g.PS = bytecode(ps[i]);
    g.BlendState.RenderTarget[0].RenderTargetWriteMask = masks[i];
    g.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
    g.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    g.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    g.NumRenderTargets = i % D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT + 1;
    g.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_UINT, g.SampleDesc.Count = 1;
    const UINT elements = i == 0   ? 0
                          : i == 1 ? 1
                          : i == 2 ? D3D12_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT - 1
                                   : D3D12_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT;
    for (UINT e = 0; e < elements; e++)
      inputs[i][e] = {"UNUSED", e, DXGI_FORMAT_R32_UINT, 0, UINT(e * sizeof(UINT))};
    g.InputLayout = {inputs[i].data(), elements};
    computes[i] = {rs.Get(), bytecode(cs[i])};
    ComPtr<ID3D12PipelineState> drawn, computed, loaded;
    CHECK(device->CreateGraphicsPipelineState(&g, IID_PPV_ARGS(&drawn)));
    CHECK(device->CreateComputePipelineState(&computes[i], IID_PPV_ARGS(&computed)));
    CHECK(library->StorePipeline(names[i].c_str(), drawn.Get()));
    CHECK(library->StorePipeline((names[i] + L"c").c_str(), computed.Get()));
    expect(library->StorePipeline(names[i].c_str(), computed.Get()) == E_INVALIDARG, "a duplicate name was accepted");
    CHECK(library->LoadGraphicsPipeline(names[i].c_str(), &g, IID_PPV_ARGS(&loaded)));
    expect(drawn.Get() == loaded.Get(), "StorePipeline did not preserve object identity");
    loaded.Reset();
    auto relocated = computes[i];
    std::string code = cs[i];
    relocated.CS = bytecode(code), relocated.pRootSignature = equivalent.Get();
    CHECK(library->LoadComputePipeline((names[i] + L"c").c_str(), &relocated, IID_PPV_ARGS(&loaded)));
    expect(computed.Get() == loaded.Get(), "equal data at other addresses did not match");
    loaded.Reset();
    expect(library->LoadComputePipeline(names[i].c_str(), &computes[i], IID_PPV_ARGS(&loaded)) == E_INVALIDARG,
           "a graphics pipeline matched a compute descriptor");
    expect(library->LoadGraphicsPipeline(L"absent", &g, IID_PPV_ARGS(&loaded)) == E_INVALIDARG,
           "an absent name was accepted");
    UINT field = 0;
    auto vary = [&](auto &...values) {
      (
          [&] {
            auto saved = values;
            using T = std::decay_t<decltype(values)>;
            values = T(values + 1);
            hr = library->LoadGraphicsPipeline(names[i].c_str(), &g, IID_PPV_ARGS(&loaded));
            expect(hr == E_INVALIDARG, "field %u changed but matched: %08lx", field, hr);
            loaded.Reset();
            values = saved;
            field++;
          }(),
          ...);
    };
    auto &[fill, cull, front, depth_bias, clamp, slope, clip, multisample, aa, forced, conservative] =
        g.RasterizerState;
    vary(fill, cull, front, depth_bias, clamp, slope, clip, multisample, aa, forced, conservative);
    for (UINT rt = 0; rt < g.NumRenderTargets; rt++) {
      auto &[blend, logic, src, dst, op, src_alpha, dst_alpha, op_alpha, logic_op, mask] =
          g.BlendState.RenderTarget[rt];
      vary(blend, logic, src, dst, op, src_alpha, dst_alpha, op_alpha, logic_op, mask);
    }
    auto &[depth_enabled, depth_write, depth_func, stencil, read_mask, write_mask, front_face, back_face] =
        g.DepthStencilState;
    vary(depth_enabled, depth_write, depth_func, stencil, read_mask, write_mask);
    for (auto *face : {&front_face, &back_face}) {
      auto &[fail, depth_fail, pass, compare] = *face;
      vary(fail, depth_fail, pass, compare);
    }
    vary(g.BlendState.AlphaToCoverageEnable, g.BlendState.IndependentBlendEnable, g.SampleMask, g.IBStripCutValue,
         g.PrimitiveTopologyType, g.RTVFormats[0], g.DSVFormat, g.SampleDesc.Count, g.SampleDesc.Quality, g.NodeMask,
         g.Flags);
    auto old_root = g.pRootSignature;
    g.pRootSignature = nullptr;
    expect(library->LoadGraphicsPipeline(names[i].c_str(), &g, IID_PPV_ARGS(&loaded)) == E_INVALIDARG,
           "a different root signature matched");
    g.pRootSignature = old_root;
    if (elements) {
      auto &[semantic, index, format, slot, offset, classification, rate] = inputs[i][0];
      vary(index, format, slot, offset, classification, rate);
      semantic = "OTHER";
      expect(library->LoadGraphicsPipeline(names[i].c_str(), &g, IID_PPV_ARGS(&loaded)) == E_INVALIDARG,
             "another input semantic matched");
      semantic = nullptr;
      expect(library->LoadGraphicsPipeline(names[i].c_str(), &g, IID_PPV_ARGS(&loaded)) == E_INVALIDARG,
             "a null input semantic matched");
      semantic = "UNUSED";
    }
    auto old_layout = g.InputLayout;
    for (UINT length :
         {0u, D3D12_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT - 1u, UINT(D3D12_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT),
          D3D12_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT + 1u}) {
      std::array<D3D12_INPUT_ELEMENT_DESC, D3D12_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT + 1> relocated{};
      for (UINT e = 0; e < relocated.size(); e++)
        relocated[e] = {"UNUSED", e, DXGI_FORMAT_R32_UINT, 0, UINT(e * sizeof(UINT))};
      g.InputLayout = {relocated.data(), length};
      hr = library->LoadGraphicsPipeline(names[i].c_str(), &g, IID_PPV_ARGS(&loaded));
      expect(hr == (length == elements ? S_OK : E_INVALIDARG), "%u input elements: %08lx", length, hr);
      loaded.Reset();
    }
    g.InputLayout = {nullptr, 1};
    expect(library->LoadGraphicsPipeline(names[i].c_str(), &g, IID_PPV_ARGS(&loaded)) == E_INVALIDARG,
           "a null input array matched");
    g.InputLayout = old_layout;
    auto old_targets = g.NumRenderTargets;
    for (UINT targets : {0u, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT - 1u, UINT(D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT),
                         D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT + 1u}) {
      g.NumRenderTargets = targets;
      hr = library->LoadGraphicsPipeline(names[i].c_str(), &g, IID_PPV_ARGS(&loaded));
      expect(hr == (targets == old_targets ? S_OK : E_INVALIDARG), "%u targets: %08lx", targets, hr);
      loaded.Reset();
    }
    g.NumRenderTargets = old_targets;
    auto saved_format = g.RTVFormats[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT - 1];
    g.RTVFormats[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT - 1] = DXGI_FORMAT_R8G8B8A8_UNORM;
    hr = library->LoadGraphicsPipeline(names[i].c_str(), &g, IID_PPV_ARGS(&loaded));
    expect(hr == (old_targets == D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT ? E_INVALIDARG : S_OK),
           "last render target format: %08lx", hr);
    loaded.Reset();
    g.RTVFormats[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT - 1] = saved_format;
    std::string relocated_vs = vs, relocated_ps = ps[i];
    auto relocated_graphics = g;
    relocated_graphics.VS = bytecode(relocated_vs), relocated_graphics.PS = bytecode(relocated_ps);
    relocated_graphics.pRootSignature = equivalent.Get();
    CHECK(library->LoadGraphicsPipeline(names[i].c_str(), &relocated_graphics, IID_PPV_ARGS(&loaded)));
    loaded.Reset();
    auto &target_blend = g.BlendState.RenderTarget[0];
    for (size_t b = offsetof(D3D12_RENDER_TARGET_BLEND_DESC, RenderTargetWriteMask) +
                    sizeof(target_blend.RenderTargetWriteMask);
         b < sizeof(target_blend); b++)
      ((unsigned char *)&target_blend)[b] = (unsigned char)random();
    CHECK(library->LoadGraphicsPipeline(names[i].c_str(), &g, IID_PPV_ARGS(&loaded)));
    loaded.Reset();
    auto changed = computes[i];
    changed.CS = bytecode(cs[(i + 1) % count]);
    expect(library->LoadComputePipeline((names[i] + L"c").c_str(), &changed, IID_PPV_ARGS(&loaded)) == E_INVALIDARG,
           "another shader matched");
    changed = computes[i];
    changed.CS.pShaderBytecode = nullptr;
    expect(library->LoadComputePipeline((names[i] + L"c").c_str(), &changed, IID_PPV_ARGS(&loaded)) == E_INVALIDARG,
           "a null nonempty shader matched");
    changed.CS = {};
    expect(library->LoadComputePipeline((names[i] + L"c").c_str(), &changed, IID_PPV_ARGS(&loaded)) == E_INVALIDARG,
           "an empty shader matched");
    for (auto shader : {&g.VS, &g.PS, &g.DS, &g.HS, &g.GS}) {
      auto saved = *shader;
      *shader = bytecode(cs[i]);
      expect(library->LoadGraphicsPipeline(names[i].c_str(), &g, IID_PPV_ARGS(&loaded)) == E_INVALIDARG,
             "another graphics shader matched");
      *shader = saved;
    }
  }

  step("seed %u: stream output descriptors and a pipeline without targets", seed);
  {
    auto g = graphics[0];
    g.PS = {}, g.NumRenderTargets = 0, g.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D12PipelineState> made, loaded;
    CHECK(device->CreateGraphicsPipelineState(&g, IID_PPV_ARGS(&made)));
    CHECK(library->StorePipeline(L"no targets", made.Get()));
    CHECK(library->LoadGraphicsPipeline(L"no targets", &g, IID_PPV_ARGS(&loaded)));
    loaded.Reset();
    made.Reset();
    std::array<float, 4> position{};
    std::array<D3D12_SO_DECLARATION_ENTRY, 2> entries{{{0, "SV_Position", 0, 0, BYTE(position.size()), 0}, {}}};
    auto &entry = entries[0];
    std::array<UINT, D3D12_SO_BUFFER_SLOT_COUNT + 1> strides{};
    strides.fill(sizeof(position));
    g.StreamOutput = {&entry, 1, strides.data(), D3D12_SO_BUFFER_SLOT_COUNT, D3D12_SO_NO_RASTERIZED_STREAM};
    CHECK(device->CreateGraphicsPipelineState(&g, IID_PPV_ARGS(&made)));
    CHECK(library->StorePipeline(L"stream output", made.Get()));
    auto &[stream, semantic, index, start, components, slot] = entry;
    UINT field = 0;
    auto vary = [&](auto &...values) {
      (
          [&] {
            auto saved = values;
            using T = std::decay_t<decltype(values)>;
            values = T(values + 1);
            hr = library->LoadGraphicsPipeline(L"stream output", &g, IID_PPV_ARGS(&loaded));
            expect(hr == E_INVALIDARG, "stream output field %u matched: %08lx", field++, hr);
            loaded.Reset();
            values = saved;
          }(),
          ...);
    };
    vary(stream, index, start, components, slot, g.StreamOutput.NumEntries, g.StreamOutput.NumStrides,
         g.StreamOutput.RasterizedStream);
    for (UINT i = 0; i < D3D12_SO_BUFFER_SLOT_COUNT; i++)
      vary(strides[i]);
    for (auto name : {"OTHER", (const char *)nullptr}) {
      semantic = name;
      expect(library->LoadGraphicsPipeline(L"stream output", &g, IID_PPV_ARGS(&loaded)) == E_INVALIDARG,
             "another stream output semantic matched");
    }
    semantic = "SV_Position";
    for (UINT length :
         {0u, D3D12_SO_BUFFER_SLOT_COUNT - 1u, UINT(D3D12_SO_BUFFER_SLOT_COUNT), D3D12_SO_BUFFER_SLOT_COUNT + 1u}) {
      g.StreamOutput.NumStrides = length;
      hr = library->LoadGraphicsPipeline(L"stream output", &g, IID_PPV_ARGS(&loaded));
      expect(hr == (length == D3D12_SO_BUFFER_SLOT_COUNT ? S_OK : E_INVALIDARG), "%u stream strides: %08lx", length,
             hr);
      loaded.Reset();
    }
    g.StreamOutput.NumStrides = D3D12_SO_BUFFER_SLOT_COUNT;
    g.StreamOutput.pSODeclaration = nullptr;
    expect(library->LoadGraphicsPipeline(L"stream output", &g, IID_PPV_ARGS(&loaded)) == E_INVALIDARG,
           "a null stream output declaration matched");
    g.StreamOutput.pSODeclaration = &entry, g.StreamOutput.pBufferStrides = nullptr;
    expect(library->LoadGraphicsPipeline(L"stream output", &g, IID_PPV_ARGS(&loaded)) == E_INVALIDARG,
           "null stream output strides matched");
  }

  step("seed %u: stream loading and descriptor identity", seed);
  Stream stream;
  auto g = graphics[0];
  stream.root.data = g.pRootSignature, stream.vs.data = g.VS, stream.ps.data = g.PS;
  stream.targets.data.NumRenderTargets = g.NumRenderTargets;
  memcpy(stream.targets.data.RTFormats, g.RTVFormats, sizeof(g.RTVFormats));
  stream.raster.data = g.RasterizerState, stream.blend.data = g.BlendState, stream.mask.data = g.SampleMask;
  stream.topology.data = g.PrimitiveTopologyType, stream.input.data = g.InputLayout, stream.samples.data = g.SampleDesc;
  D3D12_PIPELINE_STATE_STREAM_DESC stream_desc{sizeof(stream), &stream};
  ComPtr<ID3D12PipelineState> stream_pipeline, legacy_pipeline;
  CHECK(library->LoadPipeline(names[0].c_str(), &stream_desc, IID_PPV_ARGS(&stream_pipeline)));
  CHECK(library->LoadGraphicsPipeline(names[0].c_str(), &g, IID_PPV_ARGS(&legacy_pipeline)));
  expect(stream_pipeline.Get() == legacy_pipeline.Get(), "stream and legacy loads returned different objects");
  stream_pipeline.Reset(), legacy_pipeline.Reset();
  stream.depth.data.DepthBoundsTestEnable = TRUE;
  expect(library->LoadPipeline(names[0].c_str(), &stream_desc, IID_PPV_ARGS(&stream_pipeline)) == E_INVALIDARG,
         "another depth bounds state matched");
  stream.depth.data.DepthBoundsTestEnable = FALSE;
  stream.vs.data = {}, stream.ps.data = {}, stream.cs.data = computes[0].CS;
  CHECK(library->LoadPipeline((names[0] + L"c").c_str(), &stream_desc, IID_PPV_ARGS(&stream_pipeline)));
  stream_pipeline.Reset();
  expect(library->LoadPipeline(nullptr, &stream_desc, IID_PPV_ARGS(&stream_pipeline)) == E_INVALIDARG,
         "a null stream name was accepted");
  expect(library->LoadPipeline(names[0].c_str(), nullptr, IID_PPV_ARGS(&stream_pipeline)) == E_INVALIDARG,
         "a null stream descriptor was accepted");
  std::vector<Stream> streams;
  std::vector<std::wstring> stream_names;
  std::vector<UINT> stream_values;
  D3D12_FEATURE_DATA_D3D12_OPTIONS2 depth_support{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &depth_support, sizeof(depth_support)));
  stream.vs.data = g.VS, stream.ps.data = g.PS, stream.cs.data = {};
  stream.blend.data.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  for (UINT depth = 0; depth <= UINT(depth_support.DepthBoundsTestSupported != FALSE); depth++) {
    stream.depth.data.DepthBoundsTestEnable = depth;
    streams.push_back(stream);
    stream_names.push_back(L"depth " + std::to_wstring(depth));
    stream_values.push_back(number[0] + bias[0]);
  }
  D3D12_FEATURE_DATA_D3D12_OPTIONS7 mesh_support{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &mesh_support, sizeof(mesh_support)));
  if (compiler.dxc && mesh_support.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED) {
    stream.vs.data = {}, stream.input.data = {}, stream.ps.data = bytecode(mesh_pixel);
    stream.depth.data.DepthBoundsTestEnable = FALSE;
    for (UINT i = 0; i < mesh.size(); i++)
      for (bool use_as : {false, true}) {
        stream.as.data = use_as ? bytecode(amplification[i]) : D3D12_SHADER_BYTECODE{};
        stream.ms.data = bytecode(use_as ? amplified_mesh[i] : mesh[i]);
        streams.push_back(stream);
        stream_names.push_back(L"mesh " + std::to_wstring(i) + (use_as ? L" amplified" : L""));
        stream_values.push_back(number[0] + bias[i] * (use_as ? 2u : 1u));
      }
  } else
    printf("unchecked: mesh stream pipelines require DXIL and mesh shader support\n");
  for (UINT i = 0; i < streams.size(); i++) {
    step("seed %u stream %u: store, load and vary stream keys", seed, i);
    auto &stored_stream = streams[i];
    D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(stored_stream), &stored_stream};
    ComPtr<ID3D12PipelineState> made, loaded;
    hr = device->CreatePipelineState(&desc, IID_PPV_ARGS(&made));
    if (!expect(hr == S_OK && made, "CreatePipelineState returned %08lx for %ls", hr, stream_names[i].c_str()))
      return verdict();
    CHECK(library->StorePipeline(stream_names[i].c_str(), made.Get()));
    CHECK(library->LoadPipeline(stream_names[i].c_str(), &desc, IID_PPV_ARGS(&loaded)));
    expect(loaded.Get() == made.Get(), "a stream load returned another pipeline");
    loaded.Reset();
    stored_stream.depth.data.DepthBoundsTestEnable ^= TRUE;
    expect(library->LoadPipeline(stream_names[i].c_str(), &desc, IID_PPV_ARGS(&loaded)) == E_INVALIDARG,
           "another stream depth bounds state matched");
    stored_stream.depth.data.DepthBoundsTestEnable ^= TRUE;
    if (stored_stream.ms.data.BytecodeLength) {
      for (auto *shader : {&stored_stream.as.data, &stored_stream.ms.data}) {
        auto saved = *shader;
        *shader = shader == &stored_stream.as.data ? bytecode(amplification[1]) : bytecode(mesh[1]);
        if (saved.pShaderBytecode == shader->pShaderBytecode)
          *shader = {};
        expect(library->LoadPipeline(stream_names[i].c_str(), &desc, IID_PPV_ARGS(&loaded)) == E_INVALIDARG,
               "another mesh or amplification shader matched");
        *shader = saved;
      }
    }
  }
  auto saved_size = stream_desc.SizeInBytes;
  stream_desc.SizeInBytes = sizeof(stream.root) - 1;
  expect(library->LoadPipeline(names[0].c_str(), &stream_desc, IID_PPV_ARGS(&stream_pipeline)) == E_INVALIDARG,
         "a truncated stream subobject was accepted");
  stream_desc.SizeInBytes = saved_size;

  step("seed %u: null arguments and another device", seed);
  ComPtr<ID3D12PipelineState> pipeline;
  expect(library->LoadComputePipeline(nullptr, &computes[0], IID_PPV_ARGS(&pipeline)) == E_INVALIDARG,
         "a null name was accepted");
  expect(library->LoadGraphicsPipeline(names[0].c_str(), nullptr, IID_PPV_ARGS(&pipeline)) == E_INVALIDARG,
         "a null descriptor was accepted");
  ComPtr<ID3D12Device> other_device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&other_device)));
  auto other_root = root_signature(other_device.Get(), {2, params, 0, nullptr, root_flags});
  if (!expect(other_root, "the other device's root signature was not made"))
    return verdict();
  auto foreign = computes[0];
  foreign.pRootSignature = other_root.Get();
  CHECK(other_device->CreateComputePipelineState(&foreign, IID_PPV_ARGS(&pipeline)));
  expect(library->StorePipeline(L"foreign", pipeline.Get()) == E_INVALIDARG,
         "a pipeline from another device was stored");
  pipeline.Reset();
  other_root.Reset();
  other_device.Reset();
  step("seed %u: concurrent stores", seed);
  CHECK(device->CreateComputePipelineState(&computes[0], IID_PPV_ARGS(&pipeline)));
  std::array<HRESULT, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> answers;
  std::vector<std::thread> threads;
  for (UINT i = 0; i < answers.size(); i++)
    threads.emplace_back([&, i] { answers[i] = library->StorePipeline(L"concurrent", pipeline.Get()); });
  for (auto &thread : threads)
    thread.join();
  UINT stored = 0;
  for (auto answer : answers) {
    stored += answer == S_OK;
    expect(answer == S_OK || answer == E_INVALIDARG, "concurrent store: %08lx", answer);
  }
  expect(stored == 1, "%u threads stored the same name, want one", stored);
  if (instrumented) {
    std::vector<char> data(library->GetSerializedSize());
    CHECK(library->Serialize(data.data(), data.size()));
    const auto offset = strlen(empty.data()) + 1 + sizeof(uint32_t) + sizeof(uint64_t) + sizeof(mullion::Sha1Digest);
    uint64_t before, after;
    memcpy(&before, data.data() + offset, sizeof(before));
    CHECK(library->StorePipeline(L"shared", pipeline.Get()));
    data.resize(library->GetSerializedSize());
    CHECK(library->Serialize(data.data(), data.size()));
    memcpy(&after, data.data() + offset, sizeof(after));
    expect(after == before, "another name duplicated compiled shader data");
  }
  pipeline.Reset();

  step("seed %u: serialized size boundaries and corruption", seed);
  SIZE_T size = library->GetSerializedSize();
  if (!expect(size != 0, "the serialized library is empty"))
    return verdict();
  std::vector<char> bytes(size + 1, '\x7f');
  expect(library->Serialize(nullptr, size) == E_INVALIDARG, "a null serialization buffer was accepted");
  for (SIZE_T length : {SIZE_T(0), size - 1, size, size + 1}) {
    hr = library->Serialize(bytes.data(), length);
    expect(hr == (length < size ? E_INVALIDARG : S_OK), "Serialize of %llu bytes: %08lx", (UINT64)length, hr);
    expect(bytes[size] == '\x7f', "Serialize wrote past GetSerializedSize");
  }
  CHECK(library->Serialize(bytes.data(), size));
  step("seed %u: persist the library for another process", seed);
  char temporary[MAX_PATH], path[MAX_PATH], executable[MAX_PATH];
  if (!expect(GetTempPathA(sizeof(temporary), temporary) && GetTempFileNameA(temporary, "lib", 0, path) &&
              GetModuleFileNameA(nullptr, executable, sizeof(executable)), "temporary library path failed"))
    return verdict();
  {
    std::ofstream file(path, std::ios::binary);
    file.write(bytes.data(), size);
    file.close();
    if (!expect(bool(file), "persisting the library failed")) {
      DeleteFileA(path);
      return verdict();
    }
  }
  std::string command = std::string("\"") + executable + "\" " + (argc > 1 ? argv[1] : "dxil") + " " +
                        std::to_string(seed) + " \"" + path + "\"";
  STARTUPINFOA startup{sizeof(startup)};
  PROCESS_INFORMATION child{};
  auto started =
      CreateProcessA(executable, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &child);
  if (!expect(started, "the persistence child did not start")) {
    DeleteFileA(path);
    return verdict();
  }
  auto waited = WaitForSingleObject(child.hProcess, 60000);
  if (waited != WAIT_OBJECT_0) {
    TerminateProcess(child.hProcess, 1);
    WaitForSingleObject(child.hProcess, 60000);
  }
  DWORD status = 1;
  GetExitCodeProcess(child.hProcess, &status);
  CloseHandle(child.hThread), CloseHandle(child.hProcess);
  expect(DeleteFileA(path), "the persisted library was not removed");
  expect(waited == WAIT_OBJECT_0 && status == 0, "persistence child exit %lu, wait %lu", status, waited);
  library.Reset();
  expect(notified == 1 && removed == 0, "destruction callbacks: %u registered, %u removed", notified, removed);
  auto corrupt = empty;
  for (SIZE_T length = 1; length < corrupt.size(); length++) {
    hr = device->CreatePipelineLibrary(corrupt.data(), length, __uuidof(ID3D12PipelineLibrary), nullptr);
    expect(FAILED(hr), "truncated blob of %llu bytes was accepted", (UINT64)length);
  }
  for (SIZE_T i = 0; i < corrupt.size(); i++) {
    corrupt[i] ^= 1;
    hr = device->CreatePipelineLibrary(corrupt.data(), corrupt.size(), __uuidof(ID3D12PipelineLibrary), nullptr);
    expect(FAILED(hr), "corrupted byte %llu was accepted", (UINT64)i);
    corrupt[i] ^= 1;
  }
  CHECK(device->CreatePipelineLibrary(bytes.data(), size, IID_PPV_ARGS(&library)));

  if (instrumented) {
    step("seed %u: a failed compiled-data allocation", seed);
    ComPtr<ID3D12PipelineLibrary> failed_library;
    fail_data = true;
    hr = device->CreatePipelineLibrary(bytes.data(), size, IID_PPV_ARGS(&failed_library));
    fail_data = false;
    expect(hr == E_OUTOFMEMORY && !failed_library, "compiled-data allocation failure: %08lx", hr);
    expect(conversions > 0, "the cold pipelines were not observed converting");
    InterlockedExchange64(&conversions, 0);
    step("seed %u: checksum-valid incompatible and malformed blobs", seed);
    const auto magic_size = strlen(empty.data()) + 1;
    const auto checksum_at = magic_size + sizeof(uint32_t) + sizeof(uint64_t);
    const auto header_size = checksum_at + sizeof(mullion::Sha1Digest);
    UINT blob_index = 0;
    auto validate = [&](std::vector<char> blob, HRESULT wanted) {
      step("seed %u blob %u: checksum-valid validation", seed, blob_index++);
      mullion::Sha1HashState hash;
      hash.update(blob.data(), checksum_at);
      hash.update(blob.data() + header_size, blob.size() - header_size);
      auto checksum = hash.final();
      memcpy(blob.data() + checksum_at, &checksum, sizeof(checksum));
      hr = device->CreatePipelineLibrary(blob.data(), blob.size(), __uuidof(ID3D12PipelineLibrary), nullptr);
      expect(hr == wanted, "checksum-valid blob: %08lx, want %08lx", hr, wanted);
    };
    auto changed = empty;
    changed.resize(header_size);
    validate(changed, E_INVALIDARG);
    changed = empty;
    changed[magic_size] ^= 1;
    validate(changed, D3D12_ERROR_DRIVER_VERSION_MISMATCH);
    changed = empty;
    changed[magic_size + sizeof(uint32_t)] ^= 1;
    validate(changed, D3D12_ERROR_ADAPTER_NOT_FOUND);
    auto append = [](std::vector<char> &blob, const auto &value) {
      auto begin = (const char *)&value;
      blob.insert(blob.end(), begin, begin + sizeof(value));
    };
    mullion::Sha1Digest key;
    uint64_t shader_size, one = 1;
    const auto shader_at = header_size + sizeof(uint64_t);
    memcpy(&key, bytes.data() + shader_at, sizeof(key));
    memcpy(&shader_size, bytes.data() + shader_at + sizeof(key), sizeof(shader_size));
    auto base = empty;
    memcpy(base.data() + header_size, &one, sizeof(one));
    base.insert(base.end(), bytes.begin() + shader_at,
                bytes.begin() + shader_at + sizeof(key) + sizeof(shader_size) + shader_size);
    auto entry = [&](uint64_t length, uint64_t shaders, const std::wstring &name) {
      auto blob = base;
      append(blob, length), append(blob, key), append(blob, shaders);
      if (shaders == 1)
        append(blob, key);
      auto begin = (const char *)name.data();
      blob.insert(blob.end(), begin, begin + name.size() * sizeof(WCHAR));
      return blob;
    };
    auto truncated = entry(1, 1, L"x");
    const size_t fields[] = {sizeof(uint64_t), sizeof(uint64_t) + sizeof(key),
                             2 * sizeof(uint64_t) + sizeof(key)};
    for (auto end : fields)
      for (size_t length : {end - 1, end}) {
        auto blob = truncated;
        blob.resize(base.size() + length);
        validate(blob, E_INVALIDARG);
      }
    validate(entry(std::numeric_limits<uint64_t>::max(), 1, L""), E_INVALIDARG);
    validate(entry(1, 1, L""), E_INVALIDARG);
    validate(entry(1, std::numeric_limits<uint64_t>::max(), L"x"), E_INVALIDARG);
    validate(entry(1, 1, std::wstring(1, L'\0')), E_INVALIDARG);
    changed = entry(1, 1, L"x");
    auto duplicate = changed;
    changed.insert(changed.end(), duplicate.begin() + base.size(), duplicate.end());
    validate(changed, E_INVALIDARG);
    changed = empty;
    memcpy(changed.data() + header_size, &one, sizeof(one));
    validate(changed, E_INVALIDARG);
    append(changed, key);
    append(changed, one);
    validate(changed, E_INVALIDARG);
    changed.resize(header_size + sizeof(one) + sizeof(key));
    uint64_t zero = 0;
    append(changed, zero);
    validate(changed, E_INVALIDARG);
    validate(entry(1, 0, L"x"), E_INVALIDARG);
    changed = entry(0, 1, L"");
    changed.back() ^= 1;
    validate(changed, E_INVALIDARG);
    changed = base;
    uint64_t two = 2;
    memcpy(changed.data() + header_size, &two, sizeof(two));
    changed.insert(changed.end(), base.begin() + header_size + sizeof(one), base.end());
    validate(changed, E_INVALIDARG);
  }

  if (instrumented) {
    step("seed %u: serialize during a cold graphics load", seed);
    pipeline_entered = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    operation_done = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!expect(pipeline_entered && operation_done, "the library lock probe events were not made"))
      return verdict();
    HRESULT serialized = E_FAIL;
    std::thread serializer([&] {
      if (WaitForSingleObject(pipeline_entered, 60000) == WAIT_OBJECT_0) {
        std::vector<char> snapshot(library->GetSerializedSize());
        serialized = library->Serialize(snapshot.data(), snapshot.size());
        SetEvent(operation_done);
      }
    });
    ComPtr<ID3D12PipelineState> loaded;
    hr = library->LoadGraphicsPipeline(names[0].c_str(), &graphics[0], IID_PPV_ARGS(&loaded));
    SetEvent(pipeline_entered);
    serializer.join();
    expect(hr == S_OK && serialized == S_OK, "cold load %08lx, concurrent serialization %08lx", hr, serialized);
    CloseHandle(pipeline_entered), CloseHandle(operation_done);
    pipeline_entered = operation_done = nullptr;
  }

  step("seed %u: concurrent cold loads of different names", seed);
  answers.fill(E_FAIL);
  threads.clear();
  for (UINT i = 0; i < answers.size(); i++)
    threads.emplace_back([&, i] {
      ComPtr<ID3D12PipelineState> loaded;
      answers[i] = library->LoadComputePipeline((names[i] + L"c").c_str(), &computes[i], IID_PPV_ARGS(&loaded));
    });
  for (auto &thread : threads)
    thread.join();
  for (UINT i = 0; i < answers.size(); i++)
    expect(answers[i] == S_OK, "cold load %u: %08lx", i, answers[i]);

  step("seed %u: public references, device lifetime and private data", seed);
  const ULONG before = references(device.Get());
  hr = library->LoadComputePipeline((names[0] + L"c").c_str(), &computes[0], IID_PPV_ARGS(&unsupported));
  expect(hr == E_NOINTERFACE && !unsupported, "an unsupported pipeline interface was returned: %08lx", hr);
  expect(references(device.Get()) == before, "a failed load retained a public pipeline reference");
  CHECK(library->LoadComputePipeline((names[0] + L"c").c_str(), &computes[0], IID_PPV_ARGS(&pipeline)));
  expect(references(device.Get()) == before + 1, "the public pipeline did not retain its device");
  ComPtr<ID3D12PipelineState> again;
  CHECK(library->LoadComputePipeline((names[0] + L"c").c_str(), &computes[0], IID_PPV_ARGS(&again)));
  expect(pipeline.Get() == again.Get() && references(pipeline.Get()) == 2,
         "two loads did not share two public references");
  ComPtr<ID3D12Fence> fence;
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
  CHECK(pipeline->SetPrivateDataInterface(__uuidof(ID3D12Fence), fence.Get()));
  auto identity = pipeline.Get();
  again.Reset();
  expect(pipeline.Detach()->Release() == 0, "the last public reference did not reach zero");
  expect(references(fence.Get()) == 2, "private data did not survive the last public reference");
  expect(references(device.Get()) == before + 1, "a private pipeline reference retained its device");
  CHECK(library->LoadComputePipeline((names[0] + L"c").c_str(), &computes[0], IID_PPV_ARGS(&pipeline)));
  expect(pipeline.Get() == identity, "reload did not revive the same pipeline");
  pipeline.Reset();

  const UINT width = count + streams.size();
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                                  0,
                                  width,
                                  1,
                                  1,
                                  1,
                                  DXGI_FORMAT_R32G32B32A32_UINT,
                                  {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                  D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                        nullptr, IID_PPV_ARGS(&target)));
  ComPtr<ID3D12DescriptorHeap> views;
  D3D12_DESCRIPTOR_HEAP_DESC view_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&view_desc, IID_PPV_ARGS(&views)));
  auto rtv = views->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 target_bytes;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &target_bytes);
  auto output = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, count * sizeof(UINT),
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, target_bytes + count * sizeof(UINT),
                         D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(output && readback, "buffers were not made"))
    return verdict();
  CHECK(forget(readback.Get()));
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  const float clear[] = {0, 0, 0, 0};
  list->ClearRenderTargetView(rtv, clear, 0, nullptr);
  for (UINT i = 0; i < count; i++) {
    step("seed %u variant %u: loaded graphics and compute output", seed, i);
    ComPtr<ID3D12PipelineState> drawn, computed;
    CHECK(library->LoadGraphicsPipeline(names[i].c_str(), &graphics[i], IID_PPV_ARGS(&drawn)));
    CHECK(library->LoadComputePipeline((names[i] + L"c").c_str(), &computes[i], IID_PPV_ARGS(&computed)));
    D3D12_VIEWPORT viewport{0, 0, (float)width, 1, 0, 1};
    D3D12_RECT pixel{(LONG)i, 0, (LONG)i + 1, 1};
    list->SetGraphicsRootSignature(rs.Get());
    list->SetGraphicsRoot32BitConstant(0, number[i], 0);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &pixel);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->SetPipelineState(drawn.Get());
    list->DrawInstanced(3, 1, 0, 0);
    list->SetComputeRootSignature(rs.Get());
    list->SetComputeRoot32BitConstant(0, number[i], 0);
    list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress() + i * sizeof(UINT));
    list->SetPipelineState(computed.Get());
    list->Dispatch(1, 1, 1);
  }
  for (UINT i = 0; i < streams.size(); i++) {
    step("seed %u stream %u: deserialized stream output", seed, i);
    D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(Stream), &streams[i]};
    ComPtr<ID3D12PipelineState> loaded;
    CHECK(library->LoadPipeline(stream_names[i].c_str(), &desc, IID_PPV_ARGS(&loaded)));
    D3D12_VIEWPORT viewport{0, 0, (float)width, 1, 0, 1};
    D3D12_RECT pixel{LONG(count + i), 0, LONG(count + i + 1), 1};
    list->SetGraphicsRootSignature(rs.Get());
    list->SetGraphicsRoot32BitConstant(0, number[0], 0);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &pixel);
    list->SetPipelineState(loaded.Get());
    if (streams[i].ms.data.BytecodeLength) {
      ComPtr<ID3D12GraphicsCommandList6> mesh_list;
      CHECK(list.As(&mesh_list));
      mesh_list->DispatchMesh(1, 1, 1);
    } else {
      list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      list->DrawInstanced(3, 1, 0, 0);
    }
  }
  transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
      to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
  list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  list->CopyBufferRegion(readback.Get(), target_bytes, output.Get(), 0, count * sizeof(UINT));
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  const char *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  for (UINT i = 0; i < count; i++) {
    step("seed %u variant %u: readback", seed, i);
    const UINT *pixel = (const UINT *)got + 4 * i;
    for (UINT channel = 0; channel < 4; channel++) {
      UINT want = masks[i] & (D3D12_COLOR_WRITE_ENABLE_RED << channel) ? number[i] + bias[i] + channel : 0;
      expect(pixel[channel] == want, "channel %u: %u, want %u", channel, pixel[channel], want);
    }
    UINT value = ((const UINT *)(got + target_bytes))[i], want = number[i] + bias[i];
    expect(value == want, "compute: %u, want %u", value, want);
  }
  for (UINT i = 0; i < streams.size(); i++) {
    step("seed %u stream %u: readback", seed, i);
    for (UINT channel = 0; channel < 4; channel++) {
      UINT value = ((const UINT *)got)[4 * (count + i) + channel];
      expect(value == stream_values[i] + channel, "stream channel %u: %u, want %u", channel, value,
             stream_values[i] + channel);
    }
  }
  readback->Unmap(0, nullptr);
  if (instrumented)
    expect(conversions == 0, "deserialized pipelines converted %lld shaders with disk caching disabled", conversions);
  library.Reset();
  expect(references(fence.Get()) == 1, "the destroyed library retained pipeline private data");
  return verdict();
}
