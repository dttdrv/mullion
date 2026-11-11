#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cstring>

static int failures = 0;

static void
check(const char *name, bool ok) {
  printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok)
    failures++;
}

static DWORD
read_rt(IDirect3DDevice9 *device, IDirect3DSurface9 *rt, int x, int y) {
  D3DSURFACE_DESC desc;
  rt->GetDesc(&desc);
  IDirect3DSurface9 *sysmem = nullptr;
  device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &sysmem, nullptr);
  DWORD value = 0xDEADBEEF;
  D3DLOCKED_RECT lr;
  if (sysmem && SUCCEEDED(device->GetRenderTargetData(rt, sysmem)) &&
      SUCCEEDED(sysmem->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
    value = ((DWORD *)((char *)lr.pBits + y * lr.Pitch))[x];
    sysmem->UnlockRect();
  }
  if (sysmem)
    sysmem->Release();
  return value;
}

static DWORD
pattern(int level, int face, int x, int y) {
  return 0xFF000000 | (level << 20) | (face << 16) | (x << 8) | y;
}

static void
fill(D3DLOCKED_RECT &lr, int w, int h, int level, int face) {
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      ((DWORD *)((char *)lr.pBits + y * lr.Pitch))[x] = pattern(level, face, x, y);
}

int WINAPI
WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
  WNDCLASSW wc = {};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = instance;
  wc.lpszClassName = L"d3d9_resources";
  RegisterClassW(&wc);
  HWND hwnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 128, 128, nullptr, nullptr, instance, nullptr);

  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  D3DPRESENT_PARAMETERS pp = {};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.BackBufferWidth = 64;
  pp.BackBufferHeight = 64;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.hDeviceWindow = hwnd;
  IDirect3DDevice9 *device = nullptr;
  if (!d3d || FAILED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device))) {
    printf("FAIL: device creation\n");
    return 1;
  }

  IDirect3DSurface9 *rt = nullptr;
  device->CreateRenderTarget(64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr);

  {
    IDirect3DTexture9 *src = nullptr, *dst = nullptr;
    device->CreateTexture(64, 64, 3, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &src, nullptr);
    device->CreateTexture(32, 32, 2, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &dst, nullptr);
    check("create 2D textures", src && dst);
    check("level count", src && src->GetLevelCount() == 3);
    D3DSURFACE_DESC ld;
    check("level desc", src && SUCCEEDED(src->GetLevelDesc(2, &ld)) && ld.Width == 16 && ld.Height == 16);
    for (int level = 0; level < 3 && src; level++) {
      D3DLOCKED_RECT lr;
      if (SUCCEEDED(src->LockRect(level, &lr, nullptr, 0))) {
        fill(lr, 64 >> level, 64 >> level, level, 0);
        src->UnlockRect(level);
      }
    }
    check("update texture skips top level", src && dst && SUCCEEDED(device->UpdateTexture(src, dst)));
    IDirect3DSurface9 *level1 = nullptr;
    if (dst && SUCCEEDED(dst->GetSurfaceLevel(1, &level1))) {
      RECT r = {0, 0, 16, 16};
      device->StretchRect(level1, &r, rt, &r, D3DTEXF_NONE);
      check("mip level 1 holds source level 2", read_rt(device, rt, 3, 5) == pattern(2, 0, 3, 5));
      dst->AddRef();
      check("surface refs forward to texture", dst->Release() == 2);
      level1->Release();
    }
    if (src)
      src->Release();
    if (dst)
      dst->Release();
  }

  {
    IDirect3DCubeTexture9 *src = nullptr, *dst = nullptr;
    device->CreateCubeTexture(16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &src, nullptr);
    device->CreateCubeTexture(16, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &dst, nullptr);
    check("create cube textures", src && dst);
    for (int face = 0; face < 6 && src; face++) {
      D3DLOCKED_RECT lr;
      if (SUCCEEDED(src->LockRect((D3DCUBEMAP_FACES)face, 0, &lr, nullptr, 0))) {
        fill(lr, 16, 16, 0, face);
        src->UnlockRect((D3DCUBEMAP_FACES)face, 0);
      }
    }
    check("update cube texture", src && dst && SUCCEEDED(device->UpdateTexture(src, dst)));
    IDirect3DSurface9 *face3 = nullptr;
    if (dst && SUCCEEDED(dst->GetCubeMapSurface(D3DCUBEMAP_FACE_NEGATIVE_Y, 0, &face3))) {
      RECT r = {0, 0, 16, 16};
      device->StretchRect(face3, &r, rt, &r, D3DTEXF_NONE);
      check("cube face 3", read_rt(device, rt, 7, 2) == pattern(0, 3, 7, 2));
      face3->Release();
    }
    if (src)
      src->Release();
    if (dst)
      dst->Release();
  }

  {
    IDirect3DVolumeTexture9 *src = nullptr, *dst = nullptr;
    device->CreateVolumeTexture(8, 8, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &src, nullptr);
    device->CreateVolumeTexture(8, 8, 4, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &dst, nullptr);
    check("create volume textures", src && dst);
    D3DLOCKED_BOX lb;
    if (src && SUCCEEDED(src->LockBox(0, &lb, nullptr, 0))) {
      for (int z = 0; z < 4; z++)
        for (int y = 0; y < 8; y++)
          for (int x = 0; x < 8; x++)
            ((DWORD *)((char *)lb.pBits + z * lb.SlicePitch + y * lb.RowPitch))[x] = pattern(z, 0, x, y);
      src->UnlockBox(0);
    }
    check("update volume texture", src && dst && SUCCEEDED(device->UpdateTexture(src, dst)));
    D3DBOX box = {2, 3, 4, 5, 2, 3};
    if (dst && SUCCEEDED(dst->LockBox(0, &lb, &box, D3DLOCK_READONLY))) {
      check("volume readback", ((DWORD *)lb.pBits)[0] == pattern(2, 0, 2, 3));
      dst->UnlockBox(0);
    } else {
      check("volume readback", false);
    }
    if (src)
      src->Release();
    if (dst)
      dst->Release();
  }

  {
    IDirect3DTexture9 *tex = nullptr;
    device->CreateTexture(16, 16, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, nullptr);
    D3DLOCKED_RECT lr;
    if (tex && SUCCEEDED(tex->LockRect(0, &lr, nullptr, D3DLOCK_DISCARD))) {
      fill(lr, 16, 16, 1, 1);
      tex->UnlockRect(0);
    }
    if (tex && SUCCEEDED(tex->LockRect(0, &lr, nullptr, D3DLOCK_READONLY))) {
      check("dynamic texture round trip", ((DWORD *)((char *)lr.pBits + 9 * lr.Pitch))[4] == pattern(1, 1, 4, 9));
      tex->UnlockRect(0);
    } else {
      check("dynamic texture round trip", false);
    }
    IDirect3DTexture9 *plain = nullptr;
    device->CreateTexture(16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &plain, nullptr);
    check("non-dynamic default texture refuses lock", plain && FAILED(plain->LockRect(0, &lr, nullptr, 0)));
    IDirect3DTexture9 *autogen = nullptr;
    device->CreateTexture(64, 64, 0, D3DUSAGE_AUTOGENMIPMAP | D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                          D3DPOOL_DEFAULT, &autogen, nullptr);
    check("autogen exposes one level", autogen && autogen->GetLevelCount() == 1);
    if (tex)
      tex->Release();
    if (plain)
      plain->Release();
    if (autogen)
      autogen->Release();
  }

  {
    IDirect3DVertexBuffer9 *vb = nullptr;
    device->CreateVertexBuffer(1024, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb, nullptr);
    check("create vertex buffer", vb != nullptr);
    void *data = nullptr;
    if (vb && SUCCEEDED(vb->Lock(0, 0, &data, 0))) {
      memset(data, 0x5A, 1024);
      vb->Unlock();
    }
    if (vb && SUCCEEDED(vb->Lock(512, 16, &data, D3DLOCK_NOOVERWRITE))) {
      check("nooverwrite keeps contents", ((unsigned char *)data)[0] == 0x5A);
      vb->Unlock();
    }
    D3DVERTEXBUFFER_DESC desc;
    check("vertex buffer desc", vb && SUCCEEDED(vb->GetDesc(&desc)) && desc.Size == 1024);
    check("lock out of range fails", vb && FAILED(vb->Lock(1000, 100, &data, 0)));
    check("unlock without lock fails", vb && FAILED(vb->Unlock()));
    IDirect3DIndexBuffer9 *ib = nullptr;
    device->CreateIndexBuffer(256, 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, nullptr);
    check("create index buffer", ib != nullptr);
    if (vb)
      vb->Release();
    if (ib)
      ib->Release();
  }

  if (rt)
    rt->Release();
  device->Release();
  d3d->Release();
  printf("%s: %d failure(s)\n", failures ? "failed" : "passed", failures);
  return failures;
}
