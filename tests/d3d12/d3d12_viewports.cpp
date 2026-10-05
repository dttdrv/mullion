// contract: see ../viewports.hpp, here through Direct3D 12, whose scissor test is always on.
#include "d3d12_test.hpp"
#include "../viewports.hpp"

using namespace viewports;

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto vs = compiler.compile(hlsl, "vs", "vs"), gs = compiler.compile(hlsl, "gs", "gs"), ps = compiler.compile(hlsl, "ps", "ps");
  if (vs.empty() || gs.empty() || ps.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER constants{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
  constants.Constants = {0, 0, 2};
  auto rs = root_signature(device.Get(), {1, &constants});
  const DXGI_FORMAT format = DXGI_FORMAT_R32_FLOAT;
  ComPtr<ID3D12PipelineState> pipelines[2];
  for (bool geometry : {false, true}) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = rs.Get();
    desc.VS = bytecode(vs);
    desc.PS = bytecode(ps);
    if (geometry)
      desc.GS = bytecode(gs);
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = ~0u;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = format;
    desc.SampleDesc = {1, 0};
    CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipelines[geometry])));
  }
  ComPtr<ID3D12Resource> target;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, format, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)));
  auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);

  // a slot of the readback buffer for each case's pixels
  const auto all = cases();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 bytes;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  const UINT64 slot = (bytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) / D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT *
                      D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, slot * all.size(), D3D12_RESOURCE_STATE_COPY_DEST);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  list->SetGraphicsRootSignature(rs.Get());
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  for (size_t c = 0; c < all.size(); c++) {
    const float clear[4] = {};
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    for (size_t n = 0; n < all[c].size(); n++) {
      auto &draw = all[c][n];
      if (draw.viewports) {
        std::vector<D3D12_VIEWPORT> set;
        for (auto part : *draw.viewports) {
          auto r = rectangle(part);
          set.push_back({(float)r.left, (float)r.top, float(r.right - r.left), float(r.bottom - r.top), 0, 1});
        }
        list->RSSetViewports(set.size(), set.data());
      }
      if (draw.scissors) {
        std::vector<D3D12_RECT> set;
        for (auto part : *draw.scissors) {
          auto r = rectangle(part);
          set.push_back({r.left, r.top, r.right, r.bottom});
        }
        list->RSSetScissorRects(set.size(), set.data());
      }
      struct {
        UINT index;
        float value;
      } constants_of{draw.index == plain ? 0 : draw.index, float(n + 1)};
      list->SetPipelineState(pipelines[draw.index != plain].Get());
      list->SetGraphicsRoot32BitConstants(0, 2, &constants_of, 0);
      list->DrawInstanced(3, 1, 0, 0);
    }
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
        into{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
    into.PlacedFootprint.Offset = c * slot;
    list->CopyTextureRegion(&into, 0, 0, 0, &from, nullptr);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
  }
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  const char *out;
  CHECK(readback->Map(0, nullptr, (void **)&out));
  unsigned failures = 0, drawn = 0;
  for (size_t c = 0; c < all.size(); c++) {
    auto want = expected(all[c]);
    drawn += std::count_if(want.begin(), want.end(), [](float v) { return v != 0; });
    failures += check("", c, want, out + c * slot, footprint.Footprint.RowPitch);
  }
  printf("%s: %u wrong pixels in %zu cases, %u pixels drawn\n", failures ? "failed" : "passed", failures, all.size(), drawn);
  return failures != 0;
}
