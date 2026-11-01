// contract: D3D11.3 15.10: "Bias = (float)DepthBias * r + SlopeScaledDepthBias" for UNORM depth;
// "Bias = (float)DepthBias * 2^(exponent(max abs(z) in primitive) - r) +" followed by "SlopeScaledDepthBias"
// for float depth. "Biasing is constant for a given primitive" and "The biasing formulas are performed with
// float32 arithmetic." positive clamps use min, negative clamps use max, zero means no clamp. source:
// D3D11_3_FunctionalSpec.htm 15.10. depth formats and component widths come from the existing header enumeration.
// 15.6: "the z value is always clamped: z = min(Viewport.MaxDepth,max(Viewport.MinDepth,z))".
#pragma once
#include "raster.hpp"
#include "../depth_formats.hpp"
#include <climits>
#include <cstdio>

namespace raster_bias {
struct Format {
  DXGI_FORMAT value;
  std::string name;
  unsigned depth = 0, stencil = 0, unused = 0;
  bool floating = false;
};
inline std::vector<Format> formats() {
  const std::pair<DXGI_FORMAT, const char *> values[] = {DXGI_DEPTH_FORMATS};
  std::vector<Format> out;
  for (auto [value, name] : values) {
    Format f{value, name};
    sscanf(name, "DXGI_FORMAT_D%u", &f.depth);
    f.floating = f.name.find("FLOAT") != std::string::npos;
    auto s = f.name.find("_S");
    if (s != std::string::npos)
      sscanf(name + s, "_S%uX%u", &f.stencil, &f.unused);
    out.push_back(f);
  }
  return out;
}
struct Case {
  int bias = 0;
  float scale = 0, clamp = 0, z = 0.25f, dx = 0, dy = 0;
  raster::Shape shape = raster::Triangle;
  bool wire = false, collinear = false, default_state = false;
  float low = 0, high = 1;
  bool depth_clip = true;
};
inline raster::Case geometry(const Case &c) {
  auto out = raster::base("depth bias");
  out.shape = c.shape;
  out.wire = c.wire;
  out.viewport.low = c.low;
  out.viewport.high = c.high;
  out.depth_clip = c.depth_clip;
  out.vertices.resize(c.shape);
  if (c.collinear)
    out.vertices = {raster::vertex(raster::width / 2, 2), raster::vertex(raster::width / 2, raster::height / 2),
                    raster::vertex(raster::width / 2, raster::height - 2)};
  for (auto &v : out.vertices) {
    auto p = raster::project(v, {});
    v.position[2] = c.z + c.dx * float(p.x) + c.dy * float(p.y);
  }
  return out;
}
inline float bias(const Format &f, const Case &c) {
  if (c.shape != raster::Triangle || (!c.bias && c.scale == 0))
    return 0;
  float slope = std::max(std::abs(c.dx), std::abs(c.dy)) * (c.high - c.low);
  float scaled = c.scale != 0 ? c.scale * slope : c.scale;
  float unit;
  if (f.floating) {
    float maximum = 0;
    for (const auto &v : geometry(c).vertices)
      maximum = std::max(maximum, std::abs(c.low + v.position[2] * (c.high - c.low)));
    int exponent = maximum == 0 ? std::numeric_limits<float>::min_exponent - 1 : std::ilogb(maximum);
    unit = std::ldexp(1.0f, exponent - (std::numeric_limits<float>::digits - 1));
  } else
    unit = 1.0f / float((uint64_t(1) << f.depth) - 1);
  float value = float(c.bias) * unit + scaled;
  if (c.clamp > 0)
    value = std::min(c.clamp, value);
  else if (c.clamp < 0)
    value = std::max(c.clamp, value);
  return value;
}
inline std::vector<Case> cases() {
  std::vector<Case> out;
  for (float z : {0.0f, 0.25f, 0.5f, 0.75f})
    for (int bias : {INT_MIN, -256, -1, 0, 1, 256, INT_MAX}) {
      if (!z && bias)
        continue;
      out.push_back({bias, 0, 0, z});
    }
  const float step = 1.0f / (raster::width * raster::height);
  for (float dx : {-step, 0.0f, step})
    for (float dy : {-step, 0.0f, step})
      for (float scale : {-2.0f, 0.0f, 2.0f})
        for (float clamp : {-step, 0.0f, step})
          out.push_back({256, scale, clamp, 0.5f, dx, dy});
  for (int sign : {-1, 1}) {
    for (float clamp : {std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                        std::numeric_limits<float>::quiet_NaN()}) {
      Case c;
      c.bias = sign * 256;
      c.clamp = clamp;
      out.push_back(c);
    }
    Case c;
    c.scale = sign * std::numeric_limits<float>::max();
    c.dx = step;
    out.push_back(c);
  }
  for (float scale : {-0.0f, std::numeric_limits<float>::denorm_min(), -std::numeric_limits<float>::denorm_min(),
                      std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                      std::numeric_limits<float>::quiet_NaN()})
    for (float dx : {0.0f, step})
      for (float clamp : {-step, 0.0f, step}) {
        Case c;
        c.scale = scale;
        c.dx = dx;
        c.clamp = clamp;
        out.push_back(c);
      }
  for (float clamp : {-0.0f, std::numeric_limits<float>::denorm_min(), -std::numeric_limits<float>::denorm_min()}) {
    Case c;
    c.bias = 256;
    c.clamp = clamp;
    out.push_back(c);
  }
  for (float z : {-1.0f, 0.0f, 0.5f, 1.0f, 2.0f})
    for (int bias : {INT_MIN, 0, INT_MAX})
      for (bool clip : {false, true}) {
        Case c;
        c.low = 0.25f;
        c.high = 0.75f;
        c.z = z;
        c.bias = bias;
        c.depth_clip = clip;
        out.push_back(c);
      }
  {
    Case c;
    c.wire = true;
    c.collinear = true;
    c.z = 0.5f;
    out.push_back(c);
  }
  for (float dx : {0.0f, step}) {
    Case c;
    c.default_state = true;
    c.dx = dx;
    out.push_back(c);
  }
  for (auto shape : {raster::Point, raster::Line})
    for (int bias : {INT_MIN, 0, INT_MAX}) {
      Case c;
      c.bias = bias;
      c.scale = 2;
      c.clamp = 0.25f;
      c.shape = shape;
      out.push_back(c);
    }
  for (int bias : {-256, 0, 256}) {
    Case c;
    c.bias = bias;
    c.wire = true;
    out.push_back(c);
  }
  return out;
}
inline void compare(const Format &f, const Case &c, const char *rows, size_t pitch, unsigned pixel_bytes = 0) {
  auto covered = raster::expected(geometry(c));
  // a state's denormal "may or may not be flushed before use by the hardware" (D3D11.3 3.1): a denormal clamp clamps,
  // or is zero and does not
  auto flushed = c;
  if (std::fpclassify(c.clamp) == FP_SUBNORMAL)
    flushed.clamp = 0;
  const float offsets[] = {bias(f, c), bias(f, flushed)};
  unsigned wrong = 0;
  const unsigned bytes = pixel_bytes ? pixel_bytes : (f.depth + f.stencil + f.unused) / CHAR_BIT;
  // three interpolator products, their sum, and the bias add bound the float32 rounding error.
  constexpr unsigned interpolation_operations = 2 * raster::Triangle + 1;
  constexpr double conversion_tolerance = 0.6; // D3D11.3 3.2.3.6, integer-side ULP tolerance
  for (unsigned y = 0; y < raster::height; y++)
    for (unsigned x = 0; x < raster::width; x++) {
      const char *at = rows + y * pitch + x * bytes;
      bool equal = false;
      double actual = 0, want = 0;
      for (size_t i = 0; i < std::size(offsets) && !equal; i++) {
        float depth = covered[y * raster::width + x][0]
                          ? std::fmin(c.high, std::fmax(c.low, c.low +
                                                                   (c.z + c.dx * (x + 0.5f) + c.dy * (y + 0.5f)) *
                                                                       (c.high - c.low) +
                                                                   offsets[i]))
                          : 1;
        double expected;
        if (f.floating) {
          float got;
          memcpy(&got, at, sizeof(got));
          actual = got;
          expected = depth;
          float tolerance = c.dx == 0 && c.dy == 0 ? 0
                                                   : interpolation_operations * std::numeric_limits<float>::epsilon() *
                                                         std::max(depth, 0.5f);
          equal = std::isfinite(got) && std::abs(got - depth) <= tolerance;
        } else {
          uint32_t got = 0;
          memcpy(&got, at, f.depth / CHAR_BIT);
          uint32_t max = uint32_t((uint64_t(1) << f.depth) - 1);
          actual = got;
          expected = double(depth) * max;
          double interpolation =
              c.dx == 0 && c.dy == 0
                  ? 0
                  : interpolation_operations * std::numeric_limits<float>::epsilon() * std::max(depth, 0.5f) * max;
          equal = std::abs(actual - expected) <= conversion_tolerance + interpolation;
        }
        if (!i)
          want = expected;
      }
      if (!equal && wrong++ < 4)
        expect(false, "depth %u,%u = %.10g, want %.10g", x, y, actual, want);
    }
}
} // namespace raster_bias
