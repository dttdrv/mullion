// contract: sampler feedback (DirectX-Specs, Sampler Feedback). a feedback map records, per mip region of its paired
// texture, what a sample would read with its sampler ("General interpretation and usage"), which D3D11.3 7.18 says:
// the nearest mip or the two around the LOD (7.18.10), and in each the texel a point filter reads or the two by two
// of a linear one (7.18.7, 7.18.8), by the magnification filter at a LOD of 0 or less and the minification filter
// above (7.18.11), placed by the address modes after the coordinate's range is reduced (7.18.6, 7.18.9), so a
// coordinate whole tiles away reads the same texels. an anisotropic filter takes its LOD from the footprint's width
// and reads along its length (7.18.11); how many taps is the implementation's choice, here as many as the ratio,
// evenly spaced. a mip's regions are a grid of its own over the whole map, which matters where the sizes do not
// divide ("Example of non-power-of-two feedback maps behavior"): the array's map is 9 by 9 regions.
// a MipRegionUsed map decodes to 0xff or 0 per region of each mip, from the LOD after the sampler's clamps; a MinMip
// map decodes to the most detailed mip wanted in each region of the first mip or 0xff, from the LOD before them
// ("Interpretation"). this implementation gives a first-mip region the mips whose region lies over its center, the
// first of the two results "Decoded representation" allows.
// WriteSamplerFeedbackLevel and Grad run in a compute shader over an array, per address mode pair; WriteSamplerFeedback
// and Bias in a pixel shader with its derivatives. a map bound without a paired texture records nothing, a clear
// forgets, a MinMip map also decodes to a buffer, and what was decoded encodes back to the same map ("Transcoding").
// a map is of its paired resource, whatever view of it a sample goes through (tier 1.0): through a view of some of
// its mips and one of its slices, with a least LOD of its own, a sample wants the resource's mips and slice that the
// view's are. the view is one more clamp, which a MinMip map does not take: it holds the mip wanted "with no mip
// level clamping applied", "the 'ideal' most-detailed mip level", of all the mips the resource has, more detailed
// or less than the view's.
#include "d3d12_test.hpp"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <set>

static const char hlsl[] = R"hlsl(
Texture2DArray<float4> paired : register(t0);
Texture2D<float4> flat : register(t1);
SamplerState s : register(s0);
FeedbackTexture2DArray<SAMPLER_FEEDBACK_MIN_MIP> min_mip : register(u0);
FeedbackTexture2DArray<SAMPLER_FEEDBACK_MIP_REGION_USED> used : register(u1);
FeedbackTexture2D<SAMPLER_FEEDBACK_MIN_MIP> flat_min_mip : register(u2);
FeedbackTexture2D<SAMPLER_FEEDBACK_MIP_REGION_USED> flat_used : register(u3);

float3 coord(uint t) { return float3(float2(t % COLUMNS, t / COLUMNS) * STEP + START, (t / 3) % SLICES); }
float lod(uint t) { return (t % LODS) * 0.5; }
[numthreads(1, 1, 1)]
void cs_level(uint3 id : SV_DispatchThreadID) {
  min_mip.WriteSamplerFeedbackLevel(paired, s, coord(id.x), lod(id.x));
  used.WriteSamplerFeedbackLevel(paired, s, coord(id.x), lod(id.x));
}
// samples whole tiles away along u, where a float has no bits left for a texel: at a tile's edge, or FAR_ODD tiles
// on, where a mirrored texture is turned around (FAR is even). scaled by the texture's size before the tiles are
// taken off, the second would round into another region
float3 far_coord(uint t) { return float3(t % 2 ? FAR + FAR_ODD : -FAR, (t / 2) * 0.5 + 0.25, t % SLICES); }
[numthreads(1, 1, 1)]
void cs_far(uint3 id : SV_DispatchThreadID) {
  min_mip.WriteSamplerFeedbackLevel(paired, s, far_coord(id.x), 0);
  used.WriteSamplerFeedbackLevel(paired, s, far_coord(id.x), 0);
}
// gradients along the axes: 2^(lod + GRAD_LOD) texels along u, and RATIO times shorter along v
[numthreads(1, 1, 1)]
void cs_grad(uint3 id : SV_DispatchThreadID) {
  float l = lod(id.x) + GRAD_LOD;
  float2 dx = float2(exp2(l) / WIDTH, 0), dy = float2(0, exp2(l) / RATIO / HEIGHT);
  min_mip.WriteSamplerFeedbackGrad(paired, s, coord(id.x), dx, dy);
  used.WriteSamplerFeedbackGrad(paired, s, coord(id.x), dx, dy);
}
float4 vs(uint id : SV_VertexID) : SV_Position { return float4(float2(id & 1, id >> 1) * 4 - 1, 0, 1); }
float4 ps(float4 pos : SV_Position) : SV_Target {
  float2 uv = pos.xy / TARGET * float2(SCALE, SCALE * SKEW);
  flat_min_mip.WriteSamplerFeedback(flat, s, uv);
  flat_used.WriteSamplerFeedbackBias(flat, s, uv, BIAS);
  return 0;
}
)hlsl";

