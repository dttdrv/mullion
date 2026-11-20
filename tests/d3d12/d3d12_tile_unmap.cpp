// contract: a reserved buffer's tile that was mapped and is not any more is a NULL tile like one never mapped.
// "Reading from NULL-mapped tiles treat that sampled value as zero. Writes to NULL-mapped tiles are discarded."
// (D3D12_TILED_RESOURCES_TIER_2, Microsoft Learn). a sequence of mappings, NULL mappings and copied NULL mappings over
// runs of tiles, each followed by a copy out of the whole buffer, a copy into all of it and a copy out again, in
// submissions of their own. a model keeps what each heap tile holds: a mapped tile reads its heap tile, which took the
// writes made while it was mapped and no other; a NULL tile reads zero. the sequence passes through no tile mapped.
#include "d3d12_test.hpp"
#include <algorithm>

int
main(int argc, char **argv) {
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
  if (options.TiledResourcesTier < D3D12_TILED_RESOURCES_TIER_2) {
    printf("failed: tiled resources tier %d\n", options.TiledResourcesTier);
    return 1;
  }
  const UINT tiles = 5, heap_tiles = 2 * tiles, rounds = 96, none = ~0u;
  const UINT64 tile = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES, bytes = tiles * tile;
  const UINT tile_words = tile / sizeof(UINT), words = tiles * tile_words;
  D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_BUFFER, 0, bytes, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                           D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
  ComPtr<ID3D12Resource> reserved, unmapped;
  CHECK(device->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&reserved)));
  CHECK(device->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&unmapped)));
  ComPtr<ID3D12Heap> heap;
  D3D12_HEAP_DESC heap_desc{heap_tiles * tile, {D3D12_HEAP_TYPE_DEFAULT}, 0, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS};
  CHECK(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap)));
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  UINT *in, *out;
  CHECK(upload->Map(0, nullptr, (void **)&in));
  CHECK(readback->Map(0, nullptr, (void **)&out));

  // the heap tile each tile is mapped to, and the first word each heap tile holds (0: never written, not checked)
  UINT at[tiles], held[heap_tiles] = {}, failures = 0, state = 1, empty = 0;
  std::fill_n(at, tiles, none);
  auto next = [&](UINT below) { return ((state = state * 1664525u + 1013904223u) >> 16) % below; };
  // a word carries the round that wrote it and its place in the buffer, so a tile read at another's place shows
  auto word = [&](UINT round, UINT i) { return round << 20 | i; };
  if (words > 1u << 20 || rounds >= 1u << 12) {
    printf("failed: a word does not hold a round and a place\n");
    return 1;
  }
  auto run = [&]() {
    HRESULT hr = submit(device.Get(), queue.Get(), list.Get());
    return FAILED(hr) || FAILED(hr = allocator->Reset()) ? hr : list->Reset(allocator.Get(), nullptr);
  };
  auto check = [&](UINT round, const char *when) {
    transition(list.Get(), reserved.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), 0, reserved.Get(), 0, bytes);
    transition(list.Get(), reserved.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    HRESULT hr = run();
    for (UINT i = 0; SUCCEEDED(hr) && i < words; i++) {
      UINT t = i / tile_words;
      if (at[t] != none && !held[at[t]])
        continue;
      UINT want = at[t] == none ? 0 : held[at[t]] + i % tile_words;
      if (out[i] != want && failures++ < 12)
        printf("round %u, %s: tile %u (%s) word %u: %#x, want %#x\n", round, when, t,
               at[t] == none ? "NULL" : "mapped", i % tile_words, out[i], want);
    }
    return hr;
  };
  for (UINT round = 1; round <= rounds; round++) {
    // a run of tiles: mapped to heap tiles no tile has, NULL, or given the mappings of a buffer that has none.
    // every eighth round takes all of them away
    UINT first = next(tiles), count = 1 + next(tiles - first), kind = next(3);
    if (round % 8 == 0)
      first = 0, count = tiles, kind = 1 + round / 8 % 2;
    D3D12_TILED_RESOURCE_COORDINATE origin{first};
    D3D12_TILE_REGION_SIZE region{count};
    std::fill_n(at + first, count, none);
    if (kind == 2) {
      queue->CopyTileMappings(reserved.Get(), &origin, unmapped.Get(), &origin, &region, D3D12_TILE_MAPPING_FLAG_NONE);
    } else if (kind == 1) {
      const D3D12_TILE_RANGE_FLAGS null = D3D12_TILE_RANGE_FLAG_NULL;
      queue->UpdateTileMappings(reserved.Get(), 1, &origin, &region, nullptr, 1, &null, nullptr, &count, D3D12_TILE_MAPPING_FLAG_NONE);
    } else {
      // a range of one tile for each, at any free heap tile
      UINT offsets[tiles], counts[tiles];
      for (UINT n = 0; n < count; n++) {
        do
          offsets[n] = next(heap_tiles);
        while (std::find(at, at + tiles, offsets[n]) != at + tiles);
        at[first + n] = offsets[n], counts[n] = 1;
      }
      queue->UpdateTileMappings(reserved.Get(), 1, &origin, &region, heap.Get(), count, nullptr, offsets, counts, D3D12_TILE_MAPPING_FLAG_NONE);
    }
    empty += std::count(at, at + tiles, none) == tiles;
    CHECK(check(round, "before the write"));
    for (UINT i = 0; i < words; i++)
      in[i] = word(round, i);
    for (UINT t = 0; t < tiles; t++)
      if (at[t] != none)
        held[at[t]] = word(round, t * tile_words);
    list->CopyBufferRegion(reserved.Get(), 0, upload.Get(), 0, bytes);
    CHECK(run());
    CHECK(check(round, "after the write"));
  }
  if (!empty && ++failures)
    printf("the sequence never left the buffer without a mapped tile\n");
  printf("%s: %u wrong words\n", failures ? "failed" : "passed", failures);
  return failures != 0;
}
