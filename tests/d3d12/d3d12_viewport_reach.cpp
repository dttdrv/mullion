// contract: a primitive may reach any distance past the viewport ("There are no restrictions to the range of input
// vertex coordinates to clipping", D3D11.3 15.4), and its depth is the plane through its vertices: "the input Z must
// exactly match the fixed function interpolated Z", set up from the snapped positions (15.16.2), in float32 (3.1).
// two triangles of a depth plane reach from 3 to 10000 viewports past each target's center; every pixel's SV_Position.z
// is that plane at the pixel's center within 16 ULPs of 1: clipping's new vertices, the plane's setup and its
// evaluation take fewer than 16 float32 steps, each within an ULP of a value below 2 (3.1.3), and snapping moves a
// vertex 1/512 of a pixel, which moves this plane by far less.
// Mullion enlarges Metal's viewports and scales positions back (EnlargeViewports), and Metal loses the plane once the
// cut of an enlarged viewport passes that of the largest (#72, bgfx's shadow volumes and shadow maps): the targets are
// small ones, whose viewports grow most, a frame's, ones whose enlarged cut ends on that bound in x and in y, and the
// largest, which is not enlarged
#include "d3d12_test.hpp"
#include <cmath>

static const char hlsl[] = R"hlsl(
cbuffer C : register(b0) { float reach; }
float4 vs(uint id : SV_VertexID) : SV_Position {
  // a quad over [-reach, reach] whose diagonal is off the center
  const float2 corners[6] = {float2(-1, -0.9), float2(1.1, -1), float2(1, 1), float2(-1, -0.9), float2(1, 1),
                             float2(-0.95, 1.05)};
  float2 p = corners[id] * reach;
  return float4(p, 0.5 + 0.25 * p.x / reach + 0.125 * p.y / reach, 1);
}
float ps(float4 p : SV_Position) : SV_Target { return p.z; }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto vs = compiler.compile(hlsl, "vs", "vs"), ps = compiler.compile(hlsl, "ps", "ps");
  if (!expect(!vs.empty() && !ps.empty(), "shaders compile"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER constants{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
  constants.Constants = {0, 0, 1};
  auto rs = root_signature(device.Get(), {1, &constants});
  const DXGI_FORMAT format = DXGI_FORMAT_R32_FLOAT;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs), bytecode(ps)};
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = format;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)));
  auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  const float tolerance = 16 * std::ldexp(1.f, -23);
  const UINT largest = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
  const UINT sizes[][2] = {{256, 256}, {1280, 720}, {largest / 4, 64}, {64, largest / 4}, {largest, 64}};
  for (auto [width, height] : sizes) {
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, format, {1, 0},
                                    D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    ComPtr<ID3D12Resource> target;
    CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                          nullptr, IID_PPV_ARGS(&target)));
    device->CreateRenderTargetView(target.Get(), nullptr, rtv);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT64 bytes;
    device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    for (float reach : {3.f, 30.f, 100.f, 1000.f, 10000.f}) {
      step("a depth plane reaching %g viewports past a %ux%u target", reach, width, height);
      CHECK(forget(readback.Get()));
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), pso.Get()));
      const float clear[4] = {-1, -1, -1, -1};
      list->ClearRenderTargetView(rtv, clear, 0, nullptr);
      D3D12_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
      D3D12_RECT scissor{0, 0, LONG(width), LONG(height)};
      list->SetGraphicsRootSignature(rs.Get());
      list->SetGraphicsRoot32BitConstants(0, 1, &reach, 0);
      list->RSSetViewports(1, &viewport);
      list->RSSetScissorRects(1, &scissor);
      list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
      list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      list->DrawInstanced(6, 1, 0, 0);
      transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
          to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
      list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
      transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      const char *texels;
      CHECK(readback->Map(0, nullptr, (void **)&texels));
      // how many pixels are off the plane, and the first few
      UINT off = 0;
      for (UINT y = 0; y < height; y++)
        for (UINT x = 0; x < width; x++) {
          float got;
          memcpy(&got, texels + y * footprint.Footprint.RowPitch + x * sizeof(float), sizeof(got));
          double nx = (x + 0.5) / width * 2 - 1, ny = 1 - (y + 0.5) / height * 2;
          double want = 0.5 + 0.25 * nx / reach + 0.125 * ny / reach;
          if (!(std::fabs(got - want) <= tolerance) && off++ < 3)
            printf("  %u,%u: %.9g, the plane %.9g\n", x, y, got, want);
        }
      readback->Unmap(0, nullptr);
      expect(!off, "reach %g, %ux%u: %u pixels off the plane", reach, width, height, off);
    }
  }
  return verdict();
}
