// contract: ExecuteIndirect draws what the direct draw with the same arguments draws. a triangle covering the target
// is drawn directly, then through a draw and an indexed draw command signature; each must color every pixel, with
// the vertex id the index buffer supplies.
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
struct V { float4 p : SV_Position; nointerpolation uint id : ID; };
V vs(uint id : SV_VertexID) {
  V v;
  float2 uv = float2((id << 1) & 2, id & 2);
  v.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
  v.id = id;
  return v;
}
float4 ps(V v) : SV_Target { return float4(1, 0, 0, 1); }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto vs = compiler.compile(hlsl, "vs", "vs"), ps = compiler.compile(hlsl, "ps", "ps");
  if (vs.empty() || ps.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  const UINT size = 8;
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  auto rs = root_signature(device.Get(), {});
  const DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = rs.Get();
  desc.VS = bytecode(vs);
  desc.PS = bytecode(ps);
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = format;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));

  ComPtr<ID3D12Resource> target;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1, format, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)));
  auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 bytes;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);

  const uint16_t indices[] = {0, 1, 2};
  const D3D12_DRAW_ARGUMENTS draw_args{3, 1, 0, 0};
  const D3D12_DRAW_INDEXED_ARGUMENTS indexed_args{3, 1, 0, 0, 0};
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_STATE_GENERIC_READ);
  char *mapped;
  CHECK(upload->Map(0, nullptr, (void **)&mapped));
  memcpy(mapped, indices, sizeof(indices));
  memcpy(mapped + 64, &draw_args, sizeof(draw_args));
  memcpy(mapped + 128, &indexed_args, sizeof(indexed_args));
  upload->Unmap(0, nullptr);
  const D3D12_INDEX_BUFFER_VIEW view{upload->GetGPUVirtualAddress(), sizeof(indices), DXGI_FORMAT_R16_UINT};
  D3D12_INDIRECT_ARGUMENT_DESC draw_desc{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW},
      indexed_desc{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED};
  const D3D12_COMMAND_SIGNATURE_DESC sig_descs[] = {
      {sizeof(draw_args), 1, &draw_desc}, {sizeof(indexed_args), 1, &indexed_desc}
  };
  ComPtr<ID3D12CommandSignature> sigs[2];
  for (int i = 0; i < 2; i++)
    CHECK(device->CreateCommandSignature(&sig_descs[i], nullptr, IID_PPV_ARGS(&sigs[i])));

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  const char *names[] = {"direct", "indirect", "indirect indexed"};
  unsigned failures = 0;
  for (int draw = 0; draw < 3; draw++) {
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), pso.Get()));
    const float clear[4] = {0, 0, 0, 0};
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    D3D12_VIEWPORT viewport{0, 0, size, size, 0, 1};
    D3D12_RECT scissor{0, 0, size, size};
    list->SetGraphicsRootSignature(rs.Get());
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->IASetIndexBuffer(&view);
    if (draw == 0)
      list->DrawInstanced(3, 1, 0, 0);
    else
      list->ExecuteIndirect(sigs[draw - 1].Get(), 1, upload.Get(), 64 * draw, nullptr, 0);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
        dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));

    char *pixels;
    CHECK(readback->Map(0, nullptr, (void **)&pixels));
    unsigned uncovered = 0;
    for (UINT y = 0; y < size; y++)
      for (UINT x = 0; x < size; x++)
        uncovered += *(const uint32_t *)(pixels + y * footprint.Footprint.RowPitch + 4 * x) != 0xff0000ffu;
    readback->Unmap(0, nullptr);
    printf("%s: %u of %u pixels not drawn\n", names[draw], uncovered, size * size);
    failures += uncovered;
  }
  printf("%s\n", failures ? "failed" : "passed");
  return failures != 0;
}
