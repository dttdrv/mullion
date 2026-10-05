// contract: what a draw leaves alone, and what it reads where nothing is bound.
// - the planes a depth stencil view holds read only (D3D12_DSV_FLAGS: "depth values are read only", "stencil values
//   are read only") keep their values under a pipeline that writes both; the other plane is written, and the depth
//   test still reads.
// - a draw with a count of 0 draws nothing (D3D11.3 8.4.1 and 8.6.1: its loops over instances and vertices do not
//   run), and the draw after it draws.
// - an indexed draw without an index buffer reads every index as 0 (D3D11.3 8.20), so each of its vertices is the one
//   at BaseVertexLocation.
// the target is one row: a column per view, then a band of columns per draw case, whose vertex n is in column n.
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
cbuffer C : register(b0) { float depth; uint value; };
float4 cover(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), depth, 1);
}
// the middle of the column's pixel
float4 point_at(uint column : COLUMN) : SV_Position { return float4((column + 0.5) * 2 / WIDTH - 1, 0, 0, 1); }
uint ps() : SV_Target { return value; }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));

  // every combination of the planes a view can hold read only
  const UINT read_only = D3D12_DSV_FLAG_READ_ONLY_DEPTH | D3D12_DSV_FLAG_READ_ONLY_STENCIL, views = read_only + 1;
  const float cleared = 0.5f, drawn = 0.25f, behind = 0.75f;
  const UINT8 ref = 0x5a;
  const UINT band = 4, hidden = ~0u;
  struct Case {
    const char *name;
    UINT count, instances;
    bool indexed, bound;
    UINT first, base; // the first index, and the vertex index 0 is, in the band
  } cases[] = {
      {"no vertices", 0, 1},
      {"no instances", band, 0},
      {"no indices", 0, 1, true, true},
      {"no instances of indices", band, 0, true, true},
      {"no index buffer", band, 1, true, false, 1, 2},
      {"no index buffer, instances", band, 2, true, false, 0, 1},
  };
  const UINT width = views + band * std::size(cases);

  const std::vector<std::string> defines = {"WIDTH=" + std::to_string(width)};
  auto cover = compiler.compile(hlsl, "cover", "vs", defines), ps = compiler.compile(hlsl, "ps", "ps", defines);
  auto point_at = compiler.compile(hlsl, "point_at", "vs", defines);
  if (cover.empty() || point_at.empty() || ps.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  D3D12_ROOT_PARAMETER param{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
  param.Constants.Num32BitValues = 2;
  auto rs = root_signature(
      device.Get(), {1, &param, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT}
  );
  const DXGI_FORMAT depth_format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = rs.Get();
  desc.VS = bytecode(cover), desc.PS = bytecode(ps);
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.RasterizerState.DepthClipEnable = TRUE;
  // writes both planes where the depth test passes
  desc.DepthStencilState = {TRUE, D3D12_DEPTH_WRITE_MASK_ALL, D3D12_COMPARISON_FUNC_LESS, TRUE,
                            D3D12_DEFAULT_STENCIL_READ_MASK, D3D12_DEFAULT_STENCIL_WRITE_MASK};
  desc.DepthStencilState.FrontFace = desc.DepthStencilState.BackFace = {
      D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_REPLACE, D3D12_COMPARISON_FUNC_ALWAYS};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1, desc.RTVFormats[0] = DXGI_FORMAT_R32_UINT, desc.DSVFormat = depth_format;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> covers, points;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&covers)));
  D3D12_INPUT_ELEMENT_DESC element{"COLUMN", 0, DXGI_FORMAT_R32_UINT};
  desc.VS = bytecode(point_at), desc.InputLayout = {&element, 1};
  desc.DepthStencilState.DepthEnable = desc.DepthStencilState.StencilEnable = FALSE;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT, desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&points)));

  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, 1, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  auto depth_desc = target_desc;
  depth_desc.Format = depth_format, depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  ComPtr<ID3D12Resource> target, depth;
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&depth)
  ));
  ComPtr<ID3D12DescriptorHeap> rtvs, dsvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1}, dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, views};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  CHECK(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  // view f holds the planes of flags f read only
  auto dsv = [&](UINT f) {
    auto handle = dsvs->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += f * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    return handle;
  };
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  for (UINT f = 0; f < views; f++) {
    D3D12_DEPTH_STENCIL_VIEW_DESC view{depth_format, D3D12_DSV_DIMENSION_TEXTURE2D, D3D12_DSV_FLAGS(f)};
    device->CreateDepthStencilView(depth.Get(), &view, dsv(f));
  }

  // vertex n of the bands is in their column n; the indices of a band are its vertices in order
  const UINT vertices = band * std::size(cases);
  auto vertex_buffer = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, vertices * sizeof(UINT), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto index_buffer = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, band * sizeof(UINT16), D3D12_RESOURCE_STATE_GENERIC_READ);
  UINT *columns;
  UINT16 *indices;
  CHECK(vertex_buffer->Map(0, nullptr, (void **)&columns));
  CHECK(index_buffer->Map(0, nullptr, (void **)&indices));
  for (UINT v = 0; v < vertices; v++)
    columns[v] = views + v;
  for (UINT i = 0; i < band; i++)
    indices[i] = i;
  D3D12_VERTEX_BUFFER_VIEW vbv{vertex_buffer->GetGPUVirtualAddress(), UINT(vertices * sizeof(UINT)), sizeof(UINT)};
  D3D12_INDEX_BUFFER_VIEW ibv{index_buffer->GetGPUVirtualAddress(), UINT(band * sizeof(UINT16)), DXGI_FORMAT_R16_UINT};

  // read back: the target's row, the depth buffer's two planes, then the occlusion counts
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT row, planes[2];
  UINT64 size;
  const UINT64 placement = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &row, nullptr, nullptr, &size);
  device->GetCopyableFootprints(&depth_desc, 0, 2, (size + placement - 1) / placement * placement, planes, nullptr, nullptr, nullptr);
  const UINT64 counts_at = (planes[1].Offset + planes[1].Footprint.RowPitch + 7) / 8 * 8;
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, counts_at + std::size(cases) * 8, D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12QueryHeap> queries;
  D3D12_QUERY_HEAP_DESC query_desc{D3D12_QUERY_HEAP_TYPE_OCCLUSION, (UINT)std::size(cases)};
  CHECK(device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&queries)));

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), covers.Get(), IID_PPV_ARGS(&list)));
  const float zero[4] = {};
  list->ClearRenderTargetView(rtv, zero, 0, nullptr);
  list->ClearDepthStencilView(dsv(0), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, cleared, 0, 0, nullptr);
  list->SetGraphicsRootSignature(rs.Get());
  D3D12_VIEWPORT viewport{0, 0, (float)width, 1, 0, 1};
  list->RSSetViewports(1, &viewport);
  auto scissor = [&](UINT first, UINT count) {
    D3D12_RECT rect{(LONG)first, 0, LONG(first + count), 1};
    list->RSSetScissorRects(1, &rect);
  };
  auto constants = [&](float z, UINT value) {
    const UINT words[2] = {*reinterpret_cast<const UINT *>(&z), value};
    list->SetGraphicsRoot32BitConstants(0, 2, words, 0);
  };
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list->OMSetStencilRef(ref);
  // through each view: a draw in front of what is stored, then one behind it
  for (UINT f = 0; f < views; f++) {
    auto view = dsv(f);
    list->OMSetRenderTargets(1, &rtv, FALSE, &view);
    scissor(f, 1);
    constants(drawn, f + 1), list->DrawInstanced(3, 1, 0, 0);
    constants(behind, hidden), list->DrawInstanced(3, 1, 0, 0);
  }

  list->SetPipelineState(points.Get());
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
  list->IASetVertexBuffers(0, 1, &vbv);
  scissor(0, width);
  // each case's draw, counted, then its band's last vertex
  for (UINT c = 0; c < std::size(cases); c++) {
    auto &k = cases[c];
    constants(0, views + c + 1);
    list->IASetIndexBuffer(k.bound ? &ibv : nullptr);
    list->BeginQuery(queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, c);
    if (k.indexed)
      list->DrawIndexedInstanced(k.count, k.instances, k.first, c * band + k.base, 0);
    else
      list->DrawInstanced(k.count, k.instances, c * band, 0);
    list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, c);
    list->DrawInstanced(1, 1, c * band + band - 1, 0);
  }
  list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0, std::size(cases), readback.Get(), counts_at);
  transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(list.Get(), depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  D3D12_TEXTURE_COPY_LOCATION src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  dst.PlacedFootprint = row;
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  src.pResource = depth.Get();
  for (UINT plane = 0; plane < std::size(planes); plane++) {
    dst.PlacedFootprint = planes[plane], src.SubresourceIndex = plane;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  char *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  auto colours = reinterpret_cast<const UINT *>(got + row.Offset);
  auto depths = reinterpret_cast<const float *>(got + planes[0].Offset);
  auto stencils = reinterpret_cast<const UINT8 *>(got + planes[1].Offset);
  unsigned failures = 0;
  for (UINT f = 0; f < views; f++) {
    float want_depth = f & D3D12_DSV_FLAG_READ_ONLY_DEPTH ? cleared : drawn;
    UINT want_stencil = f & D3D12_DSV_FLAG_READ_ONLY_STENCIL ? 0 : ref;
    if ((colours[f] != f + 1 || depths[f] != want_depth || stencils[f] != want_stencil) && failures++ < 12)
      printf("view of flags %u: colour %u, depth %g, stencil %u, want %u, %g, %u\n", f, colours[f], depths[f],
             stencils[f], f + 1, want_depth, want_stencil);
  }
  for (UINT c = 0; c < std::size(cases); c++) {
    auto &k = cases[c];
    // what the draw draws is one point per index and instance, all of them the vertex index 0 is
    UINT64 samples = k.count * k.instances, count = reinterpret_cast<const UINT64 *>(got + counts_at)[c];
    if (count != samples && failures++ < 12)
      printf("%s: %llu samples, want %llu\n", k.name, count, samples);
    for (UINT i = 0; i < band; i++) {
      UINT at = views + c * band + i, want = (samples && i == k.base) || i == band - 1 ? views + c + 1 : 0;
      if (colours[at] != want && failures++ < 12)
        printf("%s: column %u is %u, want %u\n", k.name, i, colours[at], want);
    }
  }
  readback->Unmap(0, nullptr);
  printf("%s: %u wrong values over %u views and %zu draws\n", failures ? "failed" : "passed", failures, views, std::size(cases));
  return failures != 0;
}
