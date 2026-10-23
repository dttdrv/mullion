// contract: conservative rasterization, tier 3. a triangle drawn with D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON
// covers every pixel whose square it overlaps, and no pixel farther than tier 2's uncertainty (a 256th of a pixel)
// from it, a triangle of no area (a segment, a point) as well, which is not culled. such a pixel has all its samples
// covered (at 4x the test reads the target: Metal runs a 4x pixel's shader more than once, and a discarding run still
// writes UAVs, with or without conservative rasterization), its pixel shader sees the triangle's attributes
// extrapolated from their plane, so a pixel whose center is inside sees what it sees without conservative
// rasterization, a flat input is the first vertex's, and SV_InnerCoverage says whether the square is inside the
// triangle. a pipeline without a pixel shader writes depth to the same pixels. all of it holds as well for triangles a
// geometry shader emits, for those a tessellator outputs, for a geometry shader's after a tessellator, and for a mesh
// shader's (DXIL), with and without an amplification shader: its group outputs the triangle among others without area
// away from it, past the triangles one Metal mesh grows. the device reports tier 3, and with it feature level 12_1.
// D3D11.3 functional specification 15.17 (Conservative Rasterization): "any pixel ... touched by the primitive must be
// rasterized"; 15.17.2, the tiers' uncertainty regions; 15.17.5, "the coverage mask ... is full"; 15.17.4, attributes
// are "extrapolated" for pixels whose centers are outside; 15.17.8, inner coverage.
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#15.17%20Conservative%20Rasterization
// D3D12_CONSERVATIVE_RASTERIZATION_TIER: "Tier_2 reduces the maximum uncertainty region to 1/256 and requires
// post-snap degenerate triangles not be culled"; "Tier_3 maintains a maximum 1/256 uncertainty region and adds support
// for inner input coverage".
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_conservative_rasterization_tier
// each run's SV_Coverage is every sample (15.17.5), and the attributes hold in perspective however steep: a triangle
// whose plane's 1/w is below 0 within a pixel of it.
#include "d3d12_test.hpp"
#include <algorithm>
#include <cmath>
#include <string>

