// what a sample wants of a sampler feedback map, worked out on the CPU, for the tests of sampler feedback
// (d3d12_feedback.cpp has the contract it follows: DirectX-Specs, Sampler Feedback, and D3D11.3 7.18)
#pragma once
#include "d3d12_test.hpp"
#include <algorithm>
#include <cmath>
#include <set>

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
