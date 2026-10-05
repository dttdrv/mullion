// contract: a 2D texture is a DXGI surface.
// - a texture of one mip level that is not an array answers QueryInterface for IDXGISurface, and the surface for the
//   texture; one with more mip levels or elements does not (IDXGISurface, Remarks). the surface has the texture's
//   size, format and sample description, and maps the texture's memory (IDXGISurface::Map: DXGI_MAP_READ, _WRITE).
// - IDXGIResource1::CreateSubresourceSurface gives an element of a one-level array as a surface ("a valid surface if
//   the original resource would have been a valid surface had its array size been equal to 1"), which
//   IDXGISurface2::GetResource traces back to its texture and index; a texture that is a surface is index 0 of itself.
// - IDXGISurface1::GetDC draws with GDI: only on a surface created GDI compatible, with one DC at a time; without
//   Discard the DC shows what Direct3D drew; the output merger no longer has the surface as a target; and what GDI
//   draws is in the texture after ReleaseDC.
// - IDXGIDevice::CreateSurface makes as many surfaces as asked, each a 2D texture bound as its usage says.
// GDI does not write a bitmap's fourth byte, so colours are compared without alpha.
#include "d3d11_test.hpp"
#include <d3d11_1.h>
#include <dxgi1_2.h>

int
main() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context));
  unsigned wrong = 0, checks = 0;
  auto expect = [&](bool ok, const char *what) {
    checks++;
    if (!ok && wrong++ < 16)
      printf("%s\n", what);
  };
  const UINT width = 48, height = 32;
  const DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;
  auto texture = [&](UINT mips, UINT elements, D3D11_USAGE usage, UINT bind, UINT cpu, UINT misc) {
    D3D11_TEXTURE2D_DESC desc{width, height, mips, elements, format, {1, 0}, usage, bind, cpu, misc};
    ComPtr<ID3D11Texture2D> out;
    device->CreateTexture2D(&desc, nullptr, &out);
    return out;
  };

  // which textures are surfaces
  {
    auto one = texture(1, 1, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0);
    auto mips = texture(2, 1, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0);
    auto array = texture(1, 3, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0);
    if (!one || !mips || !array) {
      printf("failed: no textures\n");
      return 1;
    }
    ComPtr<IDXGISurface> surface, none;
    ComPtr<IDXGISurface2> surface2;
    expect(SUCCEEDED(one.As(&surface)) && SUCCEEDED(one.As(&surface2)), "a texture of one subresource is no surface");
    expect(FAILED(mips.As(&none)), "a texture of two mip levels is a surface");
    expect(FAILED(array.As(&none)), "an array of textures is a surface");
    if (!surface || !surface2)
      return 1;
    ComPtr<ID3D11Texture2D> back;
    ComPtr<IUnknown> identity[2];
    expect(SUCCEEDED(surface.As(&back)) && back == one, "the surface is not its texture");
    surface.As(&identity[0]);
    one.As(&identity[1]);
    expect(identity[0] == identity[1], "the surface and its texture are two objects");
    DXGI_SURFACE_DESC desc{};
    expect(SUCCEEDED(surface->GetDesc(&desc)) && desc.Width == width && desc.Height == height && desc.Format == format &&
               desc.SampleDesc.Count == 1,
           "the surface's description is not its texture's");
    ComPtr<ID3D11Resource> parent;
    UINT index = ~0u;
    expect(SUCCEEDED(surface2->GetResource(IID_PPV_ARGS(&parent), &index)) && parent.Get() == one.Get() && index == 0,
           "a texture's surface is not subresource 0 of the texture");
    ComPtr<ID3D11Device> owner;
    expect(SUCCEEDED(surface->GetDevice(IID_PPV_ARGS(&owner))) && owner == device, "the surface is of another device");
    ComPtr<IDXGIResource> resource;
    DXGI_USAGE usage = 0;
    expect(SUCCEEDED(one.As(&resource)) && SUCCEEDED(resource->GetUsage(&usage)) && usage == DXGI_USAGE_SHADER_INPUT,
           "a shader resource's usage is not shader input");

    // an array's elements
    ComPtr<IDXGIResource1> elements, levels;
    CHECK(array.As(&elements));
    CHECK(mips.As(&levels));
    ComPtr<IDXGISurface2> element[3], beyond;
    for (UINT i = 0; i < 3; i++) {
      ComPtr<ID3D11Resource> of;
      UINT at = ~0u;
      bool made = SUCCEEDED(elements->CreateSubresourceSurface(i, &element[i])) && element[i];
      expect(made, "an array's element is no surface");
      if (!made)
        continue;
      expect(SUCCEEDED(element[i]->GetResource(IID_PPV_ARGS(&of), &at)) && of.Get() == array.Get() && at == i,
             "an element's surface does not name its texture and index");
      ComPtr<ID3D11Texture2D> not_a_texture;
      expect(FAILED(element[i].As(&not_a_texture)), "an element's surface is a texture");
      expect(SUCCEEDED(element[i]->GetDesc(&desc)) && desc.Width == width && desc.Height == height,
             "an element's surface has another size");
    }
    expect(element[0] != element[1] && element[1] != element[2], "two elements are one surface");
    expect(FAILED(elements->CreateSubresourceSurface(3, &beyond)), "an array has a surface past its elements");
    expect(FAILED(levels->CreateSubresourceSurface(0, &beyond)), "a mip level of several is a surface");
  }

  // mapping
  {
    auto staging = texture(1, 1, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE, 0);
    ComPtr<IDXGISurface> surface;
    CHECK(staging.As(&surface));
    auto texel = [](UINT x, UINT y) { return 0x01000193u * (y * width + x + 1); };
    DXGI_MAPPED_RECT rect{};
    CHECK(surface->Map(&rect, DXGI_MAP_WRITE));
    for (UINT y = 0; y < height; y++)
      for (UINT x = 0; x < width; x++)
        memcpy(rect.pBits + y * rect.Pitch + 4 * x, &(const UINT &)texel(x, y), 4);
    CHECK(surface->Unmap());
    D3D11_MAPPED_SUBRESOURCE mapped;
    CHECK(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    unsigned differ = 0;
    for (UINT y = 0; y < height; y++)
      for (UINT x = 0; x < width; x++)
        differ += *(const UINT *)((const char *)mapped.pData + y * mapped.RowPitch + 4 * x) != texel(x, y);
    context->Unmap(staging.Get(), 0);
    expect(!differ, "the texture does not hold what was written through the surface");
    CHECK(surface->Map(&rect, DXGI_MAP_READ));
    differ = 0;
    for (UINT y = 0; y < height; y++)
      for (UINT x = 0; x < width; x++)
        differ += *(const UINT *)(rect.pBits + y * rect.Pitch + 4 * x) != texel(x, y);
    CHECK(surface->Unmap());
    expect(!differ, "the surface does not read what the texture holds");
  }

  // GDI
  {
    auto plain = texture(1, 1, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0);
    auto gdi = texture(1, 1, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, D3D11_RESOURCE_MISC_GDI_COMPATIBLE);
    auto readable = texture(1, 1, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0);
    if (!plain || !gdi || !readable) {
      printf("failed: no GDI compatible texture\n");
      return 1;
    }
    ComPtr<IDXGISurface1> plain_surface, surface;
    CHECK(plain.As(&plain_surface));
    CHECK(gdi.As(&surface));
    HDC dc = nullptr, second = nullptr;
    expect(FAILED(plain_surface->GetDC(FALSE, &dc)), "a surface that is not GDI compatible gives a DC");
    // what Direct3D draws, as bytes a UNORM target holds exactly, and what GDI draws over a part of it
    const BYTE cleared[3] = {64, 128, 192}, drawn[3] = {10, 200, 30};
    const RECT part{8, 4, 40, 20};
    ComPtr<ID3D11RenderTargetView> rtv;
    CHECK(device->CreateRenderTargetView(gdi.Get(), nullptr, &rtv));
    const float clear[4] = {cleared[0] / 255.0f, cleared[1] / 255.0f, cleared[2] / 255.0f, 1};
    context->ClearRenderTargetView(rtv.Get(), clear);
    context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    CHECK(surface->GetDC(FALSE, &dc));
    ComPtr<ID3D11RenderTargetView> bound;
    context->OMGetRenderTargets(1, &bound, nullptr);
    expect(!bound, "the output merger still renders to a surface GDI draws to");
    expect(FAILED(surface->GetDC(FALSE, &second)), "a surface gives a second DC");
    expect(GetPixel(dc, 0, 0) == RGB(cleared[0], cleared[1], cleared[2]) &&
               GetPixel(dc, width - 1, height - 1) == RGB(cleared[0], cleared[1], cleared[2]),
           "the DC does not show what Direct3D drew");
    HBRUSH brush = CreateSolidBrush(RGB(drawn[0], drawn[1], drawn[2]));
    FillRect(dc, &part, brush);
    DeleteObject(brush);
    CHECK(surface->ReleaseDC(nullptr));
    expect(FAILED(surface->ReleaseDC(nullptr)), "a surface releases a DC it does not have");
    context->CopyResource(readable.Get(), gdi.Get());
    D3D11_MAPPED_SUBRESOURCE mapped;
    CHECK(context->Map(readable.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    unsigned differ = 0;
    for (LONG y = 0; y < (LONG)height; y++)
      for (LONG x = 0; x < (LONG)width; x++) {
        // B8G8R8A8: blue first
        const BYTE *got = (const BYTE *)mapped.pData + y * mapped.RowPitch + 4 * x;
        const BYTE *want = x >= part.left && x < part.right && y >= part.top && y < part.bottom ? drawn : cleared;
        if ((got[2] != want[0] || got[1] != want[1] || got[0] != want[2]) && differ++ < 2)
          printf("after GDI, texel %ld,%ld is %u %u %u, want %u %u %u\n", x, y, got[2], got[1], got[0], want[0], want[1], want[2]);
      }
    context->Unmap(readable.Get(), 0);
    expect(!differ, "the texture does not hold what GDI drew over what Direct3D drew");
    // a DC again, once the first is released
    expect(SUCCEEDED(surface->GetDC(TRUE, &dc)) && SUCCEEDED(surface->ReleaseDC(nullptr)), "a surface gives no DC again");
  }

  // surfaces from the DXGI device
  {
    ComPtr<IDXGIDevice> dxgi;
    CHECK(device.As(&dxgi));
    DXGI_SURFACE_DESC desc{width, height, format, {1, 0}};
    IDXGISurface *made[2] = {};
    bool ok = SUCCEEDED(dxgi->CreateSurface(&desc, 2, DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT, nullptr, made));
    expect(ok && made[0] && made[1] && made[0] != made[1], "the device does not make two surfaces");
    for (auto surface : made) {
      if (!surface)
        continue;
      ComPtr<ID3D11Texture2D> as_texture;
      D3D11_TEXTURE2D_DESC texture_desc{};
      if (SUCCEEDED(surface->QueryInterface(IID_PPV_ARGS(&as_texture))))
        as_texture->GetDesc(&texture_desc);
      expect(texture_desc.Width == width && texture_desc.Height == height && texture_desc.Format == format &&
                 texture_desc.MipLevels == 1 && texture_desc.ArraySize == 1 &&
                 texture_desc.BindFlags == (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE),
             "a surface from the device is not the 2D texture its description and usage ask for");
      surface->Release();
    }
  }
  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
