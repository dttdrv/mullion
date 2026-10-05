// contract: CopyTiles (D3D12 tiled resources). on the buffer side each tile is 64KB of linear rows, a row as wide as a
// full tile even where the texture ends inside it; tiles follow the region's order. copying in writes the mapped
// tiles and drops the writes to unmapped ones; copying out reads a mapped tile's texels and zeros from an unmapped one.
// a reserved buffer's tiles copy the same way. the texture is 1.5 tiles across and down, so its edge tiles are partial.
// the texture is checked through CopyTextureRegion, the buffer side byte for byte. copying in and reading run in
// separate submissions: within one, a dropped write to an unmapped tile can still read back from the GPU's caches.
#include "d3d12_test.hpp"

int
main(int argc, char **argv) {
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
  if (options.TiledResourcesTier < D3D12_TILED_RESOURCES_TIER_2) {
    printf("failed: tiled resources tier %d\n", options.TiledResourcesTier);
    return 1;
  }
  const UINT tile = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES, texel = 4;
  D3D12_RESOURCE_DESC texture_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 1, 1, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0},
                                   D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE};
  // the shape, then a texture of one and a half tiles each way
  ComPtr<ID3D12Resource> texture;
  CHECK(device->CreateReservedResource(&texture_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)));
  UINT total, count = 1;
  D3D12_PACKED_MIP_INFO packed;
  D3D12_TILE_SHAPE shape;
  D3D12_SUBRESOURCE_TILING sub;
  device->GetResourceTiling(texture.Get(), &total, &packed, &shape, &count, 0, &sub);
  const UINT tw = shape.WidthInTexels, th = shape.HeightInTexels, width = tw * 3 / 2, height = th * 3 / 2;
  texture_desc.Width = width, texture_desc.Height = height;
  CHECK(device->CreateReservedResource(&texture_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)));
  count = 1;
  device->GetResourceTiling(texture.Get(), &total, &packed, &shape, &count, 0, &sub);
  if (sub.WidthInTiles != 2 || sub.HeightInTiles != 2 || packed.NumPackedMips) {
    printf("failed: %ux%u tiles, %u packed mips\n", sub.WidthInTiles, sub.HeightInTiles, packed.NumPackedMips);
    return 1;
  }
  D3D12_RESOURCE_DESC buffer_desc{D3D12_RESOURCE_DIMENSION_BUFFER, 0, 2 * tile, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
  ComPtr<ID3D12Resource> reserved;
  CHECK(device->CreateReservedResource(&buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&reserved)));

  // texture tiles 0, 1 and 3 (x, then y) and the buffer's tile 0 are mapped; texture tile 2 and buffer tile 1 are not
  ComPtr<ID3D12Heap> heap;
  D3D12_HEAP_DESC heap_desc{4 * tile, {D3D12_HEAP_TYPE_DEFAULT}};
  CHECK(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap)));
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  const bool mapped[4] = {true, true, false, true};
  D3D12_TILED_RESOURCE_COORDINATE origin{};
  D3D12_TILE_REGION_SIZE box{4, TRUE, 2, 2, 1}, pair{2};
  const D3D12_TILE_RANGE_FLAGS flags[] = {D3D12_TILE_RANGE_FLAG_NONE, D3D12_TILE_RANGE_FLAG_NULL, D3D12_TILE_RANGE_FLAG_NONE};
  const UINT offsets[] = {0, 0, 2}, counts[] = {2, 1, 1};
  queue->UpdateTileMappings(texture.Get(), 1, &origin, &box, heap.Get(), 3, flags, offsets, counts, D3D12_TILE_MAPPING_FLAG_NONE);
  const UINT buffer_offsets[] = {3, 0}, buffer_counts[] = {1, 1};
  queue->UpdateTileMappings(
      reserved.Get(), 1, &origin, &pair, heap.Get(), 2, flags, buffer_offsets, buffer_counts, D3D12_TILE_MAPPING_FLAG_NONE
  );

  // the source: word w of tile n is n << 24 | w + 1
  const UINT words = tile / texel;
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 4 * tile, D3D12_RESOURCE_STATE_GENERIC_READ);
  UINT *source;
  CHECK(upload->Map(0, nullptr, (void **)&source));
  for (UINT n = 0; n < 4; n++)
    for (UINT w = 0; w < words; w++)
      source[n * words + w] = n << 24 | (w + 1);
  const UINT pitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT * ((width * texel + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) / D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
  // read back: the texture's texels, then the tiles copied out of it, then the reserved buffer
  const UINT64 tiles_at = UINT64(pitch) * height, buffer_at = tiles_at + 4 * tile;
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, buffer_at + 2 * tile, D3D12_RESOURCE_STATE_COPY_DEST);
  auto out = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, 4 * tile, D3D12_RESOURCE_STATE_COPY_DEST);

  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  list->CopyTiles(texture.Get(), &origin, &box, upload.Get(), 0, D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE);
  list->CopyTiles(reserved.Get(), &origin, &pair, upload.Get(), 0, D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  CHECK(allocator->Reset());
  CHECK(list->Reset(allocator.Get(), nullptr));
  transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(list.Get(), reserved.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyTiles(texture.Get(), &origin, &box, out.Get(), 0, D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
  D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  dst.PlacedFootprint = {0, {DXGI_FORMAT_R32_UINT, width, height, 1, pitch}};
  D3D12_TEXTURE_COPY_LOCATION src{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(), tiles_at, out.Get(), 0, 4 * tile);
  list->CopyBufferRegion(readback.Get(), buffer_at, reserved.Get(), 0, 2 * tile);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  UINT8 *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  unsigned failures = 0;
  auto expect = [&](UINT value, UINT want, const char *what, UINT at) {
    if (value != want && failures++ < 12)
      printf("%s at %u: %#x, want %#x\n", what, at, value, want);
  };
  auto word = [&](UINT64 byte) { return *reinterpret_cast<UINT *>(got + byte); };
  for (UINT y = 0; y < height; y++)
    for (UINT x = 0; x < width; x++) {
      UINT n = y / th * 2 + x / tw, w = y % th * tw + x % tw;
      expect(word(UINT64(y) * pitch + x * texel), mapped[n] ? source[n * words + w] : 0, "texel", y * width + x);
      expect(word(tiles_at + UINT64(n) * tile + w * texel), mapped[n] ? source[n * words + w] : 0, "tile copied out", n * words + w);
    }
  for (UINT w = 0; w < 2 * words; w++)
    expect(word(buffer_at + w * texel), w < words ? source[w] : 0, "reserved buffer", w);
  readback->Unmap(0, nullptr);
  printf("%s: %u wrong words\n", failures ? "failed" : "passed", failures);
  return failures != 0;
}
