// contract: a draw of many triangles whose vertex shader writes cull distances discards exactly the triangles all of
// whose vertices are behind one distance (negative or NaN; D3D11.3 15.4.2,
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm), and draws the others as it would
// without cull distances: the same pixels, SV_PrimitiveID counting every primitive of its instance before it, the
// culled ones too (8.17), SV_InstanceID, and a flat attribute from the triangle's first vertex (the provoking vertex,
// 8.14). it holds for draws with a start vertex, a start index, a base vertex and a start instance, for an indexed list
// with the strip cut index among its indices (a list never cuts, 8.13 / D3D12 IBStripCutValue), for ExecuteIndirect
// of both kinds of draw, for a strip, and for a list drawn between two draws of a geometry shader pipeline that reads
// the same vertex buffers.
// the reference draws the same triangles without cull distances, its pixel shader discarding the triangles the CPU
// finds culled by their SV_PrimitiveID and SV_InstanceID. the draws are thousands of triangles, so a Metal pipeline that
// passes several triangles on per threadgroup meets culled and kept triangles side by side in every group.
#include "d3d12_test.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

static const char hlsl[] = R"hlsl(
struct In { float2 pos : POSITION; float2 cull : DISTANCE; uint id : ID; float2 offset : OFFSET; float flip : FLIP; };
struct V {
  float4 pos : SV_Position;
  nointerpolation uint id : ID;
  nointerpolation uint instance : INSTANCE;
#if CULL
  float2 cull : SV_CullDistance;
#endif
};
ByteAddressBuffer culled : register(t0);
V vs(In i, uint instance : SV_InstanceID) {
  V v;
  v.pos = float4((i.pos + i.offset) / float2(WIDTH, HEIGHT) * float2(2, -2) + float2(-1, 1), 0.5, 1);
  // the root arguments reach the vertex shader too: the table's last word, 1000
  v.id = i.id + culled.Load(2 * PRIMITIVES * 4) - 1000;
  v.instance = instance;
#if CULL
  v.cull = float2(i.cull.x * i.flip, i.cull.y);
#endif
  return v;
}
// a geometry shader that passes triangles on, drawn before and after a case's draw in the same pass
[maxvertexcount(3)] void gs(triangle V i[3], inout TriangleStream<V> s) {
  for (uint k = 0; k < 3; k++)
    s.Append(i[k]);
}
uint4 ps(V v, uint primitive : SV_PrimitiveID) : SV_Target {
#if !CULL
  if (culled.Load((v.instance * PRIMITIVES + primitive) * 4))
    discard;
#endif
  return uint4(primitive + 1, v.instance + 1, v.id, 1);
}
)hlsl";

namespace {

constexpr UINT kWidth = 256, kHeight = 128, kCell = 4, kColumns = 32, kRows = 32;
constexpr UINT kTriangles = kColumns * kRows * 2; // per instance, at most
constexpr UINT kVertices = 65536;                 // vertex 65535 is reached through the strip cut index

struct Vertex {
  float pos[2];
  float cull[2];
  uint32_t id;
};
struct Instance {
  float offset[2];
  float flip;
};

uint32_t seed = 777;
uint32_t
next() {
  seed = seed * 1664525u + 1013904223u;
  return seed >> 8;
}

// many triangles culled, many kept, and many with one vertex in front
float
distance() {
  static const float values[] = {1.0f,  0.0f, -0.0f, 1e-30f, -1.0f, -1e-30f, -2.0f,
                                 -1.0f, -3.0f, std::numeric_limits<float>::quiet_NaN()};
  return values[next() % std::size(values)];
}

bool
behind(float d) {
  return !(d >= 0);
}

} // namespace


