// contract: Direct3D 9's fixed-function lighting for a D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE vertex. A lit
// vertex's colour is the device's global ambient (D3DRS_AMBIENT), the material's emissive, and every enabled light's
// contribution through the material's ambient, diffuse and specular reflectances (Microsoft Learn, "Mathematics of
// Direct3D Lighting", D3DLIGHT9, D3DMATERIAL9, D3DRS_LIGHTING):
//
//   colour = material.Ambient * (D3DRS_AMBIENT + sum of every enabled light's Ambient) + material.Emissive
//          + sum over enabled lights of
//              material.Diffuse  * light.Diffuse  * max(0, n . L) * attenuation
//            + material.Specular * light.Specular * max(0, n . H)^power * attenuation   (D3DRS_SPECULARENABLE)
//
// with attenuation 1 / (Attenuation0 + Attenuation1 * d + Attenuation2 * d * d) for a D3DLIGHT_POINT, nothing past the
// light's Range, and 1 for a D3DLIGHT_DIRECTIONAL. L points from the vertex to the light: a point light's Position less
// the vertex, or the negation of a directional light's Direction. H is the half vector between L and the eye.
// https://learn.microsoft.com/en-us/windows/win32/direct3d9/mathematics-of-direct3d-lighting
// https://learn.microsoft.com/en-us/windows/win32/api/d3d9helper/nf-d3d9helper-idirect3ddevice9-setlight
// https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dlight9
//
// The lighting is per vertex: a quad's four corners are lit and the rasterizer interpolates between them, which is what
// D3D9's fixed-function pipeline does too (D3D9 has no per-pixel fixed-function lighting). A check that reads one pixel
// therefore draws a small patch around it, so the four corners a pixel interpolates from are all within a fraction of a
// world unit of it, rather than across the whole frame.
//
// With D3DRS_LIGHTING FALSE the vertex's own diffuse colour is the colour. A light index past the device's
// MaxActiveLights is D3DERR_INVALIDCALL for SetLight, GetLight, LightEnable and GetLightEnable, and a vertex-state block
// carries the lights, their enables and the material.
//
// The checks, each a frame drawn on a 64 by 64 back buffer read back before Present:
// - the state D3D9 starts with, light count included.
// - lighting off draws the vertex colour whatever the lights say.
// - lighting on: the global ambient alone, the directional light's diffuse alone, and the two together, each the colour
//   the equation above gives.
// - a directional light pointing away from the face contributes nothing.
// - a light turned 90 degrees lights the face whose normal points at it and not the other.
// - a point light's attenuation is 1/(A0 + A1 d + A2 d^2) at two distances, its Range stops it, and each attenuation
//   term alone behaves as documented.
// - the material's emissive is added whatever the lights do.
// - the specular term is there only while D3DRS_SPECULARENABLE is TRUE.
// - LightEnable turns a light off and on and GetLightEnable reports it.
// - two of the eight lights add.
// - D3DRS_COLORVERTEX with D3DRS_DIFFUSEMATERIALSOURCE D3DMCS_COLOR1 takes the material's diffuse from the vertex.
// - the calls round-trip, refuse a null pointer and refuse an index past MaxActiveLights.
// - a vertex-state block puts the lights, their enables and the material back, and the frame drawn afterwards is the
//   light the block captured.
#include "d3d9_frame.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

// a vertex that carries a normal, which is what a lit draw reads: D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE
struct LitVertex {
  float x, y, z;
  float nx, ny, nz;
  D3DCOLOR colour;
};

static const DWORD kLitFVF = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE;
using d3d9_test::identity;
using d3d9_test::ndc_x;
using d3d9_test::ndc_y;

static D3DCOLORVALUE
colour_of(D3DCOLOR colour) {
  D3DCOLORVALUE value{};
  value.r = ((colour >> 16) & 0xFF) / 255.0f;
  value.g = ((colour >> 8) & 0xFF) / 255.0f;
  value.b = (colour & 0xFF) / 255.0f;
  value.a = ((colour >> 24) & 0xFF) / 255.0f;
  return value;
}

static D3DLIGHT9
directional(float x, float y, float z, D3DCOLOR diffuse, D3DCOLOR ambient = 0, D3DCOLOR specular = 0) {
  D3DLIGHT9 light{};
  light.Type = D3DLIGHT_DIRECTIONAL;
  light.Direction = {x, y, z};
  light.Diffuse = colour_of(diffuse);
  light.Ambient = colour_of(ambient);
  light.Specular = colour_of(specular);
  return light;
}

static D3DLIGHT9
point(float x, float y, float z, float range, float a0, float a1, float a2, D3DCOLOR diffuse) {
  D3DLIGHT9 light{};
  light.Type = D3DLIGHT_POINT;
  light.Position = {x, y, z};
  light.Range = range;
  light.Attenuation0 = a0;
  light.Attenuation1 = a1;
  light.Attenuation2 = a2;
  light.Diffuse = colour_of(diffuse);
  return light;
}

static D3DMATERIAL9
material(D3DCOLOR diffuse, D3DCOLOR ambient = 0, D3DCOLOR specular = 0, D3DCOLOR emissive = 0, float power = 0.0f) {
  D3DMATERIAL9 value{};
  value.Diffuse = colour_of(diffuse);
  value.Ambient = colour_of(ambient);
  value.Specular = colour_of(specular);
  value.Emissive = colour_of(emissive);
  value.Power = power;
  return value;
}

