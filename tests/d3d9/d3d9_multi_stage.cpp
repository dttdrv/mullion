// contract: what the fixed-function path's texture stages do past the second one (D3D9's MaxTextureBlendStages is
// eight, Microsoft Learn, "Texture Blending"), which the two-stage pixel path cannot express:
// - a stage past 1 that is active (its colour operation is not D3DTOP_DISABLE) takes part. the stages run in order;
//   each reads the previous stage's result through D3DTA_CURRENT, so a stage 2 that adds D3DTA_TFACTOR to the colour
//   two earlier stages made, and a stage 5 that multiplies it by four, are both visible in the frame.
// - an argument with D3DTA_COMPLEMENT is one minus the argument it names.
// - a stage's D3DTSS_RESULTARG sends its result to D3DTA_TEMP instead of the current, which a later stage reads;
//   D3D9's current register keeps the value it had (Microsoft Learn, D3DTSS_RESULTARG).
// - a vertex with D3DFVF_SPECULAR carries the COLOR1 the D3DTA_SPECULAR argument reads, which D3DRS_SPECULARENABLE
//   adds to the colour after the last stage (Wine's fixed-function pixel pipeline, glsl_shader.c).
// A two-stage-only implementation draws stage 0 and stage 1 and the passes' current, which makes every check here
// wrong in a way the readback sees.
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

