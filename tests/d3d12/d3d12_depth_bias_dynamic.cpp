// contract: RSSetDepthBias's values hold until another pipeline state is set. a list changes the bias of a pipeline
// made with D3D12_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS through ID3D12GraphicsCommandList9::RSSetDepthBias, where
// D3D12_FEATURE_DATA_D3D12_OPTIONS16 has DynamicDepthBiasSupported. Windows keeps the values when the pipeline that
// is set is set again, takes the pipeline's own when another one is set, and gives a pipeline without the flag its
// own whatever was set before: vkd3d-proton's test_dynamic_depth_bias expects all three of Windows.
// a float depth buffer's bias unit is 2^(e - 23) for the exponent e of the primitive's largest z (D3D11.3 15.10):
// one unit in the last place at z = 0.5, so each draw's depth is 0.5 and its bias in the bits. each draw covers a
// column pair of its own.
#include "d3d12_test.hpp"

#ifndef __ID3D12GraphicsCommandList8_INTERFACE_DEFINED__
#define __ID3D12GraphicsCommandList8_INTERFACE_DEFINED__
MIDL_INTERFACE("ee936ef9-599d-4d28-938e-23c4ad05ce51")
ID3D12GraphicsCommandList8 : public ID3D12GraphicsCommandList7 {
  virtual void STDMETHODCALLTYPE OMSetFrontAndBackStencilRef(UINT FrontStencilRef, UINT BackStencilRef) = 0;
};
__CRT_UUID_DECL(ID3D12GraphicsCommandList8, 0xee936ef9, 0x599d, 0x4d28, 0x93, 0x8e, 0x23, 0xc4, 0xad, 0x05, 0xce, 0x51)
#endif
#ifndef __ID3D12GraphicsCommandList9_INTERFACE_DEFINED__
#define __ID3D12GraphicsCommandList9_INTERFACE_DEFINED__
// DirectX-Headers, d3d12.idl; absent from the installed SDK
MIDL_INTERFACE("34ed2808-ffe6-4c2b-b11a-cabd2b0c59e1")
ID3D12GraphicsCommandList9 : public ID3D12GraphicsCommandList8 {
  virtual void STDMETHODCALLTYPE RSSetDepthBias(FLOAT DepthBias, FLOAT DepthBiasClamp, FLOAT SlopeScaledDepthBias) = 0;
  virtual void STDMETHODCALLTYPE IASetIndexBufferStripCutValue(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE IBStripCutValue) = 0;
};
__CRT_UUID_DECL(ID3D12GraphicsCommandList9, 0x34ed2808, 0xffe6, 0x4c2b, 0xb1, 0x1a, 0xca, 0xbd, 0x2b, 0x0c, 0x59, 0xe1)
#endif
// D3D12_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS (DirectX-Headers, d3d12.idl), past the installed SDK's enumeration
constexpr UINT dynamic_depth_bias = 4;

static const char hlsl[] = R"hlsl(
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0.5, 1);
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto vs = compiler.compile(hlsl, "vs", "vs");
  if (vs.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS16 options{};
  if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &options, sizeof(options))) ||
      !options.DynamicDepthBiasSupported) {
    printf("skipped: no dynamic depth bias\n");
    return 77;
  }
  const DXGI_FORMAT depth_format = DXGI_FORMAT_D32_FLOAT;
  auto rs = root_signature(device.Get(), {});

  // the pipelines: two that take a list's bias, with biases of their own, and one that does not
  const struct {
    INT bias;
    bool dynamic;
  } kinds[] = {{1, true}, {4, true}, {2, false}};
  ComPtr<ID3D12PipelineState> pso[std::size(kinds)];
  for (UINT k = 0; k < std::size(kinds); k++) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs)};
    desc.SampleMask = ~0u;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.RasterizerState.DepthBias = kinds[k].bias;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.DSVFormat = depth_format;
    desc.SampleDesc = {1, 0};
    const UINT flags = kinds[k].dynamic ? dynamic_depth_bias : 0;
    memcpy(&desc.Flags, &flags, sizeof(flags));
    CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso[k])));
  }

  // each draw: the bias a list sets before it, if any, the pipeline it sets, if any, and the bias it is drawn with
  const struct {
    INT set;
    int pipeline;
    INT want;
    const char *what;
  } draws[] = {
      {0, 0, 1, "a pipeline's own bias"},
      {3, -1, 3, "a bias set on the list"},
      {0, 0, 3, "the same pipeline set again"},
      {0, 1, 4, "another pipeline"},
      {5, 0, 1, "a bias set before another pipeline"},
      {7, 2, 2, "a pipeline that takes no list's bias"},
  };
  const UINT pair = 2, width = std::size(draws) * pair, height = 2;
  ComPtr<ID3D12Resource> depth;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC depth_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, depth_format, {1, 0},
                                 D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL};
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&depth)
  ));
  ComPtr<ID3D12DescriptorHeap> dsvs;
  D3D12_DESCRIPTOR_HEAP_DESC dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
  CHECK(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsvs)));
  auto dsv = dsvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateDepthStencilView(depth.Get(), nullptr, dsv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 bytes;
  device->GetCopyableFootprints(&depth_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  CHECK(readback ? S_OK : E_FAIL);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12GraphicsCommandList9> list9;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list.As(&list9));

  const D3D12_VIEWPORT viewport{0, 0, (float)width, (float)height, 0, 1};
  list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
  list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
  list->SetGraphicsRootSignature(rs.Get());
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list->RSSetViewports(1, &viewport);
  for (UINT d = 0; d < std::size(draws); d++) {
    const D3D12_RECT scissor{LONG(d * pair), 0, LONG((d + 1) * pair), (LONG)height};
    if (draws[d].set)
      list9->RSSetDepthBias(float(draws[d].set), 0, 0);
    if (draws[d].pipeline >= 0)
      list->SetPipelineState(pso[draws[d].pipeline].Get());
    list->RSSetScissorRects(1, &scissor);
    list->DrawInstanced(3, 1, 0, 0);
  }
  transition(list.Get(), depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION from{depth.Get()}, to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  to.PlacedFootprint = footprint;
  list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  // 0.5 is 0x3f000000, and the floats from it to 1 are 2^-24 apart
  const UINT32 half = 0x3f000000;
  const UINT8 *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  for (UINT y = 0; y < height; y++)
    for (UINT x = 0; x < width; x++) {
      const auto &draw = draws[x / pair];
      UINT32 bits;
      memcpy(&bits, got + y * footprint.Footprint.RowPitch + x * 4, sizeof(bits));
      expect(bits == half + draw.want, "%s: depth %u,%u is 0.5 and %d units, want %d (bits %08x)", draw.what, x, y,
             int(bits - half), draw.want, bits);
    }
  readback->Unmap(0, nullptr);
  return verdict();
}
