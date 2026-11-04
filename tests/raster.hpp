// contract: D3D11.3 3.4.2: "Any pixel sample locations which fall inside the triangle are drawn."
// 3.4.2.1: "the sample is inside the triangle if the edge is a \"top\" edge or a \"left\" edge."
// 15.4.1: "Primitive setup only invokes rasterization on pixels for which the interpolated plane distance(s)
// are >= 0." 15.4.2: "If a vertex has a cull distance of NaN, that vertex counts as \"out\" (as if it is < 0)."
// 15.7: "The implicit scissor to the viewport (mentioned in the Viewport(15.6) section) rounds the viewport X and Y
// extents to negative infinity." source: D3D11_3_FunctionalSpec.htm.
// clipping is homogeneous; edge tests use the specified 8-bit fixed-point grid, independently of Metal.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

namespace raster {
constexpr unsigned width = 16, height = 16;
constexpr unsigned subpixel_bits = 8; // D3D11.3 15.16, exactly eight fractional bits
constexpr unsigned sample_grid = 16;  // D3D11.3 19.2.4, sample offsets in sixteenths
constexpr unsigned distances = 8;     // D3D11.3 15.4.3, two four-component registers
constexpr unsigned lanes = 4;
using Color = std::array<float, lanes>;
struct Vertex {
  Color position;
  std::array<float, distances> distance;
};
enum Shape { Point = 1, Line, Triangle };
enum Distances {
  None,
  Clip,
  Cull,
  Mixed,
  PackedMixed,
  Clip1Cull7,
  Clip2Cull6,
  Clip3Cull5,
  Clip5Cull3,
  Clip6Cull2,
  Clip7Cull1,
  ModeCount
};
constexpr unsigned clip_count(Distances mode) {
  switch (mode) {
  case Clip:
    return distances;
  case Mixed:
  case PackedMixed:
    return distances / 2;
  case Clip1Cull7:
    return 1;
  case Clip2Cull6:
    return 2;
  case Clip3Cull5:
    return 3;
  case Clip5Cull3:
    return 5;
  case Clip6Cull2:
    return 6;
  case Clip7Cull1:
    return 7;
  default:
    return 0;
  }
}
enum CullMode { NoCull, CullFront, CullBack };
struct Viewport {
  float x = 0, y = 0, w = width, h = height, low = 0, high = 1;
};
struct Rect {
  int left = 0, top = 0, right = width, bottom = height;
};
struct Case {
  std::string name;
  std::vector<Vertex> vertices;
  Shape shape = Triangle;
  Distances mode = None;
  CullMode cull = NoCull;
  bool ccw = false, wire = false, depth_clip = true, scissor = true, multisample = false, aa = false, additive = false,
       geometry = false, default_state = false, strip = false, conservative = false, inner = false, clear_state = false;
  Viewport viewport;
  Rect rect;
  unsigned instances = 1;
  float bias_scale = 0, bias_clamp = 0;
};

static const char hlsl[] = R"hlsl(
struct Input {
  float4 position : POSITION;
#if DISTANCES != 0
  float4 d0 : DISTANCE0; float4 d1 : DISTANCE1;
#endif
};
struct V {
  float4 position : SV_Position;
  float2 uv : TEXCOORD0;
#if DISTANCES == 1
  float4 d0 : SV_ClipDistance0; float4 d1 : SV_ClipDistance1;
#elif DISTANCES == 2
  float4 d0 : SV_CullDistance0; float4 d1 : SV_CullDistance1;
#elif DISTANCES == 3
  float4 d0 : SV_ClipDistance0; float4 d1 : SV_CullDistance0;
#elif DISTANCES == 4
  float3 clip0 : SV_ClipDistance0; float clip1 : SV_ClipDistance1;
  float cull0 : SV_CullDistance0; float3 cull1 : SV_CullDistance1;
#elif DISTANCES == 5
  float clip0 : SV_ClipDistance0; float3 cull0 : SV_CullDistance0; float4 cull1 : SV_CullDistance1;
#elif DISTANCES == 6
  float2 clip0 : SV_ClipDistance0; float2 cull0 : SV_CullDistance0; float4 cull1 : SV_CullDistance1;
#elif DISTANCES == 7
  float3 clip0 : SV_ClipDistance0; float cull0 : SV_CullDistance0; float4 cull1 : SV_CullDistance1;
#elif DISTANCES == 8
  float4 clip0 : SV_ClipDistance0; float clip1 : SV_ClipDistance1; float3 cull0 : SV_CullDistance0;
#elif DISTANCES == 9
  float4 clip0 : SV_ClipDistance0; float2 clip1 : SV_ClipDistance1; float2 cull0 : SV_CullDistance0;
#elif DISTANCES == 10
  float4 clip0 : SV_ClipDistance0; float3 clip1 : SV_ClipDistance1; float cull0 : SV_CullDistance0;
#endif
};
V vs(Input i) {
  V v; v.position = i.position; v.uv = i.position.xy / i.position.w;
#if DISTANCES == 4
  v.clip0=i.d0.xyz;v.clip1=i.d0.w;v.cull0=i.d1.x;v.cull1=i.d1.yzw;
#elif DISTANCES == 5
  v.clip0=i.d0.x;v.cull0=i.d0.yzw;v.cull1=i.d1;
#elif DISTANCES == 6
  v.clip0=i.d0.xy;v.cull0=i.d0.zw;v.cull1=i.d1;
#elif DISTANCES == 7
  v.clip0=i.d0.xyz;v.cull0=i.d0.w;v.cull1=i.d1;
#elif DISTANCES == 8
  v.clip0=i.d0;v.clip1=i.d1.x;v.cull0=i.d1.yzw;
#elif DISTANCES == 9
  v.clip0=i.d0;v.clip1=i.d1.xy;v.cull0=i.d1.zw;
#elif DISTANCES == 10
  v.clip0=i.d0;v.clip1=i.d1.xyz;v.cull0=i.d1.w;
#elif DISTANCES != 0
  v.d0 = i.d0; v.d1 = i.d1;
#endif
  return v;
}
[maxvertexcount(1)] void gs_point(point V input[1], inout PointStream<V> stream) { stream.Append(input[0]); }
[maxvertexcount(2)] void gs_line(line V input[2], inout LineStream<V> stream) { stream.Append(input[0]);stream.Append(input[1]); }
[maxvertexcount(3)] void gs_triangle(triangle V input[3], inout TriangleStream<V> stream) {
  stream.Append(input[0]);stream.Append(input[1]);stream.Append(input[2]);
}
float4 ps(float4 p : SV_Position, bool face : SV_IsFrontFace) : SV_Target {
  return float4(face ? 1 : 2, p.z, p.w, 1);
}
)hlsl";

