// contract: a bundle's BeginQuery and EndQuery run on the list that executes it, as its other calls do: the methods
// a bundle may not call are clears, copies, DiscardResource, ExecuteBundle, ResourceBarrier, ResolveSubresource,
// SetPredication and the event and marker calls (Microsoft Learn, Creating and recording command lists and bundles,
// Bundle restrictions), and the query calls are not among them. an occlusion query's result is the number of samples
// that passed the depth and stencil tests between its BeginQuery and EndQuery (Microsoft Learn, D3D12_QUERY_TYPE and
// Queries), which is every sample a draw covers when no depth stencil view is bound.
// https://learn.microsoft.com/en-us/windows/win32/direct3d12/recording-command-lists-and-bundles
// https://learn.microsoft.com/en-us/windows/win32/direct3d12/queries
// each draw is a triangle over a target of `size` x `size` pixels, so it passes size * size samples. one bundle
// begins a query, draws and ends it; one begins a query and draws, and the list draws once more and ends it; one
// draws and ends a query the list began before a draw of its own. the results start as a value no query gives.
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
  if (!expect(!vs.empty() && !ps.empty(), "HLSL did not compile"))
    return verdict();
  constexpr UINT size = 4, queries = 3;
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  auto rs = root_signature(device.Get(), {});
  if (!expect(bool(rs), "root signature was not created"))
    return verdict();
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
  ComPtr<ID3D12Resource> target;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                                  0,
                                  size,
                                  size,
                                  1,
                                  1,
                                  desc.RTVFormats[0],
                                  {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                  D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  ComPtr<ID3D12QueryHeap> query_heap;
  D3D12_QUERY_HEAP_DESC query_desc{D3D12_QUERY_HEAP_TYPE_OCCLUSION, queries};
  CHECK(device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&query_heap)));
  auto results =
      buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, queries * sizeof(UINT64), D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(bool(results), "result buffer was not created"))
    return verdict();
  CHECK(forget(results.Get()));

  const auto type = D3D12_QUERY_TYPE_OCCLUSION;
  ComPtr<ID3D12CommandAllocator> bundle_allocator;
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&bundle_allocator)));
  // an allocator has one list recording at a time, so each bundle is closed before the next is made
  ComPtr<ID3D12GraphicsCommandList> whole, begins, ends;
  auto bundle = [&](ComPtr<ID3D12GraphicsCommandList> &made, auto record) -> HRESULT {
    HRESULT hr = device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bundle_allocator.Get(), pso.Get(), IID_PPV_ARGS(&made)
    );
    if (FAILED(hr))
      return hr;
    made->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    record(made.Get());
    return made->Close();
  };
  CHECK(bundle(whole, [&](ID3D12GraphicsCommandList *b) {
    b->BeginQuery(query_heap.Get(), type, 0);
    b->DrawInstanced(3, 1, 0, 0);
    b->EndQuery(query_heap.Get(), type, 0);
  }));
  CHECK(bundle(begins, [&](ID3D12GraphicsCommandList *b) {
    b->BeginQuery(query_heap.Get(), type, 1);
    b->DrawInstanced(3, 1, 0, 0);
  }));
  CHECK(bundle(ends, [&](ID3D12GraphicsCommandList *b) {
    b->DrawInstanced(3, 1, 0, 0);
    b->EndQuery(query_heap.Get(), type, 2);
  }));

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  step("queries a bundle begins, ends or both");
  list->SetGraphicsRootSignature(rs.Get());
  D3D12_VIEWPORT viewport{0, 0, float(size), float(size), 0, 1};
  D3D12_RECT scissor{0, 0, LONG(size), LONG(size)};
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list->ExecuteBundle(whole.Get());
  list->ExecuteBundle(begins.Get());
  list->DrawInstanced(3, 1, 0, 0);
  list->EndQuery(query_heap.Get(), type, 1);
  list->BeginQuery(query_heap.Get(), type, 2);
  list->DrawInstanced(3, 1, 0, 0);
  list->ExecuteBundle(ends.Get());
  list->ResolveQueryData(query_heap.Get(), type, 0, queries, results.Get(), 0);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  const UINT64 *got;
  CHECK(results->Map(0, nullptr, (void **)&got));
  const UINT64 draw = size * size, want[queries] = {draw, 2 * draw, 2 * draw};
  for (UINT q = 0; q < queries; q++)
    expect(got[q] == want[q], "query %u counted %llu samples, expected %llu", q, got[q], want[q]);
  results->Unmap(0, nullptr);
  return verdict();
}
