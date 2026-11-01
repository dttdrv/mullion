// contract: conservative rasterization, tier 3. a triangle drawn with D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON covers
// every pixel whose square it overlaps, and no pixel farther than tier 2's uncertainty (a 256th of a pixel) from it, a
// triangle of no area (a segment, a point) as well, which is not culled. such a pixel has all its samples covered and
// its shader runs once (at 4x the test reads the target, as Metal runs a shader that writes memory once a sample), its
// pixel shader sees the triangle's attributes extrapolated from their plane, so a pixel whose center is inside sees
// what it sees without conservative rasterization, a triangle of no area has its leading vertex's (after a tessellator
// one corner's) and faces back, a flat input is the first vertex's, and SV_InnerCoverage says whether the square is
// inside the triangle. a pipeline with a pixel shader or without writes the plane's depth, clamped to the viewport's
// range, to the same pixels. all of it holds of what is left of a triangle clipped by the near and far planes or by the
// eye, as an ordinary rasterizer clips it, and without depth clipping of all of a triangle before the eye; and for
// triangles a geometry shader emits, for those a tessellator outputs, for a geometry shader's after a tessellator, and
// for a mesh shader's (DXIL), with and without an amplification shader: its group outputs the triangle among others
// without area away from it, past the triangles one Metal mesh grows. the device reports tier 3, and with it feature
// level 12_1.
// D3D11.3 functional specification 15.17 (Conservative Rasterization): "any pixel ... touched by the primitive must be
// rasterized"; 15.17.2, the tiers' uncertainty regions; 15.17.5, "the coverage mask ... is full"; 15.17.4, attributes
// are "extrapolated" for pixels whose centers are outside; 15.17.8, inner coverage. 15.4, clipping to "0 <= z <= w",
// and without it "only w > 0"; 15.11.1, "zero area geometry is considered back facing"; 16.2, the shader "is invoked
// once per covered pixel"; 17.10, depth "always clamped" to the viewport's MinDepth and MaxDepth.
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#15.17%20Conservative%20Rasterization
// D3D12_CONSERVATIVE_RASTERIZATION_TIER: "Tier_2 reduces the maximum uncertainty region to 1/256 and requires
// post-snap degenerate triangles not be culled"; "Tier_3 maintains a maximum 1/256 uncertainty region and adds support
// for inner input coverage".
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_conservative_rasterization_tier
// each run's SV_Coverage is every sample (15.17.5), and the attributes hold in perspective however steep: a triangle
// whose plane's 1/w is below 0 within a pixel of it.
// a pixel shader that interpolates the attributes itself, of SV_Barycentrics and each vertex's (GetAttributeAtVertex),
// sees the same on every path, where the device has barycentrics: the weights are of "the original API primitive
// prior to clipping", "not constrained to be within [0...1]" outside it, and vertex 0's is the provoking vertex's.
// https://github.com/microsoft/DirectXShaderCompiler/wiki/SV_Barycentrics
#include "d3d12_test.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <string>

static const char hlsl[] = R"hlsl(
// GetAttributeAtVertex reads only inputs without interpolation, and every stage writes V as ps_barycentrics reads it:
// DXC packs a signature by its elements' interpolation, and an input must sit where the stage before put its output
// (D3D12 pipeline validation, DirectXShaderCompiler issue 75)
#ifdef BARYCENTRICS
#define PERSPECTIVE_MODE nointerpolation
#define SCREEN_MODE nointerpolation
#else
#define PERSPECTIVE_MODE
#define SCREEN_MODE noperspective
#endif
struct V {
  float4 position : SV_Position;
  PERSPECTIVE_MODE float perspective : PERSPECTIVE;
  SCREEN_MODE float screen : SCREEN;
  nointerpolation uint flat : FLAT;
};
V vs(float4 position : POSITION, float perspective : PERSPECTIVE, float screen : SCREEN, uint flat : FLAT) {
  V v = {position, perspective, screen, flat};
  return v;
}
// per pixel: the attributes, then how many times the shader ran there, the samples it covered, the runs that did not
// cover every sample, and the faces they saw (1 back, 2 front)
RWStructuredBuffer<uint4> o : register(u0);
float ps(V v, uint coverage : SV_Coverage, bool front : SV_IsFrontFace) : SV_Target {
  uint2 p = uint2(v.position.xy);
  uint i = p.y * WIDTH + p.x, was;
  o[i].xyz = uint3(asuint(v.perspective), asuint(v.screen), v.flat);
  InterlockedAdd(o[PIXELS + i].x, 1, was);
  InterlockedOr(o[PIXELS + i].y, coverage, was);
  InterlockedAdd(o[PIXELS + i].z, coverage != (1u << GetRenderTargetSampleCount()) - 1 ? 1 : 0, was);
  InterlockedOr(o[PIXELS + i].w, front ? 2 : 1, was);
  return 1;
}
// whether the square is inside, where the attributes' place is
float ps_inner(V v, uint inner : SV_InnerCoverage) : SV_Target {
  uint2 p = uint2(v.position.xy);
  uint i = p.y * WIDTH + p.x, was;
  o[i].w = inner;
  InterlockedAdd(o[PIXELS + i].x, 1, was);
  return 1;
}
// one that writes no memory, which Metal clamps the depth of otherwise than one that does
float ps_plain(V v) : SV_Target { return v.perspective; }
// the perspective input as the shader evaluates it, at the centroid: at 1x, of every sample covered, the center
float ps_evaluate(V v, bool front : SV_IsFrontFace) : SV_Target {
  uint2 p = uint2(v.position.xy);
  uint i = p.y * WIDTH + p.x, was;
  o[i].x = asuint(EvaluateAttributeCentroid(v.perspective));
  InterlockedAdd(o[PIXELS + i].x, 1, was);
  InterlockedOr(o[PIXELS + i].w, front ? 2 : 1, was);
  return 1;
}
#ifdef BARYCENTRICS
float ps_barycentrics(V v, float3 b : SV_Barycentrics, noperspective float3 n : SV_Barycentrics1) : SV_Target {
  uint2 p = uint2(v.position.xy);
  uint i = p.y * WIDTH + p.x, was;
  float3 perspective = {GetAttributeAtVertex(v.perspective, 0), GetAttributeAtVertex(v.perspective, 1),
                        GetAttributeAtVertex(v.perspective, 2)},
         screen = {GetAttributeAtVertex(v.screen, 0), GetAttributeAtVertex(v.screen, 1), GetAttributeAtVertex(v.screen, 2)};
  o[i].xyz = uint3(asuint(dot(b, perspective)), asuint(dot(n, screen)), v.flat);
  InterlockedAdd(o[PIXELS + i].x, 1, was);
  return 1;
}
#endif
float4 vs_depth(float4 position : POSITION) : SV_Position { return position; }
// the triangle, then a strip of a triangle without area past the viewport's right, which rasterizes nothing there
[maxvertexcount(6)] void gs(triangle V v[3], inout TriangleStream<V> s) {
  s.Append(v[0]);
  s.Append(v[1]);
  s.Append(v[2]);
  s.RestartStrip();
  V away = v[0];
  away.position = float4(3, 0, 0.5, 1);
  s.Append(away);
  s.Append(away);
  s.Append(away);
}
struct D {
  float4 position : SV_Position;
};
[maxvertexcount(6)] void gs_depth(triangle D v[3], inout TriangleStream<D> s) {
  s.Append(v[0]);
  s.Append(v[1]);
  s.Append(v[2]);
  s.RestartStrip();
  D away = {float4(3, 0, 0.5, 1)};
  s.Append(away);
  s.Append(away);
  s.Append(away);
}
// a patch of the triangle tessellated at 1, which makes the triangle; the flat input is one for all its corners
struct Factors {
  float edges[3] : SV_TessFactor;
  float inside : SV_InsideTessFactor;
};
Factors factors() {
  Factors f = {{1, 1, 1}, 1};
  return f;
}
[domain("tri")] [partitioning("integer")] [outputtopology("triangle_cw")] [outputcontrolpoints(3)]
[patchconstantfunc("factors")] V hs(InputPatch<V, 3> p, uint i : SV_OutputControlPointID) { return p[i]; }
[domain("tri")] V ds(Factors f, float3 b : SV_DomainLocation, const OutputPatch<V, 3> p) {
  V v;
  v.position = b.x * p[0].position + b.y * p[1].position + b.z * p[2].position;
  v.perspective = b.x * p[0].perspective + b.y * p[1].perspective + b.z * p[2].perspective;
  v.screen = b.x * p[0].screen + b.y * p[1].screen + b.z * p[2].screen;
  v.flat = p[0].flat;
  return v;
}
[domain("tri")] [partitioning("integer")] [outputtopology("triangle_cw")] [outputcontrolpoints(3)]
[patchconstantfunc("factors")] D hs_depth(InputPatch<D, 3> p, uint i : SV_OutputControlPointID) { return p[i]; }
[domain("tri")] D ds_depth(Factors f, float3 b : SV_DomainLocation, const OutputPatch<D, 3> p) {
  D v;
  v.position = b.x * p[0].position + b.y * p[1].position + b.z * p[2].position;
  return v;
}
#ifdef MESH
ByteAddressBuffer corners : register(t0);
struct Payload {
  uint unused;
};
// the triangle as triangle REAL of TRIANGLES, the others without area
#define MESH_BODY                                                                                                     \
  SetMeshOutputCounts(4, TRIANGLES);                                                                                  \
  if (t == 3) {                                                                                                       \
    v[t].position = float4(3, 0, 0.5, 1);                                                                             \
    v[t].perspective = v[t].screen = 0;                                                                               \
    v[t].flat = 0;                                                                                                    \
  }                                                                                                                   \
  if (t < 3) {                                                                                                        \
    uint o = t * 28;                                                                                                  \
    v[t].position = asfloat(corners.Load4(o));                                                                        \
    v[t].perspective = asfloat(corners.Load(o + 16));                                                                 \
    v[t].screen = asfloat(corners.Load(o + 20));                                                                      \
    v[t].flat = corners.Load(24);                                                                                     \
  }                                                                                                                   \
  for (uint i = t; i < TRIANGLES; i += 32)                                                                            \
    tris[i] = i == REAL ? uint3(0, 1, 2) : uint3(3, 3, 3);
