// contract: D3D11.3 19.2.4: "Standard sample patterns are defined for the following sample counts:
// 1(trivial), 2, 4, 8, 16." 22.4.22: "If sampleIndex is out of bounds a zero vector is returned."
// 16.3.2: "The InputCoverage bitfield is not affected by depth/stencil tests, but it is ANDed with the SampleMask
// Rasterizer state." source: D3D11_3_FunctionalSpec.htm; DXIL.rst operations 75,76,77,90,91.
// 3.5.5: "Else the attribute is evaluated at the first covered sample, in increasing order of sample index".
// 3.5.6.4: "The SampleMask Rasterizer State does not affect rasterization behavior at all here."
// positions below are transcribed from the specification's ordered pattern diagrams, not from the implementation.
#pragma once
#include "raster.hpp"
#include <numeric>

namespace raster_samples {
using namespace raster;
enum class RenderMode { Regular, RtvForced, RtvForcedMsaa, UavForced, UavDefault };
enum Pass {
  Shading,
  Queries,
  Unbound,
  Export,
  Forced,
  IndexOnly,
  ModifierOnly,
  LegacyShading,
  LegacyIndex,
  LegacyModifier,
  LegacyExport,
  LegacyCoverage,
  Varying,
  Centroid,
  PerspectiveVarying,
  PerspectiveCentroid,
  PositionDepth,
  ForcedMsaa,
  Uav
};
inline std::vector<std::array<int, 2>> pattern(unsigned count) {
  switch (count) {
  case 1:
    return {{0, 0}};
  case 2:
    return {{4, 4}, {-4, -4}};
  case 4:
    return {{-2, -6}, {6, -2}, {-6, 2}, {2, 6}};
  case 8:
    return {{1, -3}, {-1, 3}, {5, 1}, {-3, -5}, {-5, 5}, {-7, -1}, {3, 7}, {7, -7}};
  case 16:
    return {{1, 1},  {-1, -3}, {-3, 2},  {4, -1}, {-5, -2}, {2, 5},  {5, 3}, {3, -5},
            {-2, 6}, {0, -7},  {-4, -6}, {-6, 4}, {-8, 0},  {7, -4}, {6, 7}, {-7, -8}};
  default:
    return {};
  }
}
static const char hlsl[] = R"hlsl(
Texture2DMS<float4> image : register(t0);
#ifdef LEGACY
#define COVERAGE_INPUT
#define COVERAGE_VALUE 1
#else
#define COVERAGE_INPUT , uint mask : SV_Coverage
#define COVERAGE_VALUE mask
RWStructuredBuffer<float4> result : register(u0);
#endif
float4 ps_sample(sample float4 p : SV_Position, uint sid : SV_SampleIndex COVERAGE_INPUT) : SV_Target {
  return float4(p.xy, sid+1, COVERAGE_VALUE);
}
float4 ps_index(float4 p : SV_Position, uint sid : SV_SampleIndex COVERAGE_INPUT) : SV_Target {
  return float4(p.xy,sid+1,COVERAGE_VALUE);
}
float4 ps_modifier(sample float4 p : SV_Position COVERAGE_INPUT) : SV_Target {
  return float4(p.xy,1,COVERAGE_VALUE);
}
#ifdef PERSPECTIVE
#define INTERPOLATION linear
#else
#define INTERPOLATION noperspective
#endif
float4 ps_varying(float4 p : SV_Position, INTERPOLATION sample float2 uv : TEXCOORD0) : SV_Target {
  return float4(uv,p.xy);
}
float4 ps_centroid(float4 p : SV_Position, INTERPOLATION centroid float2 uv : TEXCOORD0) : SV_Target {
  return float4(uv,p.xy);
}
float4 ps_depth(sample float4 p : SV_Position) : SV_Target { return float4(p.zw,p.xy); }
float4 ps_query(float4 p : SV_Position) : SV_Target {
  uint n = uint(p.x) + uint(p.y) * WIDTH;
  int index = n < COUNT ? int(n) : n == COUNT ? COUNT : n == COUNT+1 ? -1 : COUNT+1;
  return float4(GetRenderTargetSamplePosition(index), image.GetSamplePosition(index));
}
struct Output { float4 color : SV_Target; uint mask : SV_Coverage; };
Output ps_export(float4 p : SV_Position COVERAGE_INPUT) {
  uint n = (uint(p.x) + uint(p.y)*WIDTH) % (2*COUNT+2);
  Output o; o.color = float4(COVERAGE_VALUE, GetRenderTargetSampleCount(), 1, 1);
  o.mask = n == 0 ? 0 : n == 1 ? ~0u : n < COUNT+2 ? 1u << (n-2) : ~(1u << (n-COUNT-2));
  return o;
}
float4 vs_read(uint id : SV_VertexID) : SV_Position {
  return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0, 1);
}
float4 ps_read(float4 p : SV_Position) : SV_Target {
  uint sid = uint(p.x) / WIDTH;
  return image.Load(uint2(uint(p.x) % WIDTH, uint(p.y)), sid);
}
float4 ps_color() : SV_Target { return 1; }
#ifndef LEGACY
void ps_uav(float4 p : SV_Position, uint coverage : SV_Coverage) {
  result[uint(p.y)*WIDTH+uint(p.x)] = float4(coverage,p.xy,1);
}
float4 ps_forced(float4 p : SV_Position, uint coverage : SV_Coverage) : SV_Target {
  return float4(coverage,p.xy,1);
}
[numthreads(1,1,1)] void cs(uint3 p : SV_DispatchThreadID) {
  for (uint sid = 0; sid < STORAGE_COUNT; sid++) result[(p.y*WIDTH+p.x)*STORAGE_COUNT+sid] = image.Load(p.xy,sid);
}
#endif
)hlsl";

