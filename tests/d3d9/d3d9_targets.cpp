// contract: the surfaces a frame is drawn into and tested against, and the viewport that places it, read back out of
// the pixels rather than out of a return code.
// - CreateDepthStencilSurface makes a surface a device can be given with SetDepthStencilSurface, GetDepthStencilSurface
//   hands it back, and depth testing is what it does (a nearer triangle wins over a farther one on a custom target too).
// - CreateRenderTarget makes a surface SetRenderTarget takes: Clear and the draws go to it, and the back buffer keeps
//   what it had; SetRenderTarget back to the back buffer puts the draws there and leaves the custom target alone.
// - Clear clears the rectangles it is given and nothing else (Microsoft Learn, IDirect3DDevice9::Clear), a rectangle of
//   no pixels clears none, no rectangles at all (a NULL rect list) clears the whole target, a count of 0 with rectangles
//   named clears none of them, and a clear is clipped to the viewport and the scissor rectangle; a clear of part of a
//   depth-stencil surface leaves depth as it was rather than clearing the whole of it.
// - ValidateDevice answers a pass count for drawable state, and GetAvailableTextureMem answers memory a title can size
//   its textures against.
// - SetViewport places and clips the draw: a triangle larger than the viewport is drawn inside it and not outside, and
//   the vertex map follows the viewport's origin, so a pixel keeps the place it names.
// - what cannot be a target (a depth surface, a target index but 0, no surface at all) and a depth surface smaller
//   than the target (a larger one is allowed and clipped by the viewport) are D3DERR_INVALIDCALL, and the device
//   keeps the target it had. SetViewport validates nothing but the pointer (Wine device.c): an empty one, one past
//   the target and an inverted Z range are all kept whole, and the raster clips the draw at the target.
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

  // Wine's stateblock test creates a device the way a game does when it lets D3D9 choose the windowed defaults:
  // a hidden plain window, D3DFMT_UNKNOWN, zero width and height, and software vertex processing. The device must
  // resolve those to the client size and the desktop format rather than refuse them before the chain exists.
  {
    HWND defaults_window = CreateWindowA("static", "d3d9_targets defaults", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480,
                                        nullptr, nullptr, nullptr, nullptr);
    expect(defaults_window != nullptr, "no window for the default windowed device");
    if (defaults_window) {
      D3DPRESENT_PARAMETERS defaults{};
      defaults.Windowed = TRUE;
      defaults.hDeviceWindow = defaults_window;
      defaults.SwapEffect = D3DSWAPEFFECT_DISCARD;
      ComPtr<IDirect3DDevice9> defaults_device;
      const HRESULT made = d3d9->CreateDevice(0, D3DDEVTYPE_HAL, defaults_window,
                                             D3DCREATE_SOFTWARE_VERTEXPROCESSING, &defaults, &defaults_device);
      printf("probe: default windowed CreateDevice 0x%08lx\n", (unsigned long)made);
      expect(SUCCEEDED(made), "a windowed device with zero dimensions and D3DFMT_UNKNOWN was refused");
      if (SUCCEEDED(made)) {
        D3DPRESENT_PARAMETERS resolved{};
        ComPtr<IDirect3DSwapChain9> defaults_chain;
        CHECK(defaults_device->GetSwapChain(0, &defaults_chain));
        CHECK(defaults_chain->GetPresentParameters(&resolved));
        expect(resolved.BackBufferWidth > 0 && resolved.BackBufferHeight > 0,
               "the default windowed device did not resolve its dimensions");
        expect(resolved.BackBufferFormat != D3DFMT_UNKNOWN,
               "the default windowed device did not resolve its format");
        printf("probe: default windowed device resolved %ux%u format %lu\n", resolved.BackBufferWidth,
               resolved.BackBufferHeight, (unsigned long)resolved.BackBufferFormat);
      }
      defaults_device.Reset();
      DestroyWindow(defaults_window);
    }
  }

  // what a title reads before it draws: the passes its state takes, and the memory textures may still take
  DWORD passes = 0;
  CHECK(device->ValidateDevice(&passes));
  expect(passes == 1, "ValidateDevice did not answer a pass count for drawable state");
  expect(device->ValidateDevice(nullptr) == D3DERR_INVALIDCALL, "ValidateDevice took no place to write");
  expect(device->GetAvailableTextureMem() > 0, "GetAvailableTextureMem answered no memory at all");
  printf("device: ValidateDevice answers %lu pass, GetAvailableTextureMem answers %lu bytes\n", (unsigned long)passes,
         (unsigned long)device->GetAvailableTextureMem());

  // the default target, a depth surface for it, and the surfaces frames are read back into
  ComPtr<IDirect3DSurface9> back_buffer;
  CHECK(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer));
  ComPtr<IDirect3DSurface9> depth;
  CHECK(device->CreateDepthStencilSurface(client, client, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &depth,
                                          nullptr));
  CHECK(device->SetDepthStencilSurface(depth.Get()));
  // the chain was made without an automatic depth-stencil surface, so depth testing starts off (D3DRS_ZENABLE): the
  // depth checks below ask for it, as a title that makes its own depth surface does
  CHECK(device->SetRenderState(D3DRS_ZENABLE, TRUE));
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

  // Clear clears the rectangles it is given and leaves the rest of the target as it was (Microsoft Learn,
  // IDirect3DDevice9::Clear: "the rectangles to be cleared"); a rectangle of no pixels clears none, and a count with no
  // rectangles at all is refused
  const D3DRECT corner[1] = {{0, 0, 32, 32}};
  CHECK(device->Clear(1, corner, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 255, 0), 1.0f, 0));
  expect(pixel_of(render_target.Get(), readback96.Get(), 16, 16) == 0x00FF00,
         "Clear did not clear the rectangle it was given");
  expect(pixel_of(render_target.Get(), readback96.Get(), 64, 64) == 0x0000FF,
         "Clear of a rectangle cleared a pixel outside it");
  const D3DRECT empty[1] = {{40, 40, 40, 60}};
  CHECK(device->Clear(1, empty, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255, 0, 0), 1.0f, 0));
  expect(pixel_of(render_target.Get(), readback96.Get(), 40, 40) == 0x0000FF,
         "a rectangle that names no pixel cleared one");
  // a NULL rectangle list is the whole target whatever the count says (Wine visual.c clear_test: Clear(1, NULL) leaves
  // the whole target clear), while a count of 0 with a rectangle list clears none of it
  CHECK(device->Clear(1, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255, 0, 0), 1.0f, 0));
  expect(pixel_of(render_target.Get(), readback96.Get(), 40, 40) == 0xFF0000,
         "a clear with no rectangle list did not clear the target");
  CHECK(device->Clear(0, empty, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 255, 0), 1.0f, 0));
  expect(pixel_of(render_target.Get(), readback96.Get(), 40, 40) == 0xFF0000,
         "a clear of no rectangles cleared one");
  // the viewport clips a clear as it clips a draw (Wine visual.c clear_test expects the pixels outside the viewport to
  // keep the colour they had)
  D3DVIEWPORT9 clear_viewport{0, 0, 32, 32, 0.0f, 1.0f};
  CHECK(device->SetViewport(&clear_viewport));
  CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 255), 1.0f, 0));
  expect(pixel_of(render_target.Get(), readback96.Get(), 16, 16) == 0x0000FF,
         "a clear inside the viewport did not clear there");
  expect(pixel_of(render_target.Get(), readback96.Get(), 48, 48) == 0xFF0000,
         "a clear outside the viewport cleared a pixel there");
  // the scissor rectangle clips it too, for a whole-target clear and for a rectangle of one
  clear_viewport = D3DVIEWPORT9{0, 0, 96, 96, 0.0f, 1.0f};
  CHECK(device->SetViewport(&clear_viewport));
  const RECT clear_scissor{32, 32, 64, 64};
  CHECK(device->SetScissorRect(&clear_scissor));
  CHECK(device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE));
  CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 255, 0), 1.0f, 0));
  expect(pixel_of(render_target.Get(), readback96.Get(), 48, 48) == 0x00FF00,
         "a clear inside the scissor rectangle did not clear there");
  expect(pixel_of(render_target.Get(), readback96.Get(), 16, 16) == 0x0000FF,
         "a clear outside the scissor rectangle cleared a pixel there");
  CHECK(device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE));
  CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 255), 1.0f, 0));
  printf("target: a 32 by 32 rect clear reads %06lx inside it and %06lx outside",
         (unsigned long)pixel_of(render_target.Get(), readback96.Get(), 16, 16),
         (unsigned long)pixel_of(render_target.Get(), readback96.Get(), 64, 64));
  printf("; a viewport-clipped clear leaves %06lx outside\n",
         (unsigned long)pixel_of(render_target.Get(), readback96.Get(), 48, 48));

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

  // the viewport: it clips the draw, and the vertex map follows its origin. The clear that comes before it covers the
  // whole target, since a clear is clipped by the viewport too.
  CHECK(device->SetDepthStencilSurface(depth.Get()));
  D3DVIEWPORT9 whole_viewport{0, 0, client, client, 0.0f, 1.0f};
  CHECK(device->SetViewport(&whole_viewport));
  CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, kClear, 1.0f, 0));
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

  // the depth-stencil binding survives a target of another size: D3D9 keeps the surface bound and clips it, so a
  // target of the matching size again has depth, and a Clear that names depth is valid (Wine visual.c
  // test_mipmap_autogen sets a 1024x1024 target, then the back buffer, and clears depth)
  CHECK(device->SetRenderTarget(0, render_target.Get()));
  CHECK(device->SetRenderTarget(0, back_buffer.Get()));
  expect(SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, kClear, 1.0f, 0)),
         "the depth-stencil binding was lost when a render target of another size was set");

  // what cannot be a target, and what does not match the one there is
  expect(device->SetRenderTarget(0, nullptr) == D3DERR_INVALIDCALL, "SetRenderTarget took no surface");
  expect(device->SetRenderTarget(1, render_target.Get()) == D3DERR_INVALIDCALL, "SetRenderTarget took a second target");
  expect(device->SetRenderTarget(0, depth.Get()) == D3DERR_INVALIDCALL, "SetRenderTarget took a depth surface");
  expect(device->SetDepthStencilSurface(render_target.Get()) == D3DERR_INVALIDCALL,
         "SetDepthStencilSurface took a colour target");
  // a slot the device has not set has no target: D3DERR_NOTFOUND, not the invalid-argument error of an index past
  // NumSimultaneousRTs
  expect(device->GetRenderTarget(1, &current_target) == D3DERR_NOTFOUND, "GetRenderTarget returned a second target");

  ComPtr<IDirect3DSurface9> small_depth;
  CHECK(device->CreateDepthStencilSurface(96, 96, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &small_depth, nullptr));
  // Wine device.c allows a depth surface smaller or larger than the target (the viewport clips the intersection)
  expect(SUCCEEDED(device->SetDepthStencilSurface(small_depth.Get())),
         "a depth surface of another size than the target was refused");
  expect(device->SetRenderTarget(0, render_target.Get()) == S_OK,
         "a target smaller than the depth surface was refused");
  ComPtr<IDirect3DSurface9> tiny_depth;
  CHECK(device->CreateDepthStencilSurface(64, 64, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &tiny_depth, nullptr));
  expect(SUCCEEDED(device->SetDepthStencilSurface(tiny_depth.Get())),
         "a depth surface smaller than the target was refused");
  expect(device->SetDepthStencilSurface(nullptr) == S_OK, "SetDepthStencilSurface did not take no surface");
  ComPtr<IDirect3DSurface9> no_depth;
  expect(device->GetDepthStencilSurface(&no_depth) == D3DERR_NOTFOUND && no_depth.Get() == nullptr,
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

  // a clear of part of a depth-stencil surface clears the depth of the rectangle it is given and leaves the rest of it
  // as it was (Microsoft Learn, IDirect3DDevice9::Clear: "the rectangles to be cleared"): the near triangle's depth of
  // 0.25 is still there to the left of the rectangle, so the far triangle loses there, and the right of it is depth 1,
  // so the far triangle draws
  const D3DRECT right_half[1] = {{48, 0, 96, 96}};
  expect(device->Clear(1, right_half, D3DCLEAR_ZBUFFER, kClear, 1.0f, 0) == S_OK,
         "a clear of part of the depth surface was refused");
  PreTransformedVertex far_again[3];
  triangle_at(20.0f, 20.0f, 60.0f, 0.75f, D3DCOLOR_XRGB(255, 0, 0), far_again);
  draw(far_again);
  const D3DCOLOR kept = pixel_of(render_target.Get(), readback96.Get(), 30, 30);
  const D3DCOLOR cleared = pixel_of(render_target.Get(), readback96.Get(), 60, 30);
  expect(kept == 0x00FF00, "a clear of part of the depth surface changed the depth outside the rectangle it was given");
  expect(cleared == 0xFF0000, "a clear of part of the depth surface did not clear the depth inside the rectangle");
  printf("target: after a clear of the right half's depth, 30,30 reads %06lx and 60,30 reads %06lx\n",
         (unsigned long)kept, (unsigned long)cleared);

  // Wine device.c SetViewport validates nothing but the pointer: an empty one, one past the target and an inverted
  // Z range are all kept, and the raster clips the draw at the target (Wine device.c test_set_viewport sets one as
  // wide as the maximum texture over a much smaller one)
  expect(device->SetViewport(nullptr) == D3DERR_INVALIDCALL, "SetViewport took no viewport");
  D3DVIEWPORT9 small_viewport{0, 0, 96, 96, 0.0f, 1.0f};
  CHECK(device->SetViewport(&small_viewport));
  D3DVIEWPORT9 bad{0, 0, 0, 64, 0.0f, 1.0f};
  expect(SUCCEEDED(device->SetViewport(&bad)), "a viewport of no width was refused");
  expect(device->GetViewport(&read_back) == S_OK && read_back.Width == 0,
         "GetViewport did not keep the empty viewport SetViewport was given");
  bad = D3DVIEWPORT9{64, 64, 64, 64, 0.0f, 1.0f};
  expect(SUCCEEDED(device->SetViewport(&bad)), "a viewport outside the target was refused");
  expect(device->GetViewport(&read_back) == S_OK && read_back.X == 64,
         "GetViewport did not keep the viewport past the target");
  bad = D3DVIEWPORT9{0, 0, 64, 64, 0.5f, 0.2f};
  expect(SUCCEEDED(device->SetViewport(&bad)),
         "a viewport with its near plane behind its far one was refused");
  expect(device->GetViewport(nullptr) == D3DERR_INVALIDCALL, "GetViewport took no place to write");
  CHECK(device->SetViewport(&small_viewport));

  // the viewport's depth range: a zero or reversed range is widened by wined3d's compatibility rule max_z =
  // max(max_z, min_z + 0.001) before it is clamped to 0..1 (Wine visual.c test_viewport passes MinZ == MaxZ and then
  // MaxZ < MinZ), so a pretransformed z of 1 lands just past the near plane rather than exactly on it. Each clear is
  // 0.0005 and the z = 1 draw passes it only with GREATER while the widened range holds.
  {
    D3DVIEWPORT9 depth_viewport{0, 0, 96, 96, 0.0f, 0.0f};
    CHECK(device->SetViewport(&depth_viewport));
    CHECK(device->SetRenderState(D3DRS_ZENABLE, TRUE));
    CHECK(device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE));
    CHECK(device->SetRenderState(D3DRS_ZFUNC, D3DCMP_GREATER));
    PreTransformedVertex deep[3], shallow[3];
    triangle_at(20.0f, 20.0f, 60.0f, 1.0f, D3DCOLOR_XRGB(255, 0, 0), deep);
    triangle_at(20.0f, 20.0f, 60.0f, 0.0f, D3DCOLOR_XRGB(0, 255, 0), shallow);
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, kClear, 0.0005f, 0));
    draw(deep);
    expect(pixel_of(render_target.Get(), readback96.Get(), 40, 40) == 0xFF0000,
           "a zero-depth viewport did not widen its range to 0.001");
    // a z of 0 is still nearer than the clear: with GREATER it loses to the 0.001 the rule gives the far plane
    draw(shallow);
    expect(pixel_of(render_target.Get(), readback96.Get(), 40, 40) == 0xFF0000,
           "a zero-depth viewport mapped a near vertex onto the clear's depth");

    depth_viewport = D3DVIEWPORT9{0, 0, 96, 96, 0.5f, 0.2f};
    CHECK(device->SetViewport(&depth_viewport));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, kClear, 0.5005f, 0));
    draw(deep);
    expect(pixel_of(render_target.Get(), readback96.Get(), 40, 40) == 0xFF0000,
           "a viewport whose far plane is before its near one did not widen its range to 0.501");

    // an ordinary range maps z = 1 to its far plane, 0.75, which is past a 0.5 clear
    depth_viewport = D3DVIEWPORT9{0, 0, 96, 96, 0.25f, 0.75f};
    CHECK(device->SetViewport(&depth_viewport));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, kClear, 0.5f, 0));
    draw(deep);
    expect(pixel_of(render_target.Get(), readback96.Get(), 40, 40) == 0xFF0000,
           "an ordinary depth range did not map z = 1 to its far plane");

    // a pretransformed draw clamps z outside 0..1 instead of clipping it, with no depth target and with one
    depth_viewport = D3DVIEWPORT9{0, 0, 96, 96, 0.0f, 1.0f};
    CHECK(device->SetViewport(&depth_viewport));
    CHECK(device->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS));
    CHECK(device->SetDepthStencilSurface(nullptr));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
    PreTransformedVertex beyond[3];
    triangle_at(20.0f, 20.0f, 60.0f, 2.0f, D3DCOLOR_XRGB(255, 0, 0), beyond);
    draw(beyond);
    expect(pixel_of(render_target.Get(), readback96.Get(), 40, 40) == 0xFF0000,
           "a pretransformed draw without a depth target clipped a z past the far plane");
    CHECK(device->SetDepthStencilSurface(small_depth.Get()));
    CHECK(device->SetRenderState(D3DRS_ZENABLE, TRUE));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, kClear, 1.0f, 0));
    triangle_at(20.0f, 20.0f, 60.0f, -1.0f, D3DCOLOR_XRGB(0, 255, 0), shallow);
    draw(shallow);
    expect(pixel_of(render_target.Get(), readback96.Get(), 40, 40) == 0x00FF00,
           "a pretransformed draw with a depth target clipped a z before the near plane");
    CHECK(device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE));
    CHECK(device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL));
  }

  // a clear converts its colour to sRGB while D3DRS_SRGBWRITEENABLE is on (Wine visual.c clear_test: 0x7f clears to
  // 0xbb)
  {
    CHECK(device->SetRenderState(D3DRS_SRGBWRITEENABLE, TRUE));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0x7F, 0x7F, 0x7F), 1.0f, 0));
    expect(pixel_of(render_target.Get(), readback96.Get(), 40, 40) == 0xBBBBBB,
           "a clear with sRGB write on did not convert its colour to sRGB");
    CHECK(device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE));
  }

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

  // a rectangle's stencil: Clear of part of a D24S8 surface writes those pixels' stencil (Microsoft Learn,
  // IDirect3DDevice9::Clear, D3DCLEAR_STENCIL) and leaves the rest; a later stencil-equal draw covers only the rectangle
  {
    CHECK(device->SetDepthStencilSurface(nullptr));
    CHECK(device->SetRenderTarget(0, back_buffer.Get()));
    CHECK(device->SetDepthStencilSurface(depth.Get()));
    D3DVIEWPORT9 full{0, 0, client, client, 0.0f, 1.0f};
    CHECK(device->SetViewport(&full));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, kClear, 1.0f, 0));
    const D3DRECT stencil_rect{20, 20, 80, 80};
    CHECK(device->Clear(1, &stencil_rect, D3DCLEAR_STENCIL, 0, 1.0f, 1));
    CHECK(device->SetRenderState(D3DRS_ZENABLE, FALSE));
    CHECK(device->SetRenderState(D3DRS_STENCILENABLE, TRUE));
    CHECK(device->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_EQUAL));
    CHECK(device->SetRenderState(D3DRS_STENCILREF, 1));
    CHECK(device->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP));
    PreTransformedVertex cover[3] = {
        {0.0f, 0.0f, 0.0f, 1.0f, D3DCOLOR_XRGB(255, 255, 255)},
        {0.0f, (float)client, 0.0f, 1.0f, D3DCOLOR_XRGB(255, 255, 255)},
        {(float)client, 0.0f, 0.0f, 1.0f, D3DCOLOR_XRGB(255, 255, 255)},
    };
    CHECK(device->BeginScene());
    CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, cover, sizeof(PreTransformedVertex)));
    CHECK(device->EndScene());
    ComPtr<IDirect3DSurface9> stencil_readback;
    CHECK(device->CreateOffscreenPlainSurface(client, client, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &stencil_readback,
                                              nullptr));
    CHECK(device->GetRenderTargetData(back_buffer.Get(), stencil_readback.Get()));
    D3DLOCKED_RECT stencil_locked{};
    CHECK(stencil_readback->LockRect(&stencil_locked, nullptr, D3DLOCK_READONLY));
    auto colour_at = [&](UINT x, UINT y) -> D3DCOLOR {
      const DWORD *row = (const DWORD *)((const char *)stencil_locked.pBits + y * stencil_locked.Pitch);
      return row[x] & 0x00FFFFFF;
    };
    expect(colour_at(40, 40) == 0xFFFFFF, "the stencil rectangle did not receive the stencil-equal draw");
    expect(colour_at(10, 10) == (kClear & 0x00FFFFFF), "a pixel outside the stencil rectangle was drawn");
    stencil_readback->UnlockRect();
    CHECK(device->SetRenderState(D3DRS_STENCILENABLE, FALSE));
    CHECK(device->SetRenderState(D3DRS_ZENABLE, TRUE));
  }

  // a depth-stencil surface of another sample count is still bound, in either order: D3D9 accepts a 4x target over a
  // single-sample depth surface and a single-sample target over a 4x depth surface (Wine visual.c
  // test_multisample_mismatch sets a 2x target over the automatic 1x depth), while a draw of such a pair binds no
  // depth-stencil (bound_depth_stencil)
  {
    CHECK(device->SetDepthStencilSurface(nullptr));
    CHECK(device->SetRenderTarget(0, render_target.Get()));
    CHECK(device->SetDepthStencilSurface(small_depth.Get()));
    ComPtr<IDirect3DSurface9> msaa_target;
    HRESULT made_target = device->CreateRenderTarget(96, 96, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_4_SAMPLES, 0, FALSE,
                                                     &msaa_target, nullptr);
    expect(SUCCEEDED(made_target), "a 4x render target was not made");
    if (SUCCEEDED(made_target)) {
      CHECK(device->SetDepthStencilSurface(nullptr));
      CHECK(device->SetRenderTarget(0, msaa_target.Get()));
      expect(device->SetDepthStencilSurface(small_depth.Get()) == S_OK,
             "a single-sample depth surface was refused for a multisampled target");
      ComPtr<IDirect3DSurface9> bound_depth;
      expect(device->GetDepthStencilSurface(&bound_depth) == S_OK && bound_depth.Get() == small_depth.Get(),
             "the single-sample depth surface was not left set");

      ComPtr<IDirect3DSurface9> msaa_depth;
      CHECK(device->CreateDepthStencilSurface(96, 96, D3DFMT_D24S8, D3DMULTISAMPLE_4_SAMPLES, 0, TRUE, &msaa_depth,
                                              nullptr));
      CHECK(device->SetDepthStencilSurface(msaa_depth.Get()));
      expect(device->SetRenderTarget(0, render_target.Get()) == S_OK,
             "a single-sample target was refused while a multisampled depth surface was set");
      ComPtr<IDirect3DSurface9> kept_target;
      CHECK(device->GetRenderTarget(0, &kept_target));
      expect(kept_target.Get() == render_target.Get(), "the target the device had was not replaced");
      CHECK(device->SetDepthStencilSurface(nullptr));
      CHECK(device->SetRenderTarget(0, render_target.Get()));
      CHECK(device->SetDepthStencilSurface(small_depth.Get()));
      CHECK(device->SetDepthStencilSurface(nullptr));
    }
  }

  device.Reset();
  d3d9.Reset();
  DestroyWindow(window);

  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