[outputtopology("triangle")] [numthreads(32, 1, 1)]
void ms(uint t : SV_GroupThreadID, out vertices V v[4], out indices uint3 tris[TRIANGLES]) { MESH_BODY }
[outputtopology("triangle")] [numthreads(32, 1, 1)]
void ms_amplified(uint t : SV_GroupThreadID, in payload Payload p, out vertices V v[4], out indices uint3 tris[TRIANGLES]) {
  MESH_BODY
}
// a group small enough for one mesh, which counts its runs in the last pixel's, which nothing reaches
[outputtopology("triangle")] [numthreads(32, 1, 1)]
void ms_writes(uint t : SV_GroupThreadID, out vertices V v[4], out indices uint3 tris[TRIANGLES]) {
  MESH_BODY
  uint was;
  if (t == 0)
    InterlockedAdd(o[2 * PIXELS - 1].w, 1, was);
}
[numthreads(1, 1, 1)] void as() {
  Payload p;
  p.unused = 0;
  DispatchMesh(1, 1, 1, p);
}
#endif
)hlsl";

static const UINT kWidth = 24, kHeight = 24;

template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename T> struct alignas(void *) Subobject {
  D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
  T data{};
};
// the viewport is offset in the target, so window coordinates are not the viewport's, and its depth range is not 0
// to 1, which a clamp shows
static const D3D12_VIEWPORT kViewport{2, 3, 20, 20, 0, 0.75f};

struct Vertex {
  float position[4];
  float perspective, linear;
  UINT flat;
};

struct Triangle {
  const char *what;
  float window[3][2];              // the corners in the target's pixels; behind the eye, mirrored through it
  float w[3];                      // the corners' clip w: perspective
  float z[3] = {0.5f, 0.5f, 0.5f}; // and their z over w
};

using Point = std::array<double, 2>;
using Shape = std::vector<Point>;

static std::array<double, 4>
clip(const Triangle &t, int k) {
  double x = (t.window[k][0] - kViewport.TopLeftX) / kViewport.Width * 2 - 1;
  double y = 1 - (t.window[k][1] - kViewport.TopLeftY) / kViewport.Height * 2;
  return {x * t.w[k], y * t.w[k], t.z[k] * t.w[k], t.w[k]};
}

// what of the triangle reaches the window: what is in 0 <= z <= w where depth clips, all of it where it does not
// (asked only of triangles before the eye) (15.4)
static Shape
drawn(const Triangle &t, bool depth_clips) {
  std::vector<std::array<double, 4>> p = {clip(t, 0), clip(t, 1), clip(t, 2)};
  for (int plane = 0; plane < 2 * depth_clips; plane++) {
    auto inside = [&](auto &v) { return plane ? v[3] - v[2] : v[2]; };
    std::vector<std::array<double, 4>> kept;
    for (size_t k = 0; k < p.size(); k++) {
      auto &a = p[k], &b = p[(k + 1) % p.size()];
      double da = inside(a), db = inside(b);
      if (da >= 0)
        kept.push_back(a);
      if ((da >= 0) != (db >= 0)) {
        std::array<double, 4> m;
        for (int c = 0; c < 4; c++)
          m[c] = a[c] + da / (da - db) * (b[c] - a[c]);
        kept.push_back(m);
      }
    }
    p = kept;
  }
  Shape out;
  for (auto &v : p)
    out.push_back(
        {kViewport.TopLeftX + (v[0] / v[3] + 1) * kViewport.Width / 2,
         kViewport.TopLeftY + (1 - v[1] / v[3]) * kViewport.Height / 2}
    );
  return out;
}

