// contract: a pipeline made of what a held pipeline already has (shaders, depth stencil state) makes no Metal
// library and no depth stencil state, and asks the system nothing (the bridge's call counts); another depth test does
// make one, which shows the counts see the calls
#include "d3d12_test.hpp"
#include "../../src/winemetal/winemetal.h"
#include <map>

static const char hlsl[] = R"hlsl(
float4 vs(uint id : SV_VertexID) : SV_Position { return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0.5, 1); }
float4 ps() : SV_Target { return 1; }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler))
    return printf("skipped: dxcompiler.dll not found\n"), 77;
  auto vs = compiler.compile(hlsl, "vs", "vs"), ps = compiler.compile(hlsl, "ps", "ps");
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
  desc.DepthStencilState = {TRUE, D3D12_DEPTH_WRITE_MASK_ALL, D3D12_COMPARISON_FUNC_LESS};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
  desc.SampleDesc.Count = 1;
  auto blended = desc, deeper = desc;
  blended.BlendState.RenderTarget[0] = {TRUE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ONE, D3D12_BLEND_OP_ADD,
                                        D3D12_BLEND_ONE, D3D12_BLEND_ONE, D3D12_BLEND_OP_ADD};
  blended.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  deeper.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;

  std::vector<ComPtr<ID3D12PipelineState>> held;
  // the calls a creation makes, by name
  auto make = [&](const D3D12_GRAPHICS_PIPELINE_STATE_DESC &made) {
    static WMTDiagCall calls[WMTUnixCallCount];
    totals(calls);
    expect(SUCCEEDED(device->CreateGraphicsPipelineState(&made, IID_PPV_ARGS(&held.emplace_back()))), "created");
    totals(calls);
    std::map<std::string, uint64_t> named;
    for (auto &[count, ticks, name] : calls)
      if (name)
        named[name] += count;
    return named;
  };
  make(desc);
  for (auto *again : {&desc, &blended}) {
    step(again == &desc ? "the same pipeline again" : "blended");
    auto calls = make(*again);
    for (auto name : {"MTLDevice_newLibrary", "MTLDevice_newDepthStencilState", "WMTGetOSVersion"})
      expect(!calls[name], "%llu %s", (unsigned long long)calls[name], name);
  }
  step("another depth test");
  expect(make(deeper)["MTLDevice_newDepthStencilState"] > 0, "no depth stencil state made");
  return verdict();
}
