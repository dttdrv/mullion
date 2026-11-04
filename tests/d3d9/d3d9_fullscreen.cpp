#include <windows.h>
#include <d3d9.h>
#include <cstdio>

static int failures = 0;

static void
check(const char *name, bool ok) {
  printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
  failures += !ok;
}

// takes over the display at the current desktop resolution, so run.sh only runs it on request.
int WINAPI
WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
  WNDCLASSW wc = {};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = instance;
  wc.lpszClassName = L"d3d9_fullscreen";
  RegisterClassW(&wc);
  HWND hwnd = CreateWindowW(
      wc.lpszClassName, L"d3d9_fullscreen", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 200, 150, nullptr, nullptr,
      instance, nullptr
  );
  RECT windowed_rect;
  GetWindowRect(hwnd, &windowed_rect);

  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  D3DDISPLAYMODE mode;
  d3d->GetAdapterDisplayMode(0, &mode);
  D3DPRESENT_PARAMETERS pp = {};
  pp.Windowed = FALSE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.BackBufferWidth = mode.Width;
  pp.BackBufferHeight = mode.Height;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.FullScreen_RefreshRateInHz = mode.RefreshRate;
  pp.hDeviceWindow = hwnd;
  IDirect3DDevice9 *device = nullptr;
  HRESULT hr = d3d->CreateDevice(0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device);
  check("fullscreen device", SUCCEEDED(hr));
  if (FAILED(hr))
    return 1;

  RECT rect;
  GetWindowRect(hwnd, &rect);
  HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO info = {sizeof(info)};
  GetMonitorInfoW(monitor, &info);
  check("window covers the monitor", EqualRect(&rect, &info.rcMonitor));
  for (int i = 0; i < 30; i++) {
    device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, i * 8, 0x40), 1.0f, 0);
    if (FAILED(device->Present(nullptr, nullptr, nullptr, nullptr))) {
      check("fullscreen present", false);
      break;
    }
  }

  check("device usable while active", device->TestCooperativeLevel() == D3D_OK);

  // GPU work completes in fullscreen (Wine's test_occlusion_query never saw its queries finish)
  IDirect3DQuery9 *event = nullptr, *occlusion = nullptr;
  device->CreateQuery(D3DQUERYTYPE_EVENT, &event);
  device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &occlusion);
  struct Vertex {
    float x, y, z, rhw;
  } quad[4] = {{0, 0, 0.5f, 1}, {(float)pp.BackBufferWidth, 0, 0.5f, 1}, {0, (float)pp.BackBufferHeight, 0.5f, 1},
               {(float)pp.BackBufferWidth, (float)pp.BackBufferHeight, 0.5f, 1}};
  device->SetFVF(D3DFVF_XYZRHW);
  device->BeginScene();
  occlusion->Issue(D3DISSUE_BEGIN);
  device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));
  occlusion->Issue(D3DISSUE_END);
  device->EndScene();
  event->Issue(D3DISSUE_END);
  auto wait = [](IDirect3DQuery9 *query, void *data, DWORD size) {
    DWORD start = GetTickCount();
    HRESULT hr;
    while ((hr = query->GetData(data, size, D3DGETDATA_FLUSH)) == S_FALSE && GetTickCount() - start < 5000)
      Sleep(10);
    return hr;
  };
  BOOL done = FALSE;
  DWORD samples = 0;
  check("event query completes", wait(event, &done, sizeof(done)) == S_OK && done);
  check("occlusion query completes", wait(occlusion, &samples, sizeof(samples)) == S_OK);
  check("occlusion counts the screen", samples == pp.BackBufferWidth * pp.BackBufferHeight);
  printf("occlusion samples %lu\n", samples);
  event->Release();
  occlusion->Release();

  pp.Windowed = TRUE;
  pp.BackBufferWidth = 0;
  pp.BackBufferHeight = 0;
  pp.FullScreen_RefreshRateInHz = 0;
  check("Reset to windowed", SUCCEEDED(device->Reset(&pp)));
  GetWindowRect(hwnd, &rect);
  check("window rect restored", EqualRect(&rect, &windowed_rect));

  device->Release();
  d3d->Release();
  DestroyWindow(hwnd);
  printf("%s: %d failure(s)\n", failures ? "failed" : "passed", failures);
  return failures;
}
