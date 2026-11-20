// contract: a fixed-function texture stage samples the dimension its texture actually is, not one dimension for every
// texture (Microsoft Learn, "Texture Blending": a stage's texture argument is the texture set for the stage; and
// IDirect3DDevice9::CreateCubeTexture / CreateVolumeTexture, which make cubemaps and volumes rather than 2D textures).
// - A 2D texture is sampled at (u, v), which is what D3DFVF_TEX1's two coordinates are.
// - A cube is sampled by a direction of three components: the coordinate set's x, y and z, the components the FVF does
//   not carry being zero (D3DFVF_TEXCOORDn declares a count; Wine's fixed-function vertex shaders read the attribute as
//   vec4, which fills the tail with 0,0,0,1). The face is the direction's dominant axis, so (1, 0.5, 0) samples +X and
//   (0.5, 1, 0) samples +Y.
// - A volume is sampled by (u, v, w) inside its box, so a two-component coordinate reads slice 0.
// - The dimension is a property of each stage, not of the draw: stage 0 may sample a cube while stage 1 samples a 2D
//   texture, and a draw after either takes the dimensions bound for it (no stale variant survives a change).
// - A stage with no texture bound samples the device's white texel (the two-stage paths' documented fill), which is a
//   2D sample, not a cube or volume one.
#include <windows.h>
#include <d3d9.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