inline Vertex vertex(float x, float y, float z = 0.5f, float w = 1) {
  Vertex v{{(x / width * 2 - 1) * w, (1 - y / height * 2) * w, z * w, w}, {}};
  v.distance.fill(1);
  return v;
}
inline Case base(std::string name = "triangle") {
  return {std::move(name), {vertex(2, 2), vertex(width - 2, 2), vertex(2, height - 2)}};
}
inline std::vector<Case> viewport_boundaries(float minimum, float maximum) {
  std::vector<Case> out;
  // 15.6.1 constrains both origins and origin+extent; API headers supply each feature level's bounds.
  for (bool horizontal : {false, true}) {
    for (float extent : {0.0f, 1.0f, maximum - minimum - 1, maximum - minimum}) {
      auto c = base("viewport maximum extent at minimum origin");
      (horizontal ? c.viewport.x : c.viewport.y) = minimum;
      (horizontal ? c.viewport.w : c.viewport.h) = extent;
      c.vertices = {vertex(0, 0), vertex(width * 2, 0), vertex(0, height * 2)};
      out.push_back(c);
    }
    for (float origin : {minimum, minimum + 1, maximum - 1, maximum}) {
      auto c = base("viewport origin boundary");
      (horizontal ? c.viewport.x : c.viewport.y) = origin;
      (horizontal ? c.viewport.w : c.viewport.h) = std::min(float(width), maximum - origin);
      out.push_back(c);
    }
  }
  return out;
}
inline std::vector<Case> reset_cases() {
  std::vector<Case> out;
  for (bool empty_viewport : {false, true})
    for (bool outside_depth : {false, true}) {
      auto c = base("ClearState resets raster state and removes all viewport/scissor bindings");
      c.clear_state = c.default_state = true;
      c.cull = CullBack;
      c.scissor = false;
      c.rect.right = c.rect.bottom = 0;
      if (empty_viewport)
        c.viewport.w = c.viewport.h = 0;
      if (outside_depth)
        for (auto &v : c.vertices)
          v.position[2] = 2;
      out.push_back(c);
    }
  return out;
}

