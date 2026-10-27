// contract: what Direct3D 9's draw path refuses and what it keeps between calls, each with the code D3D9 names for it
// rather than a crash or a wrong frame.
// - a draw of no primitives succeeds and draws nothing.
// - a screen-clockwise triangle is a back face: the default cull mode is D3DCULL_CW, and nothing sets another one yet.
// - an indexed draw whose index count or vertex range does not fit the buffers it names is D3DERR_INVALIDCALL, and
//   the frame is what it was: nothing is drawn.
// - D3DLOCK_DISCARD and D3DLOCK_NOOVERWRITE are only a dynamic buffer's (D3DERR_INVALIDCALL on any other), and a
//   second lock without an unlock is D3DERR_INVALIDCALL too.
// - a vertex buffer is the device's while it is the stream source: the application's release does not take it away,
//   and the draw after the release still draws.
// - Reset is still refused (this build has none), and a draw after that refusal still draws.
// - a draw with no stream source is D3DERR_INVALIDCALL.
// - the one stream this milestone draws from takes the setting that does not instance, and no other stream does.
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

#define CHECK(x)                                                                                                       \
  if (FAILED(x)) {                                                                                                     \
    printf("failed: %s (line %d)\n", #x, __LINE__);                                                                    \
    return 1;                                                                                                          \
  }

struct PreTransformedVertex {
  float x, y, z, rhw;
  D3DCOLOR colour;
};

static const DWORD kPreTransformedFVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;
static const D3DCOLOR kClear = D3DCOLOR_XRGB(10, 20, 30);
static const D3DCOLOR kTriangle = D3DCOLOR_XRGB(0, 255, 255);

int
main() {
  unsigned wrong = 0, checks = 0;
  auto expect = [&](bool ok, const char *what) {
    checks++;
    if (!ok && wrong++ < 16)
      printf("wrong: %s\n", what);
  };

  const UINT client = 192;
  WNDCLASSA window_class{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr,
                         "d3d9_draw_contract"};
  RegisterClassA(&window_class);
  RECT window_rect{0, 0, (LONG)client, (LONG)client};
  AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);
  HWND window = CreateWindowA("d3d9_draw_contract", "d3d9_draw_contract", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0,
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
  ComPtr<IDirect3DSurface9> readback;
  CHECK(device->CreateOffscreenPlainSurface(client, client, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr));

  // the pixel at `x, y` of the back buffer, read back into the system-memory surface
  auto pixel = [&](UINT x, UINT y) -> D3DCOLOR {
    ComPtr<IDirect3DSurface9> back_buffer;
    D3DLOCKED_RECT locked{};
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer)) ||
        FAILED(device->GetRenderTargetData(back_buffer.Get(), readback.Get())) ||
        FAILED(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
      return 0;
    const DWORD *row = (const DWORD *)((const char *)locked.pBits + (size_t)y * locked.Pitch);
    const D3DCOLOR got = row[x] & 0x00FFFFFF;
    readback->UnlockRect();
    return got;
  };

  // a triangle this device draws under its default states, and the frame it is checked in: clockwise on the screen,
  // which is the way round the device's default cull mode (D3DCULL_CCW) draws a face
  PreTransformedVertex vertices[3] = {{20.0f, 20.0f, 0.0f, 1.0f, kTriangle},
                                      {120.0f, 20.0f, 0.0f, 1.0f, kTriangle},
                                      {20.0f, 120.0f, 0.0f, 1.0f, kTriangle}};
  ComPtr<IDirect3DVertexBuffer9> vertex_buffer;
  CHECK(device->CreateVertexBuffer(sizeof(vertices), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, kPreTransformedFVF,
                                   D3DPOOL_DEFAULT, &vertex_buffer, nullptr));
  void *data = nullptr;
  CHECK(vertex_buffer->Lock(0, 0, &data, D3DLOCK_DISCARD));
  memcpy(data, vertices, sizeof(vertices));
  CHECK(vertex_buffer->Unlock());
  CHECK(device->SetFVF(kPreTransformedFVF));
  CHECK(device->SetStreamSource(0, vertex_buffer.Get(), 0, sizeof(PreTransformedVertex)));

  // a draw of no primitives: no error, and no pixels
  CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
  expect(device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 0) == S_OK, "a draw of no primitives is not S_OK");
  expect(pixel(40, 40) == (kClear & 0x00FFFFFF), "a draw of no primitives drew something");

  // the same draw with one primitive is the frame's triangle: the boundary checks below are measured against a frame
  // the path does draw
  CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
  CHECK(device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1));
  expect(pixel(40, 40) == (kTriangle & 0x00FFFFFF), "a triangle from a vertex buffer is not in the back buffer");

  // the other way round is a back face: the default cull mode is D3DCULL_CCW, and nothing sets another one yet
  PreTransformedVertex counter_clockwise[3] = {vertices[0], vertices[2], vertices[1]};
  CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
  CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, counter_clockwise, sizeof(PreTransformedVertex)));
  expect(pixel(40, 40) == (kClear & 0x00FFFFFF), "a counter-clockwise triangle is drawn past the default cull mode");

  // an index buffer of three indices, and what is refused around it
  const WORD indices[3] = {0, 1, 2};
  ComPtr<IDirect3DIndexBuffer9> index_buffer;
  CHECK(device->CreateIndexBuffer(sizeof(indices), 0, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &index_buffer, nullptr));
  void *index_data = nullptr;
  CHECK(index_buffer->Lock(0, 0, &index_data, 0));
  memcpy(index_data, indices, sizeof(indices));
  CHECK(index_buffer->Unlock());
  CHECK(device->SetIndices(index_buffer.Get()));

  CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
  expect(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 3, 0, 4) == D3DERR_INVALIDCALL,
         "an index count past the index buffer is drawn");
  expect(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 1) == D3DERR_INVALIDCALL,
         "a vertex range past the vertex buffer is drawn");
  expect(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 1, 0, 3, 0, 1) == D3DERR_INVALIDCALL,
         "a base vertex that puts the range past the vertex buffer is drawn");
  expect(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 3, 1, 1) == D3DERR_INVALIDCALL,
         "an index range past the index buffer is drawn");
  expect(pixel(40, 40) == (kClear & 0x00FFFFFF), "a refused indexed draw drew something");

  // what is not refused, around the same buffer
  expect(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 3, 0, 0) == S_OK,
         "an indexed draw of no primitives is not S_OK");
  CHECK(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1));
  expect(pixel(40, 40) == (kTriangle & 0x00FFFFFF), "a triangle from an index buffer is not in the back buffer");

  // a lock's flags: DISCARD and NOOVERWRITE are a dynamic buffer's alone
  void *first = nullptr, *second = nullptr;
  expect(vertex_buffer->Lock(0, 0, &first, D3DLOCK_DISCARD) == S_OK, "a dynamic buffer refuses DISCARD");
  CHECK(vertex_buffer->Unlock());
  expect(vertex_buffer->Lock(0, 0, &first, D3DLOCK_NOOVERWRITE) == S_OK, "a dynamic buffer refuses NOOVERWRITE");
  CHECK(vertex_buffer->Unlock());

  ComPtr<IDirect3DVertexBuffer9> still_buffer;
  CHECK(device->CreateVertexBuffer(sizeof(vertices), 0, kPreTransformedFVF, D3DPOOL_DEFAULT, &still_buffer, nullptr));
  expect(still_buffer->Lock(0, 0, &first, D3DLOCK_DISCARD) == D3DERR_INVALIDCALL,
         "a buffer that is not dynamic takes DISCARD");
  expect(still_buffer->Lock(0, 0, &first, D3DLOCK_NOOVERWRITE) == D3DERR_INVALIDCALL,
         "a buffer that is not dynamic takes NOOVERWRITE");
  CHECK(still_buffer->Lock(0, 0, &first, 0));
  expect(still_buffer->Lock(0, 0, &second, 0) == D3DERR_INVALIDCALL, "a buffer locks twice without an unlock");
  CHECK(still_buffer->Unlock());

  // a vertex buffer is the device's while it is the stream source: releasing the application's reference does not
  // take it away, and the draw after the release draws
  CHECK(still_buffer->Lock(0, 0, &first, 0));
  memcpy(first, vertices, sizeof(vertices));
  CHECK(still_buffer->Unlock());
  CHECK(device->SetStreamSource(0, still_buffer.Get(), 0, sizeof(PreTransformedVertex)));
  still_buffer.Reset();
  CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
  CHECK(device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1));
  expect(pixel(40, 40) == (kTriangle & 0x00FFFFFF), "a draw from a released stream source is not in the back buffer");

  // Reset is refused, and a draw after the refusal is still drawn
  expect(device->Reset(&present) == D3DERR_INVALIDCALL, "Reset changed the device");
  CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
  CHECK(device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1));
  expect(pixel(40, 40) == (kTriangle & 0x00FFFFFF), "a draw after a refused Reset is not in the back buffer");

  // instancing is not drawn yet: the stream a draw reads is the one without a divider, and there is only the one
  UINT divider = 0;
  expect(device->SetStreamSourceFreq(0, 1) == S_OK, "the stream a draw reads refuses the setting that does not instance");
  expect(device->GetStreamSourceFreq(0, &divider) == S_OK && divider == 1, "the stream's setting is not 1");
  expect(device->SetStreamSourceFreq(0, 2) == D3DERR_INVALIDCALL, "a stream takes a setting that instances");
  expect(device->SetStreamSourceFreq(1, 1) == D3DERR_INVALIDCALL, "a second stream takes a setting");

  // no stream source: nothing to draw from
  CHECK(device->SetStreamSource(0, nullptr, 0, 0));
  expect(device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1) == D3DERR_INVALIDCALL, "a draw with no stream source drew");
  expect(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1) == D3DERR_INVALIDCALL,
         "an indexed draw with no stream source drew");

  device.Reset();
  d3d9.Reset();
  DestroyWindow(window);

  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
