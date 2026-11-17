// contract: a pipeline the program's archive holds is made by its first draw, from the archive, and not ahead of it on
// the compile threads (DeferredPipeline), so that one the application never draws makes no Metal pipeline. the
// archive is the one a first device wrote (MULLION_SHADER_CACHE_PATH); in a device after it:
// - a pipeline it holds and one it lacks are created, and the second is drawn: its Metal pipeline is the lookup that
//   misses and the compile, and the first has none yet;
// - then the first is drawn: one Metal pipeline creation, the archive's, and it writes its shader's value.
// the creations are the bridge's call counts (WMTDiagCallTotals)
#include "d3d12_test.hpp"
#include "../../src/winemetal/winemetal.h"
#include <cstring>
#include <thread>

static const char hlsl[] = R"hlsl(
float4 vs(uint id : SV_VertexID) : SV_Position { return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0.5, 1); }
uint ps() : SV_Target { return VALUE; }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler))
    return printf("skipped: dxcompiler.dll not found\n"), 77;
  auto vs = compiler.compile(hlsl, "vs", "vs", {"VALUE=0"});
  std::string ps[2] = {compiler.compile(hlsl, "ps", "ps", {"VALUE=7"}), compiler.compile(hlsl, "ps", "ps", {"VALUE=9"})};
  if (vs.empty() || ps[0].empty() || ps[1].empty())
    return printf("failed: HLSL did not compile\n"), 1;
  char temporary[MAX_PATH], directory[MAX_PATH];
  WCHAR wide[MAX_PATH];
  GetTempPathA(sizeof(temporary), temporary);
  auto unix_name = (char *(__cdecl *)(const WCHAR *))GetProcAddress(GetModuleHandleA("kernel32"), "wine_get_unix_file_name");
  if (!temporary_directory(temporary, "ad", directory) || !MultiByteToWideChar(CP_ACP, 0, directory, -1, wide, MAX_PATH))
    return printf("failed: no temporary directory\n"), 1;
  SetEnvironmentVariableA("MULLION_SHADER_CACHE", "1");
  SetEnvironmentVariableA("MULLION_SHADER_CACHE_PATH", unix_name(wide));

  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12RootSignature> rs;
  ComPtr<ID3D12Resource> target, readback;
  ComPtr<ID3D12DescriptorHeap> rtvs;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 2, 1, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT row;
  // a device and what the draws need of it
  auto start = [&] {
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
      return false;
    rs = root_signature(device.Get(), {});
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &target_desc,
                                               D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target))) ||
        FAILED(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs))))
      return false;
    device->CreateRenderTargetView(target.Get(), nullptr, rtvs->GetCPUDescriptorHandleForHeapStart());
    UINT64 size;
    device->GetCopyableFootprints(&target_desc, 0, 1, 0, &row, nullptr, nullptr, &size);
    readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, size, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    return rs && readback && SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))) &&
           SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
           SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                               IID_PPV_ARGS(&list))) &&
           SUCCEEDED(list->Close());
  };
  auto pipeline = [&](int which) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{.pRootSignature = rs.Get(), .VS = bytecode(vs), .PS = bytecode(ps[which])};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
    desc.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> made;
    expect(SUCCEEDED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&made))), "pipeline %d created", which);
    return made;
  };
  // the pipeline drawn in the target's pixel `column`, and what that pixel holds after: ~0 when a call failed
  auto draw = [&](ID3D12PipelineState *drawn, UINT column) {
    if (!drawn || FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), drawn)))
      return ~0u;
    auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->SetGraphicsRootSignature(rs.Get());
    D3D12_VIEWPORT viewport{0, 0, 2, 1, 0, 1};
    D3D12_RECT scissor{(LONG)column, 0, LONG(column + 1), 1};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    D3D12_TEXTURE_COPY_LOCATION src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    dst.PlacedFootprint = row;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    UINT *got;
    if (FAILED(submit(device.Get(), queue.Get(), list.Get())) || FAILED(readback->Map(0, nullptr, (void **)&got)))
      return ~0u;
    UINT value = got[column];
    readback->Unmap(0, nullptr);
    return value;
  };

  step("the first device archives a pipeline");
  if (!start())
    return printf("failed: no device\n"), 1;
  {
    auto held = pipeline(0);
    expect(draw(held.Get(), 0) == 7, "the first device's draw");
  }
  list.Reset(), allocator.Reset(), queue.Reset(), readback.Reset(), rtvs.Reset(), target.Reset(), rs.Reset();
  device.Reset();

  step("a device after it");
  if (!start())
    return printf("failed: no second device\n"), 1;
  auto winemetal = GetModuleHandleA("winemetal.dll");
  ((decltype(&WMTDiagTimeCalls))GetProcAddress(winemetal, "WMTDiagTimeCalls"))();
  auto totals = (decltype(&WMTDiagCallTotals))GetProcAddress(winemetal, "WMTDiagCallTotals");
  // the Metal pipelines made since the last count
  auto made = [&] {
    static WMTDiagCall calls[WMTUnixCallCount];
    totals(calls);
    uint64_t count = 0;
    for (auto &[number, ticks, name] : calls)
      if (name && !strcmp(name, "MTLDevice_newRenderPipelineState"))
        count += number;
    return count;
  };
  made();
  auto held = pipeline(0), lacked = pipeline(1);
  expect(draw(lacked.Get(), 1) == 9, "the draw of the pipeline the archive lacks");
  // what the compile threads had begun before that draw has ended well before now, a lookup or a compile
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  auto ahead = made();
  expect(ahead == 2, "%llu Metal pipeline creations before the archived pipeline's draw, want the other's lookup and compile",
         (unsigned long long)ahead);
  expect(draw(held.Get(), 0) == 7, "the archived pipeline's draw");
  auto at_draw = made();
  expect(at_draw == 1, "%llu Metal pipeline creations at its draw, want the archive's one", (unsigned long long)at_draw);
  return verdict();
}
