// contract: staging resources take every copy and update that other resources take.
// - UpdateSubresource writes a staging buffer and a box of a staging texture (it may not write only immutable and
//   dynamic resources), in order with the copies around it: a copy out before the update sees the bytes before.
// - copies between a block-compressed format and an uncompressed one of the block's size ("CopySubresourceRegion",
//   format conversion between BC1 and 64-bit formats, BC3 and 128-bit ones) carry a block as a texel, bit for bit,
//   whichever side is a staging texture, a default one, or both are staging; a region of blocks lands at the
//   destination's texel or block, and a mip smaller than a block takes its one block.
// - copies between staging textures of one compressed format carry the blocks named.
// - copies between default textures of two formats of one type group (ID3D11DeviceContext::CopyResource, Remarks:
//   "the formats must be identical or at least from the same type group") carry the bits, also where the two differ
//   in sRGB alone.
// every result is the bytes written, read back by Map.
#include "d3d11_test.hpp"
#include <algorithm>
#include <iterator>

int
main() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context));
  unsigned failures = 0, compared = 0;
  auto expect = [&](bool ok, const char *what, long long got, long long want) {
    compared++;
    if (!ok && failures++ < 16)
      printf("%s: %#llx, want %#llx\n", what, got, want);
  };
  // a texture of a usage, its mips filled from `data` (rows of `pitch` bytes) when given
  auto texture = [&](UINT width, UINT height, UINT mips, DXGI_FORMAT format, D3D11_USAGE usage, const void *data = nullptr, UINT pitch = 0) {
    D3D11_TEXTURE2D_DESC desc{width, height, mips, 1, format, {1, 0}, usage,
                              usage == D3D11_USAGE_STAGING ? 0u : D3D11_BIND_SHADER_RESOURCE,
                              usage == D3D11_USAGE_STAGING ? D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE : 0u};
    D3D11_SUBRESOURCE_DATA init{data, pitch};
    ComPtr<ID3D11Texture2D> out;
    device->CreateTexture2D(&desc, data ? &init : nullptr, &out);
    return out;
  };
  // the bytes of `rows` rows of `row_bytes` of a subresource, through a staging copy unless it is staging itself
  auto bytes_of = [&](ID3D11Texture2D *of, UINT subresource, UINT rows, UINT row_bytes, std::vector<uint8_t> &bytes) {
    D3D11_TEXTURE2D_DESC desc;
    of->GetDesc(&desc);
    ComPtr<ID3D11Texture2D> staging = of;
    if (desc.Usage != D3D11_USAGE_STAGING) {
      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      CHECK(device->CreateTexture2D(&desc, nullptr, &staging));
      context->CopyResource(staging.Get(), of);
    }
    D3D11_MAPPED_SUBRESOURCE mapped;
    CHECK(context->Map(staging.Get(), subresource, D3D11_MAP_READ, 0, &mapped));
    bytes.resize(rows * row_bytes);
    for (UINT row = 0; row < rows; row++)
      memcpy(bytes.data() + row * row_bytes, (const uint8_t *)mapped.pData + row * mapped.RowPitch, row_bytes);
    context->Unmap(staging.Get(), subresource);
    return 0;
  };
  // byte `i` of the content numbered `seed`: no two near bytes alike, and no two contents
  auto byte = [](UINT seed, UINT i) { return uint8_t(seed * 37 + i * 11 + (i >> 3)); };
  auto content = [&](UINT seed, UINT size) {
    std::vector<uint8_t> bytes(size);
    for (UINT i = 0; i < size; i++)
      bytes[i] = byte(seed, i);
    return bytes;
  };

  // updates: a staging buffer, part of it, then a staging texture's box
  {
    const UINT size = 64, from = 20, to = 44;
    D3D11_BUFFER_DESC desc{size, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE};
    ComPtr<ID3D11Buffer> staging, before;
    CHECK(device->CreateBuffer(&desc, nullptr, &staging));
    CHECK(device->CreateBuffer(&desc, nullptr, &before));
    auto whole = content(1, size), part = content(2, to - from);
    context->UpdateSubresource(staging.Get(), 0, nullptr, whole.data(), 0, 0);
    // a copy out here holds the first update only
    context->CopyResource(before.Get(), staging.Get());
    const D3D11_BOX box{from, 0, 0, to, 1, 1};
    context->UpdateSubresource(staging.Get(), 0, &box, part.data(), 0, 0);
    D3D11_MAPPED_SUBRESOURCE mapped;
    CHECK(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    for (UINT i = 0; i < size; i++) {
      uint8_t want = i >= from && i < to ? part[i - from] : whole[i];
      expect(((const uint8_t *)mapped.pData)[i] == want, "an updated staging buffer's byte", ((const uint8_t *)mapped.pData)[i], want);
    }
    context->Unmap(staging.Get(), 0);
    CHECK(context->Map(before.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    expect(!memcmp(mapped.pData, whole.data(), size), "the copy made before the second update", 0, 1);
    context->Unmap(before.Get(), 0);

    const UINT width = 8, height = 6, texel = 4;
    auto target = texture(width, height, 1, DXGI_FORMAT_R8G8B8A8_UINT, D3D11_USAGE_STAGING);
    auto first = content(3, width * height * texel);
    context->UpdateSubresource(target.Get(), 0, nullptr, first.data(), width * texel, 0);
    // a box, from rows wider apart than the box
    const D3D11_BOX rect{2, 1, 0, 7, 5, 1};
    const UINT box_width = rect.right - rect.left, box_height = rect.bottom - rect.top, pitch = box_width * texel + 12;
    auto second = content(4, box_height * pitch);
    context->UpdateSubresource(target.Get(), 0, &rect, second.data(), pitch, 0);
    std::vector<uint8_t> got;
    if (bytes_of(target.Get(), 0, height, width * texel, got))
      return 1;
    for (UINT y = 0; y < height; y++)
      for (UINT x = 0; x < width * texel; x++) {
        bool inside = y >= rect.top && y < rect.bottom && x >= rect.left * texel && x < rect.right * texel;
        uint8_t want = inside ? second[(y - rect.top) * pitch + x - rect.left * texel] : first[y * width * texel + x];
        expect(got[y * width * texel + x] == want, "an updated staging texture's byte", got[y * width * texel + x], want);
      }
  }

  // copies between a compressed format and the uncompressed one of its block's size, and within a compressed one
  struct Pair {
    const char *name;
    DXGI_FORMAT compressed, plain;
    UINT block; // bytes
  };
  const Pair pairs[] = {
      {"BC1 and R16G16B16A16_UINT", DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_R16G16B16A16_UINT, 8},
      {"BC3 and R32G32B32A32_UINT", DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_R32G32B32A32_UINT, 16},
  };
  // 6 by 5 blocks; a region of 3 by 2 blocks from block 2,1, placed at block 1,2
  const UINT blocks_wide = 6, blocks_high = 5;
  const UINT region[4] = {2, 1, 3, 2}, placed[2] = {1, 2};
  const D3D11_USAGE usages[] = {D3D11_USAGE_DEFAULT, D3D11_USAGE_STAGING};
  UINT seed = 10;
  for (auto &pair : pairs)
    for (bool to_compressed : {false, true})
      for (auto source_usage : usages)
        for (auto destination_usage : usages)
          // same: the copy stays in the compressed format, which is tested where staging has a part
          for (bool same : {false, true}) {
            if (same && source_usage == D3D11_USAGE_DEFAULT && destination_usage == D3D11_USAGE_DEFAULT)
              continue;
            if (same && !to_compressed)
              continue;
            auto source_format = same || !to_compressed ? pair.compressed : pair.plain;
            auto destination_format = same || to_compressed ? pair.compressed : pair.plain;
            // texels a block side has in each format
            UINT source_unit = source_format == pair.compressed ? 4 : 1, destination_unit = destination_format == pair.compressed ? 4 : 1;
            const UINT row_bytes = blocks_wide * pair.block, total = blocks_high * row_bytes;
            auto source_bytes = content(seed++, total), destination_bytes = content(seed++, total);
            auto make = [&](DXGI_FORMAT format, UINT unit, D3D11_USAGE usage, const std::vector<uint8_t> &bytes) {
              auto made = texture(blocks_wide * unit, blocks_high * unit, 1, format, usage, usage == D3D11_USAGE_STAGING ? nullptr : bytes.data(), row_bytes);
              if (made && usage == D3D11_USAGE_STAGING)
                context->UpdateSubresource(made.Get(), 0, nullptr, bytes.data(), row_bytes, 0);
              return made;
            };
            auto source = make(source_format, source_unit, source_usage, source_bytes),
                 destination = make(destination_format, destination_unit, destination_usage, destination_bytes);
            if (!source || !destination) {
              printf("failed: textures of %s\n", pair.name);
              return 1;
            }
            const D3D11_BOX box{region[0] * source_unit, region[1] * source_unit, 0,
                                (region[0] + region[2]) * source_unit, (region[1] + region[3]) * source_unit, 1};
            context->CopySubresourceRegion(
                destination.Get(), 0, placed[0] * destination_unit, placed[1] * destination_unit, 0, source.Get(), 0, &box
            );
            std::vector<uint8_t> got;
            if (bytes_of(destination.Get(), 0, blocks_high, row_bytes, got))
              return 1;
            unsigned wrong = 0;
            for (UINT y = 0; y < blocks_high; y++)
              for (UINT x = 0; x < blocks_wide; x++) {
                bool inside = x >= placed[0] && x < placed[0] + region[2] && y >= placed[1] && y < placed[1] + region[3];
                const uint8_t *want = inside ? &source_bytes[(y - placed[1] + region[1]) * row_bytes + (x - placed[0] + region[0]) * pair.block]
                                             : &destination_bytes[y * row_bytes + x * pair.block];
                wrong += memcmp(&got[y * row_bytes + x * pair.block], want, pair.block) != 0;
              }
            char what[160];
            snprintf(what, sizeof(what), "wrong blocks of %s, %s%s, from a %s texture to a %s one", pair.name,
                     same ? "compressed to compressed" : to_compressed ? "to compressed" : "from compressed", "",
                     source_usage == D3D11_USAGE_STAGING ? "staging" : "default",
                     destination_usage == D3D11_USAGE_STAGING ? "staging" : "default");
            expect(!wrong, what, wrong, 0);
          }

  // a compressed texture's last mips are smaller than a block and hold one: a whole copy carries it through
  // staging both ways
  for (auto &pair : pairs) {
    const UINT side = 8, mips = 4; // 8, 4, 2, 1 texels: 2 by 2 blocks, then one block each
    std::vector<uint8_t> mip_bytes[mips];
    auto up = texture(side, side, mips, pair.compressed, D3D11_USAGE_STAGING), down = texture(side, side, mips, pair.compressed, D3D11_USAGE_STAGING);
    auto on_device = texture(side, side, mips, pair.compressed, D3D11_USAGE_DEFAULT);
    if (!up || !down || !on_device) {
      printf("failed: mipmapped textures of %s\n", pair.name);
      return 1;
    }
    for (UINT mip = 0; mip < mips; mip++) {
      UINT blocks = std::max(side >> mip, 4u) / 4;
      mip_bytes[mip] = content(seed++, blocks * blocks * pair.block);
      context->UpdateSubresource(up.Get(), mip, nullptr, mip_bytes[mip].data(), blocks * pair.block, 0);
    }
    context->CopyResource(on_device.Get(), up.Get());
    context->CopyResource(down.Get(), on_device.Get());
    for (UINT mip = 0; mip < mips; mip++) {
      UINT blocks = std::max(side >> mip, 4u) / 4;
      std::vector<uint8_t> got;
      if (bytes_of(down.Get(), mip, blocks, blocks * pair.block, got))
        return 1;
      expect(got == mip_bytes[mip], "a compressed mip through a default texture and back", mip, mip);
    }
  }
  // an update of a box of blocks of a compressed staging texture
  for (auto &pair : pairs) {
    const UINT row_bytes = blocks_wide * pair.block;
    auto before = content(seed++, blocks_high * row_bytes), blocks = content(seed++, region[3] * region[2] * pair.block);
    auto target = texture(blocks_wide * 4, blocks_high * 4, 1, pair.compressed, D3D11_USAGE_STAGING);
    context->UpdateSubresource(target.Get(), 0, nullptr, before.data(), row_bytes, 0);
    const D3D11_BOX box{region[0] * 4, region[1] * 4, 0, (region[0] + region[2]) * 4, (region[1] + region[3]) * 4, 1};
    context->UpdateSubresource(target.Get(), 0, &box, blocks.data(), region[2] * pair.block, 0);
    std::vector<uint8_t> got;
    if (bytes_of(target.Get(), 0, blocks_high, row_bytes, got))
      return 1;
    unsigned wrong = 0;
    for (UINT y = 0; y < blocks_high; y++)
      for (UINT x = 0; x < blocks_wide; x++) {
        bool inside = x >= region[0] && x < region[0] + region[2] && y >= region[1] && y < region[1] + region[3];
        const uint8_t *want = inside ? &blocks[((y - region[1]) * region[2] + x - region[0]) * pair.block]
                                     : &before[y * row_bytes + x * pair.block];
        wrong += memcmp(&got[y * row_bytes + x * pair.block], want, pair.block) != 0;
      }
    expect(!wrong, "wrong blocks after an update of a box of a compressed staging texture", wrong, 0);
  }
  // default textures of one type group
  for (DXGI_FORMAT to : {DXGI_FORMAT_R8G8B8A8_UINT, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB}) {
    const UINT side = 4, row_bytes = side * 4;
    auto bytes = content(seed++, side * row_bytes);
    auto source = texture(side, side, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_USAGE_DEFAULT, bytes.data(), row_bytes);
    auto target = texture(side, side, 1, to, D3D11_USAGE_DEFAULT);
    context->CopyResource(target.Get(), source.Get());
    std::vector<uint8_t> got;
    if (bytes_of(target.Get(), 0, side, row_bytes, got))
      return 1;
    expect(got == bytes, "a copy between default textures of one type group, to format", to, to);
  }
  printf("%s: %u wrong of %u comparisons\n", failures ? "failed" : "passed", failures, compared);
  return failures != 0;
}
