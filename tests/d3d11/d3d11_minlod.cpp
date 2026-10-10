// contract: a resource's MinLOD clamp (ID3D11DeviceContext::SetResourceMinLOD; D3D11.3 5.8).
// - "the per-resource MinLOD clamp considers the most detailed mipmap on the resource as LOD 0", whichever levels a
//   view shows: through a view that starts at a later level the clamp is that much less (5.8.3).
// - a sample that asks for a level the clamp takes off fetches "the most detailed available clamped mip" (5.8.5), and
//   a fractional clamp blends the two levels around it under linear mip filtering (5.8.4).
// - a load, which names its level, reads 0 from a level before the clamp's floor (5.8.5: "ld* instructions ... also
//   honor the per-resource MinLOD clamp"; the floor "is required to be present", 5.8.4).
// - a clamp "past the least detailed mip in the view (e.g. 5.1)" leaves the view no levels: "the out of bounds
//   behavior applies", 0, to samples and loads (5.8.5).
// - the clamp set is the clamp got, and 0 again shows every level.
// each level of the texture holds its number plus one, and a compute shader samples and loads the level asked for.
#include "d3d11_test.hpp"
#include <algorithm>
#include <cmath>

static const char hlsl[] = R"hlsl(
Texture2D<float> whole : register(t0);
Texture2D<float> later : register(t1);
Texture2D<float> early : register(t2);
SamplerState trilinear : register(s0);
cbuffer Ask : register(b0) { float lod; };
RWStructuredBuffer<float2> read : register(u0);
[numthreads(1, 1, 1)] void cs() {
  read[0] = float2(whole.SampleLevel(trilinear, 0.5, lod), whole.Load(int3(0, 0, lod)));
  read[1] = float2(later.SampleLevel(trilinear, 0.5, lod), later.Load(int3(0, 0, lod)));
  read[2] = float2(early.SampleLevel(trilinear, 0.5, lod), early.Load(int3(0, 0, lod)));
}
)hlsl";

int
main() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context));
  auto code = compile(hlsl, "cs", "cs");
  if (!code) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D11ComputeShader> cs;
  CHECK(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs));

  const UINT size = 16, levels = 5, first = 1;
  D3D11_TEXTURE2D_DESC desc{size, size, levels, 1, DXGI_FORMAT_R32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                            D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_RESOURCE_CLAMP};
  std::vector<std::vector<float>> texels(levels);
  D3D11_SUBRESOURCE_DATA data[levels];
  for (UINT l = 0; l < levels; l++) {
    texels[l].assign((size >> l) * (size >> l), float(l + 1));
    data[l] = {texels[l].data(), UINT((size >> l) * sizeof(float))};
  }
  ComPtr<ID3D11Texture2D> texture;
  CHECK(device->CreateTexture2D(&desc, data, &texture));
  // the levels each view shows, a first one and a count: all of them, those from `first`, and the first two
  const UINT shown[][2] = {{0, levels}, {first, levels - first}, {0, 2}};
  ComPtr<ID3D11ShaderResourceView> srvs[std::size(shown)];
  ID3D11ShaderResourceView *views[std::size(shown)];
  for (UINT v = 0; v < std::size(shown); v++) {
    D3D11_SHADER_RESOURCE_VIEW_DESC view_desc{DXGI_FORMAT_R32_FLOAT, D3D11_SRV_DIMENSION_TEXTURE2D};
    view_desc.Texture2D = {shown[v][0], shown[v][1]};
    CHECK(device->CreateShaderResourceView(texture.Get(), &view_desc, &srvs[v]));
    views[v] = srvs[v].Get();
  }
  D3D11_SAMPLER_DESC sampler_desc{D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
                                  D3D11_TEXTURE_ADDRESS_CLAMP, 0, 1, D3D11_COMPARISON_NEVER, {}, 0, D3D11_FLOAT32_MAX};
  ComPtr<ID3D11SamplerState> sampler;
  CHECK(device->CreateSamplerState(&sampler_desc, &sampler));
  D3D11_BUFFER_DESC ask_desc{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
  D3D11_BUFFER_DESC out_desc{UINT(8 * std::size(shown)), D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 8};
  ComPtr<ID3D11Buffer> ask, out;
  ComPtr<ID3D11UnorderedAccessView> uav;
  CHECK(device->CreateBuffer(&ask_desc, nullptr, &ask));
  CHECK(device->CreateBuffer(&out_desc, nullptr, &out));
  CHECK(device->CreateUnorderedAccessView(out.Get(), nullptr, &uav));
  context->CSSetShader(cs.Get(), nullptr, 0);
  context->CSSetShaderResources(0, std::size(shown), views);
  context->CSSetSamplers(0, 1, sampler.GetAddressOf());
  context->CSSetConstantBuffers(0, 1, ask.GetAddressOf());
  context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);

  unsigned wrong = 0, cases = 0;
  // what a level of the resource holds, between two of them their blend
  auto held = [](float lod) { return lod + 1; };
  for (float clamp : {0.0f, 2.0f, 1.25f, 3.0f, levels - 0.5f, 0.0f}) {
    context->SetResourceMinLOD(texture.Get(), clamp);
    if (context->GetResourceMinLOD(texture.Get()) != clamp && wrong++ < 8)
      printf("the clamp got is not the clamp %g set\n", clamp);
    for (float lod : {0.0f, 1.0f, 2.5f}) {
      const float asked[4] = {lod};
      context->UpdateSubresource(ask.Get(), 0, nullptr, asked, 0, 0);
      context->Dispatch(1, 1, 1);
      auto got = read(device.Get(), context.Get(), out.Get());
      for (UINT v = 0; v < std::size(shown); v++) {
        // a view's level is the resource's after its first. a view whose last level the clamp is past has none;
        // else a sample fetches no level before the clamp and none after the view's last, and a load reads the level
        // it names if that is there
        const float begin = shown[v][0], last = begin + shown[v][1] - 1, named = begin + std::floor(lod);
        const bool empty = clamp > last;
        const float want[2] = {empty ? 0 : held(std::clamp(lod + begin, clamp, last)),
                               empty || named < std::floor(clamp) || named > last ? 0 : held(named)};
        for (UINT r = 0; r < 2; r++) {
          float value;
          memcpy(&value, &got[2 * v + r], sizeof(value));
          cases++;
          if (!(std::fabs(value - want[r]) <= 1.0f / 128) && wrong++ < 8)
            printf("clamp %g, level %g of the view of levels %g to %g: %s %g, want %g\n", clamp, lod, begin, last,
                   r ? "loaded" : "sampled", value, want[r]);
        }
      }
    }
  }
  printf("%s: %u wrong of %u samples\n", wrong ? "failed" : "passed", wrong, cases);
  return wrong != 0;
}
