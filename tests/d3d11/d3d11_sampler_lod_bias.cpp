// contract: d3d11.samplerLodBias (mullion.conf) adds its bias to every
// sampler's own. D3D11.3 7.18.11: "biasedLOD = output.LOD + MipLODBias" and
// "clampedLOD = max(MinLOD,(min(MaxLOD, biasedLOD)))", a bias within
// D3D11_MIP_LOD_BIAS_MIN and _MAX (-16 and 15.99), where an isotropic filter's
// LOD is the log2 of the footprint's length. a pixel shader samples a 256x256
// texture whose mip m holds m over an 8x8 target, 32 texels to a pixel: LOD 5,
// which a linear mip filter reads as the value between the mips, through
// samplers of a bias of 0 and of 0.5. the configured bias is the first argument
// (the test's environment sets the option, MULLION_CONFIG), 0 without it; a
// sampler's description is the application's whatever the option.
#include "d3d11_test.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>

static const char hlsl[] = R"hlsl(
Texture2D<float> image : register(t0);
SamplerState unbiased : register(s0);
SamplerState biased : register(s1);
struct Out { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Out vs(uint id : SV_VertexID) {
  Out o;
  o.uv = float2((id << 1) & 2, id & 2);
  o.position = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}
float2 ps(Out i) : SV_Target {
  return float2(image.Sample(unbiased, i.uv), image.Sample(biased, i.uv));
}
)hlsl";

int main(int argc, char **argv) {
  const float configured = argc > 1 ? strtof(argv[1], nullptr) : 0;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level,
                          1, D3D11_SDK_VERSION, &device, nullptr, &context));
  auto vs_code = compile(hlsl, "vs", "vs"), ps_code = compile(hlsl, "ps", "ps");
  if (!vs_code || !ps_code) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  CHECK(device->CreateVertexShader(vs_code->GetBufferPointer(),
                                   vs_code->GetBufferSize(), nullptr, &vs));
  CHECK(device->CreatePixelShader(ps_code->GetBufferPointer(),
                                  ps_code->GetBufferSize(), nullptr, &ps));

  // mip m holds m
  const UINT side = 256, mips = 9, target_side = 8;
  std::vector<std::vector<float>> levels(mips);
  D3D11_SUBRESOURCE_DATA data[mips];
  for (UINT m = 0; m < mips; m++) {
    UINT width = side >> m;
    levels[m].assign(width * width, float(m));
    data[m] = {levels[m].data(), UINT(width * sizeof(float))};
  }
  D3D11_TEXTURE2D_DESC image_desc{side,
                                  side,
                                  mips,
                                  1,
                                  DXGI_FORMAT_R32_FLOAT,
                                  {1, 0},
                                  D3D11_USAGE_IMMUTABLE,
                                  D3D11_BIND_SHADER_RESOURCE};
  ComPtr<ID3D11Texture2D> image, target, staging;
  ComPtr<ID3D11ShaderResourceView> view;
  CHECK(device->CreateTexture2D(&image_desc, data, &image));
  CHECK(device->CreateShaderResourceView(image.Get(), nullptr, &view));
  D3D11_TEXTURE2D_DESC target_desc{target_side,
                                   target_side,
                                   1,
                                   1,
                                   DXGI_FORMAT_R32G32_FLOAT,
                                   {1, 0},
                                   D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_RENDER_TARGET};
  ComPtr<ID3D11RenderTargetView> target_view;
  CHECK(device->CreateTexture2D(&target_desc, nullptr, &target));
  CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &target_view));
  target_desc.Usage = D3D11_USAGE_STAGING;
  target_desc.BindFlags = 0;
  target_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  CHECK(device->CreateTexture2D(&target_desc, nullptr, &staging));

  const float own[2] = {0, 0.5f};
  ComPtr<ID3D11SamplerState> samplers[2];
  for (UINT i = 0; i < 2; i++) {
    D3D11_SAMPLER_DESC desc{D3D11_FILTER_MIN_MAG_MIP_LINEAR,
                            D3D11_TEXTURE_ADDRESS_WRAP,
                            D3D11_TEXTURE_ADDRESS_WRAP,
                            D3D11_TEXTURE_ADDRESS_WRAP,
                            own[i],
                            1,
                            D3D11_COMPARISON_NEVER,
                            {},
                            0,
                            D3D11_FLOAT32_MAX};
    CHECK(device->CreateSamplerState(&desc, &samplers[i]));
    D3D11_SAMPLER_DESC given{};
    samplers[i]->GetDesc(&given);
    expect(given.MipLODBias == own[i],
           "the description of a sampler of bias %g has %g", own[i],
           given.MipLODBias);
  }

  step("sampling with a configured bias of %g", configured);
  const D3D11_VIEWPORT viewport{0, 0, float(target_side), float(target_side),
                                0, 1};
  ID3D11SamplerState *slots[] = {samplers[0].Get(), samplers[1].Get()};
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->RSSetViewports(1, &viewport);
  context->VSSetShader(vs.Get(), nullptr, 0);
  context->PSSetShader(ps.Get(), nullptr, 0);
  context->PSSetShaderResources(0, 1, view.GetAddressOf());
  context->PSSetSamplers(0, 2, slots);
  context->OMSetRenderTargets(1, target_view.GetAddressOf(), nullptr);
  context->Draw(3, 0);
  context->OMSetRenderTargets(0, nullptr, nullptr);
  context->CopyResource(staging.Get(), target.Get());
  D3D11_MAPPED_SUBRESOURCE map;
  CHECK(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map));
  const float lod = std::log2(float(side) / target_side);
  for (UINT y = 0; y < target_side; y++)
    for (UINT x = 0; x < target_side; x++) {
      auto got = reinterpret_cast<const float *>(
                     static_cast<const char *>(map.pData) + y * map.RowPitch) +
                 2 * x;
      for (UINT i = 0; i < 2; i++) {
        float want =
            std::clamp(lod + std::clamp(own[i] + configured, -16.0f, 15.99f),
                       0.0f, float(mips - 1));
        if (!expect(std::fabs(got[i] - want) <= 1.0f / 32,
                    "pixel %u,%u through the sampler of bias %g reads mip %g, "
                    "want %g",
                    x, y, own[i], got[i], want) &&
            trace::wrong > 8)
          break;
      }
    }
  context->Unmap(staging.Get(), 0);
  return verdict();
}
