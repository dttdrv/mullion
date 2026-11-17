// contract: a pipeline writes the render targets and the depth its formats name and no others, whatever pipeline
// drew to the bound targets before it: "The pipeline state object (PSO) ... the formats of the render targets and
// the depth stencil" belong to the pipeline, and a target whose format it leaves DXGI_FORMAT_UNKNOWN is not
// written (Microsoft Learn, D3D12_GRAPHICS_PIPELINE_STATE_DESC, RTVFormats and DSVFormat).
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_graphics_pipeline_state_desc
// and Mullion's own: pipelines that write fewer of the targets than the one that began the pass draw in that pass.
// a Metal pass stores and loads every target it has, so a pass for each of them costs that each time; the list's
// trace (MULLION_TRACE_ENCODERS) counts the passes of a child process.
// two colour targets of different formats and a depth buffer; each pipeline draws its own column pair with a
// colour and a depth of its own, over cleared targets.
#include "d3d12_test.hpp"
#include <filesystem>
#include <fstream>

static const char hlsl[] = R"hlsl(
cbuffer Draw : register(b0) { float shade; float depth; };
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), depth, 1);
}
struct Both { float4 first : SV_Target0; float4 second : SV_Target1; };
Both ps_both() { Both to; to.first = shade; to.second = 2 * shade; return to; }
float4 ps_first() : SV_Target0 { return shade; }
float4 ps_second() : SV_Target1 { return 2 * shade; }
)hlsl";

