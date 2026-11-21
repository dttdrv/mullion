// contract: every draw uses its current viewport and scissor, however often they change within one frame.
// "Viewports map clip-space vertex positions into RenderTarget space." (D3D11.3 15.6).
// "There is always an implicit scissoring by the Viewport x/y extents, orthogonal to other Scissor (15.7) state."
// "Scissor cuts out a rectangle in RenderTarget space where pixels are permitted to appear. Any pixel outside
// these extents is discarded." (15.7). source: D3D11_3_FunctionalSpec.htm, 15.6 and 15.7.
// whole-pixel edges and ONE/ONE ADD make each pixel's exact value the number of draws whose viewport and scissor
// contain its centre. the scissor test is always enabled in Direct3D 12.
// the measured Metal totals, 65,536 scissors and a little before 87,381 viewports, are not Direct3D limits on
// state changes. the unrepaired path sends 16 rectangles per change; three crossings plus a target's worth of
// draws exercise repeated pass splits, their preserved sums, and the rectangle restored at each split.
#include "d3d12_test.hpp"
#include <algorithm>
#include <limits>

static const char hlsl[] = R"hlsl(
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float ps() : SV_Target { return 1; }
)hlsl";

// a stuck API call or teardown must not outlive the test, even if its completion fence is broken
static DWORD WINAPI
deadline(void *) {
  const DWORD test_ms = 5 * 60 * 1000;
  Sleep(test_ms);
  printf("failed: 1 timeout after %lu ms\n", test_ms);
  ExitProcess(1);
}

