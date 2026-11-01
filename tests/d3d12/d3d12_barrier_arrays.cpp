// contract: every barrier of a ResourceBarrier or Barrier call counts ("Using Resource Barriers to Synchronize Resource
// States"), and a list's work runs in order, so a copy recorded after a ClearRenderTargetView reads the cleared texels
// (D3D11.3 4.2, Fixed Order of Pipeline Results). a target holding blue from an earlier list is cleared to red, then
// one call of two barriers hands an unrelated target to the pixel shader and the cleared one to a copy, which reads it
// back; a second call hands both back to render targets (with legacy barriers after a UAV barrier, a third), and a
// draw covers the cleared target's left half. the copy reads all red, and the target ends red with the draw's yellow on the left. the same
// runs with legacy barriers and with enhanced barriers (ID3D12GraphicsCommandList7::Barrier) on resources made in
// barrier layouts. Mullion makes a full clear the load action of the first later pass that draws to the view, unless a
// barrier names its texture first.
// https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12
#include "d3d12_test.hpp"

static const char hlsl[] = R"(
float4 vs(uint id : SV_VertexID) : SV_Position {
  return float4(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0, 0.25, 1.0);
}
float4 ps() : SV_Target { return float4(1.0, 1.0, 0.0, 1.0); }
)";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler))
    return 77;
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  ComPtr<ID3D12GraphicsCommandList7> list;
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

  D3D12_FEATURE_DATA_D3D12_OPTIONS12 options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options, sizeof(options)));
  ComPtr<ID3D12Device10> device10;
  const bool enhanced_supported = options.EnhancedBarriersSupported && SUCCEEDED(device.As(&device10));

  constexpr UINT size = 16;
  const D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
  const D3D12_RESOURCE_DESC1 color_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1,
                                        DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 bytes;
  device->GetCopyableFootprints((const D3D12_RESOURCE_DESC *)&color_desc, 0, 1, 0, &footprint, nullptr, nullptr,
                                &bytes);
  constexpr UINT64 slot = 64 * 1024;
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 2 * slot, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(!!readback, "the readback buffer is refused"))
    return verdict();
  ComPtr<ID3D12DescriptorHeap> rtvs;
  const D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  D3D12_CPU_DESCRIPTOR_HANDLE rtv[2] = {rtvs->GetCPUDescriptorHandleForHeapStart(),
                                        rtvs->GetCPUDescriptorHandleForHeapStart()};
  rtv[1].ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

  const float blue[4] = {0, 0, 1, 1}, red[4] = {1, 0, 0, 1};
  const UINT32 cleared = 0xff0000ffu, drawn = 0xff00ffffu;
  for (bool enhanced : {false, true}) {
    step("%s barriers", enhanced ? "enhanced" : "legacy");
    if (enhanced && !enhanced_supported) {
      printf("enhanced barriers are not supported\n");
      continue;
    }
    // the cleared target, and one the barriers name first
    ComPtr<ID3D12Resource> resources[2];
    auto &target = resources[0], &other = resources[1];
    for (auto &resource : resources) {
      if (enhanced) {
        CHECK(device10->CreateCommittedResource3(&props, D3D12_HEAP_FLAG_NONE, &color_desc,
                                                 D3D12_BARRIER_LAYOUT_RENDER_TARGET, nullptr, nullptr, 0, nullptr,
                                                 IID_PPV_ARGS(&resource)));
      } else {
        CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, (const D3D12_RESOURCE_DESC *)&color_desc,
                                              D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&resource)));
      }
    }
    device->CreateRenderTargetView(target.Get(), nullptr, rtv[0]);
    device->CreateRenderTargetView(other.Get(), nullptr, rtv[1]);

    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), nullptr));
    list->ClearRenderTargetView(rtv[0], blue, 0, nullptr);
    list->ClearRenderTargetView(rtv[1], blue, 0, nullptr);
    CHECK(list->Close());
    CHECK(execute(device.Get(), queue.Get(), list.Get()));
    CHECK(forget(readback.Get()));

    // to_copy: the other target to the pixel shader and the cleared one to a copy; back: both to render targets,
    // with legacy barriers after a UAV barrier on the other one
    auto barriers = [&](bool to_copy) {
      if (enhanced) {
        D3D12_TEXTURE_BARRIER texture[2]{};
        for (UINT i = 0; i < 2; i++) {
          auto &barrier = texture[i];
          barrier.pResource = i ? target.Get() : other.Get();
          barrier.Subresources.IndexOrFirstMipLevel = 0xffffffff;
          D3D12_BARRIER_SYNC sync = i ? D3D12_BARRIER_SYNC_COPY : D3D12_BARRIER_SYNC_PIXEL_SHADING;
          D3D12_BARRIER_ACCESS access = i ? D3D12_BARRIER_ACCESS_COPY_SOURCE : D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
          D3D12_BARRIER_LAYOUT layout = i ? D3D12_BARRIER_LAYOUT_COPY_SOURCE : D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
          barrier.SyncBefore = to_copy ? D3D12_BARRIER_SYNC_RENDER_TARGET : sync;
          barrier.SyncAfter = to_copy ? sync : D3D12_BARRIER_SYNC_RENDER_TARGET;
          barrier.AccessBefore = to_copy ? D3D12_BARRIER_ACCESS_RENDER_TARGET : access;
          barrier.AccessAfter = to_copy ? access : D3D12_BARRIER_ACCESS_RENDER_TARGET;
          barrier.LayoutBefore = to_copy ? D3D12_BARRIER_LAYOUT_RENDER_TARGET : layout;
          barrier.LayoutAfter = to_copy ? layout : D3D12_BARRIER_LAYOUT_RENDER_TARGET;
        }
        // a global barrier names every resource, so the call has the two texture barriers alone
        D3D12_BARRIER_GROUP group{D3D12_BARRIER_TYPE_TEXTURE, 2};
        group.pTextureBarriers = texture;
        list->Barrier(1, &group);
        return;
      }
      D3D12_RESOURCE_BARRIER legacy[3]{};
      legacy[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
      legacy[0].UAV.pResource = other.Get();
      for (UINT i = 0; i < 2; i++) {
        auto &barrier = legacy[1 + i].Transition;
        barrier.pResource = i ? target.Get() : other.Get();
        barrier.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        D3D12_RESOURCE_STATES state =
            i ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.StateBefore = to_copy ? D3D12_RESOURCE_STATE_RENDER_TARGET : state;
        barrier.StateAfter = to_copy ? state : D3D12_RESOURCE_STATE_RENDER_TARGET;
      }
      if (to_copy)
        list->ResourceBarrier(2, legacy + 1);
      else
        list->ResourceBarrier(3, legacy);
    };
    auto copy = [&](UINT to) {
      D3D12_TEXTURE_COPY_LOCATION src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
          dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
      dst.PlacedFootprint = footprint;
      dst.PlacedFootprint.Offset = to * slot;
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    };

    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), nullptr));
    list->ClearRenderTargetView(rtv[0], red, 0, nullptr);
    barriers(true);
    copy(0);
    barriers(false);
    list->SetGraphicsRootSignature(root.Get());
    list->SetPipelineState(pipeline.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VIEWPORT viewport{0, 0, size, size, 0, 1};
    const D3D12_RECT left{0, 0, size / 2, size};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &left);
    list->OMSetRenderTargets(1, &rtv[0], FALSE, nullptr);
    list->DrawInstanced(3, 1, 0, 0);
    barriers(true);
    copy(1);
    barriers(false);
    CHECK(list->Close());
    CHECK(execute(device.Get(), queue.Get(), list.Get()));

    BYTE *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    UINT wrong = 0;
    for (UINT copied = 0; copied < 2; copied++)
      for (UINT y = 0; y < size; y++)
        for (UINT x = 0; x < size; x++) {
          UINT32 texel;
          memcpy(&texel, got + copied * slot + y * footprint.Footprint.RowPitch + x * 4, 4);
          UINT32 want = copied && x < size / 2 ? drawn : cleared;
          if (texel != want && !wrong++)
            printf("%s: texel %u,%u is %08x, want %08x\n", copied ? "after the draw" : "the copy after the clear", x,
                   y, texel, want);
        }
    readback->Unmap(0, nullptr);
    expect(!wrong, "%u wrong texels", wrong);
  }
  return verdict();
}