#define CHECK(x)                                                                                                       \
  if (FAILED(x)) {                                                                                                     \
    printf("failed: %s (line %d)\n", #x, __LINE__);                                                                    \
    return 1;                                                                                                          \
  }

namespace {

const UINT kClient = 64;
// the target is X8R8G8B8, so a pixel read back is the colour's 24 bits: the expected values are the same 24 bits
const D3DCOLOR kClear = 0x000000u;
const D3DCOLOR kRed = 0xFF0000u;
const D3DCOLOR kGreen = 0x00FF00u;
const D3DCOLOR kBlue = 0x0000FFu;
const D3DCOLOR kCyan = 0x00FFFFu;
const D3DCOLOR kWhite = 0xFFFFFFu;

// a screen-space vertex with one set of two texture coordinates, which D3DFVF_XYZRHW's path places itself
struct Vertex {
  float x, y, z, rhw, u, v;
};

const DWORD kFVF = D3DFVF_XYZRHW | D3DFVF_TEX1;

bool
fill_2d(IDirect3DTexture9 *texture, D3DCOLOR colour) {
  D3DSURFACE_DESC desc{};
  if (FAILED(texture->GetLevelDesc(0, &desc)))
    return false;
  D3DLOCKED_RECT locked{};
  if (FAILED(texture->LockRect(0, &locked, nullptr, 0)))
    return false;
  for (UINT y = 0; y < desc.Height; y++) {
    DWORD *row = (DWORD *)((char *)locked.pBits + (size_t)y * locked.Pitch);
    for (UINT x = 0; x < desc.Width; x++)
      row[x] = colour;
  }
  return SUCCEEDED(texture->UnlockRect(0));
}

bool
fill_face(IDirect3DCubeTexture9 *texture, D3DCUBEMAP_FACES face, D3DCOLOR colour) {
  D3DSURFACE_DESC desc{};
  if (FAILED(texture->GetLevelDesc(0, &desc)))
    return false;
  D3DLOCKED_RECT locked{};
  if (FAILED(texture->LockRect(face, 0, &locked, nullptr, 0)))
    return false;
  for (UINT y = 0; y < desc.Height; y++) {
    DWORD *row = (DWORD *)((char *)locked.pBits + (size_t)y * locked.Pitch);
    for (UINT x = 0; x < desc.Width; x++)
      row[x] = colour;
  }
  return SUCCEEDED(texture->UnlockRect(face, 0));
}

bool
fill_slice(IDirect3DVolumeTexture9 *texture, UINT slice, D3DCOLOR colour) {
  D3DVOLUME_DESC desc{};
  if (FAILED(texture->GetLevelDesc(0, &desc)))
    return false;
  D3DLOCKED_BOX locked{};
  if (FAILED(texture->LockBox(0, &locked, nullptr, 0)))
    return false;
  for (UINT z = 0; z < desc.Depth; z++) {
    for (UINT y = 0; y < desc.Height; y++) {
      DWORD *row = (DWORD *)((char *)locked.pBits + (size_t)z * locked.SlicePitch + (size_t)y * locked.RowPitch);
      for (UINT x = 0; x < desc.Width; x++)
        row[x] = z == slice ? colour : D3DCOLOR_XRGB(255, 255, 0); // any other slice is yellow
    }
  }
  return SUCCEEDED(texture->UnlockBox(0));
}

} // namespace

int
main() {
  unsigned wrong = 0, checks = 0;
  auto expect = [&](bool ok, const char *what) {
    checks++;
    if (!ok && wrong++ < 24)
      printf("wrong: %s\n", what);
  };

  WNDCLASSA wc{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr, "d3d9_ff_sampling"};
  RegisterClassA(&wc);
  HWND window = CreateWindowA("d3d9_ff_sampling", "d3d9_ff_sampling", WS_OVERLAPPEDWINDOW, 0, 0, kClient, kClient,
                              nullptr, nullptr, wc.hInstance, nullptr);
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
  present.SwapEffect = D3DSWAPEFFECT_DISCARD;
  present.hDeviceWindow = window;
  present.Windowed = TRUE;
  present.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  ComPtr<IDirect3DDevice9> device;
  CHECK(d3d9->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &present, &device));

  CHECK(device->SetFVF(kFVF));
  CHECK(device->SetRenderState(D3DRS_LIGHTING, FALSE));
  CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
  CHECK(device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE));
  for (UINT stage = 0; stage < 3; stage++) {
    CHECK(device->SetSamplerState(stage, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
    CHECK(device->SetSamplerState(stage, D3DSAMP_MINFILTER, D3DTEXF_POINT));
    CHECK(device->SetSamplerState(stage, D3DSAMP_MIPFILTER, D3DTEXF_POINT));
    CHECK(device->SetSamplerState(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP));
    CHECK(device->SetSamplerState(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));
  }

  ComPtr<IDirect3DSurface9> back, readback;
  CHECK(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back));
  CHECK(device->CreateOffscreenPlainSurface(kClient, kClient, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr));
  auto centre_pixel = [&]() -> D3DCOLOR {
    if (FAILED(device->GetRenderTargetData(back.Get(), readback.Get())))
      return 0;
    D3DLOCKED_RECT locked{};
    if (FAILED(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
      return 0;
    const DWORD got = ((const DWORD *)((const char *)locked.pBits + (kClient / 2) * locked.Pitch))[kClient / 2];
    readback->UnlockRect();
    return got & 0x00FFFFFF;
  };
  // one full-screen quad whose four corners carry the same coordinate: every pixel's coordinate is the one the case
  // names, so the pixel says what that coordinate samples
  auto draw = [&](float u, float v) -> D3DCOLOR {
    const Vertex quad[6] = {
        {0, 0, 0, 1, u, v}, {0, (float)kClient, 0, 1, u, v}, {(float)kClient, 0, 0, 1, u, v},
        {(float)kClient, 0, 0, 1, u, v}, {0, (float)kClient, 0, 1, u, v}, {(float)kClient, (float)kClient, 0, 1, u, v},
    };
    if (FAILED(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0)) || FAILED(device->BeginScene()) ||
        FAILED(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, quad, sizeof(Vertex))) || FAILED(device->EndScene()))
      return 0xFFFFFFFFu;
    return centre_pixel();
  };

  // ---- a 2D texture samples (u, v) ------------------------------------------------------------------------------
  ComPtr<IDirect3DTexture9> flat;
  CHECK(device->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &flat, nullptr));
  expect(fill_2d(flat.Get(), D3DCOLOR_XRGB(0, 255, 0)), "a 2D texture was not written");
  CHECK(device->SetTexture(0, flat.Get()));
  {
    const D3DCOLOR got = draw(0.5f, 0.5f);
    expect(got == kGreen, "a fixed-function draw did not sample a 2D texture at (u, v)");
    printf("2D: %06lx (green)\n", (unsigned long)got);
  }

  // ---- a cube samples a direction, and the face is the dominant axis -------------------------------------------
  ComPtr<IDirect3DCubeTexture9> cube;
  CHECK(device->CreateCubeTexture(4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cube, nullptr));
  for (UINT face = 0; face < 6; face++) {
    const D3DCOLOR colour = face == D3DCUBEMAP_FACE_POSITIVE_X ? D3DCOLOR_XRGB(0, 0, 255) : D3DCOLOR_XRGB(255, 0, 0);
    expect(fill_face(cube.Get(), (D3DCUBEMAP_FACES)face, colour), "a cube face was not written");
  }
  CHECK(device->SetTexture(0, cube.Get()));
  {
    const D3DCOLOR plus_x = draw(1.0f, 0.5f);
    expect(plus_x == kBlue, "a +X direction did not sample the cube's +X face");
    printf("cube +X: %06lx (blue)\n", (unsigned long)plus_x);
    const D3DCOLOR plus_y = draw(0.5f, 1.0f);
    expect(plus_y == kRed, "a +Y direction did not sample the cube's +Y face (or sampled a 2D view of it)");
    printf("cube +Y: %06lx (red)\n", (unsigned long)plus_y);
    const D3DCOLOR zero = draw(0.0f, 0.0f);
    expect(zero == kBlue, "a zero direction did not stay on the cube (a 2D sample of the cube's +X face is what the "
                          "old path did)");
    printf("cube zero: %06lx (blue)\n", (unsigned long)zero);
  }

  // ---- the same cube through a vertex the device transforms (D3DFVF_XYZ | D3DFVF_TEX1) ------------------------
  // the Wine visual.c cube phase draws exactly this format: an XYZ vertex with one two-component coordinate set, under
  // the device's (identity) matrices
  {
    struct Transformed {
      float x, y, z, u, v;
    };
    const Transformed quad[6] = {
        {-1, -1, 0.5f, 1.0f, 0.5f}, {-1, 1, 0.5f, 1.0f, 0.5f}, {1, -1, 0.5f, 1.0f, 0.5f},
        {1, -1, 0.5f, 1.0f, 0.5f},  {-1, 1, 0.5f, 1.0f, 0.5f}, {1, 1, 0.5f, 1.0f, 0.5f},
    };
    CHECK(device->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1));
    CHECK(device->SetTexture(0, cube.Get()));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
    CHECK(device->BeginScene());
    CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, quad, sizeof(Transformed)));
    CHECK(device->EndScene());
    const D3DCOLOR got = centre_pixel();
    expect(got == kBlue, "a cube sampled by a transformed (XYZ) vertex did not read its +X face");
    printf("transformed cube: %06lx (blue)\n", (unsigned long)got);
    CHECK(device->SetFVF(kFVF));
  }

  // ---- a cube's generated levels sample the level 0 content, not uninitialized bytes ----------------------------
  // Wine visual.c test_mipmap_autogen's cube is a 1024 managed AUTOGENMIPMAP cube whose level 0 is written through
  // LockRect and sampled minified: the level the filter selects must be made from level 0 (the same test's 2D phases
  // do it with a 2D chain), so a minified draw is the face's colour and not black
  {
    ComPtr<IDirect3DCubeTexture9> big;
    CHECK(device->CreateCubeTexture(1024, 0, D3DUSAGE_AUTOGENMIPMAP, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, &big, nullptr));
    printf("autogen cube levels: %lu\n", (unsigned long)big->GetLevelCount());
    for (UINT face = 0; face < 6; face++) {
      D3DLOCKED_RECT locked{};
      CHECK(big->LockRect((D3DCUBEMAP_FACES)face, 0, &locked, nullptr, 0));
      for (UINT y = 0; y < 1024; y++) {
        DWORD *row = (DWORD *)((char *)locked.pBits + (size_t)y * locked.Pitch);
        for (UINT x = 0; x < 1024; x++)
          row[x] = D3DCOLOR_XRGB(255, 255, 255);
      }
      CHECK(big->UnlockRect((D3DCUBEMAP_FACES)face, 0));
    }
    CHECK(device->SetTexture(0, big.Get()));
    CHECK(device->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1));
    // a four-pixel quad over a 1024 face: the derivatives select a level well below 0
    struct Transformed {
      float x, y, z, u, v;
    };
    const float extent = 4.0f / (kClient / 2.0f);
    const Transformed quad[6] = {
        {-extent, -extent, 0.5f, 1.0f, 0.5f}, {-extent, extent, 0.5f, 1.0f, 0.5f}, {extent, -extent, 0.5f, 1.0f, 0.5f},
        {extent, -extent, 0.5f, 1.0f, 0.5f},  {-extent, extent, 0.5f, 1.0f, 0.5f}, {extent, extent, 0.5f, 1.0f, 0.5f},
    };
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0));
    CHECK(device->BeginScene());
    CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, quad, sizeof(Transformed)));
    CHECK(device->EndScene());
    const D3DCOLOR got = centre_pixel();
    expect(got == kWhite, "a minified draw of a managed AUTOGENMIPMAP cube did not sample a generated level of its face");
    printf("autogen cube minified: %06lx (white)\n", (unsigned long)got);
    CHECK(device->SetFVF(kFVF));
    CHECK(device->SetTexture(0, flat.Get()));
  }

  // ---- a volume samples (u, v, w): a two-component coordinate reads slice 0 -------------------------------------
  ComPtr<IDirect3DVolumeTexture9> volume;
  CHECK(device->CreateVolumeTexture(4, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &volume, nullptr));
  expect(fill_slice(volume.Get(), 0, D3DCOLOR_XRGB(0, 255, 255)), "a volume slice was not written");
  CHECK(device->SetTexture(0, volume.Get()));
  {
    const D3DCOLOR got = draw(0.5f, 0.5f);
    expect(got == kCyan, "a volume was not sampled as a volume at (u, v, 0)");
    printf("volume: %06lx (cyan, slice 0; yellow is another slice)\n", (unsigned long)got);
  }

  // ---- a texture-type change between draws takes effect, both ways ---------------------------------------------
  CHECK(device->SetTexture(0, cube.Get()));
  {
    const D3DCOLOR got = draw(1.0f, 0.5f);
    expect(got == kBlue, "the draw after a volume did not sample the cube again");
    printf("cube again: %06lx (blue)\n", (unsigned long)got);
  }
  CHECK(device->SetTexture(0, flat.Get()));
  expect(draw(0.5f, 0.5f) == kGreen, "the draw after a cube did not sample the 2D texture again");

  // ---- a stage with no texture samples the white texel, not the texture the draw before it sampled --------------
  CHECK(device->SetTexture(0, nullptr));
  {
    const D3DCOLOR got = draw(0.5f, 0.5f);
    expect(got == kWhite, "a stage with no texture did not sample the white texel");
    printf("null: %06lx (white)\n", (unsigned long)got);
  }

  // ---- mixed dimensions across stages: stage 0 a cube, stage 1 a 2D texture -----------------------------------
  // stage 0 keeps its default MODULATE of the texture with the (white) diffuse, so it contributes its face's colour;
  // stage 1 adds its 2D texel, so blue + green is cyan. a stage 0 that sampled nothing (or the white texel) would add
  // to green and leave green or white, which is what tells the two apart.
  CHECK(device->SetTexture(0, cube.Get()));
  CHECK(device->SetTexture(1, flat.Get()));
  CHECK(device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_ADD));
  CHECK(device->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_TEXTURE));
  CHECK(device->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT));
  {
    const D3DCOLOR got = draw(1.0f, 0.5f);
    expect(got == kCyan, "a cube on stage 0 and a 2D texture on stage 1 did not both sample");
    printf("mixed two stages: %06lx (cyan)\n", (unsigned long)got);
  }

  // ---- the same mix with a third stage, which is the eight-stage combiner's path -------------------------------
  ComPtr<IDirect3DTexture9> black;
  CHECK(device->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &black, nullptr));
  expect(fill_2d(black.Get(), D3DCOLOR_XRGB(0, 0, 0)), "the third stage's texture was not written");
  CHECK(device->SetTexture(2, black.Get()));
  CHECK(device->SetTextureStageState(2, D3DTSS_COLOROP, D3DTOP_ADD));
  CHECK(device->SetTextureStageState(2, D3DTSS_COLORARG1, D3DTA_TEXTURE));
  CHECK(device->SetTextureStageState(2, D3DTSS_COLORARG2, D3DTA_CURRENT));
  {
    const D3DCOLOR got = draw(1.0f, 0.5f);
    expect(got == kCyan, "the eight-stage combiner did not sample a cube on stage 0 and 2D textures on stages 1 and 2");
    printf("mixed eight stages: %06lx (cyan)\n", (unsigned long)got);
  }

  device.Reset();
  d3d9.Reset();
  DestroyWindow(window);

  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
