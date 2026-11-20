// contract: a depth texture sampled by a pixel shader of model 2.0 and above answers the depth comparison, not the
// texel: the reference is the coordinate's third component (divided by the fourth for texldp), the LEQUAL test passes
// when it is at most the texel's depth, and every component the destination writes takes the answer (Microsoft Learn,
// "Shadow maps and depth buffers"; Wine visual.c shadow_test, whose texld + texldp program reads a D16/D24S8 depth
// texture bound after the depth pass). the compare's filter follows the sampler, so a point-filtered stage compares
// the one texel the coordinate names.
#include <windows.h>
#include <d3d9.h>
#include <wrl/client.h>
#include <cstdio>
#include <initializer_list>

using Microsoft::WRL::ComPtr;

#define CHECK(x)                                                                                                       \
  if (FAILED(x)) {                                                                                                     \
    printf("failed: %s (line %d)\n", #x, __LINE__);                                                                    \
    return 1;                                                                                                          \
  }

static const UINT kSize = 64;
static const D3DCOLOR kClear = D3DCOLOR_XRGB(0, 0, 255);

// ps_2_0: dcl_2d s0, dcl t0; r1 = c0; r0 = texld(t0, s0); r1 = texldp(t0, s0); r0.y = r1.x; oC0 = r0 — Wine's own
// shadow_test program, so the drawn pixel names the plain compare in .x/.z/.w and the projected one in .y
static const DWORD shadow_ps[] = {
    0xffff0200u,
    0x0200001fu, 0x90000000u, 0xa00f0800u,                                       // dcl_2d s0
    0x0200001fu, 0x80000000u, 0xb00f0000u,                                       // dcl t0
    0x05000051u, 0xa00f0000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x3f800000u, // def c0, 0, 0, 0, 1
    0x02000001u, 0x800f0001u, 0xa0e40000u,                                       // mov r1, c0
    0x03000042u, 0x800f0000u, 0xb0e40000u, 0xa0e40800u,                          // texld r0, t0, s0
    0x03010042u, 0x800f0001u, 0xb0e40000u, 0xa0e40800u,                          // texldp r1, t0, s0
    0x02000001u, 0x80020000u, 0x80000001u,                                       // mov r0.y, r1.x
    0x02000001u, 0x800f0800u, 0x80e40000u,                                       // mov oC0, r0
    0x0000ffffu,
};

// ps_2_0: a plain texld only — the compare without the projection, which tells the reference from the coordinate
static const DWORD plain_ps[] = {
    0xffff0200u,
    0x0200001fu, 0x90000000u, 0xa00f0800u,                                       // dcl_2d s0
    0x0200001fu, 0x80000000u, 0xb00f0000u,                                       // dcl t0
    0x03000042u, 0x800f0000u, 0xb0e40000u, 0xa0e40800u,                          // texld r0, t0, s0
    0x02000001u, 0x800f0800u, 0x80e40000u,                                       // mov oC0, r0
    0x0000ffffu,
};

// the shadow quad of Wine's shadow_test: z runs 0..1 with x, so the depth texture holds the horizontal coordinate
static const float quad[4][7] = {
    {-1.0f,  1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f},
    { 1.0f,  1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f},
    {-1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    { 1.0f, -1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f},
};

int
main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  unsigned wrong = 0, checks = 0;
  auto expect = [&](bool ok, const char *what) {
    checks++;
    if (!ok && wrong++ < 16)
      printf("wrong: %s\n", what);
  };

  WNDCLASSA wc{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr, "d3d9_shadow"};
  RegisterClassA(&wc);
  HWND window = CreateWindowA("d3d9_shadow", "d3d9_shadow", WS_OVERLAPPEDWINDOW, 0, 0, kSize, kSize, nullptr, nullptr,
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
  present.BackBufferWidth = kSize;
  present.BackBufferHeight = kSize;
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
  CHECK(device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE));
  CHECK(device->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS));
  CHECK(device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE));
  CHECK(device->SetFVF(D3DFVF_XYZ | D3DFVF_TEXCOORDSIZE4(0) | D3DFVF_TEX1));
  for (D3DSAMPLERSTATETYPE state : {D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV})
    CHECK(device->SetSamplerState(0, state, D3DTADDRESS_WRAP));
  CHECK(device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
  CHECK(device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT));
  CHECK(device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_POINT));

  ComPtr<IDirect3DPixelShader9> shader, plain;
  CHECK(device->CreatePixelShader(shadow_ps, &shader));
  CHECK(device->CreatePixelShader(plain_ps, &plain));

  // the draws run into an offscreen render target, as Wine's shadow_test does (its rt is a 1024 texel one); the
  // depth pass's readback needs the same surface
  ComPtr<IDirect3DSurface9> rt;
  CHECK(device->CreateRenderTarget(kSize, kSize, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr));
  CHECK(device->SetRenderTarget(0, rt.Get()));

  ComPtr<IDirect3DSurface9> readback;
  CHECK(device->CreateOffscreenPlainSurface(kSize, kSize, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr));
  auto pixel = [&](UINT x, UINT y) -> D3DCOLOR {
    D3DLOCKED_RECT locked{};
    if (FAILED(device->GetRenderTargetData(rt.Get(), readback.Get())) ||
        FAILED(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
      return 0;
    const DWORD *row = (const DWORD *)((const char *)locked.pBits + (size_t)y * locked.Pitch);
    const D3DCOLOR got = row[x] & 0x00FFFFFF;
    readback->UnlockRect();
    return got;
  };

  // the depth texture's content is the depth pass's own: the quad's z runs with x, so texel (tx, ty) holds
  // (tx + 0.5)/64. a screen pixel (x, y) then reads t0 = (u, 1 - v, 1 - v, v) with u = (x + 0.5)/64 and v = (y + 0.5)/64,
  // so the plain texld's reference is 1 - v against the depth u at (u, 1 - v), and the texldp one is (1 - v)/v against
  // the depth of the wrapped (u/v, (1 - v)/v). The five pixels below name both answers of both instructions, each at
  // least one texel clear of its boundary (worked out by hand, then against the drawn frames):
  //   (16, 16): plain compare fails    -> black (a raw texel sample would answer the depth 0.2578, grey)
  //   (48, 16): plain passes, projected fails (its reference 2.879 against the wrapped depth 0.9453)
  //   (32, 32): plain passes, projected fails (its reference 0.9693 against the wrapped depth 0.0078)
  //   (40, 40): plain passes, projected fails (its reference 1.475 against the wrapped depth 0.8438)
  //   (24, 40): plain passes, projected passes (its reference 0.6000 against the depth 0.6172 at (0.6125, 0.6))
  // so the pair proves both a comparison that answers 0 and one that answers 1, and the plain draw alone proves the
  // answer is the compare and not the texel.
  const D3DCOLOR black = 0x000000, white = 0xffffff, magenta = 0xff00ff;
  struct Point { UINT x, y; D3DCOLOR plain_want, shadow_want; };
  const Point points[] = {
      {16, 16, black, black},
      {48, 16, white, magenta},
      {32, 32, white, magenta},
      {40, 40, white, magenta},
      {24, 40, white, white},
  };

  // the depth formats the device takes as a texture: the ones Wine's shadow_test walks (0.5 and the compared depths
  // are exact in D16 as in D24, so the point-filtered compare is the same value in both)
  const D3DFORMAT formats[] = {D3DFMT_D16, D3DFMT_D24S8, D3DFMT_D24X8};
  for (D3DFORMAT format : formats) {
    if (FAILED(d3d9->CheckDeviceFormat(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE,
                                       format)))
      continue;
    char what[128];
    ComPtr<IDirect3DTexture9> depth;
    CHECK(device->CreateTexture(kSize, kSize, 1, D3DUSAGE_DEPTHSTENCIL, format, D3DPOOL_DEFAULT, &depth, nullptr));
    ComPtr<IDirect3DSurface9> ds;
    CHECK(depth->GetSurfaceLevel(0, &ds));
    CHECK(device->SetDepthStencilSurface(ds.Get()));

    // the depth pass: the quad's z (the horizontal coordinate) is written into the depth texture, with no pixel shader
    // (the fixed-function stage writes no colour) and D3DCMP_ALWAYS so nothing is rejected
    CHECK(device->SetPixelShader(nullptr));
    CHECK(device->SetTexture(0, nullptr));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_ZBUFFER, 0, 0.0f, 0));
    CHECK(device->BeginScene());
    CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, 7 * sizeof(float)));
    CHECK(device->EndScene());
    CHECK(device->SetDepthStencilSurface(nullptr));

    CHECK(device->SetTexture(0, depth.Get()));


    // the plain compare through every named pixel
    CHECK(device->SetPixelShader(plain.Get()));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
    CHECK(device->BeginScene());
    CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, 7 * sizeof(float)));
    CHECK(device->EndScene());
    for (const Point &point : points) {
      snprintf(what, sizeof(what), "%04lx: plain texld at (%u, %u)", (unsigned long)format, point.x, point.y);
      const D3DCOLOR got = pixel(point.x, point.y);
      printf("%s: pixel=%06lx want=%06lx\n", what, (unsigned long)got, (unsigned long)point.plain_want);
      expect(got == point.plain_want, what);
    }

    // Wine's own program: the plain compare in .x/.z and the projected one in .y
    CHECK(device->SetPixelShader(shader.Get()));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
    CHECK(device->BeginScene());
    CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, 7 * sizeof(float)));
    CHECK(device->EndScene());
    for (const Point &point : points) {
      snprintf(what, sizeof(what), "%04lx: texld + texldp at (%u, %u)", (unsigned long)format, point.x, point.y);
      const D3DCOLOR got = pixel(point.x, point.y);
      printf("%s: pixel=%06lx want=%06lx\n", what, (unsigned long)got, (unsigned long)point.shadow_want);
      expect(got == point.shadow_want, what);
    }

    CHECK(device->SetTexture(0, nullptr));
  }

  printf("passed: %d wrong of %d checks\n", wrong, checks);
  return wrong ? 1 : 0;
}
