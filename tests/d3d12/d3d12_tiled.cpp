// contract: tiled resources, tier 2 (D3D12 tiled resources, D3D11.3 5.9). a reserved buffer's tiles map to chosen heap
// tiles, a NULL tile reads 0 and drops writes, two resources mapped to one heap tile alias, CopyTileMappings gives a
// resource another's mappings, and a reserved texture maps tiles of a mip by box, loads report through
// CheckAccessFullyMapped whether the tile they read is mapped. writes and reads run in separate submissions, so what is
// read is what memory holds. GetResourceTiling reports D3D12's standard 2D tile shapes, and keeps every mip at least
// one tile in size unpacked (tier 2).
#include "d3d12_test.hpp"
#include <algorithm>

static const char hlsl[] = R"hlsl(
RWStructuredBuffer<uint> written : register(u0);
RWTexture2D<uint> texels : register(u1);
[numthreads(64, 1, 1)] void write(uint i : SV_DispatchThreadID) {
  written[i] = i + 1;
  texels[uint2(i % SIZE, i / SIZE)] = i + 1;
}

StructuredBuffer<uint> a : register(t0);
StructuredBuffer<uint> b : register(t1);
StructuredBuffer<uint> c : register(t2);
Texture2D<uint> tiled : register(t3);
RWStructuredBuffer<uint> o : register(u0);
[numthreads(64, 1, 1)] void read(uint i : SV_DispatchThreadID) {
  o[i * 5 + 0] = a[i];
  o[i * 5 + 1] = b[i];
  o[i * 5 + 2] = c[i];
  uint status;
  o[i * 5 + 3] = tiled.Load(int3(i % SIZE, i / SIZE, 0), int2(0, 0), status);
  o[i * 5 + 4] = CheckAccessFullyMapped(status);
}
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
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
  if (options.TiledResourcesTier < D3D12_TILED_RESOURCES_TIER_2) {
    printf("failed: tiled resources tier %d\n", options.TiledResourcesTier);
    return 1;
  }
  unsigned failures = 0;
  auto expect = [&](bool ok, const char *what, UINT at, UINT got, UINT want) {
    if (!ok && failures++ < 12)
      printf("%s at %u: %u, want %u\n", what, at, got, want);
  };

  // tiling: D3D12's standard shape for 4-byte texels is 128x128; mips of at least one tile are unpacked
  const UINT tile = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES, dword_tile = tile / 4, size = 256;
  D3D12_RESOURCE_DESC texture_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 0, DXGI_FORMAT_R32_UINT, {1, 0},
                                   D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
  ComPtr<ID3D12Resource> texture;
  CHECK(device->CreateReservedResource(&texture_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&texture)));
  UINT total, count = 1;
  D3D12_PACKED_MIP_INFO packed;
  D3D12_TILE_SHAPE shape;
  D3D12_SUBRESOURCE_TILING mip0;
  device->GetResourceTiling(texture.Get(), &total, &packed, &shape, &count, 0, &mip0);
  const UINT standard = 128; // D3D12 standard tile, 2D, 32 bits per texel
  expect(shape.WidthInTexels == standard && shape.HeightInTexels == standard && shape.DepthInTexels == 1,
         "tile shape width", 0, shape.WidthInTexels, standard);
  UINT tiled_mips = 0;
  while ((size >> tiled_mips) >= standard)
    tiled_mips++;
  expect(packed.NumStandardMips >= tiled_mips, "unpacked mips", 0, packed.NumStandardMips, tiled_mips);
  expect(mip0.WidthInTiles == size / standard && mip0.HeightInTiles == size / standard && mip0.StartTileIndexInOverallResource == 0,
         "mip 0 tiles across", 0, mip0.WidthInTiles, size / standard);

  // three reserved buffers of four tiles, and a heap of four for them and two for the texture
  auto reserved = [&](UINT64 bytes) {
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_BUFFER, 0, bytes, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                             D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    ComPtr<ID3D12Resource> res;
    device->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&res));
    return res;
  };
  const UINT tiles = 4, words = tiles * dword_tile;
  auto a = reserved(tiles * tile), b = reserved(tiles * tile), c = reserved(tiles * tile);
  ComPtr<ID3D12Heap> heap;
  D3D12_HEAP_DESC heap_desc{(tiles + 2) * tile, {D3D12_HEAP_TYPE_DEFAULT}};
  CHECK(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap)));
  if (!a || !b || !c) {
    printf("failed: reserved buffers\n");
    return 1;
  }

  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  // a: tiles 0 and 1 at heap tiles 2 and 3, tile 2 NULL, tile 3 at heap tile 0
  D3D12_TILED_RESOURCE_COORDINATE origin{};
  D3D12_TILE_REGION_SIZE all{tiles};
  const D3D12_TILE_RANGE_FLAGS flags[] = {D3D12_TILE_RANGE_FLAG_NONE, D3D12_TILE_RANGE_FLAG_NULL, D3D12_TILE_RANGE_FLAG_NONE};
  const UINT offsets[] = {2, 0, 0}, counts[] = {2, 1, 1};
  queue->UpdateTileMappings(a.Get(), 1, &origin, &all, heap.Get(), 3, flags, offsets, counts, D3D12_TILE_MAPPING_FLAG_NONE);
  const UINT heap_tile_of_a[tiles] = {2, 3, ~0u, 0};
  // b's tile 0 is a's tile 1 (heap tile 3); c takes a's mappings
  D3D12_TILE_REGION_SIZE one{1};
  const UINT three = 3;
  queue->UpdateTileMappings(b.Get(), 1, &origin, &one, heap.Get(), 1, nullptr, &three, nullptr, D3D12_TILE_MAPPING_FLAG_NONE);
  queue->CopyTileMappings(c.Get(), &origin, a.Get(), &origin, &all, D3D12_TILE_MAPPING_FLAG_NONE);
  // the texture's mip 0 is 2x2 tiles: its first row maps to heap tiles 4 and 5, the second stays unmapped
  D3D12_TILE_REGION_SIZE row{2, TRUE, 2, 1, 1};
  const UINT texture_heap_tile = tiles;
  queue->UpdateTileMappings(
      texture.Get(), 1, &origin, &row, heap.Get(), 1, nullptr, &texture_heap_tile, nullptr, D3D12_TILE_MAPPING_FLAG_NONE
  );
  auto texture_tile_mapped = [&](UINT x, UINT y) { return y / standard == 0; };

  auto write = compiler.compile(hlsl, "write", "cs", {"SIZE=" + std::to_string(size)}),
       read = compiler.compile(hlsl, "read", "cs", {"SIZE=" + std::to_string(size)});
  if (write.empty() || read.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  D3D12_DESCRIPTOR_RANGE uav_range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1}, srv_range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3};
  D3D12_ROOT_PARAMETER params[6] = {{D3D12_ROOT_PARAMETER_TYPE_UAV}, {D3D12_ROOT_PARAMETER_TYPE_SRV},
                                    {D3D12_ROOT_PARAMETER_TYPE_SRV}, {D3D12_ROOT_PARAMETER_TYPE_SRV},
                                    {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}, {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}};
  params[2].Descriptor.ShaderRegister = 1;
  params[3].Descriptor.ShaderRegister = 2;
  params[4].DescriptorTable = {1, &uav_range};
  params[5].DescriptorTable = {1, &srv_range};
  auto rs = root_signature(device.Get(), {6, params});
  ComPtr<ID3D12PipelineState> write_pso, read_pso;
  D3D12_COMPUTE_PIPELINE_STATE_DESC write_desc{rs.Get(), bytecode(write)}, read_desc{rs.Get(), bytecode(read)};
  CHECK(device->CreateComputePipelineState(&write_desc, IID_PPV_ARGS(&write_pso)));
  CHECK(device->CreateComputePipelineState(&read_desc, IID_PPV_ARGS(&read_pso)));
  ComPtr<ID3D12DescriptorHeap> views;
  D3D12_DESCRIPTOR_HEAP_DESC views_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&views_desc, IID_PPV_ARGS(&views)));
  auto step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  auto cpu = views->GetCPUDescriptorHandleForHeapStart();
  auto gpu = views->GetGPUDescriptorHandleForHeapStart();
  D3D12_UNORDERED_ACCESS_VIEW_DESC texture_uav{DXGI_FORMAT_R32_UINT, D3D12_UAV_DIMENSION_TEXTURE2D};
  device->CreateUnorderedAccessView(texture.Get(), nullptr, &texture_uav, cpu);
  D3D12_SHADER_RESOURCE_VIEW_DESC texture_srv{DXGI_FORMAT_R32_UINT, D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
  texture_srv.Texture2D.MipLevels = 1;
  device->CreateShaderResourceView(texture.Get(), &texture_srv, {cpu.ptr + step});

  const UINT threads = words; // every dword of a, and as many texels
  auto out = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, threads * 5 * 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, threads * 5 * 4, D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  // one submission writes, the next reads
  for (int pass = 0; pass < 2; pass++) {
    if (pass)
      CHECK(allocator->Reset());
    CHECK(pass ? list->Reset(allocator.Get(), read_pso.Get())
               : device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), write_pso.Get(), IID_PPV_ARGS(&list)));
    ID3D12DescriptorHeap *heaps[] = {views.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(rs.Get());
    list->SetComputeRootUnorderedAccessView(0, (pass ? out : a)->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(1, a->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(2, b->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(3, c->GetGPUVirtualAddress());
    list->SetComputeRootDescriptorTable(4, gpu);
    list->SetComputeRootDescriptorTable(5, {gpu.ptr + step});
    list->Dispatch(threads / 64, 1, 1);
    if (pass) {
      transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyResource(readback.Get(), out.Get());
    }
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
  }

  UINT *o;
  CHECK(readback->Map(0, nullptr, (void **)&o));
  for (UINT i = 0; i < threads; i++) {
    auto r = o + i * 5;
    UINT t = i / dword_tile;
    UINT a_want = heap_tile_of_a[t] == ~0u ? 0 : i + 1;
    expect(r[0] == a_want, "reserved buffer", i, r[0], a_want);
    // b's tile 0 is heap tile 3, where a's tile 1 lives; its other tiles are unmapped
    UINT b_want = t == 0 ? i + dword_tile + 1 : 0;
    expect(r[1] == b_want, "aliasing reserved buffer", i, r[1], b_want);
    expect(r[2] == a_want, "copied mappings", i, r[2], a_want);
    UINT x = i % size, y = i / size;
    bool mapped = texture_tile_mapped(x, y);
    expect(r[3] == (mapped ? i + 1 : 0), "reserved texture", i, r[3], mapped ? i + 1 : 0);
    expect(r[4] == mapped, "CheckAccessFullyMapped", i, r[4], mapped);
  }
  readback->Unmap(0, nullptr);
  printf("%s: %u wrong values\n", failures ? "failed" : "passed", failures);
  return failures != 0;
}
