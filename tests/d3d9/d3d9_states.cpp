#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>

// type-punning through a pointer cast is undefined, and the optimizer did drop the float stores
static DWORD
f2dw(float f) {
  DWORD d;
  memcpy(&d, &f, sizeof(d));
  return d;
}

static int failures = 0;
static IDirect3DDevice9 *device;
static IDirect3DSurface9 *rt, *readback;

static bool
close(DWORD a, DWORD b, int tolerance) {
  for (int shift = 0; shift < 32; shift += 8) {
    int x = (a >> shift) & 0xFF, y = (b >> shift) & 0xFF;
    if (x - y > tolerance || y - x > tolerance)
      return false;
  }
  return true;
}

static DWORD
pixel(int x, int y) {
  if (FAILED(device->GetRenderTargetData(rt, readback)))
    return 0xDEADBEEF;
  D3DLOCKED_RECT lr;
  if (FAILED(readback->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
    return 0xDEADBEEF;
  DWORD value = ((DWORD *)((char *)lr.pBits + y * lr.Pitch))[x];
  readback->UnlockRect();
  return value;
}

static void
expect(const char *name, int x, int y, DWORD want, int tolerance = 3) {
  DWORD got = pixel(x, y);
  bool ok = close(got, want, tolerance);
  if (ok)
    printf("PASS: %s\n", name);
  else
    printf("FAIL: %s (got %08lx want %08lx)\n", name, got, want);
  failures += !ok;
}

static const DWORD *
compile(const char *source, const char *target) {
  ID3DBlob *code = nullptr, *errors = nullptr;
  if (FAILED(D3DCompile(source, strlen(source), nullptr, nullptr, nullptr, "main", target, 0, 0, &code, &errors))) {
    printf("FAIL: compile %s: %s\n", target, errors ? (const char *)errors->GetBufferPointer() : "?");
    failures++;
    return nullptr;
  }
  return (const DWORD *)code->GetBufferPointer();
}

struct Vertex {
  float x, y, z;
  DWORD color;
};

static void
quad(Vertex *v, float x0, float y0, float x1, float y1, float z, DWORD color) {
  v[0] = {x0, y0, z, color};
  v[1] = {x0, y1, z, color};
  v[2] = {x1, y0, z, color};
  v[3] = {x1, y1, z, color};
}

static void
draw_quad(float x0, float y0, float x1, float y1, float z, DWORD color) {
  Vertex v[4];
  quad(v, x0, y0, x1, y1, z, color);
  device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vertex));
}

static void
clear(DWORD color) {
  device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, color, 1.0f, 0);
}

