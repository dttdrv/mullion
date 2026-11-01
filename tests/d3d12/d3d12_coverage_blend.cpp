// contract: alpha-to-coverage takes its coverage from the pixel shader's own o0.a, before the output merger blends
// (D3D11.3 17.18: "Alpha-to-Coverage ... converts the alpha component of the output color from Pixel Shader output
// register o0 ... to a coverage mask"; an alpha of 0 or less covers no sample, and 1 or more all of them). blending
// comes after, so a blend whose alpha result differs from o0.a does not change which samples are written. a target
// of every sample count it supports, holding (0, 0, 0, `held`), takes (1, 1, 1, o0.a) with ONE/ZERO for color and
// ZERO/ONE for alpha, so a written sample has color 1 and the blended alpha is the target's own; o0.a of 0 over a
// target alpha of 1 writes no sample, and 1 over 0 writes all. R16G16B16A16_FLOAT goes above 1, which Mullion blends
// in the pixel shader; R8G8B8A8_UNORM is blended by Metal. each sample is read back through a Texture2DMS.
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
#include "d3d12_test.hpp"

static const char hlsl[] = R"(
cbuffer C : register(b0) { float alpha; };
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0.5, 1);
}
float4 ps() : SV_Target { return float4(1, 1, 1, alpha); }
Texture2DMS<float4> t : register(t0);
RWStructuredBuffer<float4> o : register(u0);
[numthreads(SAMPLES, 1, 1)] void cs(uint3 pixel : SV_GroupID, uint sample : SV_GroupIndex) {
  o[(pixel.y * WIDTH + pixel.x) * SAMPLES + sample] = t.Load(int2(pixel.xy), sample);
}
)";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  constexpr UINT width = 4, height = 4;
  auto vs = compiler.compile(hlsl, "vs", "vs", {"SAMPLES=1", "WIDTH=1"}),
       ps = compiler.compile(hlsl, "ps", "ps", {"SAMPLES=1", "WIDTH=1"});
  if (!expect(!vs.empty() && !ps.empty(), "the shaders do not compile"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1};
  D3D12_ROOT_PARAMETER params[3] = {
      {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}, {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
      {D3D12_ROOT_PARAMETER_TYPE_UAV}
  };
  params[0].Constants.Num32BitValues = 1;
  params[1].DescriptorTable = {1, &range};
  auto rs = root_signature(device.Get(), {UINT(std::size(params)), params});
  if (!expect(bool(rs), "the root signature is refused"))
    return verdict();

  ComPtr<ID3D12DescriptorHeap> rtvs, srvs;
  const D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1},
      srv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  CHECK(device->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&srvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();

  const DXGI_FORMAT formats[] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM};
  // the target's alpha, and o0.a
  const struct {
    float held, alpha;
  } cases[] = {{1, 0}, {0, 1}};
  for (UINT samples : {2u, 4u, 8u}) {
    const auto s = std::to_string(samples), w = std::to_string(width);
    auto cs = compiler.compile(hlsl, "cs", "cs", {"SAMPLES=" + s, "WIDTH=" + w});
    if (!expect(!cs.empty(), "the %u-sample read does not compile", samples))
      return verdict();
    D3D12_COMPUTE_PIPELINE_STATE_DESC read_desc{rs.Get(), bytecode(cs)};
    ComPtr<ID3D12PipelineState> read;
    CHECK(device->CreateComputePipelineState(&read_desc, IID_PPV_ARGS(&read)));
    const UINT64 bytes = UINT64(width) * height * samples * 4 * sizeof(float);
    for (auto format : formats) {
      D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS support{format, samples};
      if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &support, sizeof(support))) ||
          !support.NumQualityLevels) {
        printf("format %u has no %u-sample targets\n", format, samples);
        continue;
      }
      D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs), bytecode(ps)};
      desc.BlendState.AlphaToCoverageEnable = TRUE;
      desc.BlendState.RenderTarget[0] = {
          TRUE,
          FALSE,
          D3D12_BLEND_ONE,
          D3D12_BLEND_ZERO,
          D3D12_BLEND_OP_ADD,
          D3D12_BLEND_ZERO,
          D3D12_BLEND_ONE,
          D3D12_BLEND_OP_ADD,
          D3D12_LOGIC_OP_NOOP,
          D3D12_COLOR_WRITE_ENABLE_ALL
      };
      desc.SampleMask = UINT_MAX;
      desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
      desc.RasterizerState.DepthClipEnable = TRUE;
      desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      desc.NumRenderTargets = 1;
      desc.RTVFormats[0] = format;
      desc.SampleDesc = {samples, 0};
      ComPtr<ID3D12PipelineState> pipeline;
      CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)));
      for (auto &c : cases) {
        step("format %u, %u samples, target alpha %g, o0.a %g", format, samples, c.held, c.alpha);
        const D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, format,
                                              {samples, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                              D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
        ComPtr<ID3D12Resource> target;
        CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &target_desc,
                                              D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)));
        auto result = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!expect(result && readback, "the result buffers are refused"))
          return verdict();
        CHECK(forget(readback.Get()));
        device->CreateRenderTargetView(target.Get(), nullptr, rtv);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{format, D3D12_SRV_DIMENSION_TEXTURE2DMS,
                                            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
        device->CreateShaderResourceView(target.Get(), &srv, srvs->GetCPUDescriptorHandleForHeapStart());

        CHECK(allocator->Reset());
        CHECK(list->Reset(allocator.Get(), nullptr));
        const float held[4] = {0, 0, 0, c.held};
        list->ClearRenderTargetView(rtv, held, 0, nullptr);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const D3D12_VIEWPORT viewport{0, 0, width, height, 0, 1};
        const D3D12_RECT scissor{0, 0, width, height};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->SetGraphicsRootSignature(rs.Get());
        list->SetPipelineState(pipeline.Get());
        list->SetGraphicsRoot32BitConstants(0, 1, &c.alpha, 0);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
        transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ID3D12DescriptorHeap *heaps[] = {srvs.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(rs.Get());
        list->SetPipelineState(read.Get());
        list->SetComputeRootDescriptorTable(1, srvs->GetGPUDescriptorHandleForHeapStart());
        list->SetComputeRootUnorderedAccessView(2, result->GetGPUVirtualAddress());
        list->Dispatch(width, height, 1);
        transition(list.Get(), result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(readback.Get(), 0, result.Get(), 0, bytes);
        CHECK(submit(device.Get(), queue.Get(), list.Get()));

        float *got;
        CHECK(readback->Map(0, nullptr, (void **)&got));
        // a covered sample takes the shader's color and keeps the target's alpha; the others keep the clear
        const float want_color = c.alpha > 0 ? 1.0f : 0.0f;
        UINT wrong = 0;
        for (UINT i = 0; i < width * height * samples; i++) {
          const float *v = got + 4 * i;
          bool ok = v[0] == want_color && v[1] == want_color && v[2] == want_color && v[3] == c.held;
          if (!ok && !wrong++)
            printf("pixel %u, sample %u is (%g, %g, %g, %g), want (%g, %g, %g, %g)\n", i / samples, i % samples, v[0],
                   v[1], v[2], v[3], want_color, want_color, want_color, c.held);
        }
        readback->Unmap(0, nullptr);
        expect(!wrong, "%u wrong samples of %u", wrong, width * height * samples);
      }
    }
  }
  return verdict();
}
