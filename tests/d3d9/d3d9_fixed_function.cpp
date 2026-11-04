#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <initializer_list>

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

static void
check(const char *name, bool ok) {
  printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
  failures += !ok;
}

static void
clear() {
  device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
}

struct ScreenVertex {
  float x, y, z, rhw;
  DWORD color;
  float u, v;
};

// covers the pixel centers x0 .. x1 - 1, y0 .. y1 - 1
static void
screen_quad(float x0, float y0, float x1, float y1, DWORD color) {
  ScreenVertex v[4] = {
      {x0, y0, 0.5f, 1, color, 0, 0},
      {x1, y0, 0.5f, 1, color, 1, 0},
      {x0, y1, 0.5f, 1, color, 0, 1},
      {x1, y1, 0.5f, 1, color, 1, 1},
  };
  device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
  device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(ScreenVertex));
}

struct LitVertex {
  float x, y, z;
  float nx, ny, nz;
};

// lighting is per vertex, so a small quad keeps every vertex close to the light axis
static void
lit_quad(float nx, float ny, float nz, float half = 1) {
  LitVertex v[4] = {
      {-half, -half, 0.5f, nx, ny, nz},
      {-half, half, 0.5f, nx, ny, nz},
      {half, -half, 0.5f, nx, ny, nz},
      {half, half, 0.5f, nx, ny, nz},
  };
  device->SetFVF(D3DFVF_XYZ | D3DFVF_NORMAL);
  device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(LitVertex));
}

static void
stage(DWORD index, DWORD color_op, DWORD color_arg1, DWORD color_arg2, DWORD alpha_op, DWORD alpha_arg1) {
  device->SetTextureStageState(index, D3DTSS_COLOROP, color_op);
  device->SetTextureStageState(index, D3DTSS_COLORARG1, color_arg1);
  device->SetTextureStageState(index, D3DTSS_COLORARG2, color_arg2);
  device->SetTextureStageState(index, D3DTSS_ALPHAOP, alpha_op);
  device->SetTextureStageState(index, D3DTSS_ALPHAARG1, alpha_arg1);
}

static void
default_stages() {
  stage(0, D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_CURRENT, D3DTOP_SELECTARG1, D3DTA_TEXTURE);
  stage(1, D3DTOP_DISABLE, D3DTA_TEXTURE, D3DTA_CURRENT, D3DTOP_DISABLE, D3DTA_TEXTURE);
}

