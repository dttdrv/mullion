// contract: a geometry shader runs once per input primitive and instance, with the primitive's vertices in the input
// assembler's order, its SV_PrimitiveID and SV_GSInstanceID. a triangle geometry shader records what it gets for a
// triangle list, a strip (whose odd triangles keep the winding as (n, n + 2, n + 1)), indexed strips that the
// pipeline's IBStripCutValue cuts (that index and no other: a pipeline with cuts disabled draws index 0xFFFFFFFF, and
// 0xFFFF cuts 32-bit indices too), a long one with cuts side by side, far apart and at its ends, and an indexed list
// with a base vertex, some through ExecuteIndirect; a triangle geometry shader with adjacency records strips with adjacency, whose triangle n leads
// at index 2n of its strip and takes the vertices across its edges from two indices back, three ahead and six ahead,
// its strip's first and last triangles from the odd index beside them (D3D11.3 functional spec, 8.15). the
// expectations assemble the same primitives on the CPU, with VertexID counting from 0 in a draw, or the index, never
// the start or base vertex (8.16), and PrimitiveID counting on across cuts (8.17). a point geometry shader emits two
// triangles per point and instance with RestartStrip between them, each covering one pixel two rows apart, the second
// starting at its lower right corner, so a strip that ignored the cut would also cover the pixel between them; and
// another emits a point per point and instance, which draws as a square one pixel wide (3.4.6). two more emit a
// strip of triangles and one of lines whose middle vertices are behind a cull distance, negative or NaN: the
// primitives made of those alone are discarded, the others drawn whole (15.4.2).
#include "d3d12_test.hpp"
#include <algorithm>
#include <array>
#include <map>

