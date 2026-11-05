#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <initializer_list>

static int failures = 0;
static IDirect3DDevice9 *device;

static bool
close(DWORD a, DWORD b, int tolerance) {
  for (int shift = 0; shift < 32; shift += 8) {
    int x = (a >> shift) & 0xFF, y = (b >> shift) & 0xFF;
    if (x - y > tolerance || y - x > tolerance)
      return false;
  }
  return true;
}

static void
check(const char *name, bool ok) {
  printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
  failures += !ok;
}

static DWORD
pixel(IDirect3DSurface9 *rt, int x, int y) {
  D3DSURFACE_DESC desc;
  rt->GetDesc(&desc);
  IDirect3DSurface9 *sys = nullptr;
  device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr);
  DWORD value = 0xDEADBEEF;
  D3DLOCKED_RECT lr;
  if (sys && SUCCEEDED(device->GetRenderTargetData(rt, sys)) && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
    char *row = (char *)lr.pBits + y * lr.Pitch;
    if (desc.Format == D3DFMT_R5G6B5) {
      WORD v = ((WORD *)row)[x];
      value = 0xFF000000 | ((v >> 11) * 255 / 31) << 16 | (((v >> 5) & 63) * 255 / 63) << 8 | ((v & 31) * 255 / 31);
    } else {
      value = ((DWORD *)row)[x];
    }
    sys->UnlockRect();
  }
  if (sys)
    sys->Release();
  return value;
}

static void
expect(const char *name, IDirect3DSurface9 *rt, int x, int y, DWORD want, int tolerance = 3) {
  DWORD got = pixel(rt, x, y);
  bool ok = close(got, want, tolerance);
  if (ok)
    printf("PASS: %s\n", name);
  else
    printf("FAIL: %s (got %08lx want %08lx)\n", name, got, want);
  failures += !ok;
}

static IDirect3DSurface9 *
target(UINT width, UINT height, D3DFORMAT format = D3DFMT_A8R8G8B8, D3DMULTISAMPLE_TYPE ms = D3DMULTISAMPLE_NONE) {
  IDirect3DSurface9 *s = nullptr;
  if (FAILED(device->CreateRenderTarget(width, height, format, ms, 0, FALSE, &s, nullptr)))
    printf("FAIL: CreateRenderTarget %ux%u format %u\n", width, height, format);
  return s;
}

static void
fill(IDirect3DSurface9 *s, LONG x0, LONG y0, LONG x1, LONG y1, D3DCOLOR color) {
  RECT r = {x0, y0, x1, y1};
  device->ColorFill(s, &r, color);
}

int WINAPI
WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
  WNDCLASSW wc = {};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = instance;
  wc.lpszClassName = L"d3d9_stretch";
  RegisterClassW(&wc);
  HWND hwnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 128, 128, nullptr, nullptr, instance, nullptr);

  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  D3DPRESENT_PARAMETERS pp = {};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.BackBufferWidth = 64;
  pp.BackBufferHeight = 64;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.EnableAutoDepthStencil = TRUE;
  pp.AutoDepthStencilFormat = D3DFMT_D24S8;
  pp.hDeviceWindow = hwnd;
  if (!d3d || FAILED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device))) {
    printf("FAIL: device creation\n");
    return 1;
  }

  IDirect3DSurface9 *quads = target(2, 2), *big = target(64, 64);
  fill(quads, 0, 0, 1, 1, 0xFFFF0000);
  fill(quads, 1, 0, 2, 1, 0xFF00FF00);
  fill(quads, 0, 1, 1, 2, 0xFF0000FF);
  fill(quads, 1, 1, 2, 2, 0xFFFFFFFF);
  check("point upscale", SUCCEEDED(device->StretchRect(quads, nullptr, big, nullptr, D3DTEXF_POINT)));
  expect("point upscale top-left", big, 16, 16, 0xFFFF0000);
  expect("point upscale top-right", big, 48, 16, 0xFF00FF00);
  expect("point upscale bottom-right", big, 48, 48, 0xFFFFFFFF);

  fill(big, 0, 0, 64, 64, 0xFF000000);
  RECT one = {1, 0, 2, 1}, quarter = {32, 32, 64, 64};
  device->StretchRect(quads, &one, big, &quarter, D3DTEXF_POINT);
  expect("source and destination rects", big, 40, 40, 0xFF00FF00);
  expect("outside the destination rect", big, 16, 16, 0xFF000000);

  IDirect3DSurface9 *ramp = target(2, 1);
  fill(ramp, 0, 0, 1, 1, 0xFF000000);
  fill(ramp, 1, 0, 2, 1, 0xFFFFFFFF);
  device->StretchRect(ramp, nullptr, big, nullptr, D3DTEXF_LINEAR);
  expect("linear filter blends", big, 32, 32, 0xFF838383, 6);
  device->StretchRect(ramp, nullptr, big, nullptr, D3DTEXF_POINT);
  expect("point filter does not", big, 31, 32, 0xFF000000);

  IDirect3DSurface9 *rgb565 = target(64, 64, D3DFMT_R5G6B5);
  if (rgb565) {
    check("format conversion", SUCCEEDED(device->StretchRect(quads, nullptr, rgb565, nullptr, D3DTEXF_POINT)));
    expect("converted to R5G6B5", rgb565, 16, 48, 0xFF0000FF);
    rgb565->Release();
  }

  IDirect3DSurface9 *ms = target(64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_4_SAMPLES), *small = target(32, 32);
  if (ms && small) {
    device->SetRenderTarget(0, ms);
    device->SetDepthStencilSurface(nullptr);
    device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF336699, 1.0f, 0);
    check("multisampled source with scaling", SUCCEEDED(device->StretchRect(ms, nullptr, small, nullptr, D3DTEXF_LINEAR)));
    expect("resolved and scaled", small, 16, 16, 0xFF336699);
  }

  IDirect3DSurface9 *depth = nullptr, *depth_small = nullptr;
  device->CreateDepthStencilSurface(64, 64, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &depth, nullptr);
  device->CreateDepthStencilSurface(32, 32, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &depth_small, nullptr);
  check("scaling depth fails", device->StretchRect(depth, nullptr, depth_small, nullptr, D3DTEXF_POINT) == D3DERR_INVALIDCALL);

  for (IDirect3DSurface9 *s : {quads, big, ramp, ms, small, depth, depth_small})
    if (s)
      s->Release();
  device->Release();
  d3d->Release();
  printf("%s: %d failure(s)\n", failures ? "failed" : "passed", failures);
  return failures;
}