// `bench`: GPU time of opaque, depth-tested layers of quads over a 1920 x 1080 window, one draw each, with and without a
// cull distance (MULLION_D3D12_CULL_MESH_ONLY picks how a pipeline with cull distances culls): GRID x GRID * 9 / 16
// quads a layer, drawn back to front, every quad in front of its distance or nine in ten behind it (CULLED)
static const char bench_hlsl[] = R"hlsl(
cbuffer Layer : register(b0) { float z; };
struct BV {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
#if CULL
  float cull : SV_CullDistance;
#endif
};
BV vs_layer(uint id : SV_VertexID) {
  uint quad = id / 6, corner = id % 6, w = GRID, h = GRID * 9 / 16;
  uint2 c = uint2(quad % w, quad / w) + uint2(corner == 1 || corner >= 4 ? 1 : 0, corner == 2 || corner == 3 || corner == 5 ? 1 : 0);
  BV v;
  v.uv = float2(c) / float2(w, h);
  v.pos = float4(v.uv * float2(2, -2) + float2(-1, 1), z, 1);
#if CULL
  v.cull = CULLED && quad % 10 ? -1 : 1;
#endif
  return v;
}
float4 ps_flat(BV v) : SV_Target { return float4(v.uv, z, 1); }
float4 ps_heavy(BV v) : SV_Target {
  float3 c = float3(v.uv, z);
  for (int i = 0; i < 16; i++)
    c = sin(c * 1.7 + c.yzx) * 0.5 + cos(c.zxy * 2.3);
  return float4(c, 1);
}
)hlsl";

