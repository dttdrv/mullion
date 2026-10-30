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
// - Reset with nothing of the application's outstanding succeeds and loses the state, so the stream source it had is
//   unbound; a draw after the stream is bound again draws.
// - a draw with no stream source is D3DERR_INVALIDCALL.
// - the one stream this milestone draws from takes the setting that does not instance, and no other stream does.
// - MinVertexIndex is the first vertex of the range and NumVertices its length, so a minimum index past the
//   length is a valid draw, and a negative BaseVertexIndex is valid while the range it names stays in the stream.
// - D3DPT_POINTLIST is not a valid type for DrawIndexedPrimitive.
// - a primitive count whose vertex/index count overflows 32 bits is D3DERR_INVALIDCALL, never a smaller draw.
//   a start and stride whose byte product overflows 64 bits is the same refusal (tests/host/stream_range.cpp).
// - a UP draw leaves stream 0 unbound (and DrawIndexedPrimitiveUP the index buffer too), as Microsoft Learn says;
//   an all-state block restores them.
// - D3DRS_SCISSORTESTENABLE clips every draw to SetScissorRect's rectangle, intersected with the viewport; the
//   rectangle cannot name a pixel outside the target, GetScissorRect gives back what was set, and with the test off
//   the scissor clips nothing (Microsoft Learn, SetScissorRect, D3DRS_SCISSORTESTENABLE).
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

  // D3DRS_SCISSORTESTENABLE clips every draw to SetScissorRect's rectangle, intersected with the viewport
  {
    const RECT scissor{40, 50, 90, 100};
    expect(device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE) == S_OK,
           "the scissor test is not a render state the device takes");
    expect(device->SetScissorRect(&scissor) == S_OK, "a scissor rectangle inside the target was refused");
    RECT got{};
    expect(SUCCEEDED(device->GetScissorRect(&got)) && got.left == 40 && got.top == 50 && got.right == 90 &&
               got.bottom == 100,
           "GetScissorRect did not give back the rectangle that was set");
    const RECT outside{150, 150, 200, 200};
    expect(device->SetScissorRect(&outside) == D3DERR_INVALIDCALL,
           "a scissor rectangle outside the target was taken");
    expect(device->SetScissorRect(nullptr) == D3DERR_INVALIDCALL, "a scissor rectangle of nothing was taken");

    // a quad over the whole target, drawn with the scissor test on: only the pixels inside the rectangle are drawn
    PreTransformedVertex quad[6] = {{0.0f, 0.0f, 0.0f, 1.0f, kTriangle},
                                     {0.0f, 192.0f, 0.0f, 1.0f, kTriangle},
                                     {192.0f, 0.0f, 0.0f, 1.0f, kTriangle},
                                     {192.0f, 0.0f, 0.0f, 1.0f, kTriangle},
                                     {0.0f, 192.0f, 0.0f, 1.0f, kTriangle},
                                     {192.0f, 192.0f, 0.0f, 1.0f, kTriangle}};
    CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
    CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, quad, sizeof(PreTransformedVertex)));
    expect(pixel(60, 70) == (kTriangle & 0x00FFFFFF), "a draw inside the scissor rectangle is not drawn");
    expect(pixel(39, 70) == (kClear & 0x00FFFFFF), "a draw is not clipped at the scissor rectangle's left edge");
    expect(pixel(60, 49) == (kClear & 0x00FFFFFF), "a draw is not clipped at the scissor rectangle's top edge");
    expect(pixel(90, 70) == (kClear & 0x00FFFFFF), "a draw is not clipped at the scissor rectangle's right edge");
    expect(pixel(60, 100) == (kClear & 0x00FFFFFF), "a draw is not clipped at the scissor rectangle's bottom edge");

    // the viewport narrows it too: a scissor larger than the viewport clips to the viewport
    const RECT whole{0, 0, 192, 192};
    CHECK(device->SetScissorRect(&whole));
    const D3DVIEWPORT9 half{0, 0, 96, 96, 0.0f, 1.0f};
    CHECK(device->SetViewport(&half));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
    CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, quad, sizeof(PreTransformedVertex)));
    expect(pixel(50, 50) == (kTriangle & 0x00FFFFFF), "a draw inside the viewport is not drawn under a scissor");
    expect(pixel(120, 50) == (kClear & 0x00FFFFFF), "a scissor larger than the viewport drew past the viewport");
    const D3DVIEWPORT9 whole_viewport{0, 0, 192, 192, 0.0f, 1.0f};
    CHECK(device->SetViewport(&whole_viewport));
    CHECK(device->SetScissorRect(&scissor));

    // with the test off the whole quad is drawn, and the rectangle is still the one that was set
    CHECK(device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
    CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, quad, sizeof(PreTransformedVertex)));
    expect(pixel(10, 10) == (kTriangle & 0x00FFFFFF),
           "a draw was clipped by the scissor rectangle while the test was off");
    CHECK(device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE));
    CHECK(device->SetScissorRect(&whole));
    CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW));
  }

  // Following any DrawPrimitiveUP call, the stream 0 settings GetStreamSource reports are NULL (Microsoft Learn,
  // IDirect3DDevice9::DrawPrimitiveUP), so an ordinary draw has no stream to read until one is set again.
  {
    ComPtr<IDirect3DVertexBuffer9> stream;
    UINT offset = 1, stride = 1;
    expect(SUCCEEDED(device->GetStreamSource(0, &stream, &offset, &stride)) && !stream && offset == 0 && stride == 0,
           "DrawPrimitiveUP did not clear stream 0");
    expect(device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1) == D3DERR_INVALIDCALL,
           "a draw after DrawPrimitiveUP used the stream it should have cleared");
    CHECK(device->SetStreamSource(0, vertex_buffer.Get(), 0, sizeof(PreTransformedVertex)));
  }

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

  // MinVertexIndex is the start of the range and NumVertices its length, not a value the other must exceed
  // (Microsoft Learn, IDirect3DDevice9::DrawIndexedPrimitive: "The first vertex is located at index:
  // BaseVertexIndex + MinIndex"). A range at the end of a buffer whose minimum is larger than its length is drawn.
  {
    PreTransformedVertex range_vertices[9] = {};
    for (UINT at = 0; at < 5; at++)
      range_vertices[at] = PreTransformedVertex{0.0f, 0.0f, 0.0f, 1.0f, kTriangle};
    range_vertices[5] = vertices[0];
    range_vertices[6] = vertices[1];
    range_vertices[7] = vertices[2];
    range_vertices[8] = PreTransformedVertex{0.0f, 0.0f, 0.0f, 1.0f, kTriangle};
    ComPtr<IDirect3DVertexBuffer9> range_buffer;
    CHECK(device->CreateVertexBuffer(sizeof(range_vertices), 0, kPreTransformedFVF, D3DPOOL_DEFAULT, &range_buffer,
                                     nullptr));
    void *range_data = nullptr;
    CHECK(range_buffer->Lock(0, 0, &range_data, 0));
    memcpy(range_data, range_vertices, sizeof(range_vertices));
    CHECK(range_buffer->Unlock());
    const WORD range_indices[3] = {5, 6, 7};
    ComPtr<IDirect3DIndexBuffer9> range_index_buffer;
    CHECK(device->CreateIndexBuffer(sizeof(range_indices), 0, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &range_index_buffer,
                                   nullptr));
    void *range_index_data = nullptr;
    CHECK(range_index_buffer->Lock(0, 0, &range_index_data, 0));
    memcpy(range_index_data, range_indices, sizeof(range_indices));
    CHECK(range_index_buffer->Unlock());

    CHECK(device->SetStreamSource(0, range_buffer.Get(), 0, sizeof(PreTransformedVertex)));
    CHECK(device->SetIndices(range_index_buffer.Get()));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
    expect(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 5, 3, 0, 1) == S_OK,
           "a range whose minimum index is past its length was refused");
    expect(pixel(40, 40) == (kTriangle & 0x00FFFFFF),
           "a range whose minimum index is past its length did not draw its vertices");

    // A negative BaseVertexIndex is added to each index, so indices 8, 9 and 10 name vertices 5, 6 and 7: the range
    // MinVertexIndex names is the one after the base is added, and it is inside the stream.
    const WORD negative_indices[3] = {8, 9, 10};
    ComPtr<IDirect3DIndexBuffer9> negative_index_buffer;
    CHECK(device->CreateIndexBuffer(sizeof(negative_indices), 0, D3DFMT_INDEX16, D3DPOOL_DEFAULT,
                                   &negative_index_buffer, nullptr));
    void *negative_index_data = nullptr;
    CHECK(negative_index_buffer->Lock(0, 0, &negative_index_data, 0));
    memcpy(negative_index_data, negative_indices, sizeof(negative_indices));
    CHECK(negative_index_buffer->Unlock());
    CHECK(device->SetIndices(negative_index_buffer.Get()));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
    expect(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, -3, 5, 3, 0, 1) == S_OK,
           "a negative BaseVertexIndex with a range inside the stream was refused");
    expect(pixel(40, 40) == (kTriangle & 0x00FFFFFF),
           "a negative BaseVertexIndex put the range somewhere other than its vertices");

    // a nonzero StartIndex, and a stream offset: the index array's second entry is its first triangle
    const WORD offset_indices[4] = {0xFFFF, 0, 1, 2};
    ComPtr<IDirect3DIndexBuffer9> offset_index_buffer;
    CHECK(device->CreateIndexBuffer(sizeof(offset_indices), 0, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &offset_index_buffer,
                                   nullptr));
    void *offset_index_data = nullptr;
    CHECK(offset_index_buffer->Lock(0, 0, &offset_index_data, 0));
    memcpy(offset_index_data, offset_indices, sizeof(offset_indices));
    CHECK(offset_index_buffer->Unlock());
    const PreTransformedVertex offset_vertices[4] = {{0.0f, 0.0f, 0.0f, 1.0f, kTriangle}, vertices[0], vertices[1],
                                                     vertices[2]};
    ComPtr<IDirect3DVertexBuffer9> offset_buffer;
    CHECK(device->CreateVertexBuffer(sizeof(offset_vertices), 0, kPreTransformedFVF, D3DPOOL_DEFAULT, &offset_buffer,
                                     nullptr));
    void *offset_data = nullptr;
    CHECK(offset_buffer->Lock(0, 0, &offset_data, 0));
    memcpy(offset_data, offset_vertices, sizeof(offset_vertices));
    CHECK(offset_buffer->Unlock());
    CHECK(device->SetStreamSource(0, offset_buffer.Get(), sizeof(PreTransformedVertex), sizeof(PreTransformedVertex)));
    CHECK(device->SetIndices(offset_index_buffer.Get()));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
    expect(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 3, 1, 1) == S_OK,
           "a nonzero StartIndex with a stream offset was refused");
    expect(pixel(40, 40) == (kTriangle & 0x00FFFFFF),
           "a nonzero StartIndex with a stream offset did not draw its vertices");

    // D3DPT_POINTLIST is not a valid type for DrawIndexedPrimitive (Microsoft Learn)
    CHECK(device->SetStreamSource(0, range_buffer.Get(), 0, sizeof(PreTransformedVertex)));
    CHECK(device->SetIndices(range_index_buffer.Get()));
    expect(device->DrawIndexedPrimitive(D3DPT_POINTLIST, 0, 5, 3, 0, 1) == D3DERR_INVALIDCALL,
           "an indexed draw took D3DPT_POINTLIST");

    // A primitive count whose vertex count wraps 32 bits is not a smaller draw: D3DPT_LINELIST's two vertices a
    // primitive and D3DPT_TRIANGLELIST's three both overflow here, and MaxPrimitiveCount is the cap anyway.
    CHECK(device->SetStreamSource(0, vertex_buffer.Get(), 0, sizeof(PreTransformedVertex)));
    expect(device->DrawPrimitive(D3DPT_LINELIST, 0, 0x80000000u) == D3DERR_INVALIDCALL,
           "a DrawPrimitive whose vertex count wraps 32 bits was accepted");
    CHECK(device->SetIndices(index_buffer.Get()));
    expect(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 3, 0, 0x55555556u) == D3DERR_INVALIDCALL,
           "a DrawIndexedPrimitive whose index count wraps 32 bits was accepted");
    expect(device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 3, 0x55555556u, indices, D3DFMT_INDEX16, vertices,
                                          sizeof(PreTransformedVertex)) == D3DERR_INVALIDCALL,
           "a DrawIndexedPrimitiveUP whose index count wraps 32 bits was accepted");
    expect(device->DrawPrimitiveUP(D3DPT_LINELIST, 0x80000000u, vertices, sizeof(PreTransformedVertex)) ==
               D3DERR_INVALIDCALL,
           "a DrawPrimitiveUP whose vertex count wraps 32 bits was accepted");
  }

  // a state block's all-device state includes the stream source and the index buffer, so it puts back what a
  // DrawIndexedPrimitiveUP cleared; the draw itself is refused while they are cleared
  {
    CHECK(device->SetStreamSource(0, vertex_buffer.Get(), 0, sizeof(PreTransformedVertex)));
    CHECK(device->SetIndices(index_buffer.Get()));
    ComPtr<IDirect3DStateBlock9> block;
    CHECK(device->CreateStateBlock(D3DSBT_ALL, &block));
    CHECK(device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 3, 1, indices, D3DFMT_INDEX16, vertices,
                                         sizeof(PreTransformedVertex)));
    {
      ComPtr<IDirect3DVertexBuffer9> stream;
      ComPtr<IDirect3DIndexBuffer9> indices_now;
      UINT offset = 1, stride = 1;
      expect(SUCCEEDED(device->GetStreamSource(0, &stream, &offset, &stride)) && !stream,
             "DrawIndexedPrimitiveUP left the stream source it cleared bound");
      expect(SUCCEEDED(device->GetIndices(&indices_now)) && !indices_now,
             "DrawIndexedPrimitiveUP left the index buffer bound");
    }
    CHECK(block->Apply());
    {
      ComPtr<IDirect3DVertexBuffer9> stream;
      ComPtr<IDirect3DIndexBuffer9> indices_now;
      UINT offset = 0, stride = 0;
      CHECK(device->GetStreamSource(0, &stream, &offset, &stride));
      CHECK(device->GetIndices(&indices_now));
      expect(stream.Get() == vertex_buffer.Get() && offset == 0 && stride == sizeof(PreTransformedVertex),
             "Apply did not restore the stream source a UP draw cleared");
      expect(indices_now.Get() == index_buffer.Get(), "Apply did not restore the index buffer a UP draw cleared");
    }
  }

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

  // Reset succeeds here (the application holds nothing of the chain's), and the state it loses includes the stream
  // source; the draw after the stream is bound again is drawn
  {
    D3DPRESENT_PARAMETERS reset = present;
    CHECK(device->Reset(&reset));
    ComPtr<IDirect3DVertexBuffer9> bound;
    UINT offset = 1, stride = 1;
    CHECK(device->GetStreamSource(0, &bound, &offset, &stride));
    expect(!bound, "Reset kept the stream source bound");

    // the vertex format is state too: Reset loses it, and the draw after it is made with it set again
    CHECK(device->SetFVF(kPreTransformedFVF));
    ComPtr<IDirect3DVertexBuffer9> rebound;
    CHECK(device->CreateVertexBuffer(sizeof(vertices), 0, kPreTransformedFVF, D3DPOOL_DEFAULT, &rebound, nullptr));
    CHECK(rebound->Lock(0, 0, &first, 0));
    memcpy(first, vertices, sizeof(vertices));
    CHECK(rebound->Unlock());
    CHECK(device->SetStreamSource(0, rebound.Get(), 0, sizeof(PreTransformedVertex)));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
    CHECK(device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1));
    expect(pixel(40, 40) == (kTriangle & 0x00FFFFFF), "a draw after a Reset and a rebind is not in the back buffer");
  }

  // instancing is not drawn yet: the stream a draw reads is the one without a divider, and there is only the one
  UINT divider = 0;
  expect(device->SetStreamSourceFreq(0, 1) == S_OK, "the stream a draw reads refuses the setting that does not instance");
  expect(device->GetStreamSourceFreq(0, &divider) == S_OK && divider == 1, "the stream's setting is not 1");
  expect(device->SetStreamSourceFreq(0, 2) == D3DERR_INVALIDCALL, "a stream takes a setting that instances");
  expect(device->SetStreamSourceFreq(1, 1) == S_OK, "stream 1 (MaxStreams is 4) refused a non-instancing setting");
  expect(device->SetStreamSourceFreq(4, 1) == D3DERR_INVALIDCALL, "a stream past MaxStreams took a setting");

  // no stream source: nothing to draw from
  CHECK(device->SetStreamSource(0, nullptr, 0, 0));
  CHECK(device->SetIndices(nullptr));
  expect(device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1) == D3DERR_INVALIDCALL, "a draw with no stream source drew");
  expect(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1) == D3DERR_INVALIDCALL,
         "an indexed draw with no stream source drew");

  device.Reset();
  d3d9.Reset();
  DestroyWindow(window);

  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
