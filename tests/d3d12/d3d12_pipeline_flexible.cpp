// contract: on Metal 4, pipelines that differ only in their targets' formats, write masks and blending compile Metal
// once; each other one is a specialization of that compile ("Flexible render pipeline states: compile most of the
// shader ahead of time and optionally compile other parts once your app knows those details", Apple, Using the
// Metal 4 compilation API). control: an integer target changes what the pixel shader outputs (D3D11.3 16.6), so it is
// a pipeline of its own and compiles as the first did. the counts are the bridge's (WMTDiagCallTotals); the shader
// store is on, in a directory of the test's own, as pipelines share their functions only through it
#include "d3d12_test.hpp"
#include "../../src/winemetal/winemetal.h"
#include <map>

static const char hlsl[] = R"hlsl(
float4 vs(uint id : SV_VertexID) : SV_Position { return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0.5, 1); }
float4 ps() : SV_Target { return 0.5; }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler))
    return printf("skipped: dxcompiler.dll not found\n"), 77;
  auto vs = compiler.compile(hlsl, "vs", "vs"), ps = compiler.compile(hlsl, "ps", "ps");
  char metal4[2] = "";
  if (GetEnvironmentVariableA("MULLION_D3D12_METAL4", metal4, sizeof(metal4)) && !strcmp(metal4, "0"))
    return printf("skipped: Direct3D 12 is kept off Metal 4\n"), 77;
  char temporary[MAX_PATH], directory[MAX_PATH];
  WCHAR wide[MAX_PATH];
  GetTempPathA(sizeof(temporary), temporary);
  auto unix_name = (char *(__cdecl *)(const WCHAR *))GetProcAddress(GetModuleHandleA("kernel32"), "wine_get_unix_file_name");
  if (!temporary_directory(temporary, "pf", directory) || !MultiByteToWideChar(CP_ACP, 0, directory, -1, wide, MAX_PATH))
    return printf("failed: no temporary directory\n"), 1;
  SetEnvironmentVariableA("MULLION_SHADER_CACHE", "1");
  SetEnvironmentVariableA("MULLION_SHADER_CACHE_PATH", unix_name(wide));
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  auto winemetal = GetModuleHandleA("winemetal.dll");
  ((decltype(&WMTDiagTimeCalls))GetProcAddress(winemetal, "WMTDiagTimeCalls"))();
  auto totals = (decltype(&WMTDiagCallTotals))GetProcAddress(winemetal, "WMTDiagCallTotals");
  auto rs = root_signature(device.Get(), {});
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{.pRootSignature = rs.Get(), .VS = bytecode(vs), .PS = bytecode(ps)};
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = UINT_MAX;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;

  std::vector<ComPtr<ID3D12PipelineState>> held;
  // Metal pipeline creations of a pipeline that compiles
  uint64_t first = 0;
  auto made = [&](const char *what, const D3D12_GRAPHICS_PIPELINE_STATE_DESC &made, bool compiles) {
    step("%s", what);
    static WMTDiagCall calls[WMTUnixCallCount];
    totals(calls);
    expect(SUCCEEDED(device->CreateGraphicsPipelineState(&made, IID_PPV_ARGS(&held.emplace_back()))), "created");
    totals(calls);
    std::map<std::string, uint64_t> named;
    for (auto &[count, ticks, name] : calls)
      if (name)
        named[name] += count;
    auto creations = named["MTLDevice_newRenderPipelineState"];
    if (!first)
      first = creations;
    expect(creations == (compiles ? first : 0) && first, "%llu Metal pipeline creations, want %llu",
           (unsigned long long)creations, (unsigned long long)(compiles ? first : 0));
    expect(named["MTLDevice_newSpecializedRenderPipelineState"] == 1, "%llu specializations, want 1",
           (unsigned long long)named["MTLDevice_newSpecializedRenderPipelineState"]);
  };
  made("the first pipeline", desc, true);
  auto other = desc;
  for (auto format : {DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R16G16B16A16_FLOAT}) {
    other.RTVFormats[0] = format;
    made("another format", other, false);
  }
  other = desc;
  other.BlendState.RenderTarget[0] = {TRUE, FALSE, D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA,
                                      D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
                                      D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_RED};
  made("blended, one channel written", other, false);
  other = desc;
  other.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
  made("an integer target", other, true);
  return verdict();
}
