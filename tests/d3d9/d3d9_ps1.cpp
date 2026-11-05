#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cstring>
#include <vector>

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

static void
expect(const char *name, DWORD want, int tolerance = 3) {
  DWORD got = 0xDEADBEEF;
  D3DLOCKED_RECT lr;
  if (SUCCEEDED(device->GetRenderTargetData(rt, readback)) &&
      SUCCEEDED(readback->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
    got = ((DWORD *)((char *)lr.pBits + 32 * lr.Pitch))[32];
    readback->UnlockRect();
  }
  bool ok = close(got, want, tolerance);
  if (ok)
    printf("PASS: %s\n", name);
  else
    printf("FAIL: %s (got %08lx want %08lx)\n", name, got, want);
  failures += !ok;
}

// SM1 tokens: no instruction length, t# is register type 3
enum { R_TEMP = 0, R_INPUT = 1, R_CONST = 2, R_TEX = 3 };
static DWORD
reg(DWORD type, DWORD n) {
  return 0x80000000 | ((type & 7) << 28) | ((type & 0x18) << 8) | n;
}
static DWORD
dst(DWORD type, DWORD n, DWORD mask = 0xF, int shift = 0) {
  return reg(type, n) | (mask << 16) | ((shift & 0xF) << 24);
}
static DWORD
src(DWORD type, DWORD n) {
  return reg(type, n) | (0xE4 << 16);
}
enum { MOV = 1, MUL = 5, TEXBEM = 67, TEXLD = 66 };

static IDirect3DPixelShader9 *
shader(std::vector<DWORD> tokens) {
  tokens.push_back(0x0000FFFF);
  IDirect3DPixelShader9 *ps = nullptr;
  if (FAILED(device->CreatePixelShader(tokens.data(), &ps))) {
    printf("FAIL: CreatePixelShader %08lx\n", tokens[0]);
    failures++;
  }
  return ps;
}

static IDirect3DTexture9 *
texture(UINT width, const DWORD *texels) {
  IDirect3DTexture9 *t = nullptr;
  device->CreateTexture(width, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr);
  D3DLOCKED_RECT lr;
  t->LockRect(0, &lr, nullptr, 0);
  memcpy(lr.pBits, texels, width * 4);
  t->UnlockRect(0);
  return t;
}

struct Vertex {
  float x, y, z, rhw;
  DWORD color;
  float t0[3];
  float t1[2];
};

static void
draw(DWORD color, const float *t0, const float *t1 = nullptr) {
  static const float zero[3] = {};
  if (!t1)
    t1 = zero;
  Vertex v[4];
  float corners[4][2] = {{0, 0}, {64, 0}, {0, 64}, {64, 64}};
  for (int i = 0; i < 4; i++)
    v[i] = {corners[i][0], corners[i][1], 0, 1, color, {t0[0], t0[1], t0[2]}, {t1[0], t1[1]}};
  device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
  device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vertex));
}

static DWORD
f2dw(float f) {
  DWORD d;
  memcpy(&d, &f, 4);
  return d;
}