inline std::vector<Case> cases(bool exact_precision = true) {
  std::vector<Case> out;
  auto add = [&](Case c) { out.push_back(std::move(c)); };
  for (bool wire : {false, true})
    for (auto cull : {NoCull, CullFront, CullBack})
      for (bool ccw : {false, true})
        for (bool reverse : {false, true}) {
          auto c = base("fill/cull/front/winding");
          c.wire = wire;
          c.cull = cull;
          c.ccw = ccw;
          if (reverse)
            std::swap(c.vertices[1], c.vertices[2]);
          add(c);
        }
  const float grid = float(1u << subpixel_bits);
  // D3D10 permits greater precision and truncation; only D3D11+ requires these snapped faces (15.16).
  if (exact_precision)
    for (float phase : {0.0f, 1 / grid})
      for (float fraction : {0.25f, 0.5f, 0.75f})
        for (auto cull : {NoCull, CullFront, CullBack})
          for (bool ccw : {false, true})
            for (bool reverse : {false, true}) {
              auto c = base("post-snap degeneracy, ties and face culling");
              c.wire = true;
              c.cull = cull;
              c.ccw = ccw;
              const float x = width / 2 + phase;
              c.vertices = {vertex(x, 2), vertex(x + fraction / grid, height / 2), vertex(x, height - 2)};
              if (reverse)
                std::swap(c.vertices[1], c.vertices[2]);
              add(c);
            }
  for (float offset : {-1 / grid, 0.0f, 1 / grid, 0.5f})
    for (bool reverse : {false, true}) {
      auto c = base("top-left diagonal and subpixel boundary");
      c.vertices = {vertex(2.5f + offset, 2.5f), vertex(width - 2.5f + offset, 2.5f),
                    vertex(2.5f + offset, height - 2.5f)};
      if (reverse)
        std::swap(c.vertices[1], c.vertices[2]);
      add(c);
      c.vertices = {vertex(width - 2.5f + offset, 2.5f), vertex(width - 2.5f + offset, height - 2.5f),
                    vertex(2.5f + offset, height - 2.5f)};
      add(c);
    }
  for (float offset : {-1 / grid, 0.0f, 1 / grid, 0.5f})
    for (bool reverse : {false, true}) {
      auto c = base("shared edge has neither gaps nor double writes");
      c.additive = true;
      c.vertices = {vertex(2.5f + offset, 2.5f),
                    vertex(width - 2.5f + offset, 2.5f),
                    vertex(2.5f + offset, height - 2.5f),
                    vertex(width - 2.5f + offset, 2.5f),
                    vertex(width - 2.5f + offset, height - 2.5f),
                    vertex(2.5f + offset, height - 2.5f)};
      if (reverse)
        for (unsigned first = 0; first < c.vertices.size(); first += Triangle)
          std::swap(c.vertices[first + 1], c.vertices[first + 2]);
      add(c);
    }
  for (Shape shape : {Point, Line, Triangle})
    for (auto cull : {NoCull, CullFront, CullBack}) {
      auto c = base("empty and degenerate primitive");
      c.shape = shape;
      c.cull = cull;
      c.vertices.resize(shape);
      c.vertices.assign(shape, vertex(width / 2, height / 2));
      add(c);
      c.vertices.clear();
      add(c);
      c = base("zero instance count");
      c.instances = 0;
      add(c);
    }
  for (auto cull : {NoCull, CullFront, CullBack})
    for (bool ccw : {false, true}) {
      auto c = base("collinear wireframe is back facing");
      c.wire = true;
      c.cull = cull;
      c.ccw = ccw;
      c.vertices = {vertex(2, 4), vertex(width / 2, 4), vertex(width - 2, 4)};
      add(c);
    }
  for (Shape shape : {Point, Line, Triangle})
    for (bool clip : {false, true})
      for (float z : {-1.0f, 0.0f, 0.5f, 1.0f, 2.0f}) {
        auto c = base("near/far clipping and unclamped PS depth");
        c.shape = shape;
        c.vertices.resize(shape);
        c.depth_clip = clip;
        c.viewport.low = 0.25f;
        c.viewport.high = 0.75f;
        for (auto &v : c.vertices)
          v.position[2] = z;
        add(c);
        c.vertices[0].position[2] = 0.5f;
        add(c);
      }
  for (Shape shape : {Point, Line, Triangle})
    for (bool depth_clip : {false, true})
      for (float w : {0.0f, -1.0f, 0.25f, 2.0f}) {
        auto c = base("homogeneous w and perspective divide");
        c.vertices = {vertex(2, 2, 0.5f, w), vertex(width - 2, 2, 0.5f, w), vertex(2, height - 2, 0.5f, w)};
        c.shape = shape;
        c.depth_clip = depth_clip;
        c.vertices.resize(shape);
        add(c);
      }
  {
    auto c = base("unequal homogeneous w interpolation");
    c.vertices = {vertex(2, 2, 0.25f, 0.5f), vertex(width - 2, 2, 0.75f, 2), vertex(2, height - 2, 0.5f, 1)};
    add(c);
    c = base("crossing the positive-w plane");
    c.vertices[0].position[3] = -1;
    add(c);
    c.depth_clip = false;
    add(c);
  }
  for (unsigned corner = 0; corner < Triangle; corner++)
    for (unsigned component = 0; component < lanes; component++) {
      auto c = base("NaN position discards primitive");
      c.vertices[corner].position[component] = std::numeric_limits<float>::quiet_NaN();
      add(c);
    }
  for (bool clip : {false, true}) {
    auto c = base("large finite guard-band triangle");
    c.depth_clip = clip;
    const float extent = float(1u << (subpixel_bits * 2));
    c.vertices = {vertex(-extent, -extent), vertex(extent, -extent), vertex(0, extent)};
    add(c);
  }
  for (float origin : {-0.25f, 0.25f, 0.75f})
    for (bool scissor : {false, true}) {
      auto c = base("fractional origin implicit floor scissor");
      c.scissor = scissor;
      c.viewport.x = c.viewport.y = origin;
      c.vertices = {vertex(-float(width), -float(height)), vertex(width * 3, -float(height)),
                    vertex(-float(width), height * 3)};
      // crop the permitted nondeterministic strip at the left/top (15.6.1), keeping right/bottom checked.
      if (scissor)
        c.rect.left = c.rect.top = 1;
      add(c);
    }
  for (float origin : {-2.0f, -1.0f, 0.0f, 1.0f, 2.0f})
    for (float extent : {0.0f, 0.25f, 1.0f, float(width) - 0.25f, float(width) + 0.25f}) {
      auto c = base("integer origin and fractional extents");
      c.viewport.x = c.viewport.y = origin;
      c.viewport.w = c.viewport.h = extent;
      c.vertices = {vertex(-float(width), -float(height)), vertex(width * 3, -float(height)),
                    vertex(-float(width), height * 3)};
      add(c);
    }
  for (float depth : {0.0f, 0.25f, 0.5f, 1.0f}) {
    auto c = base("equal viewport depth endpoints");
    c.viewport.low = c.viewport.high = depth;
    add(c);
  }
  const Rect rects[] = {{},
                        {0, 0, 0, int(height)},
                        {0, 0, int(width), 0},
                        {-int(width), -int(height), 1, 1},
                        {1, 1, int(width) + 1, int(height) + 1},
                        {int(width), int(height), int(width) * 2, int(height) * 2},
                        {std::numeric_limits<int>::min(), std::numeric_limits<int>::min(),
                         std::numeric_limits<int>::max(), std::numeric_limits<int>::max()}};
  for (const auto &rect : rects) {
    auto c = base("scissor bounds");
    c.rect = rect;
    c.vertices = {vertex(0, 0), vertex(width * 2, 0), vertex(0, height * 2)};
    add(c);
  }
  for (auto mode :
       {Clip, Cull, Mixed, PackedMixed, Clip1Cull7, Clip2Cull6, Clip3Cull5, Clip5Cull3, Clip6Cull2, Clip7Cull1})
    for (Shape shape : {Point, Line, Triangle}) {
      unsigned clips = clip_count(mode);
      for (unsigned component = 0; component < distances; component++) {
        for (unsigned signs = 0; signs < (1u << shape); signs++) {
          auto c = base("distance component/sign distribution");
          c.mode = mode;
          c.shape = shape;
          c.vertices.resize(shape);
          for (unsigned i = 0; i < c.vertices.size(); i++)
            c.vertices[i].distance[component] = signs >> i & 1 ? -1 : 1;
          add(c);
        }
        for (float value :
             {-0.0f, 0.0f, std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()}) {
          auto c = base("distance uniform special value");
          c.mode = mode;
          c.shape = shape;
          c.vertices.resize(shape);
          for (auto &v : c.vertices)
            v.distance[component] = value;
          add(c);
        }
        for (unsigned corner = 0; corner < unsigned(shape); corner++) {
          auto c = base("distance NaN in each vertex");
          c.mode = mode;
          c.shape = shape;
          c.vertices.resize(shape);
          if (component >= clips)
            for (auto &v : c.vertices)
              v.distance[component] = -1;
          c.vertices[corner].distance[component] = std::numeric_limits<float>::quiet_NaN();
          add(c);
          if (component >= clips) {
            c.vertices[(corner + 1) % shape].distance[component] = -0.0f;
            add(c);
          }
        }
      }
    }
  for (Shape shape : {Point, Line})
    for (auto cull : {NoCull, CullFront, CullBack})
      for (bool wire : {false, true})
        for (bool multisample : {false, true}) {
          auto c = base("point/line unaffected by fill/cull");
          c.shape = shape;
          c.cull = cull;
          c.wire = wire;
          c.multisample = multisample;
          c.vertices = shape == Point ? std::vector<Vertex>{vertex(5.25f, 6.75f)}
                                      : std::vector<Vertex>{vertex(2.25f, 3.25f), vertex(12.25f, 9.25f)};
          add(c);
        }
  // 3.4.5: the quadrilateral's 1.4 pixel width is measured in the viewport's own pixels on each axis.
  for (bool tall : {false, true}) {
    auto c = base("quadrilateral line in a non-square viewport");
    c.shape = Line;
    c.multisample = true;
    (tall ? c.viewport.w : c.viewport.h) /= 2;
    c.vertices = {vertex(2.25f, 3.25f), vertex(12.25f, 9.25f)};
    add(c);
  }
  for (int dx : {-2, -1, 0, 1, 2})
    for (int dy : {-2, -1, 0, 1, 2})
      if ((dx || dy) && std::gcd(std::abs(dx), std::abs(dy)) == 1)
        for (float phase : {0.0f, 0.25f, 0.5f, 0.75f}) {
          auto c = base("diamond-exit line direction and endpoint");
          c.shape = Line;
          const float cx = width / 2 + phase, cy = height / 2 + phase;
          const float length = float(std::min(width, height) / 2 - 2) / 2;
          c.vertices = {vertex(cx, cy, 0.25f, 0.5f), vertex(cx + dx * length, cy + dy * length, 0.75f, 2)};
          add(c);
        }
  // 15.13: "Wireframe rendering of triangle strips is no different than drawing each triangle independently".
  for (bool reverse : {false, true})
    for (auto cull : {NoCull, CullFront, CullBack}) {
      auto c = base("wireframe strip expands to independently oriented triangles");
      c.strip = true;
      c.wire = true;
      c.cull = cull;
      c.vertices = {vertex(2.25f, 2.25f), vertex(2.25f, height - 2.25f), vertex(width - 2.25f, 2.25f),
                    vertex(width - 2.25f, height - 2.25f)};
      if (reverse)
        std::reverse(c.vertices.begin(), c.vertices.end());
      add(c);
    }
  for (bool reverse : {false, true})
    for (bool vertical : {false, true}) {
      auto c = base("diamond-exit line strip shared endpoint is written once");
      c.shape = Line;
      c.strip = true;
      c.additive = true;
      c.vertices = {vertex(2.5f, height / 2 + 0.5f), vertex(width / 2 + 0.5f, height / 2 + 0.5f),
                    vertex(width - 2.5f, height / 2 + 0.5f)};
      if (vertical)
        for (auto &v : c.vertices)
          std::swap(v.position[0], v.position[1]);
      if (reverse)
        std::reverse(c.vertices.begin(), c.vertices.end());
      add(c);
    }
  for (bool wire : {false, true}) {
    auto c = base("clipped wireframe retains only original edge fragments");
    c.wire = wire;
    c.mode = Clip;
    c.vertices[0].distance[0] = -1;
    add(c);
  }
  for (bool clip : {false, true}) {
    auto c = base("line depth clipping");
    c.shape = Line;
    c.depth_clip = clip;
    c.vertices = {vertex(2.25f, 3.25f, 2), vertex(12.25f, 9.25f, 2)};
    add(c);
  }
  for (bool reverse : {false, true}) {
    auto c = base("null raster state uses solid/back-cull/front-clockwise/depth-clip/scissor-off defaults");
    c.default_state = true;
    c.cull = CullBack;
    c.scissor = false;
    c.rect = {};
    c.rect.right = c.rect.bottom = 0;
    if (reverse)
      std::swap(c.vertices[1], c.vertices[2]);
    add(c);
    c.vertices[0].position[2] = 2;
    add(c);
  }
  {
    auto c = base("null raster state uses aliased lines");
    c.default_state = true;
    c.scissor = false;
    c.shape = Line;
    c.vertices = {vertex(2.25f, 3.25f), vertex(12.25f, 9.25f)};
    add(c);
  }
  // 15.10 applies the same slope/clamp formula without a DSV; a zero integer bias avoids an unspecified r.
  for (auto shape : {Point, Line, Triangle})
    for (bool wire : {false, true})
      for (float slope : {-1.0f, 0.0f, 1.0f})
        for (float scale : {-float(width * height), -2.0f, 0.0f, 2.0f, float(width * height)})
          for (float clamp : {-1.0f, 0.0f, 1.0f}) {
            auto c = base("no-DSV slope bias and signed clamp before PS input");
            c.shape = shape;
            c.wire = wire;
            c.vertices.resize(shape);
            const float unit = 1.0f / (width * height);
            c.bias_scale = scale;
            c.bias_clamp = clamp * unit;
            for (auto &v : c.vertices) {
              float x = (v.position[0] / v.position[3] + 1) * width * 0.5f;
              v.position[2] = (0.5f + slope * unit * x) * v.position[3];
            }
            add(c);
          }
  const auto direct_cases = out.size();
  for (size_t i = 0; i < direct_cases; i++) {
    auto c = out[i];
    c.geometry = true;
    c.name += " through GS";
    out.push_back(std::move(c));
  }
  return out;
}

