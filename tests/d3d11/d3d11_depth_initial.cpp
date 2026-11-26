// contract: a depth/stencil texture made with initial data holds that data: copied to staging and mapped, every
// texel's depth and stencil are the ones given. "pInitialData ... initializes the subresources" (Microsoft Learn,
// ID3D11Device::CreateTexture2D); "Resources which can be used as Depth/ Stencil cannot partipate in this operation as
// a destination; but they can as a source." (D3D11.3 5.6.2). D24_UNORM_S8_UINT's depth is 24 bits of UNORM and its
// stencil the top 8 (19.1.3.1); D32_FLOAT_S8X24_UINT's is a float and 8 bits of the next word. sizes cross rows of
// more than one tile and odd widths; values are hashed, a depth's every bit pattern of 24 bits read back as given.
// there are no shaders.
#include "d3d11_test.hpp"
#include <vector>

int
main() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                          &context));
  struct Size {
    UINT width, height;
  };
  for (DXGI_FORMAT format : {DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_D32_FLOAT_S8X24_UINT})
    for (Size size : {Size{1, 1}, Size{37, 5}, Size{320, 181}}) {
      step("format %u, %ux%u", format, size.width, size.height);
      bool d24 = format == DXGI_FORMAT_D24_UNORM_S8_UINT;
      UINT texel = d24 ? 4 : 8, pitch = size.width * texel + 16;
      std::vector<uint8_t> data(pitch * size.height);
      for (UINT y = 0; y < size.height; y++)
        for (UINT x = 0; x < size.width; x++) {
          uint32_t hash = (y * 7919 + x) * 2654435761u;
          auto at = data.data() + y * pitch + x * texel;
          if (d24) {
            uint32_t packed = (hash & 0xFFFFFF) | (hash >> 24 ^ y) << 24;
            memcpy(at, &packed, 4);
          } else {
            float depth = float(hash & 0xFFFFF) / float(0xFFFFF);
            uint32_t stencil = (hash >> 24 ^ x) & 0xFF;
            memcpy(at, &depth, 4);
            memcpy(at + 4, &stencil, 4);
          }
        }
      D3D11_TEXTURE2D_DESC desc{size.width, size.height, 1, 1, format, {1, 0}, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_DEPTH_STENCIL};
      D3D11_SUBRESOURCE_DATA initial{data.data(), pitch};
      ComPtr<ID3D11Texture2D> texture, staging;
      CHECK(device->CreateTexture2D(&desc, &initial, &texture));
      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      CHECK(device->CreateTexture2D(&desc, nullptr, &staging));
      context->CopyResource(staging.Get(), texture.Get());
      D3D11_MAPPED_SUBRESOURCE mapped;
      CHECK(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
      unsigned wrong = 0;
      for (UINT y = 0; y < size.height; y++)
        for (UINT x = 0; x < size.width; x++) {
          auto want = data.data() + y * pitch + x * texel;
          auto got = static_cast<const uint8_t *>(mapped.pData) + y * mapped.RowPitch + x * texel;
          uint32_t w0, g0, w1 = 0, g1 = 0;
          memcpy(&w0, want, 4);
          memcpy(&g0, got, 4);
          if (!d24) {
            memcpy(&w1, want + 4, 4);
            memcpy(&g1, got + 4, 4);
            w1 &= 0xFF;
            g1 &= 0xFF;
          }
          if ((w0 != g0 || w1 != g1) && wrong++ < 4)
            expect(false, "texel %u,%u is %08x %02x, want %08x %02x", x, y, g0, g1, w0, w1);
        }
      context->Unmap(staging.Get(), 0);
    }
  return verdict();
}
