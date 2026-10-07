// contract: a shader or a root signature that is not what a compiler wrote is refused, and the process goes on.
// both come in a container: a header with a hash of everything after it, the size, and the offsets of parts of a
// four-character name and a size each (DirectXShaderCompiler, DxilContainer.h: DxilContainerHeader,
// DxilPartHeader). Direct3D checks the hash before it reads anything else, so a container with any byte changed, or
// cut short, fails with E_INVALIDARG ("An invalid parameter was passed to the returning function", Direct3D 12
// return codes; ID3D12Device::CreateRootSignature: "E_INVALIDARG if the blob that pBlobWithRootSignature points to
// is invalid"), whatever the change does to the parts.
// a state object's DXIL library is such a container too (ID3D12Device5::CreateStateObject: "E_INVALIDARG if one of
// the input parameters is invalid").
// every container the test has (a vertex, a pixel and a compute shader, a serialized root signature, and a library
// of ray tracing shaders where the device has ray tracing and the front end is DXC) is garbled in
// every way its own layout gives: cut at every part's start and middle and inside the header, a byte flipped at
// the start, middle and end of every part, every part's size and name changed, every offset moved, the part count
// and the total size changed, and the hash alone changed. each one goes to the call that takes it. an answer other
// than E_INVALIDARG is a wrong result; a fault ends the test with the garbling it was given (tests/trace.hpp).
// the hash has three values that are no hash (HLSL specification INF-0004, Validator Hashing):
// - sixteen bytes of 1, BYPASS: "shaders can run without validating the hash regardless of whether the machine is
//   in developer mode": the whole container is taken with it;
// - sixteen bytes of 2, PREVIEW_BYPASS: "allowed to execute only if developer mode and the experimental feature
//   D3D12ExperimentalShaderModels is enabled, otherwise the runtime will produce an error";
// - sixteen bytes of 0, what DXC writes when it cannot sign: "legacy zero'd hash shaders ... treated as if they had
//   the PREVIEW_BYPASS hash set". test suites compile with such a DXC and enable the feature (vkd3d-proton,
//   tests/d3d12_crosstest.h:459).
// D3D12EnableExperimentalFeatures is called "before device creation", grants all the features it is given or none,
// and answers "E_NOINTERFACE if an unrecognized feature is specified". so the test has three devices one after
// another: one made with nothing enabled (BYPASS taken, the other two refused, and every garbling); one made after
// calls that fail (a feature Mullion does not have, alone and together with the shader models: E_NOINTERFACE and
// nothing switched on, so the same answers); one made after the shader models alone were enabled (S_OK: all three
// taken, and a hash that is merely wrong still refused). what is enabled stays enabled, which gives the order.
// BYPASS lets a container past the hash whatever its bytes are, and so does a hash made anew (dxbc_hash.hpp):
// every garbling is given both ways as well. some of those are other shaders that are whole (a flipped constant),
// so they are held only to an answer: S_OK or E_INVALIDARG, and the process goes on. one answer is known: a
// container that is whole and has no root signature in it gets E_FAIL from CreateRootSignature, as Windows gives
// (vkd3d-proton, tests/d3d12_root_signature.c:1846, a compute shader's container: "Has to be E_FAIL, not
// E_INVALIDARG, oddly enough"). that is the compute shader itself, and every garbling of the root signature's
// container that leaves its table of parts whole and no root signature in it.
#include "d3d12_test.hpp"
#include "../garbled.hpp"
#include <functional>

static const char hlsl[] = R"hlsl(
Texture2D<float4> colors : register(t0);
SamplerState point_sampler : register(s0);
cbuffer Scale : register(b0) { float4 scale; };
struct V { float4 pos : SV_Position; float2 uv : UV; };
V vs(float3 pos : POSITION, float2 uv : UV) { V v; v.pos = float4(pos * scale.xyz, 1); v.uv = uv; return v; }
float4 ps(V v) : SV_Target { return colors.Sample(point_sampler, v.uv) * scale; }
RWStructuredBuffer<uint> numbers : register(u0);
[numthreads(8, 1, 1)] void cs(uint3 id : SV_DispatchThreadID) { numbers[id.x] = numbers[id.x] * 3 + uint(scale.x); }
)hlsl";

