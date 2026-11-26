// contract: a region copy from or to a depth stencil or multisampled texture copies only the whole subresource, with
// no box and no offsets, and does nothing otherwise. "If you use CopySubresourceRegion with a depth-stencil buffer or
// a multisampled resource, you must copy the whole subresource. In this situation, you must pass 0 to the DstX, DstY,
// and DstZ parameters and NULL to the pSrcBox parameter" (Microsoft Learn, ID3D11DeviceContext::CopySubresourceRegion);
// the runtime makes sure a driver gets no other (PFND3D11_1DDI_RESOURCECOPYREGION: it "verifies that the pSrcBox
// parameter is NULL, while the DstX, DstY, and DstZ parameters are 0"), and Windows leaves the destination as it was
// (Wine's test_format_compatibility, D16_UNORM and R16_UNORM). so a box the size of the whole subresource does nothing
// too. each copy runs on the immediate context and in a deferred context's list; every texel of the destination is
// read, and the source and the destination start apart.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-copysubresourceregion
// https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d11_1ddi_resourcecopyregion
#include "d3d11_test.hpp"

int
main() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                          &immediate));
  CHECK(device->CreateDeferredContext(0, &deferred));
  const UINT side = 4, samples = 4;
  auto make = [&](DXGI_FORMAT format, UINT bind, UINT count, const void *data, UINT pitch) {
    D3D11_TEXTURE2D_DESC desc{side, side, 1, 1, format, {count, 0}, D3D11_USAGE_DEFAULT, bind};
    D3D11_SUBRESOURCE_DATA initial{data, pitch};
    ComPtr<ID3D11Texture2D> texture;
    device->CreateTexture2D(&desc, data ? &initial : nullptr, &texture);
    return texture;
  };
  // the texture's texels, of `bytes` each, through a staging copy (and a resolve first for a multisampled one)
  auto read = [&](ID3D11Texture2D *texture, UINT bytes) {
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    ComPtr<ID3D11Texture2D> single = texture;
    if (desc.SampleDesc.Count > 1) {
      single = make(desc.Format, 0, 1, nullptr, 0);
      immediate->ResolveSubresource(single.Get(), 0, texture, 0, desc.Format);
    }
    desc.SampleDesc = {1, 0};
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    std::vector<uint32_t> texels;
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (!single || FAILED(device->CreateTexture2D(&desc, nullptr, &staging)))
      return texels;
    immediate->CopyResource(staging.Get(), single.Get());
    if (FAILED(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
      return texels;
    for (UINT y = 0; y < side; y++)
      for (UINT x = 0; x < side; x++) {
        uint32_t texel = 0;
        memcpy(&texel, static_cast<const char *>(mapped.pData) + y * mapped.RowPitch + x * bytes, bytes);
        texels.push_back(texel);
      }
    immediate->Unmap(staging.Get(), 0);
    return texels;
  };

  uint16_t pattern[side * side];
  for (UINT i = 0; i < side * side; i++)
    pattern[i] = uint16_t(0x1001 * (i + 1));
  const uint16_t zeros[side * side]{};
  const float black[4] = {}, red[4] = {1, 0, 0, 1};
  const uint32_t black_texel = 0, red_texel = 0xff0000ff;
  UINT qualities = 0;
  CHECK(device->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, samples, &qualities));
  if (!expect(qualities, "4 samples of R8G8B8A8_UNORM are not supported"))
    return verdict();

  // the region copies, and the whole copy last; each but the last leaves the destination as it was
  const D3D11_BOX part{0, 0, 0, side - 1, side - 1, 1}, whole{0, 0, 0, side, side, 1};
  struct Copy {
    const char *what;
    const D3D11_BOX *box;
    UINT x, y;
  } copies[] = {
      {"a part, offset", &part, 1, 1}, {"a box of the whole", &whole, 0, 0}, {"no box, offset", nullptr, 1, 0},
      {"no box, no offset", nullptr, 0, 0}};
  struct Case {
    const char *what;
    ComPtr<ID3D11Texture2D> src, dst;
    UINT bytes;
    std::vector<uint32_t> before, after;
  } cases[] = {
      {"from D16_UNORM depth to R16_UNORM", make(DXGI_FORMAT_D16_UNORM, D3D11_BIND_DEPTH_STENCIL, 1, pattern, side * 2),
       make(DXGI_FORMAT_R16_UNORM, D3D11_BIND_SHADER_RESOURCE, 1, zeros, side * 2), 2},
      {"from R16_UNORM to D16_UNORM depth", make(DXGI_FORMAT_R16_UNORM, D3D11_BIND_SHADER_RESOURCE, 1, pattern, side * 2),
       make(DXGI_FORMAT_D16_UNORM, D3D11_BIND_DEPTH_STENCIL, 1, nullptr, 0), 2},
      {"from 4 samples to 4 samples", make(DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET, samples, nullptr, 0),
       make(DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET, samples, nullptr, 0), 4},
  };
  for (auto &c : cases) {
    if (!expect(c.src && c.dst, "%s: the textures could not be made", c.what))
      return verdict();
    c.before = c.bytes == 4 ? std::vector<uint32_t>(side * side, black_texel) : std::vector<uint32_t>(side * side, 0);
    c.after = c.bytes == 4 ? std::vector<uint32_t>(side * side, red_texel)
                           : std::vector<uint32_t>(pattern, pattern + side * side);
  }
  // the destination as it starts, and the multisampled source red
  auto reset = [&](Case &c) {
    D3D11_TEXTURE2D_DESC desc;
    c.dst->GetDesc(&desc);
    if (desc.BindFlags & D3D11_BIND_DEPTH_STENCIL) {
      ComPtr<ID3D11DepthStencilView> dsv;
      device->CreateDepthStencilView(c.dst.Get(), nullptr, &dsv);
      immediate->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 0, 0);
    } else if (desc.BindFlags & D3D11_BIND_RENDER_TARGET) {
      ComPtr<ID3D11RenderTargetView> dst_rtv, src_rtv;
      device->CreateRenderTargetView(c.dst.Get(), nullptr, &dst_rtv);
      device->CreateRenderTargetView(c.src.Get(), nullptr, &src_rtv);
      immediate->ClearRenderTargetView(dst_rtv.Get(), black);
      immediate->ClearRenderTargetView(src_rtv.Get(), red);
    } else {
      immediate->UpdateSubresource(c.dst.Get(), 0, nullptr, zeros, side * 2, 0);
    }
  };
  for (auto *context : {immediate.Get(), deferred.Get()})
    for (auto &c : cases)
      for (auto &copy : copies) {
        step("%s, %s: %s", context == immediate.Get() ? "immediate" : "deferred", c.what, copy.what);
        reset(c);
        context->CopySubresourceRegion(c.dst.Get(), 0, copy.x, copy.y, 0, c.src.Get(), 0, copy.box);
        if (context == deferred.Get()) {
          ComPtr<ID3D11CommandList> commands;
          CHECK(deferred->FinishCommandList(FALSE, &commands));
          immediate->ExecuteCommandList(commands.Get(), FALSE);
        }
        auto got = read(c.dst.Get(), c.bytes);
        auto &want = &copy == &copies[std::size(copies) - 1] ? c.after : c.before;
        if (!expect(got.size() == want.size(), "the destination could not be read"))
          continue;
        UINT wrong = 0;
        for (UINT i = 0; i < got.size(); i++)
          if (got[i] != want[i] && wrong++ < 4)
            expect(false, "texel (%u,%u) is 0x%x, want 0x%x", i % side, i / side, got[i], want[i]);
        expect(!wrong, "%u texels of %zu are wrong", wrong, got.size());
      }
  return verdict();
}
