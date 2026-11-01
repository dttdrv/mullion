// contract: a vertex shader's cull distances discard whole primitives, and nothing else of them (D3D11.3 15.4.2,
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#15.4.2%20Cull%20Distance): "If all
// vertices of a primitive have a negative value in one cull distance ... the primitive is discarded", "If a vertex has
// a cull distance of NaN, that vertex counts as \"out\" (as if it is < 0)". -0 is not negative. a primitive a cull
// distance keeps renders as it would without cull distances: the same pixels, the same depth, each pixel shader run
// once where it would run (16.2), and its clip distances still clip it (15.4.1).
// each case draws primitives with distances in 1 to 8 components, beside clip distances or not, in one cell of the
// target, scissored to it: lists, strips (each triangle on its own, D3D11.3 8.12) and lists with adjacency, whose
// adjacent vertices are no part of the primitive (8.15). the reference draws the same primitives without cull
// distances, those the distances cull left out, through the same pipeline state. the corners lie on pixel centers and
// edges, far outside the cell, behind the near plane, at several w; and the pipelines take the color target, a depth
// target alone (no pixel shader), a pixel shader that counts its runs in memory (where the clip distances are tested in
// the pixel shader), one whose depth test comes first ([earlydepthstencil]), and 4x multisampling.
#include "d3d12_test.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

static const char hlsl[] = R"hlsl(
struct In { float4 pos : POSITION; float4 d0 : D0; float4 d1 : D1; float2 clip : CLIP; };
struct V {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
#if LAYOUT == 1
#if CULL
  float cull : SV_CullDistance;
#endif
#elif LAYOUT == 2
#if CULL
  float4 cull0 : SV_CullDistance0; float4 cull1 : SV_CullDistance1;
#endif
#elif LAYOUT == 3
  float clip : SV_ClipDistance0;
#if CULL
  float3 cull : SV_CullDistance0;
#endif
#else
  float2 clip : SV_ClipDistance0; float4 t : TEXCOORD1;
#if CULL
  float2 cull : SV_CullDistance1;
#endif
#endif
};
V vs(In i) {
  V v;
  v.pos = i.pos;
  v.uv = i.pos.xy;
#if LAYOUT == 1
#if CULL
  v.cull = i.d0.x;
#endif
#elif LAYOUT == 2
#if CULL
  v.cull0 = i.d0; v.cull1 = i.d1;
#endif
#elif LAYOUT == 3
  v.clip = i.clip.x;
#if CULL
  v.cull = i.d0.xyz;
#endif
#else
  v.clip = i.clip; v.t = i.d1;
#if CULL
  v.cull = i.d0.xy;
#endif
#endif
  return v;
}
float4 ps_color(float4 pos : SV_Position) : SV_Target { return float4(1, 0.5, 0.25, 1); }
RWByteAddressBuffer runs : register(u1);
float4 ps_count(float4 pos : SV_Position) : SV_Target {
  uint2 p = uint2(pos.xy);
  runs.InterlockedAdd((p.y * WIDTH + p.x) * 4, 1);
  return float4(1, 0.5, 0.25, 1);
}
[earlydepthstencil] float4 ps_early(float4 pos : SV_Position) : SV_Target { return float4(1, 0.5, 0.25, 1); }
)hlsl";

