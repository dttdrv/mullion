// contract: a nonempty box copies its bytes to the destination offset, including between texture dimensions;
// an empty box leaves every byte alone. "This function allows sub-region copying of data from one Subresource
// to another." (D3D11.3 5.6.2, D3D11_3_FunctionalSpec.htm). "An empty box results in a no-op."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-copysubresourceregion
// Wine's d3d11.c test_copy_subresource_region_1d and _3d record Windows copying 1D rows to 2D and 2D planes to 3D;
// Learn's "Must be the same type" conflicts with that witness. only boxes that fit both subresources are used.
// seeded boxes exercise each texture dimension from the API enum, both staging directions, mips, slices and
// immediate/deferred contexts. every destination word, including outside the box, and every source is checked.
// initial 1D staging data is read before any region copy, with ignored pitches: "System-memory pitch is used
// only for 2D and 3D texture data" (Microsoft Learn, D3D11_SUBRESOURCE_DATA).
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_subresource_data
// there are no shaders. Map pitches locate words; padding is never compared.
#include "d3d11_test.hpp"
#include <d3d11_1.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <random>

int
main(int argc, char **argv) {
  const UINT seed = argc > 1 ? UINT(strtoul(argv[1], nullptr, 0)) : std::mt19937::default_seed;
  const UINT word_bytes = sizeof(UINT);
  std::mt19937 random(seed);
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  step("seed %u: device and deferred context", seed);
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                         &device, nullptr, &immediate));
  CHECK(device->CreateDeferredContext(0, &deferred));
  ComPtr<ID3D11DeviceContext1> immediate1, deferred1;
  CHECK(immediate.As(&immediate1));
  CHECK(deferred.As(&deferred1));
  auto submit = [&](bool listed) -> HRESULT {
    if (!listed)
      return S_OK;
    ComPtr<ID3D11CommandList> commands;
    HRESULT hr = deferred->FinishCommandList(FALSE, &commands);
    if (SUCCEEDED(hr))
      immediate->ExecuteCommandList(commands.Get(), FALSE);
    return hr;
  };

  struct Image {
    ComPtr<ID3D11Resource> resource;
    std::vector<std::array<UINT, 3>> extents;
    std::vector<std::vector<UINT>> words;
    UINT mips, layers;
  };
  UINT serial = 0;
  auto make = [&](UINT dimension, std::array<UINT, 3> extent, UINT layers, bool staging, Image &image) -> HRESULT {
    if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D)
      extent[1] = 1;
    if (dimension != D3D11_RESOURCE_DIMENSION_TEXTURE3D)
      extent[2] = 1;
    else
      layers = 1;
    UINT mips = 1;
    for (UINT size = *std::max_element(extent.begin(), extent.end()); size > 1; size >>= 1)
      mips++;
    image.mips = mips;
    image.layers = layers;
    image.extents.resize(mips * layers);
    image.words.resize(mips * layers);
    std::vector<D3D11_SUBRESOURCE_DATA> data(mips * layers);
    for (UINT layer = 0; layer < layers; layer++)
      for (UINT mip = 0; mip < mips; mip++) {
        UINT sub = D3D11CalcSubresource(mip, layer, mips);
        auto &e = image.extents[sub];
        for (UINT axis = 0; axis < e.size(); axis++)
          e[axis] = std::max(1u, extent[axis] >> mip);
        auto &words = image.words[sub];
        words.resize(e[0] * e[1] * e[2]);
        for (auto &word : words)
          word = ++serial;
        data[sub] = {words.data(), e[0] * word_bytes, e[0] * e[1] * word_bytes};
        if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D && staging) {
          data[sub].SysMemPitch = sub % 2 ? random() : 0;
          data[sub].SysMemSlicePitch = random();
        }
      }
    const auto usage = staging ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
    const UINT access = staging ? D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE : 0;
    switch (dimension) {
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
      D3D11_TEXTURE1D_DESC desc{extent[0], mips, layers, DXGI_FORMAT_R32_UINT, usage, 0, access};
      ComPtr<ID3D11Texture1D> texture;
      HRESULT hr = device->CreateTexture1D(&desc, data.data(), &texture);
      image.resource = texture;
      return hr;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
      D3D11_TEXTURE2D_DESC desc{extent[0], extent[1], mips, layers, DXGI_FORMAT_R32_UINT, {1, 0}, usage, 0, access};
      ComPtr<ID3D11Texture2D> texture;
      HRESULT hr = device->CreateTexture2D(&desc, data.data(), &texture);
      image.resource = texture;
      return hr;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
      D3D11_TEXTURE3D_DESC desc{extent[0], extent[1], extent[2], mips, DXGI_FORMAT_R32_UINT, usage, 0, access};
      ComPtr<ID3D11Texture3D> texture;
      HRESULT hr = device->CreateTexture3D(&desc, data.data(), &texture);
      image.resource = texture;
      return hr;
    }
    default:
      return E_INVALIDARG;
    }
  };
  auto check = [&](Image &image, Image &readback) -> int {
    immediate->CopyResource(readback.resource.Get(), image.resource.Get());
    for (UINT sub = 0; sub < image.words.size(); sub++) {
      D3D11_MAPPED_SUBRESOURCE mapped{};
      HRESULT hr = immediate->Map(readback.resource.Get(), sub, D3D11_MAP_READ, 0, &mapped);
      if (!expect(SUCCEEDED(hr), "Map subresource %u returned %#lx", sub, hr))
        return 1;
      auto e = image.extents[sub];
      for (UINT z = 0; z < e[2]; z++)
        for (UINT y = 0; y < e[1]; y++)
          for (UINT x = 0; x < e[0]; x++) {
            UINT got;
            memcpy(&got, (const char *)mapped.pData + z * mapped.DepthPitch + y * mapped.RowPitch + x * sizeof(UINT),
                   sizeof(got));
            UINT want = image.words[sub][(z * e[1] + y) * e[0] + x];
            if (!expect(got == want, "subresource %u texel (%u,%u,%u): %#x, want %#x", sub, x, y, z, got, want)) {
              immediate->Unmap(readback.resource.Get(), sub);
              return 1;
            }
          }
      immediate->Unmap(readback.resource.Get(), sub);
    }
    return 0;
  };
  const UINT page_words = DXMT_PAGE_SIZE / sizeof(UINT);
  const UINT widths[] = {1, page_words - 1, page_words, page_words + 1};
  UINT variant = 0;
  for (UINT src_dimension = D3D11_RESOURCE_DIMENSION_TEXTURE1D;
       src_dimension <= D3D11_RESOURCE_DIMENSION_TEXTURE3D; src_dimension++)
    for (UINT dst_dimension = D3D11_RESOURCE_DIMENSION_TEXTURE1D;
         dst_dimension <= D3D11_RESOURCE_DIMENSION_TEXTURE3D; dst_dimension++)
      for (UINT width : widths)
        for (UINT staging = 0; staging < 4; staging++)
          for (bool listed : {false, true}) {
            variant++;
            auto context = listed ? deferred1.Get() : immediate1.Get();
            const std::array<UINT, 3> extent{width, width == 1 ? 1u : 3u + random() % 5,
                                          width == 1 ? 1u : 2u + random() % 3};
            Image src, dst, src_readback, dst_readback;
            const UINT src_layers = width == 1 ? 1 : 3, dst_layers = width == 1 ? 1 : 2;
            step("seed %u variant %u: create dimensions %u -> %u, width %u, staging %u, listed %u",
                 seed, variant, src_dimension, dst_dimension, width, staging, listed);
            CHECK(make(src_dimension, extent, src_layers, staging & 1, src));
            auto dst_extent = extent;
            dst_extent[0] += width != 1;
            CHECK(make(dst_dimension, dst_extent, dst_layers, staging & 2, dst));
            CHECK(make(src_dimension, extent, src_layers, true, src_readback));
            CHECK(make(dst_dimension, dst_extent, dst_layers, true, dst_readback));
            if (check(src, src_readback) || check(dst, dst_readback))
              return verdict();
            for (UINT mip : {0u, src.mips - 1})
              for (UINT mode = 0; mode < 9; mode++) {
                UINT src_sub = D3D11CalcSubresource(mip, random() % src.layers, src.mips);
                UINT dst_mip = random() % dst.mips, dst_layer = random() % dst.layers;
                UINT dst_sub = D3D11CalcSubresource(dst_mip, dst_layer, dst.mips);
                auto s = src.extents[src_sub], d = dst.extents[dst_sub];
                std::array<UINT, 3> begin{}, end{}, placed{};
                bool whole = mode == 0;
                for (UINT axis = 0; axis < s.size(); axis++) {
                  whole &= s[axis] <= d[axis];
                  UINT count = mode == 1 ? std::min(s[axis], d[axis])
                                         : 1 + random() % std::min(s[axis], d[axis]);
                  begin[axis] = mode == 1 ? s[axis] - count : random() % (s[axis] - count + 1);
                  placed[axis] = mode == 1 ? d[axis] - count : random() % (d[axis] - count + 1);
                  end[axis] = begin[axis] + count;
                }
                if (whole) {
                  begin = {};
                  end = s;
                  for (UINT axis = 0; axis < s.size(); axis++)
                    placed[axis] = random() % (d[axis] - s[axis] + 1);
                }
                if (mode >= 3) {
                  UINT axis = (mode - 3) / 2;
                  if ((mode - 3) % 2)
                    std::swap(begin[axis], end[axis]);
                  else
                    end[axis] = begin[axis];
                }
                D3D11_BOX box{begin[0], begin[1], begin[2], end[0], end[1], end[2]};
                step("seed %u variant %u mip %u mode %u: sub %u -> %u, box (%u,%u,%u)-(%u,%u,%u), at (%u,%u,%u)",
                     seed, variant, mip, mode, src_sub, dst_sub, box.left, box.top, box.front, box.right, box.bottom,
                     box.back, placed[0], placed[1], placed[2]);
                if (mode % 2)
                  context->CopySubresourceRegion1(dst.resource.Get(), dst_sub, placed[0], placed[1], placed[2],
                                                  src.resource.Get(), src_sub, whole ? nullptr : &box, 0);
                else
                  context->CopySubresourceRegion(dst.resource.Get(), dst_sub, placed[0], placed[1], placed[2],
                                                 src.resource.Get(), src_sub, whole ? nullptr : &box);
                CHECK(submit(listed));
                if (mode < 3)
                  for (UINT z = begin[2]; z < end[2]; z++)
                    for (UINT y = begin[1]; y < end[1]; y++)
                      for (UINT x = begin[0]; x < end[0]; x++)
                        dst.words[dst_sub][((placed[2] + z - begin[2]) * d[1] + placed[1] + y - begin[1]) * d[0] +
                                            placed[0] + x - begin[0]] = src.words[src_sub][(z * s[1] + y) * s[0] + x];
                if (check(dst, dst_readback) || check(src, src_readback))
                  return verdict();
              }
          }

  for (UINT width : widths)
    for (UINT staging = 0; staging < 4; staging++)
      for (bool listed : {false, true}) {
        auto context = listed ? deferred1.Get() : immediate1.Get();
        variant++;
        std::vector<UINT> initial(width), source(width);
        for (UINT i = 0; i < width; i++) {
          initial[i] = ++serial;
          source[i] = ++serial;
        }
        ComPtr<ID3D11Buffer> src;
        D3D11_BUFFER_DESC desc{width * word_bytes, staging & 1 ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT, 0,
                              staging & 1 ? D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE : 0u};
        D3D11_SUBRESOURCE_DATA data{source.data()};
        CHECK(device->CreateBuffer(&desc, &data, &src));
        desc.Usage = staging & 2 ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
        desc.CPUAccessFlags = staging & 2 ? D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE : 0;
        for (UINT mode = 0; mode < 8; mode++) {
          step("seed %u variant %u buffer width %u staging %u listed %u mode %u",
               seed, variant, width, staging, listed, mode);
          ComPtr<ID3D11Buffer> dst;
          data.pSysMem = initial.data();
          CHECK(device->CreateBuffer(&desc, &data, &dst));
          D3D11_BOX box{0, 0, 0, desc.ByteWidth, 1, 1};
          if (mode >= 2) {
            UINT *begin[] = {&box.left, &box.top, &box.front}, *end[] = {&box.right, &box.bottom, &box.back};
            UINT axis = (mode - 2) / 2;
            if ((mode - 2) % 2)
              std::swap(*begin[axis], *end[axis]);
            else
              *end[axis] = *begin[axis];
          }
          context->CopySubresourceRegion(dst.Get(), 0, 0, 0, 0, src.Get(), 0, mode == 0 ? nullptr : &box);
          CHECK(submit(listed));
          auto got = read(device.Get(), immediate.Get(), dst.Get());
          if (!expect(got == (mode < 2 ? source : initial), "buffer copy returned wrong words"))
            return verdict();
        }
      }
  return verdict();
}
