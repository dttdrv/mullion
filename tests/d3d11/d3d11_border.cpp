// contract: D3D11 (and D3D10 through it) custom border colors are sampled, not replaced with opaque white
// (Microsoft Learn, D3D11_SAMPLER_DESC::BorderColor). D3D12 already has this path (Sampler::createSampler).
#include "d3d11_test.hpp"

static const char hlsl[] = R"hlsl(
Texture2D<float4> tex : register(t0);
SamplerState samp : register(s0);
float4 vs(uint id : SV_VertexID) : SV_Position {
  return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0, 1);
}
float4 ps() : SV_Target { return tex.SampleLevel(samp, float2(-1, -1), 0); }
)hlsl";

int
main() {
  auto vs = compile(hlsl, "vs", "vs"), ps = compile(hlsl, "ps", "ps");
  if (!vs || !ps) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                          &context));
  ComPtr<ID3D11VertexShader> vertex;
  ComPtr<ID3D11PixelShader> pixel;
  CHECK(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex));
  CHECK(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel));

  const float texel[4] = {0, 1, 0, 1};
  D3D11_SUBRESOURCE_DATA data{texel, sizeof(texel), 0};
  D3D11_TEXTURE2D_DESC tex_desc{1, 1, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_SHADER_RESOURCE};
  ComPtr<ID3D11Texture2D> texture;
  CHECK(device->CreateTexture2D(&tex_desc, &data, &texture));
  ComPtr<ID3D11ShaderResourceView> srv;
  CHECK(device->CreateShaderResourceView(texture.Get(), nullptr, &srv));

  D3D11_SAMPLER_DESC samp_desc{};
  samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  samp_desc.AddressU = samp_desc.AddressV = samp_desc.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
  samp_desc.BorderColor[0] = 1;
  samp_desc.BorderColor[1] = 0;
  samp_desc.BorderColor[2] = 1;
  samp_desc.BorderColor[3] = 1;
  samp_desc.MaxLOD = D3D11_FLOAT32_MAX;
  ComPtr<ID3D11SamplerState> sampler;
  CHECK(device->CreateSamplerState(&samp_desc, &sampler));

  D3D11_TEXTURE2D_DESC rt_desc{1, 1, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_RENDER_TARGET};
  ComPtr<ID3D11Texture2D> rt, staging;
  CHECK(device->CreateTexture2D(&rt_desc, nullptr, &rt));
  D3D11_TEXTURE2D_DESC staging_desc = rt_desc;
  staging_desc.Usage = D3D11_USAGE_STAGING;
  staging_desc.BindFlags = 0;
  staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  CHECK(device->CreateTexture2D(&staging_desc, nullptr, &staging));
  ComPtr<ID3D11RenderTargetView> rtv;
  CHECK(device->CreateRenderTargetView(rt.Get(), nullptr, &rtv));

  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->VSSetShader(vertex.Get(), nullptr, 0);
  context->PSSetShader(pixel.Get(), nullptr, 0);
  context->PSSetShaderResources(0, 1, srv.GetAddressOf());
  context->PSSetSamplers(0, 1, sampler.GetAddressOf());
  context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
  D3D11_VIEWPORT vp{0, 0, 1, 1, 0, 1};
  context->RSSetViewports(1, &vp);
  context->Draw(3, 0);
  context->CopyResource(staging.Get(), rt.Get());
  D3D11_MAPPED_SUBRESOURCE mapped{};
  CHECK(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
  const float got[3] = {((const float *)mapped.pData)[0], ((const float *)mapped.pData)[1],
                        ((const float *)mapped.pData)[2]};
  context->Unmap(staging.Get(), 0);
  expect(got[0] > 0.9f && got[1] < 0.1f && got[2] > 0.9f, "custom border was %f %f %f, want magenta", got[0], got[1],
         got[2]);
  return verdict();
}
