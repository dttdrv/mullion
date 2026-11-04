// CPU block decoding follows D3D11.3 19.5.2-.12 and 19.5.14.3/.4.7; storage is
// always full 4x4 blocks (19.5.1).
#pragma once
#include "format_values.hpp"

namespace block_values {
constexpr unsigned side = 4, pixels = side * side;
constexpr unsigned bc6_one_precision[] = {10, 11, 12, 16};
constexpr unsigned bc6_mode_bits = 5, bc6_low_bits = 10;
constexpr unsigned weights[] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
struct Format {
  DXGI_FORMAT id, parent;
  const char *name;
  unsigned bc, bytes;
  bool srgb, signed_values;
};
inline std::vector<Format>
formats() {
  const struct {
    DXGI_FORMAT id;
    const char *name;
  } sdk[] = {DXGI_FORMATS};
  std::vector<Format> result;
  for (auto [id, name] : sdk) {
    unsigned bc;
    if (sscanf(name, "DXGI_FORMAT_BC%u", &bc) != 1 || strstr(name, "TYPELESS"))
      continue;
    const char *suffix = strchr(name + strlen("DXGI_FORMAT_"), '_');
    std::string parent_name(name, suffix - name);
    parent_name += "_TYPELESS";
    for (auto parent : sdk)
      if (parent_name == parent.name)
        result.push_back(
            {id, parent.id, name, bc, bc == 1 || bc == 4 ? 8u : 16u, strstr(name, "SRGB") != nullptr,
             strstr(name, "SNORM") != nullptr || strstr(name, "_SF16") != nullptr}
        );
  }
  return result;
}

inline unsigned
bits(const uint8_t *block, unsigned start, unsigned count) {
  return format_values::field(block, {0, count, start});
}
inline void
put(uint8_t *block, unsigned start, unsigned count, unsigned value) {
  for (unsigned b = 0; b < count; b++)
    block[(start + b) / CHAR_BIT] |= ((value >> b) & 1) << ((start + b) % CHAR_BIT);
}

inline std::vector<uint8_t>
image(const Format &f, unsigned width, unsigned height, unsigned mip) {
  unsigned columns = (width + side - 1) / side, rows = (height + side - 1) / side;
  std::vector<uint8_t> bytes(columns * rows * f.bytes);
  for (unsigned i = 0; i < columns * rows; i++) {
    auto block = bytes.data() + i * f.bytes;
    unsigned seed = i + mip * (1u << (2 * CHAR_BIT));
    if (f.bc == 6) {
      // 19.5.13.3/.5: all mode prefixes, then ONE's contiguous low fields and reversed high bits.
      constexpr unsigned modes[] = {0, 1, 2, 6, 10, 14, 18, 22, 26, 30, 3, 7, 11, 15, 19, 23, 27, 31};
      if (seed < std::size(modes))
        block[0] = modes[seed];
      else {
        unsigned number = seed - std::size(modes), mode = number % std::size(bc6_one_precision);
        unsigned precision = bc6_one_precision[mode], delta_bits = 2 * bc6_low_bits - precision;
        unsigned value = number / std::size(bc6_one_precision);
        put(block, 0, bc6_mode_bits, (mode << 2) | 3);
        for (unsigned c = 0; c < 3; c++) {
          unsigned base = (value * (2 * c + 1) + c) & format_values::mask(precision);
          if (value < precision * 6) {
            base = ((1u << (value / 6)) + value % 3 - 1) & format_values::mask(precision);
            if (value % 6 >= 3)
              base ^= format_values::mask(precision);
          }
          unsigned delta = value < precision * 6 ? 0 : value * (2 * c + 3) + c;
          put(block, bc6_mode_bits + c * bc6_low_bits, bc6_low_bits, base);
          unsigned at = bc6_mode_bits + (3 + c) * bc6_low_bits;
          put(block, at, delta_bits, delta);
          for (unsigned b = bc6_low_bits; b < precision; b++)
            put(block, at + delta_bits + precision - 1 - b, 1, base >> b);
        }
        unsigned at = bc6_mode_bits + 6 * bc6_low_bits;
        for (unsigned t = 0; t < pixels; t++) {
          unsigned count = t ? 4 : 3;
          put(block, at, count, t + value);
          at += count;
        }
      }
    } else if (f.bc == 7) {
      if (seed < 9) {
        if (seed < 8)
          put(block, seed, 1, 1);
      } else {
        put(block, 6, 1, 1);
        unsigned at = 7;
        for (unsigned c = 0; c < 4; c++)
          for (unsigned end = 0; end < 2; end++, at += 7)
            put(block, at, 7, (seed * (2 * c + 1) + end * (c + 1)) & format_values::mask(7));
        for (unsigned end = 0; end < 2; end++, at++)
          put(block, at, 1, seed >> (7 + end) & 1);
        for (unsigned t = 0; t < pixels; t++) {
          unsigned count = t ? 4 : 3;
          put(block, at, count, (t + seed) & format_values::mask(count));
          at += count;
        }
      }
    } else {
      if (f.bc <= 3) {
        unsigned offset = f.bc == 1 ? 0 : 8 * CHAR_BIT;
        put(block, offset, 16, seed);
        put(block, offset + 16, 16, mip & 1 ? seed : ~seed);
        for (unsigned t = 0; t < pixels; t++)
          put(block, offset + 32 + 2 * t, 2, t + seed / pixels);
      }
      if (f.bc == 2)
        for (unsigned t = 0; t < pixels; t++)
          put(block, t * 4, 4, t + seed);
      if (f.bc >= 3) {
        for (unsigned component = 0; component < (f.bc == 5 ? 2u : 1u); component++) {
          unsigned offset = component * 8 * CHAR_BIT, endpoints = seed * (2 * component + 1) + component;
          put(block, offset, 16, endpoints);
          for (unsigned t = 0; t < pixels; t++)
            put(block, offset + 16 + t * 3, 3, t + component + seed / pixels);
        }
      }
    }
  }
  return bytes;
}

struct Decoded {
  std::array<double, 4> value{0, 0, 0, 1}, span{}, error{};
  std::array<bool, 4> exact{};
};

// 19.5.13.9/.10; Learn's complete signed pseudocode supplies the sign/zero branches missing in the local HTML.
inline int
bc6_unquantize(int value, unsigned precision, bool signed_values) {
  if (!signed_values) {
    if (precision >= 15)
      return value;
    if (!value)
      return 0;
    return value == int(format_values::mask(precision)) ? UINT16_MAX : ((value << 16) + (1 << 15)) >> precision;
  }
  if (precision >= 16)
    return value;
  int magnitude = std::abs(value);
  int unquantized = !magnitude ? 0
                    : magnitude >= int(format_values::mask(precision - 1))
                        ? INT16_MAX
                        : ((magnitude << 15) + (1 << 14)) >> (precision - 1);
  return value < 0 ? -unquantized : unquantized;
}

inline Decoded
decode(const Format &f, const uint8_t *block, unsigned texel) {
  Decoded d;
  if (f.bc == 6) {
    unsigned prefix = bits(block, 0, bc6_mode_bits);
    if ((prefix & 3) != 3 || (prefix >> 2) >= std::size(bc6_one_precision))
      return d;
    unsigned mode = prefix >> 2, precision = bc6_one_precision[mode], delta_bits = 2 * bc6_low_bits - precision;
    auto extend = [](unsigned value, unsigned count) {
      return value & (1u << (count - 1)) ? int(value) - int(1u << count) : int(value);
    };
    const unsigned index_at = bc6_mode_bits + 6 * bc6_low_bits;
    unsigned index = bits(block, index_at + (texel ? texel * 4 - 1 : 0), texel ? 4 : 3);
    for (unsigned c = 0; c < 3; c++) {
      unsigned base = bits(block, bc6_mode_bits + c * bc6_low_bits, bc6_low_bits);
      unsigned at = bc6_mode_bits + (3 + c) * bc6_low_bits;
      for (unsigned b = bc6_low_bits; b < precision; b++)
        base |= bits(block, at + delta_bits + precision - 1 - b, 1) << b;
      int endpoint = bits(block, at, delta_bits);
      if (mode || f.signed_values)
        endpoint = extend(endpoint, delta_bits);
      if (mode)
        endpoint = unsigned(endpoint + int(base)) & format_values::mask(precision);
      if (f.signed_values)
        endpoint = extend(unsigned(endpoint) & format_values::mask(precision), precision);
      int a = bc6_unquantize(f.signed_values ? extend(base, precision) : int(base), precision, f.signed_values);
      int b = bc6_unquantize(endpoint, precision, f.signed_values);
      int weight = int(weights[index]);
      int interpolated = (a * (64 - weight) + b * weight + 32) >> 6;
      int scaled = f.signed_values ? (interpolated < 0 ? -(((-interpolated) * 31) >> 5) : (interpolated * 31) >> 5)
                                   : (interpolated * 31) >> 6;
      unsigned half = unsigned(std::abs(scaled)) | (scaled < 0 ? 1u << 15 : 0);
      d.value[c] = format_values::small_float(half, sizeof(uint16_t) * CHAR_BIT);
    }
    return d;
  }
  if (f.bc == 7) {
    unsigned mode = 0;
    while (mode < 8 && !bits(block, mode, 1))
      mode++;
    if (mode != 6) {
      d.value[3] = mode < 4 ? 1 : 0;
      return d;
    }
    unsigned at = 7, endpoint[2][4];
    for (unsigned c = 0; c < 4; c++)
      for (unsigned end = 0; end < 2; end++, at += 7)
        endpoint[end][c] = bits(block, at, 7);
    for (unsigned end = 0; end < 2; end++, at++)
      for (unsigned c = 0; c < 4; c++)
        endpoint[end][c] = (endpoint[end][c] << 1) | bits(block, at, 1);
    unsigned index = bits(block, at + (texel ? texel * 4 - 1 : 0), texel ? 4 : 3);
    for (unsigned c = 0; c < 4; c++)
      d.value[c] =
          ((endpoint[0][c] * (64 - weights[index]) + endpoint[1][c] * weights[index] + 32) >> 6) / double(UINT8_MAX);
    return d;
  }
  if (f.bc <= 3) {
    unsigned at = f.bc == 1 ? 0 : 8 * CHAR_BIT;
    unsigned endpoints[] = {bits(block, at, 16), bits(block, at + 16, 16)};
    unsigned selector = bits(block, at + 32 + texel * 2, 2);
    const unsigned shifts[] = {11, 5, 0}, widths[] = {5, 6, 5};
    for (unsigned c = 0; c < 3; c++) {
      unsigned original[2], promoted[2];
      for (unsigned end = 0; end < 2; end++) {
        original[end] = endpoints[end] >> shifts[c] & format_values::mask(widths[c]);
        promoted[end] = (original[end] << (CHAR_BIT - widths[c])) | (original[end] >> (2 * widths[c] - CHAR_BIT));
      }
      double a = promoted[0] / double(UINT8_MAX), b = promoted[1] / double(UINT8_MAX);
      d.value[c] = selector == 0                              ? a
                   : selector == 1                            ? b
                   : f.bc != 1 || endpoints[0] > endpoints[1] ? (selector == 2 ? 2 * a + b : a + 2 * b) / 3
                   : selector == 2                            ? (a + b) / 2
                                                              : 0;
      d.span[c] =
          std::max(std::fabs(a - b), std::fabs(double(original[0]) - original[1]) / format_values::mask(widths[c]));
      d.error[c] = 1.0 / UINT8_MAX;
    }
    if (f.bc == 1 && endpoints[0] <= endpoints[1] && selector == 3)
      d.value[3] = 0;
    if (f.bc == 2) {
      d.value[3] = bits(block, texel * 4, 4) / 15.0;
      d.error[3] = 1.0 / UINT8_MAX;
    }
  }
  if (f.bc >= 3) {
    for (unsigned component = 0; component < (f.bc == 5 ? 2u : 1u); component++) {
      unsigned at = component * 8 * CHAR_BIT, channel = f.bc == 3 ? 3 : component;
      int endpoints[2];
      for (unsigned end = 0; end < 2; end++) {
        endpoints[end] = bits(block, at + end * CHAR_BIT, CHAR_BIT);
        if (f.signed_values && endpoints[end] > INT8_MAX)
          endpoints[end] -= 1u << CHAR_BIT;
      }
      double a = f.signed_values ? std::max(-1.0, endpoints[0] / double(INT8_MAX)) : endpoints[0] / double(UINT8_MAX);
      double b = f.signed_values ? std::max(-1.0, endpoints[1] / double(INT8_MAX)) : endpoints[1] / double(UINT8_MAX);
      unsigned index = bits(block, at + 16 + texel * 3, 3);
      d.value[channel] = index == 0                    ? a
                         : index == 1                  ? b
                         : endpoints[0] > endpoints[1] ? ((8 - index) * a + (index - 1) * b) / 7
                         : index < 6                   ? ((6 - index) * a + (index - 1) * b) / 5
                         : index == 6                  ? (f.signed_values ? -1 : 0)
                                                       : 1;
      d.span[channel] = std::fabs(a - b);
      if (f.signed_values) {
        auto promoted = [](int endpoint) {
          unsigned value = unsigned(std::abs(std::max(endpoint, -INT8_MAX)));
          constexpr unsigned base = CHAR_BIT - 1, target = sizeof(int16_t) * CHAR_BIT - 1;
          unsigned wide = (value << (target - base)) | (value << (target - 2 * base)) | (value >> (3 * base - target));
          return (endpoint < 0 ? -double(wide) : double(wide)) / INT16_MAX;
        };
        d.span[channel] = std::max(d.span[channel], std::fabs(promoted(endpoints[0]) - promoted(endpoints[1])));
      }
      d.error[channel] = 1.0 / (f.bc == 3 ? UINT8_MAX : f.signed_values ? INT16_MAX : UINT16_MAX);
      d.exact[channel] = index < 2;
    }
  }
  return d;
}

inline bool
matches(const Format &f, const uint8_t *image, unsigned width, unsigned x, unsigned y, const float *have) {
  unsigned columns = (width + side - 1) / side;
  auto block = image + ((y / side) * columns + x / side) * f.bytes;
  auto d = decode(f, block, (y % side) * side + x % side);
  for (unsigned c = 0; c < d.value.size(); c++) {
    double want = d.value[c], got = have[c];
    // M3 decodes reserved BC7 as opaque
    if (f.bc == 7 && !block[0] && c == 3 && have[c] == 1)
      continue;
    if (f.bc >= 6 && !(f.srgb && c < 3)) {
      if (std::bit_cast<uint32_t>(have[c]) != std::bit_cast<uint32_t>(float(want)))
        return false;
      continue;
    }
    if (!std::isfinite(got))
      return false;
    if (d.exact[c]) {
      if (have[c] != float(want))
        return false;
      continue;
    }
    // Apple's fixed-point SNORM interpolator misses by up to an 8-bit step, even where the weights land on zero
    const bool snorm_weighted = f.signed_values && f.bc <= 5 && d.span[c] > 0;
    if (!(snorm_weighted && want == 0) && (want == 0 || want == 1 || want == -1)) {
      if (got != want)
        return false;
      continue;
    }
    if (f.srgb && c < 3) {
      if (got < 0 || got > 1)
        return false;
      got = format_values::srgb(got);
      d.error[c] += (f.bc == 7 ? 0.5 : 1.0) / UINT8_MAX;
    }
    if (std::fabs(got - want) >= std::max(d.error[c] + 0.03 * d.span[c], snorm_weighted ? 1.0 / INT8_MAX : 0.0))
      return false;
  }
  return true;
}

static const char hlsl[] = R"hlsl(
cbuffer Shape : register(b0) { uint width; uint height; uint level; };
Texture2D<float4> tex : register(t0);
SamplerState nearest : register(s0);
float4 vs(uint id : SV_VertexID) : SV_Position { return float4(id==2?3:-1,id==1?3:-1,0,1); }
float4 ps(float4 p : SV_Position) : SV_Target {
#if POINT_SAMPLE
  return tex.SampleLevel(nearest, p.xy / float2(width,height),level);
#else
  return tex.Load(int3(uint2(p.xy),level));
#endif
}
)hlsl";
} // namespace block_values
