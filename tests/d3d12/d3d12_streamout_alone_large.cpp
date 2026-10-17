// contract: stream output without a geometry shader writes every vertex shader point of a draw, in order, and the
// same draw rasterizes stream 0 (D3D11.3 functional spec, 14.5). Mullion's mesh stage then starts no mesh
// threadgroups, so the draw has no mesh threadgroup limit: 2200000 points, 68750 object threadgroups of 32, stream out
// whole and their first 64 points cover the target, the others being clipped.
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
struct V { float4 pos : SV_Position; nointerpolation uint value : VALUE; };
V vs(uint id : SV_VertexID) {
  V v;
  v.pos = float4((id % 64 + 0.5) / 32 - 1, 0, id < 64 ? 0.5 : 2, 1);
  v.value = id + 1;
  return v;
}
uint ps(V v) : SV_Target { return v.value; }
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
  D3D12_ROOT_SIGNATURE_DESC rs_desc{0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT};
  auto rs = root_signature(device.Get(), rs_desc);
  auto vs = compiler.compile(hlsl, "vs", "vs"), ps = compiler.compile(hlsl, "ps", "ps");
  if (vs.empty() || ps.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  const D3D12_SO_DECLARATION_ENTRY entries[] = {{0, "VALUE", 0, 0, 1, 0}};
  const UINT strides[] = {4};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs), bytecode(ps)};
  desc.StreamOutput = {entries, 1, strides, 1, 0};
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));

  const UINT width = 64, points = 2200000;
  const UINT64 row = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, so_bytes = 4ull * points, filled_at = so_bytes;
  // the readback holds the streamed points, their filled size, then the target's row at a placement alignment
  const UINT64 pixels_at = (so_bytes + 8 + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) /
                           D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  ComPtr<ID3D12Resource> target;
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, 1, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  auto out = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, so_bytes + 8, D3D12_RESOURCE_STATE_COPY_DEST);
  auto zero = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 8, D3D12_RESOURCE_STATE_GENERIC_READ);
  void *mapped;
  CHECK(zero->Map(0, nullptr, &mapped));
  memset(mapped, 0, 8);
  zero->Unmap(0, nullptr);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, pixels_at + row, D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  list->CopyBufferRegion(out.Get(), filled_at, zero.Get(), 0, 8);
  transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
  const float clear[4] = {};
  list->ClearRenderTargetView(rtv, clear, 0, nullptr);
  list->SetGraphicsRootSignature(rs.Get());
  D3D12_VIEWPORT viewport{0, 0, (float)width, 1, 0, 1};
  D3D12_RECT scissor{0, 0, (LONG)width, 1};
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  auto base = out->GetGPUVirtualAddress();
  D3D12_STREAM_OUTPUT_BUFFER_VIEW view{base, so_bytes, base + filled_at};
  list->SOSetTargets(0, 1, &view);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
  list->DrawInstanced(points, 1, 0, 0);
  transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(), 0, out.Get(), 0, so_bytes + 8);
  transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
      src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  dst.PlacedFootprint = {pixels_at, {DXGI_FORMAT_R32_UINT, width, 1, 1, (UINT)row}};
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  const uint8_t *bytes;
  CHECK(readback->Map(0, nullptr, (void **)&bytes));
  auto values = reinterpret_cast<const uint32_t *>(bytes);
  auto filled = *reinterpret_cast<const uint64_t *>(bytes + filled_at);
  auto pixels = reinterpret_cast<const uint32_t *>(bytes + pixels_at);
  unsigned failures = 0;
  if (filled != so_bytes && failures++ < 12)
    printf("filled size: %llu, want %llu\n", (unsigned long long)filled, (unsigned long long)so_bytes);
  for (UINT p = 0; p < points; p++)
    if (values[p] != p + 1 && failures++ < 12)
      printf("streamed point %u: %u, want %u\n", p, values[p], p + 1);
  for (UINT x = 0; x < width; x++)
    if (pixels[x] != x + 1 && failures++ < 12)
      printf("pixel %u: %u, want %u\n", x, pixels[x], x + 1);
  readback->Unmap(0, nullptr);
  if (failures) {
    printf("failed: %u wrong values\n", failures);
    return 1;
  }
  printf("passed: %u points streamed out and %u pixels drawn by one draw without a geometry shader\n", points, width);
  return 0;
}