constexpr auto MIN_MIP = (DXGI_FORMAT)189, MIP_REGION_USED = (DXGI_FORMAT)190; // dxgiformat.h ends before them

struct Paired {
  UINT width, height, slices, mips, region_width, region_height;
  UINT size(UINT axis, UINT level) const { return std::max((axis ? height : width) >> level, 1u); }
  UINT grid(UINT axis) const {
    UINT region = axis ? region_height : region_width;
    return ((axis ? height : width) + region - 1) / region;
  }
  UINT regions(UINT axis, UINT level) const { return std::max(grid(axis) >> level, 1u); }
};

// per slice and mip, the regions wanted
using Wanted = std::vector<std::vector<std::set<std::pair<UINT, UINT>>>>;

// the texel an address mode reads for texel `i` of `n`, or false for the border (D3D11.3 3.3.11)
static bool
address(int &i, int n, D3D12_TEXTURE_ADDRESS_MODE mode) {
  switch (mode) {
  case D3D12_TEXTURE_ADDRESS_MODE_WRAP:
    i = (i % n + n) % n;
    return true;
  case D3D12_TEXTURE_ADDRESS_MODE_MIRROR:
    i = (i % (2 * n) + 2 * n) % (2 * n);
    i = i < n ? i : 2 * n - 1 - i;
    return true;
  case D3D12_TEXTURE_ADDRESS_MODE_BORDER:
    return i >= 0 && i < n;
  case D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE:
    i = i < 0 ? -1 - i : i;
    [[fallthrough]];
  default:
    i = std::clamp(i, 0, n - 1);
    return true;
  }
}

// a normalized coordinate with its range reduced (D3D11.3 7.18.6)
static float
reduce(float u, D3D12_TEXTURE_ADDRESS_MODE mode) {
  switch (mode) {
  case D3D12_TEXTURE_ADDRESS_MODE_WRAP:
    return u - std::floor(u);
  case D3D12_TEXTURE_ADDRESS_MODE_MIRROR:
    return (u / 2 - std::trunc(u / 2)) * 2;
  default:
    return std::clamp(u, -10.0f, 10.0f);
  }
}

struct Sampler {
  D3D12_TEXTURE_ADDRESS_MODE modes[2];
  float min_lod;
  bool bound; // whether the maps are bound with their paired texture
  D3D12_FILTER filter;
  UINT anisotropy;
  bool narrow; // whether the texture is sampled through the view of a part of it
};

// the part of a texture a view shows: its first mip and how many, its first slice and how many, and the least LOD
// its samples take, counted in the texture's mips (D3D12_TEX2D_ARRAY_SRV)
struct View {
  UINT first_mip, mips, first_slice, slices;
  float min_lod;
};

// what the filter makes of derivatives in texels of the first mip (D3D11.3 7.18.11): the LOD, the ratio of
// anisotropy and the line of anisotropy, as long as the longer derivative
struct Footprint {
  float lod, ratio, line[2];
};
static Footprint
footprint(const float dx[2], const float dy[2], const Sampler &s) {
  float max_ratio = D3D12_DECODE_IS_ANISOTROPIC_FILTER(s.filter) ? s.anisotropy : 1;
  float x2 = dx[0] * dx[0] + dx[1] * dx[1], y2 = dy[0] * dy[0] + dy[1] * dy[1];
  float determinant = std::abs(dx[0] * dy[1] - dx[1] * dy[0]);
  bool major_x = x2 > y2;
  float major2 = major_x ? x2 : y2, major = std::sqrt(major2), ratio = major2 / determinant, minor;
  if (ratio > max_ratio) {
    ratio = max_ratio;
    minor = major / ratio;
  } else {
    minor = determinant / major;
  }
  if (minor < 1)
    ratio = std::max(1.0f, ratio * minor);
  return {std::log2(minor), ratio, {major_x ? dx[0] : dy[0], major_x ? dx[1] : dy[1]}};
}