inline std::vector<Case> cases() {
  std::vector<Case> out;
  auto c = base("fullscreen sample queries");
  c.vertices = {vertex(0, 0), vertex(width * 2, 0), vertex(0, height * 2)};
  out.push_back(c);
  for (unsigned step = 0; step < sample_grid; step++) {
    c = base("standard-pattern diagonal edge sweep");
    float offset = float(step) / sample_grid;
    c.vertices = {vertex(1 + offset, 1), vertex(width - 1 + offset, 1), vertex(1 + offset, height - 1)};
    out.push_back(c);
  }
  for (unsigned rotation = 0; rotation < Triangle; rotation++) {
    c = base("sample and centroid interpolation with unequal homogeneous w");
    const float values[] = {0.5f, 1, 2};
    for (unsigned i = 0; i < c.vertices.size(); i++) {
      auto p = project(c.vertices[i], c.viewport);
      c.vertices[i] = vertex(float(p.x), float(p.y), 0.5f, values[(i + rotation) % Triangle]);
    }
    out.push_back(c);
  }
  for (unsigned sx = 0; sx < sample_grid; sx++)
    for (unsigned sy = 0; sy < sample_grid; sy++) {
      c = base("point sample coverage");
      c.shape = Point;
      c.vertices = {vertex(width / 2 + float(sx) / sample_grid, height / 2 + float(sy) / sample_grid)};
      out.push_back(c);
    }
  for (unsigned component = 0; component < distances; component++) {
    c = base("clip distance at sample resolution");
    c.mode = Clip;
    c.vertices[0].distance[component] = -1;
    out.push_back(c);
  }
  for (int dx : {-2, -1, 0, 1, 2})
    for (int dy : {-2, -1, 0, 1, 2})
      if ((dx || dy) && std::gcd(std::abs(dx), std::abs(dy)) == 1)
        for (bool aa : {false, true}) {
          c = base("quadrilateral line octant and ignored alpha-AA flag");
          c.shape = Line;
          c.multisample = true;
          c.aa = aa;
          c.vertices = {vertex(width / 2 + 0.25f, height / 2 + 0.25f),
                        vertex(width / 2 + 0.25f + dx * (int(width) / 2 - 2) / 2,
                               height / 2 + 0.25f + dy * (int(height) / 2 - 2) / 2)};
          out.push_back(c);
          for (unsigned i = 0; i < c.vertices.size(); i++) {
            auto p = project(c.vertices[i], c.viewport);
            c.vertices[i] = vertex(float(p.x), float(p.y), 0.5f, i ? 2.0f : 0.5f);
          }
          c.name += " with unequal w";
          out.push_back(c);
        }
  const size_t size = out.size();
  for (size_t i = 0; i < size; i++)
    if (out[i].shape != Line) {
      auto c = out[i];
      c.multisample = true;
      c.name += " with ignored MultisampleEnable";
      out.push_back(std::move(c));
    }
  return out;
}
inline unsigned export_mask(unsigned n, unsigned count) {
  n %= 2 * count + 2;
  return n == 0 ? 0 : n == 1 ? ~0u : n < count + 2 ? 1u << (n - 2) : ~(1u << (n - count - 2));
}
struct Job {
  size_t geometry;
  unsigned mask;
  Pass pass;
};
inline std::vector<Job> jobs(unsigned count, bool centered = false) {
  std::vector<Job> out;
  auto geometry = cases();
  for (size_t i = 0; i < geometry.size(); i++)
    if (count > 1 || geometry[i].shape != Line)
      for (auto pass : {Shading, IndexOnly, ModifierOnly, Varying, Centroid, PerspectiveVarying, PerspectiveCentroid,
                        PositionDepth}) {
        // S19.2.4.1 forbids sample-frequency shading and quad lines with the CENTER pattern.
        if (centered && (geometry[i].shape == Line || (pass != Centroid && pass != PerspectiveCentroid)))
          continue;
        out.push_back({i, ~0u, pass});
        if (centered)
          out.push_back({i, ~0u, Export});
      }
  std::vector<unsigned> masks = {0, ~0u, (1u << count) - 1, 1u << count};
  for (unsigned sid = 0; sid < count; sid++) {
    masks.push_back(1u << sid);
    masks.push_back(~(1u << sid));
  }
  for (size_t i : {0u, 1u})
    for (unsigned mask : masks) {
      if (!centered)
        out.push_back({i, mask, Shading});
      out.push_back({i, mask, Export});
      out.push_back({i, mask, Centroid});
      out.push_back({i, mask, PerspectiveCentroid});
    }
  out.push_back({0, ~0u, Queries});
  out.push_back({0, ~0u, Unbound});
  return out;
}
inline std::vector<Job> forced_jobs(unsigned storage = 1) {
  std::vector<Job> out;
  auto geometry = cases();
  std::vector<unsigned> masks = {0, ~0u};
  for (unsigned sid = 0; sid < storage; sid++) {
    masks.push_back(1u << sid);
    masks.push_back(~(1u << sid));
  }
  for (size_t i = 0; i < geometry.size(); i++)
    if (geometry[i].shape != Line)
      for (unsigned mask : masks)
        out.push_back({i, mask, storage == 1 ? Forced : ForcedMsaa});
  return out;
}
inline void compare(const Color *got, const Case &c, unsigned count, unsigned mask, Pass pass, bool centered = false) {
  // a forced count with a bound 1x render target isn't implemented: Metal rasterizes at the attachment's one sample
  auto positions = pattern(pass == ForcedMsaa || pass == Forced ? 1 : count);
  if (centered)
    positions.assign(count, {0, 0});
  std::vector<std::vector<Color>> coverage;
  for (auto p : positions)
    coverage.push_back(expected(c, double(p[0]) / sample_grid, double(p[1]) / sample_grid));
  unsigned wrong = 0;
  for (unsigned y = 0; y < height; y++)
    for (unsigned x = 0; x < width; x++) {
      unsigned pixel = y * width + x, bits = 0;
      for (unsigned sid = 0; sid < positions.size(); sid++)
        if (coverage[sid][pixel][0])
          bits |= 1u << sid;
      if (pass != Forced && pass != ForcedMsaa && pass != Uav)
        bits &= mask;
      unsigned storage = pass == Forced || pass == Uav ? 1 : count;
      for (unsigned sid = 0; sid < storage; sid++) {
        Color want{};
        bool written = pass == Uav          ? bits != 0
                       : pass == Forced     ? bits && (mask & 1)
                       : pass == ForcedMsaa ? bits && (mask >> sid & 1)
                                            : bits >> sid & 1;
        if (pass == Export || pass == LegacyExport)
          written &= (export_mask(pixel, count) >> sid & 1) != 0;
        if (written) {
          if (pass == Forced || pass == ForcedMsaa || pass == Uav)
            want = {float(bits), x + 0.5f, y + 0.5f, 1};
          else if (pass == Shading || pass == IndexOnly || pass == ModifierOnly || pass == LegacyShading ||
                   pass == LegacyIndex || pass == LegacyModifier)
            want = {x + 0.5f + (pass == IndexOnly || pass == LegacyIndex ? 0 : float(positions[sid][0]) / sample_grid),
                    y + 0.5f + (pass == IndexOnly || pass == LegacyIndex ? 0 : float(positions[sid][1]) / sample_grid),
                    pass == ModifierOnly || pass == LegacyModifier ? 1 : float(sid + 1),
                    pass >= LegacyShading ? 1 : float(bits)};
          else if (pass == Export || pass == LegacyExport)
            want = {pass == LegacyExport ? 1 : float(bits), float(count), 1, 1};
          else if (pass == LegacyCoverage)
            want = {1, 1, 1, 1};
          else if (pass == PositionDepth)
            want = {coverage[sid][pixel][1], coverage[sid][pixel][2], x + 0.5f + float(positions[sid][0]) / sample_grid,
                    y + 0.5f + float(positions[sid][1]) / sample_grid};
          else if (pass == Varying || pass == Centroid || pass == PerspectiveVarying || pass == PerspectiveCentroid) {
            unsigned selected = sid;
            bool center = false;
            if (pass == Centroid || pass == PerspectiveCentroid) {
              center = bits == (1u << count) - 1;
              if (!center) {
                selected = 0;
                while (!(bits >> selected & 1))
                  selected++;
              }
            }
            double px = x + 0.5 + (center ? 0 : double(positions[selected][0]) / sample_grid);
            double py = y + 0.5 + (center ? 0 : double(positions[selected][1]) / sample_grid);
            if (c.shape == Point) {
              auto p = project(c.vertices[0], c.viewport);
              px = p.x;
              py = p.y;
            } else if (c.shape == Line) {
              auto a = project(c.vertices[0], c.viewport), b = project(c.vertices[1], c.viewport);
              double dx = b.x - a.x, dy = b.y - a.y;
              double t = ((px - a.x) * dx + (py - a.y) * dy) / (dx * dx + dy * dy);
              px = a.x + t * dx;
              py = a.y + t * dy;
            }
            want = {float(px / width * 2 - 1), float(1 - py / height * 2), x + 0.5f, y + 0.5f};
            if (pass == PerspectiveVarying || pass == PerspectiveCentroid) {
              std::vector<Screen> points;
              for (auto &v : c.vertices)
                points.push_back(project(v, c.viewport));
              std::vector<double> weights(c.shape, 1);
              if (c.shape == Triangle) {
                double area = edge(points[0], points[1], points[2].x, points[2].y);
                for (unsigned i = 0; i < Triangle; i++)
                  weights[i] = edge(points[(i + 1) % Triangle], points[(i + 2) % Triangle], px, py) / area;
              } else if (c.shape == Line) {
                auto a = points[0], b = points[1];
                double dx = b.x - a.x, dy = b.y - a.y;
                weights[1] = ((px - a.x) * dx + (py - a.y) * dy) / (dx * dx + dy * dy);
                weights[0] = 1 - weights[1];
              }
              double denominator = 0, u = 0, v = 0;
              for (unsigned i = 0; i < c.shape; i++) {
                double coefficient = weights[i] * points[i].invw;
                denominator += coefficient;
                u += coefficient * (c.vertices[i].position[0] / c.vertices[i].position[3]);
                v += coefficient * (c.vertices[i].position[1] / c.vertices[i].position[3]);
              }
              want[0] = float(u / denominator);
              want[1] = float(v / denominator);
            }
          } else if (pixel < count) {
            want = {float(positions[pixel][0]) / sample_grid, float(positions[pixel][1]) / sample_grid,
                    pass == Unbound ? 0 : float(positions[pixel][0]) / sample_grid,
                    pass == Unbound ? 0 : float(positions[pixel][1]) / sample_grid};
          }
        }
        auto &actual = got[pixel * storage + sid];
        for (unsigned lane = 0; lane < lanes; lane++)
          if (std::abs(actual[lane] - want[lane]) >
                  (2 * Triangle + 1) * std::numeric_limits<float>::epsilon() * std::max(1.0f, std::abs(want[lane])) ||
              !std::isfinite(actual[lane]))
            if (wrong++ < 4)
              expect(false, "sample %u pixel %u,%u lane %u = %g, want %g", sid, x, y, lane, actual[lane], want[lane]);
      }
    }
}
} // namespace raster_samples
