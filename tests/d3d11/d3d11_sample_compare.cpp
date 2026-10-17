// contract: a comparison sample compares the reference with the texel and returns 1 where the comparison passes and 0
// where it fails (D3D11.3 7.18.11); a comparison gather returns the results of the four texels it gathers, at an offset
// given at run time (D3D11.3 22.4.6, gather4_po_c). two textures are read through SampleCmp, which only a pixel shader
// has (sample_c), and GatherCmp with a non-constant offset of one texel, with LESS_EQUAL and a reference of 0.125,
// 0.375, 0.625 and 0.875 across the target: a 4x4 depth texture cleared to 0.5, and a 4x4 R32_FLOAT texture whose
// columns are 0.1, 0.3, 0.6 and 0.9, which D3D11 also compares (it is not a depth format in Metal).
#include "d3d11_test.hpp"

static const char hlsl[] = R"hlsl(
cbuffer C : register(b0) { int offset; };
Texture2D<float> depth : register(t0);
SamplerComparisonState compare : register(s0);
float4 vs(uint id : SV_VertexID) : SV_Position {
  return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0, 1);
}
// red: SampleCmp times 100; green: the GatherCmp results summed
uint2 ps(float4 pos : SV_Position) : SV_Target {
  float reference = (floor(pos.x) + 0.5) / 4;
  float2 uv = float2(0.45, 0.45);
  float single = depth.SampleCmp(compare, uv, reference);
  float4 four = depth.GatherCmp(compare, uv, reference, int2(offset, 0));
  return uint2(single * 100 + 0.5, dot(four, 1) + 0.5);
}
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

  // the depth texture, cleared to 0.5 through its depth view and read through a float view
  D3D11_TEXTURE2D_DESC depth_desc{4, 4, 1, 1, DXGI_FORMAT_R32_TYPELESS, {1, 0}, D3D11_USAGE_DEFAULT,
                                  D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE};
  ComPtr<ID3D11Texture2D> depth;
  CHECK(device->CreateTexture2D(&depth_desc, nullptr, &depth));
  ComPtr<ID3D11DepthStencilView> dsv;
  D3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc{DXGI_FORMAT_D32_FLOAT, D3D11_DSV_DIMENSION_TEXTURE2D};
  CHECK(device->CreateDepthStencilView(depth.Get(), &dsv_desc, &dsv));
  ComPtr<ID3D11ShaderResourceView> srvs[2];
  D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{DXGI_FORMAT_R32_FLOAT, D3D11_SRV_DIMENSION_TEXTURE2D};
  srv_desc.Texture2D = {0, 1};
  CHECK(device->CreateShaderResourceView(depth.Get(), &srv_desc, &srvs[0]));
  context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 0.5f, 0);

  // the float texture: each row 0.1, 0.3, 0.6, 0.9
  const float columns[4] = {0.1f, 0.3f, 0.6f, 0.9f};
  float texels[16];
  for (UINT i = 0; i < 16; i++)
    texels[i] = columns[i % 4];
  D3D11_TEXTURE2D_DESC float_desc{4, 4, 1, 1, DXGI_FORMAT_R32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                                  D3D11_BIND_SHADER_RESOURCE};
  D3D11_SUBRESOURCE_DATA float_data{texels, 4 * sizeof(float)};
  ComPtr<ID3D11Texture2D> floats;
  CHECK(device->CreateTexture2D(&float_desc, &float_data, &floats));
  CHECK(device->CreateShaderResourceView(floats.Get(), nullptr, &srvs[1]));

  D3D11_SAMPLER_DESC sampler_desc{D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP,
                                  D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP};
  sampler_desc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
  sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
  ComPtr<ID3D11SamplerState> sampler;
  CHECK(device->CreateSamplerState(&sampler_desc, &sampler));

  const UINT width = 4, rows = 2;
  D3D11_TEXTURE2D_DESC target_desc{width, rows, 1, 1, DXGI_FORMAT_R32G32_UINT, {1, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_RENDER_TARGET};
  ComPtr<ID3D11Texture2D> target, target_read;
  ComPtr<ID3D11RenderTargetView> rtv;
  CHECK(device->CreateTexture2D(&target_desc, nullptr, &target));
  CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
  target_desc.Usage = D3D11_USAGE_STAGING;
  target_desc.BindFlags = 0;
  target_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  CHECK(device->CreateTexture2D(&target_desc, nullptr, &target_read));
  D3D11_BUFFER_DESC constants_desc{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
  const int words[4] = {1};
  D3D11_SUBRESOURCE_DATA constants_data{words};
  ComPtr<ID3D11Buffer> constants;
  CHECK(device->CreateBuffer(&constants_desc, &constants_data, &constants));

  context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->VSSetShader(vertex.Get(), nullptr, 0);
  context->PSSetShader(pixel.Get(), nullptr, 0);
  context->PSSetSamplers(0, 1, sampler.GetAddressOf());
  context->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
  // row 0 reads the depth texture, row 1 the float texture
  for (UINT row = 0; row < rows; row++) {
    D3D11_VIEWPORT viewport{0, (float)row, (float)width, 1, 0, 1};
    context->RSSetViewports(1, &viewport);
    context->PSSetShaderResources(0, 1, srvs[row].GetAddressOf());
    context->Draw(3, 0);
  }

  context->CopyResource(target_read.Get(), target.Get());
  D3D11_MAPPED_SUBRESOURCE mapped;
  CHECK(context->Map(target_read.Get(), 0, D3D11_MAP_READ, 0, &mapped));
  // uv 0.45 is in texel 1.8: the point sample reads column 1, the gather columns 1 and 2, and one texel on, 2 and 3
  const char *names[] = {"the depth texture of 0.5", "the float texture"};
  for (UINT row = 0; row < rows; row++) {
    step("SampleCmp and GatherCmp at a run-time offset of %s, LESS_EQUAL", names[row]);
    auto column = [&](UINT x) { return row == 0 ? 0.5f : columns[x]; };
    auto pixels = reinterpret_cast<const uint32_t *>((const uint8_t *)mapped.pData + mapped.RowPitch * row);
    for (UINT x = 0; x < width; x++) {
      float reference = (x + 0.5f) / 4;
      uint32_t single = reference <= column(1) ? 100 : 0;
      uint32_t four = 2 * (reference <= column(2)) + 2 * (reference <= column(3));
      expect(pixels[2 * x] == single, "SampleCmp at reference %.3f: %u, want %u", reference, pixels[2 * x], single);
      expect(pixels[2 * x + 1] == four, "GatherCmp at reference %.3f: %u texels pass, want %u", reference,
             pixels[2 * x + 1], four);
    }
  }
  context->Unmap(target_read.Get(), 0);
  return verdict();
}