// a library of ray tracing shaders, for a state object
static const char library_hlsl[] = R"hlsl(
struct Payload { float value; };
[shader("raygeneration")] void generate() {}
[shader("miss")] void miss(inout Payload payload) { payload.value = 1; }
)hlsl";

// a device and what it makes of the containers: `preview` says whether PREVIEW_BYPASS and no hash are taken, and
// `all` gives it every garbling too. false when there is nothing to go on with
static bool
device_takes(const Compiler &compiler, const char *when, bool preview, bool all) {
  auto vs = compiler.compile(hlsl, "vs", "vs"), ps = compiler.compile(hlsl, "ps", "ps"), cs = compiler.compile(hlsl, "cs", "cs");
  ComPtr<ID3D12Device> device;
  if (!expect(!vs.empty() && !ps.empty() && !cs.empty() && SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))),
              "%s: no shaders or no device", when))
    return false;
  D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1}, {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1}};
  D3D12_ROOT_PARAMETER params[4] = {{D3D12_ROOT_PARAMETER_TYPE_CBV},
                                    {D3D12_ROOT_PARAMETER_TYPE_UAV},
                                    {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
                                    {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}};
  params[2].DescriptorTable = {1, &ranges[0]}, params[3].DescriptorTable = {1, &ranges[1]};
  const D3D12_ROOT_SIGNATURE_DESC rs_desc{(UINT)std::size(params), params, 0, nullptr,
                                          D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
  ComPtr<ID3DBlob> blob;
  if (!expect(SUCCEEDED(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, nullptr)), "no root signature"))
    return false;
  const std::string serialized((const char *)blob->GetBufferPointer(), blob->GetBufferSize());
  ComPtr<ID3D12RootSignature> rs;
  if (!expect(SUCCEEDED(device->CreateRootSignature(0, serialized.data(), serialized.size(), IID_PPV_ARGS(&rs))), "the whole root signature is refused"))
    return false;

  const D3D12_INPUT_ELEMENT_DESC elements[] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0}, {"UV", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12}};
  auto graphics = [&](const std::string &vertex, const std::string &pixel) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = rs.Get();
    desc.VS = bytecode(vertex), desc.PS = bytecode(pixel);
    desc.InputLayout = {elements, (UINT)std::size(elements)};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = ~0u;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1, desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc = {1, 0};
    ComPtr<ID3D12PipelineState> pso;
    return device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
  };
  auto compute = [&](const std::string &code) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(code)};
    ComPtr<ID3D12PipelineState> pso;
    return device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
  };
  auto signature = [&](const std::string &code) {
    ComPtr<ID3D12RootSignature> made;
    return device->CreateRootSignature(0, code.data(), code.size(), IID_PPV_ARGS(&made));
  };
  // what the compilers wrote is taken: without that, a refusal says nothing
  step("%s: the containers as they were written", when);
  HRESULT whole_graphics = graphics(vs, ps), whole_compute = compute(cs);
  if (!expect(whole_graphics == S_OK && whole_compute == S_OK, "the pipelines of the whole shaders: %08lx and %08lx", whole_graphics, whole_compute))
    return false;

  struct Use {
    const char *call;
    const std::string &code;
    std::function<HRESULT(const std::string &)> make;
  };
  std::vector<Use> uses = {
      {"CreateGraphicsPipelineState, its vertex shader", vs, [&](auto &code) { return graphics(code, ps); }},
      {"CreateGraphicsPipelineState, its pixel shader", ps, [&](auto &code) { return graphics(vs, code); }},
      {"CreateComputePipelineState", cs, compute},
      {"CreateRootSignature", serialized, signature},
  };
  // a ray tracing pipeline of the library's two shaders, with the least a pipeline has to say
  const auto library = compiler.compile(library_hlsl, "", "lib_6_3");
  ComPtr<ID3D12Device5> device5;
  D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
  device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options));
  auto state_object = [&](const std::string &code) {
    D3D12_DXIL_LIBRARY_DESC library_desc{bytecode(code)};
    D3D12_RAYTRACING_SHADER_CONFIG shader_config{sizeof(float), 2 * sizeof(float)};
    D3D12_RAYTRACING_PIPELINE_CONFIG pipeline_config{1};
    D3D12_GLOBAL_ROOT_SIGNATURE global{rs.Get()};
    const D3D12_STATE_SUBOBJECT subobjects[] = {{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library_desc},
                                                {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader_config},
                                                {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipeline_config},
                                                {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global}};
    const D3D12_STATE_OBJECT_DESC desc{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, (UINT)std::size(subobjects), subobjects};
    ComPtr<ID3D12StateObject> made;
    return device5->CreateStateObject(&desc, IID_PPV_ARGS(&made));
  };
  if (!library.empty() && options.RaytracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED && SUCCEEDED(device.As(&device5))) {
    HRESULT whole = state_object(library);
    if (expect(whole == S_OK, "the state object of the whole library: %08lx", whole))
      uses.push_back({"CreateStateObject, its DXIL library", library, state_object});
  } else
    printf("not run with this front end or device: a state object's DXIL library\n");
  step("%s, CreateRootSignature: a whole container with no root signature, the compute shader's", when);
  HRESULT none = signature(cs);
  expect(none == E_FAIL, "%08lx, want E_FAIL", none);
  unsigned made = 0, taken = 0, past = 0, past_taken = 0;
  for (auto &use : uses) {
    const bool of_signature = &use.code == &serialized;
    // BYPASS, PREVIEW_BYPASS and no hash
    const std::pair<char, HRESULT> sentinels[] = {{1, S_OK}, {2, preview ? S_OK : E_INVALIDARG}, {0, preview ? S_OK : E_INVALIDARG}};
    for (auto [byte, want] : sentinels) {
      step("%s, %s: whole, its hash sixteen bytes of %d", when, use.call, byte);
      HRESULT hr = use.make(hashed(use.code, byte));
      expect(hr == want, "%08lx, want %08lx", hr, want);
    }
    // a hash that is another container's is wrong whatever is enabled
    std::string other = use.code;
    other[dxbc::hash_at] ^= 0x5a;
    step("%s, %s: whole, one byte of its hash flipped", when, use.call);
    HRESULT flipped = use.make(other);
    expect(flipped == E_INVALIDARG, "%08lx, want E_INVALIDARG", flipped);
    if (!all)
      continue;
    for (auto &[what, code] : garbled(use.code)) {
      step("%s: %s", use.call, what.c_str());
      HRESULT hr = use.make(code);
      made++, taken += hr == S_OK;
      expect(hr == E_INVALIDARG, hr == S_OK ? "taken as if it were whole (%08lx)" : "refused with %08lx, want E_INVALIDARG", hr);
      // past the hash: not to be checked, and signed anew
      std::string signed_anew = code;
      dxbc::sign(signed_anew);
      for (auto &through : {hashed(code, 1), signed_anew}) {
        step("%s: %s, and a hash that lets it through", use.call, what.c_str());
        hr = use.make(through);
        past++, past_taken += hr == S_OK;
        const Layout parts = layout(through, "RTS0");
        if (of_signature && parts.whole && !parts.has)
          expect(hr == E_FAIL, "a whole container with no root signature answered %08lx, want E_FAIL", hr);
        else
          expect(hr == E_INVALIDARG || hr == S_OK, "answered %08lx", hr);
      }
    }
  }
  if (all)
    printf("%u garbled containers, %u taken; %u more let past the hash, %u of them taken\n", made, taken, past, past_taken);
  return true;
}

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  if (!device_takes(compiler, "nothing enabled", false, true))
    return verdict();
  // a feature that is not here, alone and beside the one that is: all or none
  const IID both[] = {D3D12ExperimentalShaderModels, D3D12TiledResourceTier4};
  step("D3D12EnableExperimentalFeatures with a feature that is not here, alone and beside the shader models");
  HRESULT alone = D3D12EnableExperimentalFeatures(1, &both[1], nullptr, nullptr),
          beside = D3D12EnableExperimentalFeatures(std::size(both), both, nullptr, nullptr);
  expect(alone == E_NOINTERFACE && beside == E_NOINTERFACE, "%08lx alone and %08lx beside the shader models, want E_NOINTERFACE twice", alone, beside);
  if (!device_takes(compiler, "after the calls that failed", false, false))
    return verdict();
  step("D3D12EnableExperimentalFeatures with the experimental shader models alone");
  HRESULT enabled = D3D12EnableExperimentalFeatures(1, &both[0], nullptr, nullptr);
  if (expect(enabled == S_OK, "%08lx, want S_OK", enabled))
    device_takes(compiler, "experimental shader models enabled", true, false);
  return verdict();
}
