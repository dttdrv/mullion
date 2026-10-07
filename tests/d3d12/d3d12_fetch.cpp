// contract: a draw that reads past what its vertex or index buffer view covers reads zero there, and nothing else
// happens. "Any calculated address that would fall out of bounds for a Buffer being accessed results in
// out-of-bounds behavior being invoked, where the return is 0 in all non-missing components of the format (defined
// in the Input Layout), and the default for missing components" (D3D11.3 8.19.2; 0, 0, 0, 1: 19.1.3.3). In
// Direct3D 12 the buffer is the view: D3D12_VERTEX_BUFFER_VIEW and D3D12_INDEX_BUFFER_VIEW give its address and
// SizeInBytes. A buffer whose tiles are not mapped reads zero from them on a device of tiled resources tier 2
// ("reads from NULL-mapped tiles return zero", D3D12_TILED_RESOURCES_TIER_2). engines draw so: with index counts
// rounded up past the view, with a stride for a wider vertex than the stream has, and from sparsely committed
// buffers.
// vertex i is a point on pixel i of a 15 by 15 target with the value i + 1. an element is read where its bytes lie
// inside the view and is zero where they do not: a vertex whose position is out of bounds lies at the target's
// centre (0, 0, 0, 1), where no vertex of the draw is, and one whose value is out of bounds has the value 0. an
// index out of bounds is 0, the first vertex. every pixel of the target is set against that: nothing else is
// drawn. each case is a draw of its own; CASE=n (argv[2]) runs one, since a GPU that faults takes the rest of
// the process's work with it.
#include "d3d12_test.hpp"
#include <functional>
#include <memory>
#include <tuple>