// twice the signed area, positive turning clockwise in the window
static double
area(const Shape &p) {
  double sum = 0;
  for (size_t k = 0; k < p.size(); k++)
    sum += p[k][0] * p[(k + 1) % p.size()][1] - p[(k + 1) % p.size()][0] * p[k][1];
  return sum;
}

// the polygon snapped as a rasterizer snaps it, to a 256th of a pixel (15.16), and whether it has no area then
static Shape
snap(Shape p) {
  for (auto &corner : p)
    for (auto &c : corner)
      c = std::nearbyint(c * 256) / 256;
  return p;
}

static bool
flat(const Shape &p) {
  return area(snap(p)) == 0;
}

// the edges' normals into the polygon; around one of no area, both of each side's
static std::vector<std::pair<Point, Point>>
edges(const Shape &p) {
  std::vector<std::pair<Point, Point>> out;
  double inward = area(p) > 0 ? -1 : 1;
  for (size_t k = 0; k < p.size(); k++) {
    auto &a = p[k], &b = p[(k + 1) % p.size()];
    out.push_back({a, {(b[1] - a[1]) * inward, (a[0] - b[0]) * inward}});
  }
  return out;
}

// whether the square of pixel (px, py), grown by `grow` on each side, has a point in the polygon: the separating
// axis test, the square's axes, then the edges' normals
static bool
overlaps(const Shape &p, int px, int py, double grow) {
  if (p.empty())
    return false;
  double lo[2] = {px - grow, py - grow}, hi[2] = {px + 1 + grow, py + 1 + grow};
  for (int axis = 0; axis < 2; axis++) {
    double tmin = 1e30, tmax = -1e30;
    for (auto &v : p)
      tmin = std::min(tmin, v[axis]), tmax = std::max(tmax, v[axis]);
    if (tmax < lo[axis] || tmin > hi[axis])
      return false;
  }
  for (auto &[a, n] : edges(p)) {
    // the square's corner farthest inside
    double best = -1e30;
    for (double x : {lo[0], hi[0]})
      for (double y : {lo[1], hi[1]})
        best = std::max(best, n[0] * (x - a[0]) + n[1] * (y - a[1]));
    if (best < 0)
      return false;
  }
  return true;
}

// whether the box's corners are inside the polygon by `margin`, times the edge's length
static bool
inside(const Shape &p, double x0, double y0, double x1, double y1, double margin) {
  if (area(p) == 0)
    return false;
  for (auto &[a, n] : edges(p))
    for (double x : {x0, x1})
      for (double y : {y0, y1})
        if (n[0] * (x - a[0]) + n[1] * (y - a[1]) < margin * std::hypot(n[0], n[1]))
          return false;
  return true;
}

static bool
contained(const Shape &p, int px, int py, double grow) {
  return inside(p, px - grow, py - grow, px + 1 + grow, py + 1 + grow, 0);
}

// whether the pixel's center is inside, as an ordinary rasterizer would take it (away from edges)
static bool
center_inside(const Shape &p, int px, int py, double margin) {
  return inside(p, px + 0.5, py + 0.5, px + 0.5, py + 0.5, margin);
}

// the triangle's plane at a point of the window, from the corners' homogeneous barycentrics (Olano and Greer, by
// Cramer's rule): the perspective input (1, 3, 5 at the corners, NaN where the plane is behind the eye), the
// noperspective one (0, 10, 20) and z over w
static std::array<double, 3>
plane(const Triangle &t, double x, double y) {
  double m[3][3], p[3] = {(x - kViewport.TopLeftX) / kViewport.Width * 2 - 1,
                          1 - (y - kViewport.TopLeftY) / kViewport.Height * 2, 1};
  for (int k = 0; k < 3; k++) {
    auto c = clip(t, k);
    m[0][k] = c[0], m[1][k] = c[1], m[2][k] = c[3];
  }
  auto det = [](const double (&a)[3][3]) {
    return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
           a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
  };
  double u[3], rcp_w = 0, over_w = 0, screen = 0, z = 0;
  for (int j = 0; j < 3; j++) {
    double mj[3][3];
    for (int r = 0; r < 3; r++)
      for (int c = 0; c < 3; c++)
        mj[r][c] = c == j ? p[r] : m[r][c];
    u[j] = det(mj) / det(m);
    rcp_w += u[j], over_w += u[j] * (1 + 2 * j), screen += u[j] * t.w[j] * 10 * j, z += u[j] * t.w[j] * t.z[j];
  }
  return {rcp_w > 0 ? over_w / rcp_w : NAN, screen, z};
}

// how far the plane's `a` moves within a 64th of a pixel of a point: snapping the corners to a 256th of a pixel, the
// grown triangle's as well as the triangle's, moves the plane that much, which a needle makes steep
static double
moves(const Triangle &t, int a, double x, double y) {
  double here = plane(t, x, y)[a], most = 0;
  for (double dx : {-1.0 / 64, 1.0 / 64})
    for (double dy : {-1.0 / 64, 1.0 / 64})
      most = std::max(most, std::fabs(plane(t, x + dx, y + dy)[a] - here));
  return most;
}