namespace {

constexpr UINT kWidth = 256, kCell = 16, kCells = (kWidth / kCell) * (kWidth / kCell);

struct Vertex {
  float pos[4];
  float d[8];
  float clip[2];
  float pad[2];
};

// what each primitive of an item is made of: its vertices, by topology
enum Shape { List, Strip, Adjacency, Indexed };

struct Item {
  Shape shape;
  std::vector<Vertex> vertices;
};

uint32_t seed = 12345;
uint32_t
next() {
  seed = seed * 1664525u + 1013904223u;
  return seed >> 8;
}
float
unit() {
  return (next() & 0xffff) / 65535.0f;
}

const float kNaN = std::numeric_limits<float>::quiet_NaN();
// not negative, then negative or NaN (D3D11.3 15.4.2)
const float kFront[] = {0.0f, -0.0f, 1e-30f, 1.0f, 1000.0f};
const float kBehind[] = {-1.0f, -1e-30f, kNaN, -1000.0f, -std::numeric_limits<float>::infinity()};
float
front() {
  return kFront[next() % std::size(kFront)];
}
float
behind() {
  return kBehind[next() % std::size(kBehind)];
}

bool
is_behind(float d) {
  return !(d >= 0);
}

// the triangles of an item, as three vertices each
std::vector<std::array<Vertex, 3>>
triangles(const Item &item) {
  std::vector<std::array<Vertex, 3>> out;
  auto &v = item.vertices;
  switch (item.shape) {
  case List:
    out.push_back({v[0], v[1], v[2]});
    break;
  case Indexed: // drawn with indices 2, 1, 0
    out.push_back({v[2], v[1], v[0]});
    break;
  case Strip: // triangle i of a strip is (i, i + 1, i + 2), odd ones (i + 1, i, i + 2) (D3D11.3 8.12)
    out.push_back({v[0], v[1], v[2]});
    out.push_back({v[2], v[1], v[3]});
    break;
  case Adjacency: // vertices 0, 2 and 4, the others adjacent (8.15)
    out.push_back({v[0], v[2], v[4]});
    break;
  }
  return out;
}

bool
culled(const std::array<Vertex, 3> &t, unsigned distances) {
  for (unsigned j = 0; j < distances; j++)
    if (is_behind(t[0].d[j]) && is_behind(t[1].d[j]) && is_behind(t[2].d[j]))
      return true;
  return false;
}

// the cell's items: corners on pixel centers and edges (most), far away, at several w, behind the near plane; and
// distances that cull, that leave one vertex in front, and that cull only in the last component in use
Item
make_item(unsigned k, unsigned distances) {
  Item item;
  item.shape = Shape(k % 4);
  size_t count = item.shape == Strip ? 4 : item.shape == Adjacency ? 6 : 3;
  float cx = float(k % (kWidth / kCell) * kCell), cy = float(k / (kWidth / kCell) * kCell);
  unsigned geometry = (k / 4) % 6, pattern = (k / 24) % 6;
  for (size_t i = 0; i < count; i++) {
    float x, y, z = 0.25f + 0.5f * unit(), w = 1;
    if (geometry <= 2) {
      // multiples of half a pixel: edges through pixel centers
      x = cx + 1 + (next() % 29) * 0.5f;
      y = cy + 1 + (next() % 29) * 0.5f;
    } else if (geometry == 3) {
      x = cx + 8 + (i == 1 ? 300.0f : 0) * (next() % 2 ? 1 : -1) + (next() % 7);
      y = cy + 8 + (i == 2 ? 300.0f : 0) * (next() % 2 ? 1 : -1) + (next() % 7);
    } else {
      x = cx + 0.5f + unit() * 15;
      y = cy + 0.5f + unit() * 15;
      if (geometry == 4)
        w = 0.25f + 3 * unit();
    }
    Vertex v{};
    v.pos[0] = (x / kWidth * 2 - 1) * w;
    v.pos[1] = (1 - y / kWidth * 2) * w;
    v.pos[2] = z * w;
    v.pos[3] = w;
    for (auto &d : v.d)
      d = next() % 2 ? front() : behind();
    v.clip[0] = 1;
    v.clip[1] = 1;
    item.vertices.push_back(v);
  }
  // the primitive's own vertices
  std::vector<size_t> own;
  switch (item.shape) {
  case List:
  case Indexed:
    own = {0, 1, 2};
    break;
  case Strip:
    own = {0, 1, 2, 3};
    break;
  case Adjacency:
    own = {0, 2, 4};
    break;
  }
  for (unsigned j = 0; j < distances; j++) {
    switch (pattern) {
    case 0: // behind in the first
      if (j == 0)
        for (auto i : own)
          item.vertices[i].d[j] = behind();
      break;
    case 1: // one vertex in front of each: kept, its pixels on the far side of the others' edge too
    case 4: // and behind in the last
      for (auto i : own)
        item.vertices[i].d[j] = behind();
      item.vertices[own[next() % own.size()]].d[j] = front();
      if (pattern == 4 && j == distances - 1)
        for (auto i : own)
          item.vertices[i].d[j] = behind();
      break;
    case 2: // in front of all
      for (auto i : own)
        item.vertices[i].d[j] = front();
      break;
    default:
      break;
    }
  }
  // with adjacency the adjacent vertices are in front where the primitive's are behind
  if (item.shape == Adjacency)
    for (size_t i : {1, 3, 5})
      for (auto &d : item.vertices[i].d)
        d = front();
  // behind the near plane, the vertex in front of every distance alone: what of the primitive is drawn is clipped of it
  if (geometry == 5 && item.shape != Strip) {
    auto &v = item.vertices[own[0]];
    v.pos[2] = -0.5f * v.pos[3];
    for (unsigned j = 0; j < distances; j++) {
      for (auto i : own)
        item.vertices[i].d[j] = behind();
      v.d[j] = front();
    }
  }
  // clip distances: half of the cells clip some of the primitive away
  if (k % 2)
    for (auto &v : item.vertices) {
      v.clip[0] = unit() * 2 - 0.6f;
      v.clip[1] = unit() * 2 - 0.6f;
    }
  return item;
}

struct Pipeline {
  const char *name;
  int layout;
  unsigned distances;
  enum { Color, Depth, Count, Early } kind;
  UINT samples;
};

} // namespace

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const Pipeline pipelines[] = {
      {"one distance", 1, 1, Pipeline::Color, 1},
      {"eight distances", 2, 8, Pipeline::Color, 1},
      {"three distances beside a clip distance", 3, 3, Pipeline::Color, 1},
      {"two distances after two clip distances", 4, 2, Pipeline::Color, 1},
      {"eight distances, 4x multisampled", 2, 8, Pipeline::Color, 4},
      {"eight distances, depth alone", 2, 8, Pipeline::Depth, 1},
      {"two distances after two clip distances, depth alone", 4, 2, Pipeline::Depth, 1},
      {"eight distances, runs counted", 2, 8, Pipeline::Count, 1},
      {"two distances after two clip distances, runs counted", 4, 2, Pipeline::Count, 1},
      {"three distances beside a clip distance, early depth test", 3, 3, Pipeline::Early, 1},
  };

  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER uav{D3D12_ROOT_PARAMETER_TYPE_UAV};
  uav.Descriptor = {1, 0};
  uav.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  auto rs = root_signature(
      device.Get(), {1, &uav, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT}
  );
  if (!rs) {
    printf("failed: no root signature\n");
    return 1;
  }
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  // the targets, one set for the case and one for its reference, used by every pipeline in turn
  const UINT64 color_row = kWidth * 4, depth_row = kWidth * 4, count_bytes = kWidth * kWidth * 4;
  ComPtr<ID3D12Resource> readback = buffer(
      device.Get(), D3D12_HEAP_TYPE_READBACK, 2 * (color_row + depth_row) * kWidth + 2 * count_bytes,
      D3D12_RESOURCE_STATE_COPY_DEST
  );
  ComPtr<ID3D12Resource> zeros = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, count_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  {
    void *bytes;
    CHECK(zeros->Map(0, nullptr, &bytes));
    memset(bytes, 0, count_bytes);
    zeros->Unmap(0, nullptr);
  }
  ComPtr<ID3D12Resource> counts[2];
  for (auto &c : counts)
    c = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, count_bytes, D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  ComPtr<ID3D12DescriptorHeap> rtv_heap, dsv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2}, dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)));
  CHECK(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsv_heap)));
  auto rtv_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  auto dsv_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

  unsigned failures = 0, culled_total = 0, kept_total = 0;
  for (auto &p : pipelines) {
    // the items and what of them the reference draws
    seed = 12345 + p.layout * 7 + p.kind * 131 + p.samples;
    std::vector<Item> items;
    std::vector<Vertex> vertices, reference;
    std::vector<std::pair<UINT, UINT>> ranges, reference_ranges; // first vertex and count, by item
    for (unsigned k = 0; k < kCells; k++) {
      items.push_back(make_item(k, p.distances));
      ranges.push_back({UINT(vertices.size()), UINT(items.back().vertices.size())});
      vertices.insert(vertices.end(), items.back().vertices.begin(), items.back().vertices.end());
      UINT first = UINT(reference.size());
      for (auto &t : triangles(items.back())) {
        bool gone = culled(t, p.distances);
        (gone ? culled_total : kept_total)++;
        if (!gone)
          reference.insert(reference.end(), t.begin(), t.end());
      }
      reference_ranges.push_back({first, UINT(reference.size()) - first});
    }
    auto upload = [&](const std::vector<Vertex> &data) {
      auto b = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, std::max<size_t>(data.size(), 1) * sizeof(Vertex),
                      D3D12_RESOURCE_STATE_GENERIC_READ);
      void *bytes;
      if (b && SUCCEEDED(b->Map(0, nullptr, &bytes))) {
        memcpy(bytes, data.data(), data.size() * sizeof(Vertex));
        b->Unmap(0, nullptr);
      }
      return b;
    };
    auto test_vb = upload(vertices), reference_vb = upload(reference);
    const uint16_t index_data[] = {2, 1, 0};
    auto ib = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(index_data), D3D12_RESOURCE_STATE_GENERIC_READ);
    {
      void *bytes;
      CHECK(ib->Map(0, nullptr, &bytes));
      memcpy(bytes, index_data, sizeof(index_data));
      ib->Unmap(0, nullptr);
    }

    // the pipelines: with cull distances, and the reference's without
    bool color = p.kind != Pipeline::Depth, depth = p.kind == Pipeline::Depth || p.kind == Pipeline::Early;
    const char *ps_entry = p.kind == Pipeline::Count ? "ps_count" : p.kind == Pipeline::Early ? "ps_early" : "ps_color";
    ComPtr<ID3D12PipelineState> pso[2];
    for (int cull = 0; cull < 2; cull++) {
      std::vector<std::string> defines = {"LAYOUT=" + std::to_string(p.layout), "CULL=" + std::to_string(1 - cull),
                                          "WIDTH=" + std::to_string(kWidth)};
      auto vs = compiler.compile(hlsl, "vs", "vs", defines);
      auto ps = color ? compiler.compile(hlsl, ps_entry, "ps", defines) : std::string();
      if (vs.empty() || (color && ps.empty())) {
        printf("failed: %s does not compile\n", p.name);
        return 1;
      }
      D3D12_INPUT_ELEMENT_DESC elements[] = {
          {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0},
          {"D", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16},
          {"D", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32},
          {"CLIP", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 48},
      };
      D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
      desc.pRootSignature = rs.Get();
      desc.VS = bytecode(vs);
      if (color)
        desc.PS = bytecode(ps);
      desc.InputLayout = {elements, (UINT)std::size(elements)};
      desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      desc.SampleMask = ~0u;
      desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
      desc.RasterizerState.DepthClipEnable = TRUE;
      desc.RasterizerState.MultisampleEnable = p.samples > 1;
      desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      if (color) {
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
      }
      if (depth) {
        desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        desc.DepthStencilState.DepthEnable = TRUE;
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        // the early test passes nearer depths only: one a culled primitive left would hide the reference's
        desc.DepthStencilState.DepthFunc =
            p.kind == Pipeline::Early ? D3D12_COMPARISON_FUNC_LESS : D3D12_COMPARISON_FUNC_ALWAYS;
      }
      desc.SampleDesc.Count = p.samples;
      if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso[cull])))) {
        printf("failed: no pipeline for %s\n", p.name);
        return 1;
      }
    }

    // the targets
    ComPtr<ID3D12Resource> targets[2], resolved[2], depths[2];
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_CLEAR_VALUE clear_color{DXGI_FORMAT_R8G8B8A8_UNORM, {0, 0, 0, 0}}, clear_depth{DXGI_FORMAT_D32_FLOAT};
    clear_depth.DepthStencil.Depth = 1;
    for (int i = 0; i < 2; i++) {
      D3D12_RESOURCE_DESC t{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, kWidth, kWidth, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
                            {p.samples, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
      CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &t, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                            &clear_color, IID_PPV_ARGS(&targets[i])));
      if (p.samples > 1) {
        t.SampleDesc = {1, 0};
        t.Flags = D3D12_RESOURCE_FLAG_NONE;
        CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &t, D3D12_RESOURCE_STATE_RESOLVE_DEST,
                                              nullptr, IID_PPV_ARGS(&resolved[i])));
      }
      D3D12_RESOURCE_DESC d{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, kWidth, kWidth, 1, 1, DXGI_FORMAT_D32_FLOAT,
                            {p.samples, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL};
      CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                            &clear_depth, IID_PPV_ARGS(&depths[i])));
      device->CreateRenderTargetView(targets[i].Get(), nullptr,
                                     {rtv_heap->GetCPUDescriptorHandleForHeapStart().ptr + i * rtv_size});
      device->CreateDepthStencilView(depths[i].Get(), nullptr,
                                     {dsv_heap->GetCPUDescriptorHandleForHeapStart().ptr + i * dsv_size});
    }

    CHECK(forget(readback.Get()));
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), nullptr));
    for (int i = 0; i < 2; i++) {
      list->CopyBufferRegion(counts[i].Get(), 0, zeros.Get(), 0, count_bytes);
      transition(list.Get(), counts[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    for (int i = 0; i < 2; i++) {
      D3D12_CPU_DESCRIPTOR_HANDLE rtv{rtv_heap->GetCPUDescriptorHandleForHeapStart().ptr + i * rtv_size},
          dsv{dsv_heap->GetCPUDescriptorHandleForHeapStart().ptr + i * dsv_size};
      list->ClearRenderTargetView(rtv, clear_color.Color, 0, nullptr);
      list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
      list->OMSetRenderTargets(color ? 1 : 0, color ? &rtv : nullptr, FALSE, depth ? &dsv : nullptr);
      list->SetGraphicsRootSignature(rs.Get());
      list->SetGraphicsRootUnorderedAccessView(0, counts[i]->GetGPUVirtualAddress());
      list->SetPipelineState(pso[i].Get());
      D3D12_VIEWPORT viewport{0, 0, (float)kWidth, (float)kWidth, 0, 1};
      list->RSSetViewports(1, &viewport);
      auto &vb = i ? reference_vb : test_vb;
      D3D12_VERTEX_BUFFER_VIEW vbv{vb->GetGPUVirtualAddress(), UINT(vb->GetDesc().Width), sizeof(Vertex)};
      list->IASetVertexBuffers(0, 1, &vbv);
      D3D12_INDEX_BUFFER_VIEW ibv{ib->GetGPUVirtualAddress(), sizeof(index_data), DXGI_FORMAT_R16_UINT};
      list->IASetIndexBuffer(&ibv);
      for (unsigned k = 0; k < kCells; k++) {
        LONG x = LONG(k % (kWidth / kCell) * kCell), y = LONG(k / (kWidth / kCell) * kCell);
        D3D12_RECT scissor{x, y, x + LONG(kCell), y + LONG(kCell)};
        list->RSSetScissorRects(1, &scissor);
        if (i) {
          // the reference: the triangles kept, as a list
          if (!reference_ranges[k].second)
            continue;
          list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          list->DrawInstanced(reference_ranges[k].second, 1, reference_ranges[k].first, 0);
          continue;
        }
        auto [first, count] = ranges[k];
        switch (items[k].shape) {
        case List:
          list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          list->DrawInstanced(count, 1, first, 0);
          break;
        case Indexed:
          list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          list->DrawIndexedInstanced(3, 1, 0, INT(first), 0);
          break;
        case Strip:
          list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
          list->DrawInstanced(count, 1, first, 0);
          break;
        case Adjacency:
          list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ);
          list->DrawInstanced(count, 1, first, 0);
          break;
        }
      }
    }
    // the results, the case's then the reference's: color, depth and the runs
    UINT64 at = 0;
    UINT64 color_at[2], depth_at[2], count_at[2];
    for (int i = 0; i < 2; i++) {
      ID3D12Resource *source = targets[i].Get();
      if (p.samples > 1) {
        transition(list.Get(), targets[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        list->ResolveSubresource(resolved[i].Get(), 0, targets[i].Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
        transition(list.Get(), resolved[i].Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        source = resolved[i].Get();
      } else {
        transition(list.Get(), targets[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      }
      D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
          src{source, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      dst.PlacedFootprint = {at, {DXGI_FORMAT_R8G8B8A8_UNORM, kWidth, kWidth, 1, UINT(color_row)}};
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      color_at[i] = at;
      at += color_row * kWidth;
      // a multisampled depth target is compared by its color alone
      if (p.samples == 1) {
        transition(list.Get(), depths[i].Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION ddst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
            dsrc{depths[i].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        ddst.PlacedFootprint = {at, {DXGI_FORMAT_R32_TYPELESS, kWidth, kWidth, 1, UINT(depth_row)}};
        list->CopyTextureRegion(&ddst, 0, 0, 0, &dsrc, nullptr);
      }
      depth_at[i] = at;
      at += depth_row * kWidth;
      transition(list.Get(), counts[i].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyBufferRegion(readback.Get(), at, counts[i].Get(), 0, count_bytes);
      count_at[i] = at;
      at += count_bytes;
    }
    HRESULT hr = submit(device.Get(), queue.Get(), list.Get());
    if (FAILED(hr)) {
      printf("failed: %s: the list did not run (%#lx)\n", p.name, (unsigned long)hr);
      return 1;
    }
    for (auto &c : counts) {
      // back to where the next pipeline's list starts them
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), nullptr));
      transition(list.Get(), c.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
    }
    uint8_t *bytes;
    CHECK(readback->Map(0, nullptr, (void **)&bytes));
    unsigned wrong = 0, drawn = 0;
    for (UINT y = 0; y < kWidth; y++)
      for (UINT x = 0; x < kWidth; x++) {
        UINT k = y / kCell * (kWidth / kCell) + x / kCell;
        auto word = [&](UINT64 base, UINT64 row) {
          uint32_t v;
          memcpy(&v, bytes + base + y * row + x * 4, 4);
          return v;
        };
        uint32_t got[3] = {word(color_at[0], color_row), p.samples == 1 ? word(depth_at[0], depth_row) : 0,
                           word(count_at[0], kWidth * 4)},
                 want[3] = {word(color_at[1], color_row), p.samples == 1 ? word(depth_at[1], depth_row) : 0,
                            word(count_at[1], kWidth * 4)};
        drawn += want[0] != 0 || (depth && want[1] != 0x3f800000u);
        if (got[0] == want[0] && got[1] == want[1] && got[2] == want[2])
          continue;
        if (wrong++ < 8) {
          const char *shapes[] = {"list", "strip", "adjacency", "indexed"};
          printf("%s: pixel %u,%u (cell %u, %s): color %08x depth %08x runs %u, want %08x %08x %u\n", p.name, x, y, k,
                 shapes[items[k].shape], got[0], got[1], got[2], want[0], want[1], want[2]);
        }
      }
    readback->Unmap(0, nullptr);
    // the reference has to draw for the comparison to tell anything
    if (drawn < kWidth * kWidth / 50) {
      printf("failed: %s: the reference drew %u pixels\n", p.name, drawn);
      return 1;
    }
    failures += wrong;
  }
  if (failures) {
    printf("failed: %u wrong pixels\n", failures);
    return 1;
  }
  printf("passed: %zu pipelines, %u triangles culled and %u kept\n", std::size(pipelines), culled_total, kept_total);
  return 0;
}
