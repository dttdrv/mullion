// contract: alpha-to-coverage takes its coverage from the pixel shader's own o0.a, before the output merger blends
// (D3D11.3 17.18: "Alpha-to-Coverage ... converts the alpha component of the output color from Pixel Shader output
// register o0 ... to a coverage mask"; an alpha of 0 or less covers no sample, and 1 or more all of them). blending
// comes after, so a blend whose alpha result differs from o0.a does not change which samples are written. a target
// of every sample count it supports, holding (0, 0, 0, `held`), takes (1, 1, 1, o0.a) with ONE/ZERO for color and
// ZERO/ONE for alpha, so a written sample has color 1 and the blended alpha is the target's own; o0.a of 0 over a
// target alpha of 1 writes no sample, and 1 over 0 writes all. R16G16B16A16_FLOAT and R32G32B32A32_FLOAT go above 1,
// which Mullion blends in the pixel shader; R8G8B8A8_UNORM is blended by Metal. each sample is read back through a
// Texture2DMS. the blend states are made through ID3D11Device::CreateBlendState and ID3D11Device1::CreateBlendState1.
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
#include "d3d11_test.hpp"
#include <d3d11_1.h>

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
main() {
  constexpr UINT width = 4, height = 4;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Device1> device1;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                          &context));
  CHECK(device.As(&device1));
  auto vs_code = compile(hlsl, "vs", "vs", {"SAMPLES=1", "WIDTH=1"});
  auto ps_code = compile(hlsl, "ps", "ps", {"SAMPLES=1", "WIDTH=1"});
  if (!expect(vs_code && ps_code, "the shaders do not compile"))
    return verdict();
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  CHECK(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs));
  CHECK(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &ps));

  ComPtr<ID3D11BlendState> blends[2];
  {
    D3D11_BLEND_DESC desc{TRUE, FALSE};
    desc.RenderTarget[0] = {TRUE,
                            D3D11_BLEND_ONE,
                            D3D11_BLEND_ZERO,
                            D3D11_BLEND_OP_ADD,
                            D3D11_BLEND_ZERO,
                            D3D11_BLEND_ONE,
                            D3D11_BLEND_OP_ADD,
                            D3D11_COLOR_WRITE_ENABLE_ALL};
    CHECK(device->CreateBlendState(&desc, &blends[0]));
    D3D11_BLEND_DESC1 desc1{TRUE, FALSE};
    desc1.RenderTarget[0] = {TRUE,
                             FALSE,
                             D3D11_BLEND_ONE,
                             D3D11_BLEND_ZERO,
                             D3D11_BLEND_OP_ADD,
                             D3D11_BLEND_ZERO,
                             D3D11_BLEND_ONE,
                             D3D11_BLEND_OP_ADD,
                             D3D11_LOGIC_OP_NOOP,
                             D3D11_COLOR_WRITE_ENABLE_ALL};
    ComPtr<ID3D11BlendState1> blend1;
    CHECK(device1->CreateBlendState1(&desc1, &blend1));
    blends[1] = blend1;
  }
  auto constants = buffer(device.Get(), 16, D3D11_BIND_CONSTANT_BUFFER);
  if (!expect(bool(constants), "the constant buffer is refused"))
    return verdict();

  const DXGI_FORMAT formats[] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT,
                                 DXGI_FORMAT_R8G8B8A8_UNORM};
  // the target's alpha, and o0.a
  const struct {
    float held, alpha;
  } cases[] = {{1, 0}, {0, 1}};
  for (UINT samples : {2u, 4u, 8u}) {
    const auto s = std::to_string(samples), w = std::to_string(width);
    auto cs_code = compile(hlsl, "cs", "cs", {"SAMPLES=" + s, "WIDTH=" + w});
    if (!expect(bool(cs_code), "the %u-sample read does not compile", samples))
      return verdict();
    ComPtr<ID3D11ComputeShader> cs;
    CHECK(device->CreateComputeShader(cs_code->GetBufferPointer(), cs_code->GetBufferSize(), nullptr, &cs));
    const UINT count = width * height * samples;
    for (auto format : formats) {
      UINT quality = 0;
      if (FAILED(device->CheckMultisampleQualityLevels(format, samples, &quality)) || !quality) {
        printf("format %u has no %u-sample targets\n", format, samples);
        continue;
      }
      for (UINT b = 0; b < 2; b++)
        for (auto &c : cases) {
          step("format %u, %u samples, CreateBlendState%s, target alpha %g, o0.a %g", format, samples, b ? "1" : "",
               c.held, c.alpha);
          D3D11_TEXTURE2D_DESC target_desc{width, height, 1, 1, format, {samples, 0}, D3D11_USAGE_DEFAULT,
                                           D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE};
          ComPtr<ID3D11Texture2D> target;
          ComPtr<ID3D11RenderTargetView> rtv;
          ComPtr<ID3D11ShaderResourceView> srv;
          ComPtr<ID3D11UnorderedAccessView> uav;
          CHECK(device->CreateTexture2D(&target_desc, nullptr, &target));
          CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
          CHECK(device->CreateShaderResourceView(target.Get(), nullptr, &srv));
          D3D11_BUFFER_DESC result_desc{count * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                                        D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16};
          ComPtr<ID3D11Buffer> result;
          CHECK(device->CreateBuffer(&result_desc, nullptr, &result));
          CHECK(device->CreateUnorderedAccessView(result.Get(), nullptr, &uav));

          const float held[4] = {0, 0, 0, c.held};
          context->ClearRenderTargetView(rtv.Get(), held);
          const float cb[4] = {c.alpha};
          context->UpdateSubresource(constants.Get(), 0, nullptr, cb, 0, 0);
          context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
          context->OMSetBlendState(blends[b].Get(), nullptr, UINT_MAX);
          const D3D11_VIEWPORT viewport{0, 0, width, height, 0, 1};
          context->RSSetViewports(1, &viewport);
          context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          context->VSSetShader(vs.Get(), nullptr, 0);
          context->PSSetShader(ps.Get(), nullptr, 0);
          context->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
          context->Draw(3, 0);
          ID3D11RenderTargetView *none = nullptr;
          context->OMSetRenderTargets(1, &none, nullptr);
          context->CSSetShader(cs.Get(), nullptr, 0);
          context->CSSetShaderResources(0, 1, srv.GetAddressOf());
          context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
          context->Dispatch(width, height, 1);
          ID3D11ShaderResourceView *no_srv = nullptr;
          ID3D11UnorderedAccessView *no_uav = nullptr;
          context->CSSetShaderResources(0, 1, &no_srv);
          context->CSSetUnorderedAccessViews(0, 1, &no_uav, nullptr);
          auto words = read(device.Get(), context.Get(), result.Get());
          if (!expect(words.size() == count * 4, "the result is not read back"))
            continue;
          // a covered sample takes the shader's color and keeps the target's alpha; the others keep the clear
          const float want_color = c.alpha > 0 ? 1.0f : 0.0f;
          auto got = reinterpret_cast<const float *>(words.data());
          UINT wrong = 0;
          for (UINT i = 0; i < count; i++) {
            const float *v = got + 4 * i;
            bool ok = v[0] == want_color && v[1] == want_color && v[2] == want_color && v[3] == c.held;
            if (!ok && !wrong++)
              printf("pixel %u, sample %u is (%g, %g, %g, %g), want (%g, %g, %g, %g)\n", i / samples, i % samples,
                     v[0], v[1], v[2], v[3], want_color, want_color, want_color, c.held);
          }
          expect(!wrong, "%u wrong samples of %u", wrong, count);
        }
    }
  }
  return verdict();
}
