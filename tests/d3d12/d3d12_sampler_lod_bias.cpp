// contract: d3d12.samplerLodBias (mullion.conf) adds its bias to every sampler's own, in a descriptor heap or static
// in a root signature. D3D11.3 7.18.11, which D3D12 keeps: "biasedLOD = output.LOD + MipLODBias" and "clampedLOD =
// max(MinLOD,(min(MaxLOD, biasedLOD)))", a bias within D3D12_MIP_LOD_BIAS_MIN and _MAX, where an isotropic filter's LOD
// is the log2 of the footprint's length. a pixel shader samples a 256x256 texture whose mip m holds m over an 8x8
// target, 32 texels to a pixel: LOD 5, which a linear mip filter reads as the value between the mips, through a heap's
// samplers and static samplers of a bias of 0 and of 0.5. the configured bias is the second argument (the test's
// environment sets the option, MULLION_CONFIG), 0 without it.
#include "d3d12_test.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>

static const char hlsl[] = R"hlsl(
Texture2D<float> image : register(t0);
SamplerState heap_unbiased : register(s0);
SamplerState heap_biased : register(s1);
SamplerState static_unbiased : register(s2);
SamplerState static_biased : register(s3);
struct Out { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Out vs(uint id : SV_VertexID) {
  Out o;
  o.uv = float2((id << 1) & 2, id & 2);
  o.position = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}
float4 ps(Out i) : SV_Target {
  return float4(image.Sample(heap_unbiased, i.uv), image.Sample(heap_biased, i.uv), image.Sample(static_unbiased, i.uv),
                image.Sample(static_biased, i.uv));
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: no DXIL compiler (dxcompiler.dll)\n");
    return 77;
  }
  const float configured = argc > 2 ? strtof(argv[2], nullptr) : 0;
  auto vs = compiler.compile(hlsl, "vs", "vs"), ps = compiler.compile(hlsl, "ps", "ps");
  if (!expect(!vs.empty() && !ps.empty(), "the HLSL does not compile"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));