struct Screen {
  double x, y, z, invw;
};
inline Screen project(const Vertex &v, const Viewport &vp) {
  const double grid = 1u << subpixel_bits, w = v.position[3];
  float x = (v.position[0] / float(w) + 1) * vp.w * 0.5f + vp.x;
  float y = (1 - v.position[1] / float(w)) * vp.h * 0.5f + vp.y;
  return {std::nearbyint(x * grid) / grid, std::nearbyint(y * grid) / grid,
          vp.low + v.position[2] / w * (vp.high - vp.low), 1 / w};
}
inline double edge(const Screen &a, const Screen &b, double x, double y) {
  return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
}
inline bool triangle(std::array<Screen, 3> v, double x, double y, Color &result, bool front) {
  double area = edge(v[0], v[1], v[2].x, v[2].y);
  if (!area)
    return false;
  if (area < 0) {
    std::swap(v[1], v[2]);
    area = -area;
  }
  for (unsigned i = 0; i < Triangle; i++) {
    auto &a = v[i], &b = v[(i + 1) % Triangle];
    double e = edge(a, b, x, y), dy = b.y - a.y, dx = b.x - a.x;
    if (e < 0 || (e == 0 && !(dy < 0 || (dy == 0 && dx > 0))))
      return false;
  }
  double a = edge(v[1], v[2], x, y) / area, b = edge(v[2], v[0], x, y) / area, c = 1 - a - b;
  result = {front ? 1.0f : 2.0f, float(a * v[0].z + b * v[1].z + c * v[2].z),
            float(1 / (a * v[0].invw + b * v[1].invw + c * v[2].invw)), 1};
  return true;
}
inline Vertex interpolate(const Vertex &a, const Vertex &b, double t) {
  Vertex v;
  for (unsigned i = 0; i < lanes; i++)
    v.position[i] = float(a.position[i] + t * (b.position[i] - a.position[i]));
  for (unsigned i = 0; i < distances; i++)
    v.distance[i] =
        a.distance[i] == b.distance[i] ? a.distance[i] : float(a.distance[i] + t * (b.distance[i] - a.distance[i]));
  return v;
}
template <typename Plane> inline std::vector<Vertex> clip_polygon(const std::vector<Vertex> &in, Plane plane) {
  std::vector<Vertex> out;
  if (in.empty())
    return out;
  auto a = in.back();
  double da = plane(a);
  for (const auto &b : in) {
    double db = plane(b);
    if ((da >= 0) != (db >= 0))
      out.push_back(interpolate(a, b, da / (da - db)));
    if (db >= 0)
      out.push_back(b);
    a = b;
    da = db;
  }
  return out;
}
template <typename Apply> inline void planes(const Case &c, Apply apply) {
  apply([](const auto &v) { return double(v.position[3]); });
  for (unsigned axis = 0; axis < 2; axis++)
    for (int sign : {-1, 1})
      apply([&](const auto &v) { return double(v.position[3]) + sign * double(v.position[axis]); });
  if (c.depth_clip) {
    apply([](const auto &v) { return double(v.position[2]); });
    apply([](const auto &v) { return double(v.position[3]) - v.position[2]; });
  }
  unsigned clips = clip_count(c.mode);
  for (unsigned d = 0; d < clips; d++)
    apply([&](const auto &v) { return double(v.distance[d]); });
}
inline bool clip_segment(const Case &c, Vertex &a, Vertex &b) {
  bool visible = true;
  planes(c, [&](auto plane) {
    if (!visible)
      return;
    double da = plane(a), db = plane(b);
    if (da < 0 && db < 0)
      visible = false;
    else if ((da >= 0) != (db >= 0)) {
      auto intersection = interpolate(a, b, da / (da - db));
      if (da < 0)
        a = intersection;
      else
        b = intersection;
    }
  });
  return visible && a.position[3] > 0 && b.position[3] > 0;
}
inline bool diamond(const Screen &a, const Screen &b, double x, double y) {
  // Rules: lower edges and bottom corner are included; y-major diamonds also include the right corner.
  double lo = 0, hi = 1;
  for (int sx : {-1, 1})
    for (int sy : {-1, 1}) {
      double d = sx * (a.x - x) + sy * (a.y - y) - 0.5, slope = sx * (b.x - a.x) + sy * (b.y - a.y);
      if (!slope) {
        if (d > 0 || (d == 0 && sy < 0))
          return false;
      } else if (slope > 0)
        hi = std::min(hi, -d / slope);
      else
        lo = std::max(lo, -d / slope);
    }
  auto inside = [&](double t) {
    double u = a.x + t * (b.x - a.x) - x, v = a.y + t * (b.y - a.y) - y;
    if (std::abs(u) + std::abs(v) < 0.5)
      return true;
    if (std::abs(u) + std::abs(v) != 0.5)
      return false;
    return v > 0 || (u == 0.5 && std::abs(b.y - a.y) > std::abs(b.x - a.x));
  };
  return lo <= hi && hi >= 0 && (hi < 1 || !inside(1)) && inside((lo + hi) / 2);
}
inline bool line(const Screen &a, const Screen &b, double x, double y, bool quad, Color &result, bool front) {
  double dx = b.x - a.x, dy = b.y - a.y, length = std::hypot(dx, dy);
  if (!length)
    return false;
  if (quad) {
    // D3D11.3 3.4.5, quadrilateral width is 1.4 pixels before fixed-point snapping.
    constexpr double line_width = 1.4;
    double nx = -dy / length * line_width / 2, ny = dx / length * line_width / 2, grid = 1u << subpixel_bits;
    auto offset = [&](Screen v, double sign) {
      v.x = std::nearbyint((v.x + sign * nx) * grid) / grid;
      v.y = std::nearbyint((v.y + sign * ny) * grid) / grid;
      return v;
    };
    return triangle({offset(a, 1), offset(b, 1), offset(a, -1)}, x, y, result, front) ||
           triangle({offset(a, -1), offset(b, 1), offset(b, -1)}, x, y, result, front);
  }
  if (!diamond(a, b, x, y))
    return false;
  double t = ((x - a.x) * dx + (y - a.y) * dy) / (length * length);
  result = {front ? 1.0f : 2.0f, float(a.z + t * (b.z - a.z)), float(1 / (a.invw + t * (b.invw - a.invw))), 1};
  return true;
}

