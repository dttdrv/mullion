// contract: a feature level 11_0 device has 8x MSAA for every render target format but R32G32B32A32 (Microsoft Learn,
// ID3D11Device::CheckMultisampleQualityLevels remarks), whatever the GPU has. at 8 samples a pipeline and its targets
// are made, a shader sees 8 samples (GetRenderTargetSampleCount, Texture2DMS GetDimensions) and a fully covered
// pixel's SV_Coverage of all 8, the pipeline's SampleMask and the SV_Coverage output store as many samples as their
// bits (D3D11.3 17.17; which samples is implementation-defined, so only the count is read, of masks whose bits come
// in the pairs 2i, 2i + 1 that a GPU of 4 stores together), and a resolve is the samples' average (19.2.4).
#include "d3d12_test.hpp"
#include <bit>

static const char hlsl[] = R"hlsl(
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float ps_info(float4 pos : SV_Position, uint coverage : SV_Coverage) : SV_Target {
  return GetRenderTargetSampleCount() * 256 + coverage;
}
float ps_one(float4 pos : SV_Position) : SV_Target { return 1; }
#ifdef MASK
float ps_mask(float4 pos : SV_Position, out uint coverage : SV_Coverage) : SV_Target { coverage = MASK; return 1; }
#endif
Texture2DMS<float> ms : register(t0);
Texture2D<float> resolved : register(t1);
RWStructuredBuffer<float> result : register(u0);
[numthreads(1, 1, 1)] void cs(uint3 id : SV_DispatchThreadID) {
  uint w, h, n;
  ms.GetDimensions(w, h, n);
  uint at = id.x * 10;
  result[at] = n;
  for (uint s = 0; s < 8; s++)
    result[at + 1 + s] = ms.Load(int2(id.x, 0), s);
  result[at + 9] = resolved.Load(int3(id.x, 0, 0));
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler))
    return 77;
  const UINT samples = 8, width = 4, words = 10;
  const UINT masks[] = {0x0f, 0x33, 0xf0, 0x3c, 0xcc};
  auto vs = compiler.compile(hlsl, "vs", "vs"), info = compiler.compile(hlsl, "ps_info", "ps"),
       one = compiler.compile(hlsl, "ps_one", "ps"), cs = compiler.compile(hlsl, "cs", "cs");
  std::string mask_ps[std::size(masks)];
  for (UINT i = 0; i < std::size(masks); i++)
    mask_ps[i] = compiler.compile(hlsl, "ps_mask", "ps", {"MASK=" + std::to_string(masks[i])});
  if (!expect(!vs.empty() && !info.empty() && !one.empty() && !cs.empty(), "HLSL did not compile"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  for (auto format : {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_D32_FLOAT}) {
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS support{format, samples};
    HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &support, sizeof(support));
    expect(hr == S_OK && support.NumQualityLevels, "format %u at 8 samples: %08lx, %u quality levels", format, hr,
           support.NumQualityLevels);
  }

  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2};
  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[0].DescriptorTable = {1, &range};
  auto rs = root_signature(device.Get(), {UINT(std::size(params)), params});
  if (!expect(bool(rs), "root signature missing"))
    return verdict();
  ComPtr<ID3D12PipelineState> compute;
  D3D12_COMPUTE_PIPELINE_STATE_DESC cp{};
  cp.pRootSignature = rs.Get();
  cp.CS = bytecode(cs);
  CHECK(device->CreateComputePipelineState(&cp, IID_PPV_ARGS(&compute)));

  // what each draw stores: the sample count and coverage, the samples the pipeline's mask keeps, or the samples the
  // shader's coverage output keeps
  struct Case {
    const std::string *ps;
    UINT sample_mask;
    UINT kept;
    float value;
  };
  std::vector<Case> cases{{&info, ~0u, samples, float(samples * 256 + 0xff)}};
  for (auto mask : masks)
    cases.push_back({&one, mask, UINT(std::popcount(mask)), 1});
  for (UINT i = 0; i < std::size(masks); i++)
    cases.push_back({&mask_ps[i], ~0u, UINT(std::popcount(masks[i])), 1});

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  const UINT64 bytes = width * words * sizeof(float);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(bool(readback), "readback missing"))
    return verdict();

  for (UINT c = 0; c < cases.size(); c++) {
    auto &k = cases[c];
    step("case %u, sample mask %#x, %u samples kept", c, k.sample_mask, k.kept);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature = rs.Get();
    p.VS = bytecode(vs);
    p.PS = bytecode(*k.ps);
    p.NumRenderTargets = 1;
    p.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
    p.SampleDesc = {samples, 0};
    p.SampleMask = k.sample_mask;
    p.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    p.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    p.RasterizerState.DepthClipEnable = p.RasterizerState.MultisampleEnable = TRUE;
    p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    ComPtr<ID3D12PipelineState> pipeline;
    HRESULT created = device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&pipeline));
    if (!expect(created == S_OK, "8-sample pipeline returned %08lx", created))
      return verdict();

    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, 1, 1, 1, DXGI_FORMAT_R32_FLOAT, {samples, 0},
                           D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    ComPtr<ID3D12Resource> target, single;
    HRESULT made = device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
    );
    if (!expect(made == S_OK, "8-sample target returned %08lx", made))
      return verdict();
    expect(target->GetDesc().SampleDesc.Count == samples, "target describes %u samples",
           target->GetDesc().SampleDesc.Count);
    td.SampleDesc = {1, 0};
    td.Flags = D3D12_RESOURCE_FLAG_NONE;
    CHECK(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr, IID_PPV_ARGS(&single)
    ));
    auto result = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ComPtr<ID3D12DescriptorHeap> rtvs, srvs;
    D3D12_DESCRIPTOR_HEAP_DESC rh{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1},
        sh{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    CHECK(device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&rtvs)));
    CHECK(device->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&srvs)));
    auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.Get(), nullptr, rtv);
    auto srv = srvs->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R32_FLOAT;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    device->CreateShaderResourceView(target.Get(), &sd, srv);
    srv.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(single.Get(), &sd, srv);

    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), nullptr));
    CHECK(forget(readback.Get()));
    const float clear[4]{};
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT viewport{0, 0, float(width), 1, 0, 1};
    D3D12_RECT scissor{0, 0, LONG(width), 1};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->SetPipelineState(pipeline.Get());
    list->SetGraphicsRootSignature(rs.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    list->ResolveSubresource(single.Get(), 0, target.Get(), 0, DXGI_FORMAT_R32_FLOAT);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(list.Get(), single.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->SetPipelineState(compute.Get());
    list->SetComputeRootSignature(rs.Get());
    ID3D12DescriptorHeap *heaps[] = {srvs.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootDescriptorTable(0, srvs->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRootUnorderedAccessView(1, result->GetGPUVirtualAddress());
    list->Dispatch(width, 1, 1);
    transition(list.Get(), result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), 0, result.Get(), 0, bytes);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));

    const float *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    for (UINT x = 0; x < width; x++) {
      auto pixel = got + x * words;
      expect(pixel[0] == samples, "pixel %u: Texture2DMS has %g samples", x, pixel[0]);
      UINT kept = 0;
      float sum = 0;
      for (UINT s = 0; s < samples; s++) {
        float v = pixel[1 + s];
        expect(v == 0 || v == k.value, "pixel %u sample %u is %g, want 0 or %g", x, s, v, k.value);
        kept += v == k.value;
        sum += v;
      }
      expect(kept == k.kept, "pixel %u keeps %u samples, want %u", x, kept, k.kept);
      expect(pixel[9] == sum / samples, "pixel %u resolves to %g, the samples' average is %g", x, pixel[9],
             sum / samples);
    }
    readback->Unmap(0, nullptr);
  }
  return verdict();
}
