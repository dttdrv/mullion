// contract: a Texture1D created with initial data holds that data, whatever its SysMemPitch and SysMemSlicePitch,
// which have no meaning for a 1D texture: 0, smaller than a row, or far larger than one.
// D3D11_SUBRESOURCE_DATA, SysMemPitch: "System-memory pitch is used only for 2D and 3D texture data as it is has no
// meaning for the other resource types." SysMemSlicePitch: "System-memory-slice pitch is only used for 3D texture data
// as it has no meaning for the other resource types."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_subresource_data
// ID3D11Device::CreateTexture1D, pInitialData: "A pointer to an array of D3D11_SUBRESOURCE_DATA structures that
// describe subresources for the 1D texture resource."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-createtexture1d
// every usage that takes initial data (DEFAULT, IMMUTABLE, DYNAMIC, STAGING) is created with widths of 1, 7 and 64,
// one level and a full chain, one slice and three, in integer formats of 2, 4, 12 and 16 bytes a texel (12, RGB32, where
// the device supports it for Texture1D), so the bytes read back are the bytes given. each texel byte starts as a value
// derived from its subresource and offset, never 0. each subresource is read back through a STAGING copy (or mapped,
// for STAGING) and every wrong byte is counted, with the first one's location. there are no shaders.
#include "d3d11_test.hpp"

int
main() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                          &context));
  const struct {
    DXGI_FORMAT format;
    UINT bytes;
  } formats[] = {{DXGI_FORMAT_R16_UINT, 2},
                 {DXGI_FORMAT_R8G8B8A8_UINT, 4},
                 {DXGI_FORMAT_R32G32B32_UINT, 12},
                 {DXGI_FORMAT_R32G32B32A32_UINT, 16}};
  const D3D11_USAGE usages[] = {D3D11_USAGE_DEFAULT, D3D11_USAGE_IMMUTABLE, D3D11_USAGE_DYNAMIC, D3D11_USAGE_STAGING};
  const char *usage_names[] = {"DEFAULT", "IMMUTABLE", "DYNAMIC", "STAGING"};
  const UINT pitches[] = {0, 1, 0xfffffff0u};
  for (auto [format, texel] : formats) {
    UINT support = 0;
    if (FAILED(device->CheckFormatSupport(format, &support)) || !(support & D3D11_FORMAT_SUPPORT_TEXTURE1D)) {
      printf("format %u has no Texture1D\n", format);
      continue;
    }
    for (auto usage : usages)
      for (UINT width : {1u, 7u, 64u})
        for (bool chain : {false, true})
          for (UINT array : {1u, 3u})
            for (UINT pitch : pitches) {
              // DYNAMIC textures have one level and one slice
              if (usage == D3D11_USAGE_DYNAMIC && (chain || array > 1))
                continue;
              UINT levels = 1;
              for (UINT w = width; chain && w > 1; w >>= 1)
                levels++;
              step("format %u, %s, width %u, %u levels, %u slices, pitches %u", format, usage_names[usage], width,
                   levels, array, pitch);
              D3D11_TEXTURE1D_DESC desc{width, levels, array, format, usage};
              desc.BindFlags = usage == D3D11_USAGE_STAGING ? 0 : D3D11_BIND_SHADER_RESOURCE;
              desc.CPUAccessFlags = usage == D3D11_USAGE_DYNAMIC   ? D3D11_CPU_ACCESS_WRITE
                                    : usage == D3D11_USAGE_STAGING ? D3D11_CPU_ACCESS_READ
                                                                   : 0;
              std::vector<std::vector<uint8_t>> bytes(levels * array);
              std::vector<D3D11_SUBRESOURCE_DATA> data(levels * array);
              for (UINT slice = 0; slice < array; slice++)
                for (UINT mip = 0; mip < levels; mip++) {
                  UINT sub = D3D11CalcSubresource(mip, slice, levels);
                  bytes[sub].resize(std::max(1u, width >> mip) * texel);
                  for (UINT i = 0; i < bytes[sub].size(); i++)
                    bytes[sub][i] = uint8_t(1 + (sub * 37 + i * 11) % 251);
                  data[sub] = {bytes[sub].data(), pitch, pitch};
                }
              ComPtr<ID3D11Texture1D> texture, readback;
              HRESULT hr = device->CreateTexture1D(&desc, data.data(), &texture);
              if (!expect(SUCCEEDED(hr), "CreateTexture1D: %#lx", hr))
                continue;
              if (usage == D3D11_USAGE_STAGING) {
                readback = texture;
              } else {
                D3D11_TEXTURE1D_DESC staging = desc;
                staging.Usage = D3D11_USAGE_STAGING;
                staging.BindFlags = 0;
                staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                CHECK(device->CreateTexture1D(&staging, nullptr, &readback));
                context->CopyResource(readback.Get(), texture.Get());
              }
              UINT wrong = 0;
              for (UINT sub = 0; sub < levels * array; sub++) {
                D3D11_MAPPED_SUBRESOURCE mapped;
                CHECK(context->Map(readback.Get(), sub, D3D11_MAP_READ, 0, &mapped));
                auto got = static_cast<const uint8_t *>(mapped.pData);
                for (UINT i = 0; i < bytes[sub].size(); i++)
                  if (got[i] != bytes[sub][i] && !wrong++)
                    printf("subresource %u, byte %u is %u, want %u\n", sub, i, got[i], bytes[sub][i]);
                context->Unmap(readback.Get(), sub);
              }
              expect(!wrong, "%u wrong bytes", wrong);
            }
  }
  return verdict();
}
