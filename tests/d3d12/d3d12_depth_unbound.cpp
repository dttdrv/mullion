// contract: a pipeline made for a depth stencil format draws without a depth stencil view as if it had no depth
// state. "If no Depth Buffer is bound, the depth test always passes." (D3D11.3 17.11) and "If there is no stencil
// component in the Depth Buffer format, or no DepthBbuffer bound, then the stencil test always passes." (17.12), which
// D3D12 keeps: OMSetRenderTargets takes a null view. so a draw without a view reaches its target whatever the depth
// function, the stencil function or the depth its pixel shader outputs, and the same draws with a view bound and a
// function of NEVER reach nothing, which shows the state is the pipeline's. source: D3D11_3_FunctionalSpec.htm.
// each draw covers a column pair of its own of one target.
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0.5, 1);
}
float4 ps() : SV_Target { return float4(0, 1, 0, 1); }
struct Deep {
  float4 color : SV_Target;
  float depth : SV_Depth;
};
Deep ps_depth() {
  Deep to;
  to.color = float4(0, 1, 0, 1);
  to.depth = 0.25;
  return to;
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto vs = compiler.compile(hlsl, "vs", "vs"), ps = compiler.compile(hlsl, "ps", "ps"),
       ps_depth = compiler.compile(hlsl, "ps_depth", "ps");
  if (vs.empty() || ps.empty() || ps_depth.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  const DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM, depth_format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  auto rs = root_signature(device.Get(), {});

  // the pipelines: a shader that outputs depth under a test that always passes and one that never does, and a
  // shader that outputs none under depth and stencil tests that never pass
  const struct {
    const std::string &shader;
    D3D12_COMPARISON_FUNC depth;
    bool stencil;
    const char *what;
  } kinds[] = {
      {ps_depth, D3D12_COMPARISON_FUNC_ALWAYS, false, "a shader that outputs depth, depth ALWAYS"},
      {ps_depth, D3D12_COMPARISON_FUNC_NEVER, false, "a shader that outputs depth, depth NEVER"},
      {ps, D3D12_COMPARISON_FUNC_NEVER, true, "depth NEVER and stencil NEVER"},
  };
  ComPtr<ID3D12PipelineState> pso[std::size(kinds)];
  for (UINT k = 0; k < std::size(kinds); k++) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs), bytecode(kinds[k].shader)};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = ~0u;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = kinds[k].depth;
    desc.DepthStencilState.StencilEnable = kinds[k].stencil;
    desc.DepthStencilState.StencilReadMask = desc.DepthStencilState.StencilWriteMask = 0xff;
    desc.DepthStencilState.FrontFace = desc.DepthStencilState.BackFace = {
        D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_NEVER
    };
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    // a second target the draws do not bind, as a pass may bind fewer than its pipeline has: Metal takes a pipeline
    // of the pass's own formats then, depth among them
    desc.NumRenderTargets = 2;
    desc.RTVFormats[0] = desc.RTVFormats[1] = format;
    desc.DSVFormat = depth_format;
    desc.SampleDesc = {1, 0};
    CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso[k])));
  }

  // a column pair for each pipeline without a view, then for each with one
  const UINT pair = 2, draws = 2 * std::size(kinds), width = draws * pair, height = 2;
  ComPtr<ID3D12Resource> target, depth;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, format, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  auto depth_desc = target_desc;
  depth_desc.Format = depth_format;
  depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&depth)
  ));
  ComPtr<ID3D12DescriptorHeap> rtvs, dsvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1}, dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  CHECK(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart(), dsv = dsvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  device->CreateDepthStencilView(depth.Get(), nullptr, dsv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 bytes;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  CHECK(readback ? S_OK : E_FAIL);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));

  const float black[4] = {0, 0, 0, 1};
  const D3D12_VIEWPORT viewport{0, 0, (float)width, (float)height, 0, 1};
  list->ClearRenderTargetView(rtv, black, 0, nullptr);
  list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1, 0, 0, nullptr);
  list->SetGraphicsRootSignature(rs.Get());
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list->RSSetViewports(1, &viewport);
  for (UINT d = 0; d < draws; d++) {
    const bool viewed = d >= std::size(kinds);
    const D3D12_RECT scissor{LONG(d * pair), 0, LONG((d + 1) * pair), (LONG)height};
    list->OMSetRenderTargets(1, &rtv, FALSE, viewed ? &dsv : nullptr);
    list->SetPipelineState(pso[d % std::size(kinds)].Get());
    list->RSSetScissorRects(1, &scissor);
    list->DrawInstanced(3, 1, 0, 0);
  }
  transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION from{target.Get()}, to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  to.PlacedFootprint = footprint;
  list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  const UINT32 green = 0xff00ff00, cleared = 0xff000000;
  const UINT8 *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  for (UINT y = 0; y < height; y++)
    for (UINT x = 0; x < width; x++) {
      const UINT d = x / pair, k = d % std::size(kinds);
      const bool viewed = d >= std::size(kinds);
      // with a view, only the tests that always pass let the draw through
      const UINT32 want = !viewed || kinds[k].depth == D3D12_COMPARISON_FUNC_ALWAYS ? green : cleared;
      UINT32 texel;
      memcpy(&texel, got + y * footprint.Footprint.RowPitch + x * 4, sizeof(texel));
      expect(texel == want, "%s, %s a depth stencil view: texel %u,%u is %08x, want %08x", kinds[k].what,
             viewed ? "with" : "without", x, y, texel, want);
    }
  readback->Unmap(0, nullptr);
  return verdict();
}