int WINAPI
WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
  WNDCLASSW wc = {};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = instance;
  wc.lpszClassName = L"d3d9_states";
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
  device->CreateRenderTarget(64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr);
  device->CreateOffscreenPlainSurface(64, 64, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr);
  device->SetRenderTarget(0, rt);
  device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  device->SetRenderState(D3DRS_ZENABLE, FALSE);
  device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);

  const char *vs3 = "struct O { float4 p : POSITION; float4 c : COLOR0; };"
                    "O main(float3 p : POSITION, float4 c : COLOR0) { O o; o.p = float4(p, 1); o.c = c; return o; }";
  const char *ps3 = "float4 main(float4 c : COLOR0) : COLOR0 { return c; }";
  IDirect3DVertexShader9 *vs = nullptr, *vs20 = nullptr;
  IDirect3DPixelShader9 *ps = nullptr, *ps20 = nullptr;
  if (auto code = compile(vs3, "vs_3_0"))
    device->CreateVertexShader(code, &vs);
  if (auto code = compile(ps3, "ps_3_0"))
    device->CreatePixelShader(code, &ps);
  if (auto code = compile(vs3, "vs_2_0"))
    device->CreateVertexShader(code, &vs20);
  if (auto code = compile(ps3, "ps_2_0"))
    device->CreatePixelShader(code, &ps20);
  if (!vs || !ps || !vs20 || !ps20) {
    printf("FAIL: shader creation\n");
    return 1;
  }
  device->SetVertexShader(vs);
  device->SetPixelShader(ps);
  device->BeginScene();

  device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
  device->SetRenderState(D3DRS_ALPHAREF, 0x80);
  device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
  clear(0xFF000000);
  draw_quad(-1, -1, 1, 1, 0, 0x40FF0000);
  expect("alpha test rejects", 32, 32, 0xFF000000);
  device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_LESS);
  draw_quad(-1, -1, 1, 1, 0, 0x40FF0000);
  expect("alpha test accepts", 32, 32, 0x40FF0000);
  device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);

  device->SetVertexShader(vs20);
  device->SetPixelShader(ps20);
  float start = 0.0f, end = 1.0f, density = 2.0f;
  device->SetRenderState(D3DRS_FOGENABLE, TRUE);
  device->SetRenderState(D3DRS_FOGCOLOR, 0xFF0000FF);
  device->SetRenderState(D3DRS_FOGTABLEMODE, D3DFOG_LINEAR);
  device->SetRenderState(D3DRS_FOGSTART, f2dw(start));
  device->SetRenderState(D3DRS_FOGEND, f2dw(end));
  clear(0xFF000000);
  draw_quad(-1, -1, 1, 1, 0.25f, 0xFFFF0000);
  // f = (end - z) / (end - start) = 0.75
  expect("linear table fog", 32, 32, 0xFFBF0040);
  device->SetRenderState(D3DRS_FOGTABLEMODE, D3DFOG_EXP);
  device->SetRenderState(D3DRS_FOGDENSITY, f2dw(density));
  draw_quad(-1, -1, 1, 1, 0.5f, 0xFFFF0000);
  // f = exp(-2 * 0.5) = 0.368
  expect("exp table fog", 32, 32, 0xFF5E00A1);
  device->SetRenderState(D3DRS_FOGENABLE, FALSE);
  device->SetVertexShader(vs);
  device->SetPixelShader(ps);

  float plane[4] = {1, 0, 0, 0};
  device->SetClipPlane(0, plane);
  device->SetRenderState(D3DRS_CLIPPLANEENABLE, D3DCLIPPLANE0);
  clear(0xFF000000);
  draw_quad(-1, -1, 1, 1, 0, 0xFF00FF00);
  expect("clip plane keeps x > 0", 48, 32, 0xFF00FF00);
  expect("clip plane removes x < 0", 16, 32, 0xFF000000);
  device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);

  // window x 0.4 .. 1.4 covers the D3D9 pixel center at x = 1 but not 0
  clear(0xFF000000);
  float x0 = -1 + 2 * 0.4f / 64, x1 = -1 + 2 * 1.4f / 64;
  draw_quad(x0, -1, x1, 1, 0, 0xFFFFFFFF);
  expect("half-pixel offset lights column 1", 1, 32, 0xFFFFFFFF);
  expect("half-pixel offset leaves column 0", 0, 32, 0xFF000000);

  float size = 8.0f;
  device->SetRenderState(D3DRS_POINTSIZE, f2dw(size));
  clear(0xFF000000);
  Vertex point = {0, 0, 0, 0xFFFFFF00};
  device->DrawPrimitiveUP(D3DPT_POINTLIST, 1, &point, sizeof(Vertex));
  expect("point center", 32, 32, 0xFFFFFF00);
  expect("point size 8", 29, 29, 0xFFFFFF00);
  expect("point edge", 22, 22, 0xFF000000);

  device->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_FLAT);
  clear(0xFF000000);
  Vertex tri[3] = {{-1, -1, 0, 0xFFFF0000}, {-1, 3, 0, 0xFF00FF00}, {3, -1, 0, 0xFF0000FF}};
  device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(Vertex));
  expect("flat shading uses the first vertex", 50, 10, 0xFFFF0000, 1);
  device->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);

  device->EndScene();
  vs->Release();
  ps->Release();
  vs20->Release();
  ps20->Release();
  readback->Release();
  rt->Release();
  device->Release();
  d3d->Release();
  printf("%s: %d failure(s)\n", failures ? "failed" : "passed", failures);
  return failures;
}
