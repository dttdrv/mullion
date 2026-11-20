// contract: CreateCubeTexture makes six square faces as a TEXTURECUBE (Microsoft Learn,
// IDirect3DDevice9::CreateCubeTexture). LockRect of a face writes that face, SetTexture samples it from a
// programmable ps_2_0 texCUBE. UpdateTexture copies a managed cube texture's faces and levels into a default-pool one,
// and each face and level is what the source held.
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

  // UpdateTexture between cube textures: a SYSTEMMEM cube of six faces (two levels, each its own colour) is copied into
  // a default-pool one, and every face of every level is what the application authored
  const UINT kFaceSize = 8;
  auto face_colour = [](UINT face, UINT level) {
    return D3DCOLOR_XRGB(40 + 30 * face, 200 - 25 * face, 20 + 90 * level);
  };
  ComPtr<IDirect3DCubeTexture9> source_cube, target_cube;
  CHECK(device->CreateCubeTexture(kFaceSize, 2, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &source_cube, nullptr));
  CHECK(device->CreateCubeTexture(kFaceSize, 2, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &target_cube, nullptr));
  for (UINT level = 0; level < 2; level++) {
    for (UINT face = 0; face < 6; face++) {
      D3DLOCKED_RECT level_lock{};
      CHECK(source_cube->LockRect((D3DCUBEMAP_FACES)face, level, &level_lock, nullptr, 0));
      for (UINT y = 0; y < kFaceSize >> level; y++) {
        DWORD *level_row = (DWORD *)((char *)level_lock.pBits + y * level_lock.Pitch);
        for (UINT x = 0; x < kFaceSize >> level; x++)
          level_row[x] = face_colour(face, level);
      }
      CHECK(source_cube->UnlockRect((D3DCUBEMAP_FACES)face, level));
    }
  }
  CHECK(device->UpdateTexture(source_cube.Get(), target_cube.Get()));

  // a face is drawn by the direction its triangle samples the cube in, and its centre pixel read back
  auto sample_face = [&](IDirect3DCubeTexture9 *texture, UINT face, D3DCOLOR *colour, bool minified = false) -> HRESULT {
    static const float directions[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    const float *d = directions[face];
    struct Vertex {
      float x, y, z, u, v, w;
    };
    // the corners of the quad: the whole target at one texel to a pixel's worth of the face, or four pixels of the
    // target that span the face's directions
    const float extent = minified ? 4.0f / kClient : 1.0f;
    const float spread = minified ? 1.0f : 0.0f;
    const UINT axis = face / 2, first = (axis + 1) % 3, second = (axis + 2) % 3;
    auto corner = [&](float x, float y) {
      float dir[3] = {d[0], d[1], d[2]};
      dir[first] += spread * x;
      dir[second] += spread * y;
      return Vertex{x * extent, y * extent, 0, dir[0], dir[1], dir[2]};
    };
    const Vertex quad[6] = {corner(-1, -1), corner(-1, 1), corner(1, -1), corner(1, -1), corner(-1, 1), corner(1, 1)};
    HRESULT result = device->SetTexture(0, texture);
    if (FAILED(result))
      return result;
    device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0);
    device->BeginScene();
    result = device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, quad, sizeof(Vertex));
    device->EndScene();
    if (FAILED(result))
      return result;
    result = device->GetRenderTargetData(back.Get(), readback.Get());
    if (FAILED(result))
      return result;
    D3DLOCKED_RECT pixel_lock{};
    result = readback->LockRect(&pixel_lock, nullptr, D3DLOCK_READONLY);
    if (FAILED(result))
      return result;
    *colour = ((const DWORD *)((const char *)pixel_lock.pBits + 32 * pixel_lock.Pitch))[32] & 0x00FFFFFF;
    readback->UnlockRect();
    return S_OK;
  };
  for (UINT face = 0; face < 6; face++) {
    D3DCOLOR colour = 0;
    CHECK(sample_face(target_cube.Get(), face, &colour));
    expect(colour == (face_colour(face, 0) & 0x00FFFFFF), "a face of the cube UpdateTexture filled is not the face the "
                                                         "source held");
  }

  // the second level: a quad of four pixels whose directions span the face is minified to half the face's size, so the
  // sampler reads the second level
  CHECK(device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_POINT));
  for (UINT face = 0; face < 6; face++) {
    D3DCOLOR colour = 0;
    CHECK(sample_face(target_cube.Get(), face, &colour, true));
    expect(colour == (face_colour(face, 1) & 0x00FFFFFF), "a second level of the cube UpdateTexture filled is not the "
                                                         "level the source held");
  }

  // the refusals: a cube into a 2D texture, a cube of another size, and a cube of another level count
  ComPtr<IDirect3DTexture9> flat;
  CHECK(device->CreateTexture(kFaceSize, kFaceSize, 2, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &flat, nullptr));
  expect(device->UpdateTexture(source_cube.Get(), flat.Get()) == D3DERR_INVALIDCALL,
         "UpdateTexture copied a cube texture into a 2D texture");
  ComPtr<IDirect3DCubeTexture9> larger_cube, shorter_cube;
  CHECK(device->CreateCubeTexture(kFaceSize * 2, 2, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &larger_cube, nullptr));
  CHECK(device->CreateCubeTexture(kFaceSize, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &shorter_cube, nullptr));
  expect(device->UpdateTexture(source_cube.Get(), larger_cube.Get()) == D3DERR_INVALIDCALL,
         "UpdateTexture copied a cube texture into a cube texture of another size");
  expect(device->UpdateTexture(source_cube.Get(), shorter_cube.Get()) == D3DERR_INVALIDCALL,
         "UpdateTexture copied a cube texture into a cube texture of another level count");

  printf("%s: %u wrong of %u checks (pixel %06lx)\n", wrong ? "failed" : "passed", wrong, checks, (unsigned long)got);
  return wrong ? 1 : 0;
}
