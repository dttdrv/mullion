// contract: a device from D3D10CreateDevice (feature level 10_0) copies into no depth stencil texture, with
// CopyResource or a whole-subresource CopySubresourceRegion, and copies out of one; a device of feature level 10_1
// copies both ways. "Direct3D 10 has the following limitations: You cannot use a depth-stencil resource as a
// destination", and "Direct3D 10.1 enables depth-stencil resources to be used as either a source or destination"
// (Microsoft Learn, ID3D10Device::CopyResource). Windows leaves the destination as it was through D3D10CreateDevice
// (Wine's d3d10core test_format_compatibility, R16_UNORM to D16_UNORM and R16_TYPELESS into a depth stencil one) and
// copies a depth stencil source. the destination starts apart from the source, and every texel of it is read.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d10/nf-d3d10-id3d10device-copyresource
#include "../d3d11/d3d11_test.hpp"
#include <d3d10_1.h>
#include <utility>

int
main() {
  auto create10 = reinterpret_cast<decltype(&D3D10CreateDevice)>(
      GetProcAddress(LoadLibraryA("d3d10.dll"), "D3D10CreateDevice")
  );
  auto create101 = reinterpret_cast<decltype(&D3D10CreateDevice1)>(
      GetProcAddress(LoadLibraryA("d3d10_1.dll"), "D3D10CreateDevice1")
  );
  if (!expect(create10 && create101, "D3D10CreateDevice or D3D10CreateDevice1 is unavailable"))
    return verdict();
  const UINT side = 4;
  uint16_t pattern[side * side];
  for (UINT i = 0; i < side * side; i++)
    pattern[i] = uint16_t(0x1001 * (i + 1));
  const uint16_t zeros[side * side]{};
  for (bool ten_one : {false, true}) {
    ComPtr<ID3D10Device> device;
    if (ten_one) {
      ComPtr<ID3D10Device1> device1;
      CHECK(create101(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, D3D10_FEATURE_LEVEL_10_1, D3D10_1_SDK_VERSION,
                      &device1));
      device = device1;
    } else {
      CHECK(create10(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, D3D10_SDK_VERSION, &device));
    }
    auto make = [&](DXGI_FORMAT format, UINT bind, D3D10_USAGE usage, const void *data) {
      D3D10_TEXTURE2D_DESC desc{side, side, 1, 1, format, {1, 0}, usage, bind,
                                usage == D3D10_USAGE_STAGING ? UINT(D3D10_CPU_ACCESS_READ) : 0u};
      D3D10_SUBRESOURCE_DATA initial{data, side * 2};
      ComPtr<ID3D10Texture2D> texture;
      device->CreateTexture2D(&desc, data ? &initial : nullptr, &texture);
      return texture;
    };
    auto read = [&](ID3D10Texture2D *texture) {
      D3D10_TEXTURE2D_DESC desc;
      texture->GetDesc(&desc);
      std::vector<uint16_t> texels;
      auto staging = make(desc.Format, 0, D3D10_USAGE_STAGING, nullptr);
      D3D10_MAPPED_TEXTURE2D mapped;
      if (!staging)
        return texels;
      device->CopyResource(staging.Get(), texture);
      if (FAILED(staging->Map(0, D3D10_MAP_READ, 0, &mapped)))
        return texels;
      for (UINT y = 0; y < side; y++)
        for (UINT x = 0; x < side; x++) {
          uint16_t texel;
          memcpy(&texel, static_cast<const char *>(mapped.pData) + y * mapped.RowPitch + x * 2, 2);
          texels.push_back(texel);
        }
      staging->Unmap(0);
      return texels;
    };
    auto check = [&](ID3D10Texture2D *texture, const uint16_t *want) {
      auto got = read(texture);
      if (!expect(got.size() == side * side, "the destination could not be read"))
        return;
      UINT wrong = 0;
      for (UINT i = 0; i < got.size(); i++)
        if (got[i] != want[i] && wrong++ < 4)
          expect(false, "texel (%u,%u) is 0x%x, want 0x%x", i % side, i / side, got[i], want[i]);
      expect(!wrong, "%u texels of %zu are wrong", wrong, got.size());
    };
    const char *level = ten_one ? "10_1" : "10_0";
    for (auto [what, format] : {std::pair{"R16_UNORM into D16_UNORM", DXGI_FORMAT_D16_UNORM},
                                std::pair{"R16_TYPELESS into R16_TYPELESS", DXGI_FORMAT_R16_TYPELESS}}) {
      auto src_format = format == DXGI_FORMAT_D16_UNORM ? DXGI_FORMAT_R16_UNORM : format;
      auto src = make(src_format, D3D10_BIND_SHADER_RESOURCE, D3D10_USAGE_DEFAULT, pattern);
      auto dst = make(format, D3D10_BIND_DEPTH_STENCIL, D3D10_USAGE_DEFAULT, nullptr);
      ComPtr<ID3D10DepthStencilView> dsv;
      D3D10_DEPTH_STENCIL_VIEW_DESC view{DXGI_FORMAT_D16_UNORM, D3D10_DSV_DIMENSION_TEXTURE2D};
      if (!expect(src && dst && SUCCEEDED(device->CreateDepthStencilView(dst.Get(), &view, &dsv)),
                  "%s, %s: the textures could not be made", level, what))
        continue;
      for (bool whole_region : {false, true}) {
        step("feature level %s, %s: %s", level, what, whole_region ? "CopySubresourceRegion of the whole" : "CopyResource");
        device->ClearDepthStencilView(dsv.Get(), D3D10_CLEAR_DEPTH, 0, 0);
        if (whole_region)
          device->CopySubresourceRegion(dst.Get(), 0, 0, 0, 0, src.Get(), 0, nullptr);
        else
          device->CopyResource(dst.Get(), src.Get());
        check(dst.Get(), ten_one ? pattern : zeros);
      }
    }
    step("feature level %s: D16_UNORM depth into R16_UNORM", level);
    auto depth = make(DXGI_FORMAT_D16_UNORM, D3D10_BIND_DEPTH_STENCIL, D3D10_USAGE_DEFAULT, pattern);
    auto color = make(DXGI_FORMAT_R16_UNORM, D3D10_BIND_SHADER_RESOURCE, D3D10_USAGE_DEFAULT, zeros);
    if (!expect(depth && color, "%s: the textures could not be made", level))
      continue;
    device->CopyResource(color.Get(), depth.Get());
    check(color.Get(), pattern);
  }
  return verdict();
}
