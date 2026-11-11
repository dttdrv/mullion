#include <windows.h>
#include <d3d9.h>
#include <cmath>
#include <cstdio>

static int failures = 0;
static IDirect3DDevice9 *device;

static void
check(const char *name, bool ok) {
  printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
  failures += !ok;
}

static DWORD
back_buffer_pixel(UINT width, UINT height, int x, int y) {
  IDirect3DSurface9 *bb = nullptr, *sys = nullptr;
  DWORD value = 0xDEADBEEF;
  device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
  device->CreateOffscreenPlainSurface(width, height, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr);
  D3DLOCKED_RECT lr;
  if (bb && sys && SUCCEEDED(device->GetRenderTargetData(bb, sys)) &&
      SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
    value = ((DWORD *)((char *)lr.pBits + y * lr.Pitch))[x] & 0xFFFFFF;
    sys->UnlockRect();
  }
  if (sys)
    sys->Release();
  if (bb)
    bb->Release();
  return value;
}

struct Vertex {
  float x, y, z;
  DWORD color;
};

struct ScreenVertex {
  float x, y, z, rhw;
  DWORD color;
};

static void
draw_scene(unsigned frame) {
  device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000040, 1.0f, 0);
  float a = frame * 0.1f, c = cosf(a), s = sinf(a);
  D3DMATRIX rotation = {{{c, s, 0, 0, -s, c, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}};
  device->SetTransform(D3DTS_WORLD, &rotation);
  Vertex triangle[3];
  for (int i = 0; i < 3; i++) {
    float angle = i * 2.0944f;
    triangle[i] = {0.8f * sinf(angle), 0.8f * cosf(angle), 0.5f, 0xFF00FF00};
  }
  device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
  device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, triangle, sizeof(Vertex));

  // behind the triangle at the center, in front of the clear color in the corner
  D3DVIEWPORT9 vp;
  device->GetViewport(&vp);
  float w = (float)vp.Width, h = (float)vp.Height;
  ScreenVertex square[4] = {
      {0, 0, 0.9f, 1, 0xFFFF0000},
      {w * 0.6f, 0, 0.9f, 1, 0xFFFF0000},
      {0, h * 0.6f, 0.9f, 1, 0xFFFF0000},
      {w * 0.6f, h * 0.6f, 0.9f, 1, 0xFFFF0000},
  };
  device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
  device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, square, sizeof(ScreenVertex));
}

static void
run_frames(UINT width, UINT height, unsigned count, const char *label) {
  HRESULT present = D3D_OK;
  bool center = true, corner = true, behind = true;
  for (unsigned frame = 0; frame < count; frame++) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
      DispatchMessageW(&msg);
    device->BeginScene();
    draw_scene(frame);
    device->EndScene();
    if (frame % 15 == 0) {
      center &= back_buffer_pixel(width, height, width / 2, height / 2) == 0x00FF00;
      corner &= back_buffer_pixel(width, height, 2, 2) == 0xFF0000;
      behind &= back_buffer_pixel(width, height, width - 3, height - 3) == 0x000040;
    }
    HRESULT hr = device->Present(nullptr, nullptr, nullptr, nullptr);
    if (FAILED(hr))
      present = hr;
  }
  char name[128];
  snprintf(name, sizeof(name), "%s: every Present succeeds", label);
  check(name, present == D3D_OK);
  snprintf(name, sizeof(name), "%s: triangle passes the depth test", label);
  check(name, center);
  snprintf(name, sizeof(name), "%s: pretransformed square", label);
  check(name, corner);
  snprintf(name, sizeof(name), "%s: clear color", label);
  check(name, behind);
}

int WINAPI
WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
  WNDCLASSW wc = {};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = instance;
  wc.lpszClassName = L"d3d9_present";
  RegisterClassW(&wc);
  RECT rect = {0, 0, 256, 256};
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowW(
      wc.lpszClassName, L"d3d9_present", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
      rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, nullptr
  );

  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  D3DPRESENT_PARAMETERS pp = {};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.BackBufferWidth = 256;
  pp.BackBufferHeight = 256;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.EnableAutoDepthStencil = TRUE;
  pp.AutoDepthStencilFormat = D3DFMT_D24S8;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  pp.hDeviceWindow = hwnd;
  if (!d3d || FAILED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device))) {
    printf("FAIL: device creation\n");
    return 1;
  }

  auto setup = [] {
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
  };
  setup();
  run_frames(256, 256, 60, "256x256");

  pp.BackBufferWidth = 128;
  pp.BackBufferHeight = 128;
  check("Reset to 128x128", SUCCEEDED(device->Reset(&pp)));
  setup();
  IDirect3DSurface9 *bb = nullptr;
  D3DSURFACE_DESC desc = {};
  if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
    bb->GetDesc(&desc);
    bb->Release();
  }
  check("back buffer resized", desc.Width == 128 && desc.Height == 128);
  run_frames(128, 128, 30, "128x128");

  device->Release();
  d3d->Release();
  DestroyWindow(hwnd);
  printf("%s: %d failure(s)\n", failures ? "failed" : "passed", failures);
  return failures;
}
