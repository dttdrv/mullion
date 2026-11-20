// contract: CheckDeviceMultiSampleType, CreateRenderTarget and CreateDepthStencilSurface accept a count D3D11
// reports quality for, and StretchRect of an MSAA source onto a 1x dest resolves (Microsoft Learn,
// IDirect3DDevice9::StretchRect), whole, in a rectangle or scaled, and refuses a multisampled destination.
#include <windows.h>
#include <d3d9.h>
#include <wrl/client.h>
#include <cstdio>

using Microsoft::WRL::ComPtr;

#define CHECK(x)                                                                                                       \
  if (FAILED(x)) {                                                                                                     \
    printf("failed: %s (line %d)\n", #x, __LINE__);                                                                    \
    return 1;                                                                                                          \
  }

int
main() {
  unsigned wrong = 0, checks = 0;
  auto expect = [&](bool ok, const char *what) {
    checks++;
    if (!ok && wrong++ < 16)
      printf("wrong: %s\n", what);
  };

  WNDCLASSA wc{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr, "d3d9_msaa"};
  RegisterClassA(&wc);
  HWND window = CreateWindowA("d3d9_msaa", "d3d9_msaa", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
                              wc.hInstance, nullptr);
  if (!window) {
    printf("failed: no window\n");
    return 1;
  }
  ComPtr<IDirect3D9> d3d9;
  d3d9.Attach(Direct3DCreate9(D3D_SDK_VERSION));
  if (!d3d9) {
    printf("failed: Direct3DCreate9 made no object\n");
    return 1;
  }
  DWORD quality = 0;
  HRESULT supported = d3d9->CheckDeviceMultiSampleType(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, TRUE, D3DMULTISAMPLE_4_SAMPLES,
                                                       &quality);
  expect(SUCCEEDED(supported) ? quality > 0 : supported == D3DERR_NOTAVAILABLE,
         "CheckDeviceMultiSampleType was not a quality or NOTAVAILABLE");

  D3DPRESENT_PARAMETERS present{};
  present.BackBufferWidth = 64;
  present.BackBufferHeight = 64;
  present.BackBufferFormat = D3DFMT_X8R8G8B8;
  present.BackBufferCount = 1;
  present.SwapEffect = D3DSWAPEFFECT_DISCARD;
  present.hDeviceWindow = window;
  present.Windowed = TRUE;
  present.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  ComPtr<IDirect3DDevice9> device;
  CHECK(d3d9->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &present, &device));
  if (FAILED(supported) || !quality) {
    printf("%s: %u wrong of %u checks (4x not available)\n", wrong ? "failed" : "passed", wrong, checks);
    return wrong ? 1 : 0;
  }

  ComPtr<IDirect3DSurface9> msaa, resolved;
  CHECK(device->CreateRenderTarget(64, 64, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_4_SAMPLES, 0, FALSE, &msaa, nullptr));
  CHECK(device->CreateRenderTarget(64, 64, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr));
  CHECK(device->SetRenderTarget(0, msaa.Get()));
  CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255, 0, 0), 1.0f, 0));
  CHECK(device->StretchRect(msaa.Get(), nullptr, resolved.Get(), nullptr, D3DTEXF_POINT));

  ComPtr<IDirect3DSurface9> readback;
  CHECK(device->CreateOffscreenPlainSurface(64, 64, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr));
  CHECK(device->GetRenderTargetData(resolved.Get(), readback.Get()));
  D3DLOCKED_RECT locked{};
  CHECK(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY));
  const DWORD got = ((const DWORD *)locked.pBits)[0] & 0x00FFFFFF;
  readback->UnlockRect();
  expect(got == (D3DCOLOR_XRGB(255, 0, 0) & 0x00FFFFFF), "the resolved MSAA target was not the clear colour");

  // the resolve of a rectangle: the left half of the multisampled target cleared green is resolved into the right half of
  // the single-sample one (the same size, so a resolve in place), and the half it does not name keeps its colour
  const RECT left_half{0, 0, 32, 64}, right_half{32, 0, 64, 64};
  CHECK(device->Clear(1, (const D3DRECT *)&left_half, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 255, 0), 1.0f, 0));
  CHECK(device->StretchRect(msaa.Get(), &left_half, resolved.Get(), &right_half, D3DTEXF_POINT));
  auto read_pixel = [&](IDirect3DSurface9 *surface, UINT width, UINT height, UINT x, UINT y) -> DWORD {
    ComPtr<IDirect3DSurface9> staging;
    D3DLOCKED_RECT pixels{};
    if (FAILED(device->CreateOffscreenPlainSurface(width, height, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &staging, nullptr)) ||
        FAILED(device->GetRenderTargetData(surface, staging.Get())) ||
        FAILED(staging->LockRect(&pixels, nullptr, D3DLOCK_READONLY)))
      return 0xFFFFFFFF;
    const DWORD value = ((const DWORD *)((const char *)pixels.pBits + (size_t)y * pixels.Pitch))[x] & 0x00FFFFFF;
    staging->UnlockRect();
    return value;
  };
  expect(read_pixel(resolved.Get(), 64, 64, 48, 32) == (D3DCOLOR_XRGB(0, 255, 0) & 0x00FFFFFF),
         "a resolved rectangle is not the colour of the multisampled target's rectangle");
  expect(read_pixel(resolved.Get(), 64, 64, 16, 32) == (D3DCOLOR_XRGB(255, 0, 0) & 0x00FFFFFF),
         "a resolved rectangle changed the destination outside its rectangle");

  // the resolve that scales: the whole multisampled target (green left half, red right half) into a target of half its
  // size takes each half's colour
  ComPtr<IDirect3DSurface9> smaller;
  CHECK(device->CreateRenderTarget(32, 32, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &smaller, nullptr));
  CHECK(device->StretchRect(msaa.Get(), nullptr, smaller.Get(), nullptr, D3DTEXF_LINEAR));
  expect(read_pixel(smaller.Get(), 32, 32, 6, 16) == (D3DCOLOR_XRGB(0, 255, 0) & 0x00FFFFFF) &&
             read_pixel(smaller.Get(), 32, 32, 26, 16) == (D3DCOLOR_XRGB(255, 0, 0) & 0x00FFFFFF),
         "a resolve to half the size did not keep the halves");

  // a multisampled destination takes a copy, which reaches its samples: D3D9 accepts it (Wine visual.c
  // test_multisample_stretch_rect copies into a 2-sample target), and a resolve of the destination sees the source
  ComPtr<IDirect3DSurface9> msaa_other;
  CHECK(device->CreateRenderTarget(64, 64, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_4_SAMPLES, 0, FALSE, &msaa_other, nullptr));
  CHECK(device->StretchRect(resolved.Get(), nullptr, msaa_other.Get(), nullptr, D3DTEXF_POINT));
  ComPtr<IDirect3DSurface9> copied;
  CHECK(device->CreateRenderTarget(64, 64, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &copied, nullptr));
  CHECK(device->StretchRect(msaa_other.Get(), nullptr, copied.Get(), nullptr, D3DTEXF_POINT));
  expect(read_pixel(copied.Get(), 64, 64, 16, 32) == (D3DCOLOR_XRGB(255, 0, 0) & 0x00FFFFFF),
         "a copy into a multisampled destination did not reach its samples");

  // Wine visual.c test_multisample_stretch_rect: the device has an automatic depth-stencil surface, and the
  // multisampled targets are made with the last quality level CheckDeviceMultiSampleType reports. D3D9 accepts a
  // multisampled render target over a single-sample automatic depth-stencil (the pair is bound for a draw only when
  // the counts agree), and every StretchRect of that test is then a copy or a resolve between the targets.
  DWORD levels2 = 0;
  const HRESULT have2 = d3d9->CheckDeviceMultiSampleType(0, D3DDEVTYPE_HAL, D3DFMT_A8R8G8B8, TRUE,
                                                         D3DMULTISAMPLE_2_SAMPLES, &levels2);
  if (SUCCEEDED(have2) && levels2) {
    D3DPRESENT_PARAMETERS depth_present = present;
    // Wine visual.c create_device: a 640x480 A8R8G8B8 windowed chain with an automatic D24S8 depth surface
    depth_present.BackBufferWidth = 640;
    depth_present.BackBufferHeight = 480;
    depth_present.BackBufferFormat = D3DFMT_A8R8G8B8;
    depth_present.EnableAutoDepthStencil = TRUE;
    depth_present.AutoDepthStencilFormat = D3DFMT_D24S8;
    ComPtr<IDirect3DDevice9> depth_device;
    CHECK(d3d9->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &depth_present,
                             &depth_device));
    auto read_on = [](IDirect3DDevice9 *on, IDirect3DSurface9 *surface, UINT width, UINT height, UINT x, UINT y) {
      ComPtr<IDirect3DSurface9> staging;
      D3DLOCKED_RECT pixels{};
      if (FAILED(on->CreateOffscreenPlainSurface(width, height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &staging, nullptr)) ||
          FAILED(on->GetRenderTargetData(surface, staging.Get())) ||
          FAILED(staging->LockRect(&pixels, nullptr, D3DLOCK_READONLY)))
        return (DWORD)0xFFFFFFFF;
      const DWORD value = ((const DWORD *)((const char *)pixels.pBits + (size_t)y * pixels.Pitch))[x] & 0x00FFFFFF;
      staging->UnlockRect();
      return value;
    };
    ComPtr<IDirect3DSurface9> ms2, ms2b, plain2;
    CHECK(depth_device->CreateRenderTarget(128, 128, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &plain2, nullptr));
    CHECK(depth_device->CreateRenderTarget(128, 128, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_2_SAMPLES, levels2 - 1, FALSE,
                                          &ms2, nullptr));
    CHECK(depth_device->CreateRenderTarget(128, 128, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_2_SAMPLES, levels2 - 1, FALSE,
                                          &ms2b, nullptr));
    expect(SUCCEEDED(depth_device->SetRenderTarget(0, ms2.Get())),
           "an automatic depth-stencil surface refused a 2-sample render target");
    expect(SUCCEEDED(depth_device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255, 0, 255), 0.0f, 0)),
           "a 2-sample render target could not be cleared");
    expect(SUCCEEDED(depth_device->SetRenderTarget(0, plain2.Get())),
           "the single-sample target could not be set after the 2-sample one");
    for (unsigned filter = 0; filter < 3; filter++) {
      const D3DTEXTUREFILTERTYPE which = filter == 0 ? D3DTEXF_NONE : filter == 1 ? D3DTEXF_POINT : D3DTEXF_LINEAR;
      depth_device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255, 255, 255), 0.0f, 0);
      expect(SUCCEEDED(depth_device->StretchRect(ms2.Get(), nullptr, plain2.Get(), nullptr, which)),
             "a 2-sample source could not be copied to a single-sample target");
      expect(read_on(depth_device.Get(), plain2.Get(), 128, 128, 64, 64) == (D3DCOLOR_XRGB(255, 0, 255) & 0x00FFFFFF),
             "a 2-sample target's clear colour did not reach a single-sample target");
      const RECT half{0, 0, 64, 64};
      depth_device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255, 255, 255), 0.0f, 0);
      expect(SUCCEEDED(depth_device->StretchRect(plain2.Get(), nullptr, ms2b.Get(), nullptr, D3DTEXF_NONE)),
             "a single-sample source could not be copied into a 2-sample target");
      expect(SUCCEEDED(depth_device->StretchRect(ms2.Get(), nullptr, plain2.Get(), &half, which)),
             "a scaled copy of a 2-sample source was refused");
      expect(read_on(depth_device.Get(), plain2.Get(), 128, 128, 32, 32) == (D3DCOLOR_XRGB(255, 0, 255) & 0x00FFFFFF),
             "a scaled copy of a 2-sample source did not take its colour");
      expect(read_on(depth_device.Get(), plain2.Get(), 128, 128, 96, 96) == (D3DCOLOR_XRGB(255, 255, 255) & 0x00FFFFFF),
             "a scaled copy of a 2-sample source changed the target outside its rectangle");
    }
  }
  printf("%s: %u wrong of %u checks (resolved %06lx)\n", wrong ? "failed" : "passed", wrong, checks,
         (unsigned long)got);
  return wrong ? 1 : 0;
}