int WINAPI
WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
  WNDCLASSW wc = {};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = instance;
  wc.lpszClassName = L"d3d9_fixed_function";
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
  device->BeginScene();

  // default stages modulate an unbound texture, which passes the vertex color through
  clear();
  screen_quad(0, 0, 32, 64, 0xFF204080);
  expect("pretransformed vertex color", 16, 32, 0xFF204080);
  expect("pretransformed position", 48, 32, 0xFF000000);

  D3DVIEWPORT9 viewport = {32, 0, 32, 64, 0, 1};
  device->SetViewport(&viewport);
  clear();
  screen_quad(32, 0, 48, 64, 0xFFFFFFFF);
  expect("pretransformed position ignores the viewport origin", 40, 32, 0xFFFFFFFF);
  expect("pretransformed quad stays inside its rect", 56, 32, 0xFF000000);
  viewport = {0, 0, 64, 64, 0, 1};
  device->SetViewport(&viewport);

  IDirect3DTexture9 *texture = nullptr;
  device->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr);
  D3DLOCKED_RECT lr;
  texture->LockRect(0, &lr, nullptr, 0);
  for (int y = 0; y < 2; y++)
    for (int x = 0; x < 2; x++)
      ((DWORD *)((char *)lr.pBits + y * lr.Pitch))[x] = x ? 0x80FF0000 : 0xFF00FF00;
  texture->UnlockRect(0);
  device->SetTexture(0, texture);
  clear();
  screen_quad(0, 0, 64, 64, 0xFF808080);
  expect("texture modulates diffuse", 16, 32, 0xFF008000);
  expect("texture alpha selects texture", 48, 32, 0x80800000);
  device->SetTexture(0, nullptr);

  device->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFF123456);
  stage(0, D3DTOP_SELECTARG1, D3DTA_TFACTOR, D3DTA_CURRENT, D3DTOP_SELECTARG1, D3DTA_TFACTOR);
  clear();
  screen_quad(0, 0, 64, 64, 0xFFFFFFFF);
  expect("select texture factor", 32, 32, 0xFF123456);

  device->SetRenderState(D3DRS_TEXTUREFACTOR, 0x00102030);
  stage(0, D3DTOP_SELECTARG1, D3DTA_DIFFUSE, D3DTA_CURRENT, D3DTOP_SELECTARG1, D3DTA_DIFFUSE);
  stage(1, D3DTOP_ADD, D3DTA_CURRENT, D3DTA_TFACTOR, D3DTOP_SELECTARG1, D3DTA_CURRENT);
  clear();
  screen_quad(0, 0, 64, 64, 0xFF402000);
  expect("second stage adds texture factor", 32, 32, 0xFF504030);

  stage(0, D3DTOP_SELECTARG1, D3DTA_DIFFUSE | D3DTA_COMPLEMENT, D3DTA_CURRENT, D3DTOP_SELECTARG1, D3DTA_DIFFUSE);
  stage(1, D3DTOP_DISABLE, D3DTA_TEXTURE, D3DTA_CURRENT, D3DTOP_DISABLE, D3DTA_TEXTURE);
  clear();
  screen_quad(0, 0, 64, 64, 0xFF204060);
  expect("complemented argument", 32, 32, 0xFFDFBF9F);
  default_stages();

  const char *ps_source = "float4 main(float4 c : COLOR0) : COLOR0 { return c.bgra; }";
  ID3DBlob *code = nullptr;
  IDirect3DPixelShader9 *ps = nullptr;
  if (SUCCEEDED(D3DCompile(ps_source, strlen(ps_source), nullptr, nullptr, nullptr, "main", "ps_2_0", 0, 0, &code, nullptr)))
    device->CreatePixelShader((const DWORD *)code->GetBufferPointer(), &ps);
  device->SetPixelShader(ps);
  clear();
  screen_quad(0, 0, 64, 64, 0xFF204080);
  expect("fixed-function vertex stage feeds a pixel shader", 32, 32, 0xFF804020);
  device->SetPixelShader(nullptr);

  struct ColorVertex {
    float x, y, z;
    DWORD color;
  } small[4] = {
      {-0.25f, -0.25f, 0.5f, 0xFF00FF00},
      {-0.25f, 0.25f, 0.5f, 0xFF00FF00},
      {0.25f, -0.25f, 0.5f, 0xFF00FF00},
      {0.25f, 0.25f, 0.5f, 0xFF00FF00},
  };
  D3DMATRIX world = {{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.5f, 0, 0, 1}}};
  D3DMATRIX identity = {{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}};
  D3DMATRIX scale = {{{0.5f, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}};
  device->SetRenderState(D3DRS_LIGHTING, FALSE);
  device->SetTransform(D3DTS_WORLD, &world);
  device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
  clear();
  device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, small, sizeof(ColorVertex));
  // world moves the quad to x 0.25 .. 0.75, i.e. pixels 40 .. 55
  expect("world transform moves the quad", 48, 32, 0xFF00FF00);
  expect("world transform leaves the center", 32, 32, 0xFF000000);
  device->SetTransform(D3DTS_WORLD, &identity);
  device->SetTransform(D3DTS_VIEW, &scale);
  device->MultiplyTransform(D3DTS_VIEW, &world);
  // view = translate * scale: x 0.125 .. 0.375, pixels 36 .. 43
  clear();
  device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, small, sizeof(ColorVertex));
  expect("view transform composes with world", 39, 32, 0xFF00FF00);
  expect("view transform scales the quad", 46, 32, 0xFF000000);
  D3DMATRIX got;
  device->GetTransform(D3DTS_VIEW, &got);
  bool round_trip = got.m[3][0] == 0.25f && got.m[0][0] == 0.5f;
  printf("%s: MultiplyTransform result\n", round_trip ? "PASS" : "FAIL");
  failures += !round_trip;
  device->SetTransform(D3DTS_VIEW, &identity);

  device->SetRenderState(D3DRS_LIGHTING, TRUE);
  device->SetRenderState(D3DRS_AMBIENT, 0xFF404040);
  D3DMATERIAL9 material = {};
  material.Diffuse = {1, 1, 1, 1};
  material.Ambient = {1, 0.5f, 1, 1};
  device->SetMaterial(&material);
  clear();
  lit_quad(0, 0, -1);
  expect("ambient lighting", 32, 32, 0xFF402040);

  D3DLIGHT9 light = {};
  light.Type = D3DLIGHT_DIRECTIONAL;
  light.Diffuse = {1, 0, 0, 1};
  light.Direction = {0, 0, 1};
  device->SetLight(0, &light);
  device->LightEnable(0, TRUE);
  device->SetRenderState(D3DRS_AMBIENT, 0);
  clear();
  lit_quad(0, 0, -1);
  expect("directional light facing the surface", 32, 32, 0xFFFF0000);
  clear();
  lit_quad(0, 0.8f, -0.6f);
  expect("directional light at an angle", 32, 32, 0xFF990000);
  clear();
  lit_quad(1, 0, 0);
  expect("directional light parallel to the surface", 32, 32, 0xFF000000);
  device->SetRenderState(D3DRS_AMBIENT, 0xFF404040);
  clear();
  lit_quad(0, 0, 1);
  expect("light from behind adds nothing to ambient", 32, 32, 0xFF402040);
  BOOL enabled = FALSE;
  device->GetLightEnable(0, &enabled);
  printf("%s: GetLightEnable\n", enabled ? "PASS" : "FAIL");
  failures += !enabled;

  // point and spot lights at the eye, 0.5 in front of the quad
  material = {};
  material.Diffuse = {1, 1, 1, 1};
  device->SetMaterial(&material);
  device->SetRenderState(D3DRS_AMBIENT, 0);
  light = {};
  light.Type = D3DLIGHT_POINT;
  light.Diffuse = {0, 1, 0, 1};
  light.Attenuation0 = 2;
  light.Range = 10;
  device->SetLight(0, &light);
  clear();
  lit_quad(0, 0, -1, 0.05f);
  // N.L at the corners is 0.5 / |(0.05, 0.05, 0.5)| = 0.99, times attenuation 1/2
  expect("point light attenuation", 32, 32, 0xFF007E00);
  light.Range = 0.4f;
  device->SetLight(0, &light);
  clear();
  lit_quad(0, 0, -1, 0.05f);
  expect("point light out of range", 32, 32, 0xFF000000);
  light.Type = D3DLIGHT_SPOT;
  light.Range = 10;
  light.Attenuation0 = 1;
  light.Direction = {0, 0, 1};
  light.Theta = 0.4f;
  light.Phi = 0.8f;
  light.Falloff = 1;
  device->SetLight(0, &light);
  clear();
  lit_quad(0, 0, -1, 0.05f);
  expect("spot light inside the cone", 32, 32, 0xFF00FF00);
  clear();
  lit_quad(0, 0, -1);
  expect("spot light outside the cone", 32, 32, 0xFF000000);

  light = {};
  light.Type = D3DLIGHT_DIRECTIONAL;
  light.Specular = {0, 0, 1, 1};
  light.Direction = {0, 0, 1};
  device->SetLight(0, &light);
  material = {};
  material.Specular = {1, 1, 1, 1};
  material.Power = 10;
  device->SetMaterial(&material);
  // the material's diffuse alpha (0) is the vertex alpha
  device->SetRenderState(D3DRS_SPECULARENABLE, TRUE);
  device->SetRenderState(D3DRS_LOCALVIEWER, FALSE);
  clear();
  lit_quad(0, 0, -1);
  expect("specular highlight", 32, 32, 0x000000FF);
  // with a local viewer the corners see N.H = 0.8165, and 0.8165^10 = 0.13
  device->SetRenderState(D3DRS_LOCALVIEWER, TRUE);
  clear();
  lit_quad(0, 0, -1);
  expect("local viewer specular", 32, 32, 0x00000022);
  device->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
  clear();
  lit_quad(0, 0, -1);
  expect("no specular without SPECULARENABLE", 32, 32, 0x00000000);
  device->LightEnable(0, FALSE);
  device->SetRenderState(D3DRS_LIGHTING, FALSE);

  // texture coordinate transforms and generation on transformed vertices
  const DWORD strip_texels[4] = {0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFFFF};
  IDirect3DTexture9 *strip = nullptr;
  device->CreateTexture(4, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &strip, nullptr);
  strip->LockRect(0, &lr, nullptr, 0);
  memcpy(lr.pBits, strip_texels, sizeof(strip_texels));
  strip->UnlockRect(0);
  device->SetTexture(0, strip);
  device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
  stage(0, D3DTOP_SELECTARG1, D3DTA_TEXTURE, D3DTA_CURRENT, D3DTOP_SELECTARG1, D3DTA_TEXTURE);
  struct TexVertex {
    float x, y, z, u, v;
  } tex_quad[4] = {{-1, -1, 0.5f, 0, 1}, {-1, 1, 0.5f, 0, 0}, {1, -1, 0.5f, 1, 1}, {1, 1, 0.5f, 1, 0}};
  device->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
  D3DMATRIX shift = {{{1, 0, 0, 0, 0, 1, 0, 0, 0.5f, 0, 1, 0, 0, 0, 0, 1}}};
  device->SetTransform(D3DTS_TEXTURE0, &shift);
  device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
  clear();
  device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, tex_quad, sizeof(TexVertex));
  expect("texture matrix shifts u", 4, 32, 0xFF0000FF);
  device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
  clear();
  device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, tex_quad, sizeof(TexVertex));
  expect("no texture matrix when disabled", 4, 32, 0xFFFF0000);
  D3DMATRIX to_unit = {{{0.5f, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 1, 0, 0.5f, 0.5f, 0, 1}}};
  device->SetTransform(D3DTS_TEXTURE0, &to_unit);
  device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
  device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION);
  device->SetFVF(D3DFVF_XYZ);
  LitVertex plain[4] = {{-1, -1, 0.5f}, {-1, 1, 0.5f}, {1, -1, 0.5f}, {1, 1, 0.5f}};
  clear();
  device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, plain, sizeof(LitVertex));
  expect("camera-space position left", 4, 32, 0xFFFF0000);
  expect("camera-space position right", 60, 32, 0xFFFFFFFF);
  device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
  device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
  device->SetTexture(0, nullptr);
  strip->Release();
  default_stages();

  // one blend weight between WORLD0 (shift left) and WORLD1 (shift right)
  D3DMATRIX left = {{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -0.5f, 0, 0, 1}}};
  D3DMATRIX right = {{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.5f, 0, 0, 1}}};
  device->SetTransform(D3DTS_WORLDMATRIX(0), &left);
  device->SetTransform(D3DTS_WORLDMATRIX(1), &right);
  device->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_1WEIGHTS);
  device->SetFVF(D3DFVF_XYZB1 | D3DFVF_DIFFUSE);
  struct BlendVertex {
    float x, y, z, weight;
    DWORD color;
  };
  auto blended = [](float weight) {
    BlendVertex v[4] = {
        {-0.2f, -0.2f, 0.5f, weight, 0xFFFFFFFF}, {-0.2f, 0.2f, 0.5f, weight, 0xFFFFFFFF},
        {0.2f, -0.2f, 0.5f, weight, 0xFFFFFFFF}, {0.2f, 0.2f, 0.5f, weight, 0xFFFFFFFF},
    };
    clear();
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(BlendVertex));
  };
  blended(1.0f);
  expect("blend weight 1 uses WORLD0", 16, 32, 0xFFFFFFFF);
  blended(0.0f);
  expect("blend weight 0 uses WORLD1", 48, 32, 0xFFFFFFFF);
  blended(0.5f);
  expect("half weights cancel out", 32, 32, 0xFFFFFFFF);
  expect("half weights leave the left", 16, 32, 0xFF000000);
  // indexed blending picks WORLDMATRIX(3) and (7) through the vertex's blend indices
  D3DCAPS9 caps;
  device->GetDeviceCaps(&caps);
  check("indexed blend matrices advertised", caps.MaxVertexBlendMatrixIndex == 8);
  device->SetTransform(D3DTS_WORLDMATRIX(0), &identity);
  device->SetTransform(D3DTS_WORLDMATRIX(1), &identity);
  device->SetTransform(D3DTS_WORLDMATRIX(3), &left);
  device->SetTransform(D3DTS_WORLDMATRIX(7), &right);
  device->SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE, TRUE);
  struct IndexedVertex {
    float x, y, z, weight;
    DWORD indices, color;
  };
  // XYZB2 | LASTBETA_D3DCOLOR declares a D3DCOLOR weight on Windows, so D3DCOLOR indices need a declaration
  D3DVERTEXELEMENT9 color_index_elements[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT1, 0, D3DDECLUSAGE_BLENDWEIGHT, 0},
      {0, 16, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_BLENDINDICES, 0},
      {0, 20, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0},
      D3DDECL_END(),
  };
  IDirect3DVertexDeclaration9 *color_index_decl = nullptr;
  device->CreateVertexDeclaration(color_index_elements, &color_index_decl);
  for (bool d3dcolor : {false, true}) {
    if (d3dcolor)
      device->SetVertexDeclaration(color_index_decl);
    else
      device->SetFVF(D3DFVF_XYZB2 | D3DFVF_LASTBETA_UBYTE4 | D3DFVF_DIFFUSE);
    // byte 0 is the first index for both encodings
    DWORD indices = 3 | 7 << 8;
    for (float weight : {1.0f, 0.0f}) {
      IndexedVertex v[4] = {
          {-0.2f, -0.2f, 0.5f, weight, indices, 0xFFFFFFFF}, {-0.2f, 0.2f, 0.5f, weight, indices, 0xFFFFFFFF},
          {0.2f, -0.2f, 0.5f, weight, indices, 0xFFFFFFFF}, {0.2f, 0.2f, 0.5f, weight, indices, 0xFFFFFFFF},
      };
      clear();
      device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(IndexedVertex));
      char name[96];
      snprintf(name, sizeof(name), "%s indices, weight %.0f", d3dcolor ? "D3DCOLOR" : "UBYTE4", weight);
      expect(name, weight ? 16 : 48, 32, 0xFFFFFFFF);
    }
  }
  if (color_index_decl)
    color_index_decl->Release();
  device->SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE);
  device->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
  device->SetTransform(D3DTS_WORLDMATRIX(0), &identity);
  device->SetTransform(D3DTS_WORLDMATRIX(1), &identity);

  // tweening interpolates POSITION0 and POSITION1 by the tween factor
  D3DVERTEXELEMENT9 tween_elements[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 1},
      {0, 24, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0},
      D3DDECL_END(),
  };
  IDirect3DVertexDeclaration9 *tween_decl = nullptr;
  device->CreateVertexDeclaration(tween_elements, &tween_decl);
  struct TweenVertex {
    float x0, y0, z0, x1, y1, z1;
    DWORD color;
  } tween[4];
  float corners[4][2] = {{-0.2f, -0.2f}, {-0.2f, 0.2f}, {0.2f, -0.2f}, {0.2f, 0.2f}};
  for (int i = 0; i < 4; i++)
    tween[i] = {corners[i][0] - 0.5f, corners[i][1], 0.5f, corners[i][0] + 0.5f, corners[i][1], 0.5f, 0xFFFFFFFF};
  device->SetVertexDeclaration(tween_decl);
  device->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_TWEENING);
  float factors[3] = {0, 1, 0.5f};
  int columns[3] = {16, 48, 32};
  const char *names[3] = {"tween factor 0", "tween factor 1", "tween factor 0.5"};
  for (int i = 0; i < 3; i++) {
    device->SetRenderState(D3DRS_TWEENFACTOR, f2dw(factors[i]));
    clear();
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, tween, sizeof(TweenVertex));
    expect(names[i], columns[i], 32, 0xFFFFFFFF);
  }
  device->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
  tween_decl->Release();

  // sphere mapping: a normal facing the eye reflects to the map's center, one tilted toward +x to u = 0.8
  const DWORD sphere_texels[3] = {0xFFFF0000, 0xFF00FF00, 0xFF0000FF};
  IDirect3DTexture9 *sphere = nullptr;
  device->CreateTexture(3, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &sphere, nullptr);
  sphere->LockRect(0, &lr, nullptr, 0);
  memcpy(lr.pBits, sphere_texels, sizeof(sphere_texels));
  sphere->UnlockRect(0);
  device->SetTexture(0, sphere);
  device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
  stage(0, D3DTOP_SELECTARG1, D3DTA_TEXTURE, D3DTA_CURRENT, D3DTOP_SELECTARG1, D3DTA_TEXTURE);
  device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_SPHEREMAP);
  device->SetRenderState(D3DRS_LOCALVIEWER, FALSE);
  clear();
  lit_quad(0, 0, -1);
  expect("sphere map center", 32, 32, 0xFF00FF00);
  clear();
  lit_quad(0.6f, 0, -0.8f);
  expect("sphere map tilted normal", 32, 32, 0xFF0000FF);
  device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
  device->SetRenderState(D3DRS_LOCALVIEWER, TRUE);
  device->SetTexture(0, nullptr);
  sphere->Release();
  default_stages();

  // vertex fog from view-space depth, and from specular alpha for pretransformed vertices
  float fog_start = 0, fog_end = 1;
  device->SetRenderState(D3DRS_FOGENABLE, TRUE);
  device->SetRenderState(D3DRS_FOGCOLOR, 0xFF0000FF);
  device->SetRenderState(D3DRS_FOGTABLEMODE, D3DFOG_NONE);
  device->SetRenderState(D3DRS_FOGVERTEXMODE, D3DFOG_LINEAR);
  device->SetRenderState(D3DRS_FOGSTART, f2dw(fog_start));
  device->SetRenderState(D3DRS_FOGEND, f2dw(fog_end));
  ColorVertex fogged[4] = {
      {-1, -1, 0.25f, 0xFFFF0000}, {-1, 1, 0.25f, 0xFFFF0000}, {1, -1, 0.25f, 0xFFFF0000}, {1, 1, 0.25f, 0xFFFF0000}
  };
  device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
  clear();
  device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, fogged, sizeof(ColorVertex));
  expect("linear vertex fog", 32, 32, 0xFFBF0040);
  struct SpecVertex {
    float x, y, z, rhw;
    DWORD diffuse, specular;
  } spec_quad[4] = {
      {0, 0, 0.5f, 1, 0xFFFF0000, 0x40000000}, {64, 0, 0.5f, 1, 0xFFFF0000, 0x40000000},
      {0, 64, 0.5f, 1, 0xFFFF0000, 0x40000000}, {64, 64, 0.5f, 1, 0xFFFF0000, 0x40000000},
  };
  device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR);
  clear();
  device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, spec_quad, sizeof(SpecVertex));
  expect("fog from specular alpha", 32, 32, 0xFF4000BF);
  device->SetRenderState(D3DRS_FOGENABLE, FALSE);

  device->EndScene();
  if (ps)
    ps->Release();
  texture->Release();
  readback->Release();
  rt->Release();
  device->Release();
  d3d->Release();
  printf("%s: %d failure(s)\n", failures ? "failed" : "passed", failures);
  return failures;
}
