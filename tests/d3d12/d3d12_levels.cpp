// contract: the device's feature level is what its options make up, as D3D12 defines the levels, device creation
// refuses the levels above it, and the options it reports work: a 64KB standard swizzle texture exists exactly when
// the device says it does. logic ops are drawn: XOR, then AND_INVERTED, onto an
// integer target, which must hold what the two ops make of its clear value.
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
uint ps() : SV_Target { return SOURCE; }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  unsigned failures = 0;
  auto expect = [&](bool ok, const char *what) {
    if (!ok && ++failures)
      printf("%s\n", what);
  };

  D3D12_FEATURE_DATA_D3D12_OPTIONS o{};
  D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o)));
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof(o1)));
  auto want = !o.OutputMergerLogicOp ? D3D_FEATURE_LEVEL_11_0
              : o.TiledResourcesTier < D3D12_TILED_RESOURCES_TIER_2 || o.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_2 ||
                      !o.TypedUAVLoadAdditionalFormats
                  ? D3D_FEATURE_LEVEL_11_1
              : !o.ROVsSupported || !o.ConservativeRasterizationTier ? D3D_FEATURE_LEVEL_12_0
                                                                    : D3D_FEATURE_LEVEL_12_1;
  const D3D_FEATURE_LEVEL all[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_12_0,
                                   D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_2};
  D3D12_FEATURE_DATA_FEATURE_LEVELS levels{(UINT)std::size(all), all};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &levels, sizeof(levels)));
  expect(levels.MaxSupportedFeatureLevel == want, "the feature level is not the one the options make up");
  for (auto level : all) {
    ComPtr<ID3D12Device> other;
    bool created = SUCCEEDED(D3D12CreateDevice(nullptr, level, IID_PPV_ARGS(&other)));
    expect(created == (level <= want), level <= want ? "a supported level was refused" : "an unsupported level was created");
  }
  D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_HIGHEST_SHADER_MODEL};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model)));
  expect(model.HighestShaderModel == D3D_SHADER_MODEL_6_6, "shader model 6.6 not reported");
  expect(o1.TotalLaneCount && o1.TotalLaneCount % o1.WaveLaneCountMax == 0, "total lanes are not whole waves");
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC swizzled{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 256, 256, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0},
                               D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE};
  ComPtr<ID3D12Resource> texture;
  bool swizzle = SUCCEEDED(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &swizzled, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&texture)
  ));
  expect(swizzle == !!o.StandardSwizzle64KBSupported, "standard swizzle textures do not match the answer");
  if (!o.OutputMergerLogicOp) {
    printf("%s: %u wrong answers, logic ops not reported\n", failures ? "failed" : "passed", failures);
    return failures != 0;
  }

  const UINT size = 4, clear = 0x0ff0, first = 0x3c3c, second = 0x5555;
  auto vs = compiler.compile(hlsl, "vs", "vs", {"SOURCE=0"});
  auto rs = root_signature(device.Get(), {});
  ComPtr<ID3D12PipelineState> pso[2];
  const D3D12_LOGIC_OP ops[2] = {D3D12_LOGIC_OP_XOR, D3D12_LOGIC_OP_AND_INVERTED};
  for (int i = 0; i < 2; i++) {
    auto ps = compiler.compile(hlsl, "ps", "ps", {"SOURCE=" + std::to_string(i ? second : first)});
    if (vs.empty() || ps.empty()) {
      printf("failed: HLSL did not compile\n");
      return 1;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs), bytecode(ps)};
    desc.BlendState.RenderTarget[0].LogicOpEnable = TRUE;
    desc.BlendState.RenderTarget[0].LogicOp = ops[i];
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = ~0u;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
    desc.SampleDesc = {1, 0};
    CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso[i])));
  }
  ComPtr<ID3D12Resource> target;
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  const UINT64 row = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, row * size, D3D12_RESOURCE_STATE_COPY_DEST);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso[0].Get(), IID_PPV_ARGS(&list)));
  // an integer target clears to the color's values
  const float clear_color[4] = {clear, 0, 0, 0};
  list->ClearRenderTargetView(rtv, clear_color, 0, nullptr);
  list->SetGraphicsRootSignature(rs.Get());
  D3D12_VIEWPORT viewport{0, 0, (float)size, (float)size, 0, 1};
  D3D12_RECT scissor{0, 0, (LONG)size, (LONG)size};
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  for (auto &p : pso) {
    list->SetPipelineState(p.Get());
    list->DrawInstanced(3, 1, 0, 0);
  }
  transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  dst.PlacedFootprint = {0, {DXGI_FORMAT_R32_UINT, size, size, 1, (UINT)row}};
  D3D12_TEXTURE_COPY_LOCATION src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  UINT *texels;
  CHECK(readback->Map(0, nullptr, (void **)&texels));
  // XOR, then AND_INVERTED: the destination and not the source
  const UINT result = (clear ^ first) & ~second;
  for (UINT y = 0; y < size; y++)
    for (UINT x = 0; x < size; x++)
      if (texels[y * row / 4 + x] != result && failures++ < 4)
        printf("pixel (%u, %u): %#x, want %#x\n", x, y, texels[y * row / 4 + x], result);
  readback->Unmap(0, nullptr);
  printf("%s: %u wrong answers or pixels\n", failures ? "failed" : "passed", failures);
  return failures != 0;
}
