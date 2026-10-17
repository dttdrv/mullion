// contract: ExecuteIndirect runs each of its commands as the direct call with those arguments would, and the commands
// do not depend on one another (DirectX-Specs, Indirect Drawing). 262144 draw commands of one point each, the point of
// command n at pixel n of a 512x512 target, color each pixel with its command's number. the number comes from vertex
// data, which a draw reads from StartVertexLocation on (D3D11.3 8.6.1), while VertexID starts at 0 (8.16). since
// nothing ties one command to the next, the list that holds them must not take time in proportion to them on one GPU
// thread: its second run, once every pipeline exists, finishes within 200 ms (a resolver that walks the commands in
// one thread takes about 2.4 us each on an M1 Pro, over 600 ms here).
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
struct V { float4 p : SV_Position; nointerpolation uint n : N; };
V vs(uint id : ID) {
  V v;
  v.p = float4((float2(id % SIDE, id / SIDE) + 0.5) * float2(2, -2) / SIDE + float2(-1, 1), 0, 1);
  v.n = id + 1;
  return v;
}
uint ps(V v) : SV_Target { return v.n; }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const UINT side = 512, commands = side * side;
  const double limit_ms = 200;
  const std::vector<std::string> defines = {"SIDE=" + std::to_string(side)};
  auto vs = compiler.compile(hlsl, "vs", "vs", defines), ps = compiler.compile(hlsl, "ps", "ps", defines);
  if (vs.empty() || ps.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  auto rs = root_signature(device.Get(), {0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT});
  const DXGI_FORMAT format = DXGI_FORMAT_R32_UINT;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = rs.Get();
  desc.VS = bytecode(vs), desc.PS = bytecode(ps);
  D3D12_INPUT_ELEMENT_DESC element{"ID", 0, DXGI_FORMAT_R32_UINT};
  desc.InputLayout = {&element, 1};
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
  desc.NumRenderTargets = 1, desc.RTVFormats[0] = format;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));

  ComPtr<ID3D12Resource> target;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, side, side, 1, 1, format, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 bytes;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);

  // vertex n holds n, and command n draws vertex n alone
  auto vertices = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, commands * sizeof(UINT), D3D12_RESOURCE_STATE_GENERIC_READ);
  UINT *ids;
  CHECK(vertices->Map(0, nullptr, (void **)&ids));
  for (UINT n = 0; n < commands; n++)
    ids[n] = n;
  vertices->Unmap(0, nullptr);
  D3D12_VERTEX_BUFFER_VIEW vbv{vertices->GetGPUVirtualAddress(), UINT(commands * sizeof(UINT)), sizeof(UINT)};
  auto arguments = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, commands * sizeof(D3D12_DRAW_ARGUMENTS), D3D12_RESOURCE_STATE_GENERIC_READ);
  D3D12_DRAW_ARGUMENTS *args;
  CHECK(arguments->Map(0, nullptr, (void **)&args));
  for (UINT n = 0; n < commands; n++)
    args[n] = {1, 1, n, 0};
  arguments->Unmap(0, nullptr);
  D3D12_INDIRECT_ARGUMENT_DESC draw{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW};
  D3D12_COMMAND_SIGNATURE_DESC sig_desc{sizeof(D3D12_DRAW_ARGUMENTS), 1, &draw};
  ComPtr<ID3D12CommandSignature> sig;
  CHECK(device->CreateCommandSignature(&sig_desc, nullptr, IID_PPV_ARGS(&sig)));

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  const float zero[4] = {};
  list->ClearRenderTargetView(rtv, zero, 0, nullptr);
  list->SetGraphicsRootSignature(rs.Get());
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  D3D12_VIEWPORT viewport{0, 0, (float)side, (float)side, 0, 1};
  D3D12_RECT scissor{0, 0, (LONG)side, (LONG)side};
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
  list->IASetVertexBuffers(0, 1, &vbv);
  list->ExecuteIndirect(sig.Get(), commands, arguments.Get(), 0, nullptr, 0);
  transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
      dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
  CHECK(list->Close());

  // the first run makes what the list needs; the second is timed
  LARGE_INTEGER frequency, start, end;
  QueryPerformanceFrequency(&frequency);
  CHECK(execute(device.Get(), queue.Get(), list.Get()));
  CHECK(forget(readback.Get()));
  QueryPerformanceCounter(&start);
  CHECK(execute(device.Get(), queue.Get(), list.Get()));
  QueryPerformanceCounter(&end);
  double ms = double(end.QuadPart - start.QuadPart) * 1000 / frequency.QuadPart;

  char *pixels;
  CHECK(readback->Map(0, nullptr, (void **)&pixels));
  unsigned failures = 0;
  for (UINT n = 0; n < commands; n++) {
    UINT got = *(const UINT *)(pixels + (n / side) * footprint.Footprint.RowPitch + 4 * (n % side));
    if (got != n + 1 && failures++ < 12)
      printf("pixel of command %u: %u, want %u\n", n, got, n + 1);
  }
  readback->Unmap(0, nullptr);
  if (ms > limit_ms && failures++ < 12)
    printf("the list of %u commands took %.1f ms, more than %.0f ms\n", commands, ms, limit_ms);
  printf("%s: %u wrong results; %u commands in %.1f ms\n", failures ? "failed" : "passed", failures, commands, ms);
  return failures != 0;
}