static int
draws(const std::string &vs, const std::string &ps_both, const std::string &ps_first, const std::string &ps_second) {
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  const DXGI_FORMAT formats[2] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT},
                    depth_format = DXGI_FORMAT_D32_FLOAT;
  D3D12_ROOT_PARAMETER parameter{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
  parameter.Constants.Num32BitValues = 2;
  auto rs = root_signature(device.Get(), {1, &parameter});
  if (!expect(!!rs, "no root signature"))
    return verdict();

  // what each pipeline writes: the first target, the second, the depth
  const struct {
    const std::string *ps;
    bool first, second, depth;
    const char *what;
  } kinds[] = {
      {&ps_both, true, true, true, "both targets and depth"},
      {&ps_first, true, false, false, "the first target"},
      {nullptr, false, false, true, "depth"},
      {&ps_second, false, true, false, "the second target"},
      {&ps_first, true, false, true, "the first target and depth"},
  };
  ComPtr<ID3D12PipelineState> pso[std::size(kinds)];
  for (UINT k = 0; k < std::size(kinds); k++) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs)};
    if (kinds[k].ps)
      desc.PS = bytecode(*kinds[k].ps);
    desc.BlendState.IndependentBlendEnable = TRUE;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = ~0u;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = kinds[k].depth;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = kinds[k].second ? 2 : kinds[k].first ? 1 : 0;
    desc.RTVFormats[0] = kinds[k].first ? formats[0] : DXGI_FORMAT_UNKNOWN;
    desc.RTVFormats[1] = kinds[k].second ? formats[1] : DXGI_FORMAT_UNKNOWN;
    desc.DSVFormat = kinds[k].depth ? depth_format : DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc = {1, 0};
    CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso[k])));
  }

  // the draws in order: the widest pipeline first, each narrower one, then the widest again
  const UINT order[] = {0, 1, 2, 3, 4, 0};
  const UINT pair = 2, width = std::size(order) * pair, height = 2;
  ComPtr<ID3D12Resource> targets[2], depth;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC descs[3];
  for (UINT t = 0; t < 3; t++) {
    descs[t] = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, t < 2 ? formats[t] : depth_format, {1, 0},
                D3D12_TEXTURE_LAYOUT_UNKNOWN,
                t < 2 ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL};
    CHECK(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &descs[t],
        t < 2 ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr,
        IID_PPV_ARGS(t < 2 ? &targets[t] : &depth)
    ));
  }
  ComPtr<ID3D12DescriptorHeap> rtvs, dsvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2}, dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  CHECK(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsvs)));
  D3D12_CPU_DESCRIPTOR_HANDLE rtv[2] = {rtvs->GetCPUDescriptorHandleForHeapStart(),
                                        rtvs->GetCPUDescriptorHandleForHeapStart()};
  rtv[1].ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  auto dsv = dsvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(targets[0].Get(), nullptr, rtv[0]);
  device->CreateRenderTargetView(targets[1].Get(), nullptr, rtv[1]);
  device->CreateDepthStencilView(depth.Get(), nullptr, dsv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[3];
  ComPtr<ID3D12Resource> readbacks[3];
  for (UINT t = 0; t < 3; t++) {
    UINT64 bytes;
    device->GetCopyableFootprints(&descs[t], 0, 1, 0, &footprints[t], nullptr, nullptr, &bytes);
    readbacks[t] = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    CHECK(readbacks[t] ? S_OK : E_FAIL);
  }

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));

  // cleared to values no draw writes; a draw's shade and depth are its place in the order's, an eighth each
  const float cleared[4] = {1, 1, 1, 1}, cleared_depth = 1;
  const D3D12_VIEWPORT viewport{0, 0, (float)width, (float)height, 0, 1};
  list->ClearRenderTargetView(rtv[0], cleared, 0, nullptr);
  list->ClearRenderTargetView(rtv[1], cleared, 0, nullptr);
  list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, cleared_depth, 0, 0, nullptr);
  list->OMSetRenderTargets(2, rtv, FALSE, &dsv);
  list->SetGraphicsRootSignature(rs.Get());
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list->RSSetViewports(1, &viewport);
  for (UINT d = 0; d < std::size(order); d++) {
    const D3D12_RECT scissor{LONG(d * pair), 0, LONG((d + 1) * pair), (LONG)height};
    const float constants[2] = {(d + 1) / 8.0f, (d + 1) / 16.0f};
    list->SetPipelineState(pso[order[d]].Get());
    list->SetGraphicsRoot32BitConstants(0, 2, constants, 0);
    list->RSSetScissorRects(1, &scissor);
    list->DrawInstanced(3, 1, 0, 0);
  }
  for (UINT t = 0; t < 3; t++) {
    auto texture = t < 2 ? targets[t].Get() : depth.Get();
    transition(list.Get(), texture, t < 2 ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_DEPTH_WRITE,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from{texture}, to{readbacks[t].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    to.PlacedFootprint = footprints[t];
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  }
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  const UINT8 *got[3];
  for (UINT t = 0; t < 3; t++)
    CHECK(readbacks[t]->Map(0, nullptr, (void **)&got[t]));
  for (UINT y = 0; y < height; y++)
    for (UINT x = 0; x < width; x++) {
      const UINT d = x / pair;
      const auto &kind = kinds[order[d]];
      const float shade = (d + 1) / 8.0f, z = (d + 1) / 16.0f;
      // eighths are whole in both colour formats and in the depth's
      UINT8 first;
      UINT16 second;
      float deep;
      memcpy(&first, got[0] + y * footprints[0].Footprint.RowPitch + x * 4, sizeof(first));
      memcpy(&second, got[1] + y * footprints[1].Footprint.RowPitch + x * 8, sizeof(second));
      memcpy(&deep, got[2] + y * footprints[2].Footprint.RowPitch + x * 4, sizeof(deep));
      // UNORM: "within 0.6f ULP" of the value scaled (D3D11.3 3.2.3.6); a half float of 2 * shade is exact, 1 is 0x3c00
      const float want_first = kind.first ? shade * 255 : 255;
      const UINT16 halves[] = {0x3400, 0x3800, 0x3a00, 0x3c00, 0x3d00, 0x3e00}; // 0.25, 0.5, 0.75, 1, 1.25, 1.5
      const UINT16 want_second = kind.second ? halves[d] : 0x3c00;
      const float want_deep = kind.depth ? z : cleared_depth;
      expect(first >= want_first - 0.6f && first <= want_first + 0.6f && second == want_second && deep == want_deep,
             "draw %u of a pipeline for %s, texel %u,%u: first target %u, second %04x, depth %g; want %g, %04x, %g", d,
             kind.what, x, y, first, second, deep, want_first, want_second, want_deep);
    }
  for (UINT t = 0; t < 3; t++)
    readbacks[t]->Unmap(0, nullptr);
  return verdict();
}

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto vs = compiler.compile(hlsl, "vs", "vs"), ps_both = compiler.compile(hlsl, "ps_both", "ps"),
       ps_first = compiler.compile(hlsl, "ps_first", "ps"), ps_second = compiler.compile(hlsl, "ps_second", "ps");
  if (vs.empty() || ps_both.empty() || ps_first.empty() || ps_second.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  if (argc > 2)
    return draws(vs, ps_both, ps_first, ps_second);
  // the draws in a child whose library traces its passes
  char temporary[MAX_PATH], directory[MAX_PATH], exe[MAX_PATH];
  CHECK(GetTempPathA(sizeof(temporary), temporary) ? S_OK : E_FAIL);
  CHECK(GetModuleFileNameA(nullptr, exe, sizeof(exe)) ? S_OK : E_FAIL);
  CHECK(GetTempFileNameA(temporary, "pas", 0, directory) ? S_OK : E_FAIL);
  DeleteFileA(directory);
  CHECK(CreateDirectoryA(directory, nullptr) ? S_OK : E_FAIL);
  SetEnvironmentVariableA("MULLION_LOG_PATH", directory);
  SetEnvironmentVariableA("MULLION_LOG_LEVEL", "info");
  SetEnvironmentVariableA("MULLION_TRACE_ENCODERS", "1");
  SetEnvironmentVariableA("MULLION_DIAG", nullptr);
  auto command = std::string("\"") + exe + "\" " + (argc > 1 ? argv[1] : "dxil") + " child";
  STARTUPINFOA startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  CHECK(CreateProcessA(exe, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process) ? S_OK
                                                                                                           : E_FAIL);
  DWORD waited = WaitForSingleObject(process.hProcess, 120000), result = 1;
  if (waited != WAIT_OBJECT_0)
    TerminateProcess(process.hProcess, 1);
  GetExitCodeProcess(process.hProcess, &result);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  expect(waited == WAIT_OBJECT_0 && result == 0, "the draws: result %lu, wait %lu", result, waited);
  unsigned render = 0, traced = 0;
  for (auto &entry : std::filesystem::directory_iterator(directory)) {
    std::ifstream log(entry.path());
    for (std::string line; std::getline(log, line);) {
      unsigned api, r, c, b;
      if (sscanf(line.c_str(), "info:  D3D12 encoders Metal%u: render=%u compute=%u blit=%u", &api, &r, &c, &b) == 4)
        render += r, traced++;
    }
  }
  std::filesystem::remove_all(directory);
  expect(traced != 0, "no D3D12 encoder trace");
  // one pass for the six draws: the pipelines after the first write no target it lacks
  expect(render == 1, "render passes: %u, want 1", render);
  return verdict();
}
