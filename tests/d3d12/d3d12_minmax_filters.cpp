// contract: D3D12_FILTER_MINIMUM_* and D3D12_FILTER_MAXIMUM_* "Fetch the same set of texels as" the filter without
// MINIMUM or MAXIMUM "and instead of filtering them return the minimum" (or maximum) "of the texels. Texels that are
// weighted 0 during filtering aren't counted towards the minimum" (Microsoft Learn, D3D12_FILTER), as D3D11.3
// 5.9.4.5.6 (Min/Max Reduction Filtering) defines them; feature level 12_0 requires them (Microsoft Learn, Hardware
// Feature Levels, "Min/Max Filters"). ../minmax_filters.hpp computes the texels from D3D11.3 7.18 and reduces them.
// a 2x1 {0, 1} texture's shared edge reads 0 and 1, a texel's centre only that texel; a mipped RGBA texture of
// distinct values is read with SampleLevel at whole and fractional LODs, past its mips and below 0, with gradients
// and with its quad's derivatives, a bias and an immediate offset; each address mode at both edges and beyond, with a
// custom border color and one of Metal's; 1D, 2D array, 3D (its W address too) and cube textures (a cube's footprint
// stays inside one face: Mullion's emulation does not cross cube edges, 7.18.12); filters whose min, mag and mip parts
// differ; descriptor samplers and static ones. an averaging sampler and a comparison sampler stay as they were.
#include "d3d12_test.hpp"
#include "../minmax_filters.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

using namespace minmax_filters;

constexpr UINT kSamplers = 21, kImages = 8;

static const char hlsl[] = R"hlsl(
struct Case { uint op, image, slot, unused; float4 uv, dx, dy, extra; };
Texture2D<float> two : register(t0);
Texture2D<float> four : register(t1);
Texture2D<float4> mipped : register(t2);
Texture1D<float4> row : register(t3);
Texture2DArray<float4> layers : register(t4);
Texture3D<float4> volume : register(t5);
TextureCube<float4> cube : register(t6);
Texture2D<float> depth : register(t7);
StructuredBuffer<Case> cases : register(t8);
SamplerState s[SAMPLERS] : register(s0);
SamplerComparisonState cmp : register(s0, space3);
SamplerState smin : register(s0, space1);
SamplerState smax : register(s1, space1);
SamplerState splain : register(s2, space1);
cbuffer K : register(b0) { uint index; };
RWStructuredBuffer<float4> o : register(u0);
float4 vs(uint id : SV_VertexID) : SV_Position {
  return float4(float2((id << 1) & 2, id & 2) * float2(2, -2) + float2(-1, 1), 0, 1);
}
// op 0: SampleLevel, 1: SampleGrad, 2: Sample, 3: SampleBias, 4: SampleCmpLevelZero, 5: SampleLevel with an offset,
// 6 and 7: four SampleLevels or Samples in a row, each pixel storing one: the second through the same sampler, the third
// reading the first, the fourth through sampler extra.z
#define SAMPLE(T, U, DX, DY)                                                                                        \
  if (c.op == 0)                                                                                                     \
    v = T.SampleLevel(SAMPLER, U, c.extra.x);                                                                        \
  else if (c.op == 1)                                                                                                \
    v = T.SampleGrad(SAMPLER, U, DX, DY);                                                                            \
  else if (c.op == 2)                                                                                                \
    v = T.Sample(SAMPLER, U);                                                                                        \
  else                                                                                                               \
    v = T.SampleBias(SAMPLER, U, c.extra.y);
