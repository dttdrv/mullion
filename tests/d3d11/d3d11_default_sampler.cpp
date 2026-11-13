// contract: a sampler slot with nothing bound samples with Direct3D's default state. "Any sampler may be set to NULL;
// this invokes the default state": Filter D3D11_FILTER_MIN_MAG_MIP_LINEAR, AddressU, V and W
// D3D11_TEXTURE_ADDRESS_CLAMP, MipLODBias 0, MinLOD -FLT_MAX, MaxLOD FLT_MAX (ID3D11DeviceContext::PSSetSamplers,
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-pssetsamplers).
// a 2x2 texture has the texels 0 and 1 in each row and a second mip of 5. a pixel shader and a compute shader sample
// it with no sampler, and with a sampler object of the same state as a control:
// - at the middle, mip 0: 0.5, which only a linear filter gives (a point filter gives 0 or 1).
// - at u = 0.25 and 0.75: the texels themselves, 0 and 1.
// - at u = 2.25 and -1.75: 1 and 0, the edge texels, where wrapping and mirroring give the other one.
// - at mip 1: 5, and at mip 0.5: 2.75, halfway between the mips' 0.5 and 5, which only a linear mip filter gives.
#include "d3d11_test.hpp"
#include <cfloat>
#include <cmath>

static const char hlsl[] = R"hlsl(
Texture2D<float> image : register(t0);
SamplerState state : register(s0);
cbuffer Cases : register(b0) { float4 cases[8]; };
RWStructuredBuffer<float> results : register(u0);
float4 vs(uint id : SV_VertexID) : SV_Position {
  return float4(float2((id << 1) & 2, id & 2) * float2(2, -2) + float2(-1, 1), 0, 1);
}
float ps(float4 position : SV_Position) : SV_Target {
  float4 at = cases[uint(position.x)];
  return image.SampleLevel(state, at.xy, at.z);
}
[numthreads(1, 1, 1)] void cs(uint3 id : SV_DispatchThreadID) {
  float4 at = cases[id.x];
  results[id.x] = image.SampleLevel(state, at.xy, at.z);
}
)hlsl";

