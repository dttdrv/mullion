// contract: a depth/stencil texture can be copied to staging and mapped without changing its depth or stencil.
// "Resources which can be used as Depth/ Stencil cannot partipate in this operation as a destination; but they can
// as a source." (D3D11.3 5.6.2). "Mapping means granting CPU access to the Subresource's storage or contents."
// (5.6.1). "The full extent of the resource view is always cleared." (5.2.3.1).
// ClearFlags "Identify the type of data to clear" (Microsoft Learn, ID3D11DeviceContext::ClearDepthStencilView):
// a stencil-only clear leaves depth unchanged, including when the format has no stencil component.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-cleardepthstencilview
// the format names give each component's width and order (19.1.3.1). UNORM endpoints are exactly 0 and 2^n - 1;
// FLOAT quarters are exact. unused bits and pitch padding have no expectation. formats come from the API header,
// decoded by their names, not the library's texel sizes or flags. dimensions cross the readback's threadgroup and
// allocation page boundaries; all mip levels and array slices are read, directly and through a staging copy.
// there are no shaders.
#include "d3d11_test.hpp"
#include <d3d10_1.h>
#include <algorithm>
#include <climits>
#include <utility>
#include "../depth_formats.hpp"

int
main() {
  static const unsigned char commands[] = {
#embed "../../src/dxmt/dxmt_command.cpp"
      , 0};
  struct Format {
    DXGI_FORMAT format;
    UINT depth_bits, stencil_bits, unused_bits;
    bool floating;
  };
  std::vector<Format> formats;
  const std::pair<DXGI_FORMAT, const char *> candidates[] = {DXGI_DEPTH_FORMATS};
  for (auto &[value, spelling] : candidates) {
    std::string name(spelling);
    Format format{value};
    if (!expect(sscanf(name.c_str(), "DXGI_FORMAT_D%u", &format.depth_bits) == 1,
                "invalid depth width: %s", name.c_str()))
      return verdict();
    format.floating = name.find("_FLOAT") != std::string::npos;
    if (auto stencil = name.find("_S"); stencil != std::string::npos)
      sscanf(name.c_str() + stencil, "_S%uX%u", &format.stencil_bits, &format.unused_bits);
    formats.push_back(format);
  }
  UINT group[3]{};
  const char *copy = strstr((const char *)commands, "DepthStencilBlitContext::copyFromTexture(");
  const char *threads = copy ? strstr(copy, "threadgroup_size = {") : nullptr;
  if (!expect(!formats.empty() && threads &&
                  sscanf(threads, "threadgroup_size = {%u, %u, %u}", &group[0], &group[1], &group[2]) == 3 &&
                  group[0] && group[1],
              "could not read depth formats or the readback threadgroup"))
    return verdict();
  const UINT seed = 0x5a17;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D10Device1> device10;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  step("seed %u: Direct3D 10 and 11 devices", seed);
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                          nullptr, &context));
  auto create10 = reinterpret_cast<decltype(&D3D10CreateDevice1)>(
      (void *)GetProcAddress(LoadLibraryA("d3d10_1.dll"), "D3D10CreateDevice1"));
  if (!expect(create10, "no D3D10CreateDevice1 in d3d10_1.dll"))
    return verdict();
  CHECK(create10(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, D3D10_FEATURE_LEVEL_10_1,
                 D3D10_1_SDK_VERSION, &device10));
  for (auto &format : formats) {
    const UINT texel_bytes = (format.depth_bits + format.stencil_bits + format.unused_bits) / CHAR_BIT;
    const UINT page_texels = DXMT_PAGE_SIZE / texel_bytes;
    const UINT widths[] = {1, std::max(1u, group[0] - 1), group[0], group[0] + 1,
                           page_texels - 1, page_texels, page_texels + 1};
    const UINT heights[] = {1, std::max(1u, group[1] - 1), group[1], group[1] + 1};
    for (UINT variant = 0; variant < std::size(widths); variant++)
      for (bool legacy : {false, true})
        for (bool writable : {false, true}) {
          step("seed %u, format %u, variant %u, D3D%u, writable %u: create", seed, format.format, variant,
               legacy ? 10 : 11, writable);
          D3D11_TEXTURE2D_DESC desc{widths[variant], heights[variant % std::size(heights)], 0,
                                   variant % 2 + 1, format.format, {1, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_DEPTH_STENCIL};
          ComPtr<ID3D11Texture2D> textures[3];
          ComPtr<ID3D10Texture2D> textures10[std::size(textures)];
          for (UINT i = 0; i < std::size(textures); i++) {
            if (i == 1) {
              if (legacy) {
                D3D10_TEXTURE2D_DESC desc10;
                textures10[0]->GetDesc(&desc10);
                desc.MipLevels = desc10.MipLevels;
              } else {
                textures[0]->GetDesc(&desc);
              }
              desc.Usage = D3D11_USAGE_STAGING;
              desc.BindFlags = 0;
              desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | (writable ? D3D11_CPU_ACCESS_WRITE : 0);
            }
            HRESULT hr;
            if (legacy) {
              D3D10_TEXTURE2D_DESC desc10{desc.Width, desc.Height, desc.MipLevels, desc.ArraySize, desc.Format,
                                         desc.SampleDesc, D3D10_USAGE(desc.Usage), desc.BindFlags,
                                         desc.CPUAccessFlags, desc.MiscFlags};
              hr = device10->CreateTexture2D(&desc10, nullptr, &textures10[i]);
            } else {
              hr = device->CreateTexture2D(&desc, nullptr, &textures[i]);
            }
            if (!expect(hr == S_OK && (legacy ? bool(textures10[i]) : bool(textures[i])),
                        "CreateTexture2D resource %u returned %08lx", i, hr))
              return verdict();
          }
          const UINT subresources = desc.MipLevels * desc.ArraySize;
          std::vector<ComPtr<ID3D11DepthStencilView>> views(subresources);
          std::vector<ComPtr<ID3D10DepthStencilView>> views10(subresources);
          for (UINT sub = 0; sub < subresources; sub++) {
            if (legacy) {
              D3D10_DEPTH_STENCIL_VIEW_DESC view10{format.format,
                  desc.ArraySize > 1 ? D3D10_DSV_DIMENSION_TEXTURE2DARRAY : D3D10_DSV_DIMENSION_TEXTURE2D};
              if (desc.ArraySize > 1)
                view10.Texture2DArray = {sub % desc.MipLevels, sub / desc.MipLevels, 1};
              else
                view10.Texture2D.MipSlice = sub;
              CHECK(device10->CreateDepthStencilView(textures10[0].Get(), &view10, &views10[sub]));
            } else {
              D3D11_DEPTH_STENCIL_VIEW_DESC view{format.format,
                  desc.ArraySize > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DARRAY : D3D11_DSV_DIMENSION_TEXTURE2D};
              if (desc.ArraySize > 1)
                view.Texture2DArray = {sub % desc.MipLevels, sub / desc.MipLevels, 1};
              else
                view.Texture2D.MipSlice = sub;
              CHECK(device->CreateDepthStencilView(textures[0].Get(), &view, &views[sub]));
            }
          }
          std::vector<float> depths(subresources);
          std::vector<uint8_t> stencils(subresources);
          const UINT clears[] = {D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, D3D11_CLEAR_DEPTH, D3D11_CLEAR_STENCIL, 0};
          for (UINT phase = 0; phase < std::size(clears); phase++) {
            step("seed %u, format %u, variant %u, D3D%u, writable %u, phase %u: clear and copy", seed,
                 format.format, variant, legacy ? 10 : 11, writable, phase);
            for (UINT sub = 0; sub < subresources; sub++) {
              float depth = format.floating ? float((seed + sub + phase) % 5) / 4 : float((sub + phase) % 2);
              uint8_t stencil = uint8_t(seed + sub + phase);
              if (clears[phase] & D3D11_CLEAR_DEPTH)
                depths[sub] = depth;
              if (clears[phase] & D3D11_CLEAR_STENCIL)
                stencils[sub] = stencil;
              if (legacy)
                device10->ClearDepthStencilView(views10[sub].Get(), clears[phase], depth, stencil);
              else
                context->ClearDepthStencilView(views[sub].Get(), clears[phase], depth, stencil);
            }
            if (legacy) {
              device10->CopyResource(textures10[1].Get(), textures10[0].Get());
              device10->CopyResource(textures10[2].Get(), textures10[1].Get());
            } else {
              context->CopyResource(textures[1].Get(), textures[0].Get());
              context->CopyResource(textures[2].Get(), textures[1].Get());
            }
            for (UINT i = 1; i < std::size(textures); i++)
              for (UINT sub = 0; sub < subresources; sub++) {
                step("seed %u, format %u, variant %u, D3D%u, writable %u, phase %u, resource %u, sub %u: Map", seed,
                     format.format, variant, legacy ? 10 : 11, writable, phase, i, sub);
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (legacy) {
                  D3D10_MAPPED_TEXTURE2D mapped10{};
                  CHECK(textures10[i]->Map(sub, D3D10_MAP_READ, 0, &mapped10));
                  mapped.pData = mapped10.pData;
                  mapped.RowPitch = mapped10.RowPitch;
                } else {
                  CHECK(context->Map(textures[i].Get(), sub, D3D11_MAP_READ, 0, &mapped));
                }
                UINT mip = sub % desc.MipLevels;
                UINT width = std::max(1u, desc.Width >> mip), height = std::max(1u, desc.Height >> mip);
                bool storage = expect(mapped.pData && mapped.RowPitch >= width * texel_bytes,
                                      "Map returned no data or a short row (%u)", mapped.RowPitch);
                for (UINT y = 0; storage && y < height; y++)
                  for (UINT x = 0; x < width; x++) {
                    const auto *pixel = (const uint8_t *)mapped.pData + y * mapped.RowPitch + x * texel_bytes;
                    bool equal;
                    double got_depth, want_depth = depths[sub];
                    if (format.floating) {
                      float depth;
                      memcpy(&depth, pixel, sizeof(depth));
                      got_depth = depth;
                      equal = depth == depths[sub];
                    } else {
                      uint32_t depth = 0;
                      memcpy(&depth, pixel, format.depth_bits / CHAR_BIT);
                      uint32_t maximum = uint32_t((uint64_t(1) << format.depth_bits) - 1);
                      got_depth = depth;
                      want_depth *= maximum;
                      equal = depth == uint32_t(depths[sub] * maximum);
                    }
                    if (format.stencil_bits)
                      equal &= pixel[format.depth_bits / CHAR_BIT] == stencils[sub];
                    if (!expect(equal, "wrong depth or stencil at %u,%u: depth %.9g, want %.9g; stencil %u, want %u",
                                x, y, got_depth, want_depth,
                                format.stencil_bits ? pixel[format.depth_bits / CHAR_BIT] : 0,
                                format.stencil_bits ? stencils[sub] : 0)) {
                      storage = false;
                      break;
                    }
                  }
                if (legacy)
                  textures10[i]->Unmap(sub);
                else
                  context->Unmap(textures[i].Get(), sub);
              }
          }
        }
  }
  return verdict();
}
