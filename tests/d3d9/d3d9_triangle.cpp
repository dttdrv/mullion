// contract: what a device of Direct3D 9's draws through the fixed-function path this milestone has, and what its
// frames look like.
// - D3DFVF_XYZRHW | D3DFVF_DIFFUSE vertices are already in screen space: a triangle of them covers the pixels its
//   three corners name, in the colour of its vertices, over whatever the back buffer was cleared to. the triangle is
//   wound counter-clockwise on the screen, so the test turns culling off (D3D9's default culls that winding; its
//   front faces are the clockwise ones) and what it draws is about the vertex map, not about a culling mode.
// - the triangle is drawn from a vertex buffer (DrawPrimitive), from an index buffer (DrawIndexedPrimitive) and from
//   vertices in memory (DrawPrimitiveUP), and every one of them is read back out of the back buffer and out of the
//   window after Present.
// - a frame with geometry is drawn like the one before it after every Present: four frames in a row, each a different
//   triangle, which is the flip-model chain's turn-over met with the back buffer taken again (the presentation test's
//   frames are clears alone), and the fourth is drawn from a vertex buffer the application released.
// the window's pixels are read through GDI, which has them when frames get to the window through GDI: the test asks
// for that (dxgi.presentThroughGDI), since nothing reads a Metal view's pixels back.
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

#define CHECK(x)                                                                                                       \
  if (FAILED(x)) {                                                                                                     \
    printf("failed: %s (line %d)\n", #x, __LINE__);                                                                    \
    return 1;                                                                                                          \
  }

// a vertex of the one format this milestone transforms: screen-space position, reciprocal w, and a colour
struct PreTransformedVertex {
  float x, y, z, rhw;
  D3DCOLOR colour;
};

static const DWORD kPreTransformedFVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;

// a right triangle with its corner at `x, y`, `size` pixels along each axis, wound counter-clockwise on the screen
// (so the device draws it with culling off, which this test asks for)
static void
triangle_at(float x, float y, float size, D3DCOLOR colour, PreTransformedVertex vertices[3]) {
  vertices[0] = {x, y, 0.0f, 1.0f, colour};
  vertices[1] = {x, y + size, 0.0f, 1.0f, colour};
  vertices[2] = {x + size, y, 0.0f, 1.0f, colour};
}

// the bytes of a 32-bit surface at `x, y`, as D3D9 names them (X8R8G8B8: 0x00RRGGBB)
static D3DCOLOR
texel(const D3DLOCKED_RECT &locked, UINT x, UINT y) {
  const DWORD *row = (const DWORD *)((const char *)locked.pBits + (size_t)y * locked.Pitch);
  return row[x] & 0x00FFFFFF;
}

static bool
window_shows(HWND window, UINT x, UINT y, D3DCOLOR colour) {
  const float want[3] = {((colour >> 16) & 0xFF) / 255.0f, ((colour >> 8) & 0xFF) / 255.0f, (colour & 0xFF) / 255.0f};
  for (UINT attempt = 0; attempt < 250; attempt++) {
    MSG message;
    while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE))
      DispatchMessageA(&message);
    Sleep(20);
    HDC dc = GetDC(window);
    const COLORREF got = GetPixel(dc, x, y);
    ReleaseDC(window, dc);
    const int channels[3] = {(int)GetRValue(got), (int)GetGValue(got), (int)GetBValue(got)};
    bool same = true;
    for (int c = 0; c < 3; c++)
      same &= std::fabs(channels[c] - want[c] * 255) <= 1;
    if (same)
      return true;
  }
  return false;
}