void ps(float4 p : SV_Position) {
  Case c = cases[index];
  float2 q = floor(p.xy);
  float4 uv = c.uv + q.x * c.dx + q.y * c.dy;
  float4 v = -1;
  if (c.op == 4) {
    v = depth.SampleCmpLevelZero(cmp, uv.xy, c.extra.z);
  } else if (c.op == 5) {
    v = mipped.SampleLevel(SAMPLER, uv.xy, c.extra.x, int2(1, -1));
  } else if (c.op >= 6) {
    uint other = uint(c.extra.z), k = uint(q.y) * 2 + uint(q.x);
    float4 r0, r1, r2, r3;
    if (c.op == 6) {
      r0 = mipped.SampleLevel(SAMPLER, uv.xy, c.extra.x);
      r1 = mipped.SampleLevel(SAMPLER, uv.xy + float2(0, 0.25), c.extra.x);
      r2 = mipped.SampleLevel(SAMPLER, uv.xy + r0.xy / 1024, c.extra.x);
      r3 = mipped.SampleLevel(s[other], uv.xy + float2(0.25, 0), c.extra.x);
    } else {
      r0 = mipped.Sample(SAMPLER, uv.xy);
      r1 = mipped.Sample(SAMPLER, uv.xy + float2(0, 0.25));
      r2 = mipped.Sample(SAMPLER, uv.xy + float2(0.125, 0.125));
      r3 = mipped.Sample(s[other], uv.xy + float2(0.25, 0));
    }
    v = k == 0 ? r0 : k == 1 ? r1 : k == 2 ? r2 : r3;
  } else if (c.image == 0) {
    SAMPLE(two, uv.xy, c.dx.xy, c.dy.xy)
  } else if (c.image == 1) {
    SAMPLE(four, uv.xy, c.dx.xy, c.dy.xy)
  } else if (c.image == 2) {
    SAMPLE(mipped, uv.xy, c.dx.xy, c.dy.xy)
  } else if (c.image == 3) {
    SAMPLE(row, uv.x, c.dx.x, c.dy.x)
  } else if (c.image == 4) {
    SAMPLE(layers, float3(uv.xy, c.extra.w), c.dx.xy, c.dy.xy)
  } else if (c.image == 5) {
    SAMPLE(volume, uv.xyz, c.dx.xyz, c.dy.xyz)
  } else {
    SAMPLE(cube, uv.xyz, c.dx.xyz, c.dy.xyz)
  }
  o[index * 4 + uint(q.y) * 2 + uint(q.x)] = v;
}
)hlsl";

// the D3D12 resource of each oracle image
struct Resource {
  D3D12_RESOURCE_DIMENSION dimension;
  D3D12_SRV_DIMENSION view;
  DXGI_FORMAT format;
};

