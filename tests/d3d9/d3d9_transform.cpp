// contract: Direct3D 9's fixed-function transform stage. A D3DFVF_XYZ vertex is transformed by the world, view and
// projection matrices the device holds, in that order, and the result is divided by its w and mapped through the
// viewport — x and y into the viewport's rectangle, z into its MinZ..MaxZ range (Microsoft Learn, "Mathematics of
// Direct3D", IDirect3DDevice9::SetTransform, D3DVIEWPORT9). A D3DFVF_XYZRHW vertex is already in screen space and is
// mapped through the viewport alone, which is the path this build has had.
// https://learn.microsoft.com/en-us/windows/win32/direct3d9/transforms
// https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-settransform
//
// The device starts with the three matrices identity, as D3D9's does, a null matrix is D3DERR_INVALIDCALL, and a
// transform this build does not perform (a texture coordinate transform) is refused rather than kept by nobody.
//
// The checks, on a 64 by 64 frame whose viewport is the whole of it:
// - the same triangle drawn as XYZ under identity transforms and as XYZRHW at the pixels those vertices name puts the
//   same pixels on the frame, every pixel of the frame compared.
// - a world translation of 0.5 moves the geometry exactly 16 pixels; a world scale of 0.5 halves it about the origin;
//   a world rotation of 90 degrees turns it a quarter turn about the origin, checked by the pixels it leaves.
// - a projection whose w is 2 halves the geometry's distance from the centre of the screen: the divide happens, and
//   the pixel it lands on is known rather than merely not the clear colour.
// - the viewport's MinZ..MaxZ range maps the depth: two triangles whose clip z differ by 0.1 are 0.025 apart in depth
//   when the range is 0.75..1.0, so the nearer one stays in front.
// - a user clip plane is in world space for the fixed-function path (SetClipPlane): x >= 0 clips the quad's left
//   half, under a view that moves the world 0.5 right it clips at pixel 48 rather than 32, it never clips a
//   D3DFVF_XYZRHW draw, and D3DRS_CLIPPLANEENABLE of zero draws the whole quad. The planes round-trip, a seventh is
//   refused, the caps offer six, and an all-state block carries them.
// - ProcessVertices writes the screen-space vertices the transform makes: a translated quad lands 8 pixels over and
//   draws there from the processed buffer, the viewport's rectangle and depth range are applied, D3DPV_DONOTCOPYDATA
//   leaves an unlit colour alone, a destination declaration places the elements, a lit vertex's colour is the one the
//   lit draw gives, and a read or write past a buffer or a destination without a screen position is refused.
// - SetTransform and GetTransform round-trip, the default is identity, and a null matrix or an unsupported transform
//   are refused.
#include "d3d9_frame.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

// a vertex the device transforms: its position is in model space and goes through the device's matrices
struct XYZVertex {
  float x, y, z;
  D3DCOLOR colour;
};

// a vertex already in screen space: its xy are pixels of the frame, its z the depth, its w the reciprocal of w
struct XYZRHWVertex {
  float x, y, z, rhw;
  D3DCOLOR colour;
};

// a vertex the device transforms that carries the stage's texture coordinates (D3DFVF_XYZ | D3DFVF_DIFFUSE |
// D3DFVF_TEX1)
struct XYZTexturedVertex {
  float x, y, z;
  D3DCOLOR colour;
  float u, v;
};

static const DWORD kXYZFVF = D3DFVF_XYZ | D3DFVF_DIFFUSE;
static const DWORD kXYZTexturedFVF = kXYZFVF | D3DFVF_TEX1;
static const DWORD kXYZRHWFVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;
static const D3DCOLOR kClear = D3DCOLOR_XRGB(10, 20, 30);
static const D3DCOLOR kColour = D3DCOLOR_XRGB(255, 255, 0);
static const D3DCOLOR kOther = D3DCOLOR_XRGB(0, 255, 255);

// the colour a pixel of the frame holds: an X8R8G8B8 target's alpha is not a value it keeps, so a colour is compared
// without its own (D3DCOLOR_XRGB writes an alpha the target has no room for)
static D3DCOLOR
rgb(D3DCOLOR colour) {
  return colour & 0x00FFFFFF;
}

// the frame's matrices, size and pixel-to-NDC mapping are the shared frame's (d3d9_frame.hpp); a transform is the
// row-vector product D3D9 composes them by: a vertex row v is transformed by v * world * view * projection
using d3d9_test::identity;
using d3d9_test::ndc_x;
using d3d9_test::ndc_y;

static D3DMATRIX
translation(float x, float y, float z) {
  D3DMATRIX m = identity();
  m.m[3][0] = x;
  m.m[3][1] = y;
  m.m[3][2] = z;
  return m;
}

static D3DMATRIX
rotation_z(float radians) {
  D3DMATRIX m = identity();
  const float c = cosf(radians), s = sinf(radians);
  // v * M: x' = x cos - y sin, y' = x sin + y cos
  m.m[0][0] = c;
  m.m[0][1] = s;
  m.m[1][0] = -s;
  m.m[1][1] = c;
  return m;
}

