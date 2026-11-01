// contract: conservative rasterization of a vertex shader's triangles at one sample, where Mullion's mesh stage passes
// on the hull of each triangle's pixels (the triangle as clipped, grown by the pixel's square) in a fan of triangles
// its threads share, and the pixel shader tests no pixel. a triangle covers every pixel whose square it overlaps and
// none farther than tier 2's uncertainty (a 256th of a pixel) from it, a triangle of no area (a segment, a point) as
// well, which faces back; the pixel shader runs once a pixel, sees the first vertex's flat input, and the depth the
// plane has at the pixel's center, clamped to the viewport's range, whether it may discard or not. all of it holds
// draw after draw: for the viewport each draw has, set between draws of one pipeline, and for the depth clipping of
// the pipeline each draw has, set between draws of one viewport; and a geometry shader's pipeline drawn after one
// still reads its root arguments (which the hull's mesh stage does not).
// D3D11.3 functional specification 15.17 (Conservative Rasterization): "any pixel ... touched by the primitive must be
// rasterized"; 15.17.2, the tiers' uncertainty regions; 15.4, clipping to "0 <= z <= w" or not at all; 15.11.1, "zero
// area geometry is considered back facing"; 16.2, the shader "is invoked once per covered pixel"; 17.10, depth
// "always clamped" to the viewport's MinDepth and MaxDepth.
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#15.17%20Conservative%20Rasterization
// D3D12_CONSERVATIVE_RASTERIZATION_TIER: "Tier_2 reduces the maximum uncertainty region to 1/256 and requires
// post-snap degenerate triangles not be culled".
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_conservative_rasterization_tier
#include "d3d12_test.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

static const char hlsl[] = R"hlsl(
cbuffer C : register(b0) { float2 shift; }
struct V {
  float4 position : SV_Position;
  nointerpolation uint flat : FLAT;
};
V vs(float4 position : POSITION, uint flat : FLAT) {
  V v = {position, flat};
  return v;
}
// the triangle moved by the root constants, which a geometry shader reads in Mullion's mesh stage
[maxvertexcount(3)] void gs(triangle V v[3], inout TriangleStream<V> s) {
  for (uint i = 0; i < 3; i++) {
    V o = v[i];
    o.position.xy += shift * o.position.w;
    s.Append(o);
  }
}
// per pixel: how many times the shader ran, the faces it saw (1 back, 2 front), and the most flat input
RWStructuredBuffer<uint4> o : register(u0);
float ps(V v, bool front : SV_IsFrontFace) : SV_Target {
  uint2 p = uint2(v.position.xy);
  uint i = p.y * WIDTH + p.x, was;
  InterlockedAdd(o[i].x, 1, was);
  InterlockedOr(o[i].y, front ? 2 : 1, was);
  InterlockedMax(o[i].z, v.flat, was);
  return 1;
}
// one that writes no memory, and one that may discard (it never does): Metal clamps their depth differently
float ps_plain(V v) : SV_Target { return 1; }
float ps_discard(V v) : SV_Target {
  if (v.flat == 12345)
    discard;
  return 1;
}
)hlsl";

static const UINT kWidth = 32, kHeight = 32, kPixels = kWidth * kHeight;
// the first viewport is offset in the target and its depth range is not 0 to 1, which a clamp shows; the second is
// another place and size
static const D3D12_VIEWPORT kViewports[2] = {{2, 3, 26, 26, 0.25f, 0.75f}, {7, 1, 18, 30, 0.1f, 0.9f}};

using Point = std::array<double, 2>;
using Shape = std::vector<Point>;

struct Triangle {
  const char *what;
  double window[3][2]; // the corners in the target's pixels, through the first viewport
  double z[3];         // and their z (w is 1)
};

// the corner in clip space, through the first viewport: another viewport puts it elsewhere
static std::array<double, 4>
clip(const Triangle &t, int k) {
  auto &vp = kViewports[0];
  return {(t.window[k][0] - vp.TopLeftX) / vp.Width * 2 - 1, 1 - (t.window[k][1] - vp.TopLeftY) / vp.Height * 2, t.z[k],
          1};
}

