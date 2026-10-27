// contract: what a textured draw of this build samples, how D3D9's texture stage reaches the pixels, and what the
// device refuses, each read out of a frame rather than out of a return code.
// - CreateTexture makes a managed texture whose level 0 the CPU writes through LockRect, and whose levels below it the
//   device makes out of it (the whole chain, when the level count asked for is 0): GetLevelCount names them and
//   GetLevelDesc gives each one's size.
// - a level of a texture is a surface of the device's, and locking it through either object is the same bytes.
// - a textured draw of D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1 samples the texture at the vertex's coordinates,
//   under the stage's sampler and operation: with the device's defaults (MODULATE of the texture and the vertex colour,
//   the texture's own alpha, point sampling, wrapping) a pixel is the texel its coordinate falls in, times the vertex
//   colour, and its alpha is the texture's.
// - the stage's operations and arguments reach the pixels (SELECTARG1, SELECTARG2, DISABLE, D3DTA_TFACTOR and
//   D3DRS_TEXTUREFACTOR, and the alpha op on its own), the sampler's addressing does (WRAP against CLAMP), and a stage
//   whose first colour argument is the texture is not processed at all while no texture is bound, which is the vertex
//   colour; a stage that names the texture in another argument samples white.
// - what the device refuses, with D3D9's code and the last good value kept: a texture usage flag, a system-memory pool,
//   a level count past the chain, a format this build has none of, a default-pool texture with more than one level, a
//   lock of a texture whose bytes are not the CPU's, a level that is not there, a stage or sampler the device does not
//   have, a sampler or stage state (or value) this build has no meaning for, a lock of a level that is not the locked
//   one, and an FVF with more than one set of texture coordinates.
// the window is asked for frames through GDI (dxgi.presentThroughGDI) so that the frame after Present can be read out
// of it, as the presentation test does; the pixels of a Metal view are not read here.
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

// a vertex with the stage's texture coordinates: D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1
struct TexturedVertex {
  float x, y, z, rhw;
  D3DCOLOR colour;
  float u, v;
};

static const DWORD kTexturedFVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;
static const DWORD kPreTransformedFVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;

static const UINT kTextureSize = 8, kClient = 192;
static const float kQuad = 16.0f, kQuadSize = 160.0f;
static const D3DCOLOR kClear = D3DCOLOR_XRGB(16, 16, 48);

// one texel per quadrant, so a pixel says which part of the texture it sampled: red (alpha FF), green (FF), blue
// (alpha 80, which no vertex colour has) and white. the last texel is a colour of its own, so a pixel that samples it
// says the texture's rows and columns arrived whole.
static const D3DCOLOR kTextureQuadrants[4] = {D3DCOLOR_ARGB(0xFF, 255, 0, 0), D3DCOLOR_ARGB(0xFF, 0, 255, 0),
                                              D3DCOLOR_ARGB(0x80, 0, 0, 255), D3DCOLOR_ARGB(0xFF, 255, 255, 255)};
static const D3DCOLOR kLastTexel = D3DCOLOR_ARGB(0xFF, 0x12, 0x34, 0x56);

static D3DCOLOR
texel_colour(UINT x, UINT y) {
  if (x + 1 == kTextureSize && y + 1 == kTextureSize)
    return kLastTexel;
  return kTextureQuadrants[(y >= kTextureSize / 2 ? 2 : 0) + (x >= kTextureSize / 2 ? 1 : 0)];
}