static int
bench(Compiler &compiler) {
  const UINT width = 1920, height = 1080, frames = 40;
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER constants{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
  constants.Constants = {0, 0, 1};
  auto rs = root_signature(device.Get(), {1, &constants, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE});
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_CLEAR_VALUE clear_color{DXGI_FORMAT_R8G8B8A8_UNORM, {0, 0, 0, 0}}, clear_depth{DXGI_FORMAT_D32_FLOAT};
  clear_depth.DepthStencil.Depth = 1;
  D3D12_RESOURCE_DESC t{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0},
                        D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> target, depth;
  CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &t, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                        &clear_color, IID_PPV_ARGS(&target)));
  t.Format = DXGI_FORMAT_D32_FLOAT;
  t.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &t, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                        &clear_depth, IID_PPV_ARGS(&depth)));
  ComPtr<ID3D12DescriptorHeap> rtvs, dsvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1}, dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  CHECK(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart(), dsv = dsvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  device->CreateDepthStencilView(depth.Get(), nullptr, dsv);
  ComPtr<ID3D12QueryHeap> queries;
  D3D12_QUERY_HEAP_DESC query_desc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2};
  CHECK(device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&queries)));
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 16, D3D12_RESOURCE_STATE_COPY_DEST);
  UINT64 frequency;
  CHECK(queue->GetTimestampFrequency(&frequency));
  const char *mode = getenv("MULLION_D3D12_CULL_MESH_ONLY");
  printf("MULLION_D3D12_CULL_MESH_ONLY=%s\n", mode ? mode : "(unset)");
  struct Scene {
    const char *name, *ps;
    UINT grid, layers;
    bool culled;
  } scenes[] = {
      {"1 layer of 2,304 quads", "ps_flat", 64, 1, false},
      {"20 layers of 2,304 quads", "ps_flat", 64, 20, false},
      {"20 layers of 2,304 quads, shaded", "ps_heavy", 64, 20, false},
      {"20 layers of 2,304 quads, 90% culled", "ps_flat", 64, 20, true},
      {"4 layers of 36,864 quads", "ps_flat", 256, 4, false},
      {"4 layers of 36,864 quads, 90% culled", "ps_flat", 256, 4, true},
  };
  for (auto &scene : scenes)
    for (int cull = 0; cull < 2; cull++) {
      std::vector<std::string> defines = {"CULL=" + std::to_string(cull), "CULLED=" + std::to_string(scene.culled),
                                          "GRID=" + std::to_string(scene.grid)};
      auto vs = compiler.compile(bench_hlsl, "vs_layer", "vs", defines), ps = compiler.compile(bench_hlsl, scene.ps, "ps", defines);
      if (vs.empty() || ps.empty())
        return 1;
      D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
      desc.pRootSignature = rs.Get();
      desc.VS = bytecode(vs);
      desc.PS = bytecode(ps);
      desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      desc.SampleMask = ~0u;
      desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
      desc.RasterizerState.DepthClipEnable = TRUE;
      desc.DepthStencilState.DepthEnable = TRUE;
      desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
      desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
      desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      desc.NumRenderTargets = 1;
      desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
      desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
      desc.SampleDesc.Count = 1;
      ComPtr<ID3D12PipelineState> pso;
      CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));
      UINT vertices = scene.grid * (scene.grid * 9 / 16) * 6;
      std::vector<double> times;
      for (UINT f = 0; f < frames; f++) {
        CHECK(allocator->Reset());
        CHECK(list->Reset(allocator.Get(), pso.Get()));
        list->ClearRenderTargetView(rtv, clear_color.Color, 0, nullptr);
        list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
        list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        list->SetGraphicsRootSignature(rs.Get());
        D3D12_VIEWPORT viewport{0, 0, (float)width, (float)height, 0, 1};
        D3D12_RECT scissor{0, 0, (LONG)width, (LONG)height};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        for (UINT l = 0; l < scene.layers; l++) {
          float z = 0.1f + 0.8f * (scene.layers - 1 - l) / scene.layers;
          list->SetGraphicsRoot32BitConstants(0, 1, &z, 0);
          list->DrawInstanced(vertices, 1, 0, 0);
        }
        list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, readback.Get(), 0);
        CHECK(submit(device.Get(), queue.Get(), list.Get()));
        UINT64 *stamps;
        CHECK(readback->Map(0, nullptr, (void **)&stamps));
        if (f >= 5)
          times.push_back(double(stamps[1] - stamps[0]) * 1000 / frequency);
        readback->Unmap(0, nullptr);
      }
      std::sort(times.begin(), times.end());
      printf("bench %-40s %-8s gpu median %.3f ms (p10 %.3f)\n", scene.name, cull ? "cull" : "no-cull",
             times[times.size() / 2], times[times.size() / 10]);
    }
  return 0;
}

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  if (argc > 2 && !strcmp(argv[2], "bench"))
    return bench(compiler);
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));

  // vertices: cell (x, y) of the grid has corners at its own four vertices, each with its own distances; vertex 65535
  // is the last cell's third corner again
  std::vector<Vertex> vertices(kVertices);
  for (UINT y = 0; y < kRows; y++)
    for (UINT x = 0; x < kColumns; x++)
      for (UINT k = 0; k < 4; k++) {
        UINT i = (y * kColumns + x) * 4 + k;
        auto &v = vertices[i];
        v.pos[0] = float(x * kCell + (k & 1 ? kCell : 0));
        v.pos[1] = float(y * kCell + (k & 2 ? kCell : 0));
        v.cull[0] = distance();
        v.cull[1] = next() % 4 ? 1.0f : distance();
        v.id = i;
      }
  vertices[kVertices - 1] = vertices[(kColumns * kRows - 1) * 4 + 2];
  vertices[kVertices - 1].id = kVertices - 1;
  // two instances side by side, the second with the first distance's sign flipped
  const Instance instances[3] = {{{0, 0}, 1}, {{0, 0}, 1}, {{128, 0}, -1}};

  // the cases: each a list of triangles (vertex indices), drawn one of several ways
  enum Kind { List, Indexed, Cut, IndirectList, IndirectIndexed, Strip, BetweenGS };
  struct Case {
    const char *name;
    Kind kind;
    std::vector<uint32_t> indices; // of the draw, after its start
    UINT start, base, first_instance, count;
  };
  std::vector<Case> cases;
  {
    // a list: vertices in order from a start vertex, each cell's two triangles
    Case c{"a list from a start vertex and instance", List, {}, 4, 0, 1, 2};
    for (UINT cell = 1; cell < kColumns * kRows; cell++)
      for (UINT k : {0u, 1u, 2u, 2u, 1u, 3u})
        c.indices.push_back(cell * 4 + k);
    cases.push_back(c);
    // indexed, from a start index, with a base vertex
    Case d{"an indexed list from a start index, with a base vertex", Indexed, {}, 6, 4, 1, 2};
    for (UINT cell = 0; cell + 1 < kColumns * kRows; cell++)
      for (UINT k : {0u, 1u, 2u, 2u, 1u, 3u})
        d.indices.push_back(cell * 4 + k);
    cases.push_back(d);
    // an indexed list with the strip cut index among its indices
    Case e{"an indexed list with index 0xffff", Cut, {}, 0, 0, 1, 2};
    for (UINT cell = 0; cell + 1 < kColumns * kRows; cell++)
      for (UINT k : {0u, 1u, 2u, 2u, 1u, 3u})
        e.indices.push_back(cell * 4 + k);
    uint32_t last = (kColumns * kRows - 1) * 4;
    for (uint32_t k : {last, last + 1, kVertices - 1, last + 2, last + 1, last + 3})
      e.indices.push_back(k);
    cases.push_back(e);
    Case f = c;
    f.name = "ExecuteIndirect of a list";
    f.kind = IndirectList;
    cases.push_back(f);
    Case g = d;
    g.name = "ExecuteIndirect of an indexed list";
    g.kind = IndirectIndexed;
    cases.push_back(g);
    // a strip down the first column, zigzagging between its cells' left and right corners: triangle n is
    // (n, n + 1, n + 2), odd ones (n + 1, n, n + 2) (8.12)
    Case h{"a strip", Strip, {}, 0, 0, 1, 2};
    for (UINT y = 0; y < kRows; y++)
      for (UINT k : {0u, 1u})
        h.indices.push_back((y * kColumns) * 4 + k);
    cases.push_back(h);
    Case i = c;
    i.name = "a list between geometry shader draws";
    i.kind = BetweenGS;
    cases.push_back(i);
  }

  // the root signature: the reference's table of culled triangles
  D3D12_ROOT_PARAMETER srv{D3D12_ROOT_PARAMETER_TYPE_SRV};
  srv.Descriptor = {0, 0};
  srv.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  auto rs = root_signature(device.Get(), {1, &srv, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT});
  if (!rs) {
    printf("failed: no root signature\n");
    return 1;
  }
  ComPtr<ID3D12PipelineState> pso[2], gs_pso; // the case's, the reference's; the geometry shader's
  for (int reference = 0; reference < 2; reference++) {
    std::vector<std::string> defines = {"CULL=" + std::to_string(1 - reference), "WIDTH=" + std::to_string(kWidth),
                                        "HEIGHT=" + std::to_string(kHeight), "PRIMITIVES=" + std::to_string(kTriangles)};
    auto vs = compiler.compile(hlsl, "vs", "vs", defines), ps = compiler.compile(hlsl, "ps", "ps", defines);
    if (vs.empty() || ps.empty()) {
      printf("failed: the shaders do not compile\n");
      return 1;
    }
    D3D12_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0},
        {"DISTANCE", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8},
        {"ID", 0, DXGI_FORMAT_R32_UINT, 0, 16},
        {"OFFSET", 0, DXGI_FORMAT_R32G32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"FLIP", 0, DXGI_FORMAT_R32_FLOAT, 1, 8, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
    };
    {
      D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
      desc.pRootSignature = rs.Get();
      desc.VS = bytecode(vs);
      desc.PS = bytecode(ps);
      desc.InputLayout = {elements, (UINT)std::size(elements)};
      desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      desc.SampleMask = ~0u;
      desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
      desc.RasterizerState.DepthClipEnable = TRUE;
      // a list never cuts; the strip pipeline cuts at 0xffff
      desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
      desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      desc.NumRenderTargets = 1;
      desc.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_UINT;
      desc.SampleDesc.Count = 1;
      CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso[reference])));
      if (reference) {
        auto gs = compiler.compile(hlsl, "gs", "gs", defines);
        if (gs.empty()) {
          printf("failed: the geometry shader does not compile\n");
          return 1;
        }
        desc.GS = bytecode(gs);
        CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&gs_pso)));
      }
    }
  }

  auto upload = [&](const void *data, size_t size) {
    auto b = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, std::max<size_t>(size, 4), D3D12_RESOURCE_STATE_GENERIC_READ);
    void *bytes;
    if (b && SUCCEEDED(b->Map(0, nullptr, &bytes))) {
      memcpy(bytes, data, size);
      b->Unmap(0, nullptr);
    }
    return b;
  };
  auto vb = upload(vertices.data(), vertices.size() * sizeof(Vertex));
  auto ib_instances = upload(instances, sizeof(instances));

  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  D3D12_INDIRECT_ARGUMENT_DESC draw_arg{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW}, indexed_arg{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED};
  D3D12_COMMAND_SIGNATURE_DESC draw_sig{sizeof(D3D12_DRAW_ARGUMENTS), 1, &draw_arg},
      indexed_sig{sizeof(D3D12_DRAW_INDEXED_ARGUMENTS), 1, &indexed_arg};
  ComPtr<ID3D12CommandSignature> signatures[2];
  CHECK(device->CreateCommandSignature(&draw_sig, nullptr, IID_PPV_ARGS(&signatures[0])));
  CHECK(device->CreateCommandSignature(&indexed_sig, nullptr, IID_PPV_ARGS(&signatures[1])));

  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, kWidth, kHeight, 1, 1,
                                  DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                  D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  D3D12_CLEAR_VALUE clear{DXGI_FORMAT_R32G32B32A32_UINT, {0, 0, 0, 0}};
  ComPtr<ID3D12Resource> targets[2];
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  auto rtv_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  for (int i = 0; i < 2; i++) {
    CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                          &clear, IID_PPV_ARGS(&targets[i])));
    device->CreateRenderTargetView(targets[i].Get(), nullptr, {rtvs->GetCPUDescriptorHandleForHeapStart().ptr + i * rtv_size});
  }
  const UINT64 row = kWidth * 16;
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 2 * row * kHeight, D3D12_RESOURCE_STATE_COPY_DEST);

  unsigned failures = 0, culled_total = 0, kept_total = 0;
  for (auto &c : cases) {
    // the draw's vertex indices, from where it starts, and the triangles they make
    bool strip = c.kind == Strip, indexed = c.kind == Indexed || c.kind == Cut || c.kind == IndirectIndexed;
    std::vector<std::array<uint32_t, 3>> triangles;
    auto at = [&](uint32_t i) { return c.indices[i] + (indexed ? c.base : 0); };
    if (strip)
      for (uint32_t n = 0; n + 2 < c.indices.size(); n++)
        triangles.push_back(n % 2 ? std::array{at(n + 1), at(n), at(n + 2)} : std::array{at(n), at(n + 1), at(n + 2)});
    else
      for (uint32_t n = 0; n + 2 < c.indices.size(); n += 3)
        triangles.push_back({at(n), at(n + 1), at(n + 2)});
    // what the reference leaves out, by instance and primitive
    std::vector<uint32_t> table(2 * kTriangles + 1);
    table.back() = 1000;
    for (UINT instance = 0; instance < c.count; instance++)
      for (size_t t = 0; t < triangles.size() && t < kTriangles; t++) {
        bool gone = false;
        float flip = instances[c.first_instance + instance].flip;
        for (int d = 0; d < 2; d++) {
          bool all = true;
          for (auto i : triangles[t])
            all &= behind(d == 0 ? vertices[i].cull[0] * flip : vertices[i].cull[1]);
          gone |= all;
        }
        table[instance * kTriangles + t] = gone;
        (gone ? culled_total : kept_total)++;
      }
    // the draw's vertices from its start: an indexed draw's indices from its start index, a draw without indices'
    // vertices from its start vertex
    std::vector<uint16_t> index_data(c.start, 0);
    for (auto i : c.indices)
      index_data.push_back(uint16_t(i));
    auto ib = upload(index_data.data(), index_data.size() * sizeof(uint16_t));
    auto culled = upload(table.data(), table.size() * sizeof(uint32_t));
    // a draw without indices reads the vertices themselves: lay them out from its start vertex
    std::vector<Vertex> ordered;
    if (!indexed) {
      ordered.assign(c.start, Vertex{});
      for (auto i : c.indices)
        ordered.push_back(vertices[i]);
    }
    auto own_vb = indexed ? vb : upload(ordered.data(), ordered.size() * sizeof(Vertex));
    D3D12_DRAW_ARGUMENTS draw_args{UINT(c.indices.size()), c.count, c.start, c.first_instance};
    D3D12_DRAW_INDEXED_ARGUMENTS indexed_args{UINT(c.indices.size()), c.count, c.start, INT(c.base), c.first_instance};
    auto args = c.kind == IndirectList ? upload(&draw_args, sizeof(draw_args)) : upload(&indexed_args, sizeof(indexed_args));

    CHECK(forget(readback.Get()));
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), nullptr));
    for (int reference = 0; reference < 2; reference++) {
      D3D12_CPU_DESCRIPTOR_HANDLE rtv{rtvs->GetCPUDescriptorHandleForHeapStart().ptr + reference * rtv_size};
      list->ClearRenderTargetView(rtv, clear.Color, 0, nullptr);
      list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
      list->SetGraphicsRootSignature(rs.Get());
      list->SetGraphicsRootShaderResourceView(0, culled->GetGPUVirtualAddress());
      list->SetPipelineState(pso[reference].Get());
      D3D12_VIEWPORT viewport{0, 0, (float)kWidth, (float)kHeight, 0, 1};
      D3D12_RECT scissor{0, 0, (LONG)kWidth, (LONG)kHeight};
      list->RSSetViewports(1, &viewport);
      list->RSSetScissorRects(1, &scissor);
      D3D12_VERTEX_BUFFER_VIEW views[2] = {
          {own_vb->GetGPUVirtualAddress(), UINT(own_vb->GetDesc().Width), sizeof(Vertex)},
          {ib_instances->GetGPUVirtualAddress(), sizeof(instances), sizeof(Instance)},
      };
      list->IASetVertexBuffers(0, 2, views);
      D3D12_INDEX_BUFFER_VIEW ibv{ib->GetGPUVirtualAddress(), UINT(index_data.size() * 2), DXGI_FORMAT_R16_UINT};
      list->IASetIndexBuffer(&ibv);
      list->IASetPrimitiveTopology(strip ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      // the geometry shader pipeline's draws: the same vertex buffers, through the object stage
      auto gs_draw = [&] {
        list->SetPipelineState(gs_pso.Get());
        list->DrawInstanced(30, 1, c.start, c.first_instance);
        list->SetPipelineState(pso[reference].Get());
      };
      switch (c.kind) {
      case BetweenGS:
        gs_draw();
        list->DrawInstanced(UINT(c.indices.size()), c.count, c.start, c.first_instance);
        gs_draw();
        break;
      case List:
      case Strip:
        list->DrawInstanced(UINT(c.indices.size()), c.count, c.start, c.first_instance);
        break;
      case Indexed:
      case Cut:
        list->DrawIndexedInstanced(UINT(c.indices.size()), c.count, c.start, INT(c.base), c.first_instance);
        break;
      case IndirectList:
        list->ExecuteIndirect(signatures[0].Get(), 1, args.Get(), 0, nullptr, 0);
        break;
      case IndirectIndexed:
        list->ExecuteIndirect(signatures[1].Get(), 1, args.Get(), 0, nullptr, 0);
        break;
      }
      transition(list.Get(), targets[reference].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
          src{targets[reference].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      dst.PlacedFootprint = {reference * row * kHeight, {DXGI_FORMAT_R32G32B32A32_UINT, kWidth, kHeight, 1, UINT(row)}};
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      transition(list.Get(), targets[reference].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    HRESULT hr = submit(device.Get(), queue.Get(), list.Get());
    if (FAILED(hr)) {
      printf("failed: %s: the list did not run (%#lx)\n", c.name, (unsigned long)hr);
      return 1;
    }
    uint32_t *pixels;
    CHECK(readback->Map(0, nullptr, (void **)&pixels));
    unsigned wrong = 0, drawn = 0;
    for (UINT i = 0; i < kWidth * kHeight; i++) {
      auto got = pixels + i * 4, want = pixels + kWidth * kHeight * 4 + i * 4;
      drawn += want[3] != 0;
      if (!memcmp(got, want, 16))
        continue;
      if (wrong++ < 6)
        printf("%s: pixel %u,%u: primitive %d instance %d vertex %u, want %d %d %u\n", c.name, i % kWidth, i / kWidth,
               int(got[0]) - 1, int(got[1]) - 1, got[2], int(want[0]) - 1, int(want[1]) - 1, want[2]);
    }
    readback->Unmap(0, nullptr);
    if (drawn < (strip ? 50u : 1000u)) {
      printf("failed: %s: the reference drew %u pixels\n", c.name, drawn);
      return 1;
    }
    failures += wrong;
  }
  if (failures) {
    printf("failed: %u wrong pixels\n", failures);
    return 1;
  }
  printf("passed: %zu draws, %u triangles culled and %u kept\n", cases.size(), culled_total, kept_total);
  return 0;
}