int WINAPI
WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
  WNDCLASSW wc = {};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = instance;
  wc.lpszClassName = L"d3d9_ps1";
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
  if (!d3d || FAILED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device))) {
    printf("FAIL: device creation\n");
    return 1;
  }
  device->CreateRenderTarget(64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr);
  device->CreateOffscreenPlainSurface(64, 64, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr);
  device->SetRenderTarget(0, rt);
  device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  device->SetRenderState(D3DRS_ZENABLE, FALSE);
  device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX2 | D3DFVF_TEXCOORDSIZE3(0) | D3DFVF_TEXCOORDSIZE2(1));
  for (DWORD s = 0; s < 2; s++) {
    device->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
  }
  device->BeginScene();

  const DWORD rgbw[4] = {0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFFFF};
  IDirect3DTexture9 *strip = texture(4, rgbw);
  const float texel2[3] = {0.625f, 0.5f, 0};

  // ps_1_1: tex t0; mul r0, t0, v0
  auto *ps11 = shader({0xFFFF0101, TEXLD, dst(R_TEX, 0), MUL, dst(R_TEMP, 0), src(R_TEX, 0), src(R_INPUT, 0)});
  device->SetTexture(0, strip);
  device->SetPixelShader(ps11);
  draw(0xFF808080, texel2);
  expect("ps_1_1 tex modulates diffuse", 0xFF000080);

  // ps_1_4: texld r0, t0; mul_x2 r0, r0, v0
  auto *ps14 = shader({0xFFFF0104, TEXLD, dst(R_TEMP, 0), src(R_TEX, 0), MUL, dst(R_TEMP, 0, 0xF, 1), src(R_TEMP, 0), src(R_INPUT, 0)});
  device->SetPixelShader(ps14);
  draw(0xFF404040, texel2);
  expect("ps_1_4 texld with a result shift", 0xFF000080);

  // ps_1_1: tex t0; texbem t1, t0; mov r0, t1. Stage 0's red channel offsets stage 1's u by BUMPENVMAT00.
  const DWORD bump_texel = 0xFFFF0000;
  IDirect3DTexture9 *bump = texture(1, &bump_texel);
  auto *bem = shader({0xFFFF0101, TEXLD, dst(R_TEX, 0), TEXBEM, dst(R_TEX, 1), src(R_TEX, 0), MOV, dst(R_TEMP, 0), src(R_TEX, 1)});
  device->SetTexture(0, bump);
  device->SetTexture(1, strip);
  device->SetPixelShader(bem);
  const float t1[2] = {0.125f, 0.5f};
  device->SetTextureStageState(1, D3DTSS_BUMPENVMAT00, f2dw(0.25f));
  draw(0xFFFFFFFF, texel2, t1);
  expect("texbem offsets by the bump matrix", 0xFF00FF00);
  device->SetTextureStageState(1, D3DTSS_BUMPENVMAT00, f2dw(0.0f));
  draw(0xFFFFFFFF, texel2, t1);
  expect("texbem without a bump matrix", 0xFFFF0000);
  device->SetTexture(1, nullptr);

  // ps_1_1 tex on a cube map: the sampler type comes from the bound texture
  IDirect3DCubeTexture9 *cube = nullptr;
  device->CreateCubeTexture(1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cube, nullptr);
  for (int face = 0; face < 6; face++) {
    D3DLOCKED_RECT lr;
    cube->LockRect((D3DCUBEMAP_FACES)face, 0, &lr, nullptr, 0);
    *(DWORD *)lr.pBits = face == D3DCUBEMAP_FACE_NEGATIVE_Y ? 0xFF00FFFF : 0xFF000000 | (face * 0x30);
    cube->UnlockRect((D3DCUBEMAP_FACES)face, 0);
  }
  device->SetTexture(0, cube);
  device->SetPixelShader(ps11);
  const float down[3] = {0, -1, 0};
  draw(0xFFFFFFFF, down);
  expect("ps_1_1 tex samples a cube map", 0xFF00FFFF);

  // D3DTTFF_PROJECTED divides ps_1_1 tex coordinates by the last counted component
  device->SetTexture(0, strip);
  device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT3 | D3DTTFF_PROJECTED);
  const float projected[3] = {1.5f, 0.5f, 4};
  draw(0xFFFFFFFF, projected);
  expect("projected tex", 0xFF00FF00);
  device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
  draw(0xFFFFFFFF, projected);
  expect("unprojected tex", 0xFFFFFFFF);

  device->EndScene();
  device->SetPixelShader(nullptr);
  for (IUnknown *object : {(IUnknown *)ps11, (IUnknown *)ps14, (IUnknown *)bem, (IUnknown *)strip, (IUnknown *)bump,
                           (IUnknown *)cube})
    if (object)
      object->Release();
  readback->Release();
  rt->Release();
  device->Release();
  d3d->Release();
  printf("%s: %d failure(s)\n", failures ? "failed" : "passed", failures);
  return failures;
}
