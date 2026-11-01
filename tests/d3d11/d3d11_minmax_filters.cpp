// contract: a device that reports
// D3D11_FEATURE_DATA_D3D11_OPTIONS1.MinMaxFiltering takes the
// D3D11_FILTER_MINIMUM_* and D3D11_FILTER_MAXIMUM_* filters, which fetch "the
// same set of texels that a normal texture filter would fetch" and return "the
// min() or max() of the texels fetched, on a per-component basis", a texel
// "that would be 0 weight on a non-min/max filter" not contributing
// (D3D11.3 5.9.4.5.6, Min/Max Reduction Filtering; 7.18.3 lists the filters).
// ../minmax_filters.hpp computes the texels from D3D11.3 7.18 and reduces them.
// a 2x1 {0, 1} texture's shared edge reads 0 and 1, a texel's centre only that
// texel; a 4x1 texture at each address mode's edges and beyond, with a border
// color; a mipped RGBA texture of distinct values at whole and fractional LODs,
// with gradients, its quad's derivatives and a bias. an averaging sampler and a
// comparison sampler stay as they were.
#include "d3d11_test.hpp"
#include "../minmax_filters.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

using namespace minmax_filters;

static const char hlsl[] = R"hlsl(
Texture2D<float> two : register(t0);
Texture2D<float> four : register(t1);
Texture2D<float4> mipped : register(t2);
Texture2D<float> depth : register(t3);
SamplerState s : register(s0);
SamplerComparisonState cmp : register(s1);
SamplerState other : register(s2);
cbuffer C : register(b0) { float4 uv, dx, dy, extra; uint op, image; };
float4 vs(uint id : SV_VertexID) : SV_Position {
  return float4(float2((id << 1) & 2, id & 2) * float2(2, -2) + float2(-1, 1), 0, 1);
}
// op 0: SampleLevel, 1: SampleGrad, 2: Sample, 3: SampleBias, 4: SampleCmpLevelZero, 6 and 7: four SampleLevels or
// Samples in a row, each pixel storing one: the second through the same sampler, the third reading the first (moved by
// 0.125 when implicit), the fourth through `other`
#define SAMPLE(T)                                                                                                    \
  if (op == 0)                                                                                                       \
    v = T.SampleLevel(s, at, extra.x);                                                                               \
  else if (op == 1)                                                                                                  \
    v = T.SampleGrad(s, at, dx.xy, dy.xy);                                                                           \
  else if (op == 2)                                                                                                  \
    v = T.Sample(s, at);                                                                                             \
  else                                                                                                               \
    v = T.SampleBias(s, at, extra.y);
float4 ps(float4 p : SV_Position) : SV_Target {
  float2 q = floor(p.xy) % 2;
  float2 at = uv.xy + q.x * dx.xy + q.y * dy.xy;
  float4 v = -1;
  if (op == 4) {
    v = depth.SampleCmpLevelZero(cmp, at, extra.z);
  } else if (op >= 6) {
    uint k = uint(q.y) * 2 + uint(q.x);
    float4 r0, r1, r2, r3;
    if (op == 6) {
      r0 = mipped.SampleLevel(s, at, extra.x);
      r1 = mipped.SampleLevel(s, at + float2(0, 0.25), extra.x);
      r2 = mipped.SampleLevel(s, at + r0.xy / 1024, extra.x);
      r3 = mipped.SampleLevel(other, at + float2(0.25, 0), extra.x);
    } else {
      r0 = mipped.Sample(s, at);
      r1 = mipped.Sample(s, at + float2(0, 0.25));
      r2 = mipped.Sample(s, at + float2(0.125, 0.125));
      r3 = mipped.Sample(other, at + float2(0.25, 0));
    }
    v = k == 0 ? r0 : k == 1 ? r1 : k == 2 ? r2 : r3;
  } else if (image == 0) {
    SAMPLE(two)
  } else if (image == 1) {
    SAMPLE(four)
  } else {
    SAMPLE(mipped)
  }
  return v;
}
)hlsl";

struct Case {
  UINT op, image, sampler;
  float uv[4], dx[4], dy[4], extra[4];
};

