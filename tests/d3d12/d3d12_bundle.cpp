// contract: ExecuteBundle runs a bundle's calls on the calling list. the bundle reads the caller's root arguments,
// scissors and target, and what it sets (pipeline state, topology, root constants) stays set in the caller afterwards.
// a bundle runs as often as it is executed, keeps copies of what its calls pointed to, and starts with the pipeline
// state it was created with. each draw writes a value to its own pixel column, each dispatch to a buffer element.
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
cbuffer C : register(b0) { uint value; uint column; };
RWStructuredBuffer<uint> o : register(u0);
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
uint ps() : SV_Target { return value; }
[numthreads(1, 1, 1)] void cs() { o[column] = value; }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto vs = compiler.compile(hlsl, "vs", "vs"), ps = compiler.compile(hlsl, "ps", "ps"),
       cs = compiler.compile(hlsl, "cs", "cs");
  if (vs.empty() || ps.empty() || cs.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[0].Constants.Num32BitValues = 2;
  auto rs = root_signature(device.Get(), {2, params});
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs), bytecode(ps)};
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> draw_pso, compute_pso;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&draw_pso)));
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc{rs.Get(), bytecode(cs)};
  CHECK(device->CreateComputePipelineState(&compute_desc, IID_PPV_ARGS(&compute_pso)));

  // what each column and element must hold, filled in as the list is recorded
  std::vector<UINT> columns, elements;
  const UINT width = 6;
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_STATE_GENERIC_READ);
  uint8_t *mapped;
  CHECK(upload->Map(0, nullptr, (void **)&mapped));
  const uint16_t indices[3] = {0, 1, 2};
  memcpy(mapped, indices, sizeof(indices));
  auto out = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, width * 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  ComPtr<ID3D12Resource> target;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, 1, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT + width * 4,
                         D3D12_RESOURCE_STATE_COPY_DEST);

  // draws: created with the draw pipeline, it sets only the topology. indexed: sets the pipeline, its value and an index
  // buffer whose view is gone before it runs. dispatch: sets the compute pipeline
  ComPtr<ID3D12CommandAllocator> bundle_allocator;
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&bundle_allocator)));
  ComPtr<ID3D12GraphicsCommandList> draws, indexed, dispatch;
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bundle_allocator.Get(), draw_pso.Get(), IID_PPV_ARGS(&draws)));
  draws->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  draws->DrawInstanced(3, 1, 0, 0);
  CHECK(draws->Close());
  const UINT indexed_value = 50;
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bundle_allocator.Get(), nullptr, IID_PPV_ARGS(&indexed)));
  indexed->SetPipelineState(draw_pso.Get());
  indexed->SetGraphicsRoot32BitConstant(0, indexed_value, 0);
  {
    D3D12_INDEX_BUFFER_VIEW ib{upload->GetGPUVirtualAddress(), sizeof(indices), DXGI_FORMAT_R16_UINT};
    indexed->IASetIndexBuffer(&ib);
    ib = {};
  }
  indexed->DrawIndexedInstanced(3, 1, 0, 0, 0);
  CHECK(indexed->Close());
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bundle_allocator.Get(), compute_pso.Get(), IID_PPV_ARGS(&dispatch)));
  dispatch->Dispatch(1, 1, 1);
  CHECK(dispatch->Close());

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  const float zero[4] = {};
  list->ClearRenderTargetView(rtv, zero, 0, nullptr);
  list->SetGraphicsRootSignature(rs.Get());
  list->SetComputeRootSignature(rs.Get());
  list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
  D3D12_VIEWPORT viewport{0, 0, (float)width, 1, 0, 1};
  list->RSSetViewports(1, &viewport);
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  auto column = [&](UINT c) {
    D3D12_RECT rect{(LONG)c, 0, (LONG)c + 1, 1};
    list->RSSetScissorRects(1, &rect);
  };
  auto draw = [&](ID3D12GraphicsCommandList *bundle, UINT value) {
    UINT c = columns.size();
    const UINT constants[2] = {value, c};
    list->SetGraphicsRoot32BitConstants(0, 2, constants, 0);
    column(c);
    list->ExecuteBundle(bundle);
    columns.push_back(value);
  };
  // the same bundle three times, each reading the caller's value and column
  for (UINT value : {10, 20, 30})
    draw(draws.Get(), value);
  // the pipeline and topology the bundle left: a draw of the caller's own
  const UINT constants[2] = {40, (UINT)columns.size()};
  list->SetGraphicsRoot32BitConstants(0, 2, constants, 0);
  column(columns.size());
  list->DrawInstanced(3, 1, 0, 0);
  columns.push_back(40);
  // a bundle's own value over the caller's, and still set for the caller's next draw
  draw(indexed.Get(), 0);
  columns.back() = indexed_value;
  list->SetGraphicsRoot32BitConstant(0, columns.size(), 1);
  column(columns.size());
  list->DrawInstanced(3, 1, 0, 0);
  columns.push_back(indexed_value);
  for (UINT value : {70, 80}) {
    const UINT args[2] = {value, (UINT)elements.size()};
    list->SetComputeRoot32BitConstants(0, 2, args, 0);
    list->ExecuteBundle(dispatch.Get());
    elements.push_back(value);
  }
  transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  dst.PlacedFootprint = {0, {DXGI_FORMAT_R32_UINT, width, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT}};
  D3D12_TEXTURE_COPY_LOCATION src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  list->CopyBufferRegion(readback.Get(), D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, out.Get(), 0, width * 4);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  UINT *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  unsigned failures = 0;
  for (UINT c = 0; c < columns.size(); c++)
    if (got[c] != columns[c] && ++failures)
      printf("column %u: %u, want %u\n", c, got[c], columns[c]);
  for (UINT e = 0; e < elements.size(); e++)
    if (got[D3D12_TEXTURE_DATA_PITCH_ALIGNMENT / 4 + e] != elements[e] && ++failures)
      printf("element %u: %u, want %u\n", e, got[D3D12_TEXTURE_DATA_PITCH_ALIGNMENT / 4 + e], elements[e]);
  readback->Unmap(0, nullptr);
  printf("%s: %u wrong values of %zu\n", failures ? "failed" : "passed", failures, columns.size() + elements.size());
  return failures != 0;
}
