// contract: a sampler's custom border color (D3D11.3 7.18.9.1) reaches every pipeline that samples through it,
// including pipelines made before the first sampler with a custom color: a pipeline state is fixed when it is made
// (ID3D12Device::CreateGraphicsPipelineState), and nothing in D3D12 orders it after the samplers it will read. a draw
// through an input layout and a dispatch sample only the border, made of colors Metal's samplers lack
#include "d3d12_test.hpp"
#include <cmath>

static const char hlsl[] = R"hlsl(
Texture2D<float4> t : register(t0);
SamplerState s : register(s0);
RWStructuredBuffer<float4> o : register(u0);
float4 border() { return t.SampleLevel(s, float2(-1, 0.5), 0); }
float4 vs(float2 p : POSITION) : SV_Position { return float4(p, 0, 1); }
float4 ps() : SV_Target { return border(); }
[numthreads(1, 1, 1)] void cs() { o[0] = border(); }
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

  // the pipelines first, before any sampler exists
  D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1}, {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1}};
  D3D12_ROOT_PARAMETER params[3] = {
      {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
      {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
      {D3D12_ROOT_PARAMETER_TYPE_UAV}
  };
  params[0].DescriptorTable = {1, &ranges[0]};
  params[1].DescriptorTable = {1, &ranges[1]};
  auto rs = root_signature(
      device.Get(), {3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT}
  );
  if (!rs) {
    printf("failed: root signature\n");
    return 1;
  }
  const DXGI_FORMAT format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  D3D12_INPUT_ELEMENT_DESC element{"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = rs.Get();
  desc.VS = bytecode(vs);
  desc.PS = bytecode(ps);
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.InputLayout = {&element, 1};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = format;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> graphics, compute;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&graphics)));
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc{rs.Get(), bytecode(cs)};
  CHECK(device->CreateComputePipelineState(&compute_desc, IID_PPV_ARGS(&compute)));

  const float color[4] = {0.25f, 0.5f, 0.75f, 0.125f};
  D3D12_DESCRIPTOR_HEAP_DESC sampler_desc{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE},
      view_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE},
      rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  ComPtr<ID3D12DescriptorHeap> samplers, views, rtvs;
  CHECK(device->CreateDescriptorHeap(&sampler_desc, IID_PPV_ARGS(&samplers)));
  CHECK(device->CreateDescriptorHeap(&view_desc, IID_PPV_ARGS(&views)));
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  D3D12_SAMPLER_DESC sampler{D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_BORDER,
                             D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_TEXTURE_ADDRESS_MODE_BORDER};
  std::copy_n(color, 4, sampler.BorderColor);
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  device->CreateSampler(&sampler, samplers->GetCPUDescriptorHandleForHeapStart());

  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC texture_desc{
      D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 1, 1, 1, 1, format, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
      D3D12_RESOURCE_FLAG_NONE
  };
  ComPtr<ID3D12Resource> texture, target;
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &texture_desc, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, nullptr,
      IID_PPV_ARGS(&texture)
  ));
  texture_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &texture_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  device->CreateShaderResourceView(texture.Get(), nullptr, views->GetCPUDescriptorHandleForHeapStart());
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  const float triangle[] = {-1, -1, -1, 3, 3, -1};
  auto vertices = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(triangle), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto out = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, 16, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 total;
  device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, footprint.Footprint.RowPitch + 16,
                         D3D12_RESOURCE_STATE_COPY_DEST);
  if (!vertices || !out || !readback) {
    printf("failed: buffers not created\n");
    return 1;
  }
  void *mapped;
  CHECK(vertices->Map(0, nullptr, &mapped));
  memcpy(mapped, triangle, sizeof(triangle));
  vertices->Unmap(0, nullptr);
  CHECK(forget(readback.Get()));

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), graphics.Get(), IID_PPV_ARGS(&list)));
  ID3D12DescriptorHeap *heaps[] = {views.Get(), samplers.Get()};
  list->SetDescriptorHeaps(2, heaps);
  list->SetGraphicsRootSignature(rs.Get());
  list->SetGraphicsRootDescriptorTable(0, views->GetGPUDescriptorHandleForHeapStart());
  list->SetGraphicsRootDescriptorTable(1, samplers->GetGPUDescriptorHandleForHeapStart());
  D3D12_VERTEX_BUFFER_VIEW vbv{vertices->GetGPUVirtualAddress(), sizeof(triangle), 2 * sizeof(float)};
  list->IASetVertexBuffers(0, 1, &vbv);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  D3D12_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
  D3D12_RECT scissor{0, 0, 1, 1};
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  list->DrawInstanced(3, 1, 0, 0);
  list->SetPipelineState(compute.Get());
  list->SetComputeRootSignature(rs.Get());
  list->SetComputeRootDescriptorTable(0, views->GetGPUDescriptorHandleForHeapStart());
  list->SetComputeRootDescriptorTable(1, samplers->GetGPUDescriptorHandleForHeapStart());
  list->SetComputeRootUnorderedAccessView(2, out->GetGPUVirtualAddress());
  list->Dispatch(1, 1, 1);
  transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}, src{
      target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX
  };
  dst.PlacedFootprint = footprint;
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  list->CopyBufferRegion(readback.Get(), footprint.Footprint.RowPitch, out.Get(), 0, 16);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  BYTE *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  unsigned failures = 0;
  const char *names[] = {"draw", "dispatch"};
  for (UINT r = 0; r < 2; r++) {
    auto value = (const float *)(got + r * footprint.Footprint.RowPitch);
    bool ok = true;
    for (UINT i = 0; i < 4; i++)
      ok &= value[i] == color[i];
    if (!ok && ++failures)
      printf("%s: %g %g %g %g, want %g %g %g %g\n", names[r], value[0], value[1], value[2], value[3], color[0], color[1],
             color[2], color[3]);
  }
  readback->Unmap(0, nullptr);
  printf("%s: %u of 2 pipelines made before the sampler miss its border\n", failures ? "failed" : "passed", failures);
  return failures != 0;
}
