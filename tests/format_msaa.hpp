// each palette component is an independently encoded value; sample reads must retain its index and conversion.
#pragma once
#include "format_values.hpp"
#include "stored_samples.hpp"

namespace format_msaa {
// a value for each sample, and one for each stored sample where the GPU stores fewer (stored_samples)
inline std::vector<uint8_t>
palette(const format_values::Format &f, unsigned samples) {
  std::vector<uint8_t> bytes(samples * f.bytes);
  const unsigned together = samples / stored_samples(samples);
  for (unsigned at = 0; at < samples; at++)
    for (auto &c : f.components) {
      const unsigned s = at / together * together;
      if (c.channel >= 4)
        continue;
      uint32_t value;
      if (f.kind == format_values::Floating) {
        if (c.bits == 32)
          value = std::bit_cast<uint32_t>(float(s + c.channel + 1));
        else {
          unsigned fraction = c.bits == 16 ? 10 : c.bits - 5;
          value = (15u << fraction) | (format_values::mask(fraction) * s / (samples - 1));
        }
      } else {
        unsigned sign = f.kind == format_values::Snorm || f.kind == format_values::Signed;
        value = uint32_t(uint64_t(format_values::mask(c.bits - sign)) * s / (samples - 1));
        if (sign)
          value = -value & format_values::mask(c.bits);
      }
      for (unsigned b = 0; b < c.bits; b++)
        bytes[at * f.bytes + (c.offset + b) / CHAR_BIT] |= ((value >> b) & 1) << ((c.offset + b) % CHAR_BIT);
    }
  return bytes;
}

inline std::vector<format_values::Pixel>
values(const format_values::Format &f, const std::vector<uint8_t> &bytes) {
  std::vector<format_values::Pixel> result;
  for (size_t i = 0; i < bytes.size(); i += f.bytes)
    result.push_back(format_values::decode(f, bytes.data() + i));
  return result;
}

inline format_values::Pixel
average(const std::vector<format_values::Pixel> &values) {
  format_values::Pixel result;
  for (unsigned c = 0; c < result.size(); c++) {
    double sum = 0;
    for (auto &v : values)
      sum += std::bit_cast<float>(v[c]);
    result[c] = std::bit_cast<uint32_t>(float(sum / values.size()));
  }
  return result;
}

inline bool
clear_value(const format_values::Format &f, const std::vector<uint8_t> &palette, std::array<float, 4> &result) {
  std::vector<uint8_t> bytes(f.bytes);
  if (f.kind != format_values::Floating) {
    const auto &c = f.components.front();
    bytes[c.offset / CHAR_BIT] = 1u << (c.offset % CHAR_BIT);
  }
  auto value = format_values::decode(f, bytes.data());
  for (size_t i = 0; i < palette.size(); i += f.bytes)
    if (format_values::loaded(f, palette.data() + i, value))
      return false;
  for (unsigned c = 0; c < result.size(); c++)
    result[c] = f.kind == format_values::Unsigned ? float(value[c])
                : f.kind == format_values::Signed ? float(std::bit_cast<int32_t>(value[c]))
                                                  : std::bit_cast<float>(value[c]);
  return true;
}

// sRGB resolve composes the permitted 0.5-ULP source decode and 0.6-ULP destination encode errors (3.2.3.7/.8).
inline bool
resolved(
    const format_values::Format &f, const std::vector<uint8_t> &raw, const std::vector<format_values::Pixel> &values,
    const uint8_t *bytes
) {
  auto mean = average(values);
  if (f.kind != format_values::Srgb) {
    if (format_values::stored(f, mean, bytes))
      return true;
    // a resolve of float16, float11 or float10 samples is an operation on them, which rounds to nearest even (D3D11.3
    // 3.1.5, 3.1.6), where storing their float32 mean rounds toward zero (3.2.2): the spec names neither for resolves
    bool nearest = f.kind == format_values::Floating &&
                   std::ranges::all_of(f.components, [](const auto &c) { return c.bits < 32; }) &&
                   format_values::stored(f, mean, bytes, true);
    if (!nearest)
      for (const auto &c : f.components)
        if (c.channel < mean.size())
          printf("channel %u: %#x, mean %#x\n", c.channel, format_values::field(bytes, c), mean[c.channel]);
    return nearest;
  }
  for (const auto &c : f.components) {
    if (c.channel >= mean.size())
      continue;
    const double maximum = format_values::mask(c.bits);
    double low = 0, high = 0;
    if (c.channel == 3)
      low = high = std::bit_cast<float>(mean[c.channel]) * maximum;
    else {
      for (size_t i = 0; i < raw.size(); i += f.bytes) {
        double code = format_values::field(raw.data() + i, c);
        bool endpoint = code == 0 || code == maximum;
        low += format_values::linear((endpoint ? code : code - 0.5) / maximum);
        high += format_values::linear((endpoint ? code : code + 0.5) / maximum);
      }
      low = format_values::srgb(low / values.size()) * maximum;
      high = format_values::srgb(high / values.size()) * maximum;
    }
    double code = format_values::field(bytes, c);
    if (code < low - 0.6 || code > high + 0.6)
      return false;
  }
  return true;
}

static const char hlsl[] = R"hlsl(
cbuffer Shape : register(b0) { uint width; uint height; uint samples; uint only; };
Texture2DMS<TYPE> tex : register(t0);
StructuredBuffer<uint4> source : register(t1);
RWStructuredBuffer<uint4> result : register(u0);
float4 vs(uint id : SV_VertexID) : SV_Position { return float4(id==2?3:-1,id==1?3:-1,0,1); }
TYPE ps(uint index : SV_SampleIndex) : SV_Target { return UNPACK(source[index]); }
struct Center { TYPE colour : SV_Target; uint mask : SV_Coverage; };
Center ps_center() { Center v; v.colour = UNPACK(source[only]); v.mask = 1u << only; return v; }
[numthreads(1,1,1)] void cs(uint2 p : SV_DispatchThreadID) {
  for (uint index=0; index<samples; index++) result[(p.x+p.y*width)*samples+index] = asuint(tex.Load(p,index));
}
)hlsl";
} // namespace format_msaa
