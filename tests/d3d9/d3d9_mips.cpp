// contract: the mip chain a D3DPOOL_MANAGED texture owns, and when Direct3D 9 makes one level out of another.
// - A managed texture's levels are the levels the application authors: D3DPOOL_MANAGED is not
//   D3DUSAGE_AUTOGENMIPMAP (Microsoft Learn, D3DPOOL and Automatic Generation of Mipmaps), so unlocking level 0
//   does not replace levels 1..n, and a read-only unlock replaces nothing at all.
// - GenerateMipSubLevels generates the sublevels on request ("An application can generate mipmap sublevels at any
//   time by calling GenerateMipSubLevels"), and the generated levels are the filtered level above them.
// - A compressed chain's levels are the blocks the application uploads; no level is generated from another.
// - GetLevelCount is the number of levels the texture was made with, whatever the format's automatic-generation
//   support: a fallback that quietly makes one level of a requested chain is not a shorter chain, it is a refusal.
// - D3DUSAGE_AUTOGENMIPMAP is not valid with D3DPOOL_MANAGED (Microsoft Learn, D3DPOOL), and a level count past the
//   chain is D3DERR_INVALIDCALL.
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

struct TexturedVertex {
  float x, y, z, rhw;
  D3DCOLOR colour;
  float u, v;
};

const DWORD kTexturedFVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

const UINT kSize = 8;
const D3DCOLOR kRed = D3DCOLOR_ARGB(0xFF, 0xFF, 0x00, 0x00);
const D3DCOLOR kGreen = D3DCOLOR_ARGB(0xFF, 0x00, 0xFF, 0x00);
const D3DCOLOR kBlue = D3DCOLOR_ARGB(0xFF, 0x00, 0x00, 0xFF);
const D3DCOLOR kWhite = D3DCOLOR_ARGB(0xFF, 0xFF, 0xFF, 0xFF);
const D3DCOLOR kBlack = D3DCOLOR_ARGB(0xFF, 0x00, 0x00, 0x00);

// fills one level of an uncompressed texture with one colour
bool
fill_solid(IDirect3DTexture9 *texture, UINT level, D3DCOLOR colour) {
  D3DSURFACE_DESC desc{};
  if (FAILED(texture->GetLevelDesc(level, &desc)))
    return false;
  D3DLOCKED_RECT locked{};
  if (FAILED(texture->LockRect(level, &locked, nullptr, 0)))
    return false;
  for (UINT y = 0; y < desc.Height; y++) {
    DWORD *row = (DWORD *)((char *)locked.pBits + (size_t)y * locked.Pitch);
    for (UINT x = 0; x < desc.Width; x++)
      row[x] = colour;
  }
  return SUCCEEDED(texture->UnlockRect(level));
}

// whether one level is one colour, with the mismatches counted
bool
level_is_solid(IDirect3DTexture9 *texture, UINT level, D3DCOLOR colour, UINT &mismatches) {
  mismatches = 0;
  D3DSURFACE_DESC desc{};
  if (FAILED(texture->GetLevelDesc(level, &desc)))
    return false;
  D3DLOCKED_RECT locked{};
  if (FAILED(texture->LockRect(level, &locked, nullptr, D3DLOCK_READONLY)))
    return false;
  for (UINT y = 0; y < desc.Height; y++) {
    const DWORD *row = (const DWORD *)((const char *)locked.pBits + (size_t)y * locked.Pitch);
    for (UINT x = 0; x < desc.Width; x++)
      if ((row[x] & 0x00FFFFFF) != (colour & 0x00FFFFFF))
        mismatches++;
  }
  texture->UnlockRect(level);
  return mismatches == 0;
}

DWORD
read_texel(IDirect3DTexture9 *texture, UINT level, UINT x, UINT y) {
  D3DLOCKED_RECT locked{};
  if (FAILED(texture->LockRect(level, &locked, nullptr, D3DLOCK_READONLY)))
    return 0;
  const DWORD got = ((const DWORD *)((const char *)locked.pBits + (size_t)y * locked.Pitch))[x];
  texture->UnlockRect(level);
  return got;
}