  const float own[2] = {0, 0.5f};
  D3D12_DESCRIPTOR_RANGE ranges[2] = {
      {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 2, 0}
  };
  D3D12_ROOT_PARAMETER params[2]{};
  params[0].DescriptorTable = {1, &ranges[0]};
  params[1].DescriptorTable = {1, &ranges[1]};
  D3D12_STATIC_SAMPLER_DESC statics[2];
  for (UINT i = 0; i < 2; i++)
    statics[i] = {
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        own[i],
        1,
        D3D12_COMPARISON_FUNC_NEVER,
        D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE,
        0,
        D3D12_FLOAT32_MAX,
        2 + i,
        0,
        D3D12_SHADER_VISIBILITY_PIXEL
    };
  auto rs = root_signature(device.Get(), {2, params, 2, statics});
  if (!expect(rs, "the root signature"))
    return verdict();
  D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{rs.Get(), bytecode(vs), bytecode(ps)};
  graphics.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  graphics.SampleMask = ~0u;
  graphics.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  graphics.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  graphics.NumRenderTargets = 1;
  graphics.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
  graphics.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateGraphicsPipelineState(&graphics, IID_PPV_ARGS(&pso)));

  // mip m holds m
  const UINT side = 256, mips = 9, target_side = 8;
  D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC image_desc{
      D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, side, side, 1, mips, DXGI_FORMAT_R32_FLOAT, {1, 0}
  };
  ComPtr<ID3D12Resource> image, target;
  CHECK(device->CreateCommittedResource(
      &default_heap, D3D12_HEAP_FLAG_NONE, &image_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&image)
  ));
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[mips];
  UINT64 upload_size = 0;
  device->GetCopyableFootprints(&image_desc, 0, mips, 0, footprints, nullptr, nullptr, &upload_size);
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, upload_size, D3D12_RESOURCE_STATE_GENERIC_READ);
  char *bytes;
  CHECK(upload->Map(0, nullptr, (void **)&bytes));
  for (UINT m = 0; m < mips; m++)
    for (UINT y = 0; y < footprints[m].Footprint.Height; y++)
      std::fill_n(
          (float *)(bytes + footprints[m].Offset + y * footprints[m].Footprint.RowPitch), footprints[m].Footprint.Width,
          float(m)
      );
  upload->Unmap(0, nullptr);
  D3D12_RESOURCE_DESC target_desc{
      D3D12_RESOURCE_DIMENSION_TEXTURE2D,
      0,
      target_side,
      target_side,
      1,
      1,
      DXGI_FORMAT_R32G32B32A32_FLOAT,
      {1, 0},
      D3D12_TEXTURE_LAYOUT_UNKNOWN,
      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
  };
  CHECK(device->CreateCommittedResource(
      &default_heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
      IID_PPV_ARGS(&target)
  ));
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT target_footprint;
  UINT64 readback_size = 0;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &target_footprint, nullptr, nullptr, &readback_size);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, readback_size, D3D12_RESOURCE_STATE_COPY_DEST);
  CHECK(forget(readback.Get()));

  ComPtr<ID3D12DescriptorHeap> views, samplers, rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC views_desc{
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
  },
      samplers_desc{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE},
      rtvs_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&views_desc, IID_PPV_ARGS(&views)));
  CHECK(device->CreateDescriptorHeap(&samplers_desc, IID_PPV_ARGS(&samplers)));
  CHECK(device->CreateDescriptorHeap(&rtvs_desc, IID_PPV_ARGS(&rtvs)));
  device->CreateShaderResourceView(image.Get(), nullptr, views->GetCPUDescriptorHandleForHeapStart());
  device->CreateRenderTargetView(target.Get(), nullptr, rtvs->GetCPUDescriptorHandleForHeapStart());
  const UINT sampler_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  for (UINT i = 0; i < 2; i++) {
    D3D12_SAMPLER_DESC desc{
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        own[i],
        1,
        D3D12_COMPARISON_FUNC_NEVER,
        {},
        0,
        D3D12_FLOAT32_MAX
    };
    auto at = samplers->GetCPUDescriptorHandleForHeapStart();
    at.ptr += i * sampler_step;
    device->CreateSampler(&desc, at);
  }

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  for (UINT m = 0; m < mips; m++) {
    D3D12_TEXTURE_COPY_LOCATION to{image.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
        from{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    to.SubresourceIndex = m;
    from.PlacedFootprint = footprints[m];
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  }
  transition(list.Get(), image.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

  step("sampling with a configured bias of %g", configured);
  ID3D12DescriptorHeap *heaps[] = {views.Get(), samplers.Get()};
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  const D3D12_VIEWPORT viewport{0, 0, float(target_side), float(target_side), 0, 1};
  const D3D12_RECT scissor{0, 0, LONG(target_side), LONG(target_side)};
  list->SetDescriptorHeaps(2, heaps);
  list->SetGraphicsRootSignature(rs.Get());
  list->SetPipelineState(pso.Get());
  list->SetGraphicsRootDescriptorTable(0, views->GetGPUDescriptorHandleForHeapStart());
  list->SetGraphicsRootDescriptorTable(1, samplers->GetGPUDescriptorHandleForHeapStart());
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  list->DrawInstanced(3, 1, 0, 0);
  transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
      from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  to.PlacedFootprint = target_footprint;
  list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  const char *out;
  CHECK(readback->Map(0, nullptr, (void **)&out));
  const float lod = std::log2(float(side) / target_side);
  const char *names[4] = {"a heap's sampler", "a heap's sampler", "a static sampler", "a static sampler"};
  for (UINT y = 0; y < target_side; y++)
    for (UINT x = 0; x < target_side; x++) {
      auto got =
          reinterpret_cast<const float *>(out + target_footprint.Offset + y * target_footprint.Footprint.RowPitch) +
          4 * x;
      for (UINT i = 0; i < 4; i++) {
        float bias = own[i % 2];
        float want = std::clamp(lod + std::clamp(bias + configured, -16.0f, 15.99f), 0.0f, float(mips - 1));
        if (!expect(
                std::fabs(got[i] - want) <= 1.0f / 32, "pixel %u,%u through %s of bias %g reads mip %g, want %g", x, y,
                names[i], bias, got[i], want
            ) &&
            trace::wrong > 8)
          break;
      }
    }
  readback->Unmap(0, nullptr);
  return verdict();
}
