// contract: the render states this milestone honours, each read back out of the back buffer's pixels rather than out
// of a return code.
// - the documented defaults: D3DCULL_CCW, SRCBLEND ONE and DESTBLEND ZERO, and depth testing off on a device made
//   without EnableAutoDepthStencil (D3DRENDERSTATETYPE).
// - depth: with a depth surface set and D3DRS_ZENABLE on, the nearer of two triangles is at the pixel whichever order
//   they are drawn in; with ZENABLE off the one drawn later is; ZWRITEENABLE off leaves the depth
//   test where it was; ZFUNC replaces the comparison; a colour Clear leaves depth alone and D3DCLEAR_ZBUFFER resets it.
// - cull: the default culls a screen-counter-clockwise triangle (D3D9's front faces are the clockwise ones);
//   D3DCULL_NONE draws both windings; D3DCULL_CW culls the clockwise one.
// - fill: D3DFILL_WIREFRAME draws the edges and not the interior; D3DFILL_SOLID puts the interior back.
// - blend: BLENDENABLE with SRCALPHA and INVSRCALPHA mixes a vertex's alpha with what is there, BLENDOP ADD and
//   ONE/ONE add, and with blending off the colour is written as it is.
// - COLORWRITEENABLE keeps a draw out of the channels its mask does not name.
// - SHADEMODE: GOURAUD interpolates the three vertex colours, FLAT gives the triangle the first vertex's.
// - fog is stored (GetRenderState matches SetRenderState) without being applied; RasterCaps names none until a pixel
//   test reads it (tests/d3d9/d3d9_extra.cpp). a state this milestone cannot honour (D3DRS_DITHERENABLE), a state it
//   does not know, and a value it cannot use are D3DERR_INVALIDCALL, and the value the device had stays.
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

struct PreTransformedVertex {
  float x, y, z, rhw;
  D3DCOLOR colour;
};

static const DWORD kPreTransformedFVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;
static const D3DCOLOR kClear = D3DCOLOR_XRGB(10, 20, 30);

// a right triangle with its corner at `x, y`, `size` pixels along each axis, counter-clockwise on the screen (which is
// the winding the default cull mode draws), every vertex at `z` and in `colour`
static void
triangle_at(float x, float y, float size, float z, D3DCOLOR colour, PreTransformedVertex vertices[3]) {
  vertices[0] = {x, y, z, 1.0f, colour};
  vertices[1] = {x, y + size, z, 1.0f, colour};
  vertices[2] = {x + size, y, z, 1.0f, colour};
}

