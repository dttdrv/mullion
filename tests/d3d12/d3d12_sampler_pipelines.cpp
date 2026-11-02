// contract: what a sampler asks of a filter reaches every pipeline that samples through it, including pipelines made
// before that sampler: a pipeline state is fixed when it is made (ID3D12Device::CreateGraphicsPipelineState), and
// nothing in D3D12 orders it after the samplers it will read. a custom border color (D3D11.3 7.18.9.1) comes first, a
// minimum filter (D3D12_FILTER, D3D11.3 5.9.4.5.6: "Texels that are weighted 0 during filtering aren't counted") after
// the pipelines have drawn with it, each read by a draw and a dispatch. the minimum's footprint is the shared edge of
// a 2x1 {0, 1} texture, which averages to 0.5 and reduces to 0; its mip filter is a point, which Metal's own reduction
// never takes, so Mullion emulates it on every GPU
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
Texture2D<float> t : register(t0);
SamplerState border : register(s0);
SamplerState reduce : register(s1);
RWStructuredBuffer<float4> o : register(u0);
float4 read() { return float4(t.SampleLevel(border, float2(-1, 0.5), 0), t.SampleLevel(reduce, float2(0.5, 0.5), 0), 0, 1); }
float4 vs(float2 p : POSITION) : SV_Position { return float4(p, 0, 1); }
float4 ps() : SV_Target { return read(); }
[numthreads(1, 1, 1)] void cs() { o[0] = read(); }
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
  D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1}, {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 2}};
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
  const DXGI_FORMAT target_format = DXGI_FORMAT_R32G32B32A32_FLOAT;
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
  desc.RTVFormats[0] = target_format;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> graphics, compute;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&graphics)));
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc{rs.Get(), bytecode(cs)};
  CHECK(device->CreateComputePipelineState(&compute_desc, IID_PPV_ARGS(&compute)));

  D3D12_DESCRIPTOR_HEAP_DESC sampler_desc{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE},
      view_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE},
      rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  ComPtr<ID3D12DescriptorHeap> samplers, views, rtvs;
  CHECK(device->CreateDescriptorHeap(&sampler_desc, IID_PPV_ARGS(&samplers)));
  CHECK(device->CreateDescriptorHeap(&view_desc, IID_PPV_ARGS(&views)));
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  auto slot = [&](UINT i) {
    auto handle = samplers->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += i * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    return handle;
  };
  const float color[4] = {0.25f, 0.5f, 0.75f, 0.125f};
  D3D12_SAMPLER_DESC border{D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_BORDER,
                            D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_TEXTURE_ADDRESS_MODE_BORDER};
  std::copy_n(color, 4, border.BorderColor);
  border.MaxLOD = D3D12_FLOAT32_MAX;
  device->CreateSampler(&border, slot(0));
  D3D12_SAMPLER_DESC reduce{D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                            D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP};
  reduce.MaxLOD = D3D12_FLOAT32_MAX;
  device->CreateSampler(&reduce, slot(1));

  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC texture_desc{
      D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 2, 1, 1, 1, DXGI_FORMAT_R32_FLOAT, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
      D3D12_RESOURCE_FLAG_NONE
  };
  D3D12_RESOURCE_DESC target_desc{
      D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 1, 1, 1, 1, target_format, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
  };
  ComPtr<ID3D12Resource> texture, target;
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &texture_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)
  ));
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  device->CreateShaderResourceView(texture.Get(), nullptr, views->GetCPUDescriptorHandleForHeapStart());
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, nullptr);
  const float triangle[] = {-1, -1, -1, 3, 3, -1}, values[2] = {0, 1};
  auto vertices = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(triangle), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto out = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, 16, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, footprint.Footprint.RowPitch + 16,
                         D3D12_RESOURCE_STATE_COPY_DEST);
  // the texels' row in its own pitch-aligned upload
  auto row = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT,
                    D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!vertices || !row || !out || !readback) {
    printf("failed: buffers not created\n");
    return 1;
  }
  void *mapped;
  CHECK(vertices->Map(0, nullptr, &mapped));
  memcpy(mapped, triangle, sizeof(triangle));
  vertices->Unmap(0, nullptr);
  CHECK(row->Map(0, nullptr, &mapped));
  memcpy(mapped, values, sizeof(values));
  row->Unmap(0, nullptr);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  D3D12_TEXTURE_COPY_LOCATION texel_src{row.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
      texel_dst{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  texel_src.PlacedFootprint = {0, {DXGI_FORMAT_R32_FLOAT, 2, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT}};
  list->CopyTextureRegion(&texel_dst, 0, 0, 0, &texel_src, nullptr);
  transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  // the draw and the dispatch through both samplers, read back: the target's texel, then the dispatch's
  auto run = [&](float reduced) -> unsigned {
    CHECK(forget(readback.Get()));
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), graphics.Get()));
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
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    BYTE *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    unsigned failures = 0;
    const char *names[] = {"draw", "dispatch"};
    for (UINT r = 0; r < 2; r++) {
      auto value = (const float *)(got + r * footprint.Footprint.RowPitch);
      if ((value[0] != color[0] || value[1] != reduced) && ++failures)
        printf("%s: border %g, filtered %g, want %g and %g\n", names[r], value[0], value[1], color[0], reduced);
    }
    readback->Unmap(0, nullptr);
    return failures;
  };

  unsigned failures = run(0.5f);
  reduce.Filter = D3D12_FILTER_MINIMUM_MIN_MAG_LINEAR_MIP_POINT;
  device->CreateSampler(&reduce, slot(1));
  failures += run(0.0f);
  printf("%s: %u of 4 reads through pipelines made before their samplers are wrong\n", failures ? "failed" : "passed",
         failures);
  return failures != 0;
}
