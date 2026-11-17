// contract: a texture small enough may have 4KB alignment ("Textures with UNKNOWN layout without MSAA and without
// render-target nor depth-stencil flags may be created with 4KB Alignment", Microsoft Learn, D3D12_RESOURCE_DESC,
// Alignment; a 3D texture by one slice: "Windows uses the slice size to determine small alignment eligibility.
// DepthOrArraySize is ignored", vkd3d-proton's resource.c), and GetResourceAllocationInfo then gives it that
// alignment, as native drivers do (ibid.: 64KB "has not been observed on native implementations"). such textures
// are placed back to back in one heap, each where the size of the one before ends: "The HeapOffset must be a
// multiple of the resource's alignment" (CreatePlacedResource), and those offsets are no multiples of 64KB. each
// then holds what was copied into it once all were written, so none lies over another.
// the first texture and its offset are Marvel's Spider-Man 2's (dttdrv/mullion#204): a 3D R8_UNORM texture of 1 MB
// at 172032 of its heap.
#include "d3d12_test.hpp"
#include <limits>

int
main(int, char **) {
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  struct Case {
    D3D12_RESOURCE_DIMENSION dimension;
    DXGI_FORMAT format;
    UINT64 width;
    UINT height;
    UINT16 depth;
  };
  const Case cases[] = {
      {D3D12_RESOURCE_DIMENSION_TEXTURE3D, DXGI_FORMAT_R8_UNORM, 256, 256, 16},
      {D3D12_RESOURCE_DIMENSION_TEXTURE2D, DXGI_FORMAT_R8_UNORM, 256, 256, 1},
      {D3D12_RESOURCE_DIMENSION_TEXTURE2D, DXGI_FORMAT_R8G8B8A8_UNORM, 100, 60, 1},
      {D3D12_RESOURCE_DIMENSION_TEXTURE2D, DXGI_FORMAT_R32G32B32A32_FLOAT, 64, 64, 1},
      {D3D12_RESOURCE_DIMENSION_TEXTURE1D, DXGI_FORMAT_R32_FLOAT, 16384, 1, 1},
      {D3D12_RESOURCE_DIMENSION_TEXTURE3D, DXGI_FORMAT_R8_UNORM, 64, 64, 64},
      {D3D12_RESOURCE_DIMENSION_TEXTURE2D, DXGI_FORMAT_R8_UNORM, 3, 5, 1},
  };
  const UINT count = UINT(std::size(cases));
  D3D12_RESOURCE_DESC descs[count];
  UINT64 offsets[count], end = 42 * D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT, uneven = 0;
  step("the allocation of each texture at 4KB alignment");
  for (UINT i = 0; i < count; i++) {
    auto &c = cases[i];
    descs[i] = {c.dimension, D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT, c.width, c.height, c.depth, 1, c.format, {1, 0}};
    auto info = device->GetResourceAllocationInfo(0, 1, &descs[i]);
    if (!expect(info.SizeInBytes != std::numeric_limits<UINT64>::max() &&
                    info.Alignment == D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT && info.SizeInBytes &&
                    !(info.SizeInBytes % info.Alignment),
                "texture %u: %llu bytes at alignment %llu, want a multiple of 4096 at 4096", i, info.SizeInBytes,
                info.Alignment))
      return verdict();
    offsets[i] = end;
    uneven += !!(end % D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
    end += info.SizeInBytes;
  }
  expect(uneven > 1, "%llu offsets are no multiples of 64KB: the textures' sizes leave this case untried", uneven);

  step("the textures placed back to back");
  D3D12_HEAP_DESC heap_desc{(end + D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT - 1) /
                                D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT * D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,
                            {D3D12_HEAP_TYPE_DEFAULT}, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,
                            D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES};
  ComPtr<ID3D12Heap> heap;
  CHECK(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap)));
  ComPtr<ID3D12Resource> textures[count];
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[count];
  UINT rows[count];
  UINT64 row_bytes[count], bytes = 0;
  for (UINT i = 0; i < count; i++) {
    HRESULT hr = device->CreatePlacedResource(heap.Get(), offsets[i], &descs[i], D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&textures[i]));
    if (!expect(hr == S_OK, "texture %u at %llu of %llu: %08lx", i, offsets[i], heap_desc.SizeInBytes, hr))
      return verdict();
    UINT64 total;
    device->GetCopyableFootprints(&descs[i], 0, 1, bytes, &footprints[i], &rows[i], &row_bytes[i], &total);
    bytes = (bytes + total + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) / D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT *
            D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
  }
  // a texture past the heap's end is refused, the room each size has being the heap's too
  ComPtr<ID3D12Resource> past;
  expect(device->CreatePlacedResource(heap.Get(), heap_desc.SizeInBytes - D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT,
                                      &descs[0], D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                      IID_PPV_ARGS(&past)) == E_INVALIDARG,
         "a texture that ends past its heap was placed");

  step("each texture written, then each read");
  // byte `at` of row `row` of texture `i`: no two textures have a row alike
  auto byte = [](UINT i, UINT64 row, UINT64 at) { return UINT8(i * 37 + row * 13 + at * 7 + (at >> 8) + 1); };
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(upload && readback, "buffers of %llu bytes", bytes))
    return verdict();
  UINT8 *data;
  CHECK(upload->Map(0, nullptr, (void **)&data));
  for (UINT i = 0; i < count; i++)
    for (UINT64 row = 0; row < UINT64(rows[i]) * footprints[i].Footprint.Depth; row++)
      for (UINT64 at = 0; at < row_bytes[i]; at++)
        data[footprints[i].Offset + row * footprints[i].Footprint.RowPitch + at] = byte(i, row, at);
  upload->Unmap(0, nullptr);
  CHECK(forget(readback.Get()));
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  for (UINT i = 0; i < count; i++) {
    D3D12_TEXTURE_COPY_LOCATION to{textures[i].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
        from{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprints[i]}};
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  }
  for (UINT i = 0; i < count; i++) {
    transition(list.Get(), textures[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from{textures[i].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
        to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprints[i]}};
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  }
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  CHECK(readback->Map(0, nullptr, (void **)&data));
  for (UINT i = 0; i < count; i++) {
    UINT64 wrong = 0, first_row = 0, first_at = 0;
    for (UINT64 row = 0; row < UINT64(rows[i]) * footprints[i].Footprint.Depth; row++)
      for (UINT64 at = 0; at < row_bytes[i]; at++)
        if (data[footprints[i].Offset + row * footprints[i].Footprint.RowPitch + at] != byte(i, row, at) && !wrong++)
          first_row = row, first_at = at;
    expect(!wrong, "texture %u at %llu: %llu bytes are not what was copied into it, the first in row %llu at %llu", i,
           offsets[i], wrong, first_row, first_at);
  }
  readback->Unmap(0, nullptr);
  return verdict();
}
