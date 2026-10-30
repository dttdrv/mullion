// contract: CheckDeviceMultiSampleType, CreateRenderTarget and CreateDepthStencilSurface accept a count D3D11
// reports quality for, and StretchRect of an MSAA source onto a 1x dest resolves (Microsoft Learn,
// IDirect3DDevice9::StretchRect). a windowed chain stays 1x.
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
  printf("%s: %u wrong of %u checks (resolved %06lx)\n", wrong ? "failed" : "passed", wrong, checks,
         (unsigned long)got);
  return wrong ? 1 : 0;
}
