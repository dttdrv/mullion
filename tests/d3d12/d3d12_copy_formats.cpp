// contract: a footprint and a texture copy bits across a D3D10.1 reinterpret pair, one texel per block.
// "Block-compressed resource width and height must be 4 times the uncompressed resource width and height"
// (Microsoft Learn, ID3D12GraphicsCommandList::CopyResource, Reinterpret copy). D3D11.3 5.6.2: "but the raw value
// of memory happens to be equal". source coordinates use its format, destination coordinates use its own.
// formats come from the SDK; pitches come from GetCopyableFootprints. borders keep their bytes.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-copyresource
#include "d3d12_test.hpp"
#include "../copy_formats.hpp"
#include <algorithm>

int main(int, char **) {
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  for (auto &bc : copy_formats)
    for (auto &linear : copy_formats) {
      if (linear.depth || linear.unit != 1 || linear.bytes != bc.bytes ||
          (bc.unit == 1 ? strcmp(bc.family, "R9G9B9E5") || strcmp(linear.family, "R32")
                        : strcmp(linear.family, "R16G16B16A16") && strcmp(linear.family, "R32G32") &&
                              strcmp(linear.family, "R32G32B32A32")))
        continue;
      for (bool textured : {false, true})
        for (bool compressed : {false, true})
          for (bool upload : {false, true})
            for (bool boxed : {false, true})
              for (UINT count : {1u, 3u, 4u, 5u, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT / bc.bytes + 1}) {
                auto &tex = compressed ? bc : linear;
                auto &foot = compressed ? linear : bc;
                const UINT mip = count == 1 && !boxed ? 2 : 0;
                step("seed 17/201, %s texture, %s %s, %s, %s, %u units, mip %u", tex.name, foot.name,
                     textured ? "texture" : "footprint", upload ? "upload" : "readback", boxed ? "box" : "whole", count,
                     mip);
                D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                                         0,
                                         (count + 2) * tex.unit,
                                         5 * tex.unit,
                                         1,
                                         UINT16(mip + 1),
                                         tex.id,
                                         {1, 0}};
                const UINT wide = (std::max<UINT64>(1, desc.Width >> mip) + tex.unit - 1) / tex.unit,
                           high = (std::max(1u, desc.Height >> mip) + tex.unit - 1) / tex.unit;
                D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
                ComPtr<ID3D12Resource> texture;
                CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)));
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT native, cast;
                UINT rows;
                UINT64 row_bytes, native_bytes, cast_bytes;
                device->GetCopyableFootprints(&desc, mip, 1, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, &native, &rows,
                                              &row_bytes, &native_bytes);
                native_bytes += native.Offset;
                desc.Format = foot.id;
                desc.Width = wide * foot.unit;
                desc.Height = high * foot.unit;
                desc.MipLevels = 1;
                ComPtr<ID3D12Resource> other;
                if (textured)
                  CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&other)));
                device->GetCopyableFootprints(&desc, 0, 1, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, &cast, nullptr,
                                              nullptr, &cast_bytes);
                cast_bytes += cast.Offset;
                if (!expect(rows == high && row_bytes == wide * tex.bytes, "unexpected native footprint"))
                  return verdict();
                auto seed =
                    buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, native_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
                auto from = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, cast_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
                auto result = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, upload ? native_bytes : cast_bytes,
                                     D3D12_RESOURCE_STATE_COPY_DEST);
                if (!expect(seed && from && result, "copy buffers were not created"))
                  return verdict();
                auto value = [](UINT pattern, UINT y, UINT byte) {
                  return uint8_t(pattern + y * 29 + byte * 13 + byte / D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
                };
                uint8_t *bytes;
                CHECK(seed->Map(0, nullptr, (void **)&bytes));
                memset(bytes, 0, native_bytes);
                for (UINT y = 0; y < high; y++)
                  for (UINT x = 0; x < row_bytes; x++)
                    bytes[native.Offset + y * native.Footprint.RowPitch + x] = value(17, y, x);
                seed->Unmap(0, nullptr);
                CHECK(from->Map(0, nullptr, (void **)&bytes));
                memset(bytes, 0, cast_bytes);
                for (UINT y = 0; y < high; y++)
                  for (UINT x = 0; x < row_bytes; x++)
                    bytes[cast.Offset + y * cast.Footprint.RowPitch + x] = value(201, y, x);
                from->Unmap(0, nullptr);
                CHECK(allocator->Reset());
                CHECK(list->Reset(allocator.Get(), nullptr));
                D3D12_TEXTURE_COPY_LOCATION target{
                    texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = mip}};
                D3D12_TEXTURE_COPY_LOCATION initial{
                    seed.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = native}};
                list->CopyTextureRegion(&target, 0, 0, 0, &initial, nullptr);
                D3D12_TEXTURE_COPY_LOCATION source{
                    from.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = cast}};
                D3D12_TEXTURE_COPY_LOCATION destination{
                    result.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = cast}};
                D3D12_TEXTURE_COPY_LOCATION other_location{other.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
                if (textured) {
                  list->CopyTextureRegion(&other_location, 0, 0, 0, &source, nullptr);
                  if (upload) {
                    transition(list.Get(), other.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                               D3D12_RESOURCE_STATE_COPY_SOURCE);
                    source = other_location;
                  }
                }
                const UINT src_unit = upload ? foot.unit : tex.unit, dst_unit = upload ? tex.unit : foot.unit;
                const UINT src_y = high > 1 ? 1 : 0, dst_y = high > 1 ? 2 : 0, copy_rows = high > 1 ? 2 : 1;
                const D3D12_BOX box{
                    2 * src_unit, src_y * src_unit, 0, (count + 2) * src_unit, (src_y + copy_rows) * src_unit, 1};
                if (upload) {
                  if (textured && !boxed && !mip)
                    list->CopyResource(texture.Get(), other.Get());
                  else
                    list->CopyTextureRegion(&target, boxed ? dst_unit : 0, boxed ? dst_y * dst_unit : 0, 0, &source,
                                            boxed ? &box : nullptr);
                }
                transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
                if (!upload) {
                  if (textured && !boxed && !mip)
                    list->CopyResource(other.Get(), texture.Get());
                  else {
                    if (!textured)
                      list->CopyBufferRegion(result.Get(), 0, from.Get(), 0, cast_bytes);
                    list->CopyTextureRegion(textured ? &other_location : &destination, boxed ? dst_unit : 0,
                                            boxed ? dst_y * dst_unit : 0, 0, &target, boxed ? &box : nullptr);
                  }
                  if (textured) {
                    transition(list.Get(), other.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                               D3D12_RESOURCE_STATE_COPY_SOURCE);
                    list->CopyTextureRegion(&destination, 0, 0, 0, &other_location, nullptr);
                  }
                }
                D3D12_TEXTURE_COPY_LOCATION output{
                    result.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = native}};
                if (upload)
                  list->CopyTextureRegion(&output, 0, 0, 0, &target, nullptr);
                CHECK(submit(device.Get(), queue.Get(), list.Get()));
                auto &at = upload ? native : cast;
                CHECK(result->Map(0, nullptr, (void **)&bytes));
                bool same = true;
                for (UINT y = 0; y < high; y++)
                  for (UINT x = 0; x < wide; x++)
                    for (UINT b = 0; b < tex.bytes; b++) {
                      bool copied = !boxed || (x >= 1 && x < count + 1 && y >= dst_y && y < dst_y + copy_rows);
                      UINT source_row = boxed ? y - dst_y + src_y : y;
                      uint8_t want = copied ? value(upload ? 201 : 17, source_row, (boxed ? x + 1 : x) * tex.bytes + b)
                                            : value(upload ? 17 : 201, y, x * tex.bytes + b);
                      same &= bytes[at.Offset + y * at.Footprint.RowPitch + x * tex.bytes + b] == want;
                    }
                result->Unmap(0, nullptr);
                if (!expect(same, "copy changed the wrong units or lost bits"))
                  return verdict();
              }
    }
  return verdict();
}