// what a sample at `lod` wants through a view, whose mips the LOD and the footprint count, with a footprint's ratio
// and line where it has derivatives: of the view's mips, or (`ideal`) of the resource's. `margin` is how far it
// stays from where rounding would decide otherwise: its texel coordinates from a texel boundary, its LOD from a mip
// boundary and its ratio from another count of taps. values that are exact, as a LOD given in halves is, have none
// to keep, and neither has a sample whose coordinates are exact
static void
sample(
    Wanted &wanted, const Paired &p, const View &view, float u, float v, UINT slice, float lod, const Sampler &s,
    const Footprint *f, float *margin, bool ideal
) {
  bool mip_linear = D3D12_DECODE_MIP_FILTER(s.filter), anisotropic = D3D12_DECODE_IS_ANISOTROPIC_FILTER(s.filter);
  // the filter is chosen on the LOD before it is kept within the mips there are
  bool linear = anisotropic || (lod <= 0 ? D3D12_DECODE_MAG_FILTER(s.filter) : D3D12_DECODE_MIN_FILTER(s.filter));
  // counted in the resource's mips from here on
  lod = ideal ? std::clamp(lod + view.first_mip, 0.0f, float(p.mips - 1))
              : std::clamp(lod, 0.0f, float(view.mips - 1)) + view.first_mip;
  // a slice past the view's is its last
  slice = std::min(slice, view.slices - 1) + view.first_slice;
  float nearest = lod + (mip_linear ? 0 : 0.5f);
  UINT levels[2] = {(UINT)std::floor(nearest), (UINT)std::ceil(lod)};
  UINT taps = f ? (UINT)std::ceil(f->ratio) : 1;
  if (margin) {
    *margin = std::min(*margin, std::abs(nearest - std::round(nearest)) + (nearest == std::round(nearest)));
    // a ratio that is a whole number is 1 or the sampler's largest, as given
    if (f)
      *margin = std::min(*margin, std::abs(f->ratio - std::round(f->ratio)) + (f->ratio == std::round(f->ratio)));
  }
  for (UINT tap = 0; tap < taps; tap++)
    for (UINT l = 0; l < (mip_linear ? 2u : 1u); l++)
      for (UINT corner = 0; corner < (linear ? 4u : 1u); corner++) {
        UINT level = levels[l], region[2];
        bool kept = true;
        for (UINT axis = 0; axis < 2; axis++) {
          int size = p.size(axis, level);
          float along = (tap + 0.5f) / taps - 0.5f, at = axis ? v : u;
          if (f)
            at += along * f->line[axis] / p.size(axis, view.first_mip);
          at = reduce(at, s.modes[axis]) * size - (linear ? 0.5f : 0);
          if (margin)
            *margin = std::min(*margin, std::abs(at - std::round(at)));
          int texel = (int)std::floor(at) + (corner >> axis & 1);
          kept &= address(texel, size, s.modes[axis]);
          // the mip's grid of regions lies over the whole mip
          region[axis] = std::min(UINT((texel + 0.5f) / size * p.regions(axis, level)), p.regions(axis, level) - 1);
        }
        if (kept)
          wanted[slice][level].insert({region[0], region[1]});
      }
}

static uint8_t
decoded_used(const Wanted &wanted, UINT slice, UINT level, UINT x, UINT y) {
  return wanted[slice][level].count({x, y}) ? 0xff : 0;
}

