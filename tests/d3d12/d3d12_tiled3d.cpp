// contract: tiled resources, tier 3 (D3D12_TILED_RESOURCES_TIER_3: "3D textures (Volume Tiled Resources) are
// supported"). a reserved 3D texture's tiles, of the shape GetResourceTiling reports, map by tile box to chosen heap
// tiles: a mapped tile keeps what a shader writes there, a NULL tile reads 0 and drops writes, loads report through
// CheckAccessFullyMapped whether the tile they read is mapped, and a second 3D texture mapped to the same heap tile
// sees the same texels at the same place in its tile. writes and reads run in separate submissions, so what is read is
// what memory holds. the shape is Metal's 3D sparse tile, a slab one texel deep (the D3D11.3 standard 3D shapes, such
// as 32x32x16 at 32 bits, are not what a Metal sparse 3D texture maps by)
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_tiled_resources_tier
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
RWTexture3D<uint> volume : register(u0);
[numthreads(8, 8, 1)] void write(uint3 t : SV_DispatchThreadID) {
  volume[t] = 1 + t.x + t.y * WIDTH + t.z * WIDTH * HEIGHT;
}

Texture3D<uint> first : register(t0);
Texture3D<uint> second : register(t1);
RWStructuredBuffer<uint4> o : register(u1);
// every texel of both: its value and whether its tile is mapped
[numthreads(8, 8, 1)] void read(uint3 t : SV_DispatchThreadID) {
  uint status, other_status;
  uint value = first.Load(int4(t, 0), int3(0, 0, 0), status);
  uint other = second.Load(int4(t, 0), int3(0, 0, 0), other_status);
  o[t.x + t.y * WIDTH + t.z * WIDTH * HEIGHT] = uint4(value, CheckAccessFullyMapped(status), other,
                                                      CheckAccessFullyMapped(other_status));
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
  unsigned failures = 0;
  auto fail = [&](const char *format, auto... args) {
    if (failures++ < 16) {
      printf(format, args...);
      printf("\n");
    }
  };
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
  if (options.TiledResourcesTier < D3D12_TILED_RESOURCES_TIER_3) {
    printf("failed: tiled resources tier %u\n", options.TiledResourcesTier);
    return 1;
  }

  // two tiles wide and high and four deep, whatever the shape
  D3D12_RESOURCE_DESC probe{
      D3D12_RESOURCE_DIMENSION_TEXTURE3D,
      0,
      64,
      64,
      4,
      1,
      DXGI_FORMAT_R32_UINT,
      {1, 0},
      D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  };
  ComPtr<ID3D12Resource> probed;
  CHECK(device->CreateReservedResource(&probe, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&probed)));
  D3D12_TILE_SHAPE shape;
  UINT total = 0;
  device->GetResourceTiling(probed.Get(), &total, nullptr, &shape, nullptr, 0, nullptr);
  if (!shape.WidthInTexels || !shape.HeightInTexels || !shape.DepthInTexels ||
      shape.WidthInTexels * shape.HeightInTexels * shape.DepthInTexels * 4 != D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES) {
    printf(
        "failed: a 32-bit 3D tile of %ux%ux%u texels\n", shape.WidthInTexels, shape.HeightInTexels, shape.DepthInTexels
    );
    return 1;
  }
  {
    // a mip chain: its standard mips' tiles, then the packed ones, add up
    D3D12_RESOURCE_DESC chain = probe;
    chain.Width = chain.Height = 512, chain.DepthOrArraySize = 8, chain.MipLevels = 0;
    ComPtr<ID3D12Resource> mipped;
    CHECK(device->CreateReservedResource(&chain, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&mipped)));
    UINT tiles = 0, subresources = 10;
    D3D12_PACKED_MIP_INFO packed{};
    D3D12_SUBRESOURCE_TILING mips[10]{};
    device->GetResourceTiling(mipped.Get(), &tiles, &packed, nullptr, &subresources, 0, mips);
    UINT standard = 0;
    for (UINT m = 0; m < packed.NumStandardMips; m++)
      standard += mips[m].WidthInTiles * mips[m].HeightInTiles * mips[m].DepthInTiles;
    if (subresources != 10 || packed.NumStandardMips + packed.NumPackedMips != 10 ||
        standard + packed.NumTilesForPackedMips != tiles ||
        (packed.NumPackedMips && packed.StartTileIndexInOverallResource != standard))
      fail("a 512x512x8 chain: %u mips, %u standard, %u packed in %u tiles from %u, %u tiles in all", subresources,
           packed.NumStandardMips, packed.NumPackedMips, packed.NumTilesForPackedMips,
           packed.StartTileIndexInOverallResource, tiles);
  }
  const UINT width = 2 * shape.WidthInTexels, height = 2 * shape.HeightInTexels, depth = 4 * shape.DepthInTexels;
  D3D12_RESOURCE_DESC desc = probe;
  desc.Width = width, desc.Height = height, desc.DepthOrArraySize = UINT16(depth);
  ComPtr<ID3D12Resource> volumes[2];
  for (auto &v : volumes)
    CHECK(device->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&v)));
  D3D12_SUBRESOURCE_TILING tiling{};
  UINT count = 1;
  device->GetResourceTiling(volumes[0].Get(), &total, nullptr, nullptr, &count, 0, &tiling);
  if (total != 16 || tiling.WidthInTiles != 2 || tiling.HeightInTiles != 2 || tiling.DepthInTiles != 4)
    fail(
        "%ux%ux%u texels: %u tiles, %ux%ux%u", width, height, depth, total, tiling.WidthInTiles, tiling.HeightInTiles,
        tiling.DepthInTiles
    );

  std::vector<std::string> defines = {"WIDTH=" + std::to_string(width), "HEIGHT=" + std::to_string(height)};
  auto write = compiler.compile(hlsl, "write", "cs", defines), read = compiler.compile(hlsl, "read", "cs", defines);
  if (write.empty() || read.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0}};
  D3D12_ROOT_PARAMETER params[3] = {
      {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
      {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
      {D3D12_ROOT_PARAMETER_TYPE_UAV}
  };
  params[0].DescriptorTable = {1, &ranges[0]};
  params[1].DescriptorTable = {1, &ranges[1]};
  params[2].Descriptor = {1, 0};
  auto rs = root_signature(device.Get(), {3, params});
  ComPtr<ID3D12PipelineState> write_pso, read_pso;
  D3D12_COMPUTE_PIPELINE_STATE_DESC cs{rs.Get(), bytecode(write)};
  CHECK(device->CreateComputePipelineState(&cs, IID_PPV_ARGS(&write_pso)));
  cs.CS = bytecode(read);
  CHECK(device->CreateComputePipelineState(&cs, IID_PPV_ARGS(&read_pso)));

  ComPtr<ID3D12DescriptorHeap> descriptors;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
  };
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&descriptors)));
  auto step_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  auto cpu = descriptors->GetCPUDescriptorHandleForHeapStart();
  auto gpu = descriptors->GetGPUDescriptorHandleForHeapStart();
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{DXGI_FORMAT_R32_UINT, D3D12_UAV_DIMENSION_TEXTURE3D};
  uav.Texture3D = {0, 0, depth};
  device->CreateUnorderedAccessView(volumes[0].Get(), nullptr, &uav, cpu);
  for (UINT i = 0; i < 2; i++) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{
        DXGI_FORMAT_R32_UINT, D3D12_SRV_DIMENSION_TEXTURE3D, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
    };
    srv.Texture3D.MipLevels = 1;
    device->CreateShaderResourceView(volumes[i].Get(), &srv, {cpu.ptr + (1 + i) * step_size});
  }

  // heap tile 2 is the first volume's tile (1, 0, 2) and the second's (0, 1, 3); the rest is NULL
  ComPtr<ID3D12Heap> heap;
  D3D12_HEAP_DESC tiles{
      4 * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES,
      {D3D12_HEAP_TYPE_DEFAULT},
      0,
      D3D12_HEAP_FLAG_DENY_BUFFERS | D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES
  };
  CHECK(device->CreateHeap(&tiles, IID_PPV_ARGS(&heap)));
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  const D3D12_TILED_RESOURCE_COORDINATE mapped[2] = {{1, 0, 2, 0}, {0, 1, 3, 0}};
  for (UINT i = 0; i < 2; i++) {
    D3D12_TILE_REGION_SIZE one{1, FALSE};
    D3D12_TILE_RANGE_FLAGS range = D3D12_TILE_RANGE_FLAG_NONE;
    UINT start = 2, length = 1;
    queue->UpdateTileMappings(
        volumes[i].Get(), 1, &mapped[i], &one, heap.Get(), 1, &range, &start, &length, D3D12_TILE_MAPPING_FLAG_NONE
    );
  }

  const UINT texels = width * height * depth;
  auto out = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, UINT64(texels) * 16, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, UINT64(texels) * 16, D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(
      0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), write_pso.Get(), IID_PPV_ARGS(&list)
  ));
  ID3D12DescriptorHeap *heaps[] = {descriptors.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(rs.Get());
  list->SetComputeRootDescriptorTable(0, gpu);
  list->Dispatch(width / 8, height / 8, depth);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  step("read every texel of both volumes");
  CHECK(allocator->Reset());
  CHECK(list->Reset(allocator.Get(), read_pso.Get()));
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(rs.Get());
  transition(
      list.Get(), volumes[0].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
  );
  transition(
      list.Get(), volumes[1].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
  );
  list->SetComputeRootDescriptorTable(1, {gpu.ptr + step_size});
  list->SetComputeRootUnorderedAccessView(2, out->GetGPUVirtualAddress());
  list->Dispatch(width / 8, height / 8, depth);
  transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(), 0, out.Get(), 0, UINT64(texels) * 16);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  UINT *o;
  CHECK(readback->Map(0, nullptr, (void **)&o));
  auto in_tile = [&](UINT x, UINT y, UINT z, const D3D12_TILED_RESOURCE_COORDINATE &tile) {
    return x / shape.WidthInTexels == tile.X && y / shape.HeightInTexels == tile.Y && z / shape.DepthInTexels == tile.Z;
  };
  for (UINT z = 0; z < depth; z++)
    for (UINT y = 0; y < height; y++)
      for (UINT x = 0; x < width; x++) {
        auto *t = o + 4 * (x + y * width + z * width * height);
        bool first_mapped = in_tile(x, y, z, mapped[0]), second_mapped = in_tile(x, y, z, mapped[1]);
        UINT want = first_mapped ? 1 + x + y * width + z * width * height : 0;
        if (t[0] != want || t[1] != first_mapped)
          fail("first volume (%u, %u, %u): %u, mapped %u; want %u, mapped %u", x, y, z, t[0], t[1], want, first_mapped);
        // the second volume's mapped tile has the first's texels at the same place in the tile
        UINT sx = x % shape.WidthInTexels + mapped[0].X * shape.WidthInTexels,
             sy = y % shape.HeightInTexels + mapped[0].Y * shape.HeightInTexels,
             sz = z % shape.DepthInTexels + mapped[0].Z * shape.DepthInTexels;
        UINT alias = second_mapped ? 1 + sx + sy * width + sz * width * height : 0;
        if (t[2] != alias || t[3] != second_mapped)
          fail(
              "second volume (%u, %u, %u): %u, mapped %u; want %u, mapped %u", x, y, z, t[2], t[3], alias, second_mapped
          );
      }
  readback->Unmap(0, nullptr);
  printf(
      "%s: %u mismatches over %u texels of %ux%ux%u tiles\n", failures ? "failed" : "passed", failures, texels,
      shape.WidthInTexels, shape.HeightInTexels, shape.DepthInTexels
  );
  return failures != 0;
}
