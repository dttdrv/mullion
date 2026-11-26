// contract: an occlusion query counts every sample its draws pass, in 64 bits, however many draws add up past 2^32.
// its result is the number of samples that passed the depth and stencil tests between BeginQuery and EndQuery, a
// UINT64, and a binary query's is 0 only where no sample passed (Microsoft Learn, D3D12_QUERY_TYPE and Queries); with
// no depth stencil view every sample a draw covers passes. Metal counts a slot's samples in 32 bits
// (tests/native/visibility_width.swift): 64 triangles over an 8192x8192 target, 2^26 samples each, count 0 in one slot.
// a pass stores the counts of its first 4096 slots, adds later ones to what the buffer holds and counts nothing from
// slot 16383 on (tests/native/visibility.swift): the many-draws query counts more draws than that in one pass of a
// small target. the wide target is 8192 x 8192, as Wine's test_occlusion_query draws on. the results start as a value
// no query gives.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_query_type
// https://learn.microsoft.com/en-us/windows/win32/direct3d12/queries
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
  step("compile shaders and create the device");
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto vs = compiler.compile(hlsl, "vs", "vs"), ps = compiler.compile(hlsl, "ps", "ps");
  if (!expect(!vs.empty() && !ps.empty(), "HLSL did not compile"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  auto rs = root_signature(device.Get(), {});
  if (!expect(bool(rs), "root signature was not created"))
    return verdict();
  const DXGI_FORMAT format = DXGI_FORMAT_R8_UNORM;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs), bytecode(ps)};
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = format;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));

  const UINT sides[] = {8192, 8};
  ComPtr<ID3D12Resource> targets[2];
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  D3D12_CPU_DESCRIPTOR_HANDLE rtv[2];
  for (UINT i = 0; i < 2; i++) {
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, sides[i], sides[i], 1, 1, format, {1, 0},
                                    D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &target_desc,
                                          D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&targets[i])));
    rtv[i] = rtvs->GetCPUDescriptorHandleForHeapStart();
    rtv[i].ptr += i * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    device->CreateRenderTargetView(targets[i].Get(), nullptr, rtv[i]);
  }

  const UINT64 wide = UINT64(sides[0]) * sides[0], small = UINT64(sides[1]) * sides[1];
  const UINT many = 4 * 4096 + 100;
  struct {
    const char *what;
    UINT target, draws, instances;
    UINT64 samples;
  } cases[] = {
      {"one triangle", 0, 1, 1, wide},
      {"100 triangles", 0, 100, 1, 100 * wide},
      {"40 draws of 3 instances", 0, 40, 3, 120 * wide},
      {"16484 draws in one pass", 1, many, 1, many * small},
      {"binary, 64 triangles of 2^32 samples", 0, 64, 1, 1},
  };
  const UINT queries = std::size(cases), binary = queries - 1;
  ComPtr<ID3D12QueryHeap> heap;
  D3D12_QUERY_HEAP_DESC query_desc{D3D12_QUERY_HEAP_TYPE_OCCLUSION, queries};
  CHECK(device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&heap)));
  auto results = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, queries * sizeof(UINT64), D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(bool(results), "the readback buffer was not created"))
    return verdict();
  CHECK(forget(results.Get()));

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  step("record a query of each case");
  for (UINT q = 0; q < queries; q++) {
    auto &c = cases[q];
    D3D12_VIEWPORT viewport{0, 0, (float)sides[c.target], (float)sides[c.target], 0, 1};
    D3D12_RECT scissor{0, 0, LONG(sides[c.target]), LONG(sides[c.target])};
    list->SetGraphicsRootSignature(rs.Get());
    list->OMSetRenderTargets(1, &rtv[c.target], FALSE, nullptr);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    auto type = q == binary ? D3D12_QUERY_TYPE_BINARY_OCCLUSION : D3D12_QUERY_TYPE_OCCLUSION;
    list->BeginQuery(heap.Get(), type, q);
    for (UINT i = 0; i < c.draws; i++)
      list->DrawInstanced(3, c.instances, 0, 0);
    list->EndQuery(heap.Get(), type, q);
  }
  list->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0, binary, results.Get(), 0);
  list->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_BINARY_OCCLUSION, binary, 1, results.Get(),
                         binary * sizeof(UINT64));
  step("submit and wait for its fence");
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  step("read every query");
  const UINT64 *counts;
  CHECK(results->Map(0, nullptr, (void **)&counts));
  for (UINT q = 0; q < queries; q++)
    expect(counts[q] == cases[q].samples, "%s: %llu (0x%llx), want %llu (0x%llx)", cases[q].what, counts[q], counts[q],
           cases[q].samples, cases[q].samples);
  results->Unmap(0, nullptr);
  CHECK(device->GetDeviceRemovedReason());
  return verdict();
}
