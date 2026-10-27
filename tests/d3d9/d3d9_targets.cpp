// contract: the surfaces a frame is drawn into and tested against, and the viewport that places it, read back out of
// the pixels rather than out of a return code.
// - CreateDepthStencilSurface makes a surface a device can be given with SetDepthStencilSurface, GetDepthStencilSurface
//   hands it back, and depth testing is what it does (a nearer triangle wins over a farther one on a custom target too).
// - CreateRenderTarget makes a surface SetRenderTarget takes: Clear and the draws go to it, and the back buffer keeps
//   what it had; SetRenderTarget back to the back buffer puts the draws there and leaves the custom target alone.
// - SetViewport places and clips the draw: a triangle larger than the viewport is drawn inside it and not outside, and
//   the vertex map follows the viewport's origin, so a pixel keeps the place it names.
// - what cannot be a target (a depth surface, a target index but 0, no surface at all), what does not match it (a depth
//   surface of another size, a target of another size than the depth surface) and a viewport that names no pixels or
//   lies outside the target are D3DERR_INVALIDCALL, and the device keeps the target and viewport it had.
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

// a right triangle with its corner at `x, y`, `size` pixels along each axis, counter-clockwise on the screen, every
// vertex at `z` and in `colour`
static void
triangle_at(float x, float y, float size, float z, D3DCOLOR colour, PreTransformedVertex vertices[3]) {
  vertices[0] = {x, y, z, 1.0f, colour};
  vertices[1] = {x, y + size, z, 1.0f, colour};
  vertices[2] = {x + size, y, z, 1.0f, colour};
}

