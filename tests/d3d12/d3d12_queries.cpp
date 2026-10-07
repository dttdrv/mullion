// contract: occlusion queries count the samples that pass while they run, binary ones say whether any did, across
// render passes and while other queries overlap them; timestamps read the GPU clock in order, inside the window the
// queue's clock calibration brackets. results resolve into a buffer from a later command list. the draws fill
// scissor rectangles of a one-sample target, so each count is an area. a query ended, begun and ended again before a
// resolve holds its second count, however long the first took to sum. a pipeline whose sample description is left
// at zero draws with one sample, as applications leave it (Unreal Engine 5): the scissored query's draw has one.
// timestamp queries are as many as their heaps say, and heaps as many as the application makes. Metal has 32
// counter sample buffers at once, of 4096 samples each: the test has more heaps than that, a heap of more queries
// than all of them hold, with its first and last ended, and more queries ended in a row in one list than they hold.
// two queues that wait for a fence then hold lists that end a query each, all but one of Metal's buffers between
// them, while a third queue ends queries in lists of its own: every list gets a buffer in turn.
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
  ComPtr<ID3D12PipelineState> pso, unsampled;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));
  desc.SampleDesc = {};
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&unsampled)));

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
      timestamp_desc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 1 << 18}, small_desc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 1};
  CHECK(device->CreateQueryHeap(&occlusion_desc, IID_PPV_ARGS(&occlusion)));
  CHECK(device->CreateQueryHeap(&timestamp_desc, IID_PPV_ARGS(&timestamps)));
  ComPtr<ID3D12QueryHeap> small[40];
  for (auto &heap : small)
    CHECK(device->CreateQueryHeap(&small_desc, IID_PPV_ARGS(&heap)));
  // the timestamps in the order they are taken: the large heap's first query, the small heaps', a burst of the large
  // heap's, and its last
  const UINT burst = 33 * 4096, clocks = 1 + std::size(small) + burst + 1;
  // results land after a gap, to show the offset is honored
  const UINT64 gap = 16, bytes = gap + (Queries + clocks) * sizeof(UINT64);
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
  list->SetPipelineState(unsampled.Get());
  draw(3, 5);
  list->SetPipelineState(pso.Get());
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
  for (auto &heap : small)
    list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
  for (UINT q = 1; q <= burst; q++)
    list->EndQuery(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q);
  list->EndQuery(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, timestamp_desc.Count - 1);
  const UINT64 want[Queries] = {size * size, 1, 3 * 5, size * size + 2 * 7 + 6 * 1, 2 * 7 + 6 * 1 + 4 * 4, 0, 0, 2 * 3};

  // the results resolve in a list of their own, after the one that ran the queries
  resolve->ResolveQueryData(occlusion.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0, Queries, results.Get(), gap);
  UINT64 clock_at = gap + Queries * sizeof(UINT64);
  auto clock = [&](ID3D12QueryHeap *heap, UINT first, UINT count) {
    resolve->ResolveQueryData(heap, D3D12_QUERY_TYPE_TIMESTAMP, first, count, results.Get(), clock_at);
    clock_at += count * sizeof(UINT64);
  };
  clock(timestamps.Get(), 0, 1);
  for (auto &heap : small)
    clock(heap.Get(), 0, 1);
  clock(timestamps.Get(), 1, burst);
  clock(timestamps.Get(), timestamp_desc.Count - 1, 1);
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
  if (!(before <= t[0] && t[clocks - 1] <= after) && ++failures)
    printf("timestamps %llu to %llu not inside %llu to %llu\n", t[0], t[clocks - 1], before, after);
  for (UINT i = 1; i < clocks; i++)
    if (t[i - 1] > t[i] && failures++ < 8)
      printf("timestamp %u is %llu, after %llu\n", i, t[i], t[i - 1]);
  results->Unmap(0, nullptr);

  // queues 1 and 2 wait for a fence with lists behind it that each end a query, all but one of Metal's buffers
  // between them; queue 0 then runs lists that end one each, and the fence is signalled after them
  const UINT held[3] = {200, 16, 15};
  ComPtr<ID3D12Fence> gate;
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
  ComPtr<ID3D12CommandQueue> queues[3] = {queue};
  ComPtr<ID3D12QueryHeap> heaps[3];
  ComPtr<ID3D12CommandAllocator> held_allocators[3];
  ComPtr<ID3D12GraphicsCommandList> held_lists[3];
  UINT first[3], total = 0;
  for (UINT q = 0; q < 3; total += held[q++]) {
    first[q] = total;
    D3D12_QUERY_HEAP_DESC desc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, held[q]};
    if (q)
      CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queues[q])));
    CHECK(device->CreateQueryHeap(&desc, IID_PPV_ARGS(&heaps[q])));
    CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&held_allocators[q])));
    CHECK(device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, held_allocators[q].Get(), nullptr, IID_PPV_ARGS(&held_lists[q])
    ));
  }
  auto held_results = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, total * sizeof(UINT64), D3D12_RESOURCE_STATE_COPY_DEST);
  for (UINT q : {1, 2, 0}) {
    if (q)
      CHECK(queues[q]->Wait(gate.Get(), 1));
    for (UINT i = 0; i < held[q]; i++) {
      if (i)
        CHECK(held_lists[q]->Reset(held_allocators[q].Get(), nullptr));
      held_lists[q]->EndQuery(heaps[q].Get(), D3D12_QUERY_TYPE_TIMESTAMP, i);
      CHECK(held_lists[q]->Close());
      ID3D12CommandList *one[] = {held_lists[q].Get()};
      queues[q]->ExecuteCommandLists(1, one);
    }
  }
  CHECK(queue->Signal(gate.Get(), 1));
  for (UINT q = 0; q < 3; q++) {
    CHECK(held_lists[q]->Reset(held_allocators[q].Get(), nullptr));
    held_lists[q]->ResolveQueryData(
        heaps[q].Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, held[q], held_results.Get(), first[q] * sizeof(UINT64)
    );
    CHECK(submit(device.Get(), queues[q].Get(), held_lists[q].Get()));
  }
  UINT64 done;
  CHECK(queue->GetClockCalibration(&done, &cpu));
  CHECK(held_results->Map(0, nullptr, (void **)&got));
  for (UINT q = 0; q < 3; q++) {
    auto h = got + first[q];
    if (!(after <= h[0] && h[held[q] - 1] <= done) && ++failures)
      printf("queue %u: timestamps %llu to %llu not inside %llu to %llu\n", q, h[0], h[held[q] - 1], after, done);
    for (UINT i = 1; i < held[q]; i++)
      if (h[i - 1] > h[i] && failures++ < 8)
        printf("queue %u: timestamp %u is %llu, after %llu\n", q, i, h[i], h[i - 1]);
  }
  printf("%s: %u wrong results\n", failures ? "failed" : "passed", failures);
  return failures != 0;
}
