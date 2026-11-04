#include <windows.h>
#include <d3d9.h>
#include <cstdio>

static int failures = 0;

static void
check(const char *name, bool ok) {
  printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok)
    failures++;
}

static DWORD
pixel(IDirect3DSurface9 *surface, int x, int y) {
  D3DLOCKED_RECT lr;
  if (FAILED(surface->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
    return 0xDEADBEEF;
  DWORD value = ((DWORD *)((char *)lr.pBits + y * lr.Pitch))[x];
  surface->UnlockRect();
  return value;
}

static IDirect3DSurface9 *
readback(IDirect3DDevice9 *device, IDirect3DSurface9 *rt) {
  D3DSURFACE_DESC desc;
  rt->GetDesc(&desc);
  IDirect3DSurface9 *sysmem = nullptr;
  device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &sysmem, nullptr);
  if (!sysmem || FAILED(device->GetRenderTargetData(rt, sysmem))) {
    if (sysmem)
      sysmem->Release();
    return nullptr;
  }
  return sysmem;
}

int WINAPI
WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
  WNDCLASSW wc = {};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = instance;
  wc.lpszClassName = L"d3d9_readback";
  RegisterClassW(&wc);
  HWND hwnd = CreateWindowW(
      wc.lpszClassName, L"d3d9_readback", WS_OVERLAPPEDWINDOW, 0, 0, 256, 256, nullptr, nullptr, instance, nullptr
  );

  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  D3DPRESENT_PARAMETERS pp = {};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.BackBufferWidth = 128;
  pp.BackBufferHeight = 128;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.hDeviceWindow = hwnd;
  IDirect3DDevice9 *device = nullptr;
  if (!d3d || FAILED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device))) {
    printf("FAIL: device creation\n");
    return 1;
  }

  IDirect3DSurface9 *backbuffer = nullptr;
  device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer);

  {
    device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF102030, 1.0f, 0);
    D3DRECT rect = {32, 32, 96, 96};
    device->Clear(1, &rect, D3DCLEAR_TARGET, 0xFFA0B0C0, 1.0f, 0);
    IDirect3DSurface9 *copy = readback(device, backbuffer);
    check("backbuffer readback", copy != nullptr);
    if (copy) {
      check("full clear color", (pixel(copy, 2, 2) & 0xFFFFFF) == 0x102030);
      check("rect clear color", (pixel(copy, 64, 64) & 0xFFFFFF) == 0xA0B0C0);
      check("rect clear bounds", (pixel(copy, 31, 64) & 0xFFFFFF) == 0x102030);
      copy->Release();
    }
  }

  {
    D3DVIEWPORT9 vp = {16, 16, 32, 32, 0.0f, 1.0f};
    device->SetViewport(&vp);
    device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF00FF00, 1.0f, 0);
    IDirect3DSurface9 *copy = readback(device, backbuffer);
    if (copy) {
      check("viewport clear inside", (pixel(copy, 20, 20) & 0xFFFFFF) == 0x00FF00);
      check("viewport clear outside", (pixel(copy, 60, 20) & 0xFFFFFF) == 0x102030);
      copy->Release();
    }
    vp = {0, 0, 128, 128, 0.0f, 1.0f};
    device->SetViewport(&vp);
  }

  IDirect3DSurface9 *rt = nullptr;
  device->CreateRenderTarget(64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, TRUE, &rt, nullptr);
  check("lockable render target", rt != nullptr);
  if (rt) {
    check("color fill", SUCCEEDED(device->ColorFill(rt, nullptr, 0x80406080)));
    check("lock default pool reads GPU data", pixel(rt, 10, 10) == 0x80406080);

    D3DLOCKED_RECT lr;
    RECT rect = {4, 4, 8, 8};
    if (SUCCEEDED(rt->LockRect(&lr, &rect, 0))) {
      ((DWORD *)lr.pBits)[0] = 0x11223344;
      rt->UnlockRect();
    }
    check("lock write uploads", pixel(rt, 4, 4) == 0x11223344);
    check("lock write keeps neighbors", pixel(rt, 5, 4) == 0x80406080);
  }

  IDirect3DSurface9 *sysmem = nullptr, *target = nullptr;
  device->CreateOffscreenPlainSurface(64, 64, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sysmem, nullptr);
  device->CreateRenderTarget(64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &target, nullptr);
  if (sysmem && target) {
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(sysmem->LockRect(&lr, nullptr, 0))) {
      for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++)
          ((DWORD *)((char *)lr.pBits + y * lr.Pitch))[x] = 0xFF000000 | (x << 16) | (y << 8);
      sysmem->UnlockRect();
    }
    check("non-lockable RT refuses lock", FAILED(target->LockRect(&lr, nullptr, 0)));
    device->ColorFill(target, nullptr, 0xFFFFFFFF);
    RECT src = {0, 0, 32, 32};
    POINT dst = {16, 16};
    check("update surface", SUCCEEDED(device->UpdateSurface(sysmem, &src, target, &dst)));
    IDirect3DSurface9 *copy = readback(device, target);
    if (copy) {
      check("update surface origin", pixel(copy, 16, 16) == 0xFF000000);
      check("update surface texel", pixel(copy, 16 + 5, 16 + 7) == (0xFF000000 | (5 << 16) | (7 << 8)));
      check("update surface outside", pixel(copy, 50, 50) == 0xFFFFFFFF);
      copy->Release();
    }

    if (rt) {
      RECT s = {0, 0, 16, 16};
      RECT d = {40, 40, 56, 56};
      check("stretch rect copy", SUCCEEDED(device->StretchRect(rt, &s, target, &d, D3DTEXF_NONE)));
      IDirect3DSurface9 *copy2 = readback(device, target);
      if (copy2) {
        check("stretch rect texel", pixel(copy2, 44, 44) == 0x11223344);
        check("stretch rect fill", pixel(copy2, 50, 50) == 0x80406080);
        copy2->Release();
      }
    }
  }

  if (sysmem)
    sysmem->Release();
  if (target)
    target->Release();
  if (rt)
    rt->Release();
  backbuffer->Release();
  device->Release();
  d3d->Release();
  printf("%s: %d failure(s)\n", failures ? "failed" : "passed", failures);
  return failures;
}
