// contract: a copy between textures copies bits (ID3D12GraphicsCommandList::CopyResource, Remarks). the two formats
// may differ within a type group ("Restrictions"), and a block-compressed texture copies to and from an uncompressed
// one of its blocks' bit width, a block for a texel, the compressed one four times as wide and high ("Reinterpret
// copy"). CopyResource copies every subresource; CopyTextureRegion copies a box and leaves the rest of the destination.
// the bytes are followed through the footprints the device gives for both textures, whose rows are then the same.
// multisampled textures of one sample count copy too, and "you can use a depth-stencil resource as either a source or
// a destination": a colour texture cleared in quarters goes to depth and from depth to colour again, and what that
// resolves to is the quarters.
#include "d3d12_test.hpp"

int
main(int, char **) {
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  struct Case {
    const char *name;
    DXGI_FORMAT from, to;
    UINT from_width, from_height, to_width, to_height;
    UINT16 mips, layers;
    bool volume, region;
  } cases[] = {
      {"float to uint", DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_UINT, 8, 4, 8, 4, 3, 2},
      {"unorm to sRGB", DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 8, 4, 8, 4, 3, 2},
      {"a volume", DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_UINT, 4, 4, 4, 4, 2, 4, true},
      {"depth to float", DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT, 8, 4, 8, 4, 1, 1},
      {"float to depth", DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_D32_FLOAT, 8, 4, 8, 4, 1, 1},
      {"texels to BC1 blocks", DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_BC1_UNORM, 4, 2, 16, 8, 3, 1},
      {"BC5 blocks to texels", DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_R32G32B32A32_UINT, 16, 8, 4, 2, 3, 1},
      {"a box, float to unorm", DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_UNORM, 4, 4, 4, 4, 1, 1, false, true},
      {"a box, the same format", DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, 4, 4, 4, 4, 1, 1, false, true},
      {"a box of texels to BC4 blocks", DXGI_FORMAT_R16G16B16A16_UINT, DXGI_FORMAT_BC4_UNORM, 4, 4, 16, 16, 1, 1, false, true},
      {"a box of BC3 blocks to texels", DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_R32G32B32A32_SINT, 16, 16, 4, 4, 1, 1, false, true},
      // what texture streaming does: compressed textures with levels smaller than a block, between a typeless
      // texture, a typed one and its sRGB twin
      {"BC7 typeless to sRGB", DXGI_FORMAT_BC7_TYPELESS, DXGI_FORMAT_BC7_UNORM_SRGB, 24, 24, 24, 24, 4, 2},
      {"BC3 to BC3", DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM, 24, 24, 24, 24, 4, 2},
      {"a box of BC1 blocks to its sRGB twin", DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM_SRGB, 16, 16, 16, 16, 1, 1, false, true},
      {"a box of BC5 blocks to BC5", DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_BC5_UNORM, 16, 16, 16, 16, 1, 1, false, true},
  };
  // the box of a region copy and where it goes, in units: texels, or blocks of a compressed format
  const UINT box[4] = {1, 1, 3, 3}, to[2] = {2, 0};
  // the bytes a texture starts with: `seed` tells the source's from the destination's
  auto value = [](UINT seed, UINT subresource, UINT row, UINT byte) {
    return UINT8(seed + subresource * 101 + row * 29 + byte * 13);
  };
  const UINT source = 17, destination = 201;

  // a texture with its footprints, and an upload buffer of its starting bytes
  struct Side {
    ComPtr<ID3D12Resource> texture, upload;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> at;
    std::vector<UINT> rows;
    std::vector<UINT64> row_sizes;
    UINT64 bytes;
  };
  unsigned failures = 0;
  for (auto &k : cases) {
    const UINT subresources = k.mips * (k.volume ? 1 : k.layers);
    auto side = [&](DXGI_FORMAT format, UINT width, UINT height, UINT seed, Side &s) {
      D3D12_RESOURCE_DESC desc{k.volume ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                               0, width, height, k.layers, k.mips, format, {1, 0}};
      if (format == DXGI_FORMAT_D32_FLOAT)
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
      D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
      if (FAILED(device->CreateCommittedResource(
              &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s.texture)
          )))
        return false;
      s.at.resize(subresources), s.rows.resize(subresources), s.row_sizes.resize(subresources);
      device->GetCopyableFootprints(&desc, 0, subresources, 0, s.at.data(), s.rows.data(), s.row_sizes.data(), &s.bytes);
      s.upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, s.bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
      UINT8 *bytes;
      if (!s.upload || FAILED(s.upload->Map(0, nullptr, (void **)&bytes)))
        return false;
      for (UINT i = 0; i < subresources; i++)
        for (UINT row = 0; row < s.rows[i] * s.at[i].Footprint.Depth; row++)
          for (UINT byte = 0; byte < s.row_sizes[i]; byte++)
            bytes[s.at[i].Offset + row * s.at[i].Footprint.RowPitch + byte] = value(seed, i, row, byte);
      s.upload->Unmap(0, nullptr);
      for (UINT i = 0; i < subresources; i++) {
        D3D12_TEXTURE_COPY_LOCATION src{s.upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = s.at[i]}};
        D3D12_TEXTURE_COPY_LOCATION dst{s.texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = i}};
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      }
      return true;
    };
    Side from, into;
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), nullptr));
    if (!side(k.from, k.from_width, k.from_height, source, from) || !side(k.to, k.to_width, k.to_height, destination, into)) {
      printf("failed: %s: no textures\n", k.name);
      return 1;
    }
    if (from.rows != into.rows || from.row_sizes != into.row_sizes) {
      printf("failed: %s: the two textures' rows differ\n", k.name);
      return 1;
    }
    // a unit's size in texels, and in bytes
    const UINT from_unit = k.from_height / from.rows[0], to_unit = k.to_height / into.rows[0];
    const UINT unit_bytes = into.row_sizes[0] / (k.to_width / to_unit);
    transition(list.Get(), from.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (k.region) {
      D3D12_TEXTURE_COPY_LOCATION src{from.texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      D3D12_TEXTURE_COPY_LOCATION dst{into.texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      D3D12_BOX texels{box[0] * from_unit, box[1] * from_unit, 0, box[2] * from_unit, box[3] * from_unit, 1};
      list->CopyTextureRegion(&dst, to[0] * to_unit, to[1] * to_unit, 0, &src, &texels);
    } else {
      list->CopyResource(into.texture.Get(), from.texture.Get());
    }
    transition(list.Get(), into.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, into.bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    for (UINT i = 0; i < subresources; i++) {
      D3D12_TEXTURE_COPY_LOCATION src{into.texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = i}};
      D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = into.at[i]}};
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    CHECK(submit(device.Get(), queue.Get(), list.Get()));

    const UINT8 *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    unsigned wrong = 0;
    for (UINT i = 0; i < subresources; i++)
      for (UINT row = 0; row < into.rows[i] * into.at[i].Footprint.Depth; row++)
        for (UINT byte = 0; byte < into.row_sizes[i]; byte++) {
          // a region copy leaves what is outside its box, and takes the rest from the source's box
          UINT column = byte / unit_bytes;
          bool copied = !k.region || (row >= to[1] && row < to[1] + box[3] - box[1] && column >= to[0] &&
                                      column < to[0] + box[2] - box[0]);
          UINT8 want = !copied    ? value(destination, i, row, byte)
                       : k.region ? value(source, i, row - to[1] + box[1],
                                          (column - to[0] + box[0]) * unit_bytes + byte % unit_bytes)
                                  : value(source, i, row, byte);
          UINT8 have = got[into.at[i].Offset + row * into.at[i].Footprint.RowPitch + byte];
          if (have != want && wrong++ < 3)
            printf("%s: subresource %u, row %u, byte %u is %u, want %u\n", k.name, i, row, byte, have, want);
        }
    readback->Unmap(0, nullptr);
    failures += wrong;
  }
  {
    const UINT width = 6, height = 5, samples = 4;
    const DXGI_FORMAT colour = DXGI_FORMAT_R32_FLOAT;
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, colour, {samples, 0}};
    auto texture = [&](DXGI_FORMAT format, UINT count, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
      D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
      ComPtr<ID3D12Resource> out;
      desc.Format = format, desc.SampleDesc.Count = count, desc.Flags = flags;
      device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&out));
      return out;
    };
    auto first = texture(colour, samples, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET),
         depth = texture(DXGI_FORMAT_D32_FLOAT, samples, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_COPY_DEST),
         second = texture(colour, samples, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST),
         resolved = texture(colour, 1, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_RESOLVE_DEST);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT at;
    UINT64 bytes;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &at, nullptr, nullptr, &bytes);
    auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)));
    if (!first || !depth || !second || !resolved || !readback) {
      printf("failed: no multisampled textures\n");
      return 1;
    }
    auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(first.Get(), nullptr, rtv);
    // a quarter's value, each its own, all of them depths
    auto value = [&](UINT x, UINT y) { return ((x >= width / 2) + 2 * (y >= height / 2) + 1) / 8.0f; };
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), nullptr));
    for (LONG x : {0l, LONG(width / 2)})
      for (LONG y : {0l, LONG(height / 2)}) {
        const float quarter[4] = {value(x, y)};
        D3D12_RECT rect{x, y, x ? LONG(width) : LONG(width / 2), y ? LONG(height) : LONG(height / 2)};
        list->ClearRenderTargetView(rtv, quarter, 1, &rect);
      }
    transition(list.Get(), first.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(depth.Get(), first.Get());
    transition(list.Get(), depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(second.Get(), depth.Get());
    transition(list.Get(), second.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    list->ResolveSubresource(resolved.Get(), 0, second.Get(), 0, colour);
    transition(list.Get(), resolved.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{resolved.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = at}};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    const char *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    unsigned wrong = 0;
    for (UINT y = 0; y < height; y++)
      for (UINT x = 0; x < width; x++) {
        float have;
        memcpy(&have, got + at.Offset + y * at.Footprint.RowPitch + x * sizeof(float), sizeof(float));
        if (have != value(x, y) && wrong++ < 3)
          printf("multisampled colour to depth to colour: texel %u,%u is %g, want %g\n", x, y, have, value(x, y));
      }
    readback->Unmap(0, nullptr);
    failures += wrong;
  }
  printf("%s: %u wrong bytes over %zu copies\n", failures ? "failed" : "passed", failures, std::size(cases) + 2);
  return failures != 0;
}
