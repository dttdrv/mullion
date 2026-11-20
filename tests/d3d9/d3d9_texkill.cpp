// contract: texkill discards the pixel when a texture coordinate component is negative: ps_1_1 compares the first three
// (UVW), ps_2_0 and up compare all four, with the coordinate supplied by an FVF whose TEXCOORDSIZE4 names four (Microsoft
// Learn, texkill - ps; Wine visual.c texkill_test, ids 6362/6371/6423/6432/6443/6452).
#include <windows.h>
#include <d3d9.h>
#include <wrl/client.h>
#include <cstdio>

using Microsoft::WRL::ComPtr;

#define CHECK(x)                                                                                                       \
  if (FAILED(x)) {                                                                                                     \
    printf("failed: %s (line %d)\n", #x, __LINE__);                                                                    \
    return 1;                                                                                                          \
  }

#define REQUIRE(x)                                                                                                     \
  if (FAILED(x)) {                                                                                                     \
    printf("failed: %s (line %d)\n", #x, __LINE__);                                                                    \
    wrong++;                                                                                                           \
    return;                                                                                                            \
  }

static const UINT kClient = 64;
static const D3DCOLOR kClear = D3DCOLOR_XRGB(0, 0, 255);
static const D3DCOLOR kRed = D3DCOLOR_XRGB(255, 0, 0);

int
main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  unsigned wrong = 0, checks = 0;
  auto expect = [&](bool ok, const char *what) {
    checks++;
    if (!ok && wrong++ < 16)
      printf("wrong: %s\n", what);
  };

  // ps_1_1: texkill t0 / mov r0, c0. c0 is red, so a surviving pixel is red and a killed one keeps the clear colour.
  static const DWORD texkill_ps[] = {
      0xffff0101u,
      0x00000051u, 0xa00f0000u, 0x3f800000u, 0x00000000u, 0x00000000u, 0x3f800000u, // def c0, 1, 0, 0, 1
      0x00000041u, 0xb00f0000u,                                                     // texkill t0
      0x00000001u, 0x800f0000u, 0xa0e40000u,                                        // mov r0, c0
      0x0000ffffu,
  };

  WNDCLASSA wc{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr, "d3d9_texkill"};
  RegisterClassA(&wc);
  HWND window = CreateWindowA("d3d9_texkill", "d3d9_texkill", WS_OVERLAPPEDWINDOW, 0, 0, kClient, kClient, nullptr,
                              nullptr, wc.hInstance, nullptr);
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
  CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
  CHECK(device->SetRenderState(D3DRS_LIGHTING, FALSE));
  CHECK(device->SetFVF(D3DFVF_XYZ | D3DFVF_TEXCOORDSIZE4(0) | D3DFVF_TEX1));

  ComPtr<IDirect3DPixelShader9> shader;
  CHECK(device->CreatePixelShader(texkill_ps, &shader));
  CHECK(device->SetPixelShader(shader.Get()));

  ComPtr<IDirect3DSurface9> readback, back;
  CHECK(device->CreateOffscreenPlainSurface(kClient, kClient, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr));
  auto pixel = [&](UINT x, UINT y) -> D3DCOLOR {
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)))
      return 0;
    D3DLOCKED_RECT locked{};
    if (FAILED(device->GetRenderTargetData(back.Get(), readback.Get())) ||
        FAILED(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
      back.Reset();
      return 0;
    }
    const DWORD *row = (const DWORD *)((const char *)locked.pBits + (size_t)y * locked.Pitch);
    const D3DCOLOR got = row[x] & 0x00FFFFFF;
    readback->UnlockRect();
    back.Reset();
    return got;
  };

  // the same four components through an explicit declaration, which distinguishes the FVF's own size parsing from the
  // shader input path
  const D3DVERTEXELEMENT9 elements[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      D3DDECL_END(),
  };
  ComPtr<IDirect3DVertexDeclaration9> declaration;
  CHECK(device->CreateVertexDeclaration(elements, &declaration));

  // one quad, every vertex carrying the same four-component coordinate
  auto run = [&](bool by_declaration, float u, float v, float w, float q, const char *what,
                 D3DCOLOR expected) -> void {
    const float quad[4][7] = {
        {-1.0f, -1.0f, 0.0f, u, v, w, q},
        {-1.0f,  1.0f, 0.0f, u, v, w, q},
        { 1.0f, -1.0f, 0.0f, u, v, w, q},
        { 1.0f,  1.0f, 0.0f, u, v, w, q},
    };
    if (by_declaration) {
      REQUIRE(device->SetVertexDeclaration(declaration.Get()));
    } else {
      REQUIRE(device->SetFVF(D3DFVF_XYZ | D3DFVF_TEXCOORDSIZE4(0) | D3DFVF_TEX1));
    }
    REQUIRE(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
    REQUIRE(device->BeginScene());
    const HRESULT hr = device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, 7 * sizeof(float));
    REQUIRE(device->EndScene());
    const D3DCOLOR got = pixel(32, 32);
    printf("%s: hr=%08lx pixel=%06lx want=%06lx\n", what, (unsigned long)hr, (unsigned long)got,
           (unsigned long)(expected & 0x00FFFFFF));
    expect(SUCCEEDED(hr), "the texkill draw is accepted");
    expect(got == (expected & 0x00FFFFFF), what);
  };

  for (int by_declaration = 0; by_declaration < 2; by_declaration++) {
    const char *prefix = by_declaration ? "decl" : "fvf";
    char what[96];
    auto label = [&](const char *text) {
      snprintf(what, sizeof(what), "%s: %s", prefix, text);
      return (const char *)what;
    };
    run(by_declaration, 0.5f, 0.5f, 0.5f, 0.5f, label("all positive survives"), kRed);
    run(by_declaration, -0.5f, 0.5f, 0.5f, 0.5f, label("negative x kills"), kClear);
    run(by_declaration, 0.5f, -0.5f, 0.5f, 0.5f, label("negative y kills"), kClear);
    run(by_declaration, 0.5f, 0.5f, -0.5f, 0.5f, label("negative z kills"), kClear);
    run(by_declaration, 0.5f, 0.5f, 0.5f, -0.5f, label("negative w survives in ps_1_1"), kRed);
  }

  // ps_2_0 and up compare all four components, so the same negative w now kills
  static const DWORD texkill_ps20[] = {
      0xffff0200u,
      0x0200001fu, 0x80000000u, 0xb00f0000u,                                         // dcl t0
      0x05000051u, 0xa00f0000u, 0x3f800000u, 0x00000000u, 0x00000000u, 0x3f800000u, // def c0, 1, 0, 0, 1
      0x01000041u, 0xb00f0000u,                                                     // texkill t0
      0x02000001u, 0x800f0800u, 0xa0e40000u,                                        // mov oC0, c0
      0x0000ffffu,
  };
  {
    ComPtr<IDirect3DPixelShader9> shader20;
    CHECK(device->CreatePixelShader(texkill_ps20, &shader20));
    CHECK(device->SetPixelShader(shader20.Get()));
    run(false, 0.5f, 0.5f, 0.5f, 0.5f, "ps_2_0: all positive survives", kRed);
    run(false, 0.5f, 0.5f, -0.5f, 0.5f, "ps_2_0: negative z kills", kClear);
    run(false, 0.5f, 0.5f, 0.5f, -0.5f, "ps_2_0: negative w kills", kClear);
    CHECK(device->SetPixelShader(shader.Get()));
  }

  // what t0 actually holds: ps_1_1 `mov r0, t0` writes the coordinate to the colour, so a four-component coordinate of
  // 0.5 is grey and a two-component one has a zero blue channel
  static const DWORD show_ps[] = {
      0xffff0101u,
      0x00000001u, 0x800f0000u, 0xb0e40000u, // mov r0, t0 (0xE4 is the identity swizzle: bits 16-23)
      0x0000ffffu,
  };
  {
    ComPtr<IDirect3DPixelShader9> show;
    CHECK(device->CreatePixelShader(show_ps, &show));
    CHECK(device->SetPixelShader(show.Get()));
    CHECK(device->SetFVF(D3DFVF_XYZ | D3DFVF_TEXCOORDSIZE4(0) | D3DFVF_TEX1));
    const float quad[4][7] = {
        {-1.0f, -1.0f, 0.0f, 0.5f, 0.5f, 0.5f, 0.5f},
        {-1.0f,  1.0f, 0.0f, 0.5f, 0.5f, 0.5f, 0.5f},
        { 1.0f, -1.0f, 0.0f, 0.5f, 0.5f, 0.5f, 0.5f},
        { 1.0f,  1.0f, 0.0f, 0.5f, 0.5f, 0.5f, 0.5f},
    };
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
    CHECK(device->BeginScene());
    CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, 7 * sizeof(float)));
    CHECK(device->EndScene());
    const D3DCOLOR got = pixel(32, 32);
    const unsigned red = (got >> 16) & 0xFF, green = (got >> 8) & 0xFF, blue = got & 0xFF;
    printf("t0 as colour: %06lx (all four components arrived when r, g and b are 0x7f or 0x80)\n", (unsigned long)got);
    // the render target is 8-bit UNORM in ps_1_1, so 0.5 quantises to either 127 or 128
    const auto half = [](unsigned channel) { return channel == 0x7f || channel == 0x80; };
    expect(half(red) && half(green) && half(blue), "t0 carries all four components");
  }

  printf("%s: %u checks, %u wrong\n", wrong ? "failed" : "passed", checks, wrong);
  return wrong ? 1 : 0;
}
