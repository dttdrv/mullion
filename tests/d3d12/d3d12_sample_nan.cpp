// contract: a sample whose coordinate is NaN reads the texel coordinate 0 selects. "Texture coordinates for sampling
// operations are snapped to fixed point" (D3D11.3 functional spec, 3.2.4), and of that conversion "if n is a NaN,
// result = 0" (3.2.4.1 FLOAT -> Fixed Point Integer), so a NaN component addresses the first texel of its axis under
// every address mode, while the other component still selects its own.
// a pixel shader samples a 4x4 texture whose texel (x, y) holds 1 + x + 4y with point samplers, one that clamps and
// one that wraps, through Sample and SampleLevel, with NaN in u, in v and in both. the NaN comes from a root constant,
// so no compiler folds it, and the finite coordinate is the center of texel 2. each pixel starts at a value no texel
// holds.
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#3.2.4.1%20FLOAT%20-%3E%20Fixed%20Point%20Integer
#include "d3d12_test.hpp"

static constexpr UINT side = 4, cases = 6;

static const char hlsl[] = R"hlsl(
Texture2D<float> values : register(t0);
SamplerState clamps : register(s0);
SamplerState wraps : register(s1);
cbuffer constants : register(b0) { uint nan_bits; };
float4 vs(uint id : SV_VertexID) : SV_Position {
  return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0, 1);
}
// pixels 0 to 2 sample with an implicit level, 3 to 5 with level 0: NaN in u, in v, in both
float4 ps(float4 position : SV_Position) : SV_Target {
  float nan = asfloat(nan_bits);
  float center = 2.5 / SIDE;
  uint kind = uint(position.x) % 3;
  float2 at = float2(kind == 1 ? center : nan, kind == 0 ? center : nan);
  if (position.x < 3)
    return float4(values.Sample(clamps, at), values.Sample(wraps, at), 0, 1);
  return float4(values.SampleLevel(clamps, at, 0), values.SampleLevel(wraps, at, 0), 0, 1);
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const std::vector<std::string> defines = {"SIDE=" + std::to_string(side)};
  auto vs = compiler.compile(hlsl, "vs", "vs", defines), ps = compiler.compile(hlsl, "ps", "ps", defines);
  if (!expect(!vs.empty() && !ps.empty(), "HLSL did not compile"))
    return verdict();

  step("create the device and the pipeline");
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
  D3D12_ROOT_PARAMETER parameters[2] = {{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
                                        {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}};
  parameters[0].DescriptorTable = {1, &range};
  parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  parameters[1].Constants = {0, 0, 1};
  parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_STATIC_SAMPLER_DESC sampler{D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                    D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP};
  sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_STATIC_SAMPLER_DESC samplers[2] = {sampler, sampler};
  samplers[1].AddressU = samplers[1].AddressV = samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  samplers[1].ShaderRegister = 1;
  auto rs = root_signature(device.Get(), {2, parameters, 2, samplers});
  CHECK(rs ? S_OK : E_FAIL);
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs), bytecode(ps)};
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> pso;
  HRESULT created = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
  if (!expect(SUCCEEDED(created), "CreateGraphicsPipelineState returned %#lx", created))
    return verdict();

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, cases, 1, 1, 1,
                                  DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                  D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT target_footprint;
  UINT64 target_bytes;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &target_footprint, nullptr, nullptr, &target_bytes);

  step("upload the texels");
  D3D12_RESOURCE_DESC texture_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, side, side, 1, 1, DXGI_FORMAT_R32_FLOAT,
                                   {1, 0}};
  ComPtr<ID3D12Resource> texture;
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &texture_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)
  ));
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 bytes;
  device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  uint8_t *mapped;
  CHECK(upload->Map(0, nullptr, (void **)&mapped));
  auto texel = [](UINT x, UINT y) { return float(1 + x + side * y); };
  for (UINT y = 0; y < side; y++)
    for (UINT x = 0; x < side; x++) {
      float value = texel(x, y);
      memcpy(mapped + footprint.Offset + y * footprint.Footprint.RowPitch + x * sizeof(float), &value, sizeof(value));
    }
  upload->Unmap(0, nullptr);
  ComPtr<ID3D12DescriptorHeap> views;
  D3D12_DESCRIPTOR_HEAP_DESC views_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1,
                                        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&views_desc, IID_PPV_ARGS(&views)));
  device->CreateShaderResourceView(texture.Get(), nullptr, views->GetCPUDescriptorHandleForHeapStart());
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, target_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  CHECK(forget(readback.Get()));

  step("draw");
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  D3D12_TEXTURE_COPY_LOCATION from{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
      to{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  from.PlacedFootprint = footprint;
  list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  const float clear[4] = {0.25f, 0.25f, 0.25f, 0.25f};
  list->ClearRenderTargetView(rtv, clear, 0, nullptr);
  ID3D12DescriptorHeap *heaps[] = {views.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetGraphicsRootSignature(rs.Get());
  list->SetGraphicsRootDescriptorTable(0, views->GetGPUDescriptorHandleForHeapStart());
  // a quiet NaN
  list->SetGraphicsRoot32BitConstant(1, 0x7fc00000u, 0);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  const D3D12_VIEWPORT viewport{0, 0, cases, 1, 0, 1};
  const D3D12_RECT scissor{0, 0, cases, 1};
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  list->DrawInstanced(3, 1, 0, 0);
  transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION placed{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
      texture_location{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  placed.PlacedFootprint = target_footprint;
  list->CopyTextureRegion(&placed, 0, 0, 0, &texture_location, nullptr);
  HRESULT hr = submit(device.Get(), queue.Get(), list.Get());
  if (!expect(SUCCEEDED(hr), "the draw failed: %#lx", hr))
    return verdict();

  step("read back");
  float *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  const char *calls[2] = {"Sample", "SampleLevel"}, *modes[2] = {"clamp", "wrap"},
             *where[3] = {"NaN in u", "NaN in v", "NaN in u and v"};
  // the NaN axis reads texel 0, the other the texel its center names
  const float want[3] = {texel(0, 2), texel(2, 0), texel(0, 0)};
  for (UINT pixel = 0; pixel < cases; pixel++)
    for (UINT mode = 0; mode < 2; mode++)
      expect(got[4 * pixel + mode] == want[pixel % 3], "%s, %s, %s gives %g, want texel value %g", calls[pixel / 3],
             modes[mode], where[pixel % 3], got[4 * pixel + mode], want[pixel % 3]);
  readback->Unmap(0, nullptr);
  return verdict();
}
