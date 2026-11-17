// contract: a reserved texture with every tile mapped keeps what is copied into each of its mips, the packed ones
// too. Metal (macOS 27.2, placement sparse tier 2) keeps nothing in some mips of the tail of some block compressed
// sparse textures whose height is not a power of two: mips 6 and 7 of a 2048x1080 BC6H or BC7 texture read zeros
// whatever is copied in (CONTROL Resonant's window interiors, which glowed magenta and yellow). Mullion makes such a
// texture an ordinary one (MTLD3D12Device::SparseTailHolds); this holds it to that. each shape maps all its tiles,
// standard and packed, takes a copy into every mip, each mip its own bytes, and is copied out whole and compared.
// the packed tiles are then mapped to other heap tiles and filled again, as a game streaming the texture does
#include "d3d12_test.hpp"
#include <vector>

int
main(int argc, char **argv) {
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
  if (options.TiledResourcesTier < D3D12_TILED_RESOURCES_TIER_2) {
    printf("skipped: tiled resources tier %d\n", options.TiledResourcesTier);
    return 77;
  }
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  auto run = [&]() {
    HRESULT hr = submit(device.Get(), queue.Get(), list.Get());
    return FAILED(hr) || FAILED(hr = allocator->Reset()) ? hr : list->Reset(allocator.Get(), nullptr);
  };

  static const struct {
    DXGI_FORMAT format;
    UINT width, height;
    const char *name;
  } shapes[] = {
      // the shapes Metal loses tail mips of
      {DXGI_FORMAT_BC6H_UF16, 2048, 1080, "BC6H 2048x1080"},
      {DXGI_FORMAT_BC7_UNORM, 1920, 1080, "BC7 1920x1080"},
      {DXGI_FORMAT_BC7_UNORM_SRGB, 2048, 1056, "BC7_SRGB 2048x1056"},
      {DXGI_FORMAT_BC6H_UF16, 1024, 540, "BC6H 1024x540"},
      {DXGI_FORMAT_BC1_UNORM, 2048, 1080, "BC1 2048x1080"},
      // and ones it keeps, which stay sparse
      {DXGI_FORMAT_BC7_UNORM, 2048, 1024, "BC7 2048x1024"},
      {DXGI_FORMAT_R16G16B16A16_FLOAT, 2048, 1080, "RGBA16F 2048x1080"},
  };
  const UINT64 tile = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
  unsigned failures = 0;
  for (auto &shape : shapes) {
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, shape.width, shape.height, 1, 0, shape.format,
                             {1, 0}, D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE};
    ComPtr<ID3D12Resource> texture;
    CHECK(device->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)));
    desc = texture->GetDesc();
    UINT total = 0;
    D3D12_PACKED_MIP_INFO packed{};
    device->GetResourceTiling(texture.Get(), &total, &packed, nullptr, nullptr, 0, nullptr);
    if (!packed.NumPackedMips) {
      printf("%s: no packed mips, which this tests\n", shape.name);
      failures++;
      continue;
    }
    // every tile, then the packed ones again at heap tiles of their own
    ComPtr<ID3D12Heap> heap;
    D3D12_HEAP_DESC heap_desc{(total + packed.NumTilesForPackedMips) * tile, {D3D12_HEAP_TYPE_DEFAULT}, 0,
                              D3D12_HEAP_FLAG_DENY_BUFFERS | D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES};
    CHECK(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap)));
    D3D12_TILED_RESOURCE_COORDINATE origin{};
    D3D12_TILE_REGION_SIZE region{total};
    UINT offset = 0;
    queue->UpdateTileMappings(texture.Get(), 1, &origin, &region, heap.Get(), 1, nullptr, &offset, &total,
                              D3D12_TILE_MAPPING_FLAG_NONE);

    UINT mips = desc.MipLevels;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(mips);
    std::vector<UINT> rows(mips);
    std::vector<UINT64> row_bytes(mips);
    UINT64 bytes = 0;
    device->GetCopyableFootprints(&desc, 0, mips, 0, footprints.data(), rows.data(), row_bytes.data(), &bytes);
    auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    unsigned char *in, *out;
    CHECK(upload->Map(0, nullptr, (void **)&in));
    CHECK(readback->Map(0, nullptr, (void **)&out));

    // fills every mip, each byte its mip's and its place's, `round` apart from the last fill; then copies all out
    auto fill_and_check = [&](unsigned round, const char *when) {
      auto value = [&](UINT mip, UINT64 i) { return (unsigned char)(0x11 * (mip + 1) + 7 * round + i * 13); };
      for (UINT mip = 0; mip < mips; mip++)
        for (UINT row = 0; row < rows[mip]; row++)
          for (UINT64 i = 0; i < row_bytes[mip]; i++)
            in[footprints[mip].Offset + row * footprints[mip].Footprint.RowPitch + i] = value(mip, i);
      for (UINT mip = 0; mip < mips; mip++) {
        D3D12_TEXTURE_COPY_LOCATION to{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        to.SubresourceIndex = mip;
        D3D12_TEXTURE_COPY_LOCATION from{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        from.PlacedFootprint = footprints[mip];
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
      }
      transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
      for (UINT mip = 0; mip < mips; mip++) {
        D3D12_TEXTURE_COPY_LOCATION to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        to.PlacedFootprint = footprints[mip];
        D3D12_TEXTURE_COPY_LOCATION from{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        from.SubresourceIndex = mip;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
      }
      transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
      CHECK(run());
      for (UINT mip = 0; mip < mips; mip++) {
        UINT64 wrong = 0, first = 0;
        for (UINT row = 0; row < rows[mip]; row++)
          for (UINT64 i = 0; i < row_bytes[mip]; i++) {
            auto at = footprints[mip].Offset + row * footprints[mip].Footprint.RowPitch + i;
            if (out[at] != value(mip, i) && !wrong++)
              first = at;
          }
        if (wrong) {
          failures++;
          printf("%s, %s: mip %u (%s) has %llu of %llu bytes wrong, first %#x, want %#x\n", shape.name, when, mip,
                 mip < packed.NumStandardMips ? "standard" : "packed", (unsigned long long)wrong,
                 (unsigned long long)(rows[mip] * row_bytes[mip]), out[first],
                 value(mip, (first - footprints[mip].Offset) % footprints[mip].Footprint.RowPitch));
        }
      }
      return 0;
    };
    if (fill_and_check(0, "every tile mapped"))
      return 1;
    // the packed tiles moved to other heap tiles: their contents are undefined until filled again
    D3D12_TILED_RESOURCE_COORDINATE tail{0, 0, 0, packed.NumStandardMips};
    D3D12_TILE_REGION_SIZE tail_region{packed.NumTilesForPackedMips};
    UINT tail_offset = total, tail_count = packed.NumTilesForPackedMips;
    queue->UpdateTileMappings(texture.Get(), 1, &tail, &tail_region, heap.Get(), 1, nullptr, &tail_offset, &tail_count,
                              D3D12_TILE_MAPPING_FLAG_NONE);
    if (fill_and_check(1, "packed tiles remapped"))
      return 1;
    printf("%s: %u mips, %u standard, %u packed in %u tiles\n", shape.name, mips, packed.NumStandardMips,
           packed.NumPackedMips, packed.NumTilesForPackedMips);
  }
  printf("%s: %u wrong mips\n", failures ? "failed" : "passed", failures);
  return failures != 0;
}
