// contract: a minimum or maximum filter "fetches the same set of texels that a
// normal texture filter would fetch, but instead of blending the values to
// produce an answer, it returns the min() or max() of the texels fetched, on a
// per-component basis", and a texel "that would be 0 weight on a non-min/max
// filter" does "not contribute to the result" (D3D11.3 5.9.4.5.6, Min/Max
// Reduction Filtering; D3D12_FILTER on Microsoft Learn: "Texels that are
// weighted 0 during filtering aren't counted"). the texels are the ones of
// D3D11.3 7.18: the range-reduced coordinate (7.18.6), one texel at floor(u *
// size) for a point filter (7.18.7) or two at floor(u * size - 0.5), the second
// weighted by the fraction, for a linear one (7.18.8), in 16.8 fixed point
// (3.2.4.1); each axis's address mode, or the border color (7.18.9); the
// magnification filter at a clamped LOD of 0 or less (7.18.11); a point mip
// filter's mip at floor(LOD + 0.5), or a linear one's two mips, the second
// weighted by the LOD's fraction (7.18.10). a cube's face is its direction's
// major axis, Z then Y then X on a tie (7.18.7). this oracle computes those
// footprints and reduces them, or averages them for a filter that does not
// reduce, shared by the D3D11 and D3D12 tests.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace minmax_filters {

using float4 = std::array<float, 4>;

// D3D11_FILTER and D3D12_FILTER share their values; so do their texture address
// modes
enum Address { Wrap = 1, Mirror, Clamp, Border, MirrorOnce };
enum Reduction { Standard, Comparison, Minimum, Maximum };

struct Spec {
  unsigned filter;
  Address address[3];
  float4 border;

  bool
  min_linear() const {
    return filter >> 4 & 1;
  }
  bool
  mag_linear() const {
    return filter >> 2 & 1;
  }
  bool
  mip_linear() const {
    return filter & 1;
  }
  Reduction
  reduction() const {
    return Reduction(filter >> 7 & 3);
  }
  bool
  reduces() const {
    return reduction() == Minimum || reduction() == Maximum;
  }
};

struct Image {
  enum Shape { Line, Plane, Volume, Cube } shape;
  unsigned width, height, depth, layers, mips;
  // the texels: a 2x1 {0, 1}, a 4x1 {2, 3, 4, 5}, or distinct values from a
  // fixed hash (no seed), one channel or four
  unsigned kind, channels;

  unsigned
  size(unsigned axis, unsigned mip) const {
    unsigned n = axis == 0 ? width : axis == 1 ? height : depth;
    return std::max(n >> mip, 1u);
  }
  // how many coordinates address a texel of a mip; a cube's face has two
  unsigned
  axes() const {
    return shape == Line ? 1 : shape == Volume ? 3 : 2;
  }
  float
  value(unsigned layer, unsigned mip, unsigned x, unsigned y, unsigned z, unsigned c) const {
    if (kind == 0)
      return c ? 0 : float(x);
    if (kind == 1)
      return c ? 0 : float(2 + x);
    uint32_t h = layer * 0x85EBCA77u ^ mip * 0xC2B2AE3Du ^ x * 0x27D4EB2Fu ^ y * 0x165667B1u ^ z * 0xD3A2646Cu ^
                 c * 0xFD7046C5u ^ unsigned(shape) * 0x9E3779B1u;
    h ^= h >> 15, h *= 0x2C1B3C6Du, h ^= h >> 12, h *= 0x297A2D39u, h ^= h >> 15;
    return float(h % 1024) / 8;
  }
};

// a float in 16.8 fixed point, rounded to nearest even (3.2.4.1)
inline float
fixed(float v) {
  return std::nearbyint(v * 256) / 256;
}

// 7.18.6
inline float
reduce_range(float u, Address mode) {
  if (mode == Wrap)
    return u - std::floor(u);
  if (mode == Mirror)
    return (u / 2 - std::trunc(u / 2)) * 2;
  return std::clamp(u, -10.0f, 10.0f);
}

// 7.18.9: the texel an address reads, or -1 for the border color
inline int
address(int t, int n, Address mode) {
  if (t >= 0 && t < n)
    return t;
  switch (mode) {
  case Wrap:
    t %= n;
    return t < 0 ? t + n : t;
  case Mirror: {
    if (t < 0)
      t = -t - 1;
    bool flip = (t / n) & 1;
    t %= n;
    return flip ? n - t - 1 : t;
  }
  case MirrorOnce:
    if (t < 0)
      t = -t - 1;
    return std::clamp(t, 0, n - 1);
  case Border:
    return -1;
  default:
    return std::clamp(t, 0, n - 1);
  }
}