static const char hlsl[] = R"hlsl(
struct V { float4 pos : SV_Position; nointerpolation uint value : VALUE; };
V vs(float4 pos : POSITION, uint value : VALUE) { V v; v.pos = pos; v.value = value; return v; }
uint ps(V v) : SV_Target { return v.value; }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto vs = compiler.compile(hlsl, "vs", "vs"), ps = compiler.compile(hlsl, "ps", "ps");
  if (vs.empty() || ps.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  int only = -1;
  for (int i = 2; i < argc; i++)
    if (!strncmp(argv[i], "CASE=", 5))
      only = atoi(argv[i] + 5);
  struct Vertex {
    float x, y;
    UINT value;
  };
  const UINT side = 15, count = 32, clear = 0xc1ea, centre = side / 2 * side + side / 2, tile = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
  const DXGI_FORMAT format = DXGI_FORMAT_R32_UINT;
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
  auto rs = root_signature(device.Get(), {0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT});
  const D3D12_INPUT_ELEMENT_DESC elements[] = {{"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, x)},
                                               {"VALUE", 0, DXGI_FORMAT_R32_UINT, 0, offsetof(Vertex, value)}};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = rs.Get();
  desc.VS = bytecode(vs), desc.PS = bytecode(ps);
  desc.InputLayout = {elements, (UINT)std::size(elements)};
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
  desc.NumRenderTargets = 1, desc.RTVFormats[0] = format;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));

  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, side, side, 1, 1, format, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                        IID_PPV_ARGS(&target)));
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)));
  auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 bytes;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));

  // the vertices, and indices of them last to first, of 16 and of 32 bits, in an upload buffer
  std::vector<Vertex> vertices(count);
  std::vector<UINT16> narrow(count);
  std::vector<UINT> wide(count);
  for (UINT i = 0; i < count; i++) {
    vertices[i] = {(i % side + 0.5f) * 2 / side - 1, 1 - (i / side + 0.5f) * 2 / side, i + 1};
    narrow[i] = wide[i] = count - 1 - i;
  }
  const UINT vertex_bytes = count * sizeof(Vertex), narrow_bytes = count * sizeof(UINT16), wide_bytes = count * sizeof(UINT);
  enum { Vertices, Narrow = 1024, Wide = 2048, Staged = 4096 };
  auto staged = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, Staged, D3D12_RESOURCE_STATE_GENERIC_READ);
  char *start;
  CHECK(staged->Map(0, nullptr, (void **)&start));
  // what lies behind the data is not zero: an index read there names a vertex far outside, which lands at the centre
  memset(start, 0x55, Staged);
  memcpy(start + Vertices, vertices.data(), vertex_bytes);
  memcpy(start + Narrow, narrow.data(), narrow_bytes);
  memcpy(start + Wide, wide.data(), wide_bytes);
  const D3D12_GPU_VIRTUAL_ADDRESS base = staged->GetGPUVirtualAddress();

  // two buffers of two tiles each with only the first tile mapped: the first half of the vertices, and of the
  // indices, ends at the first tile's end, so what a view takes past it is in the tile that is not there
  ComPtr<ID3D12Resource> sparse_vertices, sparse_indices;
  ComPtr<ID3D12Heap> pages;
  const bool tiled = options.TiledResourcesTier >= D3D12_TILED_RESOURCES_TIER_2;
  const UINT half = count / 2;
  if (tiled) {
    D3D12_RESOURCE_DESC sparse_desc{D3D12_RESOURCE_DIMENSION_BUFFER, 0, 2 * tile, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
    D3D12_HEAP_DESC pages_desc{2 * tile, {D3D12_HEAP_TYPE_DEFAULT}, 0, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS};
    CHECK(device->CreateHeap(&pages_desc, IID_PPV_ARGS(&pages)));
    UINT page = 0;
    for (auto [made, from, size] : {std::tuple{std::addressof(sparse_vertices), UINT(Vertices), UINT(half * sizeof(Vertex))},
                                    std::tuple{std::addressof(sparse_indices), UINT(Wide), UINT(half * sizeof(UINT))}}) {
      CHECK(device->CreateReservedResource(&sparse_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(made->GetAddressOf())));
      const D3D12_TILED_RESOURCE_COORDINATE first{0}, second{1};
      const D3D12_TILE_REGION_SIZE one{1};
      const D3D12_TILE_RANGE_FLAGS none = D3D12_TILE_RANGE_FLAG_NULL;
      const UINT tiles = 1;
      queue->UpdateTileMappings(made->Get(), 1, &first, &one, pages.Get(), 1, nullptr, &page, &tiles, D3D12_TILE_MAPPING_FLAG_NONE);
      queue->UpdateTileMappings(made->Get(), 1, &second, &one, nullptr, 1, &none, nullptr, &tiles, D3D12_TILE_MAPPING_FLAG_NONE);
      list->CopyBufferRegion(made->Get(), tile - size, staged.Get(), from, size);
      transition(list.Get(), made->Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                 D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER);
      page++;
    }
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), nullptr));
  }
  CHECK(list->Close());

  // where in the index buffer a draw begins (StartIndexLocation); set for the cases that have one
  UINT first_index = 0;
  // what a draw adds to every index it reads (BaseVertexLocation: "interpreted, unaltered, as unsigned 32-bit numbers,
  // used in unsigned 32-bit addressing arithmetic", D3D11.3 8.19.1)
  INT base_vertex = 0;
  // a draw of `count` vertices through these views; what each pixel then holds follows from which bytes the views
  // cover: `covered(offset, size)` says whether a read of that many bytes at that offset of a view's start is inside
  auto draws = [&](const char *what, D3D12_VERTEX_BUFFER_VIEW vbv, const D3D12_INDEX_BUFFER_VIEW *ibv, UINT64 vertices_there,
                   UINT64 indices_there) {
    static int cases;
    const int number = cases++;
    if (only >= 0 && only != number)
      return;
    step("case %d: %s", number, what);
    std::vector<UINT> want(side * side, clear);
    auto inside = [](UINT64 offset, UINT64 size, UINT64 there) { return offset + size <= there; };
    // a slot with no buffer gives 0 in every component, the position's w too ("defaults are not applied to missing
    // channels for this case", D3D11.3 8.20): no vertex of the draw is anywhere, and nothing is drawn
    for (UINT i = 0; i < count && vbv.BufferLocation; i++) {
      // the vertex the draw's i-th element names: itself, or the index read there, 0 past the indices that are
      UINT index_size = ibv && ibv->Format == DXGI_FORMAT_R16_UINT ? sizeof(UINT16) : sizeof(UINT);
      UINT64 read = UINT64(first_index) + i;
      UINT64 v = !ibv ? i : UINT((inside(read * index_size, index_size, indices_there) ? count - 1 - read : 0) + base_vertex),
             at = v * vbv.StrideInBytes;
      bool placed = inside(at + offsetof(Vertex, x), 2 * sizeof(float), vertices_there),
           valued = inside(at + offsetof(Vertex, value), sizeof(UINT), vertices_there);
      // what lies at that address is the vertex of that place in the data, whatever the stride took the draw there by
      const UINT64 source = at / sizeof(Vertex);
      want[placed ? source : centre] = valued ? source + 1 : 0;
    }
    if (FAILED(forget(readback.Get())) || FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), pso.Get())))
      return (void)expect(false, "the list could not be begun");
    const float cleared[4] = {(float)clear};
    D3D12_VIEWPORT viewport{0, 0, (float)side, (float)side, 0, 1};
    D3D12_RECT scissor{0, 0, (LONG)side, (LONG)side};
    list->ClearRenderTargetView(rtv, cleared, 0, nullptr);
    list->SetGraphicsRootSignature(rs.Get());
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    list->IASetVertexBuffers(0, 1, &vbv);
    if (ibv) {
      list->IASetIndexBuffer(ibv);
      list->DrawIndexedInstanced(count, 1, first_index, base_vertex, 0);
    } else {
      list->DrawInstanced(count, 1, 0, 0);
    }
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
        to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    HRESULT ran = submit(device.Get(), queue.Get(), list.Get());
    const char *out;
    if (!expect(ran == S_OK, "the draw's list: %08lx (the device's reason: %08lx)", ran, device->GetDeviceRemovedReason()) ||
        FAILED(readback->Map(0, nullptr, (void **)&out)))
      return;
    unsigned wrong = 0;
    for (UINT pixel = 0; pixel < side * side; pixel++) {
      UINT got = ((const UINT *)(out + pixel / side * footprint.Footprint.RowPitch))[pixel % side];
      if (got != want[pixel] && wrong++ < 4)
        expect(false, "pixel %u,%u holds %#x, want %#x%s", pixel % side, pixel / side, got, want[pixel],
               pixel == centre ? " (the centre, where a vertex with no position lies)" : "");
    }
    expect(wrong <= 4, "and %u more pixels", wrong - 4);
    readback->Unmap(0, nullptr);
  };

  const UINT stride = sizeof(Vertex);
  auto vertex_view = [&](D3D12_GPU_VIRTUAL_ADDRESS at, UINT size, UINT step) { return D3D12_VERTEX_BUFFER_VIEW{at, size, step}; };
  const D3D12_VERTEX_BUFFER_VIEW whole = vertex_view(base + Vertices, vertex_bytes, stride);
  const D3D12_INDEX_BUFFER_VIEW narrow_whole{base + Narrow, narrow_bytes, DXGI_FORMAT_R16_UINT}, wide_whole{base + Wide, wide_bytes, DXGI_FORMAT_R32_UINT};
  draws("every vertex and every index inside its view", whole, &narrow_whole, vertex_bytes, narrow_bytes);
  for (UINT there : {count - 1, count / 2, 1u, 0u}) {
    std::string name = "a vertex view of " + std::to_string(there) + " of the draw's " + std::to_string(count) + " vertices";
    draws(name.c_str(), vertex_view(base + Vertices, there * stride, stride), nullptr, there * stride, 0);
    draws((name + ", indexed").c_str(), vertex_view(base + Vertices, there * stride, stride), &wide_whole, there * stride, wide_bytes);
  }
  draws("a vertex view that ends between a vertex's position and its value",
        vertex_view(base + Vertices, 4 * stride + offsetof(Vertex, value), stride), nullptr, 4 * stride + offsetof(Vertex, value), 0);
  draws("a stride of two vertices: every second vertex, and the view over before the draw is",
        vertex_view(base + Vertices, vertex_bytes, 2 * stride), nullptr, vertex_bytes, 0);
  draws("a stride larger than the whole view", vertex_view(base + Vertices, stride, 4096), nullptr, stride, 0);
  for (UINT there : {count - 1, count / 2, 1u, 0u}) {
    std::string name = "an index view of " + std::to_string(there) + " of the draw's " + std::to_string(count) + " indices";
    D3D12_INDEX_BUFFER_VIEW narrow_part{base + Narrow, UINT(there * sizeof(UINT16)), DXGI_FORMAT_R16_UINT},
        wide_part{base + Wide, UINT(there * sizeof(UINT)), DXGI_FORMAT_R32_UINT};
    draws((name + " of 16 bits").c_str(), whole, &narrow_part, vertex_bytes, narrow_part.SizeInBytes);
    draws((name + " of 32 bits").c_str(), whole, &wide_part, vertex_bytes, wide_part.SizeInBytes);
  }
  // a draw that begins part of the way into its indices runs past their end by as much; one that begins at their end
  // reads none
  for (UINT first : {count / 4, count}) {
    first_index = first;
    std::string name = "a draw of " + std::to_string(count) + " indices that begins at index " + std::to_string(first) + " of " + std::to_string(count);
    draws((name + ", 16 bits").c_str(), whole, &narrow_whole, vertex_bytes, narrow_bytes);
    draws((name + ", 32 bits").c_str(), whole, &wide_whole, vertex_bytes, wide_bytes);
  }
  first_index = 0;
  // a base vertex that takes the last indices past the vertices, and one below zero that takes the first ones there
  for (INT base : {8, -4}) {
    base_vertex = base;
    draws(("every index inside its view, and " + std::to_string(base) + " added to each").c_str(), whole, &narrow_whole, vertex_bytes, narrow_bytes);
  }
  base_vertex = 0;
  // no buffer at the slot the layout reads ("0 GPUVA or NULL pDesc means nothing bound", DirectX-Specs, Resource
  // Binding, SetVertexBuffers)
  draws("no vertex buffer at the layout's slot", vertex_view(0, 0, stride), nullptr, 0, 0);
  draws("no vertex buffer at the layout's slot, indexed", vertex_view(0, 0, stride), &wide_whole, 0, wide_bytes);
  if (tiled) {
    // views that say they cover the whole draw; the second half of it is in a tile with nothing behind it
    const D3D12_VERTEX_BUFFER_VIEW split = vertex_view(sparse_vertices->GetGPUVirtualAddress() + tile - half * stride, vertex_bytes, stride);
    const D3D12_INDEX_BUFFER_VIEW split_indices{sparse_indices->GetGPUVirtualAddress() + tile - half * sizeof(UINT), wide_bytes, DXGI_FORMAT_R32_UINT};
    draws("half the vertices in a reserved buffer's mapped tile, half in the tile that is not mapped", split, nullptr, half * stride, 0);
    draws("the same vertices, indexed", split, &wide_whole, half * stride, wide_bytes);
    draws("half the indices in a reserved buffer's mapped tile, half in the tile that is not mapped", whole, &split_indices, vertex_bytes,
          half * sizeof(UINT));
    draws("vertices and indices both half in tiles that are not mapped", split, &split_indices, half * stride, half * sizeof(UINT));
  } else {
    printf("not run, the device has no tiled resources of tier 2: a buffer with a tile that is not mapped\n");
  }
  return verdict();
}
