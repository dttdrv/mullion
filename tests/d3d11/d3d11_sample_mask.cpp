// contract: D3D11.3 17.17: "SampleMask is always applied, regardless of whether
// Multisample rendertargets are bound or not." 17.18 fixes A2C endpoints and
// says "If the Pixel Shader outputs oMask (output coverage), Alpha-to-Coverage
// is disabled." the number of constant-one samples must equal the surviving
// mask bits, independent of the allowed mask-bit mapping.
// every queried sample count/quality runs on immediate and deferred contexts.
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
#include "d3d11_test.hpp"
#include "../om_samples.hpp"
#include "../metal_samples.hpp"

int main() {
  auto defines = om::sample_defines();
  auto vertex = compile(om::sample_hlsl, "vs", "vs", defines);
  ComPtr<ID3DBlob> pixels[] = {
      compile(om::sample_hlsl, "ps", "ps", defines),
      compile(om::sample_hlsl, "ps_coverage", "ps", defines)};
  ComPtr<ID3DBlob> reads[2];
  for (UINT single = 0; single < std::size(reads); single++) {
    auto d = defines;
    d.push_back("SINGLE=" + std::to_string(single));
    d.push_back("COMPUTE_READ=1");
    reads[single] = compile(om::sample_hlsl, "cs", "cs", d);
  }
  if (!expect(vertex && pixels[0] && pixels[1] && reads[0] && reads[1],
              "sample-mask HLSL did not compile"))
    return verdict();
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level,
                          1, D3D11_SDK_VERSION, &device, nullptr, &immediate));
  CHECK(device->CreateDeferredContext(0, &deferred));
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps[std::size(pixels)];
  CHECK(device->CreateVertexShader(vertex->GetBufferPointer(),
                                   vertex->GetBufferSize(), nullptr, &vs));
  for (UINT i = 0; i < std::size(pixels); i++)
    CHECK(device->CreatePixelShader(pixels[i]->GetBufferPointer(),
                                    pixels[i]->GetBufferSize(), nullptr,
                                    &ps[i]));
  ComPtr<ID3D11ComputeShader> cs[std::size(reads)];
  for (UINT i = 0; i < std::size(reads); i++)
    CHECK(device->CreateComputeShader(reads[i]->GetBufferPointer(),
                                      reads[i]->GetBufferSize(), nullptr,
                                      &cs[i]));
  ComPtr<ID3D11BlendState> states[2];
  for (UINT alpha = 0; alpha < std::size(states); alpha++) {
    D3D11_BLEND_DESC desc = CD3D11_BLEND_DESC(D3D11_DEFAULT);
    desc.AlphaToCoverageEnable = alpha;
    CHECK(device->CreateBlendState(&desc, &states[alpha]));
  }
  D3D11_RASTERIZER_DESC rd{D3D11_FILL_SOLID, D3D11_CULL_NONE};
  rd.DepthClipEnable = rd.ScissorEnable = rd.MultisampleEnable = TRUE;
  ComPtr<ID3D11RasterizerState> raster;
  CHECK(device->CreateRasterizerState(&rd, &raster));
  for (UINT samples = 1; samples <= D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT;
       samples++) {
    UINT qualities = UINT_MAX;
    HRESULT hr = device->CheckMultisampleQualityLevels(DXGI_FORMAT_R32_FLOAT,
                                                       samples, &qualities);
    if (!expect(
            (samples != 1 && samples != 4 && (samples != 8 || !metal_has_8x_samples())) ||
                (hr == S_OK && qualities),
            "required R32_FLOAT sample count %u returned %08lx, %u qualities",
            samples, hr, qualities))
      return verdict();
    if (FAILED(hr) || !qualities) {
      printf("unsupported R32_FLOAT sample count %u: %08lx, %u qualities\n",
             samples, hr, qualities);
      continue;
    }
    const auto cases = om::sample_cases(samples);
    const UINT width = om::sample_width, height = UINT(cases.size());
    for (UINT quality = 0; quality < qualities; quality++)
      for (bool recorded : {false, true}) {
        step("sample count %u, quality %u, deferred %u: initialize", samples,
             quality, recorded);
        immediate->ClearState();
        deferred->ClearState();
        auto context = recorded ? deferred.Get() : immediate.Get();
        D3D11_TEXTURE2D_DESC td{width,
                                height,
                                1,
                                1,
                                DXGI_FORMAT_R32_FLOAT,
                                {samples, quality},
                                D3D11_USAGE_DEFAULT,
                                D3D11_BIND_RENDER_TARGET |
                                    D3D11_BIND_SHADER_RESOURCE};
        ComPtr<ID3D11Texture2D> target;
        CHECK(device->CreateTexture2D(&td, nullptr, &target));
        const UINT metadata_words = 3,
                   word_count = metadata_words + width * height * samples;
        std::vector<UINT> initial(word_count, UINT_MAX);
        D3D11_BUFFER_DESC bd{
            word_count * UINT(sizeof(UINT)),       D3D11_USAGE_DEFAULT,
            D3D11_BIND_UNORDERED_ACCESS,           0,
            D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, sizeof(UINT)};
        D3D11_SUBRESOURCE_DATA data{initial.data()};
        ComPtr<ID3D11Buffer> result;
        ComPtr<ID3D11UnorderedAccessView> uav;
        ComPtr<ID3D11ShaderResourceView> srv;
        CHECK(device->CreateBuffer(&bd, &data, &result));
        CHECK(device->CreateUnorderedAccessView(result.Get(), nullptr, &uav));
        CHECK(device->CreateShaderResourceView(target.Get(), nullptr, &srv));
        ComPtr<ID3D11RenderTargetView> rtv;
        CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
        const float black[4]{};
        context->ClearRenderTargetView(rtv.Get(), black);
        context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->RSSetState(raster.Get());
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        for (UINT y = 0; y < cases.size(); y++) {
          const auto &k = cases[y];
          step("sample count %u, quality %u, deferred %u, case %u", samples,
               quality, recorded, y);
          D3D11_VIEWPORT viewport{0, float(y), float(width), 1, 0, 1};
          D3D11_RECT scissor{1, LONG(y), LONG(width - 1), LONG(y + 1)};
          context->RSSetViewports(1, &viewport);
          context->RSSetScissorRects(1, &scissor);
          context->PSSetShader(ps[k.coverage].Get(), nullptr, 0);
          context->OMSetBlendState(states[k.alpha].Get(), nullptr, k.mask);
          context->Draw(3, 0);
        }
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->CSSetShader(cs[samples == 1].Get(), nullptr, 0);
        context->CSSetShaderResources(0, 1, srv.GetAddressOf());
        context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
        context->Dispatch(width, height, 1);
        ID3D11ShaderResourceView *no_srv = nullptr;
        ID3D11UnorderedAccessView *no_uav = nullptr;
        context->CSSetShaderResources(0, 1, &no_srv);
        context->CSSetUnorderedAccessViews(0, 1, &no_uav, nullptr);
        if (recorded) {
          ComPtr<ID3D11CommandList> commands;
          CHECK(deferred->FinishCommandList(FALSE, &commands));
          immediate->ExecuteCommandList(commands.Get(), FALSE);
        }
        auto got = read(device.Get(), immediate.Get(), result.Get());
        if (!expect(got.size() == word_count, "sample readback size"))
          return verdict();
        expect(got[0] == width && got[1] == height && got[2] == samples,
               "sample dimensions %u,%u,%u, want %u,%u,%u", got[0], got[1],
               got[2], width, height, samples);
        for (UINT y = 0; y < cases.size(); y++) {
          step("sample count %u, quality %u, deferred %u, case %u: readback",
               samples, quality, recorded, y);
          for (UINT x = 0; x < width; x++) {
            UINT count = 0;
            for (UINT sid = 0; sid < samples; sid++) {
              UINT value =
                  got[metadata_words + (y * width + x) * samples + sid];
              expect(value == 0 || value == std::bit_cast<UINT>(1.0f),
                     "pixel %u sample %u has %#x", x, sid, value);
              count += value == std::bit_cast<UINT>(1.0f);
            }
            UINT want = om::sample_result(cases[y], samples, x);
            expect(count == want,
                   "pixel %u has %u covered samples, want %u, mask %#x, A2C "
                   "%u, coverage %u",
                   x, count, want, cases[y].mask, cases[y].alpha,
                   cases[y].coverage);
          }
        }
      }
  }
  return verdict();
}