// the isotropic LOD of gradients in texels of the first mip:
// "log2(max(lengthX,lengthY))" (7.18.11)
inline float
gradient_lod(const Image &image, const float dx[3], const float dy[3]) {
  float lengths[2] = {};
  for (unsigned a = 0; a < image.axes(); a++) {
    float x = dx[a] * float(image.size(a, 0)), y = dy[a] * float(image.size(a, 0));
    lengths[0] += x * x, lengths[1] += y * y;
  }
  return 0.5f * std::log2(std::max(lengths[0], lengths[1]));
}

// a sample at `coord` of `layer` with the LOD before its clamps, and an
// immediate `offset` in texels of each mip read
inline float4
reference(const Image &image, const Spec &spec, const float coord[3], unsigned layer, float lod, const int offset[3]) {
  float uv[3] = {coord[0], coord[1], coord[2]};
  Address modes[3] = {spec.address[0], spec.address[1], spec.address[2]};
  if (image.shape == Image::Cube) {
    float x = coord[0], y = coord[1], z = coord[2], ax = std::fabs(x), ay = std::fabs(y), az = std::fabs(z);
    float s, t, major;
    if (az >= ax && az >= ay)
      layer = z >= 0 ? 4 : 5, s = z >= 0 ? x : -x, t = -y, major = az;
    else if (ay >= ax)
      layer = y >= 0 ? 2 : 3, s = x, t = y >= 0 ? z : -z, major = ay;
    else
      layer = x >= 0 ? 0 : 1, s = x >= 0 ? -z : z, t = -y, major = ax;
    uv[0] = s / major * 0.5f + 0.5f, uv[1] = t / major * 0.5f + 0.5f;
    // the cases stay inside a face, where the texels a filter reads across an
    // edge (7.18.12) do not arise
    modes[0] = modes[1] = Clamp;
  }
  // the sampler's MinLOD 0 and MaxLOD past the mips, then the mips there are
  lod = fixed(std::clamp(lod, 0.0f, float(image.mips - 1)));
  bool linear = lod <= 0 ? spec.mag_linear() : spec.min_linear();
  unsigned first = unsigned(std::floor(spec.mip_linear() ? lod : lod + 0.5f));
  float fraction = spec.mip_linear() ? lod - std::floor(lod) : 0;
  float4 result{}, sum{};
  bool any = false;
  for (unsigned k = 0; k < 2; k++) {
    float mip_weight = k ? fraction : 1 - fraction;
    if (mip_weight <= 0)
      continue;
    unsigned mip = std::min(first + k, image.mips - 1);
    int texel[3][2] = {};
    float weight[3][2] = {{1, 0}, {1, 0}, {1, 0}};
    for (unsigned a = 0; a < image.axes(); a++) {
      int n = int(image.size(a, mip));
      float scaled = fixed(reduce_range(uv[a], modes[a]) * float(n) + float(offset[a]) - (linear ? 0.5f : 0));
      float below = std::floor(scaled), f = linear ? scaled - below : 0;
      for (int i = 0; i < 2; i++)
        texel[a][i] = address(int(below) + i, n, modes[a]);
      weight[a][0] = 1 - f, weight[a][1] = f;
    }
    for (unsigned corner = 0; corner < 8; corner++) {
      float w = mip_weight;
      int at[3];
      bool border = false;
      for (unsigned a = 0; a < 3; a++) {
        unsigned i = corner >> a & 1;
        w *= weight[a][i];
        at[a] = texel[a][i];
        border |= a < image.axes() && at[a] < 0;
      }
      if (w <= 0)
        continue;
      float4 v;
      for (unsigned c = 0; c < 4; c++)
        v[c] = border ? spec.border[c] : image.value(layer, mip, at[0], at[1], at[2], c);
      for (unsigned c = 0; c < 4; c++) {
        sum[c] += w * v[c];
        result[c] = !any ? v[c] : spec.reduction() == Maximum ? std::max(result[c], v[c]) : std::min(result[c], v[c]);
      }
      any = true;
    }
  }
  return spec.reduces() ? result : sum;
}

} // namespace minmax_filters