static uint8_t
decoded_min_mip(const Wanted &wanted, const Paired &p, UINT slice, UINT x, UINT y) {
  // the region of each mip over the first-mip region's center
  auto over = [&](UINT axis, UINT at, UINT level) { return (2 * at + 1) * p.regions(axis, level) / (2 * p.grid(axis)); };
  for (UINT level = 0; level < p.mips; level++)
    if (wanted[slice][level].count({over(0, x, level), over(1, y, level)}))
      return level;
  return 0xff;
}

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  // the array's sizes do not divide into its regions, and its mips halve to odd sizes
  const Paired array{68, 36, 2, 7, 8, 4}, flat{64, 32, 1, 7, 4, 4};
  const UINT columns = 8, cases = 64, lods = 15, target_size = 16, far_cases = 4;
  const float step = 0.47f, start = -1.3f, grad_lod = 0.25f, scale = 1.4142135f, bias = 1.0f, distant = 4194304, distant_odd = 3, ratio = 2.5f,
              skew = 0.8f;
  std::vector<std::string> defines = {
      "COLUMNS=" + std::to_string(columns), "STEP=" + std::to_string(step),         "START=" + std::to_string(start),
      "SLICES=" + std::to_string(array.slices), "LODS=" + std::to_string(lods),     "GRAD_LOD=" + std::to_string(grad_lod),
      "WIDTH=" + std::to_string(array.width),   "HEIGHT=" + std::to_string(array.height),
      "TARGET=" + std::to_string(target_size),  "SCALE=" + std::to_string(scale),   "BIAS=" + std::to_string(bias),
      "FAR=" + std::to_string(distant),         "FAR_ODD=" + std::to_string(distant_odd),
      "RATIO=" + std::to_string(ratio),         "SKEW=" + std::to_string(skew),
  };
  auto cs_level = compiler.compile(hlsl, "cs_level", "cs_6_5", defines), cs_far = compiler.compile(hlsl, "cs_far", "cs_6_5", defines),
       cs_grad = compiler.compile(hlsl, "cs_grad", "cs_6_5", defines), vs = compiler.compile(hlsl, "vs", "vs_6_5", defines),
       ps = compiler.compile(hlsl, "ps", "ps_6_5", defines);
  if (cs_level.empty() || cs_far.empty() || cs_grad.empty() || vs.empty() || ps.empty()) {
    printf("skipped: sampler feedback is shader model 6.5\n");
    return 77;
  }
  ComPtr<ID3D12Device8> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS7 options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options, sizeof(options)));
  if (options.SamplerFeedbackTier != D3D12_SAMPLER_FEEDBACK_TIER_1_0) {
    printf("failed: sampler feedback tier %d\n", options.SamplerFeedbackTier);
    return 1;
  }

  // the runs' samplers
  const D3D12_TEXTURE_ADDRESS_MODE W = D3D12_TEXTURE_ADDRESS_MODE_WRAP, M = D3D12_TEXTURE_ADDRESS_MODE_MIRROR,
                                   C = D3D12_TEXTURE_ADDRESS_MODE_CLAMP, B = D3D12_TEXTURE_ADDRESS_MODE_BORDER,
                                   O = D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
  const D3D12_FILTER linear = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  // the anisotropic samplers: one whose largest ratio the gradients stay under, and one they exceed
  const UINT anisotropic = 7, clamped_anisotropic = 8;
  const Sampler runs[] = {
      {{W, C}, 0, true, linear},
      {{M, B}, 0, true, linear},
      {{C, O}, 0, true, linear},
      {{B, W}, 0, true, linear},
      {{O, M}, 0, true, linear},
      {{C, C}, 2, true, linear},
      {{W, W}, 0, false, linear},
      {{W, M}, 0, true, D3D12_FILTER_ANISOTROPIC, 16},
      {{C, W}, 0, true, D3D12_FILTER_ANISOTROPIC, 2},
      {{W, C}, 0, true, D3D12_FILTER_MIN_MAG_MIP_POINT},
      {{M, W}, 0, true, D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT},
      {{B, M}, 0, true, D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR},
      {{C, B}, 1, true, D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR},
      {{W, C}, 0, true, linear, 0, true},
      {{M, B}, 1, true, D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT, 0, true},
      {{C, W}, 0, true, D3D12_FILTER_ANISOTROPIC, 16, true},
  };
  // the views of the array: all of it, and three of its mips from the second and its second slice, with a least LOD
  // between the view's first two mips
  const View whole{0, array.mips, 0, array.slices, 0}, narrow{1, 3, 1, 1, 1.5f}, flat_view{0, flat.mips, 0, flat.slices, 0};
  const UINT run_count = std::size(runs);
  if (ratio >= runs[anisotropic].anisotropy || ratio <= runs[clamped_anisotropic].anisotropy) {
    printf("failed: the gradients' ratio is not between the anisotropic samplers' largest\n");
    return 1;
  }

  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  auto texture = [&](const D3D12_RESOURCE_DESC1 &desc, D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource> &out) {
    return device->CreateCommittedResource2(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, nullptr, IID_PPV_ARGS(&out));
  };
  auto desc = [](UINT width, UINT height, UINT slices, UINT mips, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags) {
    return D3D12_RESOURCE_DESC1{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, (UINT16)slices, (UINT16)mips, format,
                                {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, flags};
  };
  auto map_desc = [&](const Paired &p, DXGI_FORMAT format) {
    auto d = desc(p.width, p.height, p.slices, p.mips, format, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    d.SamplerFeedbackMipRegion = {p.region_width, p.region_height, 0};
    return d;
  };
  // a decoded MipRegionUsed map has its paired texture's mips, so it is padded to a size that has them
  auto decoded_desc = [&](const Paired &p, bool mips) {
    return mips ? desc(p.width, p.height, p.slices, p.mips, DXGI_FORMAT_R8_UINT, D3D12_RESOURCE_FLAG_NONE)
                : desc(p.grid(0), p.grid(1), p.slices, 1, DXGI_FORMAT_R8_UINT, D3D12_RESOURCE_FLAG_NONE);
  };
  // by paired texture (array, flat) and kind (MinMip, MipRegionUsed)
  const Paired *paired[2] = {&array, &flat};
  ComPtr<ID3D12Resource> textures[2], maps[2][2], decoded[2][2], target;
  for (UINT t = 0; t < 2; t++) {
    auto &p = *paired[t];
    CHECK(texture(
        desc(p.width, p.height, p.slices, p.mips, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, textures[t]
    ));
    for (UINT kind = 0; kind < 2; kind++) {
      CHECK(texture(map_desc(p, kind ? MIP_REGION_USED : MIN_MIP), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, maps[t][kind]));
      CHECK(texture(decoded_desc(p, kind), D3D12_RESOURCE_STATE_RESOLVE_DEST, decoded[t][kind]));
    }
  }
  CHECK(texture(
      desc(target_size, target_size, 1, 1, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
      D3D12_RESOURCE_STATE_RENDER_TARGET, target
  ));
  // a map answers with the description it was created from, and a mip region must be a power of two
  auto asked = map_desc(array, MIN_MIP);
  auto got = maps[0][0]->GetDesc();
  if (got.Width != asked.Width || got.Height != asked.Height || got.MipLevels != asked.MipLevels ||
      got.Format != asked.Format) {
    printf("failed: a map's description is %ux%u, %u mips, format %u\n", (UINT)got.Width, got.Height, got.MipLevels, got.Format);
    return 1;
  }
  asked.SamplerFeedbackMipRegion.Width = 6;
  ComPtr<ID3D12Resource> invalid;
  if (SUCCEEDED(texture(asked, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, invalid))) {
    printf("failed: a map with a mip region 6 texels wide was created\n");
    return 1;
  }
  const UINT flat_regions = flat.grid(0) * flat.grid(1);
  auto decoded_buffer = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, flat_regions, D3D12_RESOURCE_STATE_RESOLVE_DEST);

  // descriptors: the maps with their paired textures, the textures, the maps without, then the textures again, the
  // array through its narrow view
  enum { UAVS = 0, SRVS = 4, UNPAIRED = 6, NARROW = 10, DESCRIPTORS = 12 };
  ComPtr<ID3D12DescriptorHeap> views, samplers, rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, DESCRIPTORS, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&views)));
  heap_desc = {D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, run_count, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&samplers)));
  heap_desc = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtvs)));
  auto view_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  auto sampler_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  auto cpu = [](ID3D12DescriptorHeap *h, UINT step, UINT i) {
    auto handle = h->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += step * i;
    return handle;
  };
  auto gpu = [](ID3D12DescriptorHeap *h, UINT step, UINT i) {
    auto handle = h->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += step * i;
    return handle;
  };
  D3D12_SHADER_RESOURCE_VIEW_DESC narrow_desc{DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_SRV_DIMENSION_TEXTURE2DARRAY,
                                              D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
  narrow_desc.Texture2DArray = {narrow.first_mip, narrow.mips, narrow.first_slice, narrow.slices, 0, narrow.min_lod};
  device->CreateShaderResourceView(textures[0].Get(), &narrow_desc, cpu(views.Get(), view_step, NARROW));
  device->CreateShaderResourceView(textures[1].Get(), nullptr, cpu(views.Get(), view_step, NARROW + 1));
  for (UINT t = 0; t < 2; t++) {
    device->CreateShaderResourceView(textures[t].Get(), nullptr, cpu(views.Get(), view_step, SRVS + t));
    for (UINT kind = 0; kind < 2; kind++) {
      device->CreateSamplerFeedbackUnorderedAccessView(
          textures[t].Get(), maps[t][kind].Get(), cpu(views.Get(), view_step, UAVS + 2 * t + kind)
      );
      device->CreateSamplerFeedbackUnorderedAccessView(
          nullptr, maps[t][kind].Get(), cpu(views.Get(), view_step, UNPAIRED + 2 * t + kind)
      );
    }
  }
  for (UINT r = 0; r < run_count; r++) {
    D3D12_SAMPLER_DESC sampler{runs[r].filter, runs[r].modes[0], runs[r].modes[1], D3D12_TEXTURE_ADDRESS_MODE_CLAMP};
    sampler.MaxAnisotropy = runs[r].anisotropy;
    sampler.MinLOD = runs[r].min_lod;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    device->CreateSampler(&sampler, cpu(samplers.Get(), sampler_step, r));
  }
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);

  D3D12_DESCRIPTOR_RANGE uav_range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 4}, srv_range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2},
      sampler_range{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1};
  D3D12_ROOT_PARAMETER params[3]{};
  params[0].DescriptorTable = {1, &uav_range};
  params[1].DescriptorTable = {1, &srv_range};
  params[2].DescriptorTable = {1, &sampler_range};
  auto rs = root_signature(device.Get(), {3, params});
  if (!rs) {
    printf("failed: root signature\n");
    return 1;
  }
  ComPtr<ID3D12PipelineState> level_pso, far_pso, grad_pso, draw_pso;
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute{rs.Get(), bytecode(cs_level)};
  CHECK(device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&level_pso)));
  compute.CS = bytecode(cs_far);
  CHECK(device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&far_pso)));
  compute.CS = bytecode(cs_grad);
  CHECK(device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&grad_pso)));
  D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{rs.Get(), bytecode(vs), bytecode(ps)};
  graphics.SampleMask = ~0u;
  graphics.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  graphics.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  graphics.NumRenderTargets = 1;
  graphics.RTVFormats[0] = DXGI_FORMAT_R8_UNORM;
  graphics.SampleDesc.Count = 1;
  CHECK(device->CreateGraphicsPipelineState(&graphics, IID_PPV_ARGS(&draw_pso)));

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList1> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));

  // what was decoded is read back a subresource per slot, each large enough for a first mip
  const UINT64 row = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, slot = row * array.width;
  const UINT slots = array.slices * (1 + array.mips);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, slot * slots, D3D12_RESOURCE_STATE_COPY_DEST);
  uint8_t *out;
  CHECK(readback->Map(0, nullptr, (void **)&out));
  UINT next_slot = 0;
  auto read_texture = [&](ID3D12Resource *res, UINT subresource, UINT width, UINT height) {
    D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
        src{res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    dst.PlacedFootprint = {slot * next_slot++, {DXGI_FORMAT_R8_UINT, width, height, 1, (UINT)row}};
    src.SubresourceIndex = subresource;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  };
  auto clear = [&](UINT t, UINT kind) {
    const UINT values[4] = {7, 7, 7, 7}; // a map clears to "no mip wanted", whatever the values
    list->ClearUnorderedAccessViewUint(
        gpu(views.Get(), view_step, UAVS + 2 * t + kind), cpu(views.Get(), view_step, UAVS + 2 * t + kind),
        maps[t][kind].Get(), values, 0, nullptr
    );
  };
  auto transcode = [&](ID3D12Resource *dst, ID3D12Resource *src, D3D12_RESOLVE_MODE mode) {
    list->ResolveSubresourceRegion(dst, UINT_MAX, 0, 0, src, UINT_MAX, nullptr, DXGI_FORMAT_R8_UINT, mode);
  };
  // decodes both maps of a paired texture and reads the textures back: MinMip per slice, then MipRegionUsed per
  // slice and mip
  auto decode = [&](UINT t) {
    auto &p = *paired[t];
    next_slot = 0;
    for (UINT kind = 0; kind < 2; kind++) {
      transition(list.Get(), maps[t][kind].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
      transcode(decoded[t][kind].Get(), maps[t][kind].Get(), D3D12_RESOLVE_MODE_DECODE_SAMPLER_FEEDBACK);
      transition(list.Get(), maps[t][kind].Get(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      transition(list.Get(), decoded[t][kind].Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
      for (UINT slice = 0; slice < p.slices; slice++)
        for (UINT level = 0; level < (kind ? p.mips : 1); level++)
          read_texture(
              decoded[t][kind].Get(), slice * (kind ? p.mips : 1) + level, kind ? p.size(0, level) : p.grid(0),
              kind ? p.size(1, level) : p.grid(1)
          );
      transition(list.Get(), decoded[t][kind].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RESOLVE_DEST);
    }
  };
  unsigned failures = 0, wanted_regions = 0;
  // compares what decode read back with what the samples want of each kind of map
  auto compare = [&](const char *what, UINT t, const Wanted wanted[2]) {
    auto &p = *paired[t];
    UINT s = 0;
    for (UINT kind = 0; kind < 2; kind++)
      for (UINT slice = 0; slice < p.slices; slice++)
        for (UINT level = 0; level < (kind ? p.mips : 1); level++, s++)
          for (UINT y = 0; y < p.regions(1, level); y++)
            for (UINT x = 0; x < p.regions(0, level); x++) {
              uint8_t value = out[slot * s + row * y + x];
              uint8_t expected = kind ? decoded_used(wanted[kind], slice, level, x, y)
                                      : decoded_min_mip(wanted[kind], p, slice, x, y);
              wanted_regions += expected != (kind ? 0 : 0xff);
              if (value != expected && failures++ < 12)
                printf(
                    "%s: %s slice %u mip %u region %u,%u: %#x, want %#x\n", what, kind ? "MipRegionUsed" : "MinMip",
                    slice, level, x, y, value, expected
                );
            }
  };
  auto empty = [](const Paired &p) { return Wanted(p.slices, Wanted::value_type(p.mips)); };
  auto bind = [&](bool graphics, UINT uavs, UINT sampler, UINT srvs = SRVS) {
    ID3D12DescriptorHeap *heaps[] = {views.Get(), samplers.Get()};
    list->SetDescriptorHeaps(2, heaps);
    D3D12_GPU_DESCRIPTOR_HANDLE tables[] = {
        gpu(views.Get(), view_step, uavs), gpu(views.Get(), view_step, srvs), gpu(samplers.Get(), sampler_step, sampler)
    };
    graphics ? list->SetGraphicsRootSignature(rs.Get()) : list->SetComputeRootSignature(rs.Get());
    for (UINT i = 0; i < 3; i++)
      graphics ? list->SetGraphicsRootDescriptorTable(i, tables[i]) : list->SetComputeRootDescriptorTable(i, tables[i]);
  };
  auto run = [&]() -> HRESULT {
    HRESULT hr = submit(device.Get(), queue.Get(), list.Get());
    if (SUCCEEDED(hr) && SUCCEEDED(hr = allocator->Reset()))
      hr = list->Reset(allocator.Get(), nullptr);
    return hr;
  };

  float margin = 1;
  for (UINT r = 0; r < run_count; r++) {
    auto &test = runs[r];
    auto &view = test.narrow ? narrow : whole;
    // the least LOD of a sample that takes the clamps: the sampler's and the view's, in the view's mips
    const float least = std::max(test.min_lod, view.min_lod - view.first_mip);
    clear(0, 0);
    clear(0, 1);
    bind(false, test.bound ? UAVS : UNPAIRED, r, test.narrow ? NARROW : SRVS);
    list->SetPipelineState(level_pso.Get());
    list->Dispatch(cases, 1, 1);
    list->SetPipelineState(grad_pso.Get());
    list->Dispatch(cases, 1, 1);
    decode(0);
    CHECK(run());
    // a MinMip map takes the LOD before the sampler's clamp, a MipRegionUsed map after
    Wanted wanted[2] = {empty(array), empty(array)};
    for (UINT i = 0; test.bound && i < cases; i++) {
      float u = float(i % columns) * step + start, v = float(i / columns) * step + start, lod = float(i % lods) * 0.5f;
      UINT slice = (i / 3) % array.slices;
      // the gradients, which the shader gives as parts of the texture, in texels of the view's first mip
      float length = std::exp2(lod + grad_lod);
      float dx[2] = {length * array.size(0, view.first_mip) / array.width, 0},
            dy[2] = {0, length / ratio * array.size(1, view.first_mip) / array.height};
      auto f = footprint(dx, dy, test);
      for (UINT kind = 0; kind < 2; kind++) {
        auto clamped = [&](float l) { return kind ? std::max(l, least) : l; };
        sample(wanted[kind], array, view, u, v, slice, clamped(lod), test, nullptr, &margin, !kind);
        sample(wanted[kind], array, view, u, v, slice, clamped(f.lod), test, &f, &margin, !kind);
      }
    }
    char what[32];
    snprintf(what, sizeof(what), "run %u", r);
    compare(what, 0, wanted);
    // the far samples alone, so no other sample wants their regions. their texel coordinates are exact
    clear(0, 0);
    clear(0, 1);
    bind(false, test.bound ? UAVS : UNPAIRED, r, test.narrow ? NARROW : SRVS);
    list->SetPipelineState(far_pso.Get());
    list->Dispatch(far_cases, 1, 1);
    decode(0);
    CHECK(run());
    Wanted far_wanted[2] = {empty(array), empty(array)};
    for (UINT i = 0; test.bound && i < far_cases; i++)
      for (UINT kind = 0; kind < 2; kind++)
        sample(
            far_wanted[kind], array, view, i % 2 ? distant + distant_odd : -distant, (i / 2) * 0.5f + 0.25f,
            i % array.slices, kind ? least : 0, test, nullptr, nullptr, !kind
        );
    snprintf(what, sizeof(what), "run %u, far", r);
    compare(what, 0, far_wanted);
  }

  // the pixel shader: every pixel of the target samples at its center, with a run's sampler. a pixel's step is the
  // derivatives: SCALE texels of the target along u, and SKEW times that along v
  const float pixel_dx[2] = {scale * flat.width / target_size, 0}, pixel_dy[2] = {0, scale * skew * flat.height / target_size};
  auto draw = [&](UINT sampler) {
    clear(1, 0);
    clear(1, 1);
    bind(true, UAVS, sampler);
    list->SetPipelineState(draw_pso.Get());
    D3D12_VIEWPORT viewport{0, 0, (float)target_size, (float)target_size, 0, 1};
    D3D12_RECT scissor{0, 0, (LONG)target_size, (LONG)target_size};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    decode(1);
  };
  // the MinMip map takes WriteSamplerFeedback, the MipRegionUsed map WriteSamplerFeedbackBias
  auto drawn_by = [&](const Sampler &s, Wanted drawn[2]) {
    auto f = footprint(pixel_dx, pixel_dy, s);
    for (UINT y = 0; y < target_size; y++)
      for (UINT x = 0; x < target_size; x++)
        for (UINT kind = 0; kind < 2; kind++)
          sample(
              drawn[kind], flat, flat_view, (x + 0.5f) / target_size * scale, (y + 0.5f) / target_size * scale * skew, 0,
              f.lod + (kind ? bias : 0), s, &f, &margin, !kind
          );
  };
  draw(0);
  // the MinMip map again, to a buffer of its rows
  transition(list.Get(), maps[1][0].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
  transcode(decoded_buffer.Get(), maps[1][0].Get(), D3D12_RESOLVE_MODE_DECODE_SAMPLER_FEEDBACK);
  transition(list.Get(), decoded_buffer.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  const UINT64 buffer_at = slot * next_slot;
  list->CopyBufferRegion(readback.Get(), buffer_at, decoded_buffer.Get(), 0, flat_regions);
  transition(list.Get(), decoded_buffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
  CHECK(run());
  Wanted drawn[2] = {empty(flat), empty(flat)};
  drawn_by(runs[0], drawn);
  compare("draw", 1, drawn);
  for (UINT y = 0; y < flat.grid(1); y++)
    for (UINT x = 0; x < flat.grid(0); x++) {
      uint8_t value = out[buffer_at + y * flat.grid(0) + x], expected = decoded_min_mip(drawn[0], flat, 0, x, y);
      if (value != expected && failures++ < 12)
        printf("buffer: region %u,%u: %#x, want %#x\n", x, y, value, expected);
    }

  // what was decoded, encoded into the cleared maps, decodes to the same
  for (UINT kind = 0; kind < 2; kind++) {
    clear(1, kind);
    auto from = kind ? decoded[1][1].Get() : decoded_buffer.Get();
    if (kind)
      transition(list.Get(), from, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    transition(list.Get(), maps[1][kind].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RESOLVE_DEST);
    transcode(maps[1][kind].Get(), from, D3D12_RESOLVE_MODE_ENCODE_SAMPLER_FEEDBACK);
    transition(list.Get(), maps[1][kind].Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (kind)
      transition(list.Get(), from, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_RESOLVE_DEST);
  }
  decode(1);
  CHECK(run());
  compare("encoded", 1, drawn);

  // the pixel's footprint through an anisotropic filter
  draw(anisotropic);
  CHECK(run());
  Wanted drawn_anisotropic[2] = {empty(flat), empty(flat)};
  drawn_by(runs[anisotropic], drawn_anisotropic);
  compare("anisotropic draw", 1, drawn_anisotropic);

  // the expectations hold only while no sample sits where rounding decides its texels or mips: 64 units in the last
  // place of the largest texel coordinate away
  if (margin < 64 * FLT_EPSILON * array.width) {
    printf("failed: a sample is %g from a texel or mip boundary\n", margin);
    return 1;
  }
  if (failures) {
    printf("failed: %u wrong regions\n", failures);
    return 1;
  }
  printf("passed: %u regions wanted\n", wanted_regions);
  return 0;
}