int
main() {
  unsigned wrong = 0, checks = 0;
  auto expect = [&](bool ok, const char *what) {
    checks++;
    if (!ok && wrong++ < 20)
      printf("wrong: %s\n", what);
  };

  const UINT client = 192;
  WNDCLASSA window_class{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr,
                         "d3d9_targets"};
  RegisterClassA(&window_class);
  RECT window_rect{0, 0, (LONG)client, (LONG)client};
  AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);
  HWND window = CreateWindowA("d3d9_targets", "d3d9_targets", WS_OVERLAPPEDWINDOW, 0, 0,
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
  CHECK(device->SetFVF(kPreTransformedFVF));
  // the triangles are wound counter-clockwise on the screen, which D3D9's default cull mode culls
  CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));

  // the default target, a depth surface for it, and the surfaces frames are read back into
  ComPtr<IDirect3DSurface9> back_buffer;
  CHECK(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer));
  ComPtr<IDirect3DSurface9> depth;
  CHECK(device->CreateDepthStencilSurface(client, client, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &depth,
                                          nullptr));
  CHECK(device->SetDepthStencilSurface(depth.Get()));
  ComPtr<IDirect3DSurface9> current_depth;
  CHECK(device->GetDepthStencilSurface(&current_depth));
  expect(current_depth.Get() == depth.Get(), "GetDepthStencilSurface did not return the surface that was set");
  D3DSURFACE_DESC depth_desc{};
  CHECK(depth->GetDesc(&depth_desc));
  expect(depth_desc.Width == client && depth_desc.Height == client && depth_desc.Format == D3DFMT_D24S8 &&
             depth_desc.Pool == D3DPOOL_DEFAULT,
         "the depth surface's description is not the one it was made with");

  ComPtr<IDirect3DSurface9> readback192, readback96;
  CHECK(device->CreateOffscreenPlainSurface(client, client, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &readback192,
                                            nullptr));
  CHECK(device->CreateOffscreenPlainSurface(96, 96, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &readback96, nullptr));

  // the pixel at `x, y` of `source`, copied into the readback surface of its size
  auto pixel_of = [&](IDirect3DSurface9 *source, IDirect3DSurface9 *readback, UINT x, UINT y) -> D3DCOLOR {
    D3DLOCKED_RECT locked{};
    if (FAILED(device->GetRenderTargetData(source, readback)) ||
        FAILED(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
      return 0;
    const DWORD *row = (const DWORD *)((const char *)locked.pBits + (size_t)y * locked.Pitch);
    const D3DCOLOR got = row[x] & 0x00FFFFFF;
    readback->UnlockRect();
    return got;
  };
  auto back_pixel = [&](UINT x, UINT y) { return pixel_of(back_buffer.Get(), readback192.Get(), x, y); };
  auto draw = [&](const PreTransformedVertex vertices[3]) {
    expect(SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, vertices, sizeof(PreTransformedVertex))),
           "the draw path refused a triangle");
  };

  // a custom render target, and what reaches it
  ComPtr<IDirect3DSurface9> render_target;
  CHECK(device->CreateRenderTarget(96, 96, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &render_target, nullptr));
  D3DSURFACE_DESC target_desc{};
  CHECK(render_target->GetDesc(&target_desc));
  expect(target_desc.Width == 96 && target_desc.Height == 96 && target_desc.Format == D3DFMT_X8R8G8B8,
         "the render target's description is not the one it was made with");

  CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
  // the depth surface is the back buffer's size, so the 96 by 96 target cannot be drawn with it set
  CHECK(device->SetDepthStencilSurface(nullptr));
  CHECK(device->SetRenderTarget(0, render_target.Get()));
  ComPtr<IDirect3DSurface9> current_target;
  CHECK(device->GetRenderTarget(0, &current_target));
  expect(current_target.Get() == render_target.Get(), "GetRenderTarget did not return the surface that was set");

  CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 255), 1.0f, 0));
  expect(pixel_of(render_target.Get(), readback96.Get(), 40, 40) == 0x0000FF, "Clear did not clear the custom target");
  expect(back_pixel(40, 40) == (kClear & 0x00FFFFFF), "Clear of the custom target cleared the back buffer");

  // a target brings its own viewport: D3D9 places the viewport on the target SetRenderTarget set, so a draw with no
  // SetViewport since covers the new target rather than the old target's place in it
  D3DVIEWPORT9 placed{};
  CHECK(device->GetViewport(&placed));
  expect(placed.X == 0 && placed.Y == 0 && placed.Width == 96 && placed.Height == 96,
         "SetRenderTarget did not place the viewport on the target it set");
  PreTransformedVertex whole_target[3];
  triangle_at(0.0f, 0.0f, 96.0f, 0.0f, D3DCOLOR_XRGB(0, 255, 0), whole_target);
  draw(whole_target);
  // a pixel inside the triangle the target's own viewport maps (the corner of the 96 by 96 triangle) and outside the
  // smaller one the back buffer's viewport would map it to
  expect(pixel_of(render_target.Get(), readback96.Get(), 40, 40) == 0x00FF00,
         "a draw after SetRenderTarget did not map through the target's own viewport");

  PreTransformedVertex red[3];
  triangle_at(20.0f, 20.0f, 60.0f, 0.0f, D3DCOLOR_XRGB(255, 0, 0), red);
  draw(red);
  expect(pixel_of(render_target.Get(), readback96.Get(), 40, 40) == 0xFF0000,
         "the custom target did not receive the draw");
  expect(back_pixel(40, 40) == (kClear & 0x00FFFFFF), "the back buffer received a draw made into the custom target");
  printf("target: the custom target reads %06lx at 40,40, the back buffer %06lx\n",
         (unsigned long)pixel_of(render_target.Get(), readback96.Get(), 40, 40), (unsigned long)back_pixel(40, 40));

  // back to the back buffer: the draws go there, and the custom target keeps what it had
  CHECK(device->SetRenderTarget(0, back_buffer.Get()));
  CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
  PreTransformedVertex green[3];
  triangle_at(20.0f, 20.0f, 140.0f, 0.0f, D3DCOLOR_XRGB(0, 255, 0), green);
  draw(green);
  expect(back_pixel(40, 40) == 0x00FF00, "the back buffer did not receive the draw after SetRenderTarget put it back");
  expect(pixel_of(render_target.Get(), readback96.Get(), 40, 40) == 0xFF0000,
         "a draw into the back buffer reached the custom target");
  printf("target: the back buffer reads %06lx at 40,40 while the custom target still reads ff0000\n",
         (unsigned long)back_pixel(40, 40));

  // the viewport: it clips the draw, and the vertex map follows its origin
  CHECK(device->SetDepthStencilSurface(depth.Get()));
  D3DVIEWPORT9 viewport{32, 32, 64, 64, 0.0f, 1.0f};
  CHECK(device->SetViewport(&viewport));
  D3DVIEWPORT9 read_back{};
  CHECK(device->GetViewport(&read_back));
  expect(read_back.X == 32 && read_back.Y == 32 && read_back.Width == 64 && read_back.Height == 64,
         "GetViewport did not return the viewport that was set");
  CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, kClear, 1.0f, 0));
  PreTransformedVertex yellow[3];
  triangle_at(20.0f, 20.0f, 140.0f, 0.0f, D3DCOLOR_XRGB(255, 255, 0), yellow);
  draw(yellow);
  const D3DCOLOR inside = back_pixel(40, 40), outside = back_pixel(110, 50);
  expect(inside == 0xFFFF00, "a pixel inside the viewport and the triangle was not drawn");
  expect(outside == (kClear & 0x00FFFFFF), "the viewport did not clip a pixel outside it");
  printf("viewport: 40,40 reads %06lx inside the viewport, 110,50 (in the triangle, outside it) reads %06lx\n",
         (unsigned long)inside, (unsigned long)outside);

  viewport = {0, 0, client, client, 0.0f, 1.0f};
  CHECK(device->SetViewport(&viewport));
  CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, kClear, 1.0f, 0));
  draw(yellow);
  const D3DCOLOR full_viewport = back_pixel(110, 50);
  expect(full_viewport == 0xFFFF00, "a pixel outside the smaller viewport was not drawn with the full one");
  printf("viewport: with the full one, 110,50 reads %06lx\n", (unsigned long)full_viewport);

  // what cannot be a target, and what does not match the one there is
  expect(device->SetRenderTarget(0, nullptr) == D3DERR_INVALIDCALL, "SetRenderTarget took no surface");
  expect(device->SetRenderTarget(1, render_target.Get()) == D3DERR_INVALIDCALL, "SetRenderTarget took a second target");
  expect(device->SetRenderTarget(0, depth.Get()) == D3DERR_INVALIDCALL, "SetRenderTarget took a depth surface");
  expect(device->SetDepthStencilSurface(render_target.Get()) == D3DERR_INVALIDCALL,
         "SetDepthStencilSurface took a colour target");
  expect(device->GetRenderTarget(1, &current_target) == D3DERR_INVALIDCALL, "GetRenderTarget returned a second target");

  ComPtr<IDirect3DSurface9> small_depth;
  CHECK(device->CreateDepthStencilSurface(96, 96, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &small_depth, nullptr));
  expect(device->SetDepthStencilSurface(small_depth.Get()) == D3DERR_INVALIDCALL,
         "a depth surface of another size than the target was taken");
  expect(device->SetRenderTarget(0, render_target.Get()) == D3DERR_INVALIDCALL,
         "a target of another size than the depth surface was taken");
  expect(device->SetDepthStencilSurface(nullptr) == S_OK, "SetDepthStencilSurface did not take no surface");
  ComPtr<IDirect3DSurface9> no_depth;
  expect(device->GetDepthStencilSurface(&no_depth) == S_OK && no_depth.Get() == nullptr,
         "GetDepthStencilSurface did not report no surface");
  CHECK(device->SetRenderTarget(0, render_target.Get()));
  CHECK(device->SetDepthStencilSurface(small_depth.Get()));

  // depth on the custom target: the near triangle still wins
  CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, kClear, 1.0f, 0));
  PreTransformedVertex far_triangle[3], near_triangle[3];
  triangle_at(20.0f, 20.0f, 60.0f, 0.75f, D3DCOLOR_XRGB(255, 0, 0), far_triangle);
  triangle_at(20.0f, 20.0f, 60.0f, 0.25f, D3DCOLOR_XRGB(0, 255, 0), near_triangle);
  draw(far_triangle);
  draw(near_triangle);
  expect(pixel_of(render_target.Get(), readback96.Get(), 40, 40) == 0x00FF00,
         "depth on the custom target did not keep the near triangle");
  printf("target: depth on the custom target reads %06lx at 40,40\n",
         (unsigned long)pixel_of(render_target.Get(), readback96.Get(), 40, 40));

  // a viewport that names no pixels, or lies outside the target, is refused and the device keeps the one it had
  expect(device->SetViewport(nullptr) == D3DERR_INVALIDCALL, "SetViewport took no viewport");
  D3DVIEWPORT9 small_viewport{0, 0, 96, 96, 0.0f, 1.0f};
  CHECK(device->SetViewport(&small_viewport));
  D3DVIEWPORT9 bad{0, 0, 0, 64, 0.0f, 1.0f};
  expect(device->SetViewport(&bad) == D3DERR_INVALIDCALL, "a viewport of no width was taken");
  bad = D3DVIEWPORT9{64, 64, 64, 64, 0.0f, 1.0f};
  expect(device->SetViewport(&bad) == D3DERR_INVALIDCALL, "a viewport outside the target was taken");
  bad = D3DVIEWPORT9{0, 0, 64, 64, 0.5f, 0.2f};
  expect(device->SetViewport(&bad) == D3DERR_INVALIDCALL, "a viewport with its near plane behind its far one was taken");
  expect(device->GetViewport(nullptr) == D3DERR_INVALIDCALL, "GetViewport took no place to write");
  CHECK(device->GetViewport(&read_back));
  expect(read_back.Width == 96 && read_back.Height == 96,
         "a refused viewport changed the viewport the custom target had");

  // a chain asked for an automatic depth-stencil surface gets one: a game that never creates a surface still has depth
  RECT auto_rect{0, 0, 96, 96};
  AdjustWindowRect(&auto_rect, WS_OVERLAPPEDWINDOW, FALSE);
  HWND auto_window = CreateWindowA("d3d9_targets", "d3d9_targets auto depth", WS_OVERLAPPEDWINDOW, 0, 0,
                                   auto_rect.right - auto_rect.left, auto_rect.bottom - auto_rect.top, nullptr,
                                   nullptr, window_class.hInstance, nullptr);
  expect(auto_window != nullptr, "no second window for the automatic depth-stencil surface");
  if (auto_window) {
    D3DPRESENT_PARAMETERS auto_present = present;
    auto_present.BackBufferWidth = auto_present.BackBufferHeight = 96;
    auto_present.hDeviceWindow = auto_window;
    auto_present.EnableAutoDepthStencil = TRUE;
    auto_present.AutoDepthStencilFormat = D3DFMT_D24S8;
    ComPtr<IDirect3DDevice9> auto_device;
    CHECK(d3d9->CreateDevice(0, D3DDEVTYPE_HAL, auto_window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &auto_present,
                             &auto_device));
    ComPtr<IDirect3DSurface9> auto_depth;
    expect(auto_device->GetDepthStencilSurface(&auto_depth) == S_OK && auto_depth.Get() != nullptr,
           "a device made with an automatic depth-stencil surface has none");
    CHECK(auto_device->SetFVF(kPreTransformedFVF));
    // the triangles are wound counter-clockwise on the screen, which D3D9's default cull mode culls
    CHECK(auto_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
    CHECK(auto_device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, kClear, 1.0f, 0));
    PreTransformedVertex auto_far[3], auto_near[3];
    triangle_at(20.0f, 20.0f, 60.0f, 0.75f, D3DCOLOR_XRGB(255, 0, 0), auto_far);
    triangle_at(20.0f, 20.0f, 60.0f, 0.25f, D3DCOLOR_XRGB(0, 255, 0), auto_near);
    CHECK(auto_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, auto_far, sizeof(PreTransformedVertex)));
    CHECK(auto_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, auto_near, sizeof(PreTransformedVertex)));
    // the readback surface is the second device's own: a copy is a device's, and one made by another device's is not
    // the destination of it (D3D9's GetRenderTargetData works within a device)
    ComPtr<IDirect3DSurface9> auto_readback, auto_back_buffer;
    CHECK(auto_device->CreateOffscreenPlainSurface(96, 96, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &auto_readback,
                                                   nullptr));
    CHECK(auto_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &auto_back_buffer));
    CHECK(auto_device->GetRenderTargetData(auto_back_buffer.Get(), auto_readback.Get()));
    D3DLOCKED_RECT auto_locked{};
    CHECK(auto_readback->LockRect(&auto_locked, nullptr, D3DLOCK_READONLY));
    const DWORD *auto_row = (const DWORD *)((const char *)auto_locked.pBits + 40 * auto_locked.Pitch);
    const D3DCOLOR auto_pixel = auto_row[40] & 0x00FFFFFF;
    auto_readback->UnlockRect();
    expect(auto_pixel == 0x00FF00, "the automatic depth-stencil surface did not keep the near triangle");
    printf("target: a device with EnableAutoDepthStencil reads %06lx at 40,40\n", (unsigned long)auto_pixel);
    auto_device.Reset();
    DestroyWindow(auto_window);
  }

  device.Reset();
  d3d9.Reset();
  DestroyWindow(window);

  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