inline std::vector<Color> expected(const Case &c, double ox = 0, double oy = 0) {
  std::vector<Color> pixels(width * height);
  if (!c.instances || c.vertices.empty() || c.viewport.w == 0 || c.viewport.h == 0)
    return pixels;
  const unsigned clips = clip_count(c.mode);
  for (size_t first = 0; first + c.shape <= c.vertices.size(); first += c.strip ? 1 : c.shape) {
    std::vector<Vertex> polygon(c.vertices.begin() + first, c.vertices.begin() + first + c.shape);
    if (c.strip && c.shape == Triangle && first % 2)
      std::swap(polygon[0], polygon[1]);
    bool discard = false;
    for (auto &v : polygon)
      for (auto value : v.position)
        discard |= std::isnan(value);
    for (unsigned d = 0; d < distances; d++) {
      if (d < clips) {
        for (auto &v : polygon)
          discard |= std::isnan(v.distance[d]);
      } else if (c.mode != None && clips < distances) {
        discard |= std::all_of(polygon.begin(), polygon.end(), [&](const auto &v) { return !(v.distance[d] >= 0); });
      }
    }
    discard |= std::all_of(polygon.begin(), polygon.end(), [](const auto &v) { return v.position[3] <= 0; });
    if (discard)
      continue;
    auto original = polygon;
    float bias = 0;
    if (c.shape == Triangle && c.bias_scale != 0) {
      auto a = project(original[0], c.viewport), b = project(original[1], c.viewport),
           d = project(original[2], c.viewport);
      double area = edge(a, b, d.x, d.y);
      double dx = ((b.z - a.z) * (d.y - a.y) - (d.z - a.z) * (b.y - a.y)) / area;
      double dy = ((b.x - a.x) * (d.z - a.z) - (d.x - a.x) * (b.z - a.z)) / area;
      bias = c.bias_scale * float(std::max(std::abs(dx), std::abs(dy)));
      if (c.bias_clamp > 0)
        bias = std::min(c.bias_clamp, bias);
      else if (c.bias_clamp < 0)
        bias = std::max(c.bias_clamp, bias);
    }
    if (c.shape != Line)
      planes(c, [&](auto plane) { polygon = clip_polygon(polygon, plane); });
    polygon.erase(std::remove_if(polygon.begin(), polygon.end(), [](const auto &v) { return v.position[3] <= 0; }),
                  polygon.end());
    if (polygon.empty())
      continue;
    if (c.shape == Triangle && polygon.size() < Triangle)
      continue;
    bool front = true;
    if (c.shape == Triangle) {
      auto a = project(polygon[0], c.viewport), b = project(polygon[1], c.viewport),
           d = project(polygon[2], c.viewport);
      double area = edge(a, b, d.x, d.y);
      front = area != 0 && (area < 0) == c.ccw;
      if ((c.cull == CullFront && front) || (c.cull == CullBack && !front))
        continue;
    }
    for (unsigned y = 0; y < height; y++)
      for (unsigned x = 0; x < width; x++) {
        if (x < std::floor(c.viewport.x) || x >= std::floor(c.viewport.x + c.viewport.w) ||
            y < std::floor(c.viewport.y) || y >= std::floor(c.viewport.y + c.viewport.h))
          continue;
        if (c.scissor &&
            (int(x) < c.rect.left || int(x) >= c.rect.right || int(y) < c.rect.top || int(y) >= c.rect.bottom))
          continue;
        double px = x + 0.5 + ox, py = y + 0.5 + oy;
        Color value{};
        bool hit = false;
        if (c.shape == Triangle && !c.wire) {
          for (unsigned i = 1; i + 1 < polygon.size(); i++)
            if (triangle({project(polygon[0], c.viewport), project(polygon[i], c.viewport),
                          project(polygon[i + 1], c.viewport)},
                         px, py, value, front)) {
              hit = true;
              break;
            }
        } else if (c.shape == Point) {
          auto p = project(polygon[0], c.viewport);
          hit = px >= p.x - 0.5 && px < p.x + 0.5 && py >= p.y - 0.5 && py < p.y + 0.5 &&
                (!c.depth_clip || (p.z >= c.viewport.low && p.z <= c.viewport.high));
          value = {1, float(p.z), float(1 / p.invw), 1};
        } else {
          for (unsigned i = 0; i < unsigned(c.shape == Triangle ? Triangle : 1); i++) {
            auto a = original[i], b = original[(i + 1) % original.size()];
            if (clip_segment(c, a, b) &&
                line(project(a, c.viewport), project(b, c.viewport), px, py, c.multisample, value, front)) {
              hit = true;
              break;
            }
          }
        }
        if (hit) {
          value[1] += bias;
          if (c.additive)
            for (unsigned lane = 0; lane < lanes; lane++)
              pixels[y * width + x][lane] += value[lane];
          else
            pixels[y * width + x] = value;
        }
      }
  }
  return pixels;
}

