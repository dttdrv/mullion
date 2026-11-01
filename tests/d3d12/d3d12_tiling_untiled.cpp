// contract: ID3D12Device::GetResourceTiling "Gets info about how a tiled resource is broken into tiles"; a committed
// or placed resource has no tiles. Direct3D does not say what the call gives for one, but the call must not fault,
// and the reference here is vkd3d-proton, which gives a resource without tiles its zeroed sparse description: no
// tiles, a zero D3D12_PACKED_MIP_INFO and tile shape, and no subresource tilings (*pNumSubresourceTilings = 0).
// a committed 64x64 R8G8B8A8_UNORM texture of 1 and of 7 mips, a committed 3D texture and a committed buffer are
// asked with every output given, filled first with bytes no answer holds; the tilings array must be left alone.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-getresourcetiling
#include "d3d12_test.hpp"

int
main() {
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  struct Case {
    const char *name;
    D3D12_RESOURCE_DESC desc;
  } cases[] = {
      {"a 64x64 texture of 1 mip",
       {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 64, 64, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}}},
      {"a 64x64 texture of 7 mips",
       {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 64, 64, 1, 7, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}}},
      {"a 16x16x16 3D texture", {D3D12_RESOURCE_DIMENSION_TEXTURE3D, 0, 16, 16, 16, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}}},
      {"a buffer of 192 KB",
       {D3D12_RESOURCE_DIMENSION_BUFFER, 0, 3 << 16, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR}},
  };
  D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
  for (auto &c : cases) {
    step("GetResourceTiling of %s, committed", c.name);
    ComPtr<ID3D12Resource> resource;
    CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &c.desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                          IID_PPV_ARGS(&resource)));
    UINT total, count = 8;
    D3D12_PACKED_MIP_INFO packed;
    D3D12_TILE_SHAPE shape;
    D3D12_SUBRESOURCE_TILING tilings[8];
    memset(&total, 0xaa, sizeof(total));
    memset(&packed, 0xaa, sizeof(packed));
    memset(&shape, 0xaa, sizeof(shape));
    memset(tilings, 0xaa, sizeof(tilings));
    device->GetResourceTiling(resource.Get(), &total, &packed, &shape, &count, 0, tilings);
    expect(total == 0, "%u tiles, want 0", total);
    expect(!packed.NumStandardMips && !packed.NumPackedMips && !packed.NumTilesForPackedMips &&
               !packed.StartTileIndexInOverallResource,
           "packed mips {%u standard, %u packed, %u tiles, from tile %u}, want all 0", packed.NumStandardMips,
           packed.NumPackedMips, packed.NumTilesForPackedMips, packed.StartTileIndexInOverallResource);
    expect(!shape.WidthInTexels && !shape.HeightInTexels && !shape.DepthInTexels, "a tile shape of %ux%ux%u, want 0x0x0",
           shape.WidthInTexels, shape.HeightInTexels, shape.DepthInTexels);
    expect(count == 0, "%u subresource tilings, want 0", count);
    UINT changed = 0;
    for (auto &tiling : tilings) {
      D3D12_SUBRESOURCE_TILING untouched;
      memset(&untouched, 0xaa, sizeof(untouched));
      changed += memcmp(&tiling, &untouched, sizeof(tiling)) != 0;
    }
    expect(!changed, "%u of the subresource tilings were written", changed);
  }
  return verdict();
}