static bool
close_to(D3DCOLOR got, D3DCOLOR want, int tolerance) {
  for (int shift = 0; shift <= 16; shift += 8) {
    const int a = (int)((got >> shift) & 0xFF), b = (int)((want >> shift) & 0xFF);
    if (std::abs(a - b) > tolerance)
      return false;
  }
  return true;
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
                         "d3d9_states"};
  RegisterClassA(&window_class);
  RECT window_rect{0, 0, (LONG)client, (LONG)client};
  AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);
  HWND window = CreateWindowA("d3d9_states", "d3d9_states", WS_OVERLAPPEDWINDOW, 0, 0,
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

  // depth testing needs a depth-stencil surface; without one the depth checks report what they could not do and the
  // states that do not need it are still checked, so a red run says what every state did rather than stopping here
  ComPtr<IDirect3DSurface9> depth;
  const bool depth_ready =
      SUCCEEDED(device->CreateDepthStencilSurface(client, client, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &depth,
                                                  nullptr)) &&
      SUCCEEDED(device->SetDepthStencilSurface(depth.Get()));
  expect(depth_ready, "no depth-stencil surface for the depth states");

  ComPtr<IDirect3DSurface9> readback;
  CHECK(device->CreateOffscreenPlainSurface(client, client, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr));

  // the pixel at `x, y` of the back buffer
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
  // the frame a check is about: the back buffer cleared to `colour`, with depth when `depth` says so
  auto clear = [&](D3DCOLOR colour, bool with_depth) -> HRESULT {
    return device->Clear(0, nullptr, D3DCLEAR_TARGET | (with_depth ? D3DCLEAR_ZBUFFER : 0), colour, 1.0f, 0);
  };
  auto draw = [&](const PreTransformedVertex vertices[3]) {
    expect(SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, vertices, sizeof(PreTransformedVertex))),
           "the draw path refused a triangle");
  };
  CHECK(device->SetFVF(kPreTransformedFVF));

  // the states a device starts with, before any of them is set (D3DRENDERSTATETYPE's documented defaults)
  DWORD default_cull = 0, default_src = 0, default_dest = 0, default_z = 0;
  const bool read_cull = device->GetRenderState(D3DRS_CULLMODE, &default_cull) == S_OK;
  const bool read_src = device->GetRenderState(D3DRS_SRCBLEND, &default_src) == S_OK;
  const bool read_dest = device->GetRenderState(D3DRS_DESTBLEND, &default_dest) == S_OK;
  const bool read_z = device->GetRenderState(D3DRS_ZENABLE, &default_z) == S_OK;
  expect(read_cull && default_cull == D3DCULL_CCW, "D3DRS_CULLMODE did not start at D3DCULL_CCW");
  expect(read_src && default_src == D3DBLEND_ONE, "D3DRS_SRCBLEND did not start at D3DBLEND_ONE");
  expect(read_dest && default_dest == D3DBLEND_ZERO, "D3DRS_DESTBLEND did not start at D3DBLEND_ZERO");
  expect(read_z && default_z == FALSE, "D3DRS_ZENABLE did not start off on a device made without EnableAutoDepthStencil");
  printf("defaults: cull %s (%lu), src blend %lu, dest blend %lu, z enable %lu\n", read_cull ? "read" : "unread",
         (unsigned long)default_cull, (unsigned long)default_src, (unsigned long)default_dest, (unsigned long)default_z);
  // the triangles below are wound counter-clockwise on the screen, which the default cull mode culls, so the states
  // that are not about culling turn it off (the cull block sets it back to the default and then to each mode)
  expect(SUCCEEDED(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE)), "D3DRS_CULLMODE was refused");

  if (depth_ready) {
    // a device made without EnableAutoDepthStencil starts with depth testing off, so the depth states ask for it
    expect(SUCCEEDED(device->SetRenderState(D3DRS_ZENABLE, TRUE)), "D3DRS_ZENABLE was refused");
    // two triangles of the same shape at the same place, one behind the other
    PreTransformedVertex far_triangle[3], near_triangle[3];
    triangle_at(20.0f, 20.0f, 100.0f, 0.75f, D3DCOLOR_XRGB(255, 0, 0), far_triangle);
    triangle_at(20.0f, 20.0f, 100.0f, 0.25f, D3DCOLOR_XRGB(0, 255, 0), near_triangle);

    // depth: the near triangle is the one at the pixel, in either drawing order
    CHECK(clear(kClear, true));
    draw(far_triangle);
    draw(near_triangle);
    expect(pixel(40, 40) == 0x00FF00, "depth did not keep the near triangle drawn after the far one");
    const D3DCOLOR far_then_near = pixel(40, 40);
    CHECK(clear(kClear, true));
    draw(near_triangle);
    draw(far_triangle);
    expect(pixel(40, 40) == 0x00FF00, "depth did not keep the near triangle drawn before the far one");
    printf("depth: the near triangle reads %06lx over the far one in both orders\n", (unsigned long)far_then_near);

    // depth off: the later draw is the pixel's, wherever it is
    expect(SUCCEEDED(device->SetRenderState(D3DRS_ZENABLE, FALSE)), "D3DRS_ZENABLE was refused");
    CHECK(clear(kClear, true));
    draw(near_triangle);
    draw(far_triangle);
    expect(pixel(40, 40) == 0xFF0000, "a draw past the near one did not win with depth off");
    expect(SUCCEEDED(device->SetRenderState(D3DRS_ZENABLE, TRUE)), "D3DRS_ZENABLE was refused");

    // depth writes off: the near triangle leaves the depth where it was, so the far one still passes
    expect(SUCCEEDED(device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE)), "D3DRS_ZWRITEENABLE was refused");
    CHECK(clear(kClear, true));
    draw(near_triangle);
    expect(SUCCEEDED(device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE)), "D3DRS_ZWRITEENABLE was refused");
    draw(far_triangle);
    expect(pixel(40, 40) == 0xFF0000, "a triangle that did not write depth kept the far one out");
    expect(SUCCEEDED(device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE)), "D3DRS_ZWRITEENABLE was refused");

    // ZFUNC is the comparison: the near triangle fails a GREATER test against the far one's depth
    CHECK(clear(kClear, true));
    draw(far_triangle);
    expect(SUCCEEDED(device->SetRenderState(D3DRS_ZFUNC, D3DCMP_GREATER)), "D3DRS_ZFUNC was refused");
    draw(near_triangle);
    expect(pixel(40, 40) == 0xFF0000, "ZFUNC GREATER drew a nearer triangle");
    expect(SUCCEEDED(device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL)), "D3DRS_ZFUNC was refused");

    // a colour clear leaves depth alone; D3DCLEAR_ZBUFFER is what puts it back
    CHECK(clear(kClear, true));
    PreTransformedVertex stored[3], deeper[3];
    triangle_at(20.0f, 20.0f, 100.0f, 0.5f, D3DCOLOR_XRGB(0, 255, 0), stored);
    triangle_at(20.0f, 20.0f, 100.0f, 0.8f, D3DCOLOR_XRGB(255, 0, 0), deeper);
    draw(stored);
    CHECK(clear(kClear, false));
    draw(deeper);
    expect(pixel(40, 40) == (kClear & 0x00FFFFFF), "a colour clear forgot the depth it had");
    CHECK(clear(kClear, true));
    draw(deeper);
    expect(pixel(40, 40) == 0xFF0000, "a depth clear did not let the far triangle through");
    printf("depth: a colour clear keeps it (%06lx) and a depth clear resets it (%06lx)\n",
           (unsigned long)(kClear & 0x00FFFFFF), (unsigned long)pixel(40, 40));
  }

  // cull: D3D9's default is D3DCULL_CCW, which culls the counter-clockwise triangle and draws the clockwise one; NONE
  // draws both; CW culls the clockwise one. a winding is named the way it looks on the screen, which is where D3D9
  // measures it, so the front faces a title draws with no cull mode set are the clockwise ones.
  expect(SUCCEEDED(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW)), "D3DRS_CULLMODE was refused");
  PreTransformedVertex clockwise[3] = {{20.0f, 20.0f, 0.0f, 1.0f, D3DCOLOR_XRGB(0, 255, 255)},
                                       {120.0f, 20.0f, 0.0f, 1.0f, D3DCOLOR_XRGB(0, 255, 255)},
                                       {20.0f, 120.0f, 0.0f, 1.0f, D3DCOLOR_XRGB(0, 255, 255)}};
  PreTransformedVertex counter_clockwise[3] = {{20.0f, 20.0f, 0.0f, 1.0f, D3DCOLOR_XRGB(255, 255, 0)},
                                               {20.0f, 120.0f, 0.0f, 1.0f, D3DCOLOR_XRGB(255, 255, 0)},
                                               {120.0f, 20.0f, 0.0f, 1.0f, D3DCOLOR_XRGB(255, 255, 0)}};
  CHECK(clear(kClear, false));
  draw(counter_clockwise);
  expect(pixel(40, 40) == (kClear & 0x00FFFFFF), "CULLMODE CCW drew a counter-clockwise triangle");
  CHECK(clear(kClear, false));
  draw(clockwise);
  expect(pixel(40, 40) == 0x00FFFF, "CULLMODE CCW culled a clockwise triangle");
  expect(SUCCEEDED(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE)), "D3DRS_CULLMODE was refused");
  CHECK(clear(kClear, false));
  draw(counter_clockwise);
  expect(pixel(40, 40) == 0xFFFF00, "CULLMODE NONE culled a counter-clockwise triangle");
  CHECK(clear(kClear, false));
  draw(clockwise);
  expect(pixel(40, 40) == 0x00FFFF, "CULLMODE NONE culled a clockwise triangle");
  expect(SUCCEEDED(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW)), "D3DRS_CULLMODE was refused");
  CHECK(clear(kClear, false));
  draw(clockwise);
  expect(pixel(40, 40) == (kClear & 0x00FFFFFF), "CULLMODE CW drew a clockwise triangle");
  CHECK(clear(kClear, false));
  draw(counter_clockwise);
  expect(pixel(40, 40) == 0xFFFF00, "CULLMODE CW culled a counter-clockwise triangle");
  printf("cull: the default (CCW) culls the counter-clockwise winding, NONE draws both, CW culls the clockwise one\n");

  // fill: wireframe is the edges, solid is the interior
  CHECK(clear(kClear, false));
  expect(SUCCEEDED(device->SetRenderState(D3DRS_FILLMODE, D3DFILL_WIREFRAME)), "D3DRS_FILLMODE was refused");
  draw(counter_clockwise);
  expect(pixel(35, 35) == (kClear & 0x00FFFFFF), "wireframe filled the triangle's interior");
  bool edge = false;
  for (UINT x = 19; x <= 21; x++)
    edge |= pixel(x, 60) == 0xFFFF00;
  expect(edge, "wireframe did not draw the triangle's edge");
  expect(SUCCEEDED(device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID)), "D3DRS_FILLMODE was refused");
  CHECK(clear(kClear, false));
  draw(counter_clockwise);
  expect(pixel(35, 35) == 0xFFFF00, "solid fill did not fill the triangle's interior");
  printf("fill: wireframe leaves 35,35 at %06lx and hit its edge, solid fills it\n",
         (unsigned long)(kClear & 0x00FFFFFF));

  // blend: half a red over blue is a mix of the two; off, the red is written whole
  PreTransformedVertex half_red[3];
  triangle_at(20.0f, 20.0f, 100.0f, 0.0f, D3DCOLOR_ARGB(128, 255, 0, 0), half_red);
  const D3DCOLOR blue = D3DCOLOR_XRGB(0, 0, 255);
  CHECK(clear(blue, false));
  expect(SUCCEEDED(device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE)), "D3DRS_ALPHABLENDENABLE was refused");
  // the documented default factors are ONE and ZERO, so blending turned on with neither set writes the draw over the
  // target: the vertex alpha does not mix it with what is there
  draw(half_red);
  expect(pixel(40, 40) == 0xFF0000, "the default ONE/ZERO factors did not write the draw over the target");
  CHECK(clear(blue, false));
  expect(SUCCEEDED(device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA)), "D3DRS_SRCBLEND was refused");
  expect(SUCCEEDED(device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA)), "D3DRS_DESTBLEND was refused");
  draw(half_red);
  const D3DCOLOR blended = pixel(40, 40);
  expect(close_to(blended, 0x80007F, 3), "SRCALPHA/INVSRCALPHA did not mix the vertex alpha with the target");
  expect(SUCCEEDED(device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE)), "D3DRS_ALPHABLENDENABLE was refused");
  CHECK(clear(blue, false));
  draw(half_red);
  expect(pixel(40, 40) == 0xFF0000, "a draw with blending off did not write its colour whole");
  expect(SUCCEEDED(device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE)), "D3DRS_ALPHABLENDENABLE was refused");
  expect(SUCCEEDED(device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE)), "D3DRS_SRCBLEND was refused");
  expect(SUCCEEDED(device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE)), "D3DRS_DESTBLEND was refused");
  CHECK(clear(blue, false));
  draw(half_red);
  const D3DCOLOR added = pixel(40, 40);
  expect(added == 0xFF00FF, "ONE/ONE did not add the draw to the target");

  // BLENDOP is the operation: a 200 grey minus a 100 one is 100, and the other way round clamps to black
  const D3DCOLOR grey = D3DCOLOR_XRGB(100, 100, 100);
  PreTransformedVertex light_grey[3];
  triangle_at(20.0f, 20.0f, 100.0f, 0.0f, D3DCOLOR_XRGB(200, 200, 200), light_grey);
  expect(SUCCEEDED(device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_SUBTRACT)), "D3DRS_BLENDOP was refused");
  CHECK(clear(grey, false));
  draw(light_grey);
  const D3DCOLOR subtracted = pixel(40, 40);
  expect(subtracted == 0x646464, "BLENDOP SUBTRACT did not draw the source minus the destination");
  expect(SUCCEEDED(device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_REVSUBTRACT)), "D3DRS_BLENDOP was refused");
  CHECK(clear(grey, false));
  draw(light_grey);
  const D3DCOLOR reverse_subtracted = pixel(40, 40);
  expect(reverse_subtracted == 0x000000, "BLENDOP REVSUBTRACT did not draw the destination minus the source");
  expect(SUCCEEDED(device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD)), "D3DRS_BLENDOP was refused");
  printf("blend: SRCALPHA/INVSRCALPHA reads %06lx, blending off %06lx, ONE/ONE %06lx, SUBTRACT %06lx, REVSUBTRACT "
         "%06lx\n",
         (unsigned long)blended, (unsigned long)0xFF0000, (unsigned long)added, (unsigned long)subtracted,
         (unsigned long)reverse_subtracted);
  expect(SUCCEEDED(device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE)), "D3DRS_ALPHABLENDENABLE was refused");

  // COLORWRITEENABLE: a mask keeps the draw out of the channels it does not name
  CHECK(clear(blue, false));
  expect(SUCCEEDED(device->SetRenderState(D3DRS_COLORWRITEENABLE, 0)), "D3DRS_COLORWRITEENABLE was refused");
  draw(half_red);
  expect(pixel(40, 40) == (blue & 0x00FFFFFF), "COLORWRITEENABLE 0 wrote a colour");
  expect(SUCCEEDED(device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED)), "D3DRS_COLORWRITEENABLE was refused");
  draw(half_red);
  expect(pixel(40, 40) == 0xFF00FF, "COLORWRITEENABLE RED changed a channel it does not name");
  expect(SUCCEEDED(device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF)), "D3DRS_COLORWRITEENABLE was refused");
  printf("colorwrite: 0 leaves the target, RED writes only red\n");

  // shade: GOURAUD interpolates the three colours, FLAT gives the triangle the first vertex's
  PreTransformedVertex shaded[3] = {{20.0f, 20.0f, 0.0f, 1.0f, D3DCOLOR_XRGB(255, 0, 0)},
                                    {20.0f, 120.0f, 0.0f, 1.0f, D3DCOLOR_XRGB(0, 255, 0)},
                                    {120.0f, 20.0f, 0.0f, 1.0f, D3DCOLOR_XRGB(0, 0, 255)}};
  CHECK(clear(kClear, false));
  draw(shaded);
  const D3DCOLOR gouraud = pixel(25, 25);
  expect(((gouraud >> 16) & 0xFF) > 200 && gouraud != 0xFF0000 && gouraud != 0x00FF00 && gouraud != 0x0000FF,
         "GOURAUD did not interpolate the three vertex colours");
  expect(SUCCEEDED(device->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_FLAT)), "D3DRS_SHADEMODE was refused");
  CHECK(clear(kClear, false));
  draw(shaded);
  const D3DCOLOR flat = pixel(25, 25);
  expect(flat == 0xFF0000, "FLAT did not give the triangle its first vertex's colour");
  expect(device->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_PHONG) == D3DERR_INVALIDCALL, "SHADEMODE PHONG was taken");
  CHECK(clear(kClear, false));
  draw(shaded);
  expect(pixel(25, 25) == 0xFF0000, "a refused SHADEMODE changed the mode");
  expect(SUCCEEDED(device->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD)), "D3DRS_SHADEMODE was refused");
  printf("shade: GOURAUD reads %06lx at 25,25, FLAT reads %06lx (wanted ff0000)\n", (unsigned long)gouraud,
         (unsigned long)flat);

  // fog is stored; a state this milestone cannot honour is refused, and the device keeps what it had
  DWORD value = 0;
  CHECK(device->SetRenderState(D3DRS_FOGENABLE, TRUE));
  CHECK(device->GetRenderState(D3DRS_FOGENABLE, &value));
  expect(value == TRUE, "FOGENABLE was not stored");
  expect(device->SetRenderState(D3DRS_DITHERENABLE, TRUE) == D3DERR_INVALIDCALL,
         "a state that is not honoured was taken");
  expect(device->GetRenderState(D3DRS_DITHERENABLE, &value) == D3DERR_INVALIDCALL,
         "a state that is not honoured was read back");
  expect(device->SetRenderState((D3DRENDERSTATETYPE)0xFFFF, 0) == D3DERR_INVALIDCALL, "an unknown state was taken");
  expect(device->SetRenderState(D3DRS_CULLMODE, 99) == D3DERR_INVALIDCALL, "an unknown cull mode was taken");
  CHECK(clear(kClear, false));
  draw(clockwise);
  expect(pixel(40, 40) == (kClear & 0x00FFFFFF), "a refused cull mode changed the culling");
  expect(device->GetRenderState(D3DRS_CULLMODE, &value) == S_OK && value == D3DCULL_CW,
         "a refused cull mode did not leave the device's last one");
  expect(device->GetRenderState(D3DRS_ZENABLE, &value) == S_OK && value == TRUE,
         "GetRenderState did not return the depth state");

  device.Reset();
  d3d9.Reset();
  DestroyWindow(window);

  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