int
main() {
  unsigned wrong = 0, checks = 0;
  auto expect = [&](bool ok, const char *what) {
    checks++;
    if (!ok && wrong++ < 16)
      printf("wrong: %s\n", what);
  };
  SetEnvironmentVariableA("MULLION_CONFIG", "dxgi.presentThroughGDI=True");

  // the window and the device
  const UINT client = 192;
  WNDCLASSA window_class{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr,
                         "d3d9_triangle"};
  RegisterClassA(&window_class);
  RECT window_rect{0, 0, (LONG)client, (LONG)client};
  AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);
  HWND window = CreateWindowA("d3d9_triangle", "d3d9_triangle", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0,
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
  // the triangles are wound counter-clockwise on the screen, which D3D9's default cull mode culls
  CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));

  ComPtr<IDirect3DSurface9> readback;
  CHECK(device->CreateOffscreenPlainSurface(client, client, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr));

  // the frames' colours: what the back buffer is cleared to, and what a triangle of each frame is
  const D3DCOLOR clear = D3DCOLOR_XRGB(16, 16, 48);
  const D3DCOLOR colours[4] = {D3DCOLOR_XRGB(255, 0, 0), D3DCOLOR_XRGB(0, 255, 0), D3DCOLOR_XRGB(0, 0, 255),
                               D3DCOLOR_XRGB(255, 255, 0)};
  // where each frame's triangle is; the one outside it is a pixel past its hypotenuse
  const float corners[4] = {16.0f, 24.0f, 32.0f, 40.0f};
  const float size = 140.0f;

  // a frame's back buffer read back: the pixel inside the triangle and one outside it, the frame's pixels named when
  // either is not what it should be. the pixel inside is returned, so that a frame that is right says what it read
  auto frame_is = [&](const char *what, IDirect3DSurface9 *back_buffer, UINT inside_x, UINT inside_y, D3DCOLOR colour,
                      UINT outside_x, UINT outside_y) -> D3DCOLOR {
    D3DLOCKED_RECT locked{};
    if (FAILED(device->GetRenderTargetData(back_buffer, readback.Get())) ||
        FAILED(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
      expect(false, what);
      return 0xFFFFFFFF;
    }
    const D3DCOLOR want = colour & 0x00FFFFFF, wants = clear & 0x00FFFFFF;
    const D3DCOLOR in = texel(locked, inside_x, inside_y);
    const D3DCOLOR out = texel(locked, outside_x, outside_y);
    if (in != want || out != wants)
      printf("%s: the triangle's pixel %u,%u is %06lx, want %06lx; outside %u,%u is %06lx, want %06lx\n", what,
             inside_x, inside_y, (unsigned long)in, (unsigned long)want, outside_x, outside_y, (unsigned long)out,
             (unsigned long)wants);
    expect(in == want && out == wants, what);
    readback->UnlockRect();
    return in;
  };

  for (unsigned index = 0; index < 4; index++) {
    const D3DCOLOR colour = colours[index];
    const float corner = corners[index];
    const UINT inside_x = (UINT)corner + 12, inside_y = (UINT)corner + 12;
    const UINT outside_x = (UINT)(corner + size) - 8, outside_y = (UINT)(corner + size) - 8;

    PreTransformedVertex vertices[3];
    triangle_at(corner, corner, size, colour, vertices);
    ComPtr<IDirect3DVertexBuffer9> vertex_buffer;
    ComPtr<IDirect3DIndexBuffer9> index_buffer;
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, clear, 1.0f, 0));
    CHECK(device->BeginScene());

    switch (index) {
    case 0: { // DrawPrimitive, from a vertex buffer locked with DISCARD
      CHECK(device->CreateVertexBuffer(sizeof(vertices), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, kPreTransformedFVF,
                                       D3DPOOL_DEFAULT, &vertex_buffer, nullptr));
      void *data = nullptr;
      CHECK(vertex_buffer->Lock(0, 0, &data, D3DLOCK_DISCARD));
      memcpy(data, vertices, sizeof(vertices));
      CHECK(vertex_buffer->Unlock());
      CHECK(device->SetFVF(kPreTransformedFVF));
      CHECK(device->SetStreamSource(0, vertex_buffer.Get(), 0, sizeof(PreTransformedVertex)));
      CHECK(device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1));
      break;
    }
    case 1: { // DrawIndexedPrimitive, from an index buffer
      CHECK(device->CreateVertexBuffer(sizeof(vertices), 0, kPreTransformedFVF, D3DPOOL_MANAGED, &vertex_buffer,
                                       nullptr));
      void *data = nullptr;
      CHECK(vertex_buffer->Lock(0, 0, &data, 0));
      memcpy(data, vertices, sizeof(vertices));
      CHECK(vertex_buffer->Unlock());
      const WORD indices[3] = {0, 1, 2};
      CHECK(device->CreateIndexBuffer(sizeof(indices), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT,
                                      &index_buffer, nullptr));
      void *index_data = nullptr;
      CHECK(index_buffer->Lock(0, 0, &index_data, 0));
      memcpy(index_data, indices, sizeof(indices));
      CHECK(index_buffer->Unlock());
      CHECK(device->SetFVF(kPreTransformedFVF));
      CHECK(device->SetStreamSource(0, vertex_buffer.Get(), 0, sizeof(PreTransformedVertex)));
      CHECK(device->SetIndices(index_buffer.Get()));
      CHECK(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1));
      break;
    }
    case 2: { // DrawPrimitiveUP, from vertices in memory
      CHECK(device->SetFVF(kPreTransformedFVF));
      CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, vertices, sizeof(PreTransformedVertex)));
      break;
    }
    default: { // the fourth frame, from a vertex buffer the application released before the draw
      CHECK(device->CreateVertexBuffer(sizeof(vertices), 0, kPreTransformedFVF, D3DPOOL_MANAGED, &vertex_buffer,
                                       nullptr));
      void *data = nullptr;
      CHECK(vertex_buffer->Lock(0, 0, &data, 0));
      memcpy(data, vertices, sizeof(vertices));
      CHECK(vertex_buffer->Unlock());
      CHECK(device->SetFVF(kPreTransformedFVF));
      CHECK(device->SetStreamSource(0, vertex_buffer.Get(), 0, sizeof(PreTransformedVertex)));
      vertex_buffer.Reset();
      CHECK(device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1));
      break;
    }
    }
    CHECK(device->EndScene());

    ComPtr<IDirect3DSurface9> back_buffer;
    CHECK(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer));
    const D3DCOLOR drawn = frame_is("the drawn triangle is not in the back buffer", back_buffer.Get(), inside_x,
                                    inside_y, colour, outside_x, outside_y);

    CHECK(device->Present(nullptr, nullptr, nullptr, nullptr));
    expect(window_shows(window, inside_x, inside_y, colour), "the window does not show the triangle");
    expect(window_shows(window, outside_x, outside_y, clear), "the window shows a triangle where there is none");

    printf("frame %u: the back buffer at %u,%u reads %06lx, wanted %06lx; the window shows it after Present\n", index,
           inside_x, inside_y, (unsigned long)drawn, (unsigned long)(colour & 0x00FFFFFF));
  }

  // a vertex format this build cannot draw is refused rather than drawn wrong, and the last format that worked stays
  // the device's: two sets of texture coordinates are more than the one stage this build's blender has reads
  expect(device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX2) == D3DERR_INVALIDCALL,
         "a vertex format this build cannot draw is taken");
  CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, clear, 1.0f, 0));
  PreTransformedVertex vertices[3];
  triangle_at(20.0f, 20.0f, 100.0f, D3DCOLOR_XRGB(0, 255, 255), vertices);
  CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, vertices, sizeof(PreTransformedVertex)));
  ComPtr<IDirect3DSurface9> back_buffer;
  CHECK(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer));
  const D3DCOLOR kept_format = frame_is("a refused format did not leave the device's last one", back_buffer.Get(), 40, 40,
                                       D3DCOLOR_XRGB(0, 255, 255), 160, 160);
  printf("the frame after the refused format reads %06lx at 40,40, wanted %06lx\n", (unsigned long)kept_format,
         (unsigned long)(D3DCOLOR_XRGB(0, 255, 255) & 0x00FFFFFF));

  device.Reset();
  d3d9.Reset();
  DestroyWindow(window);

  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
