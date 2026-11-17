// D3D11.3 17.17: "SampleMask is always applied, regardless of whether
// Multisample rendertargets are bound or not." only the low n bits contribute;
// their sample mapping is implementation-defined, so the count of stored ones
// must equal popcount(mask) independently of that mapping (of the mask's pairs, on a GPU that stores a sample for
// two: stored_samples.hpp). 17.18 requires
// alpha <= 0 and NaN to cover none, alpha >= 1 to cover all, and disables A2C
// when the shader writes coverage. 19.2.3 defines the queried quality range.
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
#pragma once
#include <d3d11.h>
#include "stored_samples.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <climits>
#include <limits>
#include <string>
#include <vector>

namespace om {
inline const float sample_alphas[] = {
    -INFINITY, -1, -0.0f,    0,
    1,         2,  INFINITY, std::numeric_limits<float>::quiet_NaN()};
inline constexpr UINT sample_width = std::size(sample_alphas) + 2;
struct SampleCase {
  UINT mask;
  bool alpha, coverage;
};
inline std::vector<SampleCase> sample_cases(UINT samples) {
  std::vector<UINT> masks{0, ~0u};
  UINT alternating = 0;
  for (UINT bit = 0; bit < D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT; bit++) {
    masks.push_back(1u << bit);
    masks.push_back(~(1u << bit));
    if (bit % 2)
      alternating |= 1u << bit;
  }
  masks.push_back(alternating);
  masks.push_back(~alternating);
  masks.push_back(UINT(~((uint64_t(1) << samples) - 1)));
  std::sort(masks.begin(), masks.end());
  masks.erase(std::unique(masks.begin(), masks.end()), masks.end());
  std::vector<SampleCase> result;
  for (auto mask : masks)
    for (bool alpha : {false, true})
      for (bool coverage : {false, true})
        result.push_back({mask, alpha, coverage});
  return result;
}
inline UINT sample_result(const SampleCase &k, UINT samples, UINT x) {
  if (!x || x + 1 == sample_width)
    return 0;
  float alpha = sample_alphas[x - 1];
  if (k.alpha && !k.coverage && !(alpha >= 1))
    return 0;
  UINT bits = k.mask & UINT((uint64_t(1) << samples) - 1);
  // where the GPU stores half the samples, a stored one is written when either of its two is, and both read it
  if (stored_samples(samples) < samples)
    bits = (bits | bits >> 1) & 0x55555555u, bits |= bits << 1;
  return std::popcount(bits);
}
inline std::vector<std::string> sample_defines() {
  std::string bits;
  for (float alpha : sample_alphas) {
    if (!bits.empty())
      bits += ',';
    bits += std::to_string(std::bit_cast<UINT>(alpha)) + 'u';
  }
  return {"ALPHAS=" + bits,
          "ALPHA_COUNT=" + std::to_string(std::size(sample_alphas))};
}
static const char sample_hlsl[] = R"hlsl(
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 color(float4 pos) {
  static const uint bits[] = {ALPHAS};
  return float4(1, 0, 0, asfloat(bits[(uint(pos.x) + ALPHA_COUNT - 1) % ALPHA_COUNT]));
}
float4 ps(float4 pos : SV_Position) : SV_Target { return color(pos); }
float4 ps_coverage(float4 pos : SV_Position, out uint coverage : SV_Coverage) : SV_Target {
  coverage = ~0u; return color(pos);
}
#if SINGLE
Texture2D<float> image : register(t0);
#elif STATIC_SAMPLES
Texture2DMS<float, SAMPLES> image : register(t0);
#else
Texture2DMS<float> image : register(t0);
#endif
#if PIXEL_READ
uint ps_read(float4 pos : SV_Position) : SV_Target {
  uint width, height, count;
#if SINGLE
  image.GetDimensions(width, height); count = 1;
  float value = image.Load(int3(uint2(pos.xy), 0));
#else
  #if STATIC_SAMPLES
  count = SAMPLES;
  #else
  image.GetDimensions(width, height, count);
  #endif
  #if STATIC_SAMPLE_INDEX
  float value = 0;
  [unroll] for (uint sid=0; sid<SAMPLES; sid++)
    if (sid == uint(pos.x)%count) value = image.Load(uint2(uint(pos.x)/count, uint(pos.y)), sid);
  #else
  float value = image.Load(uint2(uint(pos.x)/count, uint(pos.y)), uint(pos.x)%count);
  #endif
#endif
  return asuint(value);
}
#endif
#if COMPUTE_READ
RWStructuredBuffer<uint> words : register(u0);
[numthreads(1, 1, 1)] void cs(uint2 pos : SV_DispatchThreadID) {
  uint width, height, count;
#if SINGLE
  image.GetDimensions(width, height); count = 1;
#else
  image.GetDimensions(width, height, count);
#endif
  if (pos.x == 0 && pos.y == 0) { words[0]=width; words[1]=height; words[2]=count; }
  for (uint sid=0; sid<count; sid++) {
#if SINGLE
    float value = image.Load(int3(pos, 0));
#else
    float value = image.Load(pos, sid);
#endif
    words[3 + (pos.y*width + pos.x)*count + sid] = asuint(value);
  }
}
#endif
)hlsl";
} // namespace om