int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  // each step's first few
  unsigned failures = 0, failures_before = 0;
  auto fail = [&](const char *format, auto... args) {
    if (failures++ - failures_before < 12) {
      printf(format, args...);
      printf("\n");
    }
  };

  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
  if (options.ConservativeRasterizationTier < D3D12_CONSERVATIVE_RASTERIZATION_TIER_3)
    fail("conservative rasterization tier %u", options.ConservativeRasterizationTier);
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_12_1};
  D3D12_FEATURE_DATA_FEATURE_LEVELS level{2, levels};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &level, sizeof(level)));
  if (level.MaxSupportedFeatureLevel != D3D_FEATURE_LEVEL_12_1)
    fail("feature level %#x of 12_0 and 12_1", level.MaxSupportedFeatureLevel);
  if (D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_1, __uuidof(ID3D12Device), nullptr) != S_FALSE)
    fail("no device of feature level 12_1");

  std::vector<std::string> defines = {"WIDTH=" + std::to_string(kWidth), "PIXELS=" + std::to_string(kWidth * kHeight)};
  auto ps_evaluate = compiler.compile(hlsl, "ps_evaluate", "ps", defines);
  if (ps_evaluate.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  auto ps_inner = compiler.compile(hlsl, "ps_inner", "ps", defines),
       ps_plain = compiler.compile(hlsl, "ps_plain", "ps", defines);
  if (ps_inner.empty() || ps_plain.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  auto vs = compiler.compile(hlsl, "vs", "vs", defines), ps = compiler.compile(hlsl, "ps", "ps", defines),
       vs_depth = compiler.compile(hlsl, "vs_depth", "vs", defines), gs = compiler.compile(hlsl, "gs", "gs", defines),
       gs_depth = compiler.compile(hlsl, "gs_depth", "gs", defines), hs = compiler.compile(hlsl, "hs", "hs", defines),
       ds = compiler.compile(hlsl, "ds", "ds", defines), hs_depth = compiler.compile(hlsl, "hs_depth", "hs", defines),
       ds_depth = compiler.compile(hlsl, "ds_depth", "ds", defines);
  if (vs.empty() || ps.empty() || vs_depth.empty() || gs.empty() || gs_depth.empty() || hs.empty() || ds.empty() ||
      hs_depth.empty() || ds_depth.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  // mesh shaders are DXIL's, on a device that has them
  ComPtr<ID3D12Device2> device2;
  CHECK(device.As(&device2));
  D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
  device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7));
  bool meshes = compiler.dxc && options7.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;
  // a group of the most primitives a mesh shader outputs, 256 (D3D12 mesh shader specification), more than one Metal
  // mesh of 256 vertices holds at three for each, the triangle in a later one
  auto mesh_defines = defines;
  mesh_defines.insert(mesh_defines.end(), {"MESH", "TRIANGLES=256", "REAL=200"});
  auto small_defines = defines;
  small_defines.insert(small_defines.end(), {"MESH", "TRIANGLES=4", "REAL=1"});
  // barycentrics are DXIL's (shader model 6.1), on a device that has them
  D3D12_FEATURE_DATA_D3D12_OPTIONS3 options3{};
  device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &options3, sizeof(options3));
  bool supported = compiler.dxc && options3.BarycentricsSupported;
  // the mesh shaders, then those of V as ps_barycentrics reads it
  std::string ms[2], ms_amplified[2], ms_writes[2], as;
  for (int b = 0; meshes && b <= supported; b++) {
    auto with = [&](std::vector<std::string> d) {
      if (b)
        d.push_back("BARYCENTRICS");
      return d;
    };
    ms[b] = compiler.compile(hlsl, "ms", "ms_6_5", with(mesh_defines));
    ms_amplified[b] = compiler.compile(hlsl, "ms_amplified", "ms_6_5", with(mesh_defines));
    ms_writes[b] = compiler.compile(hlsl, "ms_writes", "ms_6_5", with(small_defines));
    if (ms[b].empty() || ms_amplified[b].empty() || ms_writes[b].empty()) {
      printf("failed: the mesh shaders did not compile\n");
      return 1;
    }
  }
  if (meshes && (as = compiler.compile(hlsl, "as", "as_6_5", mesh_defines)).empty()) {
    printf("failed: the mesh shaders did not compile\n");
    return 1;
  }
  std::string ps_barycentrics, barycentric_vs, barycentric_gs, barycentric_hs, barycentric_ds;
  if (supported) {
    auto barycentric_defines = defines;
    barycentric_defines.push_back("BARYCENTRICS");
    ps_barycentrics = compiler.compile(hlsl, "ps_barycentrics", "ps_6_1", barycentric_defines);
    barycentric_vs = compiler.compile(hlsl, "vs", "vs", barycentric_defines);
    barycentric_gs = compiler.compile(hlsl, "gs", "gs", barycentric_defines);
    barycentric_hs = compiler.compile(hlsl, "hs", "hs", barycentric_defines);
    barycentric_ds = compiler.compile(hlsl, "ds", "ds", barycentric_defines);
    if (ps_barycentrics.empty() || barycentric_vs.empty() || barycentric_gs.empty() || barycentric_hs.empty() ||
        barycentric_ds.empty()) {
      printf("failed: the barycentrics shaders did not compile\n");
      return 1;
    }
  }

  // the UAV the pixel shader writes, and the corners a mesh shader reads
  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_UAV}, {D3D12_ROOT_PARAMETER_TYPE_SRV}};
  auto rs = root_signature(
      device.Get(), {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT}
  );
  const D3D12_INPUT_ELEMENT_DESC elements[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"PERSPECTIVE", 0, DXGI_FORMAT_R32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"SCREEN", 0, DXGI_FORMAT_R32_FLOAT, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"FLAT", 0, DXGI_FORMAT_R32_UINT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  };
  // the stages before the rasterizer: the vertex shader, then a geometry shader (1), a tessellator (2), or both; or a
  // mesh shader (4), after an amplification shader (5), or one that writes resources (6)
  const int kPaths = 7;
  auto tessellated = [](int path) { return path < 4 && (path & 2); };
  auto meshed = [](int path) { return path >= 4; };
  struct {
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> rs;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, D3D12_SHADER_BYTECODE> as;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> ms;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> raster;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC> depth;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> targets;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT> depth_format;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> samples;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> sample_mask;
  } stream;
  // a mesh pipeline of the graphics pipeline's state, of the mesh shaders for ps_barycentrics with `b`
  auto mesh_pipeline = [&](int path, const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc, ComPtr<ID3D12PipelineState> &out,
                           int b = 0) {
    stream = {};
    stream.rs.data = desc.pRootSignature;
    if (path == 5)
      stream.as.data = bytecode(as);
    stream.ms.data = bytecode(path == 5 ? ms_amplified[b] : path == 6 ? ms_writes[b] : ms[b]);
    stream.ps.data = desc.PS;
    stream.raster.data = desc.RasterizerState;
    stream.depth.data = desc.DepthStencilState;
    stream.targets.data.NumRenderTargets = desc.NumRenderTargets;
    std::copy_n(desc.RTVFormats, 8, stream.targets.data.RTFormats);
    stream.depth_format.data = desc.DSVFormat;
    stream.samples.data = desc.SampleDesc;
    stream.sample_mask.data = desc.SampleMask;
    D3D12_PIPELINE_STATE_STREAM_DESC stream_desc{sizeof(stream), &stream};
    return device2->CreatePipelineState(&stream_desc, IID_PPV_ARGS(&out));
  };
  // by the stages, conservative rasterization and sample count
  ComPtr<ID3D12PipelineState> pso[kPaths][2][2], depth_pso[kPaths], inner_pso[kPaths], unclipped_pso, shaded_pso[kPaths],
      shaded_unclipped_pso, barycentric_pso[kPaths];
  for (int geometry = 0; geometry < kPaths; geometry++)
    for (int conservative = 0; conservative < 2; conservative++)
      for (int msaa = 0; msaa < 2; msaa++) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs), bytecode(ps)};
        if (geometry & 1)
          desc.GS = bytecode(gs);
        if (tessellated(geometry))
          desc.HS = bytecode(hs), desc.DS = bytecode(ds);
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.SampleMask = ~0u;
        desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.RasterizerState.ConservativeRaster =
            conservative ? D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON : D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
        desc.InputLayout = {elements, (UINT)std::size(elements)};
        desc.PrimitiveTopologyType =
            tessellated(geometry) ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
        desc.SampleDesc = {msaa ? 4u : 1u, 0};
        if (meshed(geometry)) {
          if (meshes)
            CHECK(mesh_pipeline(geometry, desc, pso[geometry][conservative][msaa]));
        } else {
          CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso[geometry][conservative][msaa])));
        }
        // one that writes depth too, with depth clipped and (of the vertex shader's) without, and one whose pixel
        // shader reads SV_InnerCoverage
        if (conservative && !msaa && (!meshed(geometry) || meshes)) {
          auto shaded = desc;
          shaded.PS = bytecode(ps_plain);
          shaded.NumRenderTargets = 0;
          shaded.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
          shaded.DSVFormat = DXGI_FORMAT_D32_FLOAT;
          shaded.DepthStencilState.DepthEnable = TRUE;
          shaded.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
          shaded.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
          if (meshed(geometry)) {
            CHECK(mesh_pipeline(geometry, shaded, shaded_pso[geometry]));
          } else {
            CHECK(device->CreateGraphicsPipelineState(&shaded, IID_PPV_ARGS(&shaded_pso[geometry])));
          }
          if (!geometry) {
            shaded.RasterizerState.DepthClipEnable = FALSE;
            CHECK(device->CreateGraphicsPipelineState(&shaded, IID_PPV_ARGS(&shaded_unclipped_pso)));
          }
          desc.PS = bytecode(ps_inner);
          if (meshed(geometry)) {
            CHECK(mesh_pipeline(geometry, desc, inner_pso[geometry]));
          } else {
            CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&inner_pso[geometry])));
          }
          if (supported) {
            desc.PS = bytecode(ps_barycentrics);
            desc.VS = bytecode(barycentric_vs);
            if (desc.GS.pShaderBytecode)
              desc.GS = bytecode(barycentric_gs);
            if (desc.HS.pShaderBytecode)
              desc.HS = bytecode(barycentric_hs), desc.DS = bytecode(barycentric_ds);
            if (meshed(geometry)) {
              CHECK(mesh_pipeline(geometry, desc, barycentric_pso[geometry], 1));
            } else {
              CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&barycentric_pso[geometry])));
            }
          }
        }
      }
  for (int geometry = 0; geometry < kPaths; geometry++) {
    // depth only, without a pixel shader
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs_depth)};
    if (geometry & 1)
      desc.GS = bytecode(gs_depth);
    if (tessellated(geometry))
      desc.HS = bytecode(hs_depth), desc.DS = bytecode(ds_depth);
    desc.SampleMask = ~0u;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON;
    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.InputLayout = {elements, 1};
    desc.PrimitiveTopologyType =
        tessellated(geometry) ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    desc.SampleDesc = {1, 0};
    if (meshed(geometry)) {
      if (meshes)
        CHECK(mesh_pipeline(geometry, desc, depth_pso[geometry]));
      continue;
    }
    CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&depth_pso[geometry])));
    // and the vertex shader's without depth clipping
    if (!geometry) {
      desc.RasterizerState.DepthClipEnable = FALSE;
      CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&unclipped_pso)));
    }
  }

  // and one of the vertex shader's triangles that evaluates the perspective input, its front turning counterclockwise
  ComPtr<ID3D12PipelineState> evaluate_pso;
  {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs), bytecode(ps_evaluate)};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = ~0u;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON;
    desc.RasterizerState.FrontCounterClockwise = TRUE;
    desc.InputLayout = {elements, (UINT)std::size(elements)};
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
    desc.SampleDesc = {1, 0};
    CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&evaluate_pso)));
  }

  const Triangle triangles[] = {
      {"a large triangle", {{4.3f, 5.1f}, {19.6f, 7.4f}, {9.2f, 20.7f}}, {1, 1, 1}},
      {"a triangle inside one pixel, around no center", {{10.1f, 10.1f}, {10.4f, 10.2f}, {10.2f, 10.35f}}, {1, 1, 1}},
      // whose plane's 1/w is below 0 a pixel away: at (9.1, 9.1), and across the pixel's square, (11, 10.1)
      {"a triangle inside one pixel in steep perspective", {{10.1f, 10.1f}, {10.4f, 10.2f}, {10.2f, 10.35f}}, {1, 4, 0.5f}},
      {"a sliver across pixels", {{3.2f, 12.05f}, {20.8f, 12.2f}, {3.3f, 12.12f}}, {1, 1, 1}},
      {"a needle", {{4.5f, 4.5f}, {20.5f, 19.0f}, {20.2f, 19.3f}}, {1, 1, 1}},
      {"a triangle in perspective", {{5.5f, 6.2f}, {18.7f, 9.9f}, {7.1f, 19.4f}}, {1, 4, 0.5f}},
      {"a triangle past the viewport", {{-6.0f, 8.0f}, {12.0f, 1.0f}, {8.0f, 30.0f}}, {1, 2, 1}},
      {"a segment, a triangle of no area", {{4.25f, 6.5f}, {10.25f, 9.5f}, {16.25f, 12.5f}}, {1, 1, 1}},
      {"a point, a triangle of no area", {{9.6f, 14.4f}, {9.6f, 14.4f}, {9.6f, 14.4f}}, {1, 1, 1}},
      // a 1024th of a pixel off the segment's line, which snapping to a 256th (15.16) puts on it
      {"a sliver of no area once snapped", {{4.25f, 6.5f}, {10.25f, 9.5f + 1.0f / 1024}, {16.25f, 12.5f}}, {1, 1, 1}},
      // clipped to a quadrilateral, to a pentagon, past both its edges into the window, and to nothing
      {"a triangle across the near plane", {{4.0f, 6.0f}, {19.0f, 9.0f}, {8.0f, 20.0f}}, {1, 1, 1},
       {0.5f, -0.6f, 0.7f}},
      {"a triangle across the near and far planes in perspective", {{3.5f, 5.0f}, {20.0f, 12.0f}, {6.0f, 21.0f}},
       {1, 2, 1.5f}, {-0.4f, 0.5f, 1.6f}},
      // whose edges leave the near plane at w 0.5, (8, 2), and 0.8, (23, 20)
      {"a triangle with a corner behind the eye", {{6.0f, 6.0f}, {2.0f, 14.0f}, {16.0f, 18.0f}}, {1, -1, 2},
       {0.5f, 1.5f, 0.5f}},
      {"a triangle before the near plane", {{5.0f, 5.0f}, {18.0f, 8.0f}, {9.0f, 19.0f}}, {1, 1, 1},
       {-0.5f, -0.2f, -0.9f}},
  };

  const UINT64 row = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, pixels = kWidth * kHeight;
  const UINT64 uav_bytes = pixels * 32, target_at = uav_bytes, depth_at = target_at + row * kHeight;
  auto uav = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, uav_bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto clear_uav = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, uav_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto readback =
      buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, depth_at + row * kHeight, D3D12_RESOURCE_STATE_COPY_DEST);
  auto vertices = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(Vertex) * 3, D3D12_RESOURCE_STATE_GENERIC_READ);
  {
    void *bytes;
    CHECK(clear_uav->Map(0, nullptr, &bytes));
    memset(bytes, 0xff, uav_bytes / 2);
    memset((char *)bytes + uav_bytes / 2, 0, uav_bytes / 2);
    clear_uav->Unmap(0, nullptr);
  }
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  // the 4x target resolves to the fraction of each pixel's samples covered
  ComPtr<ID3D12Resource> targets[2], resolved, depth;
  for (int msaa = 0; msaa < 2; msaa++) {
    D3D12_RESOURCE_DESC desc{
        D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        0,
        kWidth,
        kHeight,
        1,
        1,
        DXGI_FORMAT_R32_FLOAT,
        {msaa ? 4u : 1u, 0},
        D3D12_TEXTURE_LAYOUT_UNKNOWN,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
    };
    CHECK(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&targets[msaa])
    ));
  }
  {
    D3D12_RESOURCE_DESC desc{
        D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        0,
        kWidth,
        kHeight,
        1,
        1,
        DXGI_FORMAT_R32_FLOAT,
        {1, 0},
        D3D12_TEXTURE_LAYOUT_UNKNOWN,
        D3D12_RESOURCE_FLAG_NONE
    };
    CHECK(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr, IID_PPV_ARGS(&resolved)
    ));
  }
  {
    D3D12_RESOURCE_DESC desc{
        D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        0,
        kWidth,
        kHeight,
        1,
        1,
        DXGI_FORMAT_D32_FLOAT,
        {1, 0},
        D3D12_TEXTURE_LAYOUT_UNKNOWN,
        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
    };
    D3D12_CLEAR_VALUE clear{DXGI_FORMAT_D32_FLOAT};
    clear.DepthStencil.Depth = 1;
    CHECK(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(&depth)
    ));
  }
  ComPtr<ID3D12DescriptorHeap> rtvs, dsvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2}, dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  CHECK(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsvs)));
  D3D12_CPU_DESCRIPTOR_HANDLE rtv[2];
  for (int msaa = 0; msaa < 2; msaa++) {
    rtv[msaa] = rtvs->GetCPUDescriptorHandleForHeapStart();
    rtv[msaa].ptr += msaa * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    device->CreateRenderTargetView(targets[msaa].Get(), nullptr, rtv[msaa]);
  }
  auto dsv = dsvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateDepthStencilView(depth.Get(), nullptr, dsv);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  // the attributes and samples each pixel's shader saw, whether the target was written, and its depth
  struct Pixels {
    UINT attributes[kWidth * kHeight][4];
    UINT runs[kWidth * kHeight][4];
    float target[kWidth * kHeight]; // 1x: the shader's 1, 4x: the samples covered, of 1
    float depth[kWidth * kHeight];
  };
  auto draw = [&](const Triangle &t, ID3D12PipelineState *pipeline, int msaa, bool depth_only, Pixels &out, bool patch,
                  bool mesh = false) -> HRESULT {
    Vertex *v;
    HRESULT hr;
    if (FAILED(hr = vertices->Map(0, nullptr, (void **)&v)))
      return hr;
    for (int k = 0; k < 3; k++) {
      auto c = clip(t, k);
      v[k] = {{(float)c[0], (float)c[1], (float)c[2], (float)c[3]}, 1.0f + 2 * k, 10.0f * k, 7u + k};
    }
    vertices->Unmap(0, nullptr);
    allocator->Reset();
    list->Reset(allocator.Get(), pipeline);
    transition(list.Get(), uav.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyBufferRegion(uav.Get(), 0, clear_uav.Get(), 0, uav_bytes);
    transition(list.Get(), uav.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const float zero[4] = {};
    list->ClearRenderTargetView(rtv[msaa], zero, 0, nullptr);
    list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
    list->SetGraphicsRootSignature(rs.Get());
    list->SetGraphicsRootUnorderedAccessView(0, uav->GetGPUVirtualAddress());
    D3D12_RECT scissor{0, 0, (LONG)kWidth, (LONG)kHeight};
    list->RSSetViewports(1, &kViewport);
    list->RSSetScissorRects(1, &scissor);
    if (depth_only)
      list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
    else
      list->OMSetRenderTargets(1, &rtv[msaa], FALSE, nullptr);
    list->IASetPrimitiveTopology(
        patch ? D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST
    );
    list->SetGraphicsRootShaderResourceView(1, vertices->GetGPUVirtualAddress());
    D3D12_VERTEX_BUFFER_VIEW view{vertices->GetGPUVirtualAddress(), sizeof(Vertex) * 3, sizeof(Vertex)};
    list->IASetVertexBuffers(0, 1, &view);
    ComPtr<ID3D12GraphicsCommandList6> list6;
    if (mesh && SUCCEEDED(list.As(&list6)))
      list6->DispatchMesh(1, 1, 1);
    else
      list->DrawInstanced(3, 1, 0, 0);
    transition(list.Get(), uav.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), 0, uav.Get(), 0, uav_bytes);
    transition(list.Get(), uav.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    {
      ID3D12Resource *read = targets[0].Get();
      if (msaa) {
        transition(
            list.Get(), targets[1].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE
        );
        list->ResolveSubresource(resolved.Get(), 0, targets[1].Get(), 0, DXGI_FORMAT_R32_FLOAT);
        transition(
            list.Get(), targets[1].Get(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET
        );
        transition(list.Get(), resolved.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        read = resolved.Get();
      } else {
        transition(list.Get(), read, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      }
      D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
      dst.PlacedFootprint = {target_at, {DXGI_FORMAT_R32_FLOAT, kWidth, kHeight, 1, (UINT)row}};
      D3D12_TEXTURE_COPY_LOCATION src{read, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      transition(
          list.Get(), read, D3D12_RESOURCE_STATE_COPY_SOURCE,
          msaa ? D3D12_RESOURCE_STATE_RESOLVE_DEST : D3D12_RESOURCE_STATE_RENDER_TARGET
      );
    }
    transition(list.Get(), depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION ddst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    ddst.PlacedFootprint = {depth_at, {DXGI_FORMAT_R32_TYPELESS, kWidth, kHeight, 1, (UINT)row}};
    D3D12_TEXTURE_COPY_LOCATION dsrc{depth.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    list->CopyTextureRegion(&ddst, 0, 0, 0, &dsrc, nullptr);
    transition(list.Get(), depth.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    if (FAILED(hr = submit(device.Get(), queue.Get(), list.Get())))
      return hr;
    char *bytes;
    if (FAILED(hr = readback->Map(0, nullptr, (void **)&bytes)))
      return hr;
    memcpy(out.attributes, bytes, uav_bytes / 2);
    memcpy(out.runs, bytes + uav_bytes / 2, uav_bytes / 2);
    for (UINT y = 0; y < kHeight; y++)
      for (UINT x = 0; x < kWidth; x++) {
        out.target[y * kWidth + x] = reinterpret_cast<float *>(bytes + target_at + y * row)[x];
        out.depth[y * kWidth + x] = reinterpret_cast<float *>(bytes + depth_at + y * row)[x];
      }
    readback->Unmap(0, nullptr);
    return S_OK;
  };

  static Pixels normal, conservative, multisampled, depth_only, unclipped, evaluated, inner, shaded, shaded_unclipped,
      barycentric;
  // a pixel inside the viewport and target: what a draw can reach
  auto reachable = [&](int x, int y) {
    return x >= kViewport.TopLeftX && y >= kViewport.TopLeftY && x < kViewport.TopLeftX + kViewport.Width &&
           y < kViewport.TopLeftY + kViewport.Height;
  };
  const char *paths[kPaths] = {
      "",
      ", from a geometry shader",
      ", from a tessellator",
      ", from a geometry shader after a tessellator",
      ", from a mesh shader",
      ", from a mesh shader after an amplification shader",
      ", from a mesh shader that writes resources"
  };
  for (int geometry = 0; geometry < kPaths; geometry++)
    for (auto &shape : triangles) {
      if (meshed(geometry) && !meshes)
        continue;
      auto t = shape;
      std::string what = std::string(shape.what) + paths[geometry];
      t.what = what.c_str();
      failures_before = failures;
      step("%s", t.what);
      bool patch = tessellated(geometry);
      bool mesh = meshed(geometry);
      // without depth clipping, only of triangles before the eye: the rest is still clipped to w above 0 (15.4), which
      // reaches past any window
      bool unclippable = std::all_of(std::begin(t.w), std::end(t.w), [](float w) { return w > 0; });
      CHECK(draw(t, pso[geometry][0][0].Get(), 0, false, normal, patch, mesh));
      CHECK(draw(t, pso[geometry][1][0].Get(), 0, false, conservative, patch, mesh));
      CHECK(draw(t, pso[geometry][1][1].Get(), 1, false, multisampled, patch, mesh));
      CHECK(draw(t, depth_pso[geometry].Get(), 0, true, depth_only, patch, mesh));
      if (!geometry)
        CHECK(draw(t, evaluate_pso.Get(), 0, false, evaluated, false));
      CHECK(draw(t, inner_pso[geometry].Get(), 0, false, inner, patch, mesh));
      CHECK(draw(t, shaded_pso[geometry].Get(), 0, true, shaded, patch, mesh));
      bool barycentrics = barycentric_pso[geometry];
      if (barycentrics)
        CHECK(draw(t, barycentric_pso[geometry].Get(), 0, false, barycentric, patch, mesh));
      if (!geometry && unclippable) {
        CHECK(draw(t, unclipped_pso.Get(), 0, true, unclipped, false));
        CHECK(draw(t, shaded_unclipped_pso.Get(), 0, true, shaded_unclipped, false));
      }
      // what reaches the window, and whether it has no area there, which turns it back (15.11.1, 15.12) and gives it
      // its leading vertex's attributes: after a tessellator any one corner, the same for the whole triangle (11.7.9),
      // so it is the one the first pixel shows
      auto polygon = drawn(t, true), whole = drawn(t, false);
      bool degenerate = flat(polygon), front = area(snap(polygon)) > 0;
      int leading = 0, shown = 0;
      while (patch && degenerate && shown < (int)(kWidth * kHeight) && !conservative.runs[shown][0])
        shown++;
      for (int k = 0; k < 3 && shown < (int)(kWidth * kHeight) && patch && degenerate; k++) {
        float got;
        memcpy(&got, &conservative.attributes[shown][0], 4);
        if (std::fabs(got - (1.0 + 2 * k)) <= 1e-3 * (1.0 + 2 * k))
          leading = k;
      }
      for (int y = 0; y < (int)kHeight; y++)
        for (int x = 0; x < (int)kWidth; x++) {
          int i = y * kWidth + x;
          // certainly overlapped: the square less a sliver; certainly not: farther than the uncertainty region
          bool must = reachable(x, y) && overlaps(polygon, x, y, -1.0 / 64),
               may = reachable(x, y) && overlaps(polygon, x, y, 1.0 / 128);
          bool ran = conservative.runs[i][0];
          // one primitive runs a pixel's shader once (D3D11.3 16.2). at 4x Metal runs a shader that writes memory
          // once a covered sample, and a run that discards still writes it, with or without conservative
          // rasterization (probe-4x-runs.m): there the target tells
          if (conservative.runs[i][0] > 1)
            fail("%s: pixel (%d, %d) ran its shader %u times", t.what, x, y, conservative.runs[i][0]);
          if ((conservative.target[i] == 1) != ran)
            fail(
                "%s: pixel (%d, %d) ran its shader %d, wrote the target %g", t.what, x, y, ran, conservative.target[i]
            );
          if (must && !ran)
            fail("%s: pixel (%d, %d), which the triangle overlaps, was not rasterized", t.what, x, y);
          if (!may && ran)
            fail("%s: pixel (%d, %d), more than half a pixel away, was rasterized", t.what, x, y);
          float samples = multisampled.target[i];
          if (must && samples != 1)
            fail("%s: pixel (%d, %d) at 4x has %g of its samples, want all", t.what, x, y, samples);
          if (!may && samples != 0)
            fail("%s: pixel (%d, %d) at 4x, more than half a pixel away, has %g of its samples", t.what, x, y, samples);
          if (may && samples != 0 && samples != 1)
            fail("%s: pixel (%d, %d) at 4x has %g of its samples, want all or none", t.what, x, y, samples);
          // every run's SV_Coverage is all of the pixel's samples (15.17.5), the grown triangles' shared edges too
          if (conservative.runs[i][2] || multisampled.runs[i][2])
            fail("%s: pixel (%d, %d) ran its shader with partial coverage %u times at 1x, %u at 4x (covering %#x)", t.what,
                 x, y, conservative.runs[i][2], multisampled.runs[i][2], multisampled.runs[i][1]);
          if (must && multisampled.runs[i][1] != 0xf)
            fail("%s: pixel (%d, %d) at 4x saw coverage %#x, want 0xf", t.what, x, y, multisampled.runs[i][1]);
          // the face, as the ordinary rasterizer sees it, and turned by the state
          UINT face = front && !degenerate ? 2 : 1, turned = !front && !degenerate ? 2 : 1;
          if ((ran && conservative.runs[i][3] != face) || (normal.runs[i][0] && normal.runs[i][3] != face))
            fail("%s: pixel (%d, %d) saw faces %u, without conservative rasterization %u, want %u", t.what, x, y,
                 conservative.runs[i][3], normal.runs[i][3], face);
          if (!geometry && evaluated.runs[i][0] && evaluated.runs[i][3] != turned)
            fail("%s: pixel (%d, %d), its front counterclockwise, saw faces %u, want %u", t.what, x, y,
                 evaluated.runs[i][3], turned);
          // inner coverage: no square that is not inside, and every square inside by more than the uncertainty
          if (inner.runs[i][0] && inner.attributes[i][3] == 1 && !contained(polygon, x, y, -1.0 / 128))
            fail("%s: pixel (%d, %d) has inner coverage, and its square is not inside", t.what, x, y);
          if (reachable(x, y) && contained(polygon, x, y, 1.0 / 128) && inner.attributes[i][3] != 1)
            fail("%s: pixel (%d, %d), inside, has inner coverage %#x", t.what, x, y, inner.attributes[i][3]);
          if ((inner.runs[i][0] != 0) != ran)
            fail(
                "%s: pixel (%d, %d) ran the inner coverage shader %u times, the other %d", t.what, x, y,
                inner.runs[i][0], ran
            );
          // the same pixels and their depth, without a pixel shader and with one, with depth clipped and without
          // depth: the plane's z at the center, or the leading corner's where it has no area, clamped to the
          // viewport's range (17.10)
          auto check_depth = [&](const Pixels &got, const Shape &p, const char *how) {
            bool written = got.depth[i] != 1, must = reachable(x, y) && overlaps(p, x, y, -1.0 / 64),
                 may = reachable(x, y) && overlaps(p, x, y, 1.0 / 128), no_area = flat(p);
            if ((must && !written) || (!may && written))
              fail("%s: pixel (%d, %d)%s: depth %g", t.what, x, y, how, got.depth[i]);
            double range = kViewport.MaxDepth - kViewport.MinDepth,
                   z = no_area ? t.z[leading] : plane(t, x + 0.5, y + 0.5)[2],
                   want = kViewport.MinDepth + std::clamp(z, 0.0, 1.0) * range,
                   error = 1e-3 + (no_area ? 0 : moves(t, 2, x + 0.5, y + 0.5) * range);
            if (written && !(std::fabs(got.depth[i] - want) <= error))
              fail("%s: pixel (%d, %d)%s: depth %g, want %g", t.what, x, y, how, got.depth[i], want);
          };
          check_depth(depth_only, polygon, " without a pixel shader");
          check_depth(shaded, polygon, " with a pixel shader");
          if (!geometry && unclippable) {
            check_depth(unclipped, whole, " without a pixel shader or depth clipping");
            check_depth(shaded_unclipped, whole, " with a pixel shader, without depth clipping");
          }
          if (ran && conservative.attributes[i][2] != 7)
            fail("%s: pixel (%d, %d): flat %u, want the first vertex's 7", t.what, x, y, conservative.attributes[i][2]);
          if (barycentrics && (barycentric.runs[i][0] != 0) != ran)
            fail("%s: pixel (%d, %d) ran the barycentrics shader %u times, the other %d", t.what, x, y,
                 barycentric.runs[i][0], ran);
          if (barycentrics && ran && barycentric.attributes[i][2] != 7)
            fail("%s: pixel (%d, %d): flat %u beside barycentrics, want the first vertex's 7", t.what, x, y,
                 barycentric.attributes[i][2]);
          // the attributes are their plane's at the center, inside or not (15.17.4), the leading vertex's where the
          // triangle has no area; noperspective ones are taken of a triangle before the eye. inside, they are the
          // ordinary rasterizer's
          for (int a = 0; a < 2 && ran; a++) {
            float got;
            memcpy(&got, &conservative.attributes[i][a], 4);
            double want = degenerate ? (a ? 10.0 * leading : 1.0 + 2 * leading) : plane(t, x + 0.5, y + 0.5)[a],
                   error = 1e-3 * std::max(1.0, std::fabs(want)) + (degenerate ? 0 : moves(t, a, x + 0.5, y + 0.5));
            const char *name = a ? "noperspective" : "perspective";
            if (!std::isnan(want) && (!a || unclippable) && !(std::fabs(got - want) <= error))
              fail("%s: pixel (%d, %d): %s %g, want %g", t.what, x, y, name, got, want);
            float own;
            memcpy(&own, &barycentric.attributes[i][a], 4);
            if (barycentrics && !std::isnan(want) && (!a || unclippable) && !(std::fabs(own - want) <= error))
              fail("%s: pixel (%d, %d): %s %g of barycentrics, want %g", t.what, x, y, name, own, want);
            float normal_got;
            memcpy(&normal_got, &normal.attributes[i][a], 4);
            if ((!a || unclippable) && reachable(x, y) && center_inside(polygon, x, y, 0.05) && normal.runs[i][0] &&
                !(std::fabs(got - normal_got) <= error))
              fail("%s: pixel (%d, %d): %s %g, without conservative rasterization %g", t.what, x, y, name, got,
                   normal_got);
          }
          // and as the shader evaluates it there
          if (!geometry && ran && reachable(x, y) && center_inside(polygon, x, y, 0.05) && normal.runs[i][0]) {
            float got, want;
            memcpy(&got, &evaluated.attributes[i][0], 4);
            memcpy(&want, &normal.attributes[i][0], 4);
            if (!(std::fabs(got - want) <= 1e-3f * std::max(1.0f, std::fabs(want)) + moves(t, 0, x + 0.5, y + 0.5)))
              fail("%s: pixel (%d, %d): perspective evaluated %g, without conservative rasterization %g", t.what, x, y, got,
                   want);
          }
          // an ordinary rasterizer's pixel is a conservative one's
          if (normal.target[i] == 1 && !ran)
            fail("%s: pixel (%d, %d) is rasterized without conservative rasterization, not with it", t.what, x, y);
        }
      if (geometry == 6 && conservative.runs[pixels - 1][3] != 1)
        fail("%s: the mesh shader ran %u times, want once", t.what, conservative.runs[pixels - 1][3]);
    }
  printf(
      "%s: %u mismatches over %zu triangles\n", failures ? "failed" : "passed", failures, kPaths * std::size(triangles)
  );
  return failures != 0;
}