int
main() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context
  ));
  step("ask for MinMaxFiltering");
  D3D11_FEATURE_DATA_D3D11_OPTIONS1 options{};
  CHECK(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS1, &options, sizeof(options)));
  // the filters are sampled below whatever the answer, so a device that
  // averages them shows it
  expect(options.MinMaxFiltering, "MinMaxFiltering is FALSE");

  step("compile the sampling shaders");
  auto vs_code = compile(hlsl, "vs", "vs"), ps_code = compile(hlsl, "ps", "ps");
  if (!expect(vs_code && ps_code, "HLSL did not compile"))
    return verdict();
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  CHECK(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs));
  CHECK(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &ps));

  const Image images[] = {
      {Image::Plane, 2, 1, 1, 1, 1, 0, 1},
      {Image::Plane, 4, 1, 1, 1, 1, 1, 1},
      {Image::Plane, 8, 8, 1, 1, 4, 2, 4},
      {Image::Plane, 2, 1, 1, 1, 1, 0, 1},
  };
  auto same = [](D3D11_FILTER filter, Address mode, float4 border = {}) {
    return Spec{UINT(filter), {mode, mode, mode}, border};
  };
  std::vector<Spec> specs = {
      same(D3D11_FILTER_MINIMUM_MIN_MAG_MIP_LINEAR, Clamp),
      same(D3D11_FILTER_MAXIMUM_MIN_MAG_MIP_LINEAR, Clamp),
      same(D3D11_FILTER_MINIMUM_MIN_MAG_MIP_POINT, Clamp),
      same(D3D11_FILTER_MAXIMUM_MIN_MAG_MIP_POINT, Clamp),
      same(D3D11_FILTER_MIN_MAG_MIP_LINEAR, Clamp),
      same(D3D11_FILTER_MINIMUM_MIN_MAG_LINEAR_MIP_POINT, Clamp),
      same(D3D11_FILTER_MAXIMUM_MIN_POINT_MAG_LINEAR_MIP_POINT, Clamp),
      same(D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, Clamp),
  };
  const UINT compare = 7, first_address = UINT(specs.size());
  for (Address mode : {Wrap, Mirror, Clamp, Border, MirrorOnce})
    for (auto filter : {D3D11_FILTER_MINIMUM_MIN_MAG_MIP_LINEAR, D3D11_FILTER_MAXIMUM_MIN_MAG_MIP_LINEAR})
      specs.push_back(same(filter, mode, {1, 1, 1, 1}));

  step("make the samplers");
  std::vector<ComPtr<ID3D11SamplerState>> samplers(specs.size());
  for (UINT i = 0; i < specs.size(); i++) {
    auto &spec = specs[i];
    D3D11_SAMPLER_DESC desc{
        D3D11_FILTER(spec.filter),
        D3D11_TEXTURE_ADDRESS_MODE(spec.address[0]),
        D3D11_TEXTURE_ADDRESS_MODE(spec.address[1]),
        D3D11_TEXTURE_ADDRESS_MODE(spec.address[2]),
        0,
        1,
        D3D11_COMPARISON_LESS,
        {spec.border[0], spec.border[1], spec.border[2], spec.border[3]},
        0,
        D3D11_FLOAT32_MAX
    };
    expect(SUCCEEDED(device->CreateSamplerState(&desc, &samplers[i])), "filter 0x%x was refused", spec.filter);
  }
  if (!expect(std::all_of(samplers.begin(), samplers.end(), [](auto &s) { return bool(s); }), "a sampler is missing"))
    return verdict();

  std::vector<Case> cases;
  auto add = [&](UINT op, UINT image, UINT sampler, float4 uv, float4 dx = {}, float4 dy = {}, float4 extra = {}) {
    Case c{op, image, sampler};
    std::copy_n(uv.data(), 4, c.uv), std::copy_n(dx.data(), 4, c.dx), std::copy_n(dy.data(), 4, c.dy);
    std::copy_n(extra.data(), 4, c.extra);
    cases.push_back(c);
  };
  for (UINT sampler : {0, 1, 2, 3, 4})
    for (float u : {0.5f, 0.25f, 0.75f, 0.4f, 0.6f})
      add(0, 0, sampler, {u, 0.5f});
  for (UINT sampler = first_address; sampler < specs.size(); sampler++)
    for (float u : {0.0f, 1.0f, -0.25f, 1.25f})
      add(0, 1, sampler, {u, 0.5f});
  for (UINT sampler : {0, 1, 2, 3, 5, 6})
    for (float4 uv : {float4{0.5f, 0.5f}, float4{0.3f, 0.7f}, float4{0.0625f, 0.9375f}})
      for (float lod : {0.0f, 0.5f, 1.0f, 1.25f, 3.0f, -1.0f})
        add(0, 2, sampler, uv, {}, {}, {lod});
  for (UINT sampler : {0, 1, 5})
    for (float texels : {2.0f, 3.0f, 0.5f})
      for (UINT op : {1, 2, 3}) {
        float step = texels / 8;
        add(op, 2, sampler, {0.37f, 0.61f}, {step}, {0, step}, {0, 1});
      }
  // runs of samples whose branches the converter joins where they have one sampler (MergeMinMaxBranchesPass): one
  // that reduces beside one that averages, either way round, and two that reduce differently
  for (float4 slots : {float4{0, 4}, float4{4, 1}, float4{1, 0}})
    for (float level : {0.5f, 1.0f}) {
      add(6, 2, UINT(slots[0]), {0.3f, 0.4f}, {}, {}, {level, 0, slots[1]});
      add(7, 2, UINT(slots[0]), {0.3f, 0.4f}, {level * 2 / 8}, {0, level * 2 / 8}, {0, 0, slots[1]});
    }
  for (float u : {0.5f, 0.25f})
    add(4, 3, compare, {u, 0.5f}, {}, {}, {0, 0, 0.5f});
  const UINT count = UINT(cases.size());

  step("make the textures");
  ComPtr<ID3D11ShaderResourceView> views[4];
  for (UINT i = 0; i < 3; i++) {
    auto &image = images[i];
    std::vector<std::vector<float>> texels(image.mips);
    std::vector<D3D11_SUBRESOURCE_DATA> data(image.mips);
    for (UINT m = 0; m < image.mips; m++) {
      UINT w = image.size(0, m), h = image.size(1, m);
      for (UINT y = 0; y < h; y++)
        for (UINT x = 0; x < w; x++)
          for (UINT c = 0; c < image.channels; c++)
            texels[m].push_back(image.value(0, m, x, y, 0, c));
      data[m] = {texels[m].data(), UINT(w * image.channels * sizeof(float))};
    }
    DXGI_FORMAT format = image.channels == 4 ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R32_FLOAT;
    D3D11_TEXTURE2D_DESC desc{image.width, image.height, image.mips,          1,
                              format,      {1, 0},       D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
    ComPtr<ID3D11Texture2D> texture;
    CHECK(device->CreateTexture2D(&desc, data.data(), &texture));
    CHECK(device->CreateShaderResourceView(texture.Get(), nullptr, &views[i]));
  }
  {
    // a depth texture of {0, 1}, from its initial data
    D3D11_TEXTURE2D_DESC desc{
        2,
        1,
        1,
        1,
        DXGI_FORMAT_R32_TYPELESS,
        {1, 0},
        D3D11_USAGE_DEFAULT,
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL
    };
    const float texels[] = {0, 1};
    D3D11_SUBRESOURCE_DATA data{texels, sizeof(texels)};
    ComPtr<ID3D11Texture2D> texture;
    CHECK(device->CreateTexture2D(&desc, &data, &texture));
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{DXGI_FORMAT_R32_FLOAT, D3D11_SRV_DIMENSION_TEXTURE2D};
    srv.Texture2D = {0, 1};
    CHECK(device->CreateShaderResourceView(texture.Get(), &srv, &views[3]));
  }
  D3D11_TEXTURE2D_DESC target_desc{
      2 * count, 2, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET
  };
  ComPtr<ID3D11Texture2D> target, staging;
  CHECK(device->CreateTexture2D(&target_desc, nullptr, &target));
  target_desc.Usage = D3D11_USAGE_STAGING;
  target_desc.BindFlags = 0;
  target_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  CHECK(device->CreateTexture2D(&target_desc, nullptr, &staging));
  ComPtr<ID3D11RenderTargetView> rtv;
  CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
  // what no case reads: NaN
  const float nan[4] = {NAN, NAN, NAN, NAN};
  context->ClearRenderTargetView(rtv.Get(), nan);
  auto cb = buffer(device.Get(), 80, D3D11_BIND_CONSTANT_BUFFER);
  if (!expect(bool(cb), "constant buffer was not created"))
    return verdict();

  step("draw %u cases", count);
  ID3D11ShaderResourceView *srvs[] = {views[0].Get(), views[1].Get(), views[2].Get(), views[3].Get()};
  context->VSSetShader(vs.Get(), nullptr, 0);
  context->PSSetShader(ps.Get(), nullptr, 0);
  context->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
  context->PSSetShaderResources(0, 4, srvs);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
  for (UINT i = 0; i < count; i++) {
    auto &c = cases[i];
    float constants[20];
    std::copy_n(c.uv, 4, constants), std::copy_n(c.dx, 4, constants + 4), std::copy_n(c.dy, 4, constants + 8);
    std::copy_n(c.extra, 4, constants + 12);
    memcpy(constants + 16, &c.op, 4), memcpy(constants + 17, &c.image, 4);
    constants[18] = constants[19] = 0;
    context->UpdateSubresource(cb.Get(), 0, nullptr, constants, 0, 0);
    ID3D11SamplerState *bound[] = {c.op == 4 ? nullptr : samplers[c.sampler].Get(), samplers[compare].Get(),
                                   samplers[c.op >= 6 ? UINT(c.extra[2]) : 0].Get()};
    context->PSSetSamplers(0, 3, bound);
    D3D11_VIEWPORT viewport{float(2 * i), 0, 2, 2, 0, 1};
    context->RSSetViewports(1, &viewport);
    context->Draw(3, 0);
  }
  context->CopyResource(staging.Get(), target.Get());

  D3D11_MAPPED_SUBRESOURCE mapped;
  CHECK(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
  const char *ops[] = {"SampleLevel",        "SampleGrad", "Sample",          "SampleBias",
                       "SampleCmpLevelZero", "",           "SampleLevel run", "Sample run"};
  UINT wrong = 0;
  for (UINT i = 0; i < count; i++) {
    auto &c = cases[i];
    auto &image = images[c.image];
    for (UINT p = 0; p < 4; p++) {
      // a run's last pixel stores the sample through its other sampler
      auto &spec = specs[c.op >= 6 && p == 3 ? UINT(c.extra[2]) : c.sampler];
      float qx = float(p & 1), qy = float(p >> 1);
      float coord[3];
      for (UINT a = 0; a < 3; a++)
        coord[a] = c.uv[a] + qx * c.dx[a] + qy * c.dy[a];
      float4 want;
      if (c.op == 4) {
        // LESS: the reference below the texel passes; a linear filter averages
        // the results (7.18.15)
        float u = minmax_filters::fixed(coord[0] * 2 - 0.5f), f = u - std::floor(u);
        auto texel = [&](int x) { return image.value(0, 0, std::clamp(x, 0, 1), 0, 0, 0); };
        float left = c.extra[2] < texel(int(std::floor(u))) ? 1.0f : 0.0f;
        float right = c.extra[2] < texel(int(std::floor(u)) + 1) ? 1.0f : 0.0f;
        want = {left * (1 - f) + right * f};
      } else {
        float lod = c.op == 0 || c.op == 6 ? c.extra[0]
                                           : gradient_lod(image, c.dx, c.dy) + (c.op == 3 ? c.extra[1] : 0);
        const int none[3] = {};
        if (c.op >= 6) {
          // the run's samples: the second 0.25 lower, the third moved by the first's red and green over 1024 (or by
          // 0.125 when they are implicit), the fourth 0.25 to the right
          if (p == 1)
            coord[1] += 0.25f;
          else if (p == 2 && c.op == 6) {
            auto first = reference(image, spec, coord, 0, lod, none);
            coord[0] += first[0] / 1024, coord[1] += first[1] / 1024;
          } else if (p == 2)
            coord[0] += 0.125f, coord[1] += 0.125f;
          else if (p == 3)
            coord[0] += 0.25f;
        }
        want = reference(image, spec, coord, 0, lod, none);
      }
      auto value = (const float *)((const char *)mapped.pData + UINT(qy) * mapped.RowPitch) + 4 * (2 * i + UINT(qx));
      bool ok = true;
      for (UINT k = 0; k < image.channels; k++)
        ok &= spec.reduces() ? value[k] == want[k] : std::fabs(value[k] - want[k]) <= 1.0f / 256;
      if (!ok && wrong++ < 64) {
        step(
            "case %u: %s of texture %u through sampler %u (filter 0x%x), "
            "pixel %u, coordinate (%g, %g), LOD %g",
            i, ops[c.op], c.image, c.sampler, spec.filter, p, coord[0], coord[1], c.extra[0]
        );
        expect(
            false, "read (%g, %g, %g, %g), expected (%g, %g, %g, %g)", value[0], value[1], value[2], value[3], want[0],
            want[1], want[2], want[3]
        );
      } else if (!ok) {
        expect(false, "case %u pixel %u is wrong too", i, p);
      }
    }
  }
  context->Unmap(staging.Get(), 0);
  return verdict();
}
