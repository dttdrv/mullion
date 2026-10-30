// contract: the copies between a frame's surfaces that a game makes outside a draw, with D3D9's rules for each:
// - ColorFill fills a default-pool surface, whole or in a rectangle, and refuses a system-memory one.
// - StretchRect copies a rectangle of one default-pool surface to another of the same format when the two rectangles are
//   the same size; a stretched copy, a format conversion, an overlapping copy on one surface and a system-memory
//   destination are refused (src/d3d9/README says what is not there yet).
// - UpdateSurface copies a system-memory surface's rectangle into a default-pool one at a point, with no format
//   conversion and no stretch; the bytes the application locked are the bytes that arrive, and the destination's other
//   pixels are unchanged.
// - UpdateTexture copies a managed texture's levels into a default-pool texture of the same format, size and level
//   count, each level as the application authored it; a level count that differs, a default-pool source, a managed
//   destination and one texture named as both are refused.
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

// one pixel of a surface, read back through a system-memory surface of the surface's own size and format: B, G, R, A
static bool read_pixel(IDirect3DDevice9 *device, IDirect3DSurface9 *surface, UINT x, UINT y, BYTE pixel[4]) {
  D3DSURFACE_DESC desc{};
  if (FAILED(surface->GetDesc(&desc)))
    return false;
  ComPtr<IDirect3DSurface9> staging;
  if (FAILED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &staging,
                                                 nullptr)))
    return false;
  if (FAILED(device->GetRenderTargetData(surface, staging.Get())))
    return false;
  D3DLOCKED_RECT locked{};
  if (FAILED(staging->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
    return false;
  std::memcpy(pixel, static_cast<const char *>(locked.pBits) + (size_t)y * locked.Pitch + (size_t)x * 4, 4);
  staging->UnlockRect();
  return true;
}

static bool is_colour(const BYTE pixel[4], BYTE r, BYTE g, BYTE b) {
  return pixel[2] == r && pixel[1] == g && pixel[0] == b;
}

int
main() {
  unsigned wrong = 0, checks = 0;
  auto expect = [&](bool ok, const char *what) {
    checks++;
    if (!ok && wrong++ < 24)
      printf("wrong: %s\n", what);
  };

  WNDCLASSA window_class{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr,
                         "d3d9_transfer"};
  RegisterClassA(&window_class);
  RECT window_rect{0, 0, 64, 64};
  AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);
  HWND window = CreateWindowA("d3d9_transfer", "d3d9_transfer", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0,
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

  // a default-pool offscreen surface: ColorFill fills it, whole and in part
  ComPtr<IDirect3DSurface9> target;
  CHECK(device->CreateOffscreenPlainSurface(16, 16, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &target, nullptr));
  D3DSURFACE_DESC target_desc{};
  CHECK(target->GetDesc(&target_desc));
  expect(target_desc.Pool == D3DPOOL_DEFAULT && target_desc.Width == 16 && target_desc.Height == 16,
         "the default-pool offscreen surface is not a default-pool surface of 16x16");

  CHECK(device->ColorFill(target.Get(), nullptr, D3DCOLOR_XRGB(0, 0, 0)));
  BYTE pixel[4] = {};
  expect(read_pixel(device.Get(), target.Get(), 0, 0, pixel) && is_colour(pixel, 0, 0, 0),
         "ColorFill did not fill the surface black");

  const RECT part{4, 4, 8, 8};
  CHECK(device->ColorFill(target.Get(), &part, D3DCOLOR_XRGB(255, 128, 0)));
  expect(read_pixel(device.Get(), target.Get(), 5, 5, pixel) && is_colour(pixel, 255, 128, 0),
         "ColorFill's rectangle is not in the surface");
  expect(read_pixel(device.Get(), target.Get(), 2, 2, pixel) && is_colour(pixel, 0, 0, 0),
         "ColorFill's rectangle filled a pixel outside it");
  expect(read_pixel(device.Get(), target.Get(), 8, 8, pixel) && is_colour(pixel, 0, 0, 0),
         "ColorFill's rectangle filled its right edge");

  // the same call on a surface whose bytes are the CPU's is refused
  {
    ComPtr<IDirect3DSurface9> system_memory;
    CHECK(device->CreateOffscreenPlainSurface(4, 4, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &system_memory, nullptr));
    expect(device->ColorFill(system_memory.Get(), nullptr, D3DCOLOR_XRGB(1, 2, 3)) == D3DERR_INVALIDCALL,
           "ColorFill filled a system-memory surface");
  }

  // UpdateSurface: the bytes the application locked reach the destination at the point it names, and no others
  {
    ComPtr<IDirect3DSurface9> source;
    CHECK(device->CreateOffscreenPlainSurface(8, 8, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &source, nullptr));
    D3DLOCKED_RECT locked{};
    CHECK(source->LockRect(&locked, nullptr, 0));
    for (UINT y = 0; y < 8; y++)
      for (UINT x = 0; x < 8; x++) {
        BYTE *at = static_cast<BYTE *>(locked.pBits) + (size_t)y * locked.Pitch + (size_t)x * 4;
        at[0] = 30; // B
        at[1] = 20; // G
        at[2] = 10; // R
        at[3] = 255;
      }
    CHECK(source->UnlockRect());

    CHECK(device->ColorFill(target.Get(), nullptr, D3DCOLOR_XRGB(0, 0, 0)));
    const RECT source_rect{2, 2, 6, 6};
    const POINT destination{3, 4};
    CHECK(device->UpdateSurface(source.Get(), &source_rect, target.Get(), &destination));
    expect(read_pixel(device.Get(), target.Get(), 4, 5, pixel) && is_colour(pixel, 10, 20, 30),
           "UpdateSurface's pixels are not at the point it names");
    expect(read_pixel(device.Get(), target.Get(), 6, 7, pixel) && is_colour(pixel, 10, 20, 30),
           "UpdateSurface did not copy the whole rectangle");
    expect(read_pixel(device.Get(), target.Get(), 2, 3, pixel) && is_colour(pixel, 0, 0, 0),
           "UpdateSurface copied a pixel before its destination point");
    expect(read_pixel(device.Get(), target.Get(), 7, 7, pixel) && is_colour(pixel, 0, 0, 0),
           "UpdateSurface copied a pixel after its rectangle");

    // a rectangle that does not fit, and a default-pool source, are refused
    const POINT out_of_bounds{13, 13};
    expect(device->UpdateSurface(source.Get(), &source_rect, target.Get(), &out_of_bounds) == D3DERR_INVALIDCALL,
           "UpdateSurface took a rectangle that does not fit the destination");
    expect(device->UpdateSurface(target.Get(), &source_rect, target.Get(), &destination) == D3DERR_INVALIDCALL,
           "UpdateSurface took a default-pool source");
  }

  // UpdateTexture: a managed texture's authored levels reach a default-pool texture of the same shape, level by level
  {
    ComPtr<IDirect3DTexture9> source_texture;
    CHECK(device->CreateTexture(8, 8, 4, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &source_texture, nullptr));
    for (UINT level = 0; level < 4; level++) {
      D3DLOCKED_RECT authoring{};
      CHECK(source_texture->LockRect(level, &authoring, nullptr, 0));
      const UINT size = 8u >> level;
      for (UINT y = 0; y < size; y++)
        for (UINT x = 0; x < size; x++) {
          BYTE *at = static_cast<BYTE *>(authoring.pBits) + (size_t)y * authoring.Pitch + (size_t)x * 4;
          at[0] = (BYTE)(level * 40);        // B
          at[1] = (BYTE)(10 + level);        // G
          at[2] = (BYTE)(200 - level);       // R
          at[3] = 255;
        }
      CHECK(source_texture->UnlockRect(level));
    }

    ComPtr<IDirect3DTexture9> destination_texture;
    CHECK(device->CreateTexture(8, 8, 4, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &destination_texture, nullptr));
    CHECK(device->UpdateTexture(source_texture.Get(), destination_texture.Get()));
    for (UINT level = 0; level < 4; level++) {
      ComPtr<IDirect3DSurface9> level_surface;
      CHECK(destination_texture->GetSurfaceLevel(level, &level_surface));
      D3DSURFACE_DESC level_desc{};
      CHECK(level_surface->GetDesc(&level_desc));
      ComPtr<IDirect3DSurface9> staging;
      CHECK(device->CreateOffscreenPlainSurface(level_desc.Width, level_desc.Height, D3DFMT_A8R8G8B8,
                                                D3DPOOL_SYSTEMMEM, &staging, nullptr));
      CHECK(device->GetRenderTargetData(level_surface.Get(), staging.Get()));
      D3DLOCKED_RECT readback{};
      CHECK(staging->LockRect(&readback, nullptr, D3DLOCK_READONLY));
      const BYTE *at = static_cast<const BYTE *>(readback.pBits);
      char what[96];
      snprintf(what, sizeof what, "UpdateTexture's level %u is not the level the source authored", level);
      expect(at[0] == (BYTE)(level * 40) && at[1] == (BYTE)(10 + level) && at[2] == (BYTE)(200 - level), what);
      staging->UnlockRect();
    }

    // the shape, the pools and the two objects have to be the ones D3D9's UpdateTexture takes
    ComPtr<IDirect3DTexture9> one_level;
    CHECK(device->CreateTexture(8, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &one_level, nullptr));
    expect(device->UpdateTexture(source_texture.Get(), one_level.Get()) == D3DERR_INVALIDCALL,
           "UpdateTexture took textures of different level counts");
    expect(device->UpdateTexture(one_level.Get(), destination_texture.Get()) == D3DERR_INVALIDCALL,
           "UpdateTexture took a default-pool source");
    expect(device->UpdateTexture(source_texture.Get(), source_texture.Get()) == D3DERR_INVALIDCALL,
           "UpdateTexture took one texture as both its textures");
    ComPtr<IDirect3DTexture9> managed_destination;
    CHECK(device->CreateTexture(8, 8, 4, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &managed_destination, nullptr));
    expect(device->UpdateTexture(source_texture.Get(), managed_destination.Get()) == D3DERR_INVALIDCALL,
           "UpdateTexture took a managed destination");
  }

  // A dynamic texture: its levels are the bytes the application maps, DISCARD throwing the level away and NOOVERWRITE
  // promising not to touch pixels the GPU may still read; a read-only lock is refused, as D3D9 refuses one
  {
    ComPtr<IDirect3DTexture9> dynamic_texture;
    CHECK(device->CreateTexture(8, 8, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &dynamic_texture,
                                nullptr));
    D3DSURFACE_DESC dynamic_desc{};
    CHECK(dynamic_texture->GetLevelDesc(0, &dynamic_desc));
    expect((dynamic_desc.Usage & D3DUSAGE_DYNAMIC) != 0, "a dynamic texture's level does not say it is dynamic");

    D3DLOCKED_RECT mapping{};
    expect(dynamic_texture->LockRect(0, &mapping, nullptr, D3DLOCK_READONLY) == D3DERR_INVALIDCALL,
           "a dynamic texture took a read-only lock");
    CHECK(dynamic_texture->LockRect(0, &mapping, nullptr, D3DLOCK_DISCARD));
    for (UINT y = 0; y < 8; y++)
      for (UINT x = 0; x < 8; x++) {
        BYTE *at = static_cast<BYTE *>(mapping.pBits) + (size_t)y * mapping.Pitch + (size_t)x * 4;
        at[0] = 30;
        at[1] = 20;
        at[2] = 10;
        at[3] = 255;
      }
    CHECK(dynamic_texture->UnlockRect(0));
    expect(dynamic_texture->UnlockRect(0) == D3DERR_INVALIDCALL, "a dynamic texture unlocked a level twice");

    ComPtr<IDirect3DSurface9> dynamic_level;
    CHECK(dynamic_texture->GetSurfaceLevel(0, &dynamic_level));
    expect(read_pixel(device.Get(), dynamic_level.Get(), 2, 2, pixel) && is_colour(pixel, 10, 20, 30),
           "a dynamic texture's discarded lock is not what it wrote");

    // NOOVERWRITE for a part the application has not written yet: the rest of the level keeps what it had
    const RECT appending{4, 0, 8, 8};
    CHECK(dynamic_texture->LockRect(0, &mapping, &appending, D3DLOCK_NOOVERWRITE));
    for (UINT y = 0; y < 8; y++)
      for (UINT x = 0; x < 4; x++) {
        BYTE *at = static_cast<BYTE *>(mapping.pBits) + (size_t)y * mapping.Pitch + (size_t)x * 4;
        at[0] = 0;
        at[1] = 200;
        at[2] = 0;
        at[3] = 255;
      }
    CHECK(dynamic_texture->UnlockRect(0));
    expect(read_pixel(device.Get(), dynamic_level.Get(), 5, 5, pixel) && is_colour(pixel, 0, 200, 0),
           "a dynamic texture's appended lock is not at the rectangle it names");
    expect(read_pixel(device.Get(), dynamic_level.Get(), 1, 5, pixel) && is_colour(pixel, 10, 20, 30),
           "a dynamic texture's appended lock overwrote pixels outside it");

    // the flags and pools that cannot go together
    expect(dynamic_texture->LockRect(0, &mapping, nullptr, D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE) ==
               D3DERR_INVALIDCALL,
           "a dynamic texture took DISCARD and NOOVERWRITE at once");
    ComPtr<IDirect3DTexture9> refused;
    expect(device->CreateTexture(8, 8, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &refused, nullptr) ==
               D3DERR_INVALIDCALL,
           "a dynamic texture in a managed pool is taken");
    expect(device->CreateTexture(8, 8, 1, D3DUSAGE_DYNAMIC | D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                 D3DPOOL_DEFAULT, &refused, nullptr) == D3DERR_INVALIDCALL,
           "a texture that is dynamic and a render target at once is taken");
  }

  // StretchRect: an unscaled copy from one default surface to another, at an offset, and the refusals
  {
    ComPtr<IDirect3DSurface9> other;
    CHECK(device->CreateOffscreenPlainSurface(16, 16, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &other, nullptr));
    CHECK(device->ColorFill(target.Get(), nullptr, D3DCOLOR_XRGB(255, 0, 0)));
    CHECK(device->StretchRect(target.Get(), nullptr, other.Get(), nullptr, D3DTEXF_NONE));
    expect(read_pixel(device.Get(), other.Get(), 0, 0, pixel) && is_colour(pixel, 255, 0, 0),
           "StretchRect did not copy the surface");
    expect(read_pixel(device.Get(), other.Get(), 15, 15, pixel) && is_colour(pixel, 255, 0, 0),
           "StretchRect did not copy the whole surface");

    CHECK(device->ColorFill(other.Get(), nullptr, D3DCOLOR_XRGB(0, 0, 0)));
    const RECT source_rect{0, 0, 8, 8};
    const RECT destination_rect{8, 8, 16, 16};
    CHECK(device->StretchRect(target.Get(), &source_rect, other.Get(), &destination_rect, D3DTEXF_POINT));
    expect(read_pixel(device.Get(), other.Get(), 8, 8, pixel) && is_colour(pixel, 255, 0, 0),
           "StretchRect's rectangle is not at the destination it names");
    expect(read_pixel(device.Get(), other.Get(), 0, 0, pixel) && is_colour(pixel, 0, 0, 0),
           "StretchRect copied outside its destination rectangle");

    const RECT stretched{0, 0, 16, 16};
    expect(device->StretchRect(target.Get(), &source_rect, other.Get(), &stretched, D3DTEXF_LINEAR) ==
               D3DERR_INVALIDCALL,
           "StretchRect stretched a copy");

    ComPtr<IDirect3DSurface9> xrgb;
    CHECK(device->CreateOffscreenPlainSurface(16, 16, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &xrgb, nullptr));
    expect(device->StretchRect(target.Get(), nullptr, xrgb.Get(), nullptr, D3DTEXF_NONE) == D3DERR_INVALIDCALL,
           "StretchRect converted a format");

    ComPtr<IDirect3DSurface9> system_memory;
    CHECK(device->CreateOffscreenPlainSurface(16, 16, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &system_memory, nullptr));
    expect(device->StretchRect(target.Get(), nullptr, system_memory.Get(), nullptr, D3DTEXF_NONE) ==
               D3DERR_INVALIDCALL,
           "StretchRect took a system-memory destination");
  }

  // a render-target texture: a frame is drawn into one of its levels, and that level reads back what was drawn
  {
    ComPtr<IDirect3DTexture9> target_texture;
    CHECK(device->CreateTexture(16, 16, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &target_texture,
                                nullptr));
    ComPtr<IDirect3DSurface9> level;
    CHECK(target_texture->GetSurfaceLevel(0, &level));
    D3DSURFACE_DESC level_desc{};
    CHECK(level->GetDesc(&level_desc));
    expect((level_desc.Usage & D3DUSAGE_RENDERTARGET) != 0,
           "a texture level made as a render target does not say it is one");
    CHECK(device->SetRenderTarget(0, level.Get()));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(12, 34, 56), 1.0f, 0));
    expect(read_pixel(device.Get(), level.Get(), 8, 8, pixel) && is_colour(pixel, 12, 34, 56),
           "a render-target texture's level is not what the frame cleared");
    // the frame's target is the back buffer again
    ComPtr<IDirect3DSurface9> back_buffer;
    CHECK(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer));
    CHECK(device->SetRenderTarget(0, back_buffer.Get()));
  }

  // a dynamic texture's DISCARD lock forgets earlier pixels; NOOVERWRITE writes a rectangle and keeps the rest
  // (Microsoft Learn, D3DLOCK_DISCARD, D3DLOCK_NOOVERWRITE)
  {
    ComPtr<IDirect3DTexture9> dynamic;
    CHECK(device->CreateTexture(8, 8, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &dynamic, nullptr));
    D3DLOCKED_RECT locked{};
    CHECK(dynamic->LockRect(0, &locked, nullptr, D3DLOCK_DISCARD));
    for (UINT y = 0; y < 8; y++)
      for (UINT x = 0; x < 8; x++) {
        DWORD *at = (DWORD *)((char *)locked.pBits + y * locked.Pitch + x * 4);
        *at = D3DCOLOR_XRGB(255, 0, 0);
      }
    CHECK(dynamic->UnlockRect(0));
    const RECT keep{2, 2, 4, 4};
    CHECK(dynamic->LockRect(0, &locked, &keep, D3DLOCK_NOOVERWRITE));
    for (UINT y = 0; y < 2; y++)
      for (UINT x = 0; x < 2; x++) {
        DWORD *at = (DWORD *)((char *)locked.pBits + y * locked.Pitch + x * 4);
        *at = D3DCOLOR_XRGB(0, 255, 0);
      }
    CHECK(dynamic->UnlockRect(0));
    ComPtr<IDirect3DSurface9> source, dest;
    CHECK(dynamic->GetSurfaceLevel(0, &source));
    CHECK(device->CreateOffscreenPlainSurface(8, 8, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &dest, nullptr));
    CHECK(device->StretchRect(source.Get(), nullptr, dest.Get(), nullptr, D3DTEXF_NONE));
    expect(read_pixel(device.Get(), dest.Get(), 0, 0, pixel) && is_colour(pixel, 255, 0, 0),
           "D3DLOCK_DISCARD did not keep the texels a later NOOVERWRITE left alone");
    expect(read_pixel(device.Get(), dest.Get(), 2, 2, pixel) && is_colour(pixel, 0, 255, 0),
           "D3DLOCK_NOOVERWRITE did not write its rectangle");
  }

  // the implicit swap chain's back buffer is a default surface ColorFill fills too
  {
    ComPtr<IDirect3DSurface9> back_buffer;
    CHECK(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer));
    CHECK(device->ColorFill(back_buffer.Get(), nullptr, D3DCOLOR_XRGB(0, 255, 0)));
    expect(read_pixel(device.Get(), back_buffer.Get(), 32, 32, pixel) && is_colour(pixel, 0, 255, 0),
           "ColorFill did not fill the back buffer");
  }

  device.Reset();
  d3d9.Reset();
  DestroyWindow(window);

  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
