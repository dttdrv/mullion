// contract: occlusion queries count the samples that pass while they run, binary ones say whether any did, across
// render passes and while other queries overlap them; timestamps read the GPU clock in order, inside the window the
// queue's clock calibration brackets. results resolve into a buffer from a later command list. the draws fill
// scissor rectangles of a one-sample target, so each count is an area. a query ended, begun and ended again before a
// resolve holds its second count, however long the first took to sum.
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps() : SV_Target { return 1; }
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
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  auto rs = root_signature(device.Get(), {});
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs), bytecode(ps)};
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));

  const UINT size = 8;
  ComPtr<ID3D12Resource> target;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
                                  {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);

  enum { Full, Binary, Scissored, Split, Overlap, Empty, EmptyBinary, Reused, Queries };
  ComPtr<ID3D12QueryHeap> occlusion, timestamps;
  D3D12_QUERY_HEAP_DESC occlusion_desc{D3D12_QUERY_HEAP_TYPE_OCCLUSION, Queries},
      timestamp_desc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2};
  CHECK(device->CreateQueryHeap(&occlusion_desc, IID_PPV_ARGS(&occlusion)));
  CHECK(device->CreateQueryHeap(&timestamp_desc, IID_PPV_ARGS(&timestamps)));
  // results land after a gap, to show the offset is honored
  const UINT64 gap = 16, bytes = gap + (Queries + 2) * sizeof(UINT64);
  auto results = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocators[2];
  ComPtr<ID3D12GraphicsCommandList> list, resolve;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  for (auto &a : allocators)
    CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0].Get(), pso.Get(), IID_PPV_ARGS(&list)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[1].Get(), nullptr, IID_PPV_ARGS(&resolve)));

  D3D12_VIEWPORT viewport{0, 0, (float)size, (float)size, 0, 1};
  const float black[4] = {};
  auto bind = [&] {
    list->SetGraphicsRootSignature(rs.Get());
    list->RSSetViewports(1, &viewport);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  };
  // a full-screen triangle, clipped to w x h pixels: that many samples
  auto draw = [&](LONG w, LONG h) {
    D3D12_RECT scissor{0, 0, w, h};
    list->RSSetScissorRects(1, &scissor);
    list->DrawInstanced(3, 1, 0, 0);
  };
  auto begin = [&](UINT q, bool binary = false) {
    list->BeginQuery(occlusion.Get(), binary ? D3D12_QUERY_TYPE_BINARY_OCCLUSION : D3D12_QUERY_TYPE_OCCLUSION, q);
  };
  auto end = [&](UINT q, bool binary = false) {
    list->EndQuery(occlusion.Get(), binary ? D3D12_QUERY_TYPE_BINARY_OCCLUSION : D3D12_QUERY_TYPE_OCCLUSION, q);
  };
  bind();
  list->EndQuery(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
  begin(Full);
  draw(size, size);
  end(Full);
  begin(Binary, true);
  draw(size, size);
  end(Binary, true);
  begin(Scissored);
  draw(3, 5);
  end(Scissored);
  // Split runs through a clear, which ends the render pass; Overlap starts and ends inside it, on either side
  begin(Split);
  draw(size, size);
  begin(Overlap);
  draw(2, 7);
  list->ClearRenderTargetView(rtv, black, 0, nullptr);
  bind();
  draw(6, 1);
  end(Split);
  draw(4, 4);
  end(Overlap);
  begin(Empty);
  end(Empty);
  begin(EmptyBinary, true);
  draw(0, 0);
  end(EmptyBinary, true);
  // the first count spans many passes, so its sum outlasts the second's
  begin(Reused);
  for (UINT pass = 0; pass < 64; pass++) {
    list->ClearRenderTargetView(rtv, black, 0, nullptr);
    bind();
    draw(size, size);
  }
  end(Reused);
  begin(Reused);
  draw(2, 3);
  end(Reused);
  list->EndQuery(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
  const UINT64 want[Queries] = {size * size, 1, 3 * 5, size * size + 2 * 7 + 6 * 1, 2 * 7 + 6 * 1 + 4 * 4, 0, 0, 2 * 3};

  // the results resolve in a list of their own, after the one that ran the queries
  resolve->ResolveQueryData(occlusion.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0, Queries, results.Get(), gap);
  resolve->ResolveQueryData(
      timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, results.Get(), gap + Queries * sizeof(UINT64)
  );
  CHECK(list->Close());
  UINT64 cpu, before, after;
  CHECK(queue->GetClockCalibration(&before, &cpu));
  ID3D12CommandList *lists[] = {list.Get()};
  queue->ExecuteCommandLists(1, lists);
  CHECK(submit(device.Get(), queue.Get(), resolve.Get()));
  CHECK(queue->GetClockCalibration(&after, &cpu));

  UINT64 *got;
  CHECK(results->Map(0, nullptr, (void **)&got));
  unsigned failures = 0;
  const char *names[] = {"full", "binary", "scissored", "split by a pass", "overlapping", "empty", "empty binary", "reused"};
  for (UINT q = 0; q < Queries; q++)
    if (got[gap / 8 + q] != want[q] && ++failures)
      printf("%s query: %llu, want %llu\n", names[q], got[gap / 8 + q], want[q]);
  auto t = got + gap / 8 + Queries;
  if (!(before <= t[0] && t[0] <= t[1] && t[1] <= after) && ++failures)
    printf("timestamps %llu, %llu not in order inside %llu to %llu\n", t[0], t[1], before, after);
  results->Unmap(0, nullptr);
  printf("%s: %u wrong results\n", failures ? "failed" : "passed", failures);
  return failures != 0;
}
