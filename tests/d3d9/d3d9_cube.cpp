// contract: CreateCubeTexture makes six square faces as a TEXTURECUBE (Microsoft Learn,
// IDirect3DDevice9::CreateCubeTexture). LockRect of a face writes that face, SetTexture samples it from a
// programmable ps_2_0 texCUBE.
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

#define CHECK(x)                                                                                                       \
  if (FAILED(x)) {                                                                                                     \
    printf("failed: %s (line %d)\n", #x, __LINE__);                                                                    \
    return 1;                                                                                                          \
  }

static const UINT kClient = 64;
static const D3DCOLOR kClear = D3DCOLOR_XRGB(0, 0, 255);
static const D3DCOLOR kRed = D3DCOLOR_XRGB(255, 0, 0);

const char *kVertexHlsl = "struct VSOut { float4 position : POSITION; float3 dir : TEXCOORD0; };\n"
                          "VSOut main(float4 position : POSITION, float3 dir : TEXCOORD0) {\n"
                          "  VSOut o; o.position = position; o.dir = dir; return o;\n"
                          "}\n";
const char *kPixelHlsl = "samplerCUBE s0 : register(s0);\n"
                         "float4 main(float3 dir : TEXCOORD0) : COLOR { return texCUBE(s0, dir); }\n";

ComPtr<ID3DBlob>
compile(const char *hlsl, const char *profile) {
  ComPtr<ID3DBlob> code, errors;
  if (FAILED(D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, "main", profile, 0, 0, &code, &errors)) &&
      errors)
    printf("%s: %.*s\n", profile, (int)errors->GetBufferSize(), (const char *)errors->GetBufferPointer());
  return code;
}

int
main() {
  unsigned wrong = 0, checks = 0;
  auto expect = [&](bool ok, const char *what) {
    checks++;
    if (!ok && wrong++ < 16)
      printf("wrong: %s\n", what);
  };

  WNDCLASSA wc{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr, "d3d9_cube"};
  RegisterClassA(&wc);
  HWND window = CreateWindowA("d3d9_cube", "d3d9_cube", WS_OVERLAPPEDWINDOW, 0, 0, kClient, kClient, nullptr, nullptr,
                              wc.hInstance, nullptr);
  if (!window) {
    printf("failed: no window\n");
    return 1;
  }
  ComPtr<IDirect3D9> d3d9;
  d3d9.Attach(Direct3DCreate9(D3D_SDK_VERSION));
  if (!d3d9) {
    printf("failed: Direct3DCreate9 made no object\n");
    return 1;
  }
  D3DPRESENT_PARAMETERS present{};
  present.BackBufferWidth = kClient;
  present.BackBufferHeight = kClient;
  present.BackBufferFormat = D3DFMT_X8R8G8B8;
  present.BackBufferCount = 1;
  present.SwapEffect = D3DSWAPEFFECT_DISCARD;
  present.hDeviceWindow = window;
  present.Windowed = TRUE;
  present.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  ComPtr<IDirect3DDevice9> device;
  CHECK(d3d9->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &present, &device));

  ComPtr<IDirect3DCubeTexture9> cube;
  CHECK(device->CreateCubeTexture(4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cube, nullptr));
  for (UINT face = 0; face < 6; face++) {
    D3DLOCKED_RECT locked{};
    CHECK(cube->LockRect((D3DCUBEMAP_FACES)face, 0, &locked, nullptr, 0));
    for (UINT y = 0; y < 4; y++) {
      DWORD *row = (DWORD *)((char *)locked.pBits + y * locked.Pitch);
      for (UINT x = 0; x < 4; x++)
        row[x] = face == D3DCUBEMAP_FACE_POSITIVE_X ? kRed : D3DCOLOR_XRGB(0, 255, 0);
    }
    CHECK(cube->UnlockRect((D3DCUBEMAP_FACES)face, 0));
  }
  ComPtr<IDirect3DSurface9> face;
  CHECK(cube->GetCubeMapSurface(D3DCUBEMAP_FACE_POSITIVE_X, 0, &face));
  expect(face != nullptr, "GetCubeMapSurface of +X made no surface");

  ComPtr<ID3DBlob> vs = compile(kVertexHlsl, "vs_2_0");
  ComPtr<ID3DBlob> ps = compile(kPixelHlsl, "ps_2_0");
  expect(vs && ps, "Microsoft's compiler made no cube shaders");
  if (!vs || !ps)
    return 1;
  ComPtr<IDirect3DVertexShader9> vertex;
  ComPtr<IDirect3DPixelShader9> pixel;
  CHECK(device->CreateVertexShader((const DWORD *)vs->GetBufferPointer(), &vertex));
  CHECK(device->CreatePixelShader((const DWORD *)ps->GetBufferPointer(), &pixel));
  const D3DVERTEXELEMENT9 elements[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      D3DDECL_END(),
  };
  ComPtr<IDirect3DVertexDeclaration9> declaration;
  CHECK(device->CreateVertexDeclaration(elements, &declaration));
  CHECK(device->SetVertexDeclaration(declaration.Get()));
  CHECK(device->SetVertexShader(vertex.Get()));
  CHECK(device->SetPixelShader(pixel.Get()));
  CHECK(device->SetTexture(0, cube.Get()));
  CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));

  struct Vertex {
    float x, y, z, u, v, w;
  };
  const Vertex triangle[3] = {
      {-0.5f, -0.5f, 0, 1, 0, 0},
      {0.5f, -0.5f, 0, 1, 0, 0},
      {0.0f, 0.5f, 0, 1, 0, 0},
  };
  CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
  CHECK(device->BeginScene());
  CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, triangle, sizeof(Vertex)));
  CHECK(device->EndScene());

  ComPtr<IDirect3DSurface9> back, readback;
  CHECK(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back));
  CHECK(device->CreateOffscreenPlainSurface(kClient, kClient, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr));
  CHECK(device->GetRenderTargetData(back.Get(), readback.Get()));
  D3DLOCKED_RECT locked{};
  CHECK(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY));
  const DWORD *row = (const DWORD *)((const char *)locked.pBits + 32 * locked.Pitch);
  const D3DCOLOR got = row[32] & 0x00FFFFFF;
  readback->UnlockRect();
  expect(got == (kRed & 0x00FFFFFF), "sampling +X of the cube was not the red face");
  printf("%s: %u wrong of %u checks (pixel %06lx)\n", wrong ? "failed" : "passed", wrong, checks, (unsigned long)got);
  return wrong ? 1 : 0;
}