int
main() {
  unsigned wrong = 0, checks = 0;
  auto expect = [&](bool ok, const char *what) {
    checks++;
    if (!ok && wrong++ < 16)
      printf("wrong: %s\n", what);
  };

  const UINT client = d3d9_test::kSize;
  d3d9_test::Frame window_frame;
  if (!window_frame.create("d3d9_transform"))
    return 1;
  IDirect3DDevice9 *device = window_frame.device();
  // these draws are about the transform, not lighting: D3D9 lights an XYZ | DIFFUSE draw by default, and with no
  // material ambient or emissive that would make the vertex colour black
  CHECK(device->SetRenderState(D3DRS_LIGHTING, FALSE));
  auto read_frame = [&]() { return window_frame.read(); };
  auto pixel = [&](UINT x, UINT y) -> D3DCOLOR { return window_frame.pixel(x, y); };
  const std::vector<D3DCOLOR> &frame = window_frame.pixels();

  // a quad from `left, top` to `right, bottom` in pixels, in the device's space (xy of the XY vertex are the frame's
  // pixel positions, through the viewport and nothing else)
  auto draw_quad = [&](float left, float top, float right, float bottom, float z_x, float z_y,
                       D3DCOLOR colour) -> HRESULT {
    const XYZVertex vertices[4] = {{ndc_x(left), ndc_y(top), z_x, colour},
                                   {ndc_x(right), ndc_y(top), z_x, colour},
                                   {ndc_x(left), ndc_y(bottom), z_y, colour},
                                   {ndc_x(right), ndc_y(bottom), z_y, colour}};
    return device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(XYZVertex));
  };

  // every check draws on a cleared frame with the fixed-function path's default states and the whole viewport, and the
  // world, view and projection the caller asked for
  auto begin = [&](const D3DMATRIX &world, const D3DMATRIX &view, const D3DMATRIX &projection) -> HRESULT {
    HRESULT hr = device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    if (FAILED(hr))
      return hr;
    D3DVIEWPORT9 viewport{0, 0, client, client, 0.0f, 1.0f};
    if (FAILED(hr = device->SetViewport(&viewport)))
      return hr;
    if (FAILED(hr = device->SetTransform(D3DTS_WORLD, &world)) ||
        FAILED(hr = device->SetTransform(D3DTS_VIEW, &view)) ||
        FAILED(hr = device->SetTransform(D3DTS_PROJECTION, &projection)))
      return hr;
    if (FAILED(hr = device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, kClear, 1.0f, 0)))
      return hr;
    return device->BeginScene();
  };
  // a frame is read from the back buffer before it is presented: a D3DSWAPEFFECT_DISCARD chain keeps nothing of what
  // it presented, so the pixels are the back buffer's only until Present
  auto end = [&]() -> HRESULT { return device->EndScene(); };
  auto show = [&]() -> HRESULT { return device->Present(nullptr, nullptr, nullptr, nullptr); };

  const D3DMATRIX identity_matrix = identity();

  // ---- the identity transforms are the screen-space path ------------------------------------------------
  // the same triangle as pixels: 8,8 to 56,8 to 8,56
  {
    CHECK(device->SetFVF(kXYZRHWFVF));
    CHECK(begin(identity_matrix, identity_matrix, identity_matrix));
    const XYZRHWVertex screen[3] = {{8.0f, 8.0f, 0.5f, 1.0f, kColour},
                                    {56.0f, 8.0f, 0.5f, 1.0f, kColour},
                                    {8.0f, 56.0f, 0.5f, 1.0f, kColour}};
    CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, screen, sizeof(XYZRHWVertex)));
    CHECK(end());
    std::vector<D3DCOLOR> screen_frame;
    expect(read_frame(), "the frame with the screen-space triangle could not be read");
    CHECK(show());
    screen_frame = frame;

    // the same triangle through the device's matrices, which start identity: the clip positions the identity makes are
    // the NDC of those pixels, so the rasterizer covers the pixels the screen-space triangle covered
    CHECK(device->SetFVF(kXYZFVF));
    CHECK(begin(identity_matrix, identity_matrix, identity_matrix));
    const XYZVertex model[3] = {{ndc_x(8.0f), ndc_y(8.0f), 0.5f, kColour},
                                {ndc_x(56.0f), ndc_y(8.0f), 0.5f, kColour},
                                {ndc_x(8.0f), ndc_y(56.0f), 0.5f, kColour}};
    CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, model, sizeof(XYZVertex)));
    CHECK(end());
    expect(read_frame(), "the frame with the transformed triangle could not be read");
    CHECK(show());
    size_t differing = 0;
    for (size_t at = 0; at < frame.size(); at++)
      if ((frame[at] & 0x00FFFFFF) != (screen_frame[at] & 0x00FFFFFF))
        differing++;
    expect(differing == 0, "the transformed triangle and the screen-space one are not the same pixels");
    expect(pixel(32, 20) == rgb(kColour), "the identity transform drew nothing");
  }

  // ---- a world translation moves the geometry by its pixels -------------------------------------------
  {
    CHECK(device->SetFVF(kXYZFVF));
    CHECK(begin(translation(0.5f, 0.0f, 0.0f), identity_matrix, identity_matrix));
    // 8..56 moved right by half the screen: 24..72, of which 64 is the frame's edge
    CHECK(draw_quad(8.0f, 8.0f, 56.0f, 56.0f, 0.5f, 0.5f, kColour));
    CHECK(end());
    expect(read_frame(), "a translated frame could not be read");
    CHECK(show());
    expect(pixel(40, 32) == rgb(kColour), "a translation of 0.5 did not draw at all");
    expect(pixel(60, 32) == rgb(kColour), "a translation of 0.5 did not move the geometry 16 pixels right");
    expect(pixel(16, 32) == rgb(kClear), "a translation of 0.5 left the geometry where it was");
  }

  // ---- a world scale halves the geometry about the origin ----------------------------------------------
  {
    D3DMATRIX half = identity_matrix;
    half.m[0][0] = 0.5f;
    half.m[1][1] = 0.5f;
    CHECK(device->SetFVF(kXYZFVF));
    CHECK(begin(half, identity_matrix, identity_matrix));
    // 8..56 about the centre, halved: 20..44
    CHECK(draw_quad(8.0f, 8.0f, 56.0f, 56.0f, 0.5f, 0.5f, kColour));
    CHECK(end());
    expect(read_frame(), "a scaled frame could not be read");
    CHECK(show());
    expect(pixel(32, 32) == rgb(kColour), "a scale of 0.5 did not draw at all");
    expect(pixel(24, 32) == rgb(kColour), "a scale of 0.5 did not keep the geometry's middle");
    expect(pixel(48, 32) == rgb(kClear), "a scale of 0.5 did not shrink the geometry");
    expect(pixel(16, 32) == rgb(kClear), "a scale of 0.5 drew outside the halved geometry");
  }

  // ---- a world rotation turns it a quarter turn about the origin ---------------------------------------
  {
    // a rectangle from 8,8 to 40,24 (the top left of the frame), turned 90 degrees anticlockwise about the centre of
    // the frame: it lands at 8,40 to 24,56 (the bottom left)
    CHECK(device->SetFVF(kXYZFVF));
    CHECK(begin(identity_matrix, identity_matrix, identity_matrix));
    CHECK(draw_quad(8.0f, 8.0f, 40.0f, 24.0f, 0.5f, 0.5f, kColour));
    CHECK(end());
    expect(read_frame(), "the frame before a rotation could not be read");
    CHECK(show());
    expect(pixel(16, 16) == rgb(kColour), "the rectangle was not drawn before the rotation");
    expect(pixel(16, 48) == rgb(kClear), "the rectangle was drawn where the rotation should move it");

    CHECK(begin(rotation_z(3.14159265f / 2.0f), identity_matrix, identity_matrix));
    CHECK(draw_quad(8.0f, 8.0f, 40.0f, 24.0f, 0.5f, 0.5f, kColour));
    CHECK(end());
    expect(read_frame(), "a rotated frame could not be read");
    CHECK(show());
    expect(pixel(16, 48) == rgb(kColour), "a rotation of 90 degrees did not move the rectangle a quarter turn");
    expect(pixel(16, 16) == rgb(kClear), "a rotation of 90 degrees left the rectangle where it was");
    expect(pixel(32, 48) == rgb(kClear), "a rotation of 90 degrees drew outside the turned rectangle");
  }

  // ---- the perspective divide --------------------------------------------------------------------------
  {
    // a projection whose w is 2 for every vertex: the clip position (x, y, z, 1) becomes (x, y, z, 2), so the
    // geometry lands at half the distance from the middle of the screen that its clip position names
    D3DMATRIX projection = identity_matrix;
    projection.m[3][3] = 2.0f;
    CHECK(device->SetFVF(kXYZFVF));
    CHECK(begin(identity_matrix, identity_matrix, projection));
    // x from 40 to 56 (NDC 0.25 to 0.75) becomes NDC 0.125 to 0.375, which is x from 36 to 44
    CHECK(draw_quad(40.0f, 24.0f, 56.0f, 40.0f, 0.5f, 0.5f, kColour));
    CHECK(end());
    expect(read_frame(), "a projected frame could not be read");
    CHECK(show());
    expect(pixel(40, 32) == rgb(kColour), "the perspective divide did not put the geometry where its w says");
    expect(pixel(52, 32) == rgb(kClear), "the perspective divide did not halve the geometry's distance");
    expect(pixel(32, 32) == rgb(kClear), "the perspective divide doubled the geometry's distance the other way");
  }

  // ---- the viewport's depth range ----------------------------------------------------------------------
  {
    D3DVIEWPORT9 viewport{0, 0, client, client, 0.75f, 1.0f};
    CHECK(device->SetViewport(&viewport));
    CHECK(device->SetRenderState(D3DRS_ZENABLE, TRUE));
    CHECK(device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS));
    CHECK(device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, kClear, 1.0f, 0));
    CHECK(device->BeginScene());
    // clip z 0.5 is depth 0.875 and clip z 0.6 is 0.9 in a 0.75..1.0 range: the farther one does not pass
    CHECK(draw_quad(8.0f, 8.0f, 56.0f, 56.0f, 0.5f, 0.5f, kColour));
    CHECK(draw_quad(8.0f, 8.0f, 56.0f, 56.0f, 0.6f, 0.6f, kOther));
    CHECK(end());
    expect(read_frame(), "a depth-ranged frame could not be read");
    CHECK(show());
    expect(pixel(32, 32) == rgb(kColour),
           "the viewport's MinZ..MaxZ range did not keep the nearer triangle in front of the farther one");
    // and a triangle that is nearer by the same amount does pass
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, kClear, 1.0f, 0));
    CHECK(device->BeginScene());
    CHECK(draw_quad(8.0f, 8.0f, 56.0f, 56.0f, 0.5f, 0.5f, kColour));
    CHECK(draw_quad(8.0f, 8.0f, 56.0f, 56.0f, 0.4f, 0.4f, kOther));
    CHECK(end());
    expect(read_frame(), "a depth-ranged frame could not be read");
    CHECK(show());
    expect(pixel(32, 32) == rgb(kOther), "the viewport's MinZ..MaxZ range kept a nearer triangle behind a farther one");
    CHECK(device->SetRenderState(D3DRS_ZENABLE, FALSE));
    D3DVIEWPORT9 whole{0, 0, client, client, 0.0f, 1.0f};
    CHECK(device->SetViewport(&whole));
  }

  // ---- a vertex the device transforms that samples a texture -------------------------------------------
  // the transform's textured variant carries the coordinates as well as the position, so a stage-0 texture reaches a
  // D3DFVF_XYZ | D3DFVF_TEX1 draw the way it reaches a screen-space one
  {
    ComPtr<IDirect3DTexture9> texture;
    CHECK(device->CreateTexture(2, 2, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr));
    D3DLOCKED_RECT locked{};
    CHECK(texture->LockRect(0, &locked, nullptr, 0));
    // red, green on the top row, blue and white below it, so which texel a pixel is says where it sampled
    const D3DCOLOR texels[2][2] = {{D3DCOLOR_XRGB(255, 0, 0), D3DCOLOR_XRGB(0, 255, 0)},
                                   {D3DCOLOR_XRGB(0, 0, 255), D3DCOLOR_XRGB(255, 255, 255)}};
    for (int row = 0; row < 2; row++)
      memcpy((char *)locked.pBits + (size_t)row * locked.Pitch, texels[row], sizeof(texels[row]));
    CHECK(texture->UnlockRect(0));

    CHECK(device->SetTexture(0, texture.Get()));
    CHECK(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
    CHECK(device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT));
    CHECK(device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
    CHECK(device->SetFVF(kXYZTexturedFVF));
    CHECK(begin(identity_matrix, identity_matrix, identity_matrix));
    // the whole texture across the frame as pixels 0..64, which is what the identity transform makes of it
    const XYZTexturedVertex quad[4] = {{ndc_x(0.0f), ndc_y(0.0f), 0.5f, 0xFFFFFFFF, 0.0f, 0.0f},
                                       {ndc_x(64.0f), ndc_y(0.0f), 0.5f, 0xFFFFFFFF, 1.0f, 0.0f},
                                       {ndc_x(0.0f), ndc_y(64.0f), 0.5f, 0xFFFFFFFF, 0.0f, 1.0f},
                                       {ndc_x(64.0f), ndc_y(64.0f), 0.5f, 0xFFFFFFFF, 1.0f, 1.0f}};
    CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(XYZTexturedVertex)));
    CHECK(end());
    expect(read_frame(), "the frame with the transformed textured quad could not be read");
    CHECK(show());
    expect(pixel(16, 16) == rgb(texels[0][0]),
           "a transformed vertex's texture coordinates do not reach the stage: the red texel is not where they name it");
    expect(pixel(48, 16) == rgb(texels[0][1]), "a transformed textured draw did not sample the texture's green texel");
    expect(pixel(16, 48) == rgb(texels[1][0]), "a transformed textured draw did not sample the texture's blue texel");
    expect(pixel(48, 48) == rgb(texels[1][1]),
           "a transformed textured draw did not sample the texture's white texel");
    CHECK(device->SetTexture(0, nullptr));
    CHECK(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE));
  }

  // ---- user clip planes --------------------------------------------------------------------------------
  // a fixed-function plane is in world space: under a view that moves everything 0.5 right, the plane x >= 0 keeps the
  // world's right half, which is the screen from pixel 48 on, not the screen's right half from pixel 32
  {
    const float plane[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    CHECK(device->SetClipPlane(0, plane));
    CHECK(device->SetRenderState(D3DRS_CLIPPLANEENABLE, D3DCLIPPLANE0));
    CHECK(device->SetFVF(kXYZFVF));
    CHECK(begin(identity_matrix, identity_matrix, identity_matrix));
    CHECK(draw_quad(8.0f, 8.0f, 56.0f, 56.0f, 0.5f, 0.5f, kColour));
    CHECK(end());
    expect(read_frame(), "the frame with a clip plane could not be read");
    CHECK(show());
    expect(pixel(16, 32) == rgb(kClear), "the clip plane x >= 0 left the left half of the quad drawn");
    expect(pixel(48, 32) == rgb(kColour), "the clip plane x >= 0 clipped the right half of the quad");

    CHECK(begin(identity_matrix, translation(0.5f, 0.0f, 0.0f), identity_matrix));
    CHECK(draw_quad(8.0f, 8.0f, 56.0f, 56.0f, 0.5f, 0.5f, kColour));
    CHECK(end());
    expect(read_frame(), "the frame with a clip plane under a view could not be read");
    CHECK(show());
    expect(pixel(40, 32) == rgb(kClear), "the clip plane was taken in clip space rather than world space");
    expect(pixel(56, 32) == rgb(kColour), "the clip plane in world space clipped what is on its side");

    // a screen-space vertex is past the transform stage, and no plane clips it
    CHECK(device->SetFVF(kXYZRHWFVF));
    CHECK(begin(identity_matrix, identity_matrix, identity_matrix));
    const XYZRHWVertex screen[4] = {{8.0f, 8.0f, 0.5f, 1.0f, kColour},
                                    {56.0f, 8.0f, 0.5f, 1.0f, kColour},
                                    {8.0f, 56.0f, 0.5f, 1.0f, kColour},
                                    {56.0f, 56.0f, 0.5f, 1.0f, kColour}};
    CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, screen, sizeof(XYZRHWVertex)));
    CHECK(end());
    expect(read_frame(), "the screen-space frame with a clip plane could not be read");
    CHECK(show());
    expect(pixel(16, 32) == rgb(kColour), "a clip plane clipped a D3DFVF_XYZRHW draw");

    // with the plane disabled the whole quad is drawn again
    CHECK(device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0));
    CHECK(device->SetFVF(kXYZFVF));
    CHECK(begin(identity_matrix, identity_matrix, identity_matrix));
    CHECK(draw_quad(8.0f, 8.0f, 56.0f, 56.0f, 0.5f, 0.5f, kColour));
    CHECK(end());
    expect(read_frame(), "the frame with the clip plane disabled could not be read");
    CHECK(show());
    expect(pixel(16, 32) == rgb(kColour), "a disabled clip plane still clipped");

    float got[4] = {};
    CHECK(device->GetClipPlane(0, got));
    expect(memcmp(got, plane, sizeof(got)) == 0, "GetClipPlane did not give back the plane SetClipPlane was given");
    // Wine clamps an index past MaxUserClipPlanes onto the last plane instead of refusing it (winetest
    // test_user_clip_planes), so a seventh SetClipPlane lands on slot 5
    expect(device->SetClipPlane(6, plane) == D3D_OK, "SetClipPlane refused a seventh plane");
    expect(device->SetClipPlane(0, nullptr) == D3DERR_INVALIDCALL, "SetClipPlane took a null plane");
    expect(device->GetClipPlane(0, nullptr) == D3DERR_INVALIDCALL, "GetClipPlane took a null output");
    D3DCAPS9 caps{};
    CHECK(device->GetDeviceCaps(&caps));
    expect(caps.MaxUserClipPlanes == 6, "the caps do not offer the six user clip planes");

    // an all-state block carries the planes and the enable
    ComPtr<IDirect3DStateBlock9> block;
    CHECK(device->CreateStateBlock(D3DSBT_ALL, &block));
    const float other[4] = {0.0f, 1.0f, 0.0f, 0.25f};
    CHECK(device->SetClipPlane(0, other));
    CHECK(device->SetRenderState(D3DRS_CLIPPLANEENABLE, D3DCLIPPLANE0 | D3DCLIPPLANE3));
    CHECK(block->Apply());
    CHECK(device->GetClipPlane(0, got));
    expect(memcmp(got, plane, sizeof(got)) == 0, "an all-state block did not put the clip plane it captured back");
    DWORD enabled = 1;
    CHECK(device->GetRenderState(D3DRS_CLIPPLANEENABLE, &enabled));
    expect(enabled == 0, "an all-state block did not put D3DRS_CLIPPLANEENABLE back");
  }

  // ---- ProcessVertices ---------------------------------------------------------------------------------
  // the CPU's transform writes the screen-space vertices a draw would rasterize: a quad from 8..56 under a world
  // translation of 0.25 (8 pixels) is processed into a D3DFVF_XYZRHW buffer and drawn from it with identity transforms
  {
    const XYZVertex model[4] = {{ndc_x(8.0f), ndc_y(8.0f), 0.5f, kColour},
                                {ndc_x(56.0f), ndc_y(8.0f), 0.5f, kColour},
                                {ndc_x(8.0f), ndc_y(56.0f), 0.5f, kColour},
                                {ndc_x(56.0f), ndc_y(56.0f), 0.5f, kColour}};
    ComPtr<IDirect3DVertexBuffer9> source, processed;
    CHECK(device->CreateVertexBuffer(sizeof(model), 0, kXYZFVF, D3DPOOL_MANAGED, &source, nullptr));
    void *bytes = nullptr;
    CHECK(source->Lock(0, 0, &bytes, 0));
    memcpy(bytes, model, sizeof(model));
    CHECK(source->Unlock());
    CHECK(device->CreateVertexBuffer(4 * sizeof(XYZRHWVertex), D3DUSAGE_SOFTWAREPROCESSING, kXYZRHWFVF,
                                     D3DPOOL_MANAGED, &processed, nullptr));

    CHECK(device->SetFVF(kXYZFVF));
    CHECK(device->SetStreamSource(0, source.Get(), 0, sizeof(XYZVertex)));
    CHECK(begin(translation(0.25f, 0.0f, 0.0f), identity_matrix, identity_matrix));
    CHECK(end());
    CHECK(device->ProcessVertices(0, 0, 4, processed.Get(), nullptr, 0));
    CHECK(processed->Lock(0, 0, &bytes, D3DLOCK_READONLY));
    XYZRHWVertex written[4];
    memcpy(written, bytes, sizeof(written));
    CHECK(processed->Unlock());
    auto close_to = [](float a, float b) { return fabsf(a - b) < 1e-3f; };
    expect(close_to(written[0].x, 16.0f) && close_to(written[0].y, 8.0f), "ProcessVertices put the first vertex elsewhere");
    expect(close_to(written[3].x, 64.0f) && close_to(written[3].y, 56.0f), "ProcessVertices put the last vertex elsewhere");
    expect(close_to(written[0].z, 0.5f) && close_to(written[0].rhw, 1.0f), "ProcessVertices wrote the wrong depth or rhw");
    expect(written[2].colour == kColour, "ProcessVertices did not copy an unlit vertex's colour");

    CHECK(device->SetFVF(kXYZRHWFVF));
    CHECK(device->SetStreamSource(0, processed.Get(), 0, sizeof(XYZRHWVertex)));
    CHECK(begin(identity_matrix, identity_matrix, identity_matrix));
    CHECK(device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2));
    CHECK(end());
    expect(read_frame(), "the frame drawn from processed vertices could not be read");
    CHECK(show());
    expect(pixel(12, 32) == rgb(kClear), "the processed quad was not moved by the world translation");
    expect(pixel(20, 32) == rgb(kColour), "the processed quad did not draw where its transform took it");
    expect(pixel(60, 32) == rgb(kColour), "the processed quad did not reach the frame's right edge");

    // a viewport is applied by the processing: half the frame's width, from x = 32
    D3DVIEWPORT9 right_half{32, 0, client / 2, client, 0.25f, 0.75f};
    CHECK(device->SetFVF(kXYZFVF));
    CHECK(device->SetStreamSource(0, source.Get(), 0, sizeof(XYZVertex)));
    CHECK(device->SetTransform(D3DTS_WORLD, &identity_matrix));
    CHECK(device->SetViewport(&right_half));
    CHECK(device->ProcessVertices(0, 0, 4, processed.Get(), nullptr, 0));
    CHECK(processed->Lock(0, 0, &bytes, D3DLOCK_READONLY));
    memcpy(written, bytes, sizeof(written));
    CHECK(processed->Unlock());
    expect(close_to(written[0].x, 36.0f) && close_to(written[1].x, 60.0f), "ProcessVertices did not map x through the viewport");
    expect(close_to(written[0].z, 0.5f), "ProcessVertices did not map z into the viewport's MinZ..MaxZ");

    // D3DPV_DONOTCOPYDATA leaves what the processing did not make: an unlit vertex's colour
    CHECK(processed->Lock(0, 0, &bytes, 0));
    for (UINT at = 0; at < 4; at++)
      static_cast<XYZRHWVertex *>(bytes)[at].colour = kOther;
    CHECK(processed->Unlock());
    CHECK(device->ProcessVertices(0, 0, 4, processed.Get(), nullptr, D3DPV_DONOTCOPYDATA));
    CHECK(processed->Lock(0, 0, &bytes, D3DLOCK_READONLY));
    memcpy(written, bytes, sizeof(written));
    CHECK(processed->Unlock());
    expect(written[1].colour == kOther, "D3DPV_DONOTCOPYDATA copied the vertex colour");

    // a destination declaration names where the position and colour go: the colour first, then the position
    const D3DVERTEXELEMENT9 swapped[] = {
        {0, 0, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 4, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITIONT, 0},
        D3DDECL_END(),
    };
    ComPtr<IDirect3DVertexDeclaration9> declaration;
    CHECK(device->CreateVertexDeclaration(swapped, &declaration));
    ComPtr<IDirect3DVertexBuffer9> declared;
    CHECK(device->CreateVertexBuffer(4 * 20, 0, 0, D3DPOOL_MANAGED, &declared, nullptr));
    CHECK(device->ProcessVertices(1, 0, 3, declared.Get(), declaration.Get(), 0));
    CHECK(declared->Lock(0, 0, &bytes, D3DLOCK_READONLY));
    D3DCOLOR declared_colour = 0;
    float declared_position[4] = {};
    memcpy(&declared_colour, bytes, sizeof(declared_colour));
    memcpy(declared_position, static_cast<char *>(bytes) + 4, sizeof(declared_position));
    CHECK(declared->Unlock());
    expect(declared_colour == kColour && close_to(declared_position[0], 60.0f) && close_to(declared_position[3], 1.0f),
           "ProcessVertices did not write the layout the destination declaration names, from the source start");

    // what it refuses: a count past the source, a destination too small, and a destination without a screen position
    expect(device->ProcessVertices(2, 0, 4, processed.Get(), nullptr, 0) == D3DERR_INVALIDCALL,
           "ProcessVertices read past the end of its source");
    expect(device->ProcessVertices(0, 2, 4, processed.Get(), nullptr, 0) == D3DERR_INVALIDCALL,
           "ProcessVertices wrote past the end of its destination");
    expect(device->ProcessVertices(0, 0, 4, source.Get(), nullptr, 0) == D3DERR_INVALIDCALL,
           "ProcessVertices wrote into a buffer whose FVF has no D3DFVF_XYZRHW");
  }

  // a lit vertex's processed colour is the colour the lit draw gives it: no lights, an ambient of 0,128,255 and a
  // material whose ambient reflects all of it, drawn once through the pipeline and once from the processed buffer
  {
    struct LitVertex {
      float x, y, z;
      float nx, ny, nz;
      D3DCOLOR colour;
    };
    const DWORD lit_fvf = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE;
    const LitVertex model[4] = {{ndc_x(8.0f), ndc_y(8.0f), 0.5f, 0.0f, 0.0f, -1.0f, kColour},
                                {ndc_x(56.0f), ndc_y(8.0f), 0.5f, 0.0f, 0.0f, -1.0f, kColour},
                                {ndc_x(8.0f), ndc_y(56.0f), 0.5f, 0.0f, 0.0f, -1.0f, kColour},
                                {ndc_x(56.0f), ndc_y(56.0f), 0.5f, 0.0f, 0.0f, -1.0f, kColour}};
    D3DMATERIAL9 material{};
    material.Ambient = {1.0f, 1.0f, 1.0f, 1.0f};
    CHECK(device->SetMaterial(&material));
    CHECK(device->SetRenderState(D3DRS_LIGHTING, TRUE));
    CHECK(device->SetRenderState(D3DRS_AMBIENT, D3DCOLOR_XRGB(0, 128, 255)));
    CHECK(device->SetFVF(lit_fvf));
    CHECK(begin(identity_matrix, identity_matrix, identity_matrix));
    CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, model, sizeof(LitVertex)));
    CHECK(end());
    expect(read_frame(), "the lit frame could not be read");
    CHECK(show());
    const D3DCOLOR drawn = pixel(32, 32);

    ComPtr<IDirect3DVertexBuffer9> source, processed;
    CHECK(device->CreateVertexBuffer(sizeof(model), 0, lit_fvf, D3DPOOL_MANAGED, &source, nullptr));
    void *bytes = nullptr;
    CHECK(source->Lock(0, 0, &bytes, 0));
    memcpy(bytes, model, sizeof(model));
    CHECK(source->Unlock());
    CHECK(device->CreateVertexBuffer(4 * sizeof(XYZRHWVertex), 0, kXYZRHWFVF, D3DPOOL_MANAGED, &processed, nullptr));
    CHECK(device->SetStreamSource(0, source.Get(), 0, sizeof(LitVertex)));
    CHECK(device->ProcessVertices(0, 0, 4, processed.Get(), nullptr, D3DPV_DONOTCOPYDATA));
    CHECK(processed->Lock(0, 0, &bytes, D3DLOCK_READONLY));
    const D3DCOLOR lit = static_cast<XYZRHWVertex *>(bytes)[0].colour;
    CHECK(processed->Unlock());
    expect(rgb(lit) == rgb(D3DCOLOR_XRGB(0, 128, 255)), "ProcessVertices did not light the vertex as D3D9 lights it");
    expect(rgb(lit) == drawn, "ProcessVertices lit the vertex differently from the lit draw");

    CHECK(device->SetRenderState(D3DRS_LIGHTING, FALSE));
    CHECK(device->SetFVF(kXYZRHWFVF));
    CHECK(device->SetStreamSource(0, processed.Get(), 0, sizeof(XYZRHWVertex)));
    CHECK(begin(identity_matrix, identity_matrix, identity_matrix));
    CHECK(device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2));
    CHECK(end());
    expect(read_frame(), "the frame of processed lit vertices could not be read");
    CHECK(show());
    expect(pixel(32, 32) == drawn, "the processed lit quad does not draw the colour the lit draw drew");
    CHECK(device->SetStreamSource(0, nullptr, 0, 0));
  }

  // ---- the transform state itself ----------------------------------------------------------------------
  {
    D3DMATRIX got{};
    CHECK(device->GetTransform(D3DTS_WORLD, &got));
    const D3DMATRIX moved = translation(0.25f, 0.5f, 0.75f);
    CHECK(device->SetTransform(D3DTS_WORLD, &moved));
    CHECK(device->GetTransform(D3DTS_WORLD, &got));
    expect(memcmp(&got, &moved, sizeof(got)) == 0, "GetTransform did not give back the matrix SetTransform was given");
    expect(device->SetTransform(D3DTS_WORLD, nullptr) == D3DERR_INVALIDCALL, "SetTransform took a null matrix");
    expect(device->GetTransform(D3DTS_WORLD, nullptr) == D3DERR_INVALIDCALL, "GetTransform took a null output");
    expect(SUCCEEDED(device->SetTransform((D3DTRANSFORMSTATETYPE)D3DTS_TEXTURE0, &moved)),
           "a texture coordinate transform was refused");
    CHECK(device->GetTransform((D3DTRANSFORMSTATETYPE)D3DTS_TEXTURE0, &got));
    expect(memcmp(&got, &moved, sizeof(got)) == 0, "GetTransform(D3DTS_TEXTURE0) did not give the matrix back");
    // MultiplyTransform multiplies the matrix the device holds by the one it is given, the device's first, as D3D9's
    // does: a half scale then a translation of 0.5 is v * S * T, whose x' is x * 0.5 + 0.5 (the other order would
    // give 0.5 * (x + 0.5) = x * 0.5 + 0.25). The two differ by 0.25 in NDC, four pixels, so the geometry's right edge
    // says which one the device composed
    {
      D3DMATRIX half = identity_matrix;
      half.m[0][0] = 0.5f;
      half.m[1][1] = 0.5f;
      const D3DMATRIX move = translation(0.5f, 0.0f, 0.0f);
      CHECK(device->SetTransform(D3DTS_WORLD, &half));
      CHECK(device->MultiplyTransform(D3DTS_WORLD, &move));
      CHECK(device->GetTransform(D3DTS_WORLD, &got));
      D3DMATRIX expected{};
      for (int row = 0; row < 4; row++)
        for (int column = 0; column < 4; column++)
          for (int at = 0; at < 4; at++)
            expected.m[row][column] += half.m[row][at] * move.m[at][column];
      expect(memcmp(&got, &expected, sizeof(got)) == 0,
             "MultiplyTransform did not multiply the device's matrix by the one it was given");

      CHECK(device->SetFVF(kXYZFVF));
      CHECK(begin(half, identity_matrix, identity_matrix));
      // begin sets the world it is given, so the composition is made again over it
      CHECK(device->MultiplyTransform(D3DTS_WORLD, &move));
      // x * 0.5 + 0.5 takes the quad's 8..56 (NDC -0.75..0.75) to NDC 0.125..0.875, pixels 36..60
      CHECK(draw_quad(8.0f, 8.0f, 56.0f, 56.0f, 0.5f, 0.5f, kColour));
      CHECK(end());
      expect(read_frame(), "the frame under a multiplied transform could not be read");
      CHECK(show());
      expect(pixel(58, 32) == rgb(kColour),
             "a transform multiplied by a translation did not put the geometry where the product takes it");
      expect(pixel(30, 32) == rgb(kClear),
             "MultiplyTransform multiplied in the other order: the geometry is where that order would put it");
    }
    // and Reset puts the three back to identity
    D3DPRESENT_PARAMETERS present = window_frame.present();
    CHECK(device->Reset(&present));
    // Reset restores D3D9's default lighting on; these draws are about the transform, not lighting, and a lit
    // XYZ | DIFFUSE vertex with no material ambient or emissive would be black
    CHECK(device->SetRenderState(D3DRS_LIGHTING, FALSE));
    CHECK(device->GetTransform(D3DTS_WORLD, &got));
    const D3DMATRIX reset_identity = identity();
    expect(memcmp(&got, &reset_identity, sizeof(got)) == 0, "Reset did not put the world matrix back to identity");
    CHECK(device->GetTransform(D3DTS_VIEW, &got));
    expect(memcmp(&got, &reset_identity, sizeof(got)) == 0, "Reset did not put the view matrix back to identity");
    CHECK(device->GetTransform(D3DTS_PROJECTION, &got));
    expect(memcmp(&got, &reset_identity, sizeof(got)) == 0, "Reset did not put the projection matrix back to identity");
  }

  // the matrices a state block carries: an all-state block puts a world matrix back, a pixel-state block does not
  {
    ComPtr<IDirect3DStateBlock9> block;
    CHECK(device->CreateStateBlock(D3DSBT_ALL, &block));
    CHECK(block->Capture());
    const D3DMATRIX moved = translation(0.5f, 0.25f, 0.0f);
    CHECK(device->SetTransform(D3DTS_WORLD, &moved));
    CHECK(block->Apply());
    D3DMATRIX got{};
    CHECK(device->GetTransform(D3DTS_WORLD, &got));
    const D3DMATRIX identity_matrix2 = identity();
    expect(memcmp(&got, &identity_matrix2, sizeof(got)) == 0,
           "an all-state block did not put the world matrix it captured back");
  }

  // vertex blending: one weight, so a weight of 1 is WORLD and a weight of 0 is WORLD1 (Microsoft Learn,
  // D3DRS_VERTEXBLEND). tweening is the same two matrices on a vertex that carries no weight
  {
    D3DCAPS9 caps{};
    CHECK(device->GetDeviceCaps(&caps));
    expect(caps.MaxVertexBlendMatrices == 4, "MaxVertexBlendMatrices is not 4");
    expect(caps.MaxVertexBlendMatrixIndex == 0, "MaxVertexBlendMatrixIndex is not 0");
    D3DMATRIX got{};
    const D3DMATRIX shifted = translation(2.0f, 0.0f, 0.0f);
    expect(SUCCEEDED(device->SetTransform(D3DTS_WORLD1, &shifted)), "SetTransform(D3DTS_WORLD1) was refused");
    CHECK(device->GetTransform(D3DTS_WORLD1, &got));
    expect(memcmp(&got, &shifted, sizeof(got)) == 0, "GetTransform(D3DTS_WORLD1) did not give the matrix back");
    expect(SUCCEEDED(device->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_1WEIGHTS)), "D3DRS_VERTEXBLEND was refused");

    struct BlendVertex {
      float x, y, z;
      float weight;
      D3DCOLOR colour;
    };
    CHECK(device->SetFVF(D3DFVF_XYZB1 | D3DFVF_DIFFUSE));
    auto blend_quad = [&](float weight) -> HRESULT {
      const BlendVertex vertices[4] = {
          {ndc_x(16), ndc_y(16), 0, weight, kColour},
          {ndc_x(48), ndc_y(16), 0, weight, kColour},
          {ndc_x(16), ndc_y(48), 0, weight, kColour},
          {ndc_x(48), ndc_y(48), 0, weight, kColour},
      };
      return device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(BlendVertex));
    };
    CHECK(begin(identity_matrix, identity_matrix, identity_matrix));
    CHECK(device->SetTransform(D3DTS_WORLD1, &shifted));
    CHECK(blend_quad(1.0f));
    CHECK(end());
    expect(read_frame(), "the weight 1 frame could not be read");
    CHECK(show());
    expect(pixel(32, 32) == rgb(kColour), "a blend weight of 1 did not draw with D3DTS_WORLD");
    CHECK(begin(identity_matrix, identity_matrix, identity_matrix));
    CHECK(device->SetTransform(D3DTS_WORLD1, &shifted));
    CHECK(blend_quad(0.0f));
    CHECK(end());
    expect(read_frame(), "the weight 0 frame could not be read");
    CHECK(show());
    expect(pixel(32, 32) == rgb(kClear), "a blend weight of 0 did not draw with D3DTS_WORLD1");

    CHECK(device->SetFVF(kXYZFVF));
    CHECK(device->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_TWEENING));
    const float none = 0.0f, all = 1.0f;
    CHECK(device->SetRenderState(D3DRS_TWEENFACTOR, *reinterpret_cast<const DWORD *>(&none)));
    CHECK(begin(identity_matrix, identity_matrix, identity_matrix));
    CHECK(device->SetTransform(D3DTS_WORLD1, &shifted));
    CHECK(draw_quad(16, 16, 48, 48, 0, 0, kColour));
    CHECK(end());
    expect(read_frame(), "the tween 0 frame could not be read");
    CHECK(show());
    expect(pixel(32, 32) == rgb(kColour), "a tween factor of 0 did not draw with D3DTS_WORLD");
    CHECK(device->SetRenderState(D3DRS_TWEENFACTOR, *reinterpret_cast<const DWORD *>(&all)));
    CHECK(begin(identity_matrix, identity_matrix, identity_matrix));
    CHECK(device->SetTransform(D3DTS_WORLD1, &shifted));
    CHECK(draw_quad(16, 16, 48, 48, 0, 0, kColour));
    CHECK(end());
    expect(read_frame(), "the tween 1 frame could not be read");
    CHECK(show());
    expect(pixel(32, 32) == rgb(kClear), "a tween factor of 1 did not draw with D3DTS_WORLD1");

    // three weights and no diffuse colour (D3DFVF_XYZB3, Wine visual.c test_vertex_blending): the last weight is 1
    // minus the three, so (1,0,0) draws with WORLD0 and (0,0,0) hands the whole vertex to WORLD3. the vertex has no
    // colour channel, so the drawn white is D3D9's default and a blend shader that required one would refuse it
    {
      struct ThreeWeights {
        float x, y, z, w0, w1, w2;
      };
      CHECK(device->SetFVF(D3DFVF_XYZB3));
      CHECK(device->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_3WEIGHTS));
      const D3DMATRIX right = translation(0.75f, 0.0f, 0.0f);
      auto draw_three = [&](float w0) -> HRESULT {
        const ThreeWeights vertices[4] = {
            {ndc_x(16), ndc_y(16), 0, w0, 0, 0},
            {ndc_x(48), ndc_y(16), 0, w0, 0, 0},
            {ndc_x(16), ndc_y(48), 0, w0, 0, 0},
            {ndc_x(48), ndc_y(48), 0, w0, 0, 0},
        };
        return device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(ThreeWeights));
      };
      CHECK(device->SetTransform(D3DTS_WORLDMATRIX(0), &identity_matrix));
      CHECK(device->SetTransform(D3DTS_WORLD3, &right));
      CHECK(begin(identity_matrix, identity_matrix, identity_matrix));
      const HRESULT three = draw_three(1.0f);
      CHECK(end());
      expect(SUCCEEDED(three), "a three-weight draw with no diffuse colour was refused");
      expect(read_frame(), "the three-weight frame could not be read");
      CHECK(show());
      expect(pixel(32, 32) == rgb(D3DCOLOR_XRGB(255, 255, 255)),
             "a three-weight draw with (1,0,0) did not draw white with WORLD0");
      CHECK(begin(identity_matrix, identity_matrix, identity_matrix));
      CHECK(device->SetTransform(D3DTS_WORLD3, &right));
      CHECK(draw_three(0.0f));
      CHECK(end());
      expect(read_frame(), "the WORLD3 three-weight frame could not be read");
      CHECK(show());
      expect(pixel(32, 32) == rgb(kClear), "all-zero three weights did not hand the vertex to WORLD3");
      expect(pixel(48, 32) == rgb(D3DCOLOR_XRGB(255, 255, 255)),
             "the WORLD3-shifted three-weight quad did not cover the right side");

      // the same three weights packed into a D3DCOLOR declaration element (Wine's D3DCOLOR case): r, g and b are
      // w0, w1 and w2, and there is no FVF that spells this declaration
      const D3DVERTEXELEMENT9 packed_elements[] = {
          {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
          {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDWEIGHT, 0},
          D3DDECL_END(),
      };
      ComPtr<IDirect3DVertexDeclaration9> packed_declaration;
      CHECK(device->CreateVertexDeclaration(packed_elements, &packed_declaration));
      CHECK(device->SetVertexDeclaration(packed_declaration.Get()));
      struct PackedVertex {
        float x, y, z;
        D3DCOLOR weights;
      };
      auto draw_packed = [&](D3DCOLOR weights) -> HRESULT {
        const PackedVertex vertices[4] = {
            {ndc_x(16), ndc_y(16), 0, weights},
            {ndc_x(48), ndc_y(16), 0, weights},
            {ndc_x(16), ndc_y(48), 0, weights},
            {ndc_x(48), ndc_y(48), 0, weights},
        };
        return device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(PackedVertex));
      };
      CHECK(device->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_3WEIGHTS));
      CHECK(device->SetTransform(D3DTS_WORLDMATRIX(0), &identity_matrix));
      CHECK(device->SetTransform(D3DTS_WORLD3, &right));
      CHECK(begin(identity_matrix, identity_matrix, identity_matrix));
      const HRESULT packed_draw = draw_packed(D3DCOLOR_COLORVALUE(1.0f, 0.0f, 0.0f, 0.0f));
      CHECK(end());
      expect(SUCCEEDED(packed_draw), "a packed D3DCOLOR three-weight draw was refused");
      expect(read_frame(), "the packed three-weight frame could not be read");
      CHECK(show());
      expect(pixel(32, 32) == rgb(D3DCOLOR_XRGB(255, 255, 255)),
             "packed weights (1,0,0) did not draw white with WORLD0");
      CHECK(begin(identity_matrix, identity_matrix, identity_matrix));
      CHECK(device->SetTransform(D3DTS_WORLD3, &right));
      CHECK(draw_packed(0));
      CHECK(end());
      expect(read_frame(), "the packed WORLD3 frame could not be read");
      CHECK(show());
      expect(pixel(32, 32) == rgb(kClear), "packed zero weights did not hand the vertex to WORLD3");
      CHECK(device->SetFVF(kXYZFVF));
      CHECK(device->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_DISABLE));
    }
  }

  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
