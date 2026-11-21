// contract: a full ClearRenderTargetView or ClearDepthStencilView clears the whole view ("NULL to clear the entire
// resource view", ID3D12GraphicsCommandList::ClearRenderTargetView, pRects), whatever is recorded after it: the passes
// that draw to some of the cleared views and not to others, in any order. two targets of one size and a depth buffer
// are cleared one after another, then a pass draws to the left half of the first target alone, another to the left
// half of the second, and the depth buffer is drawn into by no pass; every texel of the two targets and the depth
// buffer is read back: the left halves hold the draws' color and the rest, and all the depth buffer, what was cleared.
// the second target is cleared twice, and holds the later color. a second round binds the depth buffer with the first
// target and draws with the depth test, before the second target's pass; a third binds both its planes read-only
// (D3D12_DSV_FLAG_READ_ONLY_DEPTH and _STENCIL), so the draws test them and leave them as cleared. Mullion clears views
// of one size in one pass and makes each clear the load action of the first pass that draws to its view, view by view.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-clearrendertargetview
#include "d3d12_test.hpp"

static const char hlsl[] = R"(
float4 vs(uint id : SV_VertexID) : SV_Position {
  return float4(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0, 0.25, 1.0);
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
  ComPtr<ID3D12CommandQueue> queue;
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  auto vs = compiler.compile(hlsl, "vs", "vs"), ps = compiler.compile(hlsl, "ps", "ps");
  if (!expect(!vs.empty() && !ps.empty(), "the shaders do not compile"))
    return verdict();
  auto root = root_signature(device.Get(), {});
  CHECK(root ? S_OK : E_FAIL);
  // without the depth buffer, with it, and with it read-only
  ComPtr<ID3D12PipelineState> pipelines[3];
  for (UINT depth : {0u, 1u, 2u}) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{root.Get(), bytecode(vs), bytecode(ps)};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = depth != 0;
    desc.DepthStencilState.DepthWriteMask = depth == 1 ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    desc.DepthStencilState.StencilEnable = depth == 2;
    desc.DepthStencilState.StencilReadMask = 0xff;
    desc.DepthStencilState.FrontFace = desc.DepthStencilState.BackFace = {
        D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_EQUAL};
    desc.DSVFormat = depth ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_UNKNOWN;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipelines[depth])));
  }

  constexpr UINT size = 16;
  const D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
  const D3D12_RESOURCE_DESC color_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1,
                                       DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                       D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  const D3D12_RESOURCE_DESC depth_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1,
                                       DXGI_FORMAT_D32_FLOAT_S8X24_UINT, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                       D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL};
  ComPtr<ID3D12Resource> targets[2], depth;
  for (auto &target : targets)
    CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &color_desc,
                                          D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)));
  CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                        nullptr, IID_PPV_ARGS(&depth)));
  ComPtr<ID3D12DescriptorHeap> rtvs, dsvs;
  const D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2},
      dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  CHECK(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsvs)));
  D3D12_CPU_DESCRIPTOR_HANDLE rtv[2] = {rtvs->GetCPUDescriptorHandleForHeapStart(),
                                        rtvs->GetCPUDescriptorHandleForHeapStart()};
  rtv[1].ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  const auto dsv = dsvs->GetCPUDescriptorHandleForHeapStart();
  auto dsv_read_only = dsv;
  dsv_read_only.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
  for (UINT i = 0; i < 2; i++)
    device->CreateRenderTargetView(targets[i].Get(), nullptr, rtv[i]);
  device->CreateDepthStencilView(depth.Get(), nullptr, dsv);
  D3D12_DEPTH_STENCIL_VIEW_DESC read_only{DXGI_FORMAT_D32_FLOAT_S8X24_UINT, D3D12_DSV_DIMENSION_TEXTURE2D,
                                          D3D12_DSV_FLAG_READ_ONLY_DEPTH | D3D12_DSV_FLAG_READ_ONLY_STENCIL};
  device->CreateDepthStencilView(depth.Get(), &read_only, dsv_read_only);

  // a slot per target and one for each plane of the depth buffer
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT color_footprint, plane_footprints[2];
  UINT64 bytes;
  device->GetCopyableFootprints(&color_desc, 0, 1, 0, &color_footprint, nullptr, nullptr, &bytes);
  device->GetCopyableFootprints(&depth_desc, 0, 2, 0, plane_footprints, nullptr, nullptr, &bytes);
  constexpr UINT64 slot = 64 * 1024;
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 4 * slot, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(!!readback, "the readback buffer is refused"))
    return verdict();

  const float colors[2][4] = {{0, 0, 1, 1}, {0, 1, 0, 1}};
  const UINT32 cleared[2] = {0xffff0000u, 0xff00ff00u}, drawn = 0xff0000ffu;
  const float cleared_depth = 0.5f;
  const UINT8 cleared_stencil = 0x5a;
  const char *rounds[] = {"the depth buffer drawn into by no pass", "the depth buffer drawn with the first target",
                          "the depth buffer bound read-only with the first target"};
  for (UINT depth_drawn : {0u, 1u, 2u}) {
    step("%s", rounds[depth_drawn]);
    CHECK(forget(readback.Get()));
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), nullptr));
    list->ClearRenderTargetView(rtv[0], colors[0], 0, nullptr);
    list->ClearRenderTargetView(rtv[1], colors[0], 0, nullptr);
    list->ClearRenderTargetView(rtv[1], colors[1], 0, nullptr);
    list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, cleared_depth, cleared_stencil,
                                0, nullptr);
    list->SetGraphicsRootSignature(root.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VIEWPORT viewport{0, 0, size, size, 0, 1};
    const D3D12_RECT left{0, 0, size / 2, size};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &left);
    list->OMSetStencilRef(cleared_stencil);
    for (UINT i = 0; i < 2; i++) {
      UINT with_depth = i == 0 ? depth_drawn : 0;
      list->SetPipelineState(pipelines[with_depth].Get());
      list->OMSetRenderTargets(1, &rtv[i], FALSE, with_depth == 2 ? &dsv_read_only : with_depth ? &dsv : nullptr);
      list->DrawInstanced(3, 1, 0, 0);
    }
    for (UINT i = 0; i < 2; i++) {
      transition(list.Get(), targets[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION src{targets[i].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
          dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
      dst.PlacedFootprint = color_footprint;
      dst.PlacedFootprint.Offset = i * slot;
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      transition(list.Get(), targets[i].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    transition(list.Get(), depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (UINT plane = 0; plane < 2; plane++) {
      D3D12_TEXTURE_COPY_LOCATION src{depth.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
          dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
      src.SubresourceIndex = plane;
      dst.PlacedFootprint = plane_footprints[plane];
      dst.PlacedFootprint.Offset = (2 + plane) * slot;
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    transition(list.Get(), depth.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    CHECK(list->Close());
    CHECK(execute(device.Get(), queue.Get(), list.Get()));

    BYTE *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    UINT wrong = 0;
    for (UINT i = 0; i < 2; i++)
      for (UINT y = 0; y < size; y++)
        for (UINT x = 0; x < size; x++) {
          UINT32 texel;
          memcpy(&texel, got + i * slot + y * color_footprint.Footprint.RowPitch + x * 4, 4);
          UINT32 want = x < size / 2 ? drawn : cleared[i];
          if (texel != want && !wrong++)
            printf("target %u texel %u,%u is %08x, want %08x\n", i, x, y, texel, want);
        }
    for (UINT y = 0; y < size; y++)
      for (UINT x = 0; x < size; x++) {
        float value;
        memcpy(&value, got + 2 * slot + y * plane_footprints[0].Footprint.RowPitch + x * 4, 4);
        float want = depth_drawn == 1 && x < size / 2 ? 0.25f : cleared_depth;
        if (value != want && !wrong++)
          printf("depth texel %u,%u is %g, want %g\n", x, y, value, want);
        UINT8 stencil = got[3 * slot + y * plane_footprints[1].Footprint.RowPitch + x];
        if (stencil != cleared_stencil && !wrong++)
          printf("stencil texel %u,%u is %u, want %u\n", x, y, stencil, cleared_stencil);
      }
    readback->Unmap(0, nullptr);
    expect(!wrong, "%u wrong texels", wrong);
  }
  return verdict();
}