static const char hlsl[] = R"hlsl(
struct V {
  float4 position : SV_Position;
  float perspective : PERSPECTIVE;
  noperspective float screen : SCREEN;
  nointerpolation uint flat : FLAT;
};
V vs(float4 position : POSITION, float perspective : PERSPECTIVE, float screen : SCREEN, uint flat : FLAT) {
  V v = {position, perspective, screen, flat};
  return v;
}
// per pixel: the attributes, then how many times the shader ran there, the samples it covered, and the runs that did
// not cover every sample
RWStructuredBuffer<uint4> o : register(u0);
float ps(V v, uint coverage : SV_Coverage) : SV_Target {
  uint2 p = uint2(v.position.xy);
  uint i = p.y * WIDTH + p.x, was;
  o[i].xyz = uint3(asuint(v.perspective), asuint(v.screen), v.flat);
  InterlockedAdd(o[PIXELS + i].x, 1, was);
  InterlockedOr(o[PIXELS + i].y, coverage, was);
  InterlockedAdd(o[PIXELS + i].z, coverage != (1u << GetRenderTargetSampleCount()) - 1 ? 1 : 0, was);
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
// the perspective input as the shader evaluates it, at the centroid: at 1x, of every sample covered, the center
float ps_evaluate(V v) : SV_Target {
  uint2 p = uint2(v.position.xy);
  uint i = p.y * WIDTH + p.x, was;
  o[i].x = asuint(EvaluateAttributeCentroid(v.perspective));
  InterlockedAdd(o[PIXELS + i].x, 1, was);
  return 1;
}
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
// the viewport is offset in the target, so window coordinates are not the viewport's
static const D3D12_VIEWPORT kViewport{2, 3, 20, 20, 0, 1};

struct Vertex {
  float position[4];
  float perspective, linear;
  UINT flat;
};

struct Triangle {
  const char *what;
  float window[3][2]; // in the target's pixels
  float w[3];         // the corners' clip w: perspective
};

// whether the square of pixel (px, py), grown by `grow` on each side, has a point in the triangle: the separating
// axis test, the square's axes, then the edges' normals
static bool
overlaps(const Triangle &t, int px, int py, double grow) {
  double lo[2] = {px - grow, py - grow}, hi[2] = {px + 1 + grow, py + 1 + grow};
  for (int axis = 0; axis < 2; axis++) {
    double tmin = 1e30, tmax = -1e30;
    for (auto &v : t.window)
      tmin = std::min<double>(tmin, v[axis]), tmax = std::max<double>(tmax, v[axis]);
    if (tmax < lo[axis] || tmin > hi[axis])
      return false;
  }
  for (int i = 0; i < 3; i++) {
    auto &a = t.window[i], &b = t.window[(i + 1) % 3], &c = t.window[(i + 2) % 3];
    double nx = b[1] - a[1], ny = a[0] - b[0];
    double side = nx * (c[0] - a[0]) + ny * (c[1] - a[1]);
    if (side < 0)
      nx = -nx, ny = -ny;
    // the square's corner farthest inside
    double best = -1e30;
    for (double x : {lo[0], hi[0]})
      for (double y : {lo[1], hi[1]})
        best = std::max(best, nx * (x - a[0]) + ny * (y - a[1]));
    if (best < 0)
      return false;
  }
  return true;
}

// the slope in the window of an attribute that is `step` times its corner's index at each corner (the vertices'
// values), taken as linear there
static double
gradient(const Triangle &t, double step) {
  double x1 = t.window[1][0] - t.window[0][0], y1 = t.window[1][1] - t.window[0][1];
  double x2 = t.window[2][0] - t.window[0][0], y2 = t.window[2][1] - t.window[0][1];
  double area = x1 * y2 - x2 * y1, a1 = step, a2 = 2 * step;
  return std::hypot((a1 * y2 - a2 * y1) / area, (a2 * x1 - a1 * x2) / area);
}

static double
area(const Triangle &t) {
  return (t.window[1][0] - t.window[0][0]) * (t.window[2][1] - t.window[0][1]) -
         (t.window[2][0] - t.window[0][0]) * (t.window[1][1] - t.window[0][1]);
}

// whether the square of pixel (px, py), grown by `grow` on each side, is inside the triangle: every corner of it
static bool
contained(const Triangle &t, int px, int py, double grow) {
  double a = area(t);
  if (a == 0)
    return false;
  for (double x : {px - grow, px + 1 + grow})
    for (double y : {py - grow, py + 1 + grow})
      for (int i = 0; i < 3; i++) {
        auto &p = t.window[i], &q = t.window[(i + 1) % 3];
        double e = (q[0] - p[0]) * (y - p[1]) - (q[1] - p[1]) * (x - p[0]);
        if ((a > 0 ? e : -e) < 0)
          return false;
      }
  return true;
}

// whether the pixel's center is inside, as an ordinary rasterizer would take it (away from edges)
static bool
center_inside(const Triangle &t, int px, int py, double margin) {
  double x = px + 0.5, y = py + 0.5;
  double area = ::area(t);
  if (area == 0)
    return false;
  for (int i = 0; i < 3; i++) {
    auto &a = t.window[i], &b = t.window[(i + 1) % 3];
    double e = (b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0]);
    double len = std::hypot(b[0] - a[0], b[1] - a[1]);
    if ((area > 0 ? e : -e) < margin * len)
      return false;
  }
  return true;
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
  auto ps_inner = compiler.compile(hlsl, "ps_inner", "ps", defines);
  if (ps_inner.empty()) {
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
  auto mesh_defines = defines;
  mesh_defines.insert(mesh_defines.end(), {"MESH", "TRIANGLES=40", "REAL=33"});
  std::string ms, ms_amplified, as;
  if (meshes) {
    ms = compiler.compile(hlsl, "ms", "ms_6_5", mesh_defines);
    ms_amplified = compiler.compile(hlsl, "ms_amplified", "ms_6_5", mesh_defines);
    as = compiler.compile(hlsl, "as", "as_6_5", mesh_defines);
    if (ms.empty() || ms_amplified.empty() || as.empty()) {
      printf("failed: the mesh shaders did not compile\n");
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
  // mesh shader (4), after an amplification shader (5)
  const int kPaths = 6;
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
  // a mesh pipeline of the graphics pipeline's state
  auto mesh_pipeline = [&](int path, const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc, ComPtr<ID3D12PipelineState> &out) {
    stream = {};
    stream.rs.data = desc.pRootSignature;
    if (path == 5)
      stream.as.data = bytecode(as);
    stream.ms.data = bytecode(path == 5 ? ms_amplified : ms);
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
  ComPtr<ID3D12PipelineState> pso[kPaths][2][2], depth_pso[kPaths], inner_pso[kPaths];
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
        // and one whose pixel shader reads SV_InnerCoverage
        if (conservative && !msaa && (!meshed(geometry) || meshes)) {
          desc.PS = bytecode(ps_inner);
          if (meshed(geometry)) {
            CHECK(mesh_pipeline(geometry, desc, inner_pso[geometry]));
          } else {
            CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&inner_pso[geometry])));
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
  }
  // and the vertex shader's, testing the depth bounds too: one fragment function does both
  ComPtr<ID3D12PipelineState> bounded_pso;
  {
    struct {
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> rs;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE> vs;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> raster;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1, D3D12_DEPTH_STENCIL_DESC1> depth;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT, D3D12_INPUT_LAYOUT_DESC> layout;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT> depth_format;
    } bounded{};
    bounded.rs.data = rs.Get();
    bounded.vs.data = bytecode(vs_depth);
    bounded.raster.data = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    bounded.raster.data.DepthClipEnable = TRUE;
    bounded.raster.data.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON;
    bounded.depth.data.DepthEnable = TRUE;
    bounded.depth.data.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    bounded.depth.data.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    bounded.depth.data.DepthBoundsTestEnable = TRUE;
    bounded.layout.data = {elements, 1};
    bounded.depth_format.data = DXGI_FORMAT_D32_FLOAT;
    D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(bounded), &bounded};
    CHECK(device2->CreatePipelineState(&desc, IID_PPV_ARGS(&bounded_pso)));
  }

  // and one of the vertex shader's triangles that evaluates the perspective input
  ComPtr<ID3D12PipelineState> evaluate_pso;
  {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs), bytecode(ps_evaluate)};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = ~0u;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON;
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
  // `bound`: the depth bounds' maximum, of a pipeline that tests them
  auto draw = [&](const Triangle &t, ID3D12PipelineState *pipeline, int msaa, bool depth_only, Pixels &out, bool patch,
                  bool mesh = false, float bound = 1) -> HRESULT {
    Vertex *v;
    HRESULT hr;
    if (FAILED(hr = vertices->Map(0, nullptr, (void **)&v)))
      return hr;
    for (int k = 0; k < 3; k++) {
      float x = (t.window[k][0] - kViewport.TopLeftX) / kViewport.Width * 2 - 1;
      float y = 1 - (t.window[k][1] - kViewport.TopLeftY) / kViewport.Height * 2;
      v[k] = {{x * t.w[k], y * t.w[k], 0.5f * t.w[k], t.w[k]}, 1.0f + 2 * k, 10.0f * k, 7u + k};
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
    ComPtr<ID3D12GraphicsCommandList1> list1;
    if (bound < 1 && SUCCEEDED(list.As(&list1)))
      list1->OMSetDepthBounds(0, bound);
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

  static Pixels normal, conservative, multisampled, depth_only, evaluated, inner, bounded, rejected;
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
      ", from a mesh shader after an amplification shader"
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
      CHECK(draw(t, pso[geometry][0][0].Get(), 0, false, normal, patch, mesh));
      CHECK(draw(t, pso[geometry][1][0].Get(), 0, false, conservative, patch, mesh));
      CHECK(draw(t, pso[geometry][1][1].Get(), 1, false, multisampled, patch, mesh));
      CHECK(draw(t, depth_pso[geometry].Get(), 0, true, depth_only, patch, mesh));
      if (!geometry)
        CHECK(draw(t, evaluate_pso.Get(), 0, false, evaluated, false));
      CHECK(draw(t, inner_pso[geometry].Get(), 0, false, inner, patch, mesh));
      // the depth cleared to 1 is in [0, 1], and not in [0, 0.5]
      if (!geometry) {
        CHECK(draw(t, bounded_pso.Get(), 0, true, bounded, false));
        CHECK(draw(t, bounded_pso.Get(), 0, true, rejected, false, false, 0.5f));
      }
      for (int y = 0; y < (int)kHeight; y++)
        for (int x = 0; x < (int)kWidth; x++) {
          int i = y * kWidth + x;
          // certainly overlapped: the square less a sliver; certainly not: farther than the uncertainty region
          bool must = reachable(x, y) && overlaps(t, x, y, -1.0 / 64),
               may = reachable(x, y) && overlaps(t, x, y, 1.0 / 128);
          bool ran = conservative.runs[i][0];
          // one primitive runs a pixel's shader once (D3D11.3 16.1); at 4x Metal may run it more than once, each with
          // some of the samples, as it does without conservative rasterization, so there the runs' samples count
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
          // at 4x, what reached the target: Metal may run a pixel's shader more than once there, and a run that
          // discards still writes UAVs, with or without conservative rasterization
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
          // inner coverage: no square that is not inside, and every square inside by more than the uncertainty
          if (inner.runs[i][0] && inner.attributes[i][3] == 1 && !contained(t, x, y, -1.0 / 128))
            fail("%s: pixel (%d, %d) has inner coverage, and its square is not inside", t.what, x, y);
          if (reachable(x, y) && contained(t, x, y, 1.0 / 128) && inner.attributes[i][3] != 1)
            fail("%s: pixel (%d, %d), inside, has inner coverage %#x", t.what, x, y, inner.attributes[i][3]);
          if ((inner.runs[i][0] != 0) != ran)
            fail(
                "%s: pixel (%d, %d) ran the inner coverage shader %u times, the other %d", t.what, x, y,
                inner.runs[i][0], ran
            );
          bool depth_written = depth_only.depth[i] != 1;
          if ((must && !depth_written) || (!may && depth_written))
            fail("%s: pixel (%d, %d) without a pixel shader: depth %g", t.what, x, y, depth_only.depth[i]);
          if (depth_written && std::fabs(depth_only.depth[i] - 0.5f) > 1e-3f)
            fail("%s: pixel (%d, %d) without a pixel shader: depth %g, want 0.5", t.what, x, y, depth_only.depth[i]);
          if (!geometry && bounded.depth[i] != depth_only.depth[i])
            fail("%s: pixel (%d, %d) testing the depth bounds: depth %g, want %g", t.what, x, y, bounded.depth[i],
                 depth_only.depth[i]);
          if (!geometry && rejected.depth[i] != 1)
            fail("%s: pixel (%d, %d) out of the depth bounds: depth %g, want 1", t.what, x, y, rejected.depth[i]);
          if (ran && conservative.attributes[i][2] != 7)
            fail("%s: pixel (%d, %d): flat %u, want the first vertex's 7", t.what, x, y, conservative.attributes[i][2]);
          // inside, the attributes are the ordinary rasterizer's
          if (ran && reachable(x, y) && center_inside(t, x, y, 0.05) && normal.runs[i][0])
            for (int a = 0; a < 2; a++) {
              float got, want;
              memcpy(&got, &conservative.attributes[i][a], 4);
              memcpy(&want, &normal.attributes[i][a], 4);
              // the rasterizer snaps vertices to a 256th of a pixel, the grown triangle's as well as the triangle's:
              // a sixty-fourth of a pixel of the attribute's slope in the window, which a needle makes steep
              double slope = gradient(t, a ? 10.0 : 2.0);
              if (!(std::fabs(got - want) <= 1e-3f * std::max(1.0f, std::fabs(want)) + slope / 64))
                fail(
                    "%s: pixel (%d, %d): %s %g, without conservative rasterization %g", t.what, x, y,
                    a ? "noperspective" : "perspective", got, want
                );
            }
          // and as the shader evaluates it there
          if (!geometry && ran && reachable(x, y) && center_inside(t, x, y, 0.05) && normal.runs[i][0]) {
            float got, want;
            memcpy(&got, &evaluated.attributes[i][0], 4);
            memcpy(&want, &normal.attributes[i][0], 4);
            if (!(std::fabs(got - want) <= 1e-3f * std::max(1.0f, std::fabs(want))))
              fail("%s: pixel (%d, %d): perspective evaluated %g, without conservative rasterization %g", t.what, x, y, got,
                   want);
          }
          // an ordinary rasterizer's pixel is a conservative one's
          if (normal.target[i] == 1 && !ran)
            fail("%s: pixel (%d, %d) is rasterized without conservative rasterization, not with it", t.what, x, y);
        }
    }
  printf(
      "%s: %u mismatches over %zu triangles\n", failures ? "failed" : "passed", failures, kPaths * std::size(triangles)
  );
  return failures != 0;
}
