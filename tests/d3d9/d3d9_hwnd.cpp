// contract: Direct3D 9 refuses a device whose window is not one, and a device whose window is destroyed later still
// presents without crashing. IDirect3DDevice9::CreateDevice / Reset require a valid hDeviceWindow for a windowed
// chain. After the window is gone, Present and GDI presentation (dxgi.presentThroughGDI) must not dereference a
// null macdrv win_data. On i386 the GDI present path copies into guest-addressable pages (mullion_presenter.cpp).
// https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3d9-createdevice
#include <windows.h>
#include <d3d9.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>

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

  SetEnvironmentVariableA("MULLION_CONFIG", "dxgi.presentThroughGDI=True");

  WNDCLASSA wc{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr, "d3d9_hwnd"};
  RegisterClassA(&wc);
  HWND window = CreateWindowA("d3d9_hwnd", "d3d9_hwnd", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 64, 64, nullptr,
                              nullptr, wc.hInstance, nullptr);
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

  D3DPRESENT_PARAMETERS present{};
  present.BackBufferWidth = 64;
  present.BackBufferHeight = 64;
  present.BackBufferFormat = D3DFMT_X8R8G8B8;
  present.BackBufferCount = 1;
  present.SwapEffect = D3DSWAPEFFECT_DISCARD;
  present.hDeviceWindow = window;
  present.Windowed = TRUE;
  present.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

  D3DPRESENT_PARAMETERS missing = present;
  missing.hDeviceWindow = (HWND)(uintptr_t)0x1234;
  ComPtr<IDirect3DDevice9> refused;
  expect(d3d9->CreateDevice(0, D3DDEVTYPE_HAL, missing.hDeviceWindow, D3DCREATE_HARDWARE_VERTEXPROCESSING, &missing,
                            &refused) == D3DERR_INVALIDCALL &&
             !refused,
         "CreateDevice of a handle that is not a window was accepted");

  HWND destroyed = CreateWindowA("d3d9_hwnd", "d3d9_hwnd_gone", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
                                 wc.hInstance, nullptr);
  expect(destroyed != nullptr, "no second window");
  DestroyWindow(destroyed);
  D3DPRESENT_PARAMETERS gone = present;
  gone.hDeviceWindow = destroyed;
  ComPtr<IDirect3DDevice9> gone_device;
  expect(d3d9->CreateDevice(0, D3DDEVTYPE_HAL, destroyed, D3DCREATE_HARDWARE_VERTEXPROCESSING, &gone, &gone_device) ==
                 D3DERR_INVALIDCALL &&
             !gone_device,
         "CreateDevice of a destroyed window was accepted");

  ComPtr<IDirect3DDevice9> device;
  CHECK(d3d9->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &present, &device));
  CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 255, 0), 1.0f, 0));
  expect(SUCCEEDED(device->Present(nullptr, nullptr, nullptr, nullptr)),
         "Present to a live window through GDI failed");

  DestroyWindow(window);
  expect(!IsWindow(window), "DestroyWindow left the device window");
  HRESULT after = device->Present(nullptr, nullptr, nullptr, nullptr);
  expect(SUCCEEDED(after) || FAILED(after), "Present after the window was destroyed crashed");
  (void)after;

  HWND recreated = CreateWindowA("d3d9_hwnd", "d3d9_hwnd_new", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 64, 64, nullptr,
                                 nullptr, wc.hInstance, nullptr);
  expect(recreated != nullptr, "no replacement window");
  if (recreated) {
    present.hDeviceWindow = recreated;
    CHECK(device->Reset(&present));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 255), 1.0f, 0));
    expect(SUCCEEDED(device->Present(nullptr, nullptr, nullptr, nullptr)),
           "Present after Reset to a recreated window failed");
    DestroyWindow(recreated);
  }

  expect(Direct3DCreate9Ex(D3D_SDK_VERSION, nullptr) == E_NOTIMPL,
         "Direct3DCreate9Ex is the 9Ex object this build keeps distinct and unimplemented");

  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
