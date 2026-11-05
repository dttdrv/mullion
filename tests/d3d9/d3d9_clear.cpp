#include <windows.h>
#include <d3d9.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>

static LRESULT CALLBACK
WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (msg == WM_DESTROY || (msg == WM_KEYDOWN && wparam == VK_ESCAPE)) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

int WINAPI
WinMain(HINSTANCE instance, HINSTANCE, LPSTR cmdline, int) {
  unsigned max_frames = cmdline && *cmdline ? strtoul(cmdline, nullptr, 10) : 0;
  WNDCLASSW wc = {};
  wc.lpfnWndProc = WindowProc;
  wc.hInstance = instance;
  wc.lpszClassName = L"d3d9_clear";
  RegisterClassW(&wc);
  HWND hwnd = CreateWindowW(
      wc.lpszClassName, L"d3d9_clear", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
      nullptr, nullptr, instance, nullptr
  );

  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) {
    printf("Direct3DCreate9 failed\n");
    return 1;
  }

  D3DADAPTER_IDENTIFIER9 id = {};
  d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id);
  D3DCAPS9 caps = {};
  d3d->GetDeviceCaps(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps);
  printf(
      "adapters: %u, %s (%s), VS %lx PS %lx, modes: %u\n", d3d->GetAdapterCount(), id.Description, id.Driver,
      caps.VertexShaderVersion, caps.PixelShaderVersion, d3d->GetAdapterModeCount(D3DADAPTER_DEFAULT, D3DFMT_X8R8G8B8)
  );

  D3DPRESENT_PARAMETERS pp = {};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.BackBufferFormat = D3DFMT_UNKNOWN;
  pp.EnableAutoDepthStencil = TRUE;
  pp.AutoDepthStencilFormat = D3DFMT_D24S8;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
  pp.hDeviceWindow = hwnd;

  IDirect3DDevice9 *device = nullptr;
  HRESULT hr =
      d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device);
  if (FAILED(hr)) {
    printf("CreateDevice failed: %lx\n", hr);
    return 1;
  }
  printf("backbuffer %ux%u format %u\n", pp.BackBufferWidth, pp.BackBufferHeight, pp.BackBufferFormat);

  MSG msg = {};
  for (unsigned frame = 0; msg.message != WM_QUIT && (!max_frames || frame < max_frames); frame++) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }

    float t = frame / 60.0f;
    D3DCOLOR color = D3DCOLOR_XRGB(
        (int)(127 + 127 * sinf(t)), (int)(127 + 127 * sinf(t + 2.1f)), (int)(127 + 127 * sinf(t + 4.2f))
    );

    device->BeginScene();
    device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, color, 1.0f, 0);
    D3DRECT center = {
        (LONG)pp.BackBufferWidth / 4, (LONG)pp.BackBufferHeight / 4, (LONG)pp.BackBufferWidth * 3 / 4,
        (LONG)pp.BackBufferHeight * 3 / 4
    };
    device->Clear(1, &center, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255, 255, 255) ^ color, 1.0f, 0);
    device->EndScene();
    device->Present(nullptr, nullptr, nullptr, nullptr);
  }

  printf("done\n");
  device->Release();
  d3d->Release();
  return 0;
}