struct Case {
  UINT op, image, sampler, variant; // variant: 0 the descriptor table's samplers, 1-3 static smin, smax, splain
  float uv[4], dx[4], dy[4], extra[4];
};

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const char *variants[] = {"s[c.slot]", "smin", "smax", "splain"};
  step("compile the sampling shaders");
  auto vs = compiler.compile(hlsl, "vs", "vs", {"SAMPLERS=" + std::to_string(kSamplers), "SAMPLER=smin"});
  std::string ps[4];
  for (UINT i = 0; i < 4; i++) {
    ps[i] = compiler.compile(
        hlsl, "ps", "ps", {"SAMPLERS=" + std::to_string(kSamplers), std::string("SAMPLER=") + variants[i]}
    );
    if (!expect(!ps[i].empty(), "the pixel shader with %s did not compile", variants[i]))
      return verdict();
  }
  if (!expect(!vs.empty(), "HLSL did not compile"))
    return verdict();

  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));

  // the descriptor samplers; the static ones take registers 0-2 of space 1
  constexpr auto clamp = Clamp;
  auto same = [](D3D12_FILTER filter, Address mode, float4 border = {}) {
    return Spec{filter, {mode, mode, mode}, border};
  };
  Spec specs[kSamplers] = {
      same(D3D12_FILTER_MINIMUM_MIN_MAG_MIP_LINEAR, clamp),
      same(D3D12_FILTER_MAXIMUM_MIN_MAG_MIP_LINEAR, clamp),
      same(D3D12_FILTER_MINIMUM_MIN_MAG_MIP_POINT, clamp),
      same(D3D12_FILTER_MAXIMUM_MIN_MAG_MIP_POINT, clamp),
      same(D3D12_FILTER_MIN_MAG_MIP_LINEAR, clamp),
      same(D3D12_FILTER_MINIMUM_MIN_LINEAR_MAG_MIP_POINT, clamp),
      same(D3D12_FILTER_MAXIMUM_MIN_POINT_MAG_LINEAR_MIP_POINT, clamp),
  };
  // 7-18: minimum then maximum per address mode; a custom border color, then Metal's opaque black
  const Address modes[] = {Wrap, Mirror, Clamp, Border, MirrorOnce, Border};
  for (UINT m = 0; m < std::size(modes); m++)
    for (UINT k = 0; k < 2; k++)
      specs[7 + 2 * m + k] = same(
          k ? D3D12_FILTER_MAXIMUM_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MINIMUM_MIN_MAG_MIP_LINEAR, modes[m],
          m == 3 ? float4{7, 0, 0, 1} : float4{0, 0, 0, 1}
      );
  specs[19] = same(D3D12_FILTER_MINIMUM_MIN_MAG_LINEAR_MIP_POINT, clamp);
  specs[20] = same(D3D12_FILTER_MAXIMUM_MIN_MAG_LINEAR_MIP_POINT, Wrap);
  const Spec statics[3] = {
      same(D3D12_FILTER_MINIMUM_MIN_MAG_MIP_LINEAR, clamp),
      same(D3D12_FILTER_MAXIMUM_MIN_MAG_MIP_LINEAR, Wrap),
      same(D3D12_FILTER_MIN_MAG_MIP_LINEAR, clamp),
  };
  const Spec compare = same(D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, clamp);

  const Image images[kImages] = {
      {Image::Plane, 2, 1, 1, 1, 1, 0, 1}, {Image::Plane, 4, 1, 1, 1, 1, 1, 1}, {Image::Plane, 8, 8, 1, 1, 4, 2, 4},
      {Image::Line, 8, 1, 1, 1, 3, 2, 4},  {Image::Plane, 4, 4, 1, 3, 3, 2, 4}, {Image::Volume, 4, 4, 4, 1, 3, 2, 4},
      {Image::Cube, 4, 4, 1, 6, 3, 2, 4},  {Image::Plane, 2, 1, 1, 1, 1, 0, 1},
  };
  constexpr auto rgba = DXGI_FORMAT_R32G32B32A32_FLOAT;
  const Resource resources[kImages] = {
      {D3D12_RESOURCE_DIMENSION_TEXTURE2D, D3D12_SRV_DIMENSION_TEXTURE2D, DXGI_FORMAT_R32_FLOAT},
      {D3D12_RESOURCE_DIMENSION_TEXTURE2D, D3D12_SRV_DIMENSION_TEXTURE2D, DXGI_FORMAT_R32_FLOAT},
      {D3D12_RESOURCE_DIMENSION_TEXTURE2D, D3D12_SRV_DIMENSION_TEXTURE2D, rgba},
      {D3D12_RESOURCE_DIMENSION_TEXTURE1D, D3D12_SRV_DIMENSION_TEXTURE1D, rgba},
      {D3D12_RESOURCE_DIMENSION_TEXTURE2D, D3D12_SRV_DIMENSION_TEXTURE2DARRAY, rgba},
      {D3D12_RESOURCE_DIMENSION_TEXTURE3D, D3D12_SRV_DIMENSION_TEXTURE3D, rgba},
      {D3D12_RESOURCE_DIMENSION_TEXTURE2D, D3D12_SRV_DIMENSION_TEXTURECUBE, rgba},
      {D3D12_RESOURCE_DIMENSION_TEXTURE2D, D3D12_SRV_DIMENSION_TEXTURE2D, DXGI_FORMAT_R32_TYPELESS},
  };

  // the cases, each one 2x2 quad drawn alone
  std::vector<Case> cases;
  auto add = [&](UINT variant, UINT op, UINT image, UINT sampler, float4 uv, float4 dx = {}, float4 dy = {},
                 float4 extra = {}) {
    Case c{op, image, sampler, variant};
    std::copy_n(uv.data(), 4, c.uv), std::copy_n(dx.data(), 4, c.dx), std::copy_n(dy.data(), 4, c.dy);
    std::copy_n(extra.data(), 4, c.extra);
    cases.push_back(c);
  };
  // the 2x1 {0, 1}: its shared edge, each texel's centre, either side of the edge
  for (UINT sampler : {0, 1, 2, 3, 4})
    for (float u : {0.5f, 0.25f, 0.75f, 0.4f, 0.6f})
      add(0, 0, 0, sampler, {u, 0.5f});
  // the 4x1 {2, 3, 4, 5} at each address mode's edges and past them
  for (UINT sampler = 7; sampler < 19; sampler++)
    for (float u : {0.0f, 1.0f, -0.25f, 1.25f, 0.9f})
      add(0, 0, 1, sampler, {u, 0.5f});
  // the mipped texture at whole and fractional LODs, past its last mip and below 0
  const UINT mip_samplers[] = {0, 1, 2, 3, 5, 6, 19, 20};
  for (UINT sampler : mip_samplers)
    for (float4 uv : {float4{0.5f, 0.5f}, float4{0.3f, 0.7f}, float4{0.0625f, 0.9375f}, float4{0.97f, 0.02f}})
      for (float level : {0.0f, 0.5f, 1.0f, 1.25f, 2.75f, 3.0f, 5.0f, -1.0f})
        add(0, 0, 2, sampler, uv, {}, {}, {level});
  // its gradients, its quad's derivatives and a bias: one texel of the first mip a pixel is a LOD of 0
  for (UINT sampler : {0, 1, 5, 6, 19, 20})
    for (float texels : {2.0f, 3.0f, 0.5f, 16.0f})
      for (UINT op : {1, 2, 3}) {
        float step = texels / 8;
        add(0, op, 2, sampler, {0.37f, 0.61f}, {step}, {0, step}, {0, 1});
      }
  // an immediate offset of (1, -1) texels
  for (UINT sampler : {0, 1, 7, 8})
    for (float level : {0.0f, 1.0f, 0.5f})
      add(0, 5, 2, sampler, {0.3f, 0.7f}, {}, {}, {level});
  // 1D, a 2D array's second and third layers, 3D (and its W address at w = 0), cube faces away from their edges
  for (UINT sampler : {0, 1, 7, 8, 2, 3}) {
    for (float u : {0.5f, 0.3f, 0.0f})
      for (float level : {0.0f, 0.5f, 1.5f})
        add(0, 0, 3, sampler, {u}, {}, {}, {level});
    add(0, 2, 3, sampler, {0.3f}, {2.0f / 8}, {}, {});
    for (float layer : {1.0f, 2.0f})
      for (float level : {0.0f, 0.5f, 1.0f})
        add(0, 0, 4, sampler, {0.4f, 0.6f}, {}, {}, {level, 0, 0, layer});
    for (float4 uvw : {float4{0.4f, 0.6f, 0.3f}, float4{0.4f, 0.6f, 0.0f}, float4{0.9f, 0.05f, 0.8f}})
      for (float level : {0.0f, 0.5f})
        add(0, 0, 5, sampler, uvw, {}, {}, {level});
  }
  for (UINT sampler : {0, 1, 2, 3})
    for (float4 dir :
         {float4{1, -0.1f, 0.2f}, float4{-1, 0.15f, 0.1f}, float4{0.2f, 1, -0.1f}, float4{-0.1f, -1, 0.15f},
          float4{0.15f, 0.2f, 1}, float4{0.1f, -0.15f, -1}})
      for (float level : {0.0f, 0.5f, 1.0f})
        add(0, 0, 6, sampler, dir, {}, {}, {level});
  // a comparison filter: LESS of 0.5 against {0, 1} at the edge and at the first texel's centre
  for (float u : {0.5f, 0.25f})
    add(0, 4, 7, 0, {u, 0.5f}, {}, {}, {0, 0, 0.5f});
  // runs of samples whose branches the converter joins where they have one sampler (MergeMinMaxBranchesPass): one
  // that reduces beside one that averages, either way round, and two that reduce differently
  for (float4 slots : {float4{0, 4}, float4{4, 1}, float4{1, 0}})
    for (float level : {0.5f, 1.0f}) {
      add(0, 6, 2, UINT(slots[0]), {0.3f, 0.4f}, {}, {}, {level, 0, slots[1]});
      add(0, 7, 2, UINT(slots[0]), {0.3f, 0.4f}, {level * 2 / 8}, {0, level * 2 / 8}, {0, 0, slots[1]});
    }
  // static samplers
  for (UINT variant = 1; variant < 4; variant++) {
    add(variant, 0, 0, 0, {0.5f, 0.5f});
    add(variant, 0, 1, 0, {0.0f, 0.5f});
    if (variant < 3) {
      add(variant, 0, 2, 0, {0.3f, 0.7f}, {}, {}, {0.5f});
      add(variant, 2, 2, 0, {0.37f, 0.61f}, {3.0f / 8}, {0, 3.0f / 8});
    }
  }
  const UINT count = UINT(cases.size());

  // the contract's own cases, before the GPU: the shared edge reads 0 and 1, a texel's centre only that texel
  {
    const float edge[3] = {0.5f, 0.5f}, centre[3] = {0.25f, 0.5f};
    const int none[3] = {};
    expect(
        reference(images[0], specs[0], edge, 0, 0, none)[0] == 0 &&
            reference(images[0], specs[1], edge, 0, 0, none)[0] == 1,
        "the oracle's edge of a 2x1 {0, 1} is not 0 and 1"
    );
    expect(reference(images[0], specs[1], centre, 0, 0, none)[0] == 0, "the oracle counts a texel weighted 0");
  }

  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  ComPtr<ID3D12Resource> textures[kImages];
  std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints[kImages];
  UINT64 upload_size = 0;
  for (UINT i = 0; i < kImages; i++) {
    auto &image = images[i];
    auto &resource = resources[i];
    bool depth = i == 7;
    D3D12_RESOURCE_DESC desc{
        resource.dimension,
        0,
        image.width,
        image.height,
        UINT16(image.shape == Image::Volume ? image.depth : image.layers),
        UINT16(image.mips),
        resource.format,
        {1, 0},
        D3D12_TEXTURE_LAYOUT_UNKNOWN,
        depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_NONE
    };
    CHECK(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&textures[i])
    ));
    if (depth)
      continue;
    UINT subs = image.mips * (image.shape == Image::Volume ? 1 : image.layers);
    footprints[i].resize(subs);
    UINT64 bytes;
    device->GetCopyableFootprints(&desc, 0, subs, upload_size, footprints[i].data(), nullptr, nullptr, &bytes);
    upload_size = (footprints[i][0].Offset + bytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) /
                  D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
  }
  const UINT64 cases_at = upload_size;
  upload_size += sizeof(Case) * count;
  const UINT64 out_size = sizeof(float4) * 4 * count;
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, upload_size + out_size, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto out = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, out_size, D3D12_RESOURCE_STATE_COPY_DEST,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, out_size, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(upload && out && readback, "buffers were not created"))
    return verdict();
  CHECK(forget(readback.Get()));
  char *mapped;
  CHECK(upload->Map(0, nullptr, (void **)&mapped));
  for (UINT i = 0; i < kImages; i++) {
    auto &image = images[i];
    for (UINT sub = 0; sub < footprints[i].size(); sub++) {
      auto &fp = footprints[i][sub];
      UINT mip = sub % image.mips, layer = sub / image.mips;
      for (UINT z = 0; z < fp.Footprint.Depth; z++)
        for (UINT y = 0; y < fp.Footprint.Height; y++)
          for (UINT x = 0; x < fp.Footprint.Width; x++) {
            auto at = (float *)(mapped + fp.Offset + (z * fp.Footprint.Height + y) * fp.Footprint.RowPitch) +
                      x * image.channels;
            for (UINT c = 0; c < image.channels; c++)
              at[c] = image.value(layer, mip, x, y, z, c);
          }
    }
  }
  memcpy(mapped + cases_at, cases.data(), sizeof(Case) * count);
  // what the shader writes starts as NaN, which no case reads
  memset(mapped + upload_size, 0xff, out_size);

  // views: the textures, then the cases
  ComPtr<ID3D12DescriptorHeap> views, samplers, dsvs;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kImages + 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
  };
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&views)));
  heap_desc = {D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplers + 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&samplers)));
  heap_desc = {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&dsvs)));
  auto at = [&](ID3D12DescriptorHeap *h, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT i) {
    return D3D12_CPU_DESCRIPTOR_HANDLE{
        h->GetCPUDescriptorHandleForHeapStart().ptr + i * device->GetDescriptorHandleIncrementSize(type)
    };
  };
  for (UINT i = 0; i < kImages; i++) {
    auto &image = images[i];
    auto &resource = resources[i];
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{
        i == 7 ? DXGI_FORMAT_R32_FLOAT : resource.format, resource.view, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
    };
    switch (resource.view) {
    case D3D12_SRV_DIMENSION_TEXTURE1D:
      srv.Texture1D.MipLevels = image.mips;
      break;
    case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
      srv.Texture2DArray = {0, image.mips, 0, image.layers};
      break;
    case D3D12_SRV_DIMENSION_TEXTURE3D:
      srv.Texture3D.MipLevels = image.mips;
      break;
    case D3D12_SRV_DIMENSION_TEXTURECUBE:
      srv.TextureCube.MipLevels = image.mips;
      break;
    default:
      srv.Texture2D.MipLevels = image.mips;
      break;
    }
    device->CreateShaderResourceView(
        textures[i].Get(), &srv, at(views.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, i)
    );
  }
  D3D12_SHADER_RESOURCE_VIEW_DESC structured{
      DXGI_FORMAT_UNKNOWN, D3D12_SRV_DIMENSION_BUFFER, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
  };
  structured.Buffer = {cases_at / sizeof(Case), count, sizeof(Case)};
  device->CreateShaderResourceView(
      upload.Get(), &structured, at(views.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kImages)
  );
  if (!expect(cases_at % sizeof(Case) == 0, "the cases are not at a whole element"))
    return verdict();
  auto desc_of = [](const Spec &spec) {
    D3D12_SAMPLER_DESC d{
        D3D12_FILTER(spec.filter),
        D3D12_TEXTURE_ADDRESS_MODE(spec.address[0]),
        D3D12_TEXTURE_ADDRESS_MODE(spec.address[1]),
        D3D12_TEXTURE_ADDRESS_MODE(spec.address[2]),
        0,
        1,
        D3D12_COMPARISON_FUNC_LESS
    };
    std::copy_n(spec.border.data(), 4, d.BorderColor);
    d.MaxLOD = D3D12_FLOAT32_MAX;
    return d;
  };
  for (UINT i = 0; i < kSamplers; i++) {
    auto d = desc_of(specs[i]);
    device->CreateSampler(&d, at(samplers.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, i));
  }
  auto d = desc_of(compare);
  device->CreateSampler(&d, at(samplers.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplers));
  D3D12_STATIC_SAMPLER_DESC fixed_samplers[3];
  for (UINT i = 0; i < 3; i++) {
    auto &s = statics[i];
    fixed_samplers[i] = {
        D3D12_FILTER(s.filter),
        D3D12_TEXTURE_ADDRESS_MODE(s.address[0]),
        D3D12_TEXTURE_ADDRESS_MODE(s.address[1]),
        D3D12_TEXTURE_ADDRESS_MODE(s.address[2]),
        0,
        1,
        D3D12_COMPARISON_FUNC_LESS,
        D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK,
        0,
        D3D12_FLOAT32_MAX,
        i,
        1,
        D3D12_SHADER_VISIBILITY_PIXEL
    };
  }

  D3D12_DESCRIPTOR_RANGE ranges[] = {
      {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kImages + 1, 0, 0, 0},
      {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, kSamplers, 0, 0, 0},
      {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0, 3, kSamplers},
  };
  D3D12_ROOT_PARAMETER params[4] = {
      {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS},
      {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
      {D3D12_ROOT_PARAMETER_TYPE_UAV},
      {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}
  };
  params[0].Constants = {0, 0, 1};
  params[1].DescriptorTable = {1, &ranges[0]};
  params[2].Descriptor = {0, 0};
  params[3].DescriptorTable = {2, &ranges[1]};
  auto root = root_signature(device.Get(), {4, params, 3, fixed_samplers});
  if (!expect(bool(root), "root signature was not created"))
    return verdict();
  ComPtr<ID3D12PipelineState> pipelines[4];
  for (UINT i = 0; i < 4; i++) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = root.Get();
    pso_desc.VS = bytecode(vs);
    pso_desc.PS = bytecode(ps[i]);
    pso_desc.SampleMask = ~0u;
    pso_desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.SampleDesc = {1, 0};
    CHECK(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pipelines[i])));
  }

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  step("upload the textures and clear the 2x1 depth texture to {0, 1}");
  for (UINT i = 0; i < kImages; i++)
    for (UINT sub = 0; sub < footprints[i].size(); sub++) {
      D3D12_TEXTURE_COPY_LOCATION dst{
          textures[i].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = sub}
      },
          src{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprints[i][sub]}};
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
  D3D12_DEPTH_STENCIL_VIEW_DESC dsv{DXGI_FORMAT_D32_FLOAT, D3D12_DSV_DIMENSION_TEXTURE2D};
  device->CreateDepthStencilView(textures[7].Get(), &dsv, at(dsvs.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 0));
  for (LONG x = 0; x < 2; x++) {
    D3D12_RECT rect{x, 0, x + 1, 1};
    list->ClearDepthStencilView(
        at(dsvs.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 0), D3D12_CLEAR_FLAG_DEPTH, float(x), 0, 1, &rect
    );
  }
  list->CopyBufferRegion(out.Get(), 0, upload.Get(), upload_size, out_size);
  for (UINT i = 0; i < kImages; i++)
    transition(
        list.Get(), textures[i].Get(), i == 7 ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
    );
  transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  step("draw %u cases", count);
  CHECK(allocator->Reset());
  CHECK(list->Reset(allocator.Get(), nullptr));
  ID3D12DescriptorHeap *heaps[] = {views.Get(), samplers.Get()};
  list->SetDescriptorHeaps(2, heaps);
  list->SetGraphicsRootSignature(root.Get());
  list->SetGraphicsRootDescriptorTable(1, views->GetGPUDescriptorHandleForHeapStart());
  list->SetGraphicsRootUnorderedAccessView(2, out->GetGPUVirtualAddress());
  list->SetGraphicsRootDescriptorTable(3, samplers->GetGPUDescriptorHandleForHeapStart());
  D3D12_VIEWPORT viewport{0, 0, 2, 2, 0, 1};
  D3D12_RECT scissor{0, 0, 2, 2};
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  for (UINT i = 0; i < count; i++) {
    list->SetPipelineState(pipelines[cases[i].variant].Get());
    list->SetGraphicsRoot32BitConstant(0, i, 0);
    list->DrawInstanced(3, 1, 0, 0);
  }
  transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(), 0, out.Get(), 0, out_size);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  upload->Unmap(0, nullptr);

  const char *ops[] = {"SampleLevel",        "SampleGrad",         "Sample",          "SampleBias",
                       "SampleCmpLevelZero", "SampleLevel+offset", "SampleLevel run", "Sample run"};
  const char *kinds[kImages] = {"2x1", "4x1", "mipped", "1D", "2D array", "3D", "cube", "depth"};
  float4 *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  UINT wrong = 0;
  for (UINT i = 0; i < count; i++) {
    auto &c = cases[i];
    auto &image = images[c.image];
    for (UINT p = 0; p < 4; p++) {
      // a run's last pixel stores the sample through its other sampler
      bool other = c.op >= 6 && p == 3;
      auto &spec = c.op == 4   ? compare
                   : other     ? specs[UINT(c.extra[2])]
                   : c.variant ? statics[c.variant - 1]
                               : specs[c.sampler];
      bool reduces = spec.reduces();
      float qx = float(p & 1), qy = float(p >> 1);
      float coord[3];
      for (UINT a = 0; a < 3; a++)
        coord[a] = c.uv[a] + qx * c.dx[a] + qy * c.dy[a];
      float4 want;
      if (c.op == 4) {
        // LESS: the reference below the texel passes; a linear filter averages the results (7.18.15)
        float u = minmax_filters::fixed(coord[0] * 2 - 0.5f), f = u - std::floor(u);
        float left = c.extra[2] < image.value(0, 0, std::clamp(int(std::floor(u)), 0, 1), 0, 0, 0) ? 1.0f : 0.0f;
        float right = c.extra[2] < image.value(0, 0, std::clamp(int(std::floor(u)) + 1, 0, 1), 0, 0, 0) ? 1.0f : 0.0f;
        want = {left * (1 - f) + right * f};
      } else {
        float lod = c.extra[0];
        if (c.op == 1 || c.op == 2 || c.op == 3 || c.op == 7)
          lod = gradient_lod(image, c.dx, c.dy) + (c.op == 3 ? c.extra[1] : 0);
        if (c.op >= 6) {
          // the run's samples: the second 0.25 lower, the third moved by the first's red and green over 1024 (or by
          // 0.125 when they are implicit), the fourth 0.25 to the right
          const int none[3] = {};
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
        // the shader's immediate offset, in texels of each mip read
        const int none[3] = {}, offset[3] = {1, -1, 0};
        want = reference(image, spec, coord, UINT(c.extra[3]), lod, c.op == 5 ? offset : none);
      }
      auto &value = got[4 * i + p];
      bool ok = true;
      for (UINT k = 0; k < image.channels; k++)
        ok &= reduces ? value[k] == want[k] : std::fabs(value[k] - want[k]) <= 1.0f / 256;
      if (!ok && wrong++ < 64) {
        step(
            "case %u: %s of the %s texture through %s %u (filter 0x%x), pixel %u, coordinate (%g, %g, %g), LOD %g", i,
            ops[c.op], kinds[c.image], c.variant ? "static sampler" : "descriptor sampler",
            c.variant ? c.variant - 1 : c.sampler, spec.filter, p, coord[0], coord[1], coord[2], c.extra[0]
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
  readback->Unmap(0, nullptr);
  return verdict();
}