inline bool comparable(const Case &c, unsigned x, unsigned y) {
  // 15.6.1 permits a half-pixel uncertainty strip at fractional left/top viewport extents.
  return !((c.viewport.x != std::floor(c.viewport.x) && x + 0.5 < c.viewport.x) ||
           (c.viewport.y != std::floor(c.viewport.y) && y + 0.5 < c.viewport.y));
}
inline bool reference_contract() {
  Color result;
  std::array<Screen, 3> v = {Screen{0, 0, 0.5, 1}, Screen{2, 0, 0.5, 1}, Screen{0, 2, 0.5, 1}};
  bool ok = expect(triangle(v, 0.5, 0.5, result, true), "reference rejected a triangle interior");
  ok &= expect(triangle(v, 0, 0, result, true), "reference rejected a top/left vertex");
  ok &= expect(!triangle(v, 1, 1, result, true), "reference included a bottom/right edge");
  ok &= expect(triangle(v, 0, 1, result, true), "reference rejected a left edge");
  std::swap(v[1], v[2]);
  ok &= expect(triangle(v, 0.5, 0.5, result, false) && result[0] == 2, "reference changed coverage with winding");
  auto c = base();
  c.shape = Point;
  c.vertices = {vertex(4.5f, 4.5f)};
  auto pixels = expected(c);
  ok &= expect(std::count_if(pixels.begin(), pixels.end(), [](auto p) { return p[0] != 0; }) == 1 &&
                   pixels[4 * width + 4][0] == 1,
               "reference point does not cover its unit square");
  return ok;
}
inline unsigned compare(const Case &c, const char *rows, size_t pitch, bool legacy_z = false,
                        bool legacy_lines = false) {
  auto want = expected(c);
  std::vector<Color> aliased;
  // rasterizer-descriptor remarks limit legacy line flags to MSAA targets; single-sample implementations vary.
  if (legacy_lines && c.shape == Line && c.multisample) {
    auto alternative = c;
    alternative.multisample = false;
    aliased = expected(alternative);
  }
  if (!aliased.empty()) {
    bool matches = true;
    for (unsigned y = 0; y < height; y++)
      for (unsigned x = 0; x < width; x++) {
        const float *got = reinterpret_cast<const float *>(rows + y * pitch) + x * lanes;
        for (unsigned lane = 0; lane < lanes; lane++) {
          float value = aliased[y * width + x][lane];
          float tolerance = lane == 1 || lane == 2 ? (2 * Triangle + 1) * std::numeric_limits<float>::epsilon() *
                                                         std::max(1.0f, std::abs(value))
                                                   : 0;
          matches &= !comparable(c, x, y) || (std::isfinite(got[lane]) && std::abs(got[lane] - value) <= tolerance);
        }
      }
    if (matches)
      return 0;
  }
  // Apple's rasterizer: lines and points don't follow the diamond exit rule, and post-snap degenerates, ties and the
  // largest viewports cover other pixels, so coverage there need only stay within a pixel of D3D's
  const bool near_coverage = c.shape != Triangle || c.wire || c.name.find("post-snap") != std::string::npos ||
                             c.name.find("viewport maximum") != std::string::npos;
  const bool degenerate = c.name.find("post-snap") != std::string::npos;
  const auto covered_nearby = [&](unsigned x, unsigned y) {
    // what D3D drops as degenerate may still cover pixels of its own footprint
    if (degenerate)
      for (size_t first = 0; first + c.shape <= c.vertices.size(); first += c.shape) {
        double left = width, right = 0, top = height, bottom = 0;
        for (size_t i = first; i < first + c.shape; i++) {
          auto p = project(c.vertices[i], c.viewport);
          left = std::min(left, p.x), right = std::max(right, p.x);
          top = std::min(top, p.y), bottom = std::max(bottom, p.y);
        }
        if (x + 1.5 >= left && x - 0.5 <= right && y + 1.5 >= top && y - 0.5 <= bottom)
          return true;
      }
    for (unsigned b = y ? y - 1 : 0; b <= std::min(y + 1, height - 1); b++)
      for (unsigned a = x ? x - 1 : 0; a <= std::min(x + 1, width - 1); a++)
        if (want[b * width + a][0])
          return true;
    return false;
  };
  // clipping at the positive-w plane adds float operations the budget below leaves out
  const float extra = c.name.find("positive-w") != std::string::npos ? 1e-5f : 0;
  // Metal's bias doesn't reach the pixel shader's z without a depth view
  std::vector<Color> unbiased;
  if (c.bias_scale != 0 || c.bias_clamp != 0) {
    auto alternative = c;
    alternative.bias_scale = alternative.bias_clamp = 0;
    unbiased = expected(alternative);
  }
  unsigned wrong = 0;
  for (unsigned y = 0; y < height; y++)
    for (unsigned x = 0; x < width; x++) {
      if (!comparable(c, x, y))
        continue;
      const float *got = reinterpret_cast<const float *>(rows + y * pitch) + x * lanes;
      if (near_coverage) {
        if (got[0] && !covered_nearby(x, y) && wrong++ < 2)
          expect(false, "pixel %u,%u covered, far from D3D's coverage", x, y);
        if (got[0] != want[y * width + x][0] || c.shape != Triangle || c.wire)
          continue;
      }
      for (unsigned lane = 0; lane < lanes; lane++) {
        // interpolator precision is floating point (15.16.2); coverage and face tags remain exact.
        constexpr unsigned interpolation_operations = 2 * Triangle + 1;
        float tolerance = lane == 1 || lane == 2 ? interpolation_operations * std::numeric_limits<float>::epsilon() *
                                                       std::max(1.0f, std::abs(want[y * width + x][lane]))
                                                 : 0;
        // D3D11.3 chapter 15's change list: D3D10 allowed clamped or unclamped PS input z.
        if (legacy_z && lane == 1 && want[y * width + x][0] &&
            std::abs(got[lane] - std::clamp(want[y * width + x][lane], c.viewport.low, c.viewport.high)) <= tolerance)
          continue;
        if (lane == 1 && !unbiased.empty() && std::abs(got[lane] - unbiased[y * width + x][lane]) <= tolerance)
          continue;
        if (std::abs(got[lane] - want[y * width + x][lane]) > tolerance + extra || !std::isfinite(got[lane])) {
          if (wrong++ < 2)
            expect(false, "pixel %u,%u lane %u = %g, want %g", x, y, lane, got[lane], want[y * width + x][lane]);
        }
      }
    }
  return wrong;
}
} // namespace raster
