// contract: copy footprints describe row-major data independently of the GPU's texture storage.
// GetCopyableFootprints: "the unpadded size in bytes of a row" differs from the aligned RowPitch; BaseOffset
// "is added to the Offset of each D3D12_PLACED_SUBRESOURCE_FOOTPRINT" (Microsoft Learn).
// Resource Heaps, Copy Operations specifies pitch/placement alignment; D3D11.3 19.1.3.1 defines component widths,
// and block compression uses 4x4 blocks. the SDK-derived format table supplies those public encodings.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-getcopyablefootprints
// https://microsoft.github.io/DirectX-Specs/d3d/ResourceHeaps.html
#include "d3d12_test.hpp"
#include "../copy_formats.hpp"
#include <algorithm>
#include <bit>
#include <limits>

int main(int, char **) {
  ComPtr<ID3D12Device8> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  auto aligned = [](UINT64 bytes, UINT64 alignment) { return (bytes + alignment - 1) / alignment * alignment; };
  for (const auto &format : copy_formats) {
    // depth/stencil plane footprints already have an independent passing conformance witness.
    if (format.depth || format.id == DXGI_FORMAT_R24G8_TYPELESS || format.id == DXGI_FORMAT_R32G8X24_TYPELESS)
      continue;
    for (auto dimension : {D3D12_RESOURCE_DIMENSION_TEXTURE1D, D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                           D3D12_RESOURCE_DIMENSION_TEXTURE3D}) {
      if (format.unit > 1 && dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D)
        continue;
      for (UINT units : {1u, 2u, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT / format.bytes - 1,
                         D3D12_TEXTURE_DATA_PITCH_ALIGNMENT / format.bytes,
                         D3D12_TEXTURE_DATA_PITCH_ALIGNMENT / format.bytes + 1}) {
        const UINT width = units * format.unit;
        const UINT height = dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? 1 : 5 * format.unit;
        const UINT16 depth_or_array = 3;
        const UINT16 mips = std::bit_width(std::max(width, height));
        const UINT arrays = dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : depth_or_array;
        const UINT subresources = mips * arrays;
        for (bool automatic : {false, true}) {
          D3D12_RESOURCE_DESC desc{dimension, 0, width, height, depth_or_array, UINT16(automatic ? 0 : mips),
                                   format.id, {1, 0}};
          D3D12_RESOURCE_DESC1 desc1{desc.Dimension, desc.Alignment, desc.Width, desc.Height,
                                    desc.DepthOrArraySize, desc.MipLevels, desc.Format, desc.SampleDesc,
                                    desc.Layout, desc.Flags};
          step("%s, dimension %u, %ux%u, %s mip chain, %u subresources", format.name, dimension, width,
               height, automatic ? "automatic" : "explicit", subresources);
          const UINT firsts[] = {0, UINT(mips - 1), UINT(mips), subresources - 1};
          for (UINT first : firsts) {
            if (first >= subresources)
              continue;
            for (UINT count : {1u, subresources - first}) {
              std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(count + 1);
              std::vector<UINT> rows(count + 1);
              std::vector<UINT64> row_sizes(count + 1);
              for (UINT64 base : {UINT64(0), UINT64(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT + 1),
                                  UINT64(std::numeric_limits<UINT>::max()) + 1})
                for (UINT outputs = 0; outputs < (1u << 4); outputs++)
                  for (bool api1 : {false, true}) {
                    std::fill(rows.begin(), rows.end(), ~0u);
                    std::fill(row_sizes.begin(), row_sizes.end(), ~UINT64(0));
                    memset(layouts.data(), 0xff, layouts.size() * sizeof(layouts[0]));
                    UINT64 total = ~UINT64(0);
                    auto out_layouts = outputs & 1 ? layouts.data() : nullptr;
                    auto out_rows = outputs & 2 ? rows.data() : nullptr;
                    auto out_sizes = outputs & 4 ? row_sizes.data() : nullptr;
                    auto out_total = outputs & 8 ? &total : nullptr;
                    if (api1)
                      device->GetCopyableFootprints1(&desc1, first, count, base, out_layouts, out_rows, out_sizes, out_total);
                    else
                      device->GetCopyableFootprints(&desc, first, count, base, out_layouts, out_rows, out_sizes, out_total);
                    UINT64 offset = 0, end = 0;
                    for (UINT i = 0; i < count; i++) {
                      const UINT mip = (first + i) % mips;
                      const UINT w = aligned(std::max(1u, width >> mip), format.unit);
                      const UINT h = aligned(std::max(1u, height >> mip), format.unit);
                      const UINT d = dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ?
                                     std::max(1u, UINT(depth_or_array) >> mip) : 1;
                      const UINT row_count = h / format.unit;
                      const UINT64 row_size = UINT64(w / format.unit) * format.bytes;
                      const UINT pitch = aligned(row_size, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
                      end = offset + UINT64(pitch) * (row_count * d - 1) + row_size;
                      bool same = true;
                      if (out_layouts) {
                        const auto &l = layouts[i];
                        same &= l.Offset == base + offset && l.Footprint.Format == format.id &&
                                l.Footprint.Width == w && l.Footprint.Height == h && l.Footprint.Depth == d &&
                                l.Footprint.RowPitch == pitch;
                      }
                      if (out_rows)
                        same &= rows[i] == row_count;
                      if (out_sizes)
                        same &= row_sizes[i] == row_size;
                      if (!expect(same, "api %u, first %u count %u base %llu outputs %#x, subresource %u",
                                  api1, first, count, base, outputs, first + i))
                        return verdict();
                      offset = aligned(end, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
                    }
                    if (!expect(!out_total || total == end, "api %u total %llu, want %llu", api1, total, end))
                      return verdict();
                    const auto *tail = reinterpret_cast<const UINT8 *>(&layouts[count]);
                    bool guarded = rows[count] == ~0u && row_sizes[count] == ~UINT64(0);
                    for (size_t i = 0; i < sizeof(layouts[count]); i++)
                      guarded &= tail[i] == UINT8(~0u);
                    if (!expect(guarded, "output arrays overwritten past count %u", count))
                      return verdict();
                  }
            }
          }
        }
      }
    }
  }
  return verdict();
}
