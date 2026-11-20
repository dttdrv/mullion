// contract: ps_1_4's texdepth makes the pixel's depth clamp(register.x / min(register.y, 1), 0, 1) and writes it as an
// implicit output. A draw with that pixel shader and no vertex shader must be accepted and the depth test must see the
// value; before the translator emitted the depth output the draw was refused with D3DERR_INVALIDCALL (Wine visual.c
// texdepth_test, ids 6156-6270). Microsoft Learn, texdepth - ps_1_4; Wine's wined3d shader_glsl_texdepth.
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

// a check inside a void lambda: a failure is recorded and the case abandoned, not returned from main
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

  // ps_1_4: mov r5, c0 / phase / texdepth r5 / mov r0, c1. c0 is the depth pair, c1 the colour the pixel keeps.
  static const DWORD texdepth_ps[] = {
      0xffff0104u,
      0x00000001u, 0x800f0005u, 0xa0e40000u, // mov r5, c0
      0x0000fffdu,                           // phase
      0x00000057u, 0x800f0005u,              // texdepth r5
      0x00000001u, 0x800f0000u, 0xa0e40001u, // mov r0, c1
      0x0000ffffu,
  };

  WNDCLASSA wc{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr, "d3d9_texdepth"};
  RegisterClassA(&wc);
  HWND window = CreateWindowA("d3d9_texdepth", "d3d9_texdepth", WS_OVERLAPPEDWINDOW, 0, 0, kClient, kClient, nullptr,
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
  present.EnableAutoDepthStencil = TRUE;
  present.AutoDepthStencilFormat = D3DFMT_D24S8;
  present.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  ComPtr<IDirect3DDevice9> device;
  CHECK(d3d9->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &present, &device));
  CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
  CHECK(device->SetRenderState(D3DRS_LIGHTING, FALSE));
  CHECK(device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE));
  CHECK(device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE));
  CHECK(device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS));
  CHECK(device->SetFVF(D3DFVF_XYZ));

  ComPtr<IDirect3DPixelShader9> shader;
  printf("device created\n");
  CHECK(device->CreatePixelShader(texdepth_ps, &shader));
  printf("shader created\n");
  CHECK(device->SetPixelShader(shader.Get()));
  const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  CHECK(device->SetPixelShaderConstantF(1, red, 1));

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

  const float quad[] = {
      -1.0f, -1.0f, 0.0f,
      -1.0f,  1.0f, 0.0f,
       1.0f, -1.0f, 0.0f,
       1.0f,  1.0f, 0.0f,
  };
  auto run = [&](float depth, float divisor, const char *what, D3DCOLOR expected) -> void {
    REQUIRE(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, kClear, 0.75f, 0));
    const float pair[4] = {depth, divisor, 0.0f, 0.0f};
    REQUIRE(device->SetPixelShaderConstantF(0, pair, 1));
    REQUIRE(device->BeginScene());
    const HRESULT hr = device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, 3 * sizeof(float));
    REQUIRE(device->EndScene());
    const D3DCOLOR got = pixel(32, 32);
    printf("%s: hr=%08lx pixel=%06lx want=%06lx\n", what, (unsigned long)hr, (unsigned long)got,
           (unsigned long)expected);
    expect(SUCCEEDED(hr), "the texdepth draw is accepted");
    expect(got == (expected & 0x00FFFFFF), what);
  };

  // 0.25 < the cleared 0.75: the pixel survives with the shader's colour
  run(0.25f, 1.0f, "0.25 / 1 passes the 0.75 depth test", kRed);
  // 0.9 does not: the clear's colour stays
  run(0.9f, 1.0f, "0.9 / 1 fails the 0.75 depth test", kClear);
  // the divisor is clamped to 1 before the divide: 0.9 / min(2, 1) is 0.9 and fails, where 0.9 / 2 would pass
  run(0.9f, 2.0f, "0.9 / min(2, 1) fails the 0.75 depth test", kClear);
  // a negative divisor survives ps_1_4's lack of a default clamp: 0.5 / min(-0.5, 1) is -1, which clamps to depth 0
  run(0.5f, -0.5f, "0.5 / min(-0.5, 1) clamps to 0 and passes", kRed);

  printf("%s: %u checks, %u wrong\n", wrong ? "failed" : "passed", checks, wrong);
  return wrong ? 1 : 0;
}