int
main() {
  unsigned wrong = 0, checks = 0;
  auto expect = [&](bool ok, const char *what) {
    checks++;
    if (!ok && wrong++ < 20)
      printf("wrong: %s\n", what);
  };

  const UINT client = d3d9_test::kSize;
  d3d9_test::Frame window_frame;
  if (!window_frame.create("d3d9_lighting"))
    return 1;
  IDirect3DDevice9 *device = window_frame.device();
  auto read_frame = [&]() { return window_frame.read(); };
  auto pixel = [&](UINT x, UINT y) -> D3DCOLOR { return window_frame.pixel(x, y); };
  const D3DCOLOR kClear = D3DCOLOR_XRGB(1, 2, 3);

  // a colour the frame should hold, within `tolerance` of each channel: the shader's arithmetic is float and the frame is
  // eight bits a channel, so an exact match is not what the equation's answer is
  auto expect_rgb = [&](UINT x, UINT y, int r, int g, int b, int tolerance, const char *what) {
    const D3DCOLOR got = pixel(x, y);
    const int got_r = (int)((got >> 16) & 0xFF), got_g = (int)((got >> 8) & 0xFF), got_b = (int)(got & 0xFF);
    const bool close = std::abs(got_r - r) <= tolerance && std::abs(got_g - g) <= tolerance &&
                       std::abs(got_b - b) <= tolerance;
    checks++;
    if (!close && wrong++ < 20)
      printf("wrong: %s (read %d,%d,%d at %u,%u; the equation says %d,%d,%d)\n", what, got_r, got_g, got_b, x, y, r, g,
             b);
  };

  // the frame every check draws: culling off, the whole viewport, the identity transforms, a cleared target and the lit
  // vertex format, with the scene begun
  auto begin = [&]() -> HRESULT {
    HRESULT hr = device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    if (FAILED(hr))
      return hr;
    D3DVIEWPORT9 viewport{0, 0, client, client, 0.0f, 1.0f};
    if (FAILED(hr = device->SetViewport(&viewport)))
      return hr;
    D3DMATRIX identity_matrix = identity();
    if (FAILED(hr = device->SetTransform(D3DTS_WORLD, &identity_matrix)) ||
        FAILED(hr = device->SetTransform(D3DTS_VIEW, &identity_matrix)) ||
        FAILED(hr = device->SetTransform(D3DTS_PROJECTION, &identity_matrix)))
      return hr;
    if (FAILED(hr = device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, kClear, 1.0f, 0)))
      return hr;
    if (FAILED(hr = device->SetFVF(kLitFVF)))
      return hr;
    return device->BeginScene();
  };
  auto set_light = [&](DWORD index, D3DLIGHT9 light) -> HRESULT { return device->SetLight(index, &light); };
  auto set_material = [&](D3DCOLOR diffuse, D3DCOLOR ambient = 0, D3DCOLOR specular = 0, D3DCOLOR emissive = 0,
                          float power = 0.0f) -> HRESULT {
    const D3DMATERIAL9 value = material(diffuse, ambient, specular, emissive, power);
    return device->SetMaterial(&value);
  };
  auto end = [&]() -> HRESULT { return device->EndScene(); };
  auto show = [&]() -> HRESULT { return device->Present(nullptr, nullptr, nullptr, nullptr); };

  // a quad of `normal` at depth `z` in the frame's pixels from `left, top` to `right, bottom`, all four vertices the
  // colour given
  auto draw_quad = [&](float left, float top, float right, float bottom, float z, float nx, float ny, float nz,
                       D3DCOLOR colour) -> HRESULT {
    const LitVertex vertices[4] = {{ndc_x(left), ndc_y(top), z, nx, ny, nz, colour},
                                   {ndc_x(right), ndc_y(top), z, nx, ny, nz, colour},
                                   {ndc_x(left), ndc_y(bottom), z, nx, ny, nz, colour},
                                   {ndc_x(right), ndc_y(bottom), z, nx, ny, nz, colour}};
    return device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(LitVertex));
  };
  // `half` pixels either side of the pixel `x, y`: the lighting is per vertex, so a check reads a pixel whose four
  // vertices are all close to it
  auto draw_patch = [&](float x, float y, float half, float z, float nx, float ny, float nz,
                        D3DCOLOR colour) -> HRESULT {
    return draw_quad(x - half, y - half, x + half, y + half, z, nx, ny, nz, colour);
  };
  // the whole frame, for a check whose lighting is the same at every vertex
  auto draw_frame = [&](float z, float nx, float ny, float nz, D3DCOLOR colour) -> HRESULT {
    return draw_quad(0.0f, 0.0f, d3d9_test::kExtent, d3d9_test::kExtent, z, nx, ny, nz, colour);
  };

  // what the device starts with, as D3DRENDERSTATETYPE documents it
  {
    DWORD value = 0xFFFFFFFF;
    expect(SUCCEEDED(device->GetRenderState(D3DRS_LIGHTING, &value)) && value == TRUE,
           "D3DRS_LIGHTING's default is not TRUE");
    value = 0xFFFFFFFF;
    expect(SUCCEEDED(device->GetRenderState(D3DRS_SPECULARENABLE, &value)) && value == FALSE,
           "D3DRS_SPECULARENABLE's default is not FALSE");
    value = 0xFFFFFFFF;
    expect(SUCCEEDED(device->GetRenderState(D3DRS_AMBIENT, &value)) && value == 0,
           "D3DRS_AMBIENT's default is not 0");
    value = 0xFFFFFFFF;
    expect(SUCCEEDED(device->GetRenderState(D3DRS_COLORVERTEX, &value)) && value == TRUE,
           "D3DRS_COLORVERTEX's default is not TRUE");
    D3DCAPS9 caps{};
    CHECK(device->GetDeviceCaps(&caps));
    expect(caps.MaxActiveLights == 8, "the device does not have D3D9's eight lights");
  }

  // ---- light 0's default settings, before any SetLight ---------------------------------------------------
  // D3D9 starts light 0 as a white directional light along +z, disabled. Wine's visual.c lighting_test enables it
  // without ever calling SetLight and expects a face whose normal is -z to be lit by it.
  {
    CHECK(device->SetRenderState(D3DRS_LIGHTING, TRUE));
    CHECK(device->SetRenderState(D3DRS_COLORVERTEX, FALSE));
    CHECK(device->SetRenderState(D3DRS_AMBIENT, 0));
    CHECK(set_material(D3DCOLOR_XRGB(255, 255, 255)));
    CHECK(device->LightEnable(0, TRUE));
    CHECK(begin());
    CHECK(draw_frame(0.5f, 0.0f, 0.0f, -1.0f, D3DCOLOR_XRGB(0, 0, 255)));
    CHECK(end());
    expect(read_frame(), "the default-light frame could not be read");
    CHECK(show());
    expect_rgb(32, 32, 255, 255, 255, 1,
               "LightEnable(0, TRUE) without SetLight did not light the face with D3D9's default white light");
    CHECK(device->LightEnable(0, FALSE));
  }

  // ---- lighting off draws the vertex colour ------------------------------------------------------------
  {
    CHECK(device->SetRenderState(D3DRS_LIGHTING, FALSE));
    CHECK(device->SetRenderState(D3DRS_AMBIENT, D3DCOLOR_XRGB(0, 255, 0)));
    CHECK(set_material(D3DCOLOR_XRGB(255, 255, 255)));
    CHECK(set_light(0, directional(0.0f, 0.0f, 1.0f, D3DCOLOR_XRGB(255, 0, 0))));
    CHECK(device->LightEnable(0, TRUE));
    CHECK(begin());
    CHECK(draw_frame(0.5f, 0.0f, 0.0f, -1.0f, D3DCOLOR_XRGB(0, 0, 255)));
    CHECK(end());
    expect(read_frame(), "the lighting-off frame could not be read");
    CHECK(show());
    expect_rgb(32, 32, 0, 0, 255, 0, "D3DRS_LIGHTING FALSE did not draw the vertex's own colour");
  }

  // ---- lighting on: global ambient, then the directional light's diffuse, then both ---------------------
  {
    CHECK(device->SetRenderState(D3DRS_LIGHTING, TRUE));
    CHECK(device->SetRenderState(D3DRS_COLORVERTEX, FALSE));
    CHECK(device->SetRenderState(D3DRS_AMBIENT, D3DCOLOR_XRGB(0, 0, 255)));
    // the ambient is multiplied by the material's ambient, so the material must reflect it
    CHECK(set_material(D3DCOLOR_XRGB(255, 255, 255), D3DCOLOR_XRGB(255, 255, 255)));

    // no light enabled: the device's ambient alone
    CHECK(device->LightEnable(0, FALSE));
    CHECK(begin());
    CHECK(draw_frame(0.5f, 0.0f, 0.0f, -1.0f, D3DCOLOR_XRGB(255, 0, 0)));
    CHECK(end());
    expect(read_frame(), "the ambient-only frame could not be read");
    CHECK(show());
    expect_rgb(32, 32, 0, 0, 255, 1, "D3DRS_AMBIENT is not the whole colour of a frame with no light enabled");

    // a directional light along -z, which is the direction the face's normal points: n . L is 1
    CHECK(device->SetRenderState(D3DRS_AMBIENT, 0));
    CHECK(set_light(0, directional(0.0f, 0.0f, 1.0f, D3DCOLOR_XRGB(255, 0, 0))));
    CHECK(device->LightEnable(0, TRUE));
    CHECK(begin());
    CHECK(draw_frame(0.5f, 0.0f, 0.0f, -1.0f, D3DCOLOR_XRGB(0, 255, 0)));
    CHECK(end());
    expect(read_frame(), "the directional frame could not be read");
    CHECK(show());
    expect_rgb(32, 32, 255, 0, 0, 1,
               "a directional light's diffuse on a face whose normal is its L is not material.Diffuse * light.Diffuse");

    // the ambient and the light together
    CHECK(device->SetRenderState(D3DRS_AMBIENT, D3DCOLOR_XRGB(0, 255, 0)));
    CHECK(begin());
    CHECK(draw_frame(0.5f, 0.0f, 0.0f, -1.0f, D3DCOLOR_XRGB(0, 0, 255)));
    CHECK(end());
    expect(read_frame(), "the ambient-and-light frame could not be read");
    CHECK(show());
    expect_rgb(32, 32, 255, 255, 0, 1, "the ambient and the directional light's diffuse do not add");
  }

  // ---- a non-affine world matrix leaves the lights out ---------------------------------------------------
  // D3D9's lighting drops every light for a world matrix whose last column is not (0, 0, 0, 1): Wine visual.c
  // lighting_test expects black for its translated quad under mat_nonaffine, whatever light is enabled
  {
    CHECK(device->SetRenderState(D3DRS_LIGHTING, TRUE));
    CHECK(device->SetRenderState(D3DRS_COLORVERTEX, FALSE));
    CHECK(device->SetRenderState(D3DRS_AMBIENT, 0));
    CHECK(set_material(D3DCOLOR_XRGB(255, 255, 255)));
    CHECK(set_light(0, directional(0.0f, 0.0f, 1.0f, D3DCOLOR_XRGB(255, 0, 0))));
    CHECK(device->LightEnable(0, TRUE));
    CHECK(begin());
    // w' = 2 - z: the last column is not (0, 0, 0, 1), but the quad still covers the viewport, so only the lighting
    // rule can make it black
    D3DMATRIX nonaffine = identity();
    nonaffine.m[2][3] = -1.0f;
    nonaffine.m[3][3] = 2.0f;
    CHECK(device->SetTransform(D3DTS_WORLD, &nonaffine));
    CHECK(draw_frame(0.5f, 0.0f, 0.0f, -1.0f, D3DCOLOR_XRGB(0, 0, 255)));
    CHECK(end());
    expect(read_frame(), "the non-affine frame could not be read");
    CHECK(show());
    expect_rgb(32, 32, 0, 0, 0, 0, "a non-affine world matrix still lit the quad");
    CHECK(device->LightEnable(0, FALSE));
  }

  // ---- a light pointing away from the face contributes nothing ------------------------------------------
  {
    CHECK(device->SetRenderState(D3DRS_AMBIENT, 0));
    CHECK(set_light(0, directional(0.0f, 0.0f, -1.0f, D3DCOLOR_XRGB(255, 0, 0))));
    CHECK(device->LightEnable(0, TRUE));
    CHECK(begin());
    CHECK(draw_frame(0.5f, 0.0f, 0.0f, -1.0f, D3DCOLOR_XRGB(0, 255, 0)));
    CHECK(end());
    expect(read_frame(), "the unlit-face frame could not be read");
    CHECK(show());
    expect_rgb(32, 32, 0, 0, 0, 1, "a directional light behind the face lit it");
  }

  // ---- a light direction turned 90 degrees lights the face whose normal points at it ---------------------
  // each face is a frame of its own: a quad drawn beside another one in the same frame is a different subject (the two
  // draws' vertices share the device's staging buffer), and a normal that faces the light is all this check is about
  {
    CHECK(device->SetRenderState(D3DRS_AMBIENT, 0));
    auto face_frame = [&](float dx, float dz, float nx, float nz, D3DCOLOR colour) -> HRESULT {
      HRESULT hr = set_light(0, directional(dx, 0.0f, dz, colour));
      if (FAILED(hr))
        return hr;
      if (FAILED(hr = device->LightEnable(0, TRUE)) || FAILED(hr = begin()))
        return hr;
      if (FAILED(hr = draw_frame(0.5f, nx, 0.0f, nz, D3DCOLOR_XRGB(255, 255, 255))))
        return hr;
      return end();
    };
    // Direction (0, 0, 1) is L = -z, which the normal -z agrees with and the normal -x does not
    CHECK(face_frame(0.0f, 1.0f, 0.0f, -1.0f, D3DCOLOR_XRGB(255, 0, 0)));
    expect(read_frame(), "the -z face's frame could not be read");
    CHECK(show());
    expect_rgb(32, 32, 255, 0, 0, 1, "a light along -z did not light the face whose normal is -z");
    CHECK(face_frame(0.0f, 1.0f, -1.0f, 0.0f, D3DCOLOR_XRGB(255, 0, 0)));
    expect(read_frame(), "the -x face's frame could not be read");
    CHECK(show());
    expect_rgb(32, 32, 0, 0, 0, 1, "a light along -z lit the face whose normal is -x");
    // the same two faces with the light turned 90 degrees: the other normal is the lit one
    CHECK(face_frame(1.0f, 0.0f, 0.0f, -1.0f, D3DCOLOR_XRGB(0, 255, 0)));
    expect(read_frame(), "the turned -z face's frame could not be read");
    CHECK(show());
    expect_rgb(32, 32, 0, 0, 0, 1, "a light rotated 90 degrees still lit the face whose normal is -z");
    CHECK(face_frame(1.0f, 0.0f, -1.0f, 0.0f, D3DCOLOR_XRGB(0, 255, 0)));
    expect(read_frame(), "the turned -x face's frame could not be read");
    CHECK(show());
    expect_rgb(32, 32, 0, 255, 0, 1, "a light rotated 90 degrees did not light the face whose normal is -x");
    // the whole face, not only its centre: the lighting is per vertex and the rasterizer interpolates it, so a face
    // whose four vertices are lit alike is one colour everywhere
    for (UINT py = 2; py < 64; py += 15)
      for (UINT px = 2; px < 64; px += 15)
        expect_rgb(px, py, 0, 255, 0, 1, "a light rotated 90 degrees left part of the face it lit unlit");
  }

  // ---- a point light's attenuation, against 1 / (A0 + A1 d + A2 d^2) ------------------------------------
  // each check draws a two-pixel patch at the frame's centre and reads the pixel at its middle: the face is at z 0.5 and
  // its normal points at -z, so a light at (0, 0, 0.5 - d) is d away from that pixel and along its normal, and the patch's
  // four vertices are within a few hundredths of a unit of the same place
  {
    CHECK(device->SetRenderState(D3DRS_AMBIENT, 0));
    CHECK(set_material(D3DCOLOR_XRGB(255, 255, 255)));
    auto attenuation_frame = [&](D3DLIGHT9 light) -> HRESULT {
      HRESULT hr = set_light(0, light);
      if (FAILED(hr))
        return hr;
      if (FAILED(hr = device->LightEnable(0, TRUE)) || FAILED(hr = begin()))
        return hr;
      if (FAILED(hr = draw_patch(32.0f, 32.0f, 2.0f, 0.5f, 0.0f, 0.0f, -1.0f, D3DCOLOR_XRGB(255, 255, 255))))
        return hr;
      return end();
    };
    // Attenuation1 of 1: 255 at d = 1, and half of it at d = 2
    CHECK(attenuation_frame(point(0.0f, 0.0f, -0.5f, 100.0f, 0.0f, 1.0f, 0.0f, D3DCOLOR_XRGB(255, 0, 0))));
    expect(read_frame(), "the d = 1 frame could not be read");
    CHECK(show());
    expect_rgb(32, 32, 255, 0, 0, 3, "a point light at d = 1 with A1 = 1 is not its full diffuse");
    CHECK(attenuation_frame(point(0.0f, 0.0f, -1.5f, 100.0f, 0.0f, 1.0f, 0.0f, D3DCOLOR_XRGB(255, 0, 0))));
    expect(read_frame(), "the d = 2 frame could not be read");
    CHECK(show());
    expect_rgb(32, 32, 128, 0, 0, 3, "a point light at d = 2 with A1 = 1 is not half its diffuse");

    // Attenuation2 alone: 1 / d^2, so a quarter at d = 2
    CHECK(attenuation_frame(point(0.0f, 0.0f, -1.5f, 100.0f, 0.0f, 0.0f, 1.0f, D3DCOLOR_XRGB(0, 255, 0))));
    expect(read_frame(), "the A2 frame could not be read");
    CHECK(show());
    expect_rgb(32, 32, 0, 64, 0, 3, "a point light at d = 2 with A2 = 1 is not a quarter of its diffuse");

    // Attenuation0 alone: 1 / 1, whatever the distance
    CHECK(attenuation_frame(point(0.0f, 0.0f, -2.5f, 100.0f, 1.0f, 0.0f, 0.0f, D3DCOLOR_XRGB(0, 0, 255))));
    expect(read_frame(), "the A0 frame could not be read");
    CHECK(show());
    expect_rgb(32, 32, 0, 0, 255, 2, "a point light with A0 = 1 and no other term is not its full diffuse at any d");

    // Range stops it: the light's Range is inside the patch's distance from it
    CHECK(attenuation_frame(point(0.0f, 0.0f, -1.5f, 1.5f, 0.0f, 1.0f, 0.0f, D3DCOLOR_XRGB(255, 0, 0))));
    expect(read_frame(), "the out-of-range frame could not be read");
    CHECK(show());
    expect_rgb(32, 32, 0, 0, 0, 1, "a point light past its Range still lit the face");
  }

  // ---- the material's emissive is added whatever the lights do ------------------------------------------
  {
    CHECK(device->SetRenderState(D3DRS_AMBIENT, 0));
    CHECK(device->LightEnable(0, FALSE));
    CHECK(set_material(D3DCOLOR_XRGB(0, 0, 0), 0, 0, D3DCOLOR_XRGB(255, 128, 0)));
    CHECK(begin());
    CHECK(draw_frame(0.5f, 0.0f, 0.0f, -1.0f, D3DCOLOR_XRGB(0, 255, 0)));
    CHECK(end());
    expect(read_frame(), "the emissive frame could not be read");
    CHECK(show());
    expect_rgb(32, 32, 255, 128, 0, 1, "the material's emissive is not added to the colour");
  }

  // ---- the specular term, gated by D3DRS_SPECULARENABLE --------------------------------------------------
  {
    CHECK(device->SetRenderState(D3DRS_AMBIENT, 0));
    // no diffuse and a white specular: the frame is the specular term alone, which along the eye's axis is 1
    CHECK(set_material(D3DCOLOR_XRGB(0, 0, 0), 0, D3DCOLOR_XRGB(255, 255, 255), 0, 8.0f));
    CHECK(set_light(0, directional(0.0f, 0.0f, 1.0f, D3DCOLOR_XRGB(0, 0, 0), 0, D3DCOLOR_XRGB(255, 255, 255))));
    CHECK(device->LightEnable(0, TRUE));
    CHECK(device->SetRenderState(D3DRS_SPECULARENABLE, FALSE));
    CHECK(begin());
    CHECK(draw_patch(32.0f, 32.0f, 2.0f, 0.5f, 0.0f, 0.0f, -1.0f, D3DCOLOR_XRGB(255, 255, 255)));
    CHECK(end());
    expect(read_frame(), "the specular-off frame could not be read");
    CHECK(show());
    expect_rgb(32, 32, 0, 0, 0, 1, "the specular term was drawn with D3DRS_SPECULARENABLE FALSE");
    CHECK(device->SetRenderState(D3DRS_SPECULARENABLE, TRUE));
    CHECK(begin());
    CHECK(draw_patch(32.0f, 32.0f, 2.0f, 0.5f, 0.0f, 0.0f, -1.0f, D3DCOLOR_XRGB(255, 255, 255)));
    CHECK(end());
    expect(read_frame(), "the specular-on frame could not be read");
    CHECK(show());
    // the patch's four vertices are a few hundredths of a unit off the eye's axis, so their specular terms (the eighth
    // power of a cosine just under 1) are a little below the axis's: the pixel is their interpolation
    expect_rgb(32, 32, 255, 255, 255, 10,
               "D3DRS_SPECULARENABLE TRUE did not draw material.Specular * light.Specular along the eye's axis");
    CHECK(device->SetRenderState(D3DRS_SPECULARENABLE, FALSE));
  }

  // ---- LightEnable, and GetLightEnable saying which ------------------------------------------------------
  {
    CHECK(device->SetRenderState(D3DRS_AMBIENT, 0));
    CHECK(set_material(D3DCOLOR_XRGB(255, 255, 255)));
    CHECK(set_light(0, directional(1.0f, 0.0f, 0.0f, D3DCOLOR_XRGB(255, 0, 0))));
    CHECK(device->LightEnable(0, TRUE));
    WINBOOL enabled = FALSE;
    expect(SUCCEEDED(device->GetLightEnable(0, &enabled)) && enabled, "GetLightEnable did not report the light enabled");
    CHECK(begin());
    CHECK(draw_frame(0.5f, 0.0f, 0.0f, -1.0f, D3DCOLOR_XRGB(255, 255, 255)));
    CHECK(end());
    expect(read_frame(), "the enabled frame could not be read");
    CHECK(show());
    expect_rgb(32, 32, 0, 0, 0, 1, "a directional light facing away from the face lit it");

    // the same light, its direction turned to the face
    CHECK(set_light(0, directional(0.0f, 0.0f, 1.0f, D3DCOLOR_XRGB(255, 0, 0))));
    CHECK(begin());
    CHECK(draw_frame(0.5f, 0.0f, 0.0f, -1.0f, D3DCOLOR_XRGB(255, 255, 255)));
    CHECK(end());
    expect(read_frame(), "the relit frame could not be read");
    CHECK(show());
    expect_rgb(32, 32, 255, 0, 0, 1, "the light did not light the face after SetLight turned its direction");

    // and disabled again
    CHECK(device->LightEnable(0, FALSE));
    enabled = TRUE;
    expect(SUCCEEDED(device->GetLightEnable(0, &enabled)) && !enabled,
           "GetLightEnable did not report the light disabled");
    CHECK(begin());
    CHECK(draw_frame(0.5f, 0.0f, 0.0f, -1.0f, D3DCOLOR_XRGB(255, 255, 255)));
    CHECK(end());
    expect(read_frame(), "the disabled frame could not be read");
    CHECK(show());
    expect_rgb(32, 32, 0, 0, 0, 1, "LightEnable(0, FALSE) left the light lighting the frame");
  }

  // ---- two of the eight lights add -----------------------------------------------------------------------
  {
    CHECK(device->SetRenderState(D3DRS_AMBIENT, 0));
    CHECK(set_material(D3DCOLOR_XRGB(255, 255, 255)));
    CHECK(set_light(0, directional(0.0f, 0.0f, 1.0f, D3DCOLOR_XRGB(255, 0, 0))));
    CHECK(set_light(1, directional(0.0f, 0.0f, 1.0f, D3DCOLOR_XRGB(0, 0, 255))));
    CHECK(device->LightEnable(0, TRUE));
    CHECK(device->LightEnable(1, TRUE));
    CHECK(begin());
    CHECK(draw_frame(0.5f, 0.0f, 0.0f, -1.0f, D3DCOLOR_XRGB(255, 255, 255)));
    CHECK(end());
    expect(read_frame(), "the two-light frame could not be read");
    CHECK(show());
    expect_rgb(32, 32, 255, 0, 255, 1, "two enabled lights do not add their diffuse");
    CHECK(device->LightEnable(1, FALSE));
  }

  // ---- D3DRS_COLORVERTEX takes the material's diffuse from the vertex -------------------------------------
  {
    CHECK(device->SetRenderState(D3DRS_AMBIENT, 0));
    CHECK(set_material(D3DCOLOR_XRGB(0, 255, 0)));
    CHECK(device->SetRenderState(D3DRS_COLORVERTEX, TRUE));
    CHECK(device->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1));
    CHECK(set_light(0, directional(0.0f, 0.0f, 1.0f, D3DCOLOR_XRGB(255, 255, 255))));
    CHECK(device->LightEnable(0, TRUE));
    // the vertex is red and the material green: the frame is the vertex's colour times the light's
    CHECK(begin());
    CHECK(draw_frame(0.5f, 0.0f, 0.0f, -1.0f, D3DCOLOR_XRGB(255, 0, 0)));
    CHECK(end());
    expect(read_frame(), "the colour-vertex frame could not be read");
    CHECK(show());
    expect_rgb(32, 32, 255, 0, 0, 1,
               "D3DRS_COLORVERTEX with D3DMCS_COLOR1 did not take the material's diffuse from the vertex");

    // and with D3DRS_COLORVERTEX FALSE the material's own diffuse is what the light multiplies
    CHECK(device->SetRenderState(D3DRS_COLORVERTEX, FALSE));
    CHECK(begin());
    CHECK(draw_frame(0.5f, 0.0f, 0.0f, -1.0f, D3DCOLOR_XRGB(255, 0, 0)));
    CHECK(end());
    expect(read_frame(), "the material-diffuse frame could not be read");
    CHECK(show());
    expect_rgb(32, 32, 0, 255, 0, 1, "D3DRS_COLORVERTEX FALSE did not use the material's diffuse");
    CHECK(device->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL));
    CHECK(device->SetRenderState(D3DRS_COLORVERTEX, TRUE));
  }

  // ---- the calls: round-trip, null pointers and an index the device does not have ------------------------
  {
    const D3DLIGHT9 light = point(1.0f, 2.0f, 3.0f, 40.0f, 0.25f, 0.5f, 0.125f, D3DCOLOR_XRGB(4, 5, 6));
    CHECK(device->SetLight(7, &light));
    D3DLIGHT9 got{};
    expect(SUCCEEDED(device->GetLight(7, &got)) && memcmp(&got, &light, sizeof(got)) == 0,
           "GetLight did not give back the light SetLight was given");
    // a light the device does not have: D3D9's MaxActiveLights is 8, so index 8 is past them
    // MaxActiveLights is how many enabled lights shade a draw; Set/Get/Enable accept a wider index range
    // (Wine device.c test_lights enables 1..MaxActiveLights inclusive and one past that)
    expect(SUCCEEDED(device->SetLight(8, &light)), "SetLight refused index MaxActiveLights");
    expect(SUCCEEDED(device->GetLight(8, &got)), "GetLight refused index MaxActiveLights");
    WINBOOL enabled = FALSE;
    expect(SUCCEEDED(device->LightEnable(8, TRUE)), "LightEnable refused index MaxActiveLights");
    expect(SUCCEEDED(device->GetLightEnable(8, &enabled)) && enabled,
           "GetLightEnable refused index MaxActiveLights");
    CHECK(device->LightEnable(8, FALSE));
    expect(device->SetLight(16, &light) == D3DERR_INVALIDCALL, "SetLight took an index past the API slot count");
    expect(device->LightEnable(16, TRUE) == D3DERR_INVALIDCALL, "LightEnable took an index past the API slot count");
    expect(device->SetLight(0, nullptr) == D3DERR_INVALIDCALL, "SetLight took a null light");
    expect(device->GetLight(0, nullptr) == D3DERR_INVALIDCALL, "GetLight took a null output");
    expect(device->LightEnable(0, TRUE) == S_OK, "LightEnable refused a light the device has");

    const D3DMATERIAL9 value = material(D3DCOLOR_XRGB(7, 8, 9), D3DCOLOR_XRGB(1, 1, 1), D3DCOLOR_XRGB(2, 2, 2),
                                        D3DCOLOR_XRGB(3, 3, 3), 12.0f);
    CHECK(device->SetMaterial(&value));
    D3DMATERIAL9 got_material{};
    expect(SUCCEEDED(device->GetMaterial(&got_material)) && memcmp(&got_material, &value, sizeof(value)) == 0,
           "GetMaterial did not give back the material SetMaterial was given");
    expect(device->SetMaterial(nullptr) == D3DERR_INVALIDCALL, "SetMaterial took a null material");
    expect(device->GetMaterial(nullptr) == D3DERR_INVALIDCALL, "GetMaterial took a null output");
  }

  // ---- a state block carries the lights, their enables and the material ---------------------------------
  {
    CHECK(device->SetRenderState(D3DRS_LIGHTING, TRUE));
    CHECK(device->SetRenderState(D3DRS_COLORVERTEX, FALSE));
    CHECK(device->SetRenderState(D3DRS_AMBIENT, 0));
    CHECK(set_material(D3DCOLOR_XRGB(255, 255, 255)));
    CHECK(set_light(0, directional(0.0f, 0.0f, 1.0f, D3DCOLOR_XRGB(255, 0, 0))));
    CHECK(device->LightEnable(0, TRUE));

    ComPtr<IDirect3DStateBlock9> block;
    CHECK(device->CreateStateBlock(D3DSBT_VERTEXSTATE, &block));
    CHECK(block->Capture());

    // the light and the material changed under the block
    CHECK(set_light(0, directional(0.0f, 0.0f, 1.0f, D3DCOLOR_XRGB(0, 0, 255))));
    CHECK(set_material(D3DCOLOR_XRGB(0, 0, 0)));
    CHECK(begin());
    CHECK(draw_frame(0.5f, 0.0f, 0.0f, -1.0f, D3DCOLOR_XRGB(255, 255, 255)));
    CHECK(end());
    expect(read_frame(), "the changed-light frame could not be read");
    CHECK(show());
    expect_rgb(32, 32, 0, 0, 0, 1, "the changed light and material did not reach the draw");

    CHECK(block->Apply());
    D3DLIGHT9 got{};
    CHECK(device->GetLight(0, &got));
    D3DLIGHT9 red = directional(0.0f, 0.0f, 1.0f, D3DCOLOR_XRGB(255, 0, 0));
    expect(memcmp(&got, &red, sizeof(got)) == 0, "a vertex-state block did not put the light it captured back");
    D3DMATERIAL9 got_material{};
    CHECK(device->GetMaterial(&got_material));
    expect(got_material.Diffuse.r > 0.9f && got_material.Diffuse.g > 0.9f && got_material.Diffuse.b > 0.9f,
           "a vertex-state block did not put the material it captured back");
    WINBOOL restored = FALSE;
    expect(SUCCEEDED(device->GetLightEnable(0, &restored)) && restored,
           "a vertex-state block did not put the light's enable back");
    CHECK(begin());
    CHECK(draw_frame(0.5f, 0.0f, 0.0f, -1.0f, D3DCOLOR_XRGB(255, 255, 255)));
    CHECK(end());
    expect(read_frame(), "the restored-light frame could not be read");
    CHECK(show());

    // a disabled light is state the block carries too
    CHECK(block->Capture());
    CHECK(device->LightEnable(0, FALSE));
    CHECK(block->Apply());
    WINBOOL enabled = FALSE;
    expect(SUCCEEDED(device->GetLightEnable(0, &enabled)) && enabled,
           "a vertex-state block did not put the light's enable back");
  }

  // a spot light's Falloff, read from a pixel off the cone's axis. Theta is 0 and Phi is a right angle, so the
  // penumbra runs from the axis out to 45 degrees (cos of half of Phi). This vertex sits close enough to the axis
  // that Falloff 1 stays bright, and far enough that Falloff 8, which raises the cone factor to the eighth power,
  // does not (Microsoft Learn, D3DLIGHT9::Falloff; "Mathematics of Lighting")
  {
    for (DWORD light = 1; light < 8; light++)
      CHECK(device->LightEnable(light, FALSE));
    CHECK(device->SetRenderState(D3DRS_LIGHTING, TRUE));
    CHECK(device->SetRenderState(D3DRS_SPECULARENABLE, FALSE));
    CHECK(device->SetRenderState(D3DRS_AMBIENT, 0));
    CHECK(device->SetRenderState(D3DRS_COLORVERTEX, FALSE));
    CHECK(set_material(D3DCOLOR_XRGB(255, 255, 255)));
    auto spot = [&](float falloff) {
      D3DLIGHT9 light{};
      light.Type = D3DLIGHT_SPOT;
      light.Diffuse = colour_of(D3DCOLOR_XRGB(255, 255, 255));
      light.Position = {0.0f, 0.0f, -1.0f};
      light.Direction = {0.0f, 0.0f, 1.0f};
      light.Range = 100.0f;
      light.Attenuation0 = 1.0f;
      light.Falloff = falloff;
      light.Theta = 0.0f;
      light.Phi = 1.5707963f;
      return light;
    };
    auto spot_frame = [&](float falloff) -> HRESULT {
      HRESULT hr = set_light(0, spot(falloff));
      if (FAILED(hr) || FAILED(hr = device->LightEnable(0, TRUE)) || FAILED(hr = begin()))
        return hr;
      if (FAILED(hr = draw_patch(44.0f, 32.0f, 2.0f, 0.0f, 0.0f, 0.0f, -1.0f, D3DCOLOR_XRGB(255, 255, 255))))
        return hr;
      return end();
    };
    CHECK(spot_frame(1.0f));
    expect(read_frame(), "the falloff 1 frame could not be read");
    CHECK(show());
    const int bright = (int)((pixel(44, 32) >> 16) & 0xFF);
    CHECK(spot_frame(8.0f));
    expect(read_frame(), "the falloff 8 frame could not be read");
    CHECK(show());
    const int dim = (int)((pixel(44, 32) >> 16) & 0xFF);
    expect(bright > 120 && dim < bright - 60, "spot Falloff 8 was not darker than Falloff 1 off the cone axis");
  }

  // ---- D3DRS_LOCALVIEWER: the eye is the view direction at infinity unless the state takes it from the vertex --------
  // A directional light whose specular is white and diffuse black leaves only the highlight, so the frame shows where
  // the half vector points. With the viewer at infinity every vertex has the same eye direction and the highlight covers
  // the frame; with the local viewer the corners look away from the eye and the highlight shrinks to a spot. Reading a
  // corner therefore tells the two states apart, and a build that always computed the local eye gave the infinite viewer
  // the dark corner.
  {
    CHECK(device->SetRenderState(D3DRS_LIGHTING, TRUE));
    CHECK(device->SetRenderState(D3DRS_COLORVERTEX, FALSE));
    CHECK(device->SetRenderState(D3DRS_AMBIENT, 0));
    CHECK(device->SetRenderState(D3DRS_SPECULARENABLE, TRUE));
    CHECK(set_material(D3DCOLOR_XRGB(0, 0, 0), 0, D3DCOLOR_XRGB(255, 255, 255), 0, 30.0f));
    D3DLIGHT9 light{};
    light.Type = D3DLIGHT_DIRECTIONAL;
    light.Diffuse = colour_of(0);
    light.Specular = colour_of(D3DCOLOR_XRGB(255, 255, 255));
    light.Direction = {0.0f, 0.0f, 1.0f};
    CHECK(set_light(0, light));
    CHECK(device->LightEnable(0, TRUE));

    for (int local = 0; local < 2; local++) {
      CHECK(device->SetRenderState(D3DRS_LOCALVIEWER, local ? TRUE : FALSE));
      CHECK(begin());
      CHECK(draw_frame(0.5f, 0.0f, 0.0f, -1.0f, D3DCOLOR_XRGB(255, 255, 255)));
      CHECK(end());
      expect(read_frame(), "the local-viewer frame could not be read");
      CHECK(show());
      if (!local)
        expect_rgb(2, 2, 255, 255, 255, 3,
                   "D3DRS_LOCALVIEWER FALSE did not use the view direction at every vertex");
      else
        expect_rgb(2, 2, 0, 0, 0, 8, "D3DRS_LOCALVIEWER TRUE did not take the eye from the vertex");
    }
    CHECK(device->SetRenderState(D3DRS_LOCALVIEWER, TRUE));
    CHECK(device->SetRenderState(D3DRS_SPECULARENABLE, FALSE));
  }

  // ---- D3DRS_COLORVERTEX and the four material sources -------------------------------------------------------------
  // A lit format with no normal has only its ambient and emissive terms (a light has no direction to shade), and each
  // reflectance can come from the material, the vertex's diffuse (COLOR1) or its specular (COLOR2). A colour the
  // format does not carry falls back to the material. The material is ambient blue 128/255 and emissive blue 64/255,
  // the global ambient is white and no light is enabled, so each expected value is the sum of the two chosen sources.
  // This is Wine visual.c test_color_vertex's table, over the four D3DFVF_XYZ formats it draws.
  {
    static const struct {
      DWORD fvf, color_vertex, ambient, emissive;
      D3DCOLOR result;
    } cases[] = {
        {D3DFVF_DIFFUSE | D3DFVF_SPECULAR, FALSE, D3DMCS_COLOR1, D3DMCS_COLOR2, D3DCOLOR_XRGB(0x00, 0x00, 0xc0)},
        {D3DFVF_DIFFUSE | D3DFVF_SPECULAR, TRUE, D3DMCS_COLOR1, D3DMCS_COLOR2, D3DCOLOR_XRGB(0xff, 0xff, 0x00)},
        {D3DFVF_DIFFUSE | D3DFVF_SPECULAR, TRUE, D3DMCS_MATERIAL, D3DMCS_COLOR2, D3DCOLOR_XRGB(0x00, 0xff, 0x80)},
        {D3DFVF_DIFFUSE | D3DFVF_SPECULAR, TRUE, D3DMCS_COLOR1, D3DMCS_MATERIAL, D3DCOLOR_XRGB(0xff, 0x00, 0x40)},
        {D3DFVF_DIFFUSE | D3DFVF_SPECULAR, TRUE, D3DMCS_COLOR1, D3DMCS_COLOR1, D3DCOLOR_XRGB(0xff, 0x00, 0x00)},
        {D3DFVF_DIFFUSE | D3DFVF_SPECULAR, TRUE, D3DMCS_COLOR2, D3DMCS_COLOR2, D3DCOLOR_XRGB(0x00, 0xff, 0x00)},
        {D3DFVF_SPECULAR, TRUE, D3DMCS_COLOR1, D3DMCS_COLOR2, D3DCOLOR_XRGB(0xff, 0x00, 0x80)},
        {D3DFVF_SPECULAR, TRUE, D3DMCS_COLOR1, D3DMCS_MATERIAL, D3DCOLOR_XRGB(0x00, 0x00, 0xc0)},
        {D3DFVF_SPECULAR, TRUE, D3DMCS_MATERIAL, D3DMCS_COLOR2, D3DCOLOR_XRGB(0xff, 0x00, 0x80)},
        {D3DFVF_DIFFUSE, TRUE, D3DMCS_COLOR1, D3DMCS_COLOR2, D3DCOLOR_XRGB(0xff, 0x00, 0x40)},
        {D3DFVF_DIFFUSE, TRUE, D3DMCS_COLOR1, D3DMCS_MATERIAL, D3DCOLOR_XRGB(0xff, 0x00, 0x40)},
        {D3DFVF_DIFFUSE, TRUE, D3DMCS_COLOR2, D3DMCS_MATERIAL, D3DCOLOR_XRGB(0x00, 0x00, 0xc0)},
        {0, TRUE, D3DMCS_COLOR1, D3DMCS_COLOR2, D3DCOLOR_XRGB(0x00, 0x00, 0xc0)},
    };
    // Wine visual.c test_color_vertex draws every one of these formats from one 20-byte vertex: the FVF's elements
    // sit at their packed offsets and the bytes past them are padding, so the caller's stream stride is larger than
    // the FVF's packed size
    struct ColourVertex {
      float x, y, z;
      D3DCOLOR diffuse, specular;
    };
    CHECK(device->SetRenderState(D3DRS_LIGHTING, TRUE));
    CHECK(device->SetRenderState(D3DRS_SPECULARENABLE, FALSE));
    CHECK(device->SetRenderState(D3DRS_AMBIENT, 0xffffffff));
    CHECK(set_material(D3DCOLOR_XRGB(0, 0, 0), D3DCOLOR_XRGB(0, 0, 128), 0, D3DCOLOR_XRGB(0, 0, 64)));
    CHECK(device->LightEnable(0, FALSE));
    const D3DCOLOR red = D3DCOLOR_XRGB(255, 0, 0), green = D3DCOLOR_XRGB(0, 255, 0);
    const float left = 0.0f, top = 0.0f, right = (float)d3d9_test::kExtent, bottom = (float)d3d9_test::kExtent;
    for (const auto &one : cases) {
      CHECK(device->SetRenderState(D3DRS_COLORVERTEX, one.color_vertex));
      CHECK(device->SetRenderState(D3DRS_AMBIENTMATERIALSOURCE, one.ambient));
      CHECK(device->SetRenderState(D3DRS_EMISSIVEMATERIALSOURCE, one.emissive));
      CHECK(device->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1));
      CHECK(device->SetRenderState(D3DRS_SPECULARMATERIALSOURCE, D3DMCS_COLOR2));
      CHECK(begin());
      const DWORD fvf = D3DFVF_XYZ | one.fvf;
      CHECK(device->SetFVF(fvf));
      const ColourVertex quad[4] = {{ndc_x(left), ndc_y(top), 0.5f, red, green},
                                    {ndc_x(left), ndc_y(bottom), 0.5f, red, green},
                                    {ndc_x(right), ndc_y(top), 0.5f, red, green},
                                    {ndc_x(right), ndc_y(bottom), 0.5f, red, green}};
      CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0])));
      CHECK(end());
      expect(read_frame(), "the colour-vertex frame could not be read");
      CHECK(show());
      expect_rgb(32, 32, (int)((one.result >> 16) & 0xFF), (int)((one.result >> 8) & 0xFF),
                 (int)(one.result & 0xFF), 2,
                 "D3DRS_COLORVERTEX did not take a material term from the source it names");
    }
    CHECK(device->SetRenderState(D3DRS_COLORVERTEX, TRUE));
    CHECK(device->SetRenderState(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL));
    CHECK(device->SetRenderState(D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL));
  }

  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