// the triangle as a viewport and depth clipping take it (0 <= z <= w; w is 1, so z is linear in the window)
static Shape
drawn(const Triangle &t, const D3D12_VIEWPORT &vp, bool depth_clips) {
  std::vector<std::array<double, 3>> p;
  for (int k = 0; k < 3; k++) {
    auto c = clip(t, k);
    p.push_back({vp.TopLeftX + (c[0] + 1) * vp.Width / 2, vp.TopLeftY + (1 - c[1]) * vp.Height / 2, c[2]});
  }
  for (int plane = 0; plane < 2 * depth_clips; plane++) {
    auto inside = [&](auto &v) { return plane ? 1 - v[2] : v[2]; };
    std::vector<std::array<double, 3>> kept;
    for (size_t k = 0; k < p.size(); k++) {
      auto &a = p[k], &b = p[(k + 1) % p.size()];
      double da = inside(a), db = inside(b);
      if (da >= 0)
        kept.push_back(a);
      if ((da >= 0) != (db >= 0)) {
        std::array<double, 3> m;
        for (int c = 0; c < 3; c++)
          m[c] = a[c] + da / (da - db) * (b[c] - a[c]);
        kept.push_back(m);
      }
    }
    p = kept;
  }
  Shape out;
  for (auto &v : p)
    out.push_back({v[0], v[1]});
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

// snapped as a rasterizer snaps it, to a 256th of a pixel (15.16)
static Shape
snap(Shape p) {
  for (auto &corner : p)
    for (auto &c : corner)
      c = std::nearbyint(c * 256) / 256;
  return p;
}

// whether the square of pixel (px, py), grown by `grow` on each side, overlaps the polygon (`strict`: more than
// touches it): the separating axis test, the square's axes, then the edges' normals
static bool
overlaps(const Shape &p, int px, int py, double grow, bool strict) {
  if (p.empty())
    return false;
  std::vector<Point> axes = {{1, 0}, {0, 1}};
  for (size_t k = 0; k < p.size(); k++)
    axes.push_back({p[(k + 1) % p.size()][1] - p[k][1], p[k][0] - p[(k + 1) % p.size()][0]});
  for (auto [dx, dy] : axes) {
    if (!dx && !dy)
      continue;
    double low = 1e30, high = -1e30;
    for (auto &v : p)
      low = std::min(low, v[0] * dx + v[1] * dy), high = std::max(high, v[0] * dx + v[1] * dy);
    double center = (px + 0.5) * dx + (py + 0.5) * dy, radius = (0.5 + grow) * (std::abs(dx) + std::abs(dy));
    if (strict ? high <= center - radius || low >= center + radius : high < center - radius || low > center + radius)
      return false;
  }
  return true;
}

// whether a pixel's center is inside the triangle by a margin (an ordinary rasterizer's coverage, away from ties)
static int
center_inside(const Shape &p, int px, int py, double margin) {
  double sign = area(p) > 0 ? 1 : -1;
  int inside = 1, outside = 0;
  for (size_t k = 0; k < p.size(); k++) {
    auto &a = p[k], &b = p[(k + 1) % p.size()];
    double nx = (a[1] - b[1]) * sign, ny = (b[0] - a[0]) * sign;
    double value = (nx * (px + 0.5 - a[0]) + ny * (py + 0.5 - a[1])) / std::hypot(nx, ny);
    inside &= value > margin;
    outside |= value < -margin;
  }
  return inside ? 1 : outside ? 0 : -1;
}

// the plane's z at a point of the window, from the corners' barycentrics there
static double
plane_z(const Triangle &t, const D3D12_VIEWPORT &vp, double x, double y) {
  double p[3][2];
  for (int k = 0; k < 3; k++) {
    auto c = clip(t, k);
    p[k][0] = vp.TopLeftX + (c[0] + 1) * vp.Width / 2, p[k][1] = vp.TopLeftY + (1 - c[1]) * vp.Height / 2;
  }
  double d = (p[1][0] - p[0][0]) * (p[2][1] - p[0][1]) - (p[2][0] - p[0][0]) * (p[1][1] - p[0][1]);
  double b1 = ((x - p[0][0]) * (p[2][1] - p[0][1]) - (p[2][0] - p[0][0]) * (y - p[0][1])) / d;
  double b2 = ((p[1][0] - p[0][0]) * (y - p[0][1]) - (x - p[0][0]) * (p[1][1] - p[0][1])) / d;
  return t.z[0] + b1 * (t.z[1] - t.z[0]) + b2 * (t.z[2] - t.z[0]);
}

struct Vertex {
  float position[4];
  UINT flat;
};

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
  if (!expect(options.ConservativeRasterizationTier >= D3D12_CONSERVATIVE_RASTERIZATION_TIER_2,
              "conservative rasterization tier %u, want 2 or more", options.ConservativeRasterizationTier))
    return verdict();

  std::vector<std::string> defines = {"WIDTH=" + std::to_string(kWidth)};
  auto vs = compiler.compile(hlsl, "vs", "vs", defines), gs = compiler.compile(hlsl, "gs", "gs", defines),
       ps = compiler.compile(hlsl, "ps", "ps", defines), ps_plain = compiler.compile(hlsl, "ps_plain", "ps", defines),
       ps_discard = compiler.compile(hlsl, "ps_discard", "ps", defines);
  if (!expect(!vs.empty() && !gs.empty() && !ps.empty() && !ps_plain.empty() && !ps_discard.empty(), "shaders compile"))
    return verdict();

  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_UAV}, {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}};
  params[1].Constants = {0, 0, 2};
  auto rs = root_signature(
      device.Get(), {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT}
  );
  const D3D12_INPUT_ELEMENT_DESC elements[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"FLAT", 0, DXGI_FORMAT_R32_UINT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  };
  // the pipelines: conservative, counting the shader's runs, with depth clipping and without; writing depth with a
  // shader that writes no memory and one that may discard; and a geometry shader's, not conservative
  auto pipeline = [&](const std::string &pixel, bool conservative, bool depth_clips, bool depth, bool geometry,
                      ComPtr<ID3D12PipelineState> &out) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = rs.Get();
    desc.VS = bytecode(vs);
    if (geometry)
      desc.GS = bytecode(gs);
    desc.PS = bytecode(pixel);
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = ~0u;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = depth_clips;
    desc.RasterizerState.ConservativeRaster =
        conservative ? D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON : D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    desc.InputLayout = {elements, (UINT)std::size(elements)};
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
    desc.SampleDesc = {1, 0};
    if (depth) {
      desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
      desc.DepthStencilState.DepthEnable = TRUE;
      desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
      desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    }
    return device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&out));
  };
  ComPtr<ID3D12PipelineState> counted, unclipped, plain, discarding, geometry;
  CHECK(pipeline(ps, true, true, false, false, counted));
  CHECK(pipeline(ps, true, false, false, false, unclipped));
  CHECK(pipeline(ps_plain, true, true, true, false, plain));
  CHECK(pipeline(ps_discard, true, true, true, false, discarding));
  CHECK(pipeline(ps, false, true, false, true, geometry));

  // two regions of counts, a draw each
  const UINT64 region = kPixels * 16, uav_bytes = 2 * region;
  auto uav = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, uav_bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto zeros = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, uav_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto counts = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, uav_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  auto vertices = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 2 * 3 * sizeof(Vertex), D3D12_RESOURCE_STATE_GENERIC_READ);
  const UINT pitch = 256; // a row of the depth read back, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT
  auto depths = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, pitch * kHeight * 2, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(uav && zeros && counts && vertices && depths, "buffers are made"))
    return verdict();
  {
    void *bytes;
    CHECK(zeros->Map(0, nullptr, &bytes));
    memset(bytes, 0, uav_bytes);
    zeros->Unmap(0, nullptr);
  }
  ComPtr<ID3D12Resource> target, depth;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  {
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, kWidth, kHeight, 1, 1, DXGI_FORMAT_R32_FLOAT,
                             {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                          nullptr, IID_PPV_ARGS(&target)));
    desc.Format = DXGI_FORMAT_D32_FLOAT;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clear{DXGI_FORMAT_D32_FLOAT};
    clear.DepthStencil.Depth = 1;
    CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
                                          IID_PPV_ARGS(&depth)));
  }
  ComPtr<ID3D12DescriptorHeap> rtvs, dsvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1}, dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  CHECK(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  auto dsv = dsvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  device->CreateDepthStencilView(depth.Get(), nullptr, dsv);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  // two draws in one pass, one list: each its triangle, pipeline and viewport, counting in its own region; the depth
  // of the last is read back
  // each region of counts its own, unless a draw shares the one before: a draw sets only the state that changes, so
  // what it keeps is the draw before's
  struct Draw {
    const Triangle *triangle;
    ID3D12PipelineState *pipeline;
    const D3D12_VIEWPORT *viewport;
    bool shares_region = false;
  };
  struct Result {
    UINT counts[2][kPixels][4];
    float depth[kPixels];
  };
  auto run = [&](std::initializer_list<Draw> draws, bool with_depth, Result &out) -> HRESULT {
    Vertex *v;
    HRESULT hr;
    if (FAILED(hr = vertices->Map(0, nullptr, (void **)&v)))
      return hr;
    int n = 0;
    for (auto &d : draws)
      for (int k = 0; k < 3; k++, n++) {
        auto c = clip(*d.triangle, k);
        v[n] = {{(float)c[0], (float)c[1], (float)c[2], (float)c[3]}, 7u + k};
      }
    vertices->Unmap(0, nullptr);
    allocator->Reset();
    list->Reset(allocator.Get(), nullptr);
    transition(list.Get(), uav.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyBufferRegion(uav.Get(), 0, zeros.Get(), 0, uav_bytes);
    transition(list.Get(), uav.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const float clear[4] = {};
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
    list->OMSetRenderTargets(1, &rtv, FALSE, with_depth ? &dsv : nullptr);
    list->SetGraphicsRootSignature(rs.Get());
    // 8 pixels right in the first viewport, for the geometry shader
    const float shift[2] = {16.0f / kViewports[0].Width, 0};
    list->SetGraphicsRoot32BitConstants(1, 2, shift, 0);
    D3D12_RECT scissor{0, 0, (LONG)kWidth, (LONG)kHeight};
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW view{vertices->GetGPUVirtualAddress(), UINT(n * sizeof(Vertex)), sizeof(Vertex)};
    list->IASetVertexBuffers(0, 1, &view);
    UINT i = 0, at = 0;
    const D3D12_VIEWPORT *set = nullptr;
    for (auto &d : draws) {
      list->SetPipelineState(d.pipeline);
      if (set != d.viewport)
        list->RSSetViewports(1, set = d.viewport);
      if (!i || !d.shares_region)
        list->SetGraphicsRootUnorderedAccessView(0, uav->GetGPUVirtualAddress() + at++ * region);
      list->DrawInstanced(3, 1, 3 * i, 0);
      i++;
    }
    transition(list.Get(), uav.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(counts.Get(), 0, uav.Get(), 0, uav_bytes);
    transition(list.Get(), uav.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (with_depth) {
      transition(list.Get(), depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION from{depth.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}, to{depths.Get()};
      to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      to.PlacedFootprint = {0, {DXGI_FORMAT_D32_FLOAT, kWidth, kHeight, 1, pitch}};
      list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
      transition(list.Get(), depth.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }
    if (FAILED(hr = submit(device.Get(), queue.Get(), list.Get())))
      return hr;
    void *bytes;
    if (FAILED(hr = counts->Map(0, nullptr, &bytes)))
      return hr;
    memcpy(out.counts, bytes, sizeof(out.counts));
    counts->Unmap(0, nullptr);
    if (with_depth) {
      if (FAILED(hr = depths->Map(0, nullptr, &bytes)))
        return hr;
      for (UINT y = 0; y < kHeight; y++)
        memcpy(&out.depth[y * kWidth], (char *)bytes + y * pitch, kWidth * sizeof(float));
      depths->Unmap(0, nullptr);
    }
    return S_OK;
  };

  // what a draw's region holds against the triangle as the viewport and the depth clipping take it: every pixel it
  // overlaps run once, none farther than a 256th of a pixel, each seeing its face and the first vertex's flat input
  auto check = [&](const Triangle &t, const D3D12_VIEWPORT &vp, bool depth_clips, const UINT (&got)[kPixels][4],
                   const char *how) {
    auto shape = drawn(t, vp, depth_clips), snapped = snap(shape);
    bool degenerate = area(snapped) == 0;
    UINT face = degenerate || area(snapped) < 0 ? 1 : 2;
    unsigned off = 0, hits = 0;
    for (int y = 0; y < (int)kHeight; y++)
      for (int x = 0; x < (int)kWidth; x++) {
        auto &p = got[y * kWidth + x];
        // the viewport's pixels: their centers in it
        bool in_viewport = x + 0.5 >= vp.TopLeftX && x + 0.5 < vp.TopLeftX + vp.Width && y + 0.5 >= vp.TopLeftY &&
                           y + 0.5 < vp.TopLeftY + vp.Height;
        bool must = in_viewport && !shape.empty() && overlaps(shape, x, y, 0, true);
        bool may = in_viewport && !snapped.empty() && overlaps(snapped, x, y, 1.0 / 256, false);
        hits += p[0] != 0;
        bool ok = (!must || p[0] == 1) && (p[0] <= 1) && (may || p[0] == 0) &&
                  (!p[0] || (p[1] == face && p[2] == 7));
        if (!ok && off++ < 6)
          printf("  %s, %s: pixel (%d, %d) ran %u times, faces %u, flat %u; overlapped %d, near %d, want face %u\n", t.what,
                 how, x, y, p[0], p[1], p[2], must, may, face);
      }
    expect(!off && hits, "%s, %s: %u pixels wrong of %u rasterized", t.what, how, off, hits);
  };

  const Triangle needle{"a needle", {{3.25, 4.5}, {26.75, 9.125}, {4.5, 6.0}}, {0.5, 0.5, 0.5}};
  const Triangle backwards{"a needle the other way round", {{3.25, 4.5}, {4.5, 6.0}, {26.75, 9.125}}, {0.5, 0.5, 0.5}};
  const Triangle square{"a triangle of edges along the axes", {{6, 6}, {20, 6}, {6, 20}}, {0.5, 0.5, 0.5}};
  const Triangle segment{"a segment", {{5, 25}, {21, 29}, {13, 27}}, {0.5, 0.5, 0.5}};
  const Triangle point{"a point", {{20.25, 14.75}, {20.25, 14.75}, {20.25, 14.75}}, {0.5, 0.5, 0.5}};
  const Triangle across{"a triangle across the near plane", {{4, 12}, {28, 14}, {10, 27}}, {-0.75, 0.5, 0.9}};
  // its left edge a 128th of a pixel into the column left of it, its top a 128th into the row above
  const Triangle grazing{"a triangle a 128th of a pixel into its neighbours", {{12 - 1.0 / 128, 4 - 1.0 / 128},
                                                                            {22, 4 - 1.0 / 128}, {12 - 1.0 / 128, 14}},
                         {0.5, 0.5, 0.5}};
  for (auto t : {&needle, &backwards, &square, &segment, &point, &across, &grazing}) {
    step("%s", t->what);
    static Result got;
    CHECK(run({{t, counted.Get(), &kViewports[0]}, {t, unclipped.Get(), &kViewports[0]}}, false, got));
    check(*t, kViewports[0], true, got.counts[0], "depth clipped");
    check(*t, kViewports[0], false, got.counts[1], "not depth clipped");
  }

  // one pipeline, another viewport for its second draw; one viewport, another depth clipping for the second
  step("a needle through one viewport, then another");
  {
    static Result got;
    CHECK(run({{&needle, counted.Get(), &kViewports[0]}, {&needle, counted.Get(), &kViewports[1]}}, false, got));
    check(needle, kViewports[0], true, got.counts[0], "the first viewport");
    check(needle, kViewports[1], true, got.counts[1], "the second viewport");
  }
  step("a triangle across the near plane, clipped, then not");
  {
    static Result got;
    CHECK(run({{&across, counted.Get(), &kViewports[0]}, {&across, unclipped.Get(), &kViewports[0]}}, false, got));
    check(across, kViewports[0], true, got.counts[0], "depth clipped first");
    check(across, kViewports[0], false, got.counts[1], "not depth clipped after");
  }

  // a geometry shader's pipeline after the hull's, with the same root arguments, which moved its triangle 8 pixels
  // right: every pixel whose center is inside the moved triangle runs, none outside, below the point drawn first
  step("a geometry shader after a conservative draw");
  {
    static Result got;
    const Triangle moved{"the moved triangle", {{14.25, 16.25}, {26.75, 16.75}, {14.75, 27.25}}, {0.5, 0.5, 0.5}};
    const Triangle unmoved{"the triangle", {{6.25, 16.25}, {18.75, 16.75}, {6.75, 27.25}}, {0.5, 0.5, 0.5}};
    const Triangle above{"a point above it", {{6.25, 6.75}, {6.25, 6.75}, {6.25, 6.75}}, {0.5, 0.5, 0.5}};
    CHECK(run({{&above, counted.Get(), &kViewports[0]}, {&unmoved, geometry.Get(), &kViewports[0], true}}, false, got));
    auto shape = drawn(moved, kViewports[0], true);
    unsigned off = 0, point = 0;
    for (int y = 0; y < (int)kHeight; y++)
      for (int x = 0; x < (int)kWidth; x++) {
        UINT runs = got.counts[0][y * kWidth + x][0];
        if (y < 12) {
          point += runs;
          continue;
        }
        int in = center_inside(shape, x, y, 1.0 / 64);
        if (in >= 0 && runs != UINT(in) && off++ < 6)
          printf("  the geometry shader's triangle: pixel (%d, %d) ran %u times, want %d\n", x, y, runs, in);
      }
    // the point is inside pixel (6, 6) alone
    expect(point == 1 && got.counts[0][6 * kWidth + 6][0] == 1,
           "the point drawn before the geometry shader covered %u pixels, want pixel (6, 6) alone", point);
    expect(!off, "the geometry shader's triangle, moved by its root constants: %u pixels wrong", off);
  }

  // the depth of a steep triangle at each pixel: the plane's at the center, clamped to the viewport's range
  const Triangle steep{"a steep triangle", {{8, 8}, {13, 8}, {8, 28}}, {0.0, 1.0, 0.5}};
  for (auto [pipeline, how] : {std::pair{plain.Get(), "a shader that writes no memory"},
                              std::pair{discarding.Get(), "a shader that may discard"}}) {
    step("the depth of a steep triangle, %s", how);
    static Result got;
    CHECK(run({{&steep, pipeline, &kViewports[0]}}, true, got));
    auto &vp = kViewports[0];
    auto snapped = snap(drawn(steep, vp, true));
    unsigned off = 0, written = 0;
    for (int y = 0; y < (int)kHeight; y++)
      for (int x = 0; x < (int)kWidth; x++) {
        float d = got.depth[y * kWidth + x];
        if (d == 1)
          continue;
        written++;
        double z = std::clamp(plane_z(steep, vp, x + 0.5, y + 0.5), 0.0, 1.0);
        double want = vp.MinDepth + z * (vp.MaxDepth - vp.MinDepth);
        // what the plane moves within a 256th of a pixel, which snapping the corners moves it
        double slope = std::abs(plane_z(steep, vp, x + 1.5, y + 0.5) - plane_z(steep, vp, x + 0.5, y + 0.5)) +
                       std::abs(plane_z(steep, vp, x + 0.5, y + 1.5) - plane_z(steep, vp, x + 0.5, y + 0.5));
        double tolerance = slope * (vp.MaxDepth - vp.MinDepth) / 256 + 1e-6;
        bool close = overlaps(snapped, x, y, 1.0 / 256, false);
        if ((!close || std::abs(d - want) > tolerance) && off++ < 6)
          printf("  %s: pixel (%d, %d) depth %.7g, want %.7g (near %d)\n", how, x, y, d, want, close);
      }
    expect(!off && written, "the steep triangle's depth, %s: %u of %u pixels wrong", how, off, written);
  }
  return verdict();
}
