// contract: a draw the device cannot run does not take the draws around it with it. a geometry shader of 32 instances
// over 96000 points is 3000 object threadgroups of 1024 mesh threadgroups, more than an Apple7 GPU runs in one draw
// (2^21), which fails its whole render pass there; on such a GPU Mullion draws it in parts when it is drawn directly
// and leaves it out when it comes from ExecuteIndirect. the small draws before and after it, in the same pass, still
// draw their points. the big draw's
// points are all clipped, so a GPU without the limit leaves the same pixels.
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
cbuffer Draw : register(b0) { uint base; };
struct V { uint id : ID; };
V vs(uint id : SV_VertexID) { V v; v.id = base + id; return v; }
struct P { float4 pos : SV_Position; nointerpolation uint value : VALUE; };
// point p comes out once, from geometry instance p % 32, on pixel p of the first 64 and clipped after them
[maxvertexcount(1)] [instance(32)]
void gs(point V v[1], uint i : SV_GSInstanceID, inout PointStream<P> s) {
  if (i != v[0].id % 32)
    return;
  P o;
  o.pos = float4((v[0].id % 64 + 0.5) / 32 - 1, 0, v[0].id < 64 ? 0.5 : 2, 1);
  o.value = v[0].id + 1;
  s.Append(o);
}
uint ps(P p) : SV_Target { return p.value; }
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
  D3D12_ROOT_PARAMETER parameter{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
  parameter.Constants = {0, 0, 1};
  parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
  auto rs = root_signature(device.Get(), {1, &parameter});
  auto vs = compiler.compile(hlsl, "vs", "vs"), gs = compiler.compile(hlsl, "gs", "gs"),
       ps = compiler.compile(hlsl, "ps", "ps");
  if (vs.empty() || gs.empty() || ps.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs), bytecode(ps)};
  desc.GS = bytecode(gs);
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

  const UINT width = 64, big = 96000;
  const UINT64 row = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
  ComPtr<ID3D12Resource> target;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, 1, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, row, D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);

  const D3D12_DRAW_ARGUMENTS big_args{big, 1, 0, 0};
  auto arguments = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_STATE_GENERIC_READ);
  void *mapped;
  CHECK(arguments->Map(0, nullptr, &mapped));
  memcpy(mapped, &big_args, sizeof(big_args));
  arguments->Unmap(0, nullptr);
  D3D12_INDIRECT_ARGUMENT_DESC draw_desc{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW};
  const D3D12_COMMAND_SIGNATURE_DESC sig_desc{sizeof(big_args), 1, &draw_desc};
  ComPtr<ID3D12CommandSignature> sig;
  CHECK(device->CreateCommandSignature(&sig_desc, nullptr, IID_PPV_ARGS(&sig)));

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  unsigned failures = 0;
  for (bool indirect : {false, true}) {
    const char *how = indirect ? "ExecuteIndirect" : "DrawInstanced";
    CHECK(forget(readback.Get()));
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), pso.Get()));
    const float clear[4] = {};
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    list->SetGraphicsRootSignature(rs.Get());
    D3D12_VIEWPORT viewport{0, 0, (float)width, 1, 0, 1};
    D3D12_RECT scissor{0, 0, (LONG)width, 1};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    // pixels 0 to 31, the clipped points 64 on, then pixels 32 to 63
    UINT base = 0;
    list->SetGraphicsRoot32BitConstants(0, 1, &base, 0);
    list->DrawInstanced(32, 1, 0, 0);
    base = 64;
    list->SetGraphicsRoot32BitConstants(0, 1, &base, 0);
    if (indirect)
      list->ExecuteIndirect(sig.Get(), 1, arguments.Get(), 0, nullptr, 0);
    else
      list->DrawInstanced(big, 1, 0, 0);
    base = 32;
    list->SetGraphicsRoot32BitConstants(0, 1, &base, 0);
    list->DrawInstanced(32, 1, 0, 0);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
        src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    dst.PlacedFootprint = {0, {DXGI_FORMAT_R32_UINT, width, 1, 1, (UINT)row}};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    HRESULT hr = submit(device.Get(), queue.Get(), list.Get());
    if (FAILED(hr)) {
      printf("failed: the list with the oversized %s draw returned %#lx\n", how, (unsigned long)hr);
      return 1;
    }
    const UINT *pixels;
    CHECK(readback->Map(0, nullptr, (void **)&pixels));
    for (UINT x = 0; x < width; x++)
      if (pixels[x] != x + 1 && failures++ < 12)
        printf("%s, pixel %u, drawn %s the oversized draw: %u, want %u\n", how, x, x < 32 ? "before" : "after",
               pixels[x], x + 1);
    readback->Unmap(0, nullptr);
  }
  if (failures) {
    printf("failed: %u wrong pixels\n", failures);
    return 1;
  }
  printf("passed: %u pixels around a draw of %u points, drawn directly and by ExecuteIndirect\n", width, big);
  return 0;
}
