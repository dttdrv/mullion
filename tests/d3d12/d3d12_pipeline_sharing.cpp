// contract: a pipeline made of what a held pipeline already has (shaders, depth stencil state) makes no Metal
// library and no depth stencil state, and asks the system nothing (the bridge's call counts); another depth test does
// make one, which shows the counts see the calls. a device made after the first one ended finds its pipelines in the
// program's archive (MULLION_SHADER_CACHE_PATH): one Metal pipeline creation each, where a miss takes a second. the
// archive is kept in files of a bounded count of pipelines, so more pipelines than one file holds are in several, and
// a later device finds every one, those made after a file was written with its vertex shader too
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
  char temporary[MAX_PATH], directory[MAX_PATH];
  WCHAR wide[MAX_PATH];
  GetTempPathA(sizeof(temporary), temporary);
  auto unix_name = (char *(__cdecl *)(const WCHAR *))GetProcAddress(GetModuleHandleA("kernel32"), "wine_get_unix_file_name");
  if (!temporary_directory(temporary, "pa", directory) || !MultiByteToWideChar(CP_ACP, 0, directory, -1, wide, MAX_PATH))
    return printf("failed: no temporary directory\n"), 1;
  SetEnvironmentVariableA("MULLION_SHADER_CACHE", "1");
  SetEnvironmentVariableA("MULLION_SHADER_CACHE_PATH", unix_name(wide));
  // the calls counted are a creation's own: its Metal pipeline is made at the create call here, not on a compile
  // thread, whose pipelines d3d12_deferred_pipelines has
  SetEnvironmentVariableA("MULLION_D3D12_DEFER_PIPELINES", "0");
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

  step("more pipelines than one archive file holds");
  // twice what d3d12_device.cpp keeps in a file (kPipelinesPerArchive) and more, each a blend Metal makes apart
  std::vector<D3D12_GRAPHICS_PIPELINE_STATE_DESC> many;
  for (auto source :
       {D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_SRC_COLOR, D3D12_BLEND_INV_SRC_ALPHA})
    for (UINT8 mask = 1; mask <= D3D12_COLOR_WRITE_ENABLE_ALL; mask++) {
      auto &variant = many.emplace_back(blended);
      variant.BlendState.RenderTarget[0].SrcBlend = source;
      variant.BlendState.RenderTarget[0].RenderTargetWriteMask = mask;
    }
  for (auto &variant : many)
    make(variant);

  step("a device after the first one ended");
  held.clear(), rs.Reset(), device.Reset();
  WIN32_FIND_DATAA found;
  auto archive = FindFirstFileA((std::string(directory) + "\\pipelines_*.metallib").c_str(), &found);
  expect(archive != INVALID_HANDLE_VALUE && found.nFileSizeLow, "no archive written");
  unsigned files = archive != INVALID_HANDLE_VALUE;
  while (archive != INVALID_HANDLE_VALUE && FindNextFileA(archive, &found))
    files++;
  FindClose(archive);
  expect(files > 1, "%u archive files for %zu pipelines", files, many.size() + 3);
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  desc.pRootSignature = (rs = root_signature(device.Get(), {})).Get();
  auto made = make(desc)["MTLDevice_newRenderPipelineState"];
  expect(made == 1, "%llu Metal pipeline creations, want the archive's one", (unsigned long long)made);
  unsigned missed = 0;
  for (auto &variant : many) {
    variant.pRootSignature = rs.Get();
    missed += make(variant)["MTLDevice_newRenderPipelineState"] != 1;
  }
  expect(!missed, "%u of %zu pipelines not found in the archive", missed, many.size());
  step("a pipeline the archive lacks");
  auto unseen = desc;
  unseen.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
  made = make(unseen)["MTLDevice_newRenderPipelineState"];
  expect(made == 2, "%llu Metal pipeline creations, want the lookup and the compile", (unsigned long long)made);
  return verdict();
}