static const char hlsl[] = R"hlsl(
cbuffer c : register(b0) { uint base; };
RWStructuredBuffer<uint4> seen : register(u0);
struct V { float4 p : SV_Position; uint id : ID; };
// vertex n sits at pixel (n % SIZE, n / SIZE)
V vs(uint id : SV_VertexID) {
  V v;
  v.id = id;
  v.p = float4(float2(id % SIZE, id / SIZE) / SIZE * float2(2, -2) + float2(-1, 1), 0, 1);
  return v;
}
[maxvertexcount(1)] [instance(INSTANCES)]
void gs_triangle(triangle V v[3], uint primitive : SV_PrimitiveID, uint instance : SV_GSInstanceID,
                 inout PointStream<V> s) {
  seen[base + primitive * INSTANCES + instance] = uint4(v[0].id, v[1].id, v[2].id, primitive);
}
// two records: the triangle, then the vertices across its edges
[maxvertexcount(1)]
void gs_adjacency(triangleadj V v[6], uint primitive : SV_PrimitiveID, inout PointStream<V> s) {
  seen[base + primitive * 2] = uint4(v[0].id, v[2].id, v[4].id, primitive);
  seen[base + primitive * 2 + 1] = uint4(v[1].id, v[3].id, v[5].id, primitive);
}
struct P { float4 p : SV_Position; nointerpolation uint value : VALUE; };
// a triangle covering the center of pixel (x, y) and no other
P corner(float x, float y, uint value) {
  P o;
  o.p = float4(float2(x, y) / SIZE * float2(2, -2) + float2(-1, 1), 0, 1);
  o.value = value;
  return o;
}
[maxvertexcount(6)] [instance(INSTANCES)]
void gs_point(point V v[1], uint primitive : SV_PrimitiveID, uint instance : SV_GSInstanceID,
              inout TriangleStream<P> s) {
  float x = v[0].id * 2, y = instance * 4;
  uint value = (primitive + 1) | (instance + 1) << 8;
  s.Append(corner(x, y, value));
  s.Append(corner(x + 1.4, y, value));
  s.Append(corner(x, y + 1.4, value));
  s.RestartStrip();
  s.Append(corner(x + 1.4, y + 2, value | 1 << 16));
  s.Append(corner(x, y + 2, value | 1 << 16));
  s.Append(corner(x, y + 3.4, value | 1 << 16));
}
// a point per point and instance, on a pixel the triangles leave alone
[maxvertexcount(1)] [instance(INSTANCES)]
void gs_dot(point V v[1], uint primitive : SV_PrimitiveID, uint instance : SV_GSInstanceID, inout PointStream<P> s) {
  s.Append(corner(v[0].id * 2 + 1.5, instance * 4 + 1.5, (primitive + 1) | (instance + 1) << 8 | 2 << 16));
}
// strips of CULL_VERTICES vertices from the first point, the middle ones behind the first cull distance
struct C { float4 p : SV_Position; nointerpolation uint value : VALUE; float2 cull : SV_CullDistance; };
C behind(float x, float y, uint m) {
  C o;
  o.p = float4(float2(x, y) / SIZE * float2(2, -2) + float2(-1, 1), 0, 1);
  o.value = (m + 1) | 3 << 16;
  bool out_ = m > 0 && m < CULL_VERTICES - 1;
  o.cull = float2(out_ ? (m % 2 ? -1.0 : asfloat(0x7fc00000)) : (float)(m % 3), 1);
  return o;
}
// triangle n has its tip over the center of pixel n + 1 of the row
[maxvertexcount(CULL_VERTICES)]
void gs_cull_triangles(point V v[1], inout TriangleStream<C> s) {
  for (uint m = 0; m < CULL_VERTICES; m++)
    s.Append(behind(m + 0.5, TRIANGLE_ROW + (m % 2 ? 0.9 : 0.1), m));
}
// line n runs from a quarter into pixel n of the row to a quarter into the next, and covers the first
[maxvertexcount(CULL_VERTICES)]
void gs_cull_lines(point V v[1], inout LineStream<C> s) {
  for (uint m = 0; m < CULL_VERTICES; m++)
    s.Append(behind(m + 0.25, LINE_ROW + 0.5, m));
}
uint ps(P p) : SV_Target { return p.value; }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  // rows the other shaders leave alone
  const UINT size = 8, instances = 2, points = 3, cull_vertices = 5, triangle_row = 7, line_row = 3;
  std::vector<std::string> defines = {
      "SIZE=" + std::to_string(size), "INSTANCES=" + std::to_string(instances),
      "CULL_VERTICES=" + std::to_string(cull_vertices), "TRIANGLE_ROW=" + std::to_string(triangle_row),
      "LINE_ROW=" + std::to_string(line_row),
  };
  auto vs = compiler.compile(hlsl, "vs", "vs", defines), gs_triangle = compiler.compile(hlsl, "gs_triangle", "gs", defines),
       gs_adjacency = compiler.compile(hlsl, "gs_adjacency", "gs", defines),
       gs_point = compiler.compile(hlsl, "gs_point", "gs", defines), gs_dot = compiler.compile(hlsl, "gs_dot", "gs", defines),
       gs_cull_triangles = compiler.compile(hlsl, "gs_cull_triangles", "gs", defines),
       gs_cull_lines = compiler.compile(hlsl, "gs_cull_lines", "gs", defines), ps = compiler.compile(hlsl, "ps", "ps", defines);
  if (vs.empty() || gs_triangle.empty() || gs_adjacency.empty() || gs_point.empty() || gs_dot.empty() ||
      gs_cull_triangles.empty() || gs_cull_lines.empty() || ps.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[0].Constants.Num32BitValues = 1;
  auto rs = root_signature(device.Get(), {2, params});

  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs)};
  desc.GS = bytecode(gs_triangle);
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.SampleDesc = {1, 0};
  // a triangle pipeline per IBStripCutValue (disabled, 0xFFFF, 0xFFFFFFFF), and one with adjacency
  const uint32_t no_cut = 0, cut = 0xffff, wide_cut = ~0u, cuts[] = {no_cut, cut, wide_cut};
  ComPtr<ID3D12PipelineState> triangle_psos[std::size(cuts)], adjacency_pso, point_pso, dot_pso, cull_psos[2];
  for (size_t i = 0; i < std::size(cuts); i++) {
    desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE(i);
    CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&triangle_psos[i])));
  }
  desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
  desc.GS = bytecode(gs_adjacency);
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&adjacency_pso)));
  auto &triangle_pso = triangle_psos[1];
  desc.GS = bytecode(gs_point);
  desc.PS = bytecode(ps);
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&point_pso)));
  desc.GS = bytecode(gs_dot);
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&dot_pso)));
  desc.GS = bytecode(gs_cull_triangles);
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&cull_psos[0])));
  desc.GS = bytecode(gs_cull_lines);
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&cull_psos[1])));

  // the draws: topology, indices and whether they are 32-bit, first vertex, vertices or indices, base vertex, and the
  // index that cuts strips
  struct Draw {
    D3D12_PRIMITIVE_TOPOLOGY topology;
    std::vector<uint32_t> indices;
    bool wide;
    UINT first, count;
    INT base_vertex;
    uint32_t cut;
    bool indirect = false;
  };
  // a long strip: each run of vertices (its own positions) ends in a cut, runs too short for a triangle, of odd and
  // even lengths, and longer than the indices one object threadgroup starts
  auto runs_of = [&](std::initializer_list<uint32_t> lengths) {
    std::vector<uint32_t> strip;
    for (uint32_t run : lengths)
      for (uint32_t i = 0; i <= run; i++)
        strip.push_back(i < run ? strip.size() : cut);
    return strip;
  };
  auto long_strip = runs_of({1, 0, 2, 3, 28, 20, 0, 29, 31, 89, 57, 4});
  auto adjacency_strip = runs_of({6, 7, 5, 12, 31, 8, 0, 9, 46});
  const auto strip = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, adjacency = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ;
  const std::vector<Draw> draws = {
      {D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, {}, false, 0, 6, 0, cut},
      {strip, {}, false, 10, 5, 0, cut},
      {strip, {20, 21, 22, 23, cut, 30, 31, 32}, false, 0, 8, 0, cut},
      {strip, long_strip, false, 0, (UINT)long_strip.size(), 0, cut},
      {D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, {40, 41, 42}, false, 0, 3, 5, cut},
      // 32-bit indices: no cut, the 16-bit cut, and their own, which the 16-bit one is then not
      {strip, {0, 1, wide_cut, 3, 4}, true, 0, 5, 0, no_cut},
      {strip, {0, 1, 2, cut, 4, 5, 6, wide_cut, 8}, true, 0, 9, 0, cut},
      {strip, {0, 1, 2, cut, 4, 5, 6, wide_cut, 8, 9, 10}, true, 0, 11, 0, wide_cut},
      {adjacency, {}, false, 3, 47, 0, cut},
      {adjacency, adjacency_strip, false, 0, (UINT)adjacency_strip.size(), 0, cut},
      {strip, long_strip, false, 0, (UINT)long_strip.size(), 0, cut, true},
      {adjacency, {}, false, 3, 47, 0, cut, true},
      {D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, {40, 41, 42}, false, 0, 3, 5, cut, true},
  };
  // what each draw's primitives are: lists in threes, strips from each run of indices between cuts, the primitive ID
  // counting on across cuts. a strip's odd triangles have their last two vertices swapped; with adjacency a strip's
  // triangle n is at 2n, 2n + 2 and 2n + 4, and each record's instances are one
  std::vector<std::array<uint32_t, 4>> want;
  std::vector<UINT> bases;
  for (auto &d : draws) {
    bases.push_back(want.size());
    std::vector<std::vector<uint32_t>> runs(1);
    for (UINT i = 0; i < d.count; i++) {
      uint32_t v = d.indices.empty() ? i : d.indices[i];
      if (!d.indices.empty() && d.cut != no_cut && v == d.cut)
        runs.emplace_back();
      else
        runs.back().push_back(v);
    }
    uint32_t primitive = 0;
    for (auto &run : runs) {
      if (d.topology == adjacency) {
        for (size_t n = 0; 2 * n + 5 < run.size(); n++, primitive++) {
          size_t at = 2 * n;
          // the last triangle is the one no whole triangle follows: a vertex left dangling after it belongs to none
          // (D3D11.3 13.10, "GS Invocations From INCOMPLETE TriStrip w/Adjacency")
          bool odd = n & 1, first = n == 0, last = at + 7 >= run.size();
          uint32_t back = run[first ? at + 1 : at - 2], side = run[at + 3], ahead = run[last ? at + 5 : at + 6];
          want.push_back({run[at], run[at + (odd ? 4 : 2)], run[at + (odd ? 2 : 4)], primitive});
          want.push_back({odd ? side : back, ahead, odd ? back : side, primitive});
        }
        continue;
      }
      for (size_t i = 0; i + 2 < run.size(); i += d.topology == strip ? 1 : 3) {
        bool odd = d.topology == strip && (i & 1);
        for (UINT n = 0; n < instances; n++)
          want.push_back({run[i], run[i + 1 + odd], run[i + 2 - odd], primitive});
        primitive++;
      }
    }
  }

  // the index data: 16-bit or 32-bit, each draw's from a 4-byte boundary
  std::vector<uint8_t> all_indices;
  std::vector<UINT> index_starts;
  for (auto &d : draws) {
    index_starts.push_back(all_indices.size());
    for (uint32_t index : d.indices)
      all_indices.insert(all_indices.end(), (uint8_t *)&index, (uint8_t *)&index + (d.wide ? 4 : 2));
    all_indices.resize((all_indices.size() + 3) & ~size_t(3));
  }
  const UINT64 seen_bytes = want.size() * 16, index_bytes = all_indices.size(), row = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
  auto seen = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, seen_bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto indices = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, index_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  void *mapped;
  CHECK(indices->Map(0, nullptr, &mapped));
  memcpy(mapped, all_indices.data(), index_bytes);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, row * size + seen_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  // an indirect draw's arguments, one command each
  ComPtr<ID3D12CommandSignature> draw_signature, indexed_signature;
  D3D12_INDIRECT_ARGUMENT_DESC draw_argument{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW},
      indexed_argument{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED};
  D3D12_COMMAND_SIGNATURE_DESC draw_signature_desc{sizeof(D3D12_DRAW_ARGUMENTS), 1, &draw_argument},
      indexed_signature_desc{sizeof(D3D12_DRAW_INDEXED_ARGUMENTS), 1, &indexed_argument};
  CHECK(device->CreateCommandSignature(&draw_signature_desc, nullptr, IID_PPV_ARGS(&draw_signature)));
  CHECK(device->CreateCommandSignature(&indexed_signature_desc, nullptr, IID_PPV_ARGS(&indexed_signature)));
  const UINT64 argument_bytes = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
  auto arguments = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, argument_bytes * draws.size(), D3D12_RESOURCE_STATE_GENERIC_READ);
  uint8_t *mapped_arguments;
  CHECK(arguments->Map(0, nullptr, (void **)&mapped_arguments));

  ComPtr<ID3D12Resource> target;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), triangle_pso.Get(), IID_PPV_ARGS(&list)));
  const UINT untouched = ~0u;
  const float clear[4] = {(float)untouched, 0, 0, 0};
  list->ClearRenderTargetView(rtv, clear, 0, nullptr);
  list->SetGraphicsRootSignature(rs.Get());
  list->SetGraphicsRootUnorderedAccessView(1, seen->GetGPUVirtualAddress());
  D3D12_VIEWPORT viewport{0, 0, (float)size, (float)size, 0, 1};
  D3D12_RECT scissor{0, 0, (LONG)size, (LONG)size};
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  for (size_t i = 0; i < draws.size(); i++) {
    auto &d = draws[i];
    list->SetPipelineState(
        d.topology == adjacency ? adjacency_pso.Get() : triangle_psos[std::find(cuts, std::end(cuts), d.cut) - cuts].Get()
    );
    list->SetGraphicsRoot32BitConstants(0, 1, &bases[i], 0);
    list->IASetPrimitiveTopology(d.topology);
    if (d.indices.empty()) {
      D3D12_DRAW_ARGUMENTS args{d.count, 1, d.first, 0};
      memcpy(mapped_arguments + argument_bytes * i, &args, sizeof(args));
      if (d.indirect)
        list->ExecuteIndirect(draw_signature.Get(), 1, arguments.Get(), argument_bytes * i, nullptr, 0);
      else
        list->DrawInstanced(d.count, 1, d.first, 0);
      continue;
    }
    // 16-bit draws start within a view of the whole buffer, 32-bit ones at the start of their own view
    UINT view_start = d.wide ? index_starts[i] : 0;
    D3D12_INDEX_BUFFER_VIEW view{indices->GetGPUVirtualAddress() + view_start, UINT(index_bytes - view_start),
                                 d.wide ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT};
    list->IASetIndexBuffer(&view);
    D3D12_DRAW_INDEXED_ARGUMENTS args{d.count, 1, (index_starts[i] - view_start) / 2, d.base_vertex, 0};
    memcpy(mapped_arguments + argument_bytes * i, &args, sizeof(args));
    if (d.indirect)
      list->ExecuteIndirect(indexed_signature.Get(), 1, arguments.Get(), argument_bytes * i, nullptr, 0);
    else
      list->DrawIndexedInstanced(args.IndexCountPerInstance, 1, args.StartIndexLocation, args.BaseVertexLocation, 0);
  }
  list->SetPipelineState(point_pso.Get());
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
  list->DrawInstanced(points, 1, 0, 0);
  list->SetPipelineState(dot_pso.Get());
  list->DrawInstanced(points, 1, 0, 0);
  for (auto &pso : cull_psos) {
    list->SetPipelineState(pso.Get());
    list->DrawInstanced(1, 1, 0, 0);
  }
  transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(list.Get(), seen.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
      src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  dst.PlacedFootprint = {0, {DXGI_FORMAT_R32_UINT, size, size, 1, (UINT)row}};
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  list->CopyBufferRegion(readback.Get(), row * size, seen.Get(), 0, seen_bytes);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  uint8_t *out;
  CHECK(readback->Map(0, nullptr, (void **)&out));

  unsigned failures = 0;
  auto got = reinterpret_cast<const std::array<uint32_t, 4> *>(out + row * size);
  for (size_t i = 0; i < want.size(); i++)
    if (got[i] != want[i] && failures++ < 8)
      printf("triangle record %zu: %u %u %u primitive %u, want %u %u %u primitive %u\n", i, got[i][0], got[i][1],
             got[i][2], got[i][3], want[i][0], want[i][1], want[i][2], want[i][3]);
  // each point's two triangles and its dot, per instance, and nothing else
  std::map<std::pair<UINT, UINT>, UINT> pixels;
  for (UINT p = 0; p < points; p++)
    for (UINT n = 0; n < instances; n++) {
      for (UINT k = 0; k < 2; k++)
        pixels[{p * 2, n * 4 + k * 2}] = (p + 1) | (n + 1) << 8 | k << 16;
      pixels[{p * 2 + 1, n * 4 + 1}] = (p + 1) | (n + 1) << 8 | 2 << 16;
    }
  // the strips: a primitive is discarded when every vertex of it is a middle one, and shows its first vertex
  auto middle = [&](UINT m) { return m > 0 && m < cull_vertices - 1; };
  for (UINT n = 0; n + 1 < cull_vertices; n++) {
    if (n + 2 < cull_vertices && !(middle(n) && middle(n + 1) && middle(n + 2)))
      pixels[{n + 1, triangle_row}] = (n + 1) | 3 << 16;
    if (!(middle(n) && middle(n + 1)))
      pixels[{n, line_row}] = (n + 1) | 3 << 16;
  }
  for (UINT y = 0; y < size; y++)
    for (UINT x = 0; x < size; x++) {
      auto value = reinterpret_cast<const UINT *>(out + row * y)[x];
      auto it = pixels.find({x, y});
      auto expected = it == pixels.end() ? untouched : it->second;
      if (value != expected && failures++ < 8)
        printf("pixel %u,%u: %#x, want %#x\n", x, y, value, expected);
    }
  if (failures) {
    printf("failed: %u wrong records or pixels\n", failures);
    return 1;
  }
  printf("passed: %zu primitives, %u pixels\n", want.size(), size * size);
  return 0;
}