// four quadrants of one colour each: a level generated from this has the same four colours, so a generated level
// says which level it was made from
bool
fill_quadrants(IDirect3DTexture9 *texture, UINT level) {
  D3DSURFACE_DESC desc{};
  if (FAILED(texture->GetLevelDesc(level, &desc)))
    return false;
  D3DLOCKED_RECT locked{};
  if (FAILED(texture->LockRect(level, &locked, nullptr, 0)))
    return false;
  for (UINT y = 0; y < desc.Height; y++) {
    DWORD *row = (DWORD *)((char *)locked.pBits + (size_t)y * locked.Pitch);
    for (UINT x = 0; x < desc.Width; x++) {
      const bool right = x >= desc.Width / 2, bottom = y >= desc.Height / 2;
      row[x] = bottom ? (right ? kWhite : kBlue) : (right ? kGreen : kRed);
    }
  }
  return SUCCEEDED(texture->UnlockRect(level));
}

UINT16
rgb565(D3DCOLOR colour) {
  const UINT r = (colour >> 16) & 0xFF, g = (colour >> 8) & 0xFF, b = colour & 0xFF;
  return (UINT16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// fills every block of a DXT1 level with one colour, encoded as colour0 with every index 0
bool
fill_dxt1_solid(IDirect3DTexture9 *texture, UINT level, D3DCOLOR colour) {
  D3DSURFACE_DESC desc{};
  if (FAILED(texture->GetLevelDesc(level, &desc)))
    return false;
  const UINT blocks_x = (desc.Width + 3) / 4, blocks_y = (desc.Height + 3) / 4;
  D3DLOCKED_RECT locked{};
  if (FAILED(texture->LockRect(level, &locked, nullptr, 0)))
    return false;
  const UINT16 colour0 = rgb565(colour);
  for (UINT by = 0; by < blocks_y; by++) {
    char *row = (char *)locked.pBits + (size_t)by * locked.Pitch;
    for (UINT bx = 0; bx < blocks_x; bx++) {
      char *block = row + (size_t)bx * 8;
      memcpy(block, &colour0, sizeof(colour0));
      const UINT16 colour1 = 0;
      memcpy(block + 2, &colour1, sizeof(colour1));
      const DWORD indices = 0;
      memcpy(block + 4, &indices, sizeof(indices));
    }
  }
  return SUCCEEDED(texture->UnlockRect(level));
}

// whether every block of a DXT1 level names `colour` as its colour0
bool
level_dxt1_is(IDirect3DTexture9 *texture, UINT level, D3DCOLOR colour, UINT &mismatches) {
  mismatches = 0;
  D3DSURFACE_DESC desc{};
  if (FAILED(texture->GetLevelDesc(level, &desc)))
    return false;
  D3DLOCKED_RECT locked{};
  if (FAILED(texture->LockRect(level, &locked, nullptr, D3DLOCK_READONLY)))
    return false;
  const UINT16 want = rgb565(colour);
  const UINT blocks_x = (desc.Width + 3) / 4, blocks_y = (desc.Height + 3) / 4;
  for (UINT by = 0; by < blocks_y; by++) {
    const char *row = (const char *)locked.pBits + (size_t)by * locked.Pitch;
    for (UINT bx = 0; bx < blocks_x; bx++) {
      UINT16 got = 0;
      memcpy(&got, row + (size_t)bx * 8, sizeof(got));
      if (got != want)
        mismatches++;
    }
  }
  texture->UnlockRect(level);
  return mismatches == 0;
}

} // namespace

int
main() {
  unsigned wrong = 0, checks = 0;
  auto expect = [&](bool ok, const char *what) {
    checks++;
    if (!ok && wrong++ < 20)
      printf("wrong: %s\n", what);
  };

  WNDCLASSA window_class{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr,
                         "d3d9_mips"};
  RegisterClassA(&window_class);
  RECT window_rect{0, 0, 64, 64};
  AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);
  HWND window = CreateWindowA("d3d9_mips", "d3d9_mips", WS_OVERLAPPEDWINDOW, 0, 0,
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
  present.BackBufferWidth = 64;
  present.BackBufferHeight = 64;
  present.BackBufferFormat = D3DFMT_X8R8G8B8;
  present.BackBufferCount = 1;
  present.MultiSampleType = D3DMULTISAMPLE_NONE;
  present.SwapEffect = D3DSWAPEFFECT_DISCARD;
  present.hDeviceWindow = window;
  present.Windowed = TRUE;
  present.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  ComPtr<IDirect3DDevice9> device;
  CHECK(d3d9->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &present, &device));

  // a frame's pixels, so a minified draw says which level the GPU actually sampled rather than which bytes the
  // managed texture's CPU-side shadow still holds
  ComPtr<IDirect3DSurface9> readback;
  CHECK(device->CreateOffscreenPlainSurface(64, 64, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr));
  ComPtr<IDirect3DSurface9> back_buffer;
  CHECK(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer));
  auto back_pixel = [&](UINT x, UINT y) -> D3DCOLOR {
    D3DLOCKED_RECT locked{};
    if (FAILED(device->GetRenderTargetData(back_buffer.Get(), readback.Get())) ||
        FAILED(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
      return 0;
    const DWORD got = ((const DWORD *)((const char *)locked.pBits + (size_t)y * locked.Pitch))[x];
    readback->UnlockRect();
    return got;
  };
  CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
  CHECK(device->SetFVF(kTexturedFVF));
  CHECK(device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT));
  CHECK(device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
  CHECK(device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_POINT));
  CHECK(device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP));
  CHECK(device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));
  // a 64 by 64 texture over a 4 by 4 quad: the derivatives select a level well below level 0, so the pixel names the
  // level the GPU sampled. every generated/authored level below 0 is one colour, so the pixel does not depend on which
  // of them the filter picked.
  auto draw_minified = [&](IDirect3DTexture9 *texture) -> D3DCOLOR {
    const float left = 16.0f, top = 16.0f, size = 4.0f;
    const TexturedVertex quad[6] = {
        {left, top, 0.0f, 1.0f, kWhite, 0.0f, 0.0f},
        {left, top + size, 0.0f, 1.0f, kWhite, 0.0f, 1.0f},
        {left + size, top, 0.0f, 1.0f, kWhite, 1.0f, 0.0f},
        {left + size, top, 0.0f, 1.0f, kWhite, 1.0f, 0.0f},
        {left, top + size, 0.0f, 1.0f, kWhite, 0.0f, 1.0f},
        {left + size, top + size, 0.0f, 1.0f, kWhite, 1.0f, 1.0f},
    };
    if (FAILED(device->SetTexture(0, texture)) || FAILED(device->Clear(0, nullptr, D3DCLEAR_TARGET, kBlack, 1.0f, 0)) ||
        FAILED(device->BeginScene()) ||
        FAILED(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, quad, sizeof(TexturedVertex))) ||
        FAILED(device->EndScene()))
      return 0;
    return back_pixel((UINT)(left + size / 2), (UINT)(top + size / 2));
  };

  // ---- a managed chain keeps the levels the application authors --------------------------------------------------
  {
    ComPtr<IDirect3DTexture9> texture;
    CHECK(device->CreateTexture(kSize, kSize, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr));
    expect(texture->GetLevelCount() == 4, "an 8 by 8 managed texture with no level count has not four levels");
    // the lower levels are authored first, then level 0: if unlocking level 0 generated the chain, it would replace
    // the three colours below it
    CHECK(fill_solid(texture.Get(), 3, kWhite));
    CHECK(fill_solid(texture.Get(), 2, kBlue));
    CHECK(fill_solid(texture.Get(), 1, kGreen));
    CHECK(fill_solid(texture.Get(), 0, kRed));
    UINT mismatches = 0;
    expect(level_is_solid(texture.Get(), 1, kGreen, mismatches),
           "unlocking level 0 replaced the authored level 1");
    expect(level_is_solid(texture.Get(), 2, kBlue, mismatches),
           "unlocking level 0 replaced the authored level 2");
    expect(level_is_solid(texture.Get(), 3, kWhite, mismatches),
           "unlocking level 0 replaced the authored level 3");
    printf("after level 0: level 1 mismatches %u, level 2 %u, level 3 %u\n",
           level_is_solid(texture.Get(), 1, kGreen, mismatches) ? 0u : mismatches,
           level_is_solid(texture.Get(), 2, kBlue, mismatches) ? 0u : mismatches,
           level_is_solid(texture.Get(), 3, kWhite, mismatches) ? 0u : mismatches);

    // a read-only lock of level 0 has nothing to upload, so it must not generate anything either
    CHECK(fill_solid(texture.Get(), 3, kWhite));
    CHECK(fill_solid(texture.Get(), 2, kBlue));
    CHECK(fill_solid(texture.Get(), 1, kGreen));
    D3DLOCKED_RECT locked{};
    CHECK(texture->LockRect(0, &locked, nullptr, D3DLOCK_READONLY));
    CHECK(texture->UnlockRect(0));
    expect(level_is_solid(texture.Get(), 1, kGreen, mismatches),
           "a read-only unlock of level 0 replaced the authored level 1");
    expect(level_is_solid(texture.Get(), 2, kBlue, mismatches),
           "a read-only unlock of level 0 replaced the authored level 2");
    expect(level_is_solid(texture.Get(), 3, kWhite, mismatches),
           "a read-only unlock of level 0 replaced the authored level 3");

    // GenerateMipSubLevels makes the chain out of level 0 on request: the four quadrants of level 0 are the four
    // quadrants of the 4 by 4 level 1, each texel the filter of a 2 by 2 block inside one quadrant
    CHECK(fill_quadrants(texture.Get(), 0));
    CHECK(fill_solid(texture.Get(), 1, kBlack));
    CHECK(fill_solid(texture.Get(), 2, kBlack));
    CHECK(fill_solid(texture.Get(), 3, kBlack));
    texture->GenerateMipSubLevels();
    const DWORD top_left = read_texel(texture.Get(), 1, 1, 1);
    const DWORD top_right = read_texel(texture.Get(), 1, 2, 1);
    const DWORD bottom_left = read_texel(texture.Get(), 1, 1, 2);
    const DWORD bottom_right = read_texel(texture.Get(), 1, 2, 2);
    auto close_to = [](DWORD got, D3DCOLOR want) {
      for (int shift = 0; shift <= 16; shift += 8) {
        const int a = (int)((got >> shift) & 0xFF), b = (int)((want >> shift) & 0xFF);
        if (a < b - 2 || a > b + 2)
          return false;
      }
      return true;
    };
    expect(close_to(top_left, kRed), "GenerateMipSubLevels did not filter level 0's red quadrant into level 1");
    expect(close_to(top_right, kGreen), "GenerateMipSubLevels did not filter level 0's green quadrant into level 1");
    expect(close_to(bottom_left, kBlue), "GenerateMipSubLevels did not filter level 0's blue quadrant into level 1");
    expect(close_to(bottom_right, kWhite),
           "GenerateMipSubLevels did not filter level 0's white quadrant into level 1");
    printf("generated level 1: %06lx %06lx / %06lx %06lx\n", (unsigned long)(top_left & 0xFFFFFF),
           (unsigned long)(top_right & 0xFFFFFF), (unsigned long)(bottom_left & 0xFFFFFF),
           (unsigned long)(bottom_right & 0xFFFFFF));
  }

  // ---- a compressed chain's levels are the application's blocks ------------------------------------------------
  {
    ComPtr<IDirect3DTexture9> texture;
    CHECK(device->CreateTexture(kSize, kSize, 0, 0, D3DFMT_DXT1, D3DPOOL_MANAGED, &texture, nullptr));
    expect(texture->GetLevelCount() == 4, "an 8 by 8 DXT1 texture with no level count has not four levels");
    CHECK(fill_dxt1_solid(texture.Get(), 3, kWhite));
    CHECK(fill_dxt1_solid(texture.Get(), 2, kBlue));
    CHECK(fill_dxt1_solid(texture.Get(), 1, kGreen));
    CHECK(fill_dxt1_solid(texture.Get(), 0, kRed));
    UINT mismatches = 0;
    expect(level_dxt1_is(texture.Get(), 1, kGreen, mismatches), "a DXT1 level 1 was replaced when level 0 was written");
    expect(level_dxt1_is(texture.Get(), 2, kBlue, mismatches), "a DXT1 level 2 was replaced when level 0 was written");
    expect(level_dxt1_is(texture.Get(), 3, kWhite, mismatches), "a DXT1 level 3 was replaced when level 0 was written");
    D3DLOCKED_RECT locked{};
    CHECK(texture->LockRect(0, &locked, nullptr, D3DLOCK_READONLY));
    CHECK(texture->UnlockRect(0));
    expect(level_dxt1_is(texture.Get(), 1, kGreen, mismatches),
           "a read-only unlock of a DXT1 level 0 replaced level 1");
  }

  // ---- what the GPU samples after level 0 is written, which is where an unintended generation shows -----------------
  {
    ComPtr<IDirect3DTexture9> texture;
    CHECK(device->CreateTexture(64, 64, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr));
    expect(texture->GetLevelCount() == 7, "a 64 by 64 texture with no level count has not seven levels");
    // every level below 0 is green; level 0 is red. a generated chain would make every lower level red too, and the
    // minified draw would read red instead of the authored green.
    for (UINT level = 1; level < texture->GetLevelCount(); level++)
      expect(fill_solid(texture.Get(), level, kGreen), "a managed level was not written");
    CHECK(fill_solid(texture.Get(), 0, kRed));
    const D3DCOLOR authored = draw_minified(texture.Get());
    expect((authored & 0x00FFFFFF) == (kGreen & 0x00FFFFFF),
           "a minified draw sampled a level generated from level 0 instead of the authored level");
    printf("minified after level 0: %06lx (green is the authored lower levels)\n", (unsigned long)(authored & 0xFFFFFF));

    // and a read-only lock of level 0 must not touch them either
    for (UINT level = 1; level < texture->GetLevelCount(); level++)
      CHECK(fill_solid(texture.Get(), level, kGreen));
    D3DLOCKED_RECT locked{};
    CHECK(texture->LockRect(0, &locked, nullptr, D3DLOCK_READONLY));
    CHECK(texture->UnlockRect(0));
    const D3DCOLOR after_read_only = draw_minified(texture.Get());
    expect((after_read_only & 0x00FFFFFF) == (kGreen & 0x00FFFFFF),
           "a read-only unlock of level 0 generated the levels below it");

    // an explicit GenerateMipSubLevels does generate them, from level 0's red
    texture->GenerateMipSubLevels();
    const D3DCOLOR generated = draw_minified(texture.Get());
    expect((generated & 0x00FFFFFF) == (kRed & 0x00FFFFFF),
           "GenerateMipSubLevels did not make the levels below level 0 from it");
    printf("minified after GenerateMipSubLevels: %06lx (red is generated from level 0)\n",
           (unsigned long)(generated & 0xFFFFFF));
  }

  // a compressed chain is the application's blocks, so a minified draw samples the authored level, not a generated one
  {
    ComPtr<IDirect3DTexture9> texture;
    CHECK(device->CreateTexture(64, 64, 0, 0, D3DFMT_DXT1, D3DPOOL_MANAGED, &texture, nullptr));
    for (UINT level = 1; level < texture->GetLevelCount(); level++)
      CHECK(fill_dxt1_solid(texture.Get(), level, kGreen));
    CHECK(fill_dxt1_solid(texture.Get(), 0, kRed));
    const D3DCOLOR compressed = draw_minified(texture.Get());
    expect((compressed & 0x00FFFFFF) == (kGreen & 0x00FFFFFF),
           "a minified DXT1 draw sampled a level generated from level 0");
    printf("minified DXT1: %06lx\n", (unsigned long)(compressed & 0xFFFFFF));
  }

  // ---- level counts, and the combinations D3D9 refuses ----------------------------------------------------------
  {
    ComPtr<IDirect3DTexture9> two_levels;
    CHECK(device->CreateTexture(kSize, kSize, 2, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &two_levels, nullptr));
    expect(two_levels->GetLevelCount() == 2, "a texture made with two levels has not two levels");
    D3DSURFACE_DESC desc{};
    CHECK(two_levels->GetLevelDesc(1, &desc));
    expect(desc.Width == kSize / 2 && desc.Height == kSize / 2, "the second level of an 8 by 8 texture is not 4 by 4");

    ComPtr<IDirect3DTexture9> too_many;
    expect(device->CreateTexture(kSize, kSize, 5, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &too_many, nullptr) ==
               D3DERR_INVALIDCALL,
           "a texture with more levels than its chain was made");

    // D3DUSAGE_AUTOGENMIPMAP is not valid with D3DPOOL_MANAGED (Microsoft Learn, D3DPOOL)
    ComPtr<IDirect3DTexture9> managed_auto;
    expect(device->CreateTexture(kSize, kSize, 0, D3DUSAGE_AUTOGENMIPMAP, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                 &managed_auto, nullptr) == D3DERR_INVALIDCALL,
           "a managed texture was made with D3DUSAGE_AUTOGENMIPMAP");

    // every format this layer accepts keeps the chain it was asked for, even where the device cannot generate levels
    const D3DFORMAT formats[] = {D3DFMT_A8R8G8B8, D3DFMT_X8R8G8B8, D3DFMT_A2R10G10B10, D3DFMT_A2B10G10R10,
                                 D3DFMT_R5G6B5,    D3DFMT_A16B16G16R16F, D3DFMT_R32F, D3DFMT_DXT2,
                                 D3DFMT_DXT3,      D3DFMT_DXT4,         D3DFMT_DXT5};
    for (D3DFORMAT format : formats) {
      ComPtr<IDirect3DTexture9> one;
      const HRESULT hr = device->CreateTexture(kSize, kSize, 4, 0, format, D3DPOOL_MANAGED, &one, nullptr);
      expect(SUCCEEDED(hr) && one && one->GetLevelCount() == 4,
             "a managed texture's requested level chain was reduced");
      if (FAILED(hr) || !one || one->GetLevelCount() != 4)
        printf("       format %u: hr 0x%08lx, levels %lu\n", (unsigned)format, (unsigned long)hr,
               one ? (unsigned long)one->GetLevelCount() : 0ul);
    }
  }

  device.Reset();
  d3d9.Reset();
  DestroyWindow(window);

  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