int
main(int argc, char **argv) {
  auto watchdog = CreateThread(nullptr, 0, deadline, nullptr, 0, nullptr);
  if (!expect(watchdog != nullptr, "watchdog could not start: %lu", GetLastError()))
    return verdict();
  CloseHandle(watchdog);
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  step("compile the triangle and additive pixel shader");
  auto vs = compiler.compile(hlsl, "vs", "vs"), ps = compiler.compile(hlsl, "ps", "ps");
  if (!expect(!vs.empty() && !ps.empty(), "HLSL did not compile"))
    return verdict();

  const UINT width = 8, height = 4;
  constexpr UINT rectangles_per_change = D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
  constexpr UINT measured_metal_scissors = 65536, measured_metal_viewports = 87381, crossings = 3;
  constexpr UINT draws =
      crossings * (std::max(measured_metal_scissors, measured_metal_viewports) / rectangles_per_change + 1) +
      width * height;
  static_assert(draws < (UINT64(1) << std::numeric_limits<float>::digits));
  const UINT report_limit = 8;
  const DWORD wait_ms = 30 * 1000;
  const DXGI_FORMAT format = DXGI_FORMAT_R32_FLOAT;
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)));
  if (!expect((support.Support1 & D3D12_FORMAT_SUPPORT1_BLENDABLE) != 0,
              "R32_FLOAT does not support blending: %#x", support.Support1))
    return verdict();
  auto rs = root_signature(device.Get(), {});
  if (!expect(rs.Get() != nullptr, "root signature could not be made"))
    return verdict();
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs), bytecode(ps)};
  auto &blend_target = desc.BlendState.RenderTarget[0];
  blend_target.BlendEnable = TRUE;
  blend_target.SrcBlend = blend_target.DestBlend = D3D12_BLEND_ONE;
  blend_target.SrcBlendAlpha = blend_target.DestBlendAlpha = D3D12_BLEND_ONE;
  blend_target.BlendOp = blend_target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
  blend_target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = format;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, format, {1, 0},
                                D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                       nullptr, IID_PPV_ARGS(&target)));
  ComPtr<ID3D12DescriptorHeap> views;
  D3D12_DESCRIPTOR_HEAP_DESC views_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&views_desc, IID_PPV_ARGS(&views)));
  const auto rtv = views->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 bytes = 0;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(readback.Get() != nullptr, "readback buffer could not be made"))
    return verdict();
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  ComPtr<ID3D12GraphicsCommandList4> render_pass;
  const HRESULT interface_hr = list.As(&render_pass);
  if (!expect(interface_hr == S_OK || interface_hr == E_NOINTERFACE, "CommandList4 returned %#lx", interface_hr))
    return verdict();
  if (!render_pass)
    printf("skipped: explicit render pass, CommandList4 is unavailable\n");
  ComPtr<ID3D12Fence> fence;
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));

  enum { Scissors, Viewports, Both, RenderPass, Cases };
  const char *names[Cases] = {"scissors", "viewports", "both", "render pass scissors"};
  const D3D12_VIEWPORT whole_viewport{0, 0, float(width), float(height), 0, 1};
  const D3D12_RECT whole_scissor{0, 0, LONG(width), LONG(height)};
  for (UINT c = 0; c < (render_pass ? Cases : RenderPass); c++) {
    step("%s: %u draws in one pass", names[c], draws);
    if (c) {
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), pso.Get()));
    }
    CHECK(forget(readback.Get()));
    list->SetGraphicsRootSignature(rs.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->RSSetViewports(1, &whole_viewport);
    list->RSSetScissorRects(1, &whole_scissor);
    if (c == RenderPass) {
      D3D12_RENDER_PASS_RENDER_TARGET_DESC access{};
      access.cpuDescriptor = rtv;
      access.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
      access.BeginningAccess.Clear.ClearValue.Format = format;
      access.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
      render_pass->BeginRenderPass(1, &access, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
    } else {
      const float clear[4] = {};
      list->ClearRenderTargetView(rtv, clear, 0, nullptr);
      list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    }
    const bool change_viewport = c == Viewports || c == Both;
    const bool change_scissor = c != Viewports;
    std::vector<UINT> want(width * height);
    for (UINT i = 0; i < draws; i++) {
      const LONG left = i % width, top = i / width % height;
      D3D12_VIEWPORT viewport = whole_viewport;
      D3D12_RECT scissor = whole_scissor;
      if (change_viewport) {
        viewport = {float(left), float(top), float(1 + i / (width * height) % width),
                    float(1 + i / (width * height * width) % height), 0, 1};
        list->RSSetViewports(1, &viewport);
      }
      if (change_scissor) {
        scissor = {left, top, left + 1 + LONG(i / (width * height) % (width + 1)),
                   top + 1 + LONG(i / (width * height * (width + 1)) % (height + 1))};
        if (i % (width * height + 1) == width * height)
          scissor.right = scissor.left;
        list->RSSetScissorRects(1, &scissor);
      }
      list->DrawInstanced(3, 1, 0, 0);
      for (UINT y = 0; y < height; y++)
        for (UINT x = 0; x < width; x++) {
          const float px = x + 0.5f, py = y + 0.5f;
          want[y * width + x] +=
              px >= viewport.TopLeftX && px < viewport.TopLeftX + viewport.Width &&
              py >= viewport.TopLeftY && py < viewport.TopLeftY + viewport.Height &&
              px >= scissor.left && px < scissor.right && py >= scissor.top && py < scissor.bottom;
        }
    }
    if (c == RenderPass)
      render_pass->EndRenderPass();
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION into{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    into.PlacedFootprint = footprint;
    list->CopyTextureRegion(&into, 0, 0, 0, &from, nullptr);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    CHECK(list->Close());
    auto event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!expect(event != nullptr, "%s: fence event could not be made", names[c]))
      return verdict();
    ID3D12CommandList *one[] = {list.Get()};
    queue->ExecuteCommandLists(1, one);
    HRESULT hr = queue->Signal(fence.Get(), c + 1);
    if (SUCCEEDED(hr))
      hr = fence->SetEventOnCompletion(c + 1, event);
    const DWORD waited = SUCCEEDED(hr) ? WaitForSingleObject(event, wait_ms) : WAIT_FAILED;
    CloseHandle(event);
    if (!expect(waited == WAIT_OBJECT_0, "%s: fence returned %#lx, wait %lu within %lu ms",
                names[c], hr, waited, wait_ms) ||
        !expect(device->GetDeviceRemovedReason() == S_OK, "%s: device removed during readback", names[c]))
      ExitProcess(verdict());

    step("%s: pixel counts after %u draws", names[c], draws);
    const char *mapped = nullptr;
    CHECK(readback->Map(0, nullptr, (void **)&mapped));
    UINT wrong = 0;
    for (UINT y = 0; y < height; y++)
      for (UINT x = 0; x < width; x++) {
        const float got = *(const float *)(mapped + footprint.Offset + y * footprint.Footprint.RowPitch +
                                          x * sizeof(float));
        if (got != float(want[y * width + x]) && wrong++ < report_limit)
          expect(false, "%s: pixel %u,%u is %g, expected %u", names[c], x, y, got, want[y * width + x]);
      }
    trace::wrong += wrong - std::min(wrong, report_limit);
    readback->Unmap(0, nullptr);
  }
  step("release rectangle test resources");
  return verdict();
}
