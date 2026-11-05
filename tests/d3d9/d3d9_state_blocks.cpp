#include <windows.h>
#include <d3d9.h>
#include <cstdio>

static int failures = 0;
static IDirect3DDevice9 *device;

static void
check(const char *name, bool ok) {
  printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
  failures += !ok;
}

static DWORD
rs(D3DRENDERSTATETYPE state) {
  DWORD value = 0;
  device->GetRenderState(state, &value);
  return value;
}

static DWORD
tss(DWORD stage, D3DTEXTURESTAGESTATETYPE state) {
  DWORD value = 0;
  device->GetTextureStageState(stage, state, &value);
  return value;
}

int WINAPI
WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
  WNDCLASSW wc = {};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = instance;
  wc.lpszClassName = L"d3d9_state_blocks";
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

  // recording
  IDirect3DStateBlock9 *recorded = nullptr;
  float constant[4] = {1, 2, 3, 4}, read[4] = {};
  device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
  check("BeginStateBlock", device->BeginStateBlock() == D3D_OK);
  check("nested BeginStateBlock fails", device->BeginStateBlock() == D3DERR_INVALIDCALL);
  device->SetRenderState(D3DRS_ZFUNC, D3DCMP_NEVER);
  device->SetVertexShaderConstantF(5, constant, 1);
  device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_ADD);
  check("recorded state is not applied", rs(D3DRS_ZFUNC) == D3DCMP_LESSEQUAL);
  device->GetVertexShaderConstantF(5, read, 1);
  check("recorded constant is not applied", read[0] == 0);
  check("EndStateBlock", device->EndStateBlock(&recorded) == D3D_OK && recorded);
  IDirect3DStateBlock9 *none = nullptr;
  check("EndStateBlock without Begin fails", device->EndStateBlock(&none) == D3DERR_INVALIDCALL);

  device->SetRenderState(D3DRS_FOGCOLOR, 0x123456);
  check("Apply", recorded->Apply() == D3D_OK);
  device->GetVertexShaderConstantF(5, read, 1);
  check("Apply sets recorded states", rs(D3DRS_ZFUNC) == D3DCMP_NEVER && read[2] == 3 &&
                                          tss(1, D3DTSS_COLOROP) == D3DTOP_ADD);
  check("Apply leaves other states", rs(D3DRS_FOGCOLOR) == 0x123456);

  device->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
  recorded->Capture();
  device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS);
  recorded->Apply();
  check("Capture refreshes recorded states", rs(D3DRS_ZFUNC) == D3DCMP_ALWAYS);
  device->SetRenderState(D3DRS_FOGCOLOR, 0x654321);
  recorded->Capture();
  device->SetRenderState(D3DRS_FOGCOLOR, 0);
  recorded->Apply();
  check("Capture ignores unrecorded states", rs(D3DRS_FOGCOLOR) == 0);

  // D3DSBT_ALL keeps its objects alive
  IDirect3DTexture9 *texture = nullptr;
  device->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr);
  device->SetTexture(3, texture);
  D3DVIEWPORT9 viewport = {8, 8, 16, 16, 0, 1}, got_viewport = {};
  device->SetViewport(&viewport);
  IDirect3DStateBlock9 *all = nullptr;
  check("CreateStateBlock(ALL)", device->CreateStateBlock(D3DSBT_ALL, &all) == D3D_OK && all);
  device->SetTexture(3, nullptr);
  texture->Release();
  viewport = {0, 0, 64, 64, 0, 1};
  device->SetViewport(&viewport);
  device->SetRenderState(D3DRS_ZFUNC, D3DCMP_GREATER);
  all->Apply();
  IDirect3DBaseTexture9 *bound = nullptr;
  device->GetTexture(3, &bound);
  check("ALL restores a texture it kept alive", bound != nullptr);
  if (bound)
    bound->Release();
  device->GetViewport(&got_viewport);
  check("ALL restores the viewport", got_viewport.X == 8 && got_viewport.Width == 16);
  check("ALL restores render states", rs(D3DRS_ZFUNC) == D3DCMP_ALWAYS);
  device->SetTexture(3, nullptr);
  all->Release();

  // pixel and vertex subsets
  device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
  device->SetRenderState(D3DRS_LIGHTING, TRUE);
  device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
  device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
  IDirect3DStateBlock9 *pixel = nullptr, *vertex = nullptr;
  device->CreateStateBlock(D3DSBT_PIXELSTATE, &pixel);
  device->LightEnable(0, TRUE);
  device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
  device->CreateStateBlock(D3DSBT_VERTEXSTATE, &vertex);
  device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
  device->SetRenderState(D3DRS_LIGHTING, FALSE);
  device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_ADD);
  device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 1);
  device->LightEnable(0, FALSE);
  device->SetFVF(D3DFVF_XYZRHW);
  pixel->Apply();
  check("PIXELSTATE restores pixel states", rs(D3DRS_SRCBLEND) == D3DBLEND_SRCALPHA &&
                                                tss(0, D3DTSS_COLOROP) == D3DTOP_MODULATE);
  check("PIXELSTATE leaves vertex states", rs(D3DRS_LIGHTING) == FALSE && tss(0, D3DTSS_TEXCOORDINDEX) == 1);
  vertex->Apply();
  BOOL enabled = FALSE;
  DWORD fvf = 0;
  device->GetLightEnable(0, &enabled);
  device->GetFVF(&fvf);
  check("VERTEXSTATE restores vertex states", rs(D3DRS_LIGHTING) == TRUE && tss(0, D3DTSS_TEXCOORDINDEX) == 0 &&
                                                  enabled && fvf == (D3DFVF_XYZ | D3DFVF_DIFFUSE));
  check("VERTEXSTATE leaves pixel states", tss(0, D3DTSS_COLOROP) == D3DTOP_MODULATE);
  pixel->Release();
  vertex->Release();

  check("Apply while recording fails", device->BeginStateBlock() == D3D_OK && recorded->Apply() == D3DERR_INVALIDCALL);
  IDirect3DStateBlock9 *empty = nullptr;
  device->EndStateBlock(&empty);
  if (empty)
    empty->Release();

  // a recorded block drives rendering
  IDirect3DSurface9 *rt = nullptr, *sys = nullptr;
  device->CreateRenderTarget(64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr);
  device->CreateOffscreenPlainSurface(64, 64, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr);
  device->SetRenderTarget(0, rt);
  viewport = {0, 0, 64, 64, 0, 1};
  device->SetViewport(&viewport);
  device->SetRenderState(D3DRS_ZENABLE, FALSE);
  device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
  IDirect3DStateBlock9 *tint = nullptr;
  device->BeginStateBlock();
  device->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFF336699);
  device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
  device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
  device->EndStateBlock(&tint);
  struct {
    float x, y, z, rhw;
    DWORD color;
  } v[4] = {{0, 0, 0, 1, 0xFFFFFFFF}, {64, 0, 0, 1, 0xFFFFFFFF}, {0, 64, 0, 1, 0xFFFFFFFF}, {64, 64, 0, 1, 0xFFFFFFFF}};
  device->BeginScene();
  tint->Apply();
  device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
  device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(v[0]));
  device->EndScene();
  DWORD pixel_value = 0;
  D3DLOCKED_RECT lr;
  if (SUCCEEDED(device->GetRenderTargetData(rt, sys)) && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
    pixel_value = ((DWORD *)((char *)lr.pBits + 32 * lr.Pitch))[32];
    sys->UnlockRect();
  }
  check("an applied block drives rendering", pixel_value == 0xFF336699);
  tint->Release();
  recorded->Release();
  sys->Release();
  rt->Release();

  device->Release();
  d3d->Release();
  printf("%s: %d failure(s)\n", failures ? "failed" : "passed", failures);
  return failures;
}
