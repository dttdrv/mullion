// contract: D3DTSS_TEXTURETRANSFORMFLAGS on a stage samples through the projected coordinate, by the component the
// flags and the vertex's declaration name (Microsoft Learn, D3DTEXTURETRANSFORMFLAGS; Wine visual.c
// projected_textures_test records the same results):
// - COUNT4 | PROJECTED divides x and y by w, COUNT3 | PROJECTED by z;
// - D3DTTFF_PROJECTED alone, and the invalid 0xffffffff, use the coordinate's own last component: z for a
//   three-component coordinate and w for a four-component one;
// - a count the vertex cannot supply (COUNT3 | PROJECTED over a FLOAT2 coordinate) draws unprojected;
// - a game's own vertex shader writes oT0 whole, and its projection divides by w;
// - the fixed-function vertex stage carries a three- or four-component declaration whole, so a size-4 no-diffuse FVF
//   is 28 bytes (position 12 + coordinate 16) and a size-4 diffuse one 32, not the two-component family's stride.
#include <windows.h>
#include <d3d9.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

#define CHECK(x)                                                                                                       \
  if (FAILED(x)) {                                                                                                     \
    printf("failed: %s (line %d)\n", #x, __LINE__);                                                                    \
    return 1;                                                                                                          \
  }

namespace {

// Wine visual.c projected_textures_test's vs_1_1: the position out and the four-component coordinate out, untouched
const DWORD kPassthroughVertexShader[] = {
    0xfffe0101,                                     // vs_1_1
    0x0000001f, 0x80000000, 0x900f0000,             // dcl_position v0
    0x0000001f, 0x80000005, 0x900f0001,             // dcl_texcoord0 v1
    0x00000001, 0xc00f0000, 0x90e40000,             // mov oPos, v0
    0x00000001, 0xe00f0000, 0x90e40001,             // mov oT0, v1
    0x0000ffff,                                     // end
};

// the same test's ps_1_3: tex t0, mov r0, t0 — the stage's projected coordinate is what it samples
const DWORD kSamplePixelShader[] = {
    0xffff0103,                                     // ps_1_3
    0x00000042, 0xb00f0000,                         // tex t0
    0x00000001, 0x800f0000, 0xb0e40000,             // mov r0, t0
    0x0000ffff,                                     // end
};

// the 4x4 luminance texture Wine's test uses: one white texel at (1, 1), every other black
const unsigned char kTexels[16] = {0, 0, 0, 0, 0, 0xff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

} // namespace

int
main() {
  unsigned wrong = 0, checks = 0;
  auto expect = [&](bool ok, const char *what) {
    checks++;
    if (!ok && wrong++ < 20)
      printf("wrong: %s\n", what);
  };

  WNDCLASSA window_class{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr,
                         "d3d9_projected"};
  RegisterClassA(&window_class);
  const UINT kWidth = 640, kHeight = 480;
  RECT window_rect{0, 0, (LONG)kWidth, (LONG)kHeight};
  AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);
  HWND window = CreateWindowA("d3d9_projected", "d3d9_projected", WS_OVERLAPPEDWINDOW, 0, 0,
                              window_rect.right - window_rect.left, window_rect.bottom - window_rect.top, nullptr,
                              nullptr, window_class.hInstance, nullptr);
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
  present.BackBufferWidth = kWidth;
  present.BackBufferHeight = kHeight;
  present.BackBufferFormat = D3DFMT_X8R8G8B8;
  present.BackBufferCount = 1;
  present.SwapEffect = D3DSWAPEFFECT_DISCARD;
  present.hDeviceWindow = window;
  present.Windowed = TRUE;
  present.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  present.EnableAutoDepthStencil = TRUE;
  present.AutoDepthStencilFormat = D3DFMT_D24S8;
  ComPtr<IDirect3DDevice9> device;
  CHECK(d3d9->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &present, &device));

  // the texture, sampled point and clamped so a texel's boundary is exactly where the coordinate puts it
  ComPtr<IDirect3DTexture9> texture;
  CHECK(device->CreateTexture(4, 4, 1, 0, D3DFMT_L8, D3DPOOL_MANAGED, &texture, nullptr));
  {
    D3DLOCKED_RECT locked{};
    CHECK(texture->LockRect(0, &locked, nullptr, 0));
    for (UINT row = 0; row < 4; row++)
      memcpy((char *)locked.pBits + (size_t)row * locked.Pitch, kTexels + 4 * row, 4);
    texture->UnlockRect(0);
  }
  CHECK(device->SetTexture(0, texture.Get()));
  CHECK(device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
  CHECK(device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT));
  CHECK(device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE));
  CHECK(device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP));
  CHECK(device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));
  CHECK(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
  CHECK(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
  CHECK(device->SetRenderState(D3DRS_LIGHTING, FALSE));
  CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));

  // the identity texture matrix: D3D9's texture transform is the stage's own matrix, and the test's is the identity
  const D3DMATRIX identity = {{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}};
  CHECK(device->SetTransform(D3DTS_TEXTURE0, &identity));

  ComPtr<IDirect3DVertexShader9> passthrough;
  ComPtr<IDirect3DPixelShader9> sampler;
  CHECK(device->CreateVertexShader(kPassthroughVertexShader, &passthrough));
  CHECK(device->CreatePixelShader(kSamplePixelShader, &sampler));

  ComPtr<IDirect3DSurface9> readback;
  CHECK(device->CreateOffscreenPlainSurface(kWidth, kHeight, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr));
  ComPtr<IDirect3DSurface9> back_buffer;
  auto pixel = [&](UINT x, UINT y) -> DWORD {
    if (!back_buffer)
      return 0;
    D3DLOCKED_RECT locked{};
    if (FAILED(device->GetRenderTargetData(back_buffer.Get(), readback.Get())) ||
        FAILED(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
      return 0;
    const DWORD got = ((const DWORD *)((const char *)locked.pBits + (size_t)y * locked.Pitch))[x] & 0x00FFFFFF;
    readback->UnlockRect();
    return got;
  };
  auto draw = [&](const float *vertices, UINT stride) -> HRESULT {
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(0, 0, 255), 1.0f, 0));
    CHECK(device->BeginScene());
    HRESULT drawn = device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, stride);
    CHECK(device->EndScene());
    back_buffer.Reset();
    CHECK(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer));
    return drawn;
  };

  const DWORD white = 0x00FFFFFF, black = 0x00000000;
  // Wine's quad 1: clip x and y from -1 to 0, and the coordinate from 0 to 4 in both directions, with w 4 and q 6
  const float quad4[4][7] = {
      {-1.0f, -1.0f, 0.1f, 0.0f, 0.0f, 4.0f, 6.0f},
      {-1.0f, 0.0f, 0.1f, 0.0f, 4.0f, 4.0f, 6.0f},
      {0.0f, -1.0f, 0.1f, 4.0f, 0.0f, 4.0f, 6.0f},
      {0.0f, 0.0f, 0.1f, 4.0f, 4.0f, 4.0f, 6.0f},
  };

  static const D3DVERTEXELEMENT9 wide_declaration[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      D3DDECL_END(),
  };
  static const D3DVERTEXELEMENT9 narrow_declaration[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      D3DDECL_END(),
  };

  // the white texel covers the coordinate's [0.25, 0.5) in both directions, which a projection by w = 6 puts at
  // screen x 120..240 and y 300..390 (v counts upward, so the lowest screen row is the highest v)
  auto expect_white_rect = [&](const char *name) {
    expect(pixel(121, 301) == white, name);
    expect(pixel(239, 389) == white, name);
    expect(pixel(119, 299) == black, name);
    expect(pixel(241, 391) == black, name);
    expect(pixel(160, 340) == white, name);
  };

  // ---- a game's vertex shader with the fixed-function pixel stage: COUNT4 behaviour, the divide by w -------------
  {
    ComPtr<IDirect3DVertexDeclaration9> declaration;
    CHECK(device->CreateVertexDeclaration(wide_declaration, &declaration));
    CHECK(device->SetVertexDeclaration(declaration.Get()));
    CHECK(device->SetVertexShader(passthrough.Get()));
    CHECK(device->SetPixelShader(nullptr));
    CHECK(device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT4 | D3DTTFF_PROJECTED));
    expect(SUCCEEDED(draw(&quad4[0][0], sizeof(quad4[0]))), "the game-vertex projected draw was refused");
    expect_white_rect("a game vertex shader with the fixed-function pixel stage did not divide by w");
    {
      UINT drawn = 0, wx0 = 9999, wx1 = 0, wy0 = 9999, wy1 = 0;
      for (UINT y = 0; y < kHeight; y += 2)
        for (UINT x = 0; x < kWidth; x += 2) {
          const DWORD at = pixel(x, y);
          if (at == 0x0000ff)
            continue;
          drawn++;
          if (at == white) {
            if (x < wx0) wx0 = x;
            if (x > wx1) wx1 = x;
            if (y < wy0) wy0 = y;
            if (y > wy1) wy1 = y;
          }
        }
      printf("probe-mixed: %u non-clear samples, white box %u,%u..%u,%u (expected 120,300..239,389)\n", drawn, wx0,
             wy0, wx1, wy1);
    }
    CHECK(device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE));
  }

  // ---- the same declaration through the fixed-function vertex stage and a game pixel shader ---------------------
  {
    ComPtr<IDirect3DVertexDeclaration9> declaration;
    CHECK(device->CreateVertexDeclaration(wide_declaration, &declaration));
    CHECK(device->SetVertexDeclaration(declaration.Get()));
    CHECK(device->SetVertexShader(nullptr));
    CHECK(device->SetPixelShader(sampler.Get()));
    CHECK(device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT4 | D3DTTFF_PROJECTED));
    expect(SUCCEEDED(draw(&quad4[0][0], sizeof(quad4[0]))), "the fixed-vertex projected draw was refused");
    expect_white_rect("the fixed-function vertex stage did not carry the four-component coordinate");

    // PROJECTED alone over the same declaration behaves like COUNT4 | PROJECTED
    CHECK(device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_PROJECTED));
    expect(SUCCEEDED(draw(&quad4[0][0], sizeof(quad4[0]))), "a PROJECTED-alone draw was refused");
    expect_white_rect("PROJECTED alone over a FLOAT4 coordinate did not divide by w");

    // and the invalid 0xffffffff does too
    CHECK(device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, 0xffffffffu));
    expect(SUCCEEDED(draw(&quad4[0][0], sizeof(quad4[0]))), "an 0xffffffff draw was refused");
    expect_white_rect("0xffffffff over a FLOAT4 coordinate did not divide by w");

    // the narrow declaration cannot supply z or w: COUNT3 | PROJECTED draws unprojected, so the coordinate's own
    // [0.25, 0.5) lands at 80..160 in both directions
    CHECK(device->SetVertexDeclaration(nullptr));
    ComPtr<IDirect3DVertexDeclaration9> narrow;
    CHECK(device->CreateVertexDeclaration(narrow_declaration, &narrow));
    CHECK(device->SetVertexDeclaration(narrow.Get()));
    CHECK(device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT3 | D3DTTFF_PROJECTED));
    expect(SUCCEEDED(draw(&quad4[0][0], sizeof(quad4[0]))), "a COUNT3 draw over a FLOAT2 coordinate was refused");
    // undivided, the coordinate's own [0.25, 0.5) is x 20..40 and y 450..465 for this quad (the coordinate spans
    // 0..4 across the quad's 320 by 240 pixels, not 0..1)
    expect(pixel(21, 451) == white && pixel(39, 463) == white,
           "COUNT3 | PROJECTED over a FLOAT2 coordinate did not draw unprojected");
    expect(pixel(19, 451) == black && pixel(41, 463) == black,
           "COUNT3 | PROJECTED over a FLOAT2 coordinate drew more than the unprojected texel");
    CHECK(device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE));
  }

  // ---- the size-4 no-diffuse FVF keeps its own 28-byte declaration, and the diffuse one its 32 ------------------
  {
    CHECK(device->SetVertexDeclaration(nullptr));
    CHECK(device->SetPixelShader(nullptr));
    CHECK(device->SetVertexShader(nullptr));
    // the FVF spelling of the same vertex: position, no colour, and a four-component coordinate
    CHECK(device->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE4(0)));
    CHECK(device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT4 | D3DTTFF_PROJECTED));
    expect(SUCCEEDED(draw(&quad4[0][0], 28)), "a size-4 no-diffuse FVF at its own 28-byte stride was refused");
    expect_white_rect("a size-4 no-diffuse FVF did not carry its coordinate whole");

    // the same vertex with a diffuse colour in front of the coordinate is 32 bytes
    float with_diffuse[4][8];
    for (UINT at = 0; at < 4; at++) {
      memcpy(with_diffuse[at], quad4[at], 3 * sizeof(float));
      with_diffuse[at][3] = 1.0f;
      with_diffuse[at][4] = 1.0f;
      with_diffuse[at][5] = 1.0f;
      with_diffuse[at][6] = 1.0f;
      memcpy(with_diffuse[at] + 4, quad4[at] + 3, 4 * sizeof(float));
    }
    CHECK(device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE4(0)));
    expect(SUCCEEDED(draw(&with_diffuse[0][0], 32)), "a size-4 diffuse FVF at its own 32-byte stride was refused");
    expect_white_rect("a size-4 diffuse FVF did not carry its coordinate whole");
    CHECK(device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE));
  }

  device.Reset();
  d3d9.Reset();
  DestroyWindow(window);
  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