// a quad of two triangles covering `size` pixels from `x, y`, wound the way round the device's default cull mode draws
// a face, with `u0, v0` to `u1, v1` across it
static void
quad_at(float x, float y, float size, float u0, float v0, float u1, float v1, D3DCOLOR colour, TexturedVertex v[6]) {
  const TexturedVertex top_left{x, y, 0.0f, 1.0f, colour, u0, v0};
  const TexturedVertex bottom_left{x, y + size, 0.0f, 1.0f, colour, u0, v1};
  const TexturedVertex top_right{x + size, y, 0.0f, 1.0f, colour, u1, v0};
  const TexturedVertex bottom_right{x + size, y + size, 0.0f, 1.0f, colour, u1, v1};
  v[0] = top_left;
  v[1] = bottom_left;
  v[2] = top_right;
  v[3] = top_right;
  v[4] = bottom_left;
  v[5] = bottom_right;
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
    if (!ok && wrong++ < 20)
      printf("wrong: %s\n", what);
  };
  SetEnvironmentVariableA("MULLION_CONFIG", "dxgi.presentThroughGDI=True");

  WNDCLASSA window_class{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr,
                         "d3d9_texture"};
  RegisterClassA(&window_class);
  RECT window_rect{0, 0, (LONG)kClient, (LONG)kClient};
  AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);
  HWND window = CreateWindowA("d3d9_texture", "d3d9_texture", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0,
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
  present.BackBufferWidth = kClient;
  present.BackBufferHeight = kClient;
  present.BackBufferFormat = D3DFMT_X8R8G8B8;
  present.BackBufferCount = 1;
  present.MultiSampleType = D3DMULTISAMPLE_NONE;
  present.SwapEffect = D3DSWAPEFFECT_DISCARD;
  present.hDeviceWindow = window;
  present.Windowed = TRUE;
  present.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  ComPtr<IDirect3DDevice9> device;
  CHECK(d3d9->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &present, &device));
  // the quad is wound counter-clockwise on the screen, which D3D9's default cull mode culls
  CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));

  ComPtr<IDirect3DSurface9> readback;
  CHECK(device->CreateOffscreenPlainSurface(kClient, kClient, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr));
  ComPtr<IDirect3DSurface9> back_buffer;
  CHECK(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer));
  ComPtr<IDirect3DSurface9> readback_argb;
  CHECK(device->CreateOffscreenPlainSurface(kClient, kClient, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &readback_argb,
                                            nullptr));

  // the pixel at `x, y` of `source`, copied into `readback`, alpha and all
  auto pixel_of = [&](IDirect3DSurface9 *source, IDirect3DSurface9 *into, UINT x, UINT y) -> D3DCOLOR {
    D3DLOCKED_RECT locked{};
    if (FAILED(device->GetRenderTargetData(source, into)) || FAILED(into->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
      return 0;
    const DWORD *row = (const DWORD *)((const char *)locked.pBits + (size_t)y * locked.Pitch);
    const D3DCOLOR got = row[x];
    into->UnlockRect();
    return got;
  };
  auto back_pixel = [&](UINT x, UINT y) -> D3DCOLOR {
    return pixel_of(back_buffer.Get(), readback.Get(), x, y) & 0x00FFFFFF;
  };
  // a check of a colour of a frame, whose 24 bits are what there is to read, and of a whole pixel of a target whose
  // format carries an alpha channel; a wrong one says what it read and what it wanted
  auto expect_colour = [&](D3DCOLOR got, D3DCOLOR want, const char *what) {
    expect((got & 0x00FFFFFF) == (want & 0x00FFFFFF), what);
    if ((got & 0x00FFFFFF) != (want & 0x00FFFFFF))
      printf("       got %08lx, want %08lx\n", (unsigned long)got, (unsigned long)want);
  };
  auto expect_argb = [&](D3DCOLOR got, D3DCOLOR want, const char *what) {
    expect(got == want, what);
    if (got != want)
      printf("       got %08lx, want %08lx\n", (unsigned long)got, (unsigned long)want);
  };
  auto expect_pixel = [&](UINT x, UINT y, D3DCOLOR want, const char *what) {
    const D3DCOLOR got = back_pixel(x, y);
    expect_colour(got, want, what);
    if ((got & 0x00FFFFFF) != (want & 0x00FFFFFF))
      printf("       at %u,%u\n", x, y);
  };
  // a frame drawn with one quad of `colour` over `u0, v0` to `u1, v1`, and read back at the four texel centres the quad
  // places and at a pixel outside it
  auto quad_frame = [&](D3DCOLOR colour, float u0, float v0, float u1, float v1) {
    TexturedVertex vertices[6];
    quad_at(kQuad, kQuad, kQuadSize, u0, v0, u1, v1, colour, vertices);
    expect(SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0)), "the frame's clear was refused");
    expect(SUCCEEDED(device->BeginScene()), "BeginScene was refused");
    expect(SUCCEEDED(device->SetFVF(kTexturedFVF)), "SetFVF refused the textured format");
    expect(SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, vertices, sizeof(TexturedVertex))),
           "the draw path refused a textured quad");
    expect(SUCCEEDED(device->EndScene()), "EndScene was refused");
  };
  // where the centre of texel `texel` lands in the quad, whose texture coordinates run from 0 to `range`
  auto quad_pos = [&](float texel, float range) -> UINT {
    return (UINT)(kQuad + (texel + 0.5f) / kTextureSize / range * kQuadSize);
  };

  // the texture: the whole chain, written through its top level, whose levels below it the device makes
  ComPtr<IDirect3DTexture9> texture;
  CHECK(device->CreateTexture(kTextureSize, kTextureSize, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr));
  expect(texture->GetLevelCount() == 4, "an 8 by 8 texture with the whole chain asked for has not four levels");
  D3DSURFACE_DESC level_desc{};
  CHECK(texture->GetLevelDesc(1, &level_desc));
  expect(level_desc.Width == kTextureSize / 2 && level_desc.Height == kTextureSize / 2 &&
             level_desc.Format == D3DFMT_A8R8G8B8 && level_desc.Pool == D3DPOOL_MANAGED,
         "the second level's description is not the one the texture was made with");
  if (level_desc.Width != kTextureSize / 2 || level_desc.Height != kTextureSize / 2 ||
      level_desc.Format != D3DFMT_A8R8G8B8 || level_desc.Pool != D3DPOOL_MANAGED)
    printf("       level 1: %ux%u, format %u, pool %u\n", level_desc.Width, level_desc.Height, level_desc.Format,
           level_desc.Pool);

  // a texture whose sides are not powers of two has as many levels as its longer side halves down to one texel, and
  // each level is that many pixels
  ComPtr<IDirect3DTexture9> odd;
  CHECK(device->CreateTexture(6, 6, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &odd, nullptr));
  expect(odd->GetLevelCount() == 3, "a 6 by 6 texture has not three levels");
  D3DSURFACE_DESC odd_desc{};
  CHECK(odd->GetLevelDesc(1, &odd_desc));
  CHECK(odd->GetLevelDesc(2, &odd_desc));
  expect(odd_desc.Width == 1 && odd_desc.Height == 1, "a 6 by 6 texture's last level is not one texel");
  CHECK(odd->GetLevelDesc(1, &odd_desc));
  expect(odd_desc.Width == 3 && odd_desc.Height == 3, "a 6 by 6 texture's second level is not 3 by 3");
  CHECK(odd->GetLevelDesc(0, &odd_desc));
  expect(odd_desc.Width == 6 && odd_desc.Height == 6, "a 6 by 6 texture's top level is not 6 by 6");
  D3DLOCKED_RECT odd_locked{};
  CHECK(odd->LockRect(0, &odd_locked, nullptr, 0));
  expect(odd_locked.Pitch >= 6 * 4, "a 6 by 6 texture's top level is narrower than its row");
  CHECK(odd->UnlockRect(0));
  D3DLOCKED_RECT locked{};
  CHECK(texture->LockRect(0, &locked, nullptr, 0));
  for (UINT y = 0; y < kTextureSize; y++) {
    DWORD *row = (DWORD *)((char *)locked.pBits + (size_t)y * locked.Pitch);
    for (UINT x = 0; x < kTextureSize; x++)
      row[x] = texel_colour(x, y);
  }
  // a second lock while one is held, and an unlock of a level that is not the locked one, are refused
  D3DLOCKED_RECT second{};
  expect(texture->LockRect(0, &second, nullptr, 0) == D3DERR_INVALIDCALL, "a texture took two locks at once");
  expect(texture->UnlockRect(1) == D3DERR_INVALIDCALL, "a texture unlocked a level that was not locked");
  CHECK(texture->UnlockRect(0));

  // a level of a texture is a surface, and locking one through it is the same bytes
  ComPtr<IDirect3DSurface9> level;
  CHECK(texture->GetSurfaceLevel(0, &level));
  D3DLOCKED_RECT through_surface{};
  CHECK(level->LockRect(&through_surface, nullptr, D3DLOCK_READONLY));
  expect(((const DWORD *)through_surface.pBits)[0] == texel_colour(0, 0),
         "the surface of a level holds other bytes than the texture's");
  expect(((const DWORD *)((const char *)through_surface.pBits + (kTextureSize - 1) * through_surface.Pitch))[kTextureSize - 1] ==
             kLastTexel,
         "the surface of a level's last row is not the texture's");
  CHECK(level->UnlockRect());

  // the device's documented defaults for the stage and its sampler
  struct stage_default {
    D3DTEXTURESTAGESTATETYPE type;
    DWORD value;
    const char *what;
  } stage_defaults[] = {
      {D3DTSS_COLOROP, D3DTOP_MODULATE, "D3DTSS_COLOROP's default"},
      {D3DTSS_COLORARG1, D3DTA_TEXTURE, "D3DTSS_COLORARG1's default"},
      {D3DTSS_COLORARG2, D3DTA_CURRENT, "D3DTSS_COLORARG2's default"},
      {D3DTSS_ALPHAOP, D3DTOP_SELECTARG1, "D3DTSS_ALPHAOP's default"},
      {D3DTSS_ALPHAARG1, D3DTA_TEXTURE, "D3DTSS_ALPHAARG1's default"},
      {D3DTSS_ALPHAARG2, D3DTA_CURRENT, "D3DTSS_ALPHAARG2's default"},
      {D3DTSS_TEXCOORDINDEX, 0, "D3DTSS_TEXCOORDINDEX's default"},
      {D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE, "D3DTSS_TEXTURETRANSFORMFLAGS's default"},
      {D3DTSS_RESULTARG, D3DTA_CURRENT, "D3DTSS_RESULTARG's default"},
  };
  for (const stage_default &one : stage_defaults) {
    DWORD value = 0xFFFFFFFF;
    expect(SUCCEEDED(device->GetTextureStageState(0, one.type, &value)) && value == one.value, one.what);
  }
  struct sampler_default {
    D3DSAMPLERSTATETYPE type;
    DWORD value;
    const char *what;
  } sampler_defaults[] = {
      {D3DSAMP_MINFILTER, D3DTEXF_POINT, "D3DSAMP_MINFILTER's default"},
      {D3DSAMP_MAGFILTER, D3DTEXF_POINT, "D3DSAMP_MAGFILTER's default"},
      {D3DSAMP_MIPFILTER, D3DTEXF_NONE, "D3DSAMP_MIPFILTER's default"},
      {D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP, "D3DSAMP_ADDRESSU's default"},
      {D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP, "D3DSAMP_ADDRESSV's default"},
      {D3DSAMP_ADDRESSW, D3DTADDRESS_WRAP, "D3DSAMP_ADDRESSW's default"},
      {D3DSAMP_MAXANISOTROPY, 1, "D3DSAMP_MAXANISOTROPY's default"},
  };
  for (const sampler_default &one : sampler_defaults) {
    DWORD value = 0xFFFFFFFF;
    expect(SUCCEEDED(device->GetSamplerState(0, one.type, &value)) && value == one.value, one.what);
  }

  // the frame every textured check draws: the whole texture across the quad, the vertex colour white so that a pixel is
  // the texel it sampled
  CHECK(device->SetTexture(0, texture.Get()));
  ComPtr<IDirect3DBaseTexture9> set_texture;
  CHECK(device->GetTexture(0, &set_texture));
  expect(set_texture.Get() == texture.Get(), "GetTexture did not return the texture that was set");

  const UINT centre_x = quad_pos(2, 1.0f), centre_y = quad_pos(2, 1.0f);
  const UINT right_x = quad_pos(6, 1.0f), bottom_y = quad_pos(6, 1.0f);
  const UINT corner_x = quad_pos(7, 1.0f);
  quad_frame(D3DCOLOR_ARGB(0xFF, 255, 255, 255), 0.0f, 0.0f, 1.0f, 1.0f);
  struct texel_check {
    UINT x, y;
    D3DCOLOR want;
    const char *what;
  } texture_pixels[] = {
      {centre_x, centre_y, D3DCOLOR_XRGB(255, 0, 0), "the red quadrant of the texture is not in the frame"},
      {right_x, centre_y, D3DCOLOR_XRGB(0, 255, 0), "the green quadrant of the texture is not in the frame"},
      {centre_x, bottom_y, D3DCOLOR_XRGB(0, 0, 255), "the blue quadrant of the texture is not in the frame"},
      {right_x, bottom_y, D3DCOLOR_XRGB(255, 255, 255), "the white quadrant of the texture is not in the frame"},
      {corner_x, corner_x, kLastTexel & 0x00FFFFFF, "the texture's last texel is not where its coordinate says"},
  };
  for (const texel_check &one : texture_pixels)
    expect_pixel(one.x, one.y, one.want, one.what);
  expect_pixel(4, 4, kClear & 0x00FFFFFF, "the frame is textured outside the quad");
  CHECK(device->Present(nullptr, nullptr, nullptr, nullptr));
  expect(window_shows(window, centre_x, centre_y, D3DCOLOR_XRGB(255, 0, 0)),
         "the window does not show the textured frame");
  printf("the frame's pixels: %06lx at %u,%u (the texture's red texel), %06lx at %u,%u (its last)\n",
         (unsigned long)back_pixel(centre_x, centre_y), centre_x, centre_y, (unsigned long)back_pixel(corner_x, corner_x),
         corner_x, corner_x);

  // the vertex colour the stage multiplies the texture by (D3D9's default MODULATE of the texture and the current
  // colour)
  quad_frame(D3DCOLOR_ARGB(0xFF, 0x80, 0x80, 0x80), 0.0f, 0.0f, 1.0f, 1.0f);
  expect_pixel(centre_x, centre_y, D3DCOLOR_XRGB(0x80, 0, 0), "the vertex colour did not modulate the texture");

  // the stage's operations and arguments, each read out of the quad's red texel
  auto draw_and_read = [&](D3DCOLOR colour, UINT x, UINT y) -> D3DCOLOR {
    quad_frame(colour, 0.0f, 0.0f, 1.0f, 1.0f);
    return back_pixel(x, y);
  };
  CHECK(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
  CHECK(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE));
  expect_colour(draw_and_read(D3DCOLOR_ARGB(0xFF, 0x11, 0x22, 0x33), centre_x, centre_y),
                D3DCOLOR_XRGB(0x11, 0x22, 0x33), "SELECTARG1 of the vertex colour is not the frame");
  CHECK(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
  CHECK(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_DISABLE));
  expect_colour(draw_and_read(D3DCOLOR_ARGB(0xFF, 0x44, 0x55, 0x66), centre_x, centre_y),
                D3DCOLOR_XRGB(0x44, 0x55, 0x66), "a disabled stage did not pass the vertex colour on");
  CHECK(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2));
  CHECK(device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR));
  CHECK(device->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_XRGB(0, 0xCC, 0)));
  expect_colour(draw_and_read(D3DCOLOR_ARGB(0xFF, 0x99, 0x99, 0x99), centre_x, centre_y), D3DCOLOR_XRGB(0, 0xCC, 0),
                "D3DTA_TFACTOR is not D3DRS_TEXTUREFACTOR's colour");
  CHECK(device->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFFFFFFFF));
  CHECK(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
  CHECK(device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_CURRENT));
  CHECK(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE));
  // D3DTOP_ADD adds the two arguments: red plus white is white in the channels that saturate
  // the green texel (0, 255, 0) added to the vertex colour (0, 0x80, 0x80): green and blue both take the addition
  CHECK(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_ADD));
  expect_colour(draw_and_read(D3DCOLOR_ARGB(0xFF, 0x00, 0x80, 0x80), right_x, centre_y), D3DCOLOR_XRGB(0x00, 0xFF, 0x80),
                "D3DTOP_ADD of the texture and the vertex colour is not the frame");
  CHECK(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE));

  // what the sampler's addressing does: the same quad with the texture's coordinates running to 2 in u, so the red
  // texel the quad places at u = 0.3125 is the one it places at u = 1.3125 as well
  CHECK(device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP));
  const UINT wrapped_x = quad_pos(10, 2.0f) /* u = 1.3125: the texture's second turn of the same texel */;
  const UINT wrapped_y = quad_pos(2, 1.0f);
  expect(wrapped_x != centre_x && quad_pos(2, 2.0f) != wrapped_x,
         "the wrapping quad's two turns of the texture are not where they should be");
  quad_frame(D3DCOLOR_ARGB(0xFF, 255, 255, 255), 0.0f, 0.0f, 2.0f, 1.0f);
  expect_pixel(wrapped_x, wrapped_y, D3DCOLOR_XRGB(255, 0, 0), "a wrapping sampler did not repeat the texture");
  CHECK(device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP));
  quad_frame(D3DCOLOR_ARGB(0xFF, 255, 255, 255), 0.0f, 0.0f, 2.0f, 1.0f);
  expect_pixel(wrapped_x, wrapped_y, D3DCOLOR_XRGB(0, 255, 0), "a clamping sampler did not hold the texture's edge");
  CHECK(device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP));

  // SHADEMODE with the texture bound: a FLAT draw samples the stage as a GOURAUD one does and takes one vertex's colour
  // for the whole triangle -- the three vertices of the quad carry the same colour, so the frame is the texture's own
  // texels, and the triangle below carries three
  CHECK(device->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_FLAT));
  quad_frame(D3DCOLOR_ARGB(0xFF, 255, 255, 255), 0.0f, 0.0f, 1.0f, 1.0f);
  expect_pixel(centre_x, centre_y, D3DCOLOR_XRGB(255, 0, 0), "a FLAT textured draw did not sample the texture");
  expect_pixel(corner_x, corner_x, kLastTexel & 0x00FFFFFF,
               "a FLAT textured draw did not sample the texture's last texel");

  // a triangle inside the texture's white quadrant -- its texture coordinates are its own position's, so the pixel at
  // 104,104 samples a white texel and carries the shading alone -- drawn with three colours whose first vertex is white
  const float shaded_x = kQuad + 84.0f, shaded_y = kQuad + 84.0f, shaded_size = 40.0f;
  const float shaded_u = 84.0f / kQuadSize, shaded_far = (84.0f + shaded_size) / kQuadSize;
  const TexturedVertex shaded[3] = {
      {shaded_x, shaded_y, 0.0f, 1.0f, D3DCOLOR_ARGB(0xFF, 255, 255, 255), shaded_u, shaded_u},
      {shaded_x, shaded_y + shaded_size, 0.0f, 1.0f, D3DCOLOR_ARGB(0xFF, 0, 255, 0), shaded_u, shaded_far},
      {shaded_x + shaded_size, shaded_y, 0.0f, 1.0f, D3DCOLOR_ARGB(0xFF, 0, 0, 255), shaded_far, shaded_u},
  };
  auto shaded_frame = [&] {
    expect(SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0)), "the frame's clear was refused");
    expect(SUCCEEDED(device->BeginScene()), "BeginScene was refused");
    expect(SUCCEEDED(device->SetFVF(kTexturedFVF)), "SetFVF refused the textured format");
    expect(SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, shaded, sizeof(TexturedVertex))),
           "the draw path refused a shaded textured triangle");
    expect(SUCCEEDED(device->EndScene()), "EndScene was refused");
  };
  shaded_frame();
  const D3DCOLOR flat_shaded = back_pixel(104, 104);
  expect_colour(flat_shaded, D3DCOLOR_XRGB(255, 255, 255),
                "a FLAT textured draw did not give the triangle its first vertex's colour");
  CHECK(device->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD));
  shaded_frame();
  const D3DCOLOR gouraud_shaded = back_pixel(104, 104);
  expect(gouraud_shaded != flat_shaded && (gouraud_shaded & 0xFF) > 0,
         "a GOURAUD textured draw did not interpolate the three vertex colours");
  printf("shade with the texture bound: FLAT reads %06lx at 104,104, GOURAUD reads %06lx\n",
         (unsigned long)flat_shaded, (unsigned long)gouraud_shaded);

  // no texture bound: a stage whose first colour argument is the texture is not processed at all, so the frame is the
  // vertex colour; a stage that names the texture in another argument samples white
  CHECK(device->SetTexture(0, nullptr));
  expect_colour(draw_and_read(D3DCOLOR_ARGB(0xFF, 0x7A, 0x00, 0x00), centre_x, centre_y), D3DCOLOR_XRGB(0x7A, 0, 0),
                "a stage with no texture and a texture's first argument was not the vertex colour");
  // the texture in the second argument instead: the stage is processed, and the texture it does not have reads white,
  // so adding it whitens every channel the vertex colour has
  CHECK(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE));
  CHECK(device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TEXTURE));
  CHECK(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_ADD));
  expect_colour(draw_and_read(D3DCOLOR_ARGB(0xFF, 0x11, 0x11, 0x11), centre_x, centre_y), D3DCOLOR_XRGB(0xFF, 0xFF, 0xFF),
                "a stage that samples no texture did not read white");
  CHECK(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
  CHECK(device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_CURRENT));
  CHECK(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE));
  CHECK(device->SetTexture(0, texture.Get()));

  // the alpha op, on its own: D3D9's default takes the texture's alpha, and SELECTARG2 of the vertex colour takes the
  // vertex's. the target is read back with its alpha channel, which the swap chain's format has none of.
  ComPtr<IDirect3DSurface9> argb_target;
  CHECK(device->CreateRenderTarget(kClient, kClient, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &argb_target,
                                   nullptr));
  CHECK(device->SetRenderTarget(0, argb_target.Get()));
  quad_frame(D3DCOLOR_ARGB(0xFF, 255, 255, 255), 0.0f, 0.0f, 1.0f, 1.0f);
  // the texture's blue texel, whose alpha is the one no vertex colour has
  const UINT blue_x = quad_pos(2, 1.0f), blue_y = quad_pos(6, 1.0f);
  expect_argb(pixel_of(argb_target.Get(), readback_argb.Get(), blue_x, blue_y), D3DCOLOR_ARGB(0x80, 0, 0, 255),
              "the default alpha op did not take the texture's alpha");
  CHECK(device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2));
  CHECK(device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE));
  quad_frame(D3DCOLOR_ARGB(0x40, 255, 255, 255), 0.0f, 0.0f, 1.0f, 1.0f);
  expect_argb(pixel_of(argb_target.Get(), readback_argb.Get(), blue_x, blue_y), D3DCOLOR_ARGB(0x40, 0, 0, 255),
              "SELECTARG2 of the vertex colour did not take the vertex's alpha");
  CHECK(device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1));
  CHECK(device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_CURRENT));
  CHECK(device->SetRenderTarget(0, back_buffer.Get()));

  // what the device refuses, and what it keeps
  ComPtr<IDirect3DTexture9> refused;
  expect(device->CreateTexture(kTextureSize, kTextureSize, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                               &refused, nullptr) == D3DERR_INVALIDCALL,
         "a texture usage flag this build has not is taken");
  expect(device->CreateTexture(kTextureSize, kTextureSize, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &refused,
                               nullptr) == D3DERR_INVALIDCALL,
         "a system-memory texture is taken");
  expect(device->CreateTexture(kTextureSize, kTextureSize, 5, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &refused, nullptr) ==
             D3DERR_INVALIDCALL,
         "a level count past the texture's chain is taken");
  expect(device->CreateTexture(kTextureSize, kTextureSize, 1, 0, D3DFMT_L8, D3DPOOL_MANAGED, &refused, nullptr) ==
             D3DERR_INVALIDCALL,
         "a texture format this build has none of is taken");
  expect(device->CreateTexture(0, kTextureSize, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &refused, nullptr) ==
             D3DERR_INVALIDCALL,
         "a texture of no pixels is taken");
  ComPtr<IDirect3DTexture9> default_pool;
  CHECK(device->CreateTexture(kTextureSize, kTextureSize, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &default_pool,
                              nullptr));
  expect(device->CreateTexture(kTextureSize, kTextureSize, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &refused, nullptr) ==
             D3DERR_INVALIDCALL,
         "a default-pool texture whose levels nothing can fill is taken");
  expect(default_pool->LockRect(0, &locked, nullptr, 0) == D3DERR_INVALIDCALL,
         "a texture whose bytes are not the CPU's took a lock");
  expect(default_pool->UnlockRect(0) == D3DERR_INVALIDCALL, "a texture unlocked a level it never locked");
  expect(texture->LockRect(texture->GetLevelCount(), &locked, nullptr, 0) == D3DERR_INVALIDCALL,
         "a texture took a lock of a level that is not there");
  expect(texture->GetLevelDesc(texture->GetLevelCount(), &level_desc) == D3DERR_INVALIDCALL,
         "a texture described a level that is not there");
  expect(texture->GetSurfaceLevel(texture->GetLevelCount(), &level) == D3DERR_INVALIDCALL,
         "a texture handed out a surface of a level that is not there");
  expect(device->SetTexture(1, texture.Get()) == D3DERR_INVALIDCALL, "a texture was set on a stage the device has not");
  expect(device->GetTexture(1, &set_texture) == D3DERR_INVALIDCALL, "a stage the device has not was read");
  expect(device->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR) == D3DERR_INVALIDCALL,
         "a sampler state was set on a sampler the device has not");
  expect(device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_NONE) == D3DERR_INVALIDCALL,
         "a minification filter that is not one is taken");
  expect(device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_BORDER) == D3DERR_INVALIDCALL,
         "a border address mode is taken");
  expect(device->SetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, 0x3F800000) == D3DERR_INVALIDCALL,
         "a level-of-detail bias is taken");
  expect(device->SetSamplerState(0, D3DSAMP_MAXANISOTROPY, 16) == S_OK &&
             device->SetSamplerState(0, D3DSAMP_MAXANISOTROPY, 17) == D3DERR_INVALIDCALL,
         "an anisotropy past the sampler's 16 is taken");
  CHECK(device->SetSamplerState(0, D3DSAMP_MAXANISOTROPY, 1));
  // each of the three filter picks is taken on its own whatever the other two are, and read back as it was set: the
  // D3D11 filter a draw samples through is the closest one to the three (src/d3d9/d3d9_texture.cpp)
  CHECK(device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC));
  expect(device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_ANISOTROPIC) == S_OK &&
             device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_POINT) == S_OK,
         "a filter pick on its own is refused while the other two are another filter");
  DWORD kept_filter = 0;
  CHECK(device->GetSamplerState(0, D3DSAMP_MINFILTER, &kept_filter));
  expect(kept_filter == D3DTEXF_ANISOTROPIC, "a sampler value did not become the sampler's own");
  // the anistropic sampler draws the texture itself, with the texels where their coordinates say
  CHECK(device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_ANISOTROPIC));
  CHECK(device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR));
  CHECK(device->SetSamplerState(0, D3DSAMP_MAXANISOTROPY, 16));
  quad_frame(D3DCOLOR_ARGB(0xFF, 255, 255, 255), 0.0f, 0.0f, 1.0f, 1.0f);
  expect_colour(back_pixel(centre_x, centre_y), D3DCOLOR_XRGB(255, 0, 0),
                "an anisotropic sampler did not sample the texture (its red texel)");
  expect_colour(back_pixel(right_x, centre_y), D3DCOLOR_XRGB(0, 255, 0),
                "an anisotropic sampler did not sample the texture (its green texel)");
  CHECK(device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT));
  CHECK(device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
  CHECK(device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE));
  CHECK(device->SetSamplerState(0, D3DSAMP_MAXANISOTROPY, 1));
  expect(device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE) == D3DERR_INVALIDCALL,
         "a texture stage state was set on a stage the device has not");
  expect(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_DOTPRODUCT3) == D3DERR_INVALIDCALL,
         "a texture operation this build has not is taken");
  expect(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE | D3DTA_COMPLEMENT) == D3DERR_INVALIDCALL,
         "a complemented texture argument is taken");
  expect(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_SPECULAR) == D3DERR_INVALIDCALL,
         "a specular argument is taken");
  expect(device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 1) == D3DERR_INVALIDCALL,
         "a second set of texture coordinates is taken");
  expect(device->SetTextureStageState(0, D3DTSS_BUMPENVMAT00, 1) == D3DERR_INVALIDCALL,
         "a bump-mapping state is taken");
  expect(device->SetTextureStageState(0, D3DTSS_RESULTARG, D3DTA_TEMP) == D3DERR_INVALIDCALL,
         "a result register other than the current one is taken");
  DWORD kept_op = 0;
  CHECK(device->GetTextureStageState(0, D3DTSS_COLOROP, &kept_op));
  expect(kept_op == D3DTOP_MODULATE, "a refused operation did not leave the stage's own");
  // the format this build transforms stays the device's when one with more texture coordinates is refused
  expect(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX2) == D3DERR_INVALIDCALL,
         "a vertex format with two sets of texture coordinates is taken");
  quad_frame(D3DCOLOR_ARGB(0xFF, 255, 255, 255), 0.0f, 0.0f, 1.0f, 1.0f);
  expect_pixel(centre_x, centre_y, D3DCOLOR_XRGB(255, 0, 0), "a refused vertex format did not leave the device's own");
  // a stride that is not the format's is refused rather than read as vertices of another size
  TexturedVertex one_quad[6];
  quad_at(kQuad, kQuad, kQuadSize, 0.0f, 0.0f, 1.0f, 1.0f, D3DCOLOR_ARGB(0xFF, 0xFF, 0xFF, 0xFF), one_quad);
  expect(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, one_quad, sizeof(TexturedVertex) - 4) == D3DERR_INVALIDCALL,
         "a draw whose stride is not the format's is taken");

  device.Reset();
  d3d9.Reset();
  DestroyWindow(window);

  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
