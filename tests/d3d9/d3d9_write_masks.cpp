// contract: D3DRS_COLORWRITEENABLE{,1,2,3} select channels of each target independently. The test keeps all four
// channels in an A8R8G8B8 readback, starts every channel at a different nonzero sentinel, and has the pixel program
// write a different nonzero value in every channel. Expected pixels are composed channel by channel from those two
// values, so an implementation that only understands NONE and ALL cannot pass.
//
// It covers all sixteen masks on every target, four different masks at once, alpha-only and RGB-only, sparse target
// slots, state-block restore, Reset's defaults, blending enabled after the mask-only passes, and an A8B8G8R8 target
// whose memory order is RGBA rather than BGRA.
#include <windows.h>
#include <d3d9.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

#define CHECK(x)                                                                                                       \
  if (FAILED(x)) {                                                                                                     \
    printf("failed: %s (line %d)\n", #x, __LINE__);                                                                    \
    return 1;                                                                                                          \
  }

namespace {

constexpr UINT kSize = 64;
const D3DCOLOR kSentinels[4] = {
    D3DCOLOR_ARGB(10, 20, 30, 40),
    D3DCOLOR_ARGB(50, 60, 70, 80),
    D3DCOLOR_ARGB(90, 100, 110, 120),
    D3DCOLOR_ARGB(130, 140, 150, 160),
};
const D3DCOLOR kSources[4] = {
    D3DCOLOR_ARGB(200, 50, 100, 150),
    D3DCOLOR_ARGB(240, 220, 40, 80),
    D3DCOLOR_ARGB(160, 180, 60, 20),
    D3DCOLOR_ARGB(120, 140, 160, 180),
};

DWORD
float_bits(float value) {
  DWORD bits = 0;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

D3DCOLOR
masked(D3DCOLOR destination, D3DCOLOR source, DWORD mask) {
  D3DCOLOR result = destination;
  if (mask & D3DCOLORWRITEENABLE_RED)
    result = (result & 0xFF00FFFF) | (source & 0x00FF0000);
  if (mask & D3DCOLORWRITEENABLE_GREEN)
    result = (result & 0xFFFF00FF) | (source & 0x0000FF00);
  if (mask & D3DCOLORWRITEENABLE_BLUE)
    result = (result & 0xFFFFFF00) | (source & 0x000000FF);
  if (mask & D3DCOLORWRITEENABLE_ALPHA)
    result = (result & 0x00FFFFFF) | (source & 0xFF000000);
  return result;
}

unsigned
channel(D3DCOLOR colour, unsigned shift) {
  return (colour >> shift) & 0xFF;
}

D3DCOLOR
blended(D3DCOLOR destination, D3DCOLOR source, DWORD mask) {
  auto sum = [](unsigned a, unsigned b) { return (D3DCOLOR)std::min(255u, a + b); };
  const D3DCOLOR both = D3DCOLOR_ARGB(sum(channel(destination, 24), channel(source, 24)),
                                      sum(channel(destination, 16), channel(source, 16)),
                                      sum(channel(destination, 8), channel(source, 8)),
                                      sum(channel(destination, 0), channel(source, 0)));
  return masked(destination, both, mask);
}

bool
near_colour(D3DCOLOR got, D3DCOLOR wanted) {
  for (unsigned shift : {0u, 8u, 16u, 24u})
    if (std::abs((int)channel(got, shift) - (int)channel(wanted, shift)) > 1)
      return false;
  return true;
}

} // namespace

int
main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  unsigned wrong = 0, checks = 0;
  auto expect = [&](bool ok, const char *what) {
    checks++;
    if (!ok && wrong++ < 24)
      printf("wrong: %s\n", what);
  };

  SetEnvironmentVariableA("MULLION_CONFIG", "dxgi.presentThroughGDI=True");
  WNDCLASSA window_class{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr,
                         "d3d9_write_masks"};
  RegisterClassA(&window_class);
  RECT window_rect{0, 0, (LONG)kSize, (LONG)kSize};
  AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);
  HWND window = CreateWindowA("d3d9_write_masks", "d3d9_write_masks", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0,
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
  present.BackBufferWidth = kSize;
  present.BackBufferHeight = kSize;
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
  CHECK(device->SetRenderState(D3DRS_ZENABLE, FALSE));
  CHECK(device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE));

  ComPtr<IDirect3DSurface9> targets[4], readback;
  for (UINT at = 0; at < 4; at++)
    CHECK(device->CreateRenderTarget(kSize, kSize, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &targets[at],
                                     nullptr));
  CHECK(device->CreateOffscreenPlainSurface(kSize, kSize, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr));

  const D3DVERTEXELEMENT9 elements[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      D3DDECL_END(),
  };
  ComPtr<IDirect3DVertexDeclaration9> declaration;
  CHECK(device->CreateVertexDeclaration(elements, &declaration));
  CHECK(device->SetVertexDeclaration(declaration.Get()));

  const DWORD vertex_shader[] = {
      0xFFFE0200, 0x0200001F, 0x00000000, 0x900F0000, 0x02000001, 0xC00F0000, 0x90E40000, 0x0000FFFF,
  };
  std::vector<DWORD> pixel_shader = {
      0xFFFF0300,
      0x05000051, 0xA00F0000, float_bits(channel(kSources[0], 16) / 255.0f),
      float_bits(channel(kSources[0], 8) / 255.0f), float_bits(channel(kSources[0], 0) / 255.0f),
      float_bits(channel(kSources[0], 24) / 255.0f),
      0x05000051, 0xA00F0001, float_bits(channel(kSources[1], 16) / 255.0f),
      float_bits(channel(kSources[1], 8) / 255.0f), float_bits(channel(kSources[1], 0) / 255.0f),
      float_bits(channel(kSources[1], 24) / 255.0f),
      0x05000051, 0xA00F0002, float_bits(channel(kSources[2], 16) / 255.0f),
      float_bits(channel(kSources[2], 8) / 255.0f), float_bits(channel(kSources[2], 0) / 255.0f),
      float_bits(channel(kSources[2], 24) / 255.0f),
      0x05000051, 0xA00F0003, float_bits(channel(kSources[3], 16) / 255.0f),
      float_bits(channel(kSources[3], 8) / 255.0f), float_bits(channel(kSources[3], 0) / 255.0f),
      float_bits(channel(kSources[3], 24) / 255.0f),
      0x02000001, 0x800F0800, 0xA0E40000, // mov oC0, c0
      0x02000001, 0x800F0801, 0xA0E40001, // mov oC1, c1
      0x02000001, 0x800F0802, 0xA0E40002, // mov oC2, c2
      0x02000001, 0x800F0803, 0xA0E40003, // mov oC3, c3
      0x0000FFFF,
  };
  ComPtr<IDirect3DVertexShader9> vs;
  ComPtr<IDirect3DPixelShader9> ps;
  CHECK(device->CreateVertexShader(vertex_shader, &vs));
  CHECK(device->CreatePixelShader(pixel_shader.data(), &ps));
  CHECK(device->SetVertexShader(vs.Get()));
  CHECK(device->SetPixelShader(ps.Get()));

  const float triangle[3][3] = {{-1.0f, -1.0f, 0.0f}, {-1.0f, 3.0f, 0.0f}, {3.0f, -1.0f, 0.0f}};
  ComPtr<IDirect3DVertexBuffer9> vertex_buffer;
  CHECK(device->CreateVertexBuffer(sizeof(triangle), 0, 0, D3DPOOL_DEFAULT, &vertex_buffer, nullptr));
  void *vertices = nullptr;
  CHECK(vertex_buffer->Lock(0, 0, &vertices, 0));
  memcpy(vertices, triangle, sizeof(triangle));
  CHECK(vertex_buffer->Unlock());
  CHECK(device->SetStreamSource(0, vertex_buffer.Get(), 0, sizeof(triangle[0])));

  const D3DRENDERSTATETYPE mask_states[4] = {D3DRS_COLORWRITEENABLE, D3DRS_COLORWRITEENABLE1,
                                             D3DRS_COLORWRITEENABLE2, D3DRS_COLORWRITEENABLE3};

  auto bind_four = [&]() -> bool {
    return SUCCEEDED(device->SetRenderTarget(0, targets[0].Get())) &&
           SUCCEEDED(device->SetRenderTarget(1, targets[1].Get())) &&
           SUCCEEDED(device->SetRenderTarget(2, targets[2].Get())) &&
           SUCCEEDED(device->SetRenderTarget(3, targets[3].Get()));
  };
  auto clear_four = [&]() -> bool {
    // Clear takes the colour once and applies it to every target the device is rendering to (Microsoft Learn,
    // IDirect3DDevice9::Clear: D3DCLEAR_TARGET clears "the current render target surface(s"), and wined3d walks all
    // its bound views the same way). So each target is bound alone for its own clear, and the four are bound again
    // for the draw.
    for (UINT at = 0; at < 4; at++) {
      if (FAILED(device->SetRenderTarget(0, targets[at].Get())))
        return false;
      for (UINT other = 1; other < 4; other++)
        if (FAILED(device->SetRenderTarget(other, nullptr)))
          return false;
      if (FAILED(device->Clear(0, nullptr, D3DCLEAR_TARGET, kSentinels[at], 1.0f, 0)))
        return false;
    }
    return bind_four();
  };
  auto draw = [&]() -> bool {
    return SUCCEEDED(device->BeginScene()) && SUCCEEDED(device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1)) &&
           SUCCEEDED(device->EndScene());
  };
  auto read_four = [&](D3DCOLOR colours[4]) -> bool {
    D3DLOCKED_RECT locked{};
    for (UINT at = 0; at < 4; at++) {
      if (FAILED(device->GetRenderTargetData(targets[at].Get(), readback.Get())) ||
          FAILED(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
        return false;
      const DWORD *row = (const DWORD *)((const char *)locked.pBits + (kSize / 2) * locked.Pitch);
      colours[at] = row[kSize / 2];
      readback->UnlockRect();
    }
    return true;
  };
  auto set_masks = [&](const DWORD masks[4]) -> bool {
    for (UINT at = 0; at < 4; at++)
      if (FAILED(device->SetRenderState(mask_states[at], masks[at])))
        return false;
    return true;
  };

  // Every one of the sixteen masks is applied to every target, with all four channels read back.
  for (DWORD mask = 0; mask < 16; mask++) {
    const DWORD masks[4] = {mask, mask, mask, mask};
    D3DCOLOR got[4] = {};
    if (!clear_four() || !set_masks(masks) || !draw() || !read_four(got)) {
      expect(false, "a mask-only pass did not complete");
      continue;
    }
    for (UINT at = 0; at < 4; at++) {
      const D3DCOLOR wanted = masked(kSentinels[at], kSources[at], mask);
      char what[128];
      snprintf(what, sizeof(what), "target %u mask %lu read %08lx, wanted %08lx", at, (unsigned long)mask,
               (unsigned long)got[at], (unsigned long)wanted);
      expect(got[at] == wanted, what);
      if (got[at] != wanted)
        printf("probe: %s\n", what);
    }
  }

  // Four different masks in one draw, including RGB-only, alpha-only, NONE and ALL.
  {
    const DWORD masks[4] = {D3DCOLORWRITEENABLE_ALPHA, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                                                          D3DCOLORWRITEENABLE_BLUE,
                            0, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE |
                                   D3DCOLORWRITEENABLE_ALPHA};
    D3DCOLOR got[4] = {};
    expect(clear_four() && set_masks(masks) && draw() && read_four(got),
           "the simultaneous different-mask pass did not complete");
    for (UINT at = 0; at < 4; at++)
      expect(got[at] == masked(kSentinels[at], kSources[at], masks[at]),
             "a target with a different simultaneous mask read the wrong channels");
    printf("probe: simultaneous masks read %08lx %08lx %08lx %08lx\n", (unsigned long)got[0], (unsigned long)got[1],
           (unsigned long)got[2], (unsigned long)got[3]);
  }

  // Blending is enabled after the mask-only passes: the equation applies to enabled channels, and disabled channels
  // still keep the destination. ONE + ONE makes the expected value exact apart from the UNORM round trip.
  {
    const DWORD masks[4] = {D3DCOLORWRITEENABLE_ALPHA,
                            D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE, 0,
                            D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE |
                                D3DCOLORWRITEENABLE_ALPHA};
    D3DCOLOR got[4] = {};
    CHECK(device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE));
    CHECK(device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE));
    CHECK(device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE));
    CHECK(device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD));
    expect(clear_four() && set_masks(masks) && draw() && read_four(got),
           "the blended mask pass did not complete");
    for (UINT at = 0; at < 4; at++)
      expect(near_colour(got[at], blended(kSentinels[at], kSources[at], masks[at])),
             "a mask did not select the blended channels");
    CHECK(device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE));
    CHECK(device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE));
    CHECK(device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ZERO));
    printf("probe: blended masks read %08lx %08lx %08lx %08lx\n", (unsigned long)got[0], (unsigned long)got[1],
           (unsigned long)got[2], (unsigned long)got[3]);
  }

  // Sparse slots: slot 1 is empty while slots 2 and 3 stay bound. oC2 and oC3 must still reach those slots.
  {
    const DWORD masks[4] = {0xF, 0xF, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_ALPHA,
                            D3DCOLORWRITEENABLE_BLUE};
    D3DCOLOR got[4] = {};
    expect(clear_four() && set_masks(masks), "the sparse mask state was not set");
    CHECK(device->SetRenderTarget(1, nullptr));
    expect(draw(), "a draw with slot 1 empty was refused");
    D3DLOCKED_RECT locked{};
    for (UINT at : {0u, 2u, 3u}) {
      if (FAILED(device->GetRenderTargetData(targets[at].Get(), readback.Get())) ||
          FAILED(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
        expect(false, "a sparse target did not read back");
        continue;
      }
      const DWORD *row = (const DWORD *)((const char *)locked.pBits + (kSize / 2) * locked.Pitch);
      got[at] = row[kSize / 2];
      readback->UnlockRect();
      expect(got[at] == masked(kSentinels[at], kSources[at], masks[at]),
             "a sparse target's oC output did not reach its slot with the right mask");
    }
    printf("probe: sparse masks read %08lx %08lx %08lx\n", (unsigned long)got[0], (unsigned long)got[2],
           (unsigned long)got[3]);
    CHECK(bind_four());
  }

  // A state block restores all four masks independently.
  {
    const DWORD before[4] = {D3DCOLORWRITEENABLE_RED, D3DCOLORWRITEENABLE_GREEN, D3DCOLORWRITEENABLE_BLUE,
                             D3DCOLORWRITEENABLE_ALPHA};
    CHECK(set_masks(before));
    ComPtr<IDirect3DStateBlock9> block;
    CHECK(device->CreateStateBlock(D3DSBT_ALL, &block));
    const DWORD changed[4] = {0, 0xF, 0, 0xF};
    CHECK(set_masks(changed));
    CHECK(block->Apply());
    for (UINT at = 0; at < 4; at++) {
      DWORD got = 0;
      expect(SUCCEEDED(device->GetRenderState(mask_states[at], &got)) && got == before[at],
             "a state block did not restore a per-target mask");
    }
  }

  // A8B8G8R8 stores the logical channels in RGBA byte order. Readback stays in the target's format and the expected
  // DWORD is built from the bytes, so a channel swap in the mask path is visible.
  {
    ComPtr<IDirect3DSurface9> rgba_target, rgba_readback;
    HRESULT made = device->CreateRenderTarget(kSize, kSize, D3DFMT_A8B8G8R8, D3DMULTISAMPLE_NONE, 0, FALSE,
                                              &rgba_target, nullptr);
    expect(SUCCEEDED(made), "the device refused an A8B8G8R8 render target it advertises");
    if (SUCCEEDED(made)) {
      CHECK(device->CreateOffscreenPlainSurface(kSize, kSize, D3DFMT_A8B8G8R8, D3DPOOL_SYSTEMMEM, &rgba_readback,
                                                nullptr));
      CHECK(device->SetRenderTarget(1, nullptr));
      CHECK(device->SetRenderTarget(2, nullptr));
      CHECK(device->SetRenderTarget(3, nullptr));
      CHECK(device->SetRenderTarget(0, rgba_target.Get()));
      CHECK(device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED));
      CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(10, 20, 30, 40), 1.0f, 0));
      CHECK(draw());
      D3DLOCKED_RECT locked{};
      CHECK(device->GetRenderTargetData(rgba_target.Get(), rgba_readback.Get()));
      CHECK(rgba_readback->LockRect(&locked, nullptr, D3DLOCK_READONLY));
      const DWORD *row = (const DWORD *)((const char *)locked.pBits + (kSize / 2) * locked.Pitch);
      const DWORD got = row[kSize / 2];
      rgba_readback->UnlockRect();
      // memory bytes R,G,B,A: only R becomes the shader's 50; the other channels keep the sentinels 30, 40, 10.
      const DWORD wanted = (10u << 24) | (40u << 16) | (30u << 8) | 50u;
      expect(got == wanted, "A8B8G8R8 did not apply the red mask to its red byte");
      printf("probe: A8B8G8R8 red-mask pixel %08lx, wanted %08lx\n", (unsigned long)got, (unsigned long)wanted);
      CHECK(device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF));
      CHECK(device->SetRenderTarget(0, targets[0].Get()));
    }
  }

  // Reset returns the four masks to ALL, as D3D9's device defaults are. Default-pool resources must be released first.
  {
    const DWORD none[4] = {0, 0, 0, 0};
    CHECK(set_masks(none));
    ComPtr<IDirect3DSurface9> back_buffer;
    CHECK(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer));
    CHECK(device->SetStreamSource(0, nullptr, 0, 0));
    CHECK(device->SetRenderTarget(1, nullptr));
    CHECK(device->SetRenderTarget(2, nullptr));
    CHECK(device->SetRenderTarget(3, nullptr));
    CHECK(device->SetRenderTarget(0, back_buffer.Get()));
    for (auto &target : targets)
      target.Reset();
    vertex_buffer.Reset();
    back_buffer.Reset();
    CHECK(device->Reset(&present));
    for (UINT at = 0; at < 4; at++) {
      DWORD got = 0;
      expect(SUCCEEDED(device->GetRenderState(mask_states[at], &got)) && got == 0xF,
             "Reset did not restore a per-target colour write mask to ALL");
    }
  }

  device.Reset();
  d3d9.Reset();
  DestroyWindow(window);
  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