int
main() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context));
  auto vs_code = compile(hlsl, "vs", "vs"), ps_code = compile(hlsl, "ps", "ps"), cs_code = compile(hlsl, "cs", "cs");
  if (!vs_code || !ps_code || !cs_code) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  ComPtr<ID3D11ComputeShader> cs;
  CHECK(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs));
  CHECK(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &ps));
  CHECK(device->CreateComputeShader(cs_code->GetBufferPointer(), cs_code->GetBufferSize(), nullptr, &cs));

  // u, v, mip and the value the default state reads there
  struct Case {
    float u, v, mip, want;
  };
  const Case cases[8] = {{0.5f, 0.5f, 0, 0.5f},   {0.25f, 0.5f, 0, 0},  {0.75f, 0.5f, 0, 1},    {2.25f, 0.5f, 0, 1},
                         {-1.75f, 0.5f, 0, 0},    {0.5f, 0.5f, 1, 5},   {0.5f, 0.5f, 0.5f, 2.75f}, {0.25f, 3.5f, 0, 0}};
  const UINT count = UINT(std::size(cases));
  float words[8][4];
  for (UINT i = 0; i < count; i++)
    words[i][0] = cases[i].u, words[i][1] = cases[i].v, words[i][2] = cases[i].mip, words[i][3] = 0;
  D3D11_BUFFER_DESC cb_desc{sizeof(words), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER};
  D3D11_SUBRESOURCE_DATA cb_data{words};
  ComPtr<ID3D11Buffer> cb;
  CHECK(device->CreateBuffer(&cb_desc, &cb_data, &cb));

  const float mip0[4] = {0, 1, 0, 1}, mip1[1] = {5};
  D3D11_TEXTURE2D_DESC image_desc{2, 2, 2, 1, DXGI_FORMAT_R32_FLOAT, {1, 0}, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE};
  D3D11_SUBRESOURCE_DATA image_data[2] = {{mip0, 2 * sizeof(float)}, {mip1, sizeof(float)}};
  ComPtr<ID3D11Texture2D> image, target, staging;
  ComPtr<ID3D11ShaderResourceView> image_view;
  CHECK(device->CreateTexture2D(&image_desc, image_data, &image));
  CHECK(device->CreateShaderResourceView(image.Get(), nullptr, &image_view));
  D3D11_TEXTURE2D_DESC target_desc{count, 1, 1, 1, DXGI_FORMAT_R32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
  ComPtr<ID3D11RenderTargetView> target_view;
  CHECK(device->CreateTexture2D(&target_desc, nullptr, &target));
  CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &target_view));
  target_desc.Usage = D3D11_USAGE_STAGING;
  target_desc.BindFlags = 0;
  target_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  CHECK(device->CreateTexture2D(&target_desc, nullptr, &staging));
  D3D11_BUFFER_DESC results_desc{count * sizeof(float), D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                                 D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, sizeof(float)};
  ComPtr<ID3D11Buffer> results, results_staging;
  ComPtr<ID3D11UnorderedAccessView> results_view;
  CHECK(device->CreateBuffer(&results_desc, nullptr, &results));
  CHECK(device->CreateUnorderedAccessView(results.Get(), nullptr, &results_view));
  results_desc = {count * sizeof(float), D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
  CHECK(device->CreateBuffer(&results_desc, nullptr, &results_staging));
  // the default state as an object, to tell a wrong default from a wrong sampler
  D3D11_SAMPLER_DESC same{D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
                          D3D11_TEXTURE_ADDRESS_CLAMP, 0, 1, D3D11_COMPARISON_NEVER, {1, 1, 1, 1}, -FLT_MAX, FLT_MAX};
  ComPtr<ID3D11SamplerState> object;
  CHECK(device->CreateSamplerState(&same, &object));

  const D3D11_VIEWPORT viewport{0, 0, float(count), 1, 0, 1};
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->RSSetViewports(1, &viewport);
  context->VSSetShader(vs.Get(), nullptr, 0);
  context->PSSetShader(ps.Get(), nullptr, 0);
  context->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
  context->PSSetShaderResources(0, 1, image_view.GetAddressOf());
  context->CSSetShader(cs.Get(), nullptr, 0);
  context->CSSetConstantBuffers(0, 1, cb.GetAddressOf());
  context->CSSetShaderResources(0, 1, image_view.GetAddressOf());

  auto check = [&](const float *got, const char *stage, const char *sampler) {
    for (UINT i = 0; i < count; i++)
      expect(std::fabs(got[i] - cases[i].want) <= 1.0f / 128, "%s shader, %s: (%g, %g) at mip %g reads %g, want %g", stage,
             sampler, cases[i].u, cases[i].v, cases[i].mip, got[i], cases[i].want);
  };
  for (bool bound : {true, false}) {
    const char *sampler = bound ? "a sampler of the default state" : "no sampler";
    step("sampling with %s", sampler);
    ID3D11SamplerState *slot = bound ? object.Get() : nullptr;
    context->PSSetSamplers(0, 1, &slot);
    context->CSSetSamplers(0, 1, &slot);
    const float unwritten[4] = {-1, -1, -1, -1};
    context->ClearRenderTargetView(target_view.Get(), unwritten);
    context->OMSetRenderTargets(1, target_view.GetAddressOf(), nullptr);
    context->Draw(3, 0);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    context->CopyResource(staging.Get(), target.Get());
    D3D11_MAPPED_SUBRESOURCE map;
    CHECK(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map));
    check(static_cast<const float *>(map.pData), "pixel", sampler);
    context->Unmap(staging.Get(), 0);

    const UINT keep = 0;
    context->CSSetUnorderedAccessViews(0, 1, results_view.GetAddressOf(), &keep);
    context->Dispatch(count, 1, 1);
    ID3D11UnorderedAccessView *none = nullptr;
    context->CSSetUnorderedAccessViews(0, 1, &none, &keep);
    context->CopyResource(results_staging.Get(), results.Get());
    CHECK(context->Map(results_staging.Get(), 0, D3D11_MAP_READ, 0, &map));
    check(static_cast<const float *>(map.pData), "compute", sampler);
    context->Unmap(results_staging.Get(), 0);
  }
  return verdict();
}
