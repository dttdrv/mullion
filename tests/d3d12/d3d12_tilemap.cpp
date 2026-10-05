// contract: UpdateTileMappings' region and range rules (D3D12 UpdateTileMappings). a box's depth on an array steps to
// the same mip of the next slices; with several ranges and no counts each range is one tile; a texture without packed
// mips reports no packed tiles. residency reads through CheckAccessFullyMapped, contents through copies.
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
Texture2DArray<uint> t : register(t0);
RWStructuredBuffer<uint> o : register(u0);
[numthreads(1, 1, 1)] void cs() {
  uint status;
  // (slice, mip) as (x, y) of the group's four reads
  for (uint i = 0; i < 4; i++) {
    t.Load(int4(0, 0, i & 1, i >> 1), int2(0, 0), status);
    o[i] = CheckAccessFullyMapped(status);
  }
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto cs = compiler.compile(hlsl, "cs", "cs");
  if (cs.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  const UINT64 tile = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
  unsigned failures = 0;
  auto expect = [&](bool ok, const char *what) {
    if (!ok && ++failures)
      printf("%s\n", what);
  };

  // two slices of two mips, each mip at least a tile: the shape first, from a one-tile texture
  D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 1, 1, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0},
                           D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE};
  ComPtr<ID3D12Resource> texture;
  CHECK(device->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&texture)));
  D3D12_TILE_SHAPE shape;
  D3D12_PACKED_MIP_INFO packed;
  device->GetResourceTiling(texture.Get(), nullptr, &packed, &shape, nullptr, 0, nullptr);
  desc.Width = 2 * shape.WidthInTexels, desc.Height = 2 * shape.HeightInTexels, desc.DepthOrArraySize = 2, desc.MipLevels = 2;
  CHECK(device->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&texture)));
  device->GetResourceTiling(texture.Get(), nullptr, &packed, nullptr, nullptr, 0, nullptr);
  expect(packed.NumStandardMips == 2 && !packed.NumPackedMips, "the two mips are not both standard");
  expect(!packed.StartTileIndexInOverallResource && !packed.NumTilesForPackedMips, "packed tiles reported without packed mips");

  ComPtr<ID3D12Heap> heap;
  D3D12_HEAP_DESC heap_desc{3 * tile, {D3D12_HEAP_TYPE_DEFAULT}};
  CHECK(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap)));
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  // mip 0's first tile, two slices deep: slice 1's mip 0, not slice 0's mip 1
  D3D12_TILED_RESOURCE_COORDINATE origin{};
  D3D12_TILE_REGION_SIZE deep{2, TRUE, 1, 1, 2};
  queue->UpdateTileMappings(texture.Get(), 1, &origin, &deep, heap.Get(), 1, nullptr, nullptr, nullptr, D3D12_TILE_MAPPING_FLAG_NONE);

  // a buffer of two tiles, mapped tile by tile to heap tiles 1 and 2 by two uncounted ranges
  D3D12_RESOURCE_DESC buffer_desc{D3D12_RESOURCE_DIMENSION_BUFFER, 0, 2 * tile, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
  ComPtr<ID3D12Resource> reserved;
  CHECK(device->CreateReservedResource(&buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&reserved)));
  const D3D12_TILED_RESOURCE_COORDINATE tiles[2] = {{0}, {1}};
  const UINT heap_tiles[2] = {2, 1};
  queue->UpdateTileMappings(reserved.Get(), 2, tiles, nullptr, heap.Get(), 2, nullptr, heap_tiles, nullptr, D3D12_TILE_MAPPING_FLAG_NONE);
  // and an alias of the whole heap range, to see where each tile landed
  ComPtr<ID3D12Resource> alias;
  CHECK(device->CreateReservedResource(&buffer_desc, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&alias)));
  const UINT from = 1;
  D3D12_TILE_REGION_SIZE both{2};
  queue->UpdateTileMappings(alias.Get(), 1, &origin, &both, heap.Get(), 1, nullptr, &from, nullptr, D3D12_TILE_MAPPING_FLAG_NONE);

  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1};
  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[0].DescriptorTable = {1, &range};
  auto rs = root_signature(device.Get(), {2, params});
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{rs.Get(), bytecode(cs)};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)));
  ComPtr<ID3D12DescriptorHeap> views;
  D3D12_DESCRIPTOR_HEAP_DESC views_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&views_desc, IID_PPV_ARGS(&views)));
  device->CreateShaderResourceView(texture.Get(), nullptr, views->GetCPUDescriptorHandleForHeapStart());

  // each buffer tile holds its index + 1 in its first word; the alias reads heap tiles 1 and 2
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 2 * tile, D3D12_RESOURCE_STATE_GENERIC_READ);
  UINT *words;
  CHECK(upload->Map(0, nullptr, (void **)&words));
  words[0] = 1, words[tile / 4] = 2;
  auto out = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, 16, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 32, D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  list->CopyBufferRegion(reserved.Get(), 0, upload.Get(), 0, 2 * tile);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  CHECK(allocator->Reset());
  CHECK(list->Reset(allocator.Get(), pso.Get()));
  ID3D12DescriptorHeap *heaps[] = {views.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(rs.Get());
  list->SetComputeRootDescriptorTable(0, views->GetGPUDescriptorHandleForHeapStart());
  list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
  list->Dispatch(1, 1, 1);
  transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(), 0, out.Get(), 0, 16);
  for (UINT i = 0; i < 2; i++)
    list->CopyBufferRegion(readback.Get(), 16 + i * 4, alias.Get(), i * tile, 4);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  UINT *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  for (UINT i = 0; i < 4; i++) {
    UINT slice = i & 1, mip = i >> 1;
    bool mapped = mip == 0;
    if (got[i] != mapped && ++failures)
      printf("slice %u, mip %u: residency %u, want %u\n", slice, mip, got[i], mapped);
  }
  // heap tile h holds the buffer tile mapped to it
  for (UINT h = 1; h <= 2; h++) {
    UINT want = heap_tiles[0] == h ? 1 : 2;
    if (got[4 + h - 1] != want && ++failures)
      printf("heap tile %u: %u, want %u\n", h, got[4 + h - 1], want);
  }
  readback->Unmap(0, nullptr);

  // CopyTileMappings within one resource: "the source and destination regions can overlap; the result of the copy
  // in this situation is as if the source was saved to a temporary location and from there written to the
  // destination". a buffer of `span` tiles on heap tiles of their own, each holding its number, shifted by one tile
  // towards its end and, from the start again, towards its beginning
  for (int shift : {1, -1}) {
    const UINT span = 4, moved = span - 1;
    D3D12_HEAP_DESC pool_desc{span * tile, {D3D12_HEAP_TYPE_DEFAULT}, 0, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS};
    ComPtr<ID3D12Heap> pool;
    CHECK(device->CreateHeap(&pool_desc, IID_PPV_ARGS(&pool)));
    auto long_desc = buffer_desc;
    long_desc.Width = span * tile;
    ComPtr<ID3D12Resource> strip;
    CHECK(device->CreateReservedResource(&long_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&strip)));
    D3D12_TILE_REGION_SIZE all{span}, part{moved};
    const UINT first = 0;
    queue->UpdateTileMappings(strip.Get(), 1, &origin, &all, pool.Get(), 1, nullptr, &first, nullptr, D3D12_TILE_MAPPING_FLAG_NONE);
    auto numbers = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, span * tile, D3D12_RESOURCE_STATE_GENERIC_READ);
    UINT *numbered;
    CHECK(numbers->Map(0, nullptr, (void **)&numbered));
    for (UINT i = 0; i < span; i++)
      numbered[i * tile / 4] = 11 * (i + 1);
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), pso.Get()));
    list->CopyBufferRegion(strip.Get(), 0, numbers.Get(), 0, span * tile);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    // tiles [from, from + moved) to [to, to + moved)
    const D3D12_TILED_RESOURCE_COORDINATE from{shift > 0 ? 0u : 1u}, to{shift > 0 ? 1u : 0u};
    queue->CopyTileMappings(strip.Get(), &to, strip.Get(), &from, &part, D3D12_TILE_MAPPING_FLAG_NONE);
    auto strip_read = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, span * 4, D3D12_RESOURCE_STATE_COPY_DEST);
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), pso.Get()));
    transition(list.Get(), strip.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (UINT i = 0; i < span; i++)
      list->CopyBufferRegion(strip_read.Get(), i * 4, strip.Get(), i * tile, 4);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    UINT *shifted;
    CHECK(strip_read->Map(0, nullptr, (void **)&shifted));
    for (UINT i = 0; i < span; i++) {
      // a tile of the destination region shows what its source tile showed before; the others are as they were
      bool copied = i >= to.X && i < to.X + moved;
      UINT want = 11 * ((copied ? i - to.X + from.X : i) + 1);
      if (shifted[i] != want && ++failures)
        printf("mappings shifted by %d: tile %u shows %u, want %u\n", shift, i, shifted[i], want);
    }
    strip_read->Unmap(0, nullptr);
  }
  printf("%s: %u wrong answers\n", failures ? "failed" : "passed", failures);
  return failures != 0;
}
