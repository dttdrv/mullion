// contract: a feature level 11_0 device has 8x MSAA for every render target format but R32G32B32A32 (Microsoft Learn,
// ID3D11Device::CheckMultisampleQualityLevels remarks), whatever the GPU has; the cases of d3d12_msaa8, through
// D3D11's own textures, blend state sample mask and resolve.
#include "d3d11_test.hpp"
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

int main() {
  const UINT samples = 8, width = 4, words = 10;
  const UINT masks[] = {0x0f, 0x33, 0xf0, 0x3c, 0xcc};
  auto vertex = compile(hlsl, "vs", "vs"), info = compile(hlsl, "ps_info", "ps"), one = compile(hlsl, "ps_one", "ps"),
       read_back = compile(hlsl, "cs", "cs");
  if (!expect(vertex && info && one && read_back, "HLSL did not compile"))
    return verdict();
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                          nullptr, &context));
  for (auto format : {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_D32_FLOAT}) {
    UINT qualities = 0;
    HRESULT hr = device->CheckMultisampleQualityLevels(format, samples, &qualities);
    expect(hr == S_OK && qualities, "format %u at 8 samples: %08lx, %u quality levels", format, hr, qualities);
  }
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11ComputeShader> cs;
  CHECK(device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &vs));
  CHECK(device->CreateComputeShader(read_back->GetBufferPointer(), read_back->GetBufferSize(), nullptr, &cs));
  auto pixel = [&](ComPtr<ID3DBlob> &blob) {
    ComPtr<ID3D11PixelShader> ps;
    expect(SUCCEEDED(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &ps)),
           "pixel shader refused");
    return ps;
  };
  // what each draw stores: the sample count and coverage, the samples the blend state's mask keeps, or the samples the
  // shader's coverage output keeps
  struct Case {
    ComPtr<ID3D11PixelShader> ps;
    UINT sample_mask, kept;
    float value;
  };
  std::vector<Case> cases{{pixel(info), ~0u, samples, float(samples * 256 + 0xff)}};
  for (auto mask : masks)
    cases.push_back({pixel(one), mask, UINT(std::popcount(mask)), 1});
  for (auto mask : masks) {
    auto blob = compile(hlsl, "ps_mask", "ps", {"MASK=" + std::to_string(mask)});
    if (!expect(bool(blob), "ps_mask did not compile"))
      return verdict();
    cases.push_back({pixel(blob), ~0u, UINT(std::popcount(mask)), 1});
  }
  D3D11_RASTERIZER_DESC rd{D3D11_FILL_SOLID, D3D11_CULL_NONE};
  rd.DepthClipEnable = rd.MultisampleEnable = TRUE;
  ComPtr<ID3D11RasterizerState> raster;
  CHECK(device->CreateRasterizerState(&rd, &raster));

  for (UINT c = 0; c < cases.size(); c++) {
    auto &k = cases[c];
    step("case %u, sample mask %#x, %u samples kept", c, k.sample_mask, k.kept);
    context->ClearState();
    D3D11_TEXTURE2D_DESC td{width, 1, 1, 1, DXGI_FORMAT_R32_FLOAT, {samples, 0}, D3D11_USAGE_DEFAULT,
                            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE};
    ComPtr<ID3D11Texture2D> target, single;
    HRESULT made = device->CreateTexture2D(&td, nullptr, &target);
    if (!expect(made == S_OK, "8-sample target returned %08lx", made))
      return verdict();
    D3D11_TEXTURE2D_DESC described;
    target->GetDesc(&described);
    expect(described.SampleDesc.Count == samples, "target describes %u samples", described.SampleDesc.Count);
    td.SampleDesc = {1, 0};
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    CHECK(device->CreateTexture2D(&td, nullptr, &single));
    std::vector<UINT> initial(width * words, 0);
    D3D11_BUFFER_DESC bd{UINT(initial.size() * sizeof(UINT)), D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                         D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, sizeof(UINT)};
    D3D11_SUBRESOURCE_DATA data{initial.data()};
    ComPtr<ID3D11Buffer> result;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11ShaderResourceView> srvs[2];
    ComPtr<ID3D11RenderTargetView> rtv;
    CHECK(device->CreateBuffer(&bd, &data, &result));
    CHECK(device->CreateUnorderedAccessView(result.Get(), nullptr, &uav));
    CHECK(device->CreateShaderResourceView(target.Get(), nullptr, &srvs[0]));
    CHECK(device->CreateShaderResourceView(single.Get(), nullptr, &srvs[1]));
    CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
    const float clear[4]{};
    context->ClearRenderTargetView(rtv.Get(), clear);
    context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    context->OMSetBlendState(nullptr, nullptr, k.sample_mask);
    D3D11_VIEWPORT viewport{0, 0, float(width), 1, 0, 1};
    context->RSSetViewports(1, &viewport);
    context->RSSetState(raster.Get());
    context->VSSetShader(vs.Get(), nullptr, 0);
    context->PSSetShader(k.ps.Get(), nullptr, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->Draw(3, 0);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    context->ResolveSubresource(single.Get(), 0, target.Get(), 0, DXGI_FORMAT_R32_FLOAT);
    ID3D11ShaderResourceView *views[] = {srvs[0].Get(), srvs[1].Get()};
    context->CSSetShader(cs.Get(), nullptr, 0);
    context->CSSetShaderResources(0, 2, views);
    context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
    context->Dispatch(width, 1, 1);
    auto got = read(device.Get(), context.Get(), result.Get());
    if (!expect(got.size() == width * words, "readback size %zu", got.size()))
      return verdict();
    for (UINT x = 0; x < width; x++) {
      auto at = [&](UINT i) { return std::bit_cast<float>(got[x * words + i]); };
      expect(at(0) == samples, "pixel %u: Texture2DMS has %g samples", x, at(0));
      UINT kept = 0;
      float sum = 0;
      for (UINT s = 0; s < samples; s++) {
        float v = at(1 + s);
        expect(v == 0 || v == k.value, "pixel %u sample %u is %g, want 0 or %g", x, s, v, k.value);
        kept += v == k.value;
        sum += v;
      }
      expect(kept == k.kept, "pixel %u keeps %u samples, want %u", x, kept, k.kept);
      expect(at(9) == sum / samples, "pixel %u resolves to %g, the samples' average is %g", x, at(9), sum / samples);
    }
  }
  return verdict();
}
