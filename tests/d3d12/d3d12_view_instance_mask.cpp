// contract: ID3D12GraphicsCommandList1::SetViewInstanceMask sets the views a pipeline with view instancing draws, and
// a pipeline without view instancing ignores it: "The view instance mask is only honored by PSOs that have view
// instancing enabled" (D3D12 View Instancing). on a device whose ViewInstancingTier is NOT_SUPPORTED no pipeline has
// view instancing, so the call changes nothing: a mask of 0 ("The view instance mask defaults to 0 (all views
// disabled)"), which would leave a view-instanced pipeline nothing to draw, and a mask past the first view still let
// every draw reach its target. a device that has view instancing fails the test, which then needs view-instanced
// pipelines to be meaningful.
// https://microsoft.github.io/DirectX-Specs/d3d/ViewInstancing.html
#include "d3d12_test.hpp"

static const char hlsl[] = R"(
float4 vs(uint id : SV_VertexID) : SV_Position {
  return float4(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0, 0.5, 1.0);
}
float4 ps() : SV_Target { return float4(1.0, 0.0, 0.0, 1.0); }
)";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler))
    return 77;
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS3 options3{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &options3, sizeof(options3)));
  if (!expect(options3.ViewInstancingTier == D3D12_VIEW_INSTANCING_TIER_NOT_SUPPORTED,
              "the device has view instancing tier %u: the test covers only pipelines without it",
              options3.ViewInstancingTier))
    return verdict();
  ComPtr<ID3D12CommandQueue> queue;
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  ComPtr<ID3D12GraphicsCommandList1> list;
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  auto vs = compiler.compile(hlsl, "vs", "vs"), ps = compiler.compile(hlsl, "ps", "ps");
  if (!expect(!vs.empty() && !ps.empty(), "the shaders do not compile"))
    return verdict();
  auto root = root_signature(device.Get(), {});
  CHECK(root ? S_OK : E_FAIL);
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{root.Get(), bytecode(vs), bytecode(ps)};
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = UINT_MAX;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  ComPtr<ID3D12PipelineState> pipeline;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)));

  constexpr UINT size = 4;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
                                  {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                        nullptr, IID_PPV_ARGS(&target)));
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 total;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);

  for (UINT mask : {0u, 2u, 1u}) {
    step("view instance mask %#x", mask);
    CHECK(forget(readback.Get()));
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), pipeline.Get()));
    const float clear[4] = {0, 0, 1, 1};
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    list->SetViewInstanceMask(mask);
    list->SetGraphicsRootSignature(root.Get());
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT viewport{0, 0, size, size, 0, 1};
    D3D12_RECT scissor{0, 0, size, size};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
        src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    dst.PlacedFootprint = footprint;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    BYTE *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    for (UINT y = 0; y < size; y++)
      for (UINT x = 0; x < size; x++) {
        UINT texel;
        memcpy(&texel, got + y * footprint.Footprint.RowPitch + x * 4, 4);
        expect(texel == 0xff0000ffu, "pixel %u,%u is %08x, want the draw's ff0000ff", x, y, texel);
      }
    readback->Unmap(0, nullptr);
  }
  return verdict();
}