#define CHECK(x)                                                                                                       \
  if (HRESULT macro_hr_ = (x); FAILED(macro_hr_)) {                                                                    \
    fprintf(stderr, "failed: %s (line %d) hr=0x%08X\n", #x, __LINE__, (unsigned)macro_hr_);                              \
    return 1;                                                                                                          \
  }

// a vertex already in screen space, with (for the specular case) the second colour D3DFVF_SPECULAR carries
struct PreVertex {
  float x, y, z, rhw;
  D3DCOLOR colour;
  float u, v;
};

struct PreSpecVertex {
  float x, y, z, rhw;
  D3DCOLOR colour;
  D3DCOLOR specular;
};

static const DWORD kPreFVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;
static const DWORD kPreSpecFVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR;

// a right triangle covering the pixel `x, y` in its middle, wound counter-clockwise on the screen (the device draws
// it with culling off)
static void
triangle_at(float x, float y, D3DCOLOR colour, PreVertex vertices[3]) {
  const float size = 64.0f;  vertices[0] = {x - 8.0f,  y - 8.0f, 0.0f, 1.0f, colour, 0.5f, 0.5f};
  vertices[1] = {x - 8.0f,  y + size, 0.0f, 1.0f, colour, 0.5f, 0.5f};
  vertices[2] = {x + size,  y - 8.0f, 0.0f, 1.0f, colour, 0.5f, 0.5f};
}

static D3DCOLOR g_last_read = 0;

// the bytes of a 32-bit surface at `x, y`, as D3D9 names them (X8R8G8B8: 0x00RRGGBB)
static D3DCOLOR
texel(const D3DLOCKED_RECT &locked, UINT x, UINT y) {
  const DWORD *row = (const DWORD *)((const char *)locked.pBits + (size_t)y * locked.Pitch);
  return row[x] & (DWORD)(0x00FFFFFF);
}

static bool
backbuffer_shows(IDirect3DDevice9 *device, UINT x, UINT y, D3DCOLOR want) {
  ComPtr<IDirect3DSurface9> back, readback;
  if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)))
    return false;
  const UINT client = 256;
  if (FAILED(device->CreateOffscreenPlainSurface(client, client, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &readback,
                                                 nullptr)))
    return false;
  if (FAILED(device->GetRenderTargetData(back.Get(), readback.Get())))
    return false;
  D3DLOCKED_RECT locked;
  if (FAILED(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
    return false;
  const D3DCOLOR got = texel(locked, x, y);
  g_last_read = got;
  readback->UnlockRect();
  const int red_want = (int)((want >> 16) & 0xFF), green_want = (int)((want >> 8) & 0xFF), blue_want = (int)(want & 0xFF);
  const DWORD got_dword = got;
  const int red_got = (int)((got_dword >> 16) & 0xFF), green_got = (int)((got_dword >> 8) & 0xFF),
            blue_got = (int)(got_dword & 0xFF);
  return std::abs(red_got - red_want) <= 1 && std::abs(green_got - green_want) <= 1 && std::abs(blue_got - blue_want) <= 1;
}

int
main() {
  unsigned wrong = 0, checks = 0;
  auto expect = [&](bool ok, const char *what) {
    checks++;
    if (!ok && wrong++ < 16)
      printf("wrong: %s (got %06X)\n", what, (unsigned)g_last_read & 0xFFFFFF);
  };

  SetEnvironmentVariableA("MULLION_CONFIG", "dxgi.presentThroughGDI=True");

  const UINT client = 256;
  WNDCLASSA window_class{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr,
                         "d3d9_multi_stage"};
  RegisterClassA(&window_class);
  RECT window_rect{0, 0, (LONG)client, (LONG)client};
  AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);
  HWND window = CreateWindowA("d3d9_multi_stage", "d3d9_multi_stage", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0,
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
  present.BackBufferWidth = client;
  present.BackBufferHeight = client;
  present.BackBufferFormat = D3DFMT_X8R8G8B8;
  present.BackBufferCount = 1;
  present.MultiSampleType = D3DMULTISAMPLE_NONE;
  present.SwapEffect = D3DSWAPEFFECT_DISCARD;
  present.hDeviceWindow = window;
  present.Windowed = TRUE;
  present.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  ComPtr<IDirect3DDevice9> device;
  CHECK(d3d9->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &present, &device));
  CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
  CHECK(device->SetRenderState(D3DRS_SPECULARENABLE, FALSE));
  CHECK(device->SetFVF(kPreFVF));

  const auto disable_stages = [&](UINT first) {
    for (UINT stage = first; stage < 8; stage++) {
      CHECK(device->SetTextureStageState(stage, D3DTSS_COLOROP, D3DTOP_DISABLE));
      CHECK(device->SetTextureStageState(stage, D3DTSS_ALPHAOP, D3DTOP_DISABLE));
      CHECK(device->SetTextureStageState(stage, D3DTSS_COLORARG1, D3DTA_TEXTURE));
      CHECK(device->SetTextureStageState(stage, D3DTSS_COLORARG2, D3DTA_CURRENT));
      CHECK(device->SetTextureStageState(stage, D3DTSS_RESULTARG, D3DTA_CURRENT));
    }
  };
  const auto base_states = [&]() {
    disable_stages(2);
    // stage 1: the previous stage's result, unchanged (SELECT_ARG1, and what the argument names is D3DTA_CURRENT)
    CHECK(device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
    CHECK(device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE));
    CHECK(device->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_CURRENT));
  };

  // a one-texel texture per colour the cases read
  const D3DCOLOR texel_colors[3] = {D3DCOLOR_ARGB(255, 255, 0, 0),                 // red
                                    D3DCOLOR_ARGB(255, 255, 128, 128),             // half grey
                                    D3DCOLOR_ARGB(255, 255, 255, 0)};              // yellow
  ComPtr<IDirect3DTexture9> textures[3];
  for (int kind = 0; kind < 3; kind++) {
    CHECK(device->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &textures[kind], nullptr));
    D3DLOCKED_RECT locked;
    CHECK(textures[kind]->LockRect(0, &locked, nullptr, 0));
    *(D3DCOLOR *)locked.pBits = texel_colors[kind];
    textures[kind]->UnlockRect(0);
  }

  const UINT x = 100, y = 100;
  const D3DCOLOR clear = D3DCOLOR_XRGB(0, 0, 0);

  // ---- stage 2 that adds the texture factor to what stages 0 and 1 made ----
  // stage 0: texture (red) * current (white vertex) = red; stage 1: passes it; stage 2: red + tfactor (green)
  {
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, clear, 0.0f, 0));
    base_states();
    CHECK(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE));
    CHECK(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
    CHECK(device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_CURRENT));
    CHECK(device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE));
    CHECK(device->SetTextureStageState(2, D3DTSS_COLOROP, D3DTOP_ADD));
    CHECK(device->SetTextureStageState(2, D3DTSS_COLORARG1, D3DTA_CURRENT));
    CHECK(device->SetTextureStageState(2, D3DTSS_COLORARG2, D3DTA_TFACTOR));
    CHECK(device->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_ARGB(0, 0, 255, 0)));
    CHECK(device->SetTexture(0, textures[0].Get()));
    PreVertex vertices[3];
    triangle_at((float)x, (float)y, D3DCOLOR_ARGB(255, 255, 255, 255), vertices);
    CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 1, vertices, sizeof(PreVertex)));
    CHECK(device->Present(nullptr, nullptr, nullptr, nullptr));
    expect(backbuffer_shows(device.Get(), x, y, D3DCOLOR_XRGB(255, 255, 0)),
           "stage 2 ADD of D3DTA_TFACTOR to stages 0-1's red makes yellow");
    CHECK(device->SetTexture(0, nullptr));
  }

  // ---- stage 5 that multiplies by four ----
  // stage 0: texture (half grey) * current (white) = grey; stages 1-4 pass it; stage 5: grey * white * 4 = white
  {
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, clear, 0.0f, 0));
    base_states();
    CHECK(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE));
    CHECK(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
    CHECK(device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_CURRENT));
    CHECK(device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE));
    for (UINT stage = 2; stage <= 4; stage++) {
      CHECK(device->SetTextureStageState(stage, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
      CHECK(device->SetTextureStageState(stage, D3DTSS_COLORARG1, D3DTA_CURRENT));
      CHECK(device->SetTextureStageState(stage, D3DTSS_COLORARG2, D3DTA_CURRENT));
    }
    for (UINT stage = 5; stage <= 5; stage++) {
      CHECK(device->SetTextureStageState(stage, D3DTSS_COLOROP, stage == 5 ? D3DTOP_MODULATE4X : D3DTOP_SELECTARG1));
      CHECK(device->SetTextureStageState(stage, D3DTSS_COLORARG1, D3DTA_CURRENT));
      CHECK(device->SetTextureStageState(stage, D3DTSS_COLORARG2, D3DTA_CURRENT));
    }
    CHECK(device->SetTexture(0, textures[1].Get()));
    PreVertex vertices[3];
    triangle_at((float)x, (float)y, D3DCOLOR_ARGB(255, 255, 255, 255), vertices);
    CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 1, vertices, sizeof(PreVertex)));
    CHECK(device->Present(nullptr, nullptr, nullptr, nullptr));
    expect(backbuffer_shows(device.Get(), x, y, D3DCOLOR_XRGB(255, 255, 255)),
           "stage 5 MODULATE4X of stages 0-4's grey makes white");
    CHECK(device->SetTexture(0, nullptr));
  }

  // ---- D3DTA_COMPLEMENT on a stage 0 argument ----
  // the vertex is white, so one minus diffuse is black
  {
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, clear, 0.0f, 0));
    base_states();
    CHECK(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
    CHECK(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_COMPLEMENT | D3DTA_DIFFUSE));
    CHECK(device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE));
    PreVertex vertices[3];
    triangle_at((float)x, (float)y, D3DCOLOR_ARGB(255, 255, 255, 255), vertices);
    CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 1, vertices, sizeof(PreVertex)));
    CHECK(device->Present(nullptr, nullptr, nullptr, nullptr));
    expect(backbuffer_shows(device.Get(), x, y, D3DCOLOR_XRGB(0, 0, 0)),
           "stage 0 SELECT_ARG1 of D3DTA_COMPLEMENT|D3DTA_DIFFUSE makes black");
  }

  // ---- D3DTSS_RESULTARG == D3DTA_TEMP on stage 0 ----
  // stage 0 selects the texture (red) into the temp register, so the current keeps the vertex colour (green);
  // stage 1 adds temp (red) to current (green) = yellow. an implementation that skips the result argument puts red
  // in the current, which its stage 1 add of current+current clamps to plain red again
  {
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, clear, 0.0f, 0));
    base_states();
    CHECK(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
    CHECK(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
    CHECK(device->SetTextureStageState(0, D3DTSS_RESULTARG, D3DTA_TEMP));
    CHECK(device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE));
    CHECK(device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_ADD));
    CHECK(device->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_TEMP));
    CHECK(device->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT));
    CHECK(device->SetTexture(0, textures[0].Get()));
    PreVertex vertices[3];
    triangle_at((float)x, (float)y, D3DCOLOR_ARGB(255, 0, 255, 0), vertices);
    CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 1, vertices, sizeof(PreVertex)));
    CHECK(device->Present(nullptr, nullptr, nullptr, nullptr));
    expect(backbuffer_shows(device.Get(), x, y, D3DCOLOR_XRGB(255, 255, 0)),
           "stage 0's temp result, added to the vertex green it left alone, makes yellow");
    CHECK(device->SetTexture(0, nullptr));
  }

  // ---- D3DFVF_SPECULAR with D3DRS_SPECULARENABLE ----
  // the vertex carries colour (64,0,0) and specular (128,0,0); the enable adds the specular to the colour after the
  // stages (192,0,0), and off leaves the colour alone (64,0,0)
  {
    PreSpecVertex vertices[3] = {
        {(float)x - 8.0f, (float)y - 8.0f, 0.0f, 1.0f, D3DCOLOR_ARGB(255, 64, 0, 0), D3DCOLOR_ARGB(255, 128, 0, 64)},
        {(float)x - 8.0f, (float)y + 64.0f, 0.0f, 1.0f, D3DCOLOR_ARGB(255, 64, 0, 0), D3DCOLOR_ARGB(255, 128, 0, 64)},
        {(float)x + 64.0f, (float)y - 8.0f, 0.0f, 1.0f, D3DCOLOR_ARGB(255, 64, 0, 0), D3DCOLOR_ARGB(255, 128, 0, 64)},
    };
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, clear, 0.0f, 0));
    CHECK(device->SetFVF(kPreSpecFVF));
    disable_stages(0);
    // the specular goes straight to the frame with no texture stages on: stage 0 disabled, which is the pass-through
    // the extended paths draw
    base_states();
    CHECK(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_DISABLE));
    CHECK(device->SetRenderState(D3DRS_SPECULARENABLE, TRUE));
    CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 1, vertices, sizeof(PreSpecVertex)));
    CHECK(device->Present(nullptr, nullptr, nullptr, nullptr));
    expect(backbuffer_shows(device.Get(), x, y, D3DCOLOR_XRGB(192, 0, 64)),
           "D3DFVF_SPECULAR's colour is added to the frame while D3DRS_SPECULARENABLE is on");
    CHECK(device->SetRenderState(D3DRS_SPECULARENABLE, FALSE));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, clear, 0.0f, 0));
    CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 1, vertices, sizeof(PreSpecVertex)));
    CHECK(device->Present(nullptr, nullptr, nullptr, nullptr));
    expect(backbuffer_shows(device.Get(), x, y, D3DCOLOR_XRGB(64, 0, 0)),
           "the same D3DFVF_SPECULAR draw leaves the colour alone while the enable is off");
  }

  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
