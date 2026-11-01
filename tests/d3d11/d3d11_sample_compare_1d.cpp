// contract: SampleCmp and SampleCmpLevelZero on a Texture1D and a Texture1DArray compare the reference with the texel
// the coordinate selects, as on a 2D texture: sample_c and sample_c_lz take "Texture1D, Texture1DArray, Texture2D,
// Texture2DArray, TextureCube or TextureCubeArray" (D3D11.3 functional spec, 22.4.19 and 22.4.20), and with
// D3D11_COMPARISON_LESS a comparison gives 1 where the reference is less than the texel. a pixel shader samples
// R32_FLOAT textures of 4 texels with a point comparison sampler at each texel's center, a Texture1D holding
// {0.1, 0.4, 0.6, 0.9} and a 2-slice Texture1DArray holding those and {0.7, 0.2, 0.8, 0.3}, so each texel and slice
// gives a result of its own. each pixel starts at a value no comparison gives. d3d12_sample_compare_1d also compares
// against depth textures. the shader also reads the same views without a comparison, as a 1D view may be both
// compared and read: Load and Sample (a point sampler) of the Texture1D, Load and SampleLevel of slice 1 of the array
// give the texels themselves (22.4.10 ld, 22.4.15 sample), on the target's second row.
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#22.4.19%20sample_c
#include "d3d11_test.hpp"

static constexpr UINT width = 4;

static const char hlsl[] = R"hlsl(
Texture1D<float> plain : register(t0);
Texture1DArray<float> layers : register(t1);
SamplerComparisonState less : register(s0);
SamplerState nearest : register(s1);
float4 vs(uint id : SV_VertexID) : SV_Position {
  return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0, 1);
}
// row 0 the comparisons, row 1 the reads, all of them made on every row
float4 ps(float4 position : SV_Position) : SV_Target {
  float u = position.x / WIDTH;
  int x = int(position.x);
  float4 compared = float4(plain.SampleCmp(less, u, 0.5), plain.SampleCmpLevelZero(less, u, 0.5),
                           layers.SampleCmp(less, float2(u, 1), 0.5),
                           layers.SampleCmpLevelZero(less, float2(u, 0), 0.35));
  float4 read = float4(plain.Load(int2(x, 0)), plain.Sample(nearest, u), layers.Load(int3(x, 1, 0)),
                       layers.SampleLevel(nearest, float2(u, 1), 0));
  return position.y < 1 ? compared : read;
}
)hlsl";

int
main() {
  const std::vector<std::string> defines = {"WIDTH=" + std::to_string(width)};
  auto vs = compile(hlsl, "vs", "vs", defines), ps = compile(hlsl, "ps", "ps", defines);
  if (!expect(vs && ps, "HLSL did not compile"))
    return verdict();
  step("create the device, the shaders and the textures");
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                          &context));
  ComPtr<ID3D11VertexShader> vertex;
  ComPtr<ID3D11PixelShader> pixel;
  CHECK(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex));
  CHECK(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel));

  const float depths[2][width] = {{0.1f, 0.4f, 0.6f, 0.9f}, {0.7f, 0.2f, 0.8f, 0.3f}};
  const D3D11_SUBRESOURCE_DATA slices[2] = {{depths[0], sizeof(depths[0]), sizeof(depths[0])},
                                            {depths[1], sizeof(depths[1]), sizeof(depths[1])}};
  ComPtr<ID3D11Texture1D> textures[2];
  ComPtr<ID3D11ShaderResourceView> srvs[2];
  for (UINT t = 0; t < 2; t++) {
    D3D11_TEXTURE1D_DESC desc{width, 1, t + 1, DXGI_FORMAT_R32_FLOAT, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
    CHECK(device->CreateTexture1D(&desc, slices, &textures[t]));
    CHECK(device->CreateShaderResourceView(textures[t].Get(), nullptr, &srvs[t]));
  }
  D3D11_SAMPLER_DESC sampler_desc{D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP,
                                  D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP};
  sampler_desc.ComparisonFunc = D3D11_COMPARISON_LESS;
  sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
  ComPtr<ID3D11SamplerState> samplers[2];
  CHECK(device->CreateSamplerState(&sampler_desc, &samplers[0]));
  sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
  CHECK(device->CreateSamplerState(&sampler_desc, &samplers[1]));

  D3D11_TEXTURE2D_DESC target_desc{width, 2, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_RENDER_TARGET};
  ComPtr<ID3D11Texture2D> target, target_read;
  ComPtr<ID3D11RenderTargetView> rtv;
  CHECK(device->CreateTexture2D(&target_desc, nullptr, &target));
  CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
  target_desc.Usage = D3D11_USAGE_STAGING;
  target_desc.BindFlags = 0;
  target_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  CHECK(device->CreateTexture2D(&target_desc, nullptr, &target_read));

  step("draw");
  const float clear[4] = {0.25f, 0.25f, 0.25f, 0.25f};
  context->ClearRenderTargetView(rtv.Get(), clear);
  context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->VSSetShader(vertex.Get(), nullptr, 0);
  context->PSSetShader(pixel.Get(), nullptr, 0);
  ID3D11SamplerState *bound[] = {samplers[0].Get(), samplers[1].Get()};
  context->PSSetSamplers(0, 2, bound);
  ID3D11ShaderResourceView *views[] = {srvs[0].Get(), srvs[1].Get()};
  context->PSSetShaderResources(0, 2, views);
  D3D11_VIEWPORT viewport{0, 0, (float)width, 2, 0, 1};
  context->RSSetViewports(1, &viewport);
  context->Draw(3, 0);
  context->CopyResource(target_read.Get(), target.Get());

  step("read back");
  D3D11_MAPPED_SUBRESOURCE mapped;
  CHECK(context->Map(target_read.Get(), 0, D3D11_MAP_READ, 0, &mapped));
  auto got = reinterpret_cast<const float *>(mapped.pData);
  const char *what[4] = {"Texture1D SampleCmp, reference 0.5", "Texture1D SampleCmpLevelZero, reference 0.5",
                         "Texture1DArray slice 1 SampleCmp, reference 0.5",
                         "Texture1DArray slice 0 SampleCmpLevelZero, reference 0.35"};
  for (UINT x = 0; x < width; x++) {
    const float want[4] = {0.5f < depths[0][x] ? 1.f : 0.f, 0.5f < depths[0][x] ? 1.f : 0.f,
                           0.5f < depths[1][x] ? 1.f : 0.f, 0.35f < depths[0][x] ? 1.f : 0.f};
    for (UINT c = 0; c < 4; c++)
      expect(got[4 * x + c] == want[c], "%s at texel %u gives %g, want %g", what[c], x, got[4 * x + c], want[c]);
  }
  const char *read[4] = {"Texture1D Load", "Texture1D Sample", "Texture1DArray slice 1 Load",
                         "Texture1DArray slice 1 SampleLevel"};
  auto second = reinterpret_cast<const float *>((const uint8_t *)mapped.pData + mapped.RowPitch);
  for (UINT x = 0; x < width; x++) {
    const float want[4] = {depths[0][x], depths[0][x], depths[1][x], depths[1][x]};
    for (UINT c = 0; c < 4; c++)
      expect(second[4 * x + c] == want[c], "%s of the compared view at texel %u gives %g, want %g", read[c], x,
             second[4 * x + c], want[c]);
  }
  context->Unmap(target_read.Get(), 0);
  return verdict();
}
