// contract: what a window's chain presents and what a render target holds, each read back out of the pixels:
// - GetFrontBufferData (the device's and the swap chain's) copies the frame the chain presented last into a system-memory
//   A8R8G8B8 surface, opaque; it is the frame shown, not the one being drawn, and before the first Present there is none
//   to read (D3DERR_INVALIDCALL). A default-pool surface, a swap chain the device has not and a null surface are refused.
// - Reset takes D3DSWAPEFFECT_FLIP and D3DSWAPEFFECT_COPY as well as DISCARD. A COPY chain's Present leaves the back
//   buffer as it was; the chain reports the effect it was made with.
// - Reset takes a multisampled DISCARD chain: the back buffer and the automatic depth-stencil surface are multisampled,
//   a draw into them is antialiased, StretchRect resolves the back buffer and Present resolves it into the frame shown;
//   GetRenderTargetData of a multisampled surface, and a multisampled chain that is not a DISCARD one, are refused.
// - CreateRenderTarget with Lockable TRUE makes a render target whose bytes LockRect reads (the pixels drawn) and writes
//   (the pixels drawn from then on); a lockable multisampled target is refused, and so is a lock of a target that is
//   not lockable.
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

constexpr UINT kSize = 64;

struct Vertex {
  float x, y, z, rhw;
  D3DCOLOR colour;
};

// one pixel (B, G, R, A) of a system-memory surface
bool surface_pixel(IDirect3DSurface9 *staging, UINT x, UINT y, BYTE pixel[4]) {
  D3DLOCKED_RECT locked{};
  if (FAILED(staging->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
    return false;
  std::memcpy(pixel, static_cast<const char *>(locked.pBits) + (size_t)y * locked.Pitch + (size_t)x * 4, 4);
  staging->UnlockRect();
  return true;
}

// one pixel of a default-pool surface, read back through a system-memory surface of its size and format
bool read_pixel(IDirect3DDevice9 *device, IDirect3DSurface9 *surface, UINT x, UINT y, BYTE pixel[4]) {
  D3DSURFACE_DESC desc{};
  if (FAILED(surface->GetDesc(&desc)))
    return false;
  ComPtr<IDirect3DSurface9> staging;
  if (FAILED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &staging,
                                                 nullptr)) ||
      FAILED(device->GetRenderTargetData(surface, staging.Get())))
    return false;
  return surface_pixel(staging.Get(), x, y, pixel);
}

bool is_colour(const BYTE pixel[4], BYTE r, BYTE g, BYTE b) {
  return pixel[2] == r && pixel[1] == g && pixel[0] == b;
}

// a window, the Direct3D 9 object and a device over them
struct Rig {
  HWND window = nullptr;
  ComPtr<IDirect3D9> d3d9;
  ComPtr<IDirect3DDevice9> device;
  D3DPRESENT_PARAMETERS present{};

  bool create() {
    WNDCLASSA window_class{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr,
                           "d3d9_chain"};
    RegisterClassA(&window_class);
    RECT rect{0, 0, (LONG)kSize, (LONG)kSize};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    window = CreateWindowA("d3d9_chain", "d3d9_chain", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, rect.right - rect.left,
                           rect.bottom - rect.top, nullptr, nullptr, window_class.hInstance, nullptr);
    if (!window)
      return false;
    d3d9.Attach(Direct3DCreate9(D3D_SDK_VERSION));
    if (!d3d9)
      return false;
    present = parameters(D3DSWAPEFFECT_DISCARD, D3DMULTISAMPLE_NONE);
    return SUCCEEDED(d3d9->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &present,
                                        &device)) &&
           device;
  }

  D3DPRESENT_PARAMETERS parameters(D3DSWAPEFFECT effect, D3DMULTISAMPLE_TYPE samples) const {
    D3DPRESENT_PARAMETERS p{};
    p.BackBufferWidth = kSize;
    p.BackBufferHeight = kSize;
    p.BackBufferFormat = D3DFMT_X8R8G8B8;
    p.BackBufferCount = 1;
    p.MultiSampleType = samples;
    p.SwapEffect = effect;
    p.EnableAutoDepthStencil = TRUE;
    p.AutoDepthStencilFormat = D3DFMT_D24S8;
    p.hDeviceWindow = window;
    p.Windowed = TRUE;
    p.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    return p;
  }

  ~Rig() {
    device.Reset();
    d3d9.Reset();
    if (window)
      DestroyWindow(window);
  }
};

// a triangle whose hypotenuse is a diagonal (so a multisampled draw has pixels it covers in part), white, in screen space
HRESULT draw_triangle(IDirect3DDevice9 *device) {
  const Vertex vertices[3] = {{4.0f, 4.0f, 0.5f, 1.0f, 0xFFFFFFFF},
                              {60.0f, 4.0f, 0.5f, 1.0f, 0xFFFFFFFF},
                              {4.0f, 60.0f, 0.5f, 1.0f, 0xFFFFFFFF}};
  device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  device->SetRenderState(D3DRS_ZENABLE, FALSE);
  device->SetRenderState(D3DRS_LIGHTING, FALSE);
  HRESULT hr = device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
  if (FAILED(hr))
    return hr;
  hr = device->BeginScene();
  if (FAILED(hr))
    return hr;
  hr = device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, vertices, sizeof(Vertex));
  device->EndScene();
  return hr;
}

} // namespace

int
main() {
  unsigned wrong = 0, checks = 0;
  auto expect = [&](bool ok, const char *what) {
    checks++;
    if (!ok && wrong++ < 30)
      printf("wrong: %s\n", what);
  };

  Rig rig;
  if (!rig.create()) {
    printf("failed: no window or no device\n");
    return 1;
  }
  IDirect3DDevice9 *device = rig.device.Get();
  BYTE pixel[4] = {};

  ComPtr<IDirect3DSurface9> front;
  CHECK(device->CreateOffscreenPlainSurface(kSize, kSize, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &front, nullptr));

  // ---- GetFrontBufferData: the frame shown ----
  // nothing has been presented, so there is no frame of the chain's to read
  expect(device->GetFrontBufferData(0, front.Get()) == D3DERR_INVALIDCALL,
         "GetFrontBufferData answered before anything was presented");

  CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255, 0, 0), 1.0f, 0));
  CHECK(device->Present(nullptr, nullptr, nullptr, nullptr));
  CHECK(device->GetFrontBufferData(0, front.Get()));
  expect(surface_pixel(front.Get(), 10, 10, pixel) && is_colour(pixel, 255, 0, 0), "the front buffer is not the red frame");
  expect(pixel[3] == 0xFF, "the front buffer's alpha is not opaque");
  expect(surface_pixel(front.Get(), 63, 63, pixel) && is_colour(pixel, 255, 0, 0),
         "the front buffer's last pixel is not the red frame");

  CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 255, 0), 1.0f, 0));
  CHECK(device->Present(nullptr, nullptr, nullptr, nullptr));
  CHECK(device->GetFrontBufferData(0, front.Get()));
  expect(surface_pixel(front.Get(), 10, 10, pixel) && is_colour(pixel, 0, 255, 0),
         "the front buffer is not the frame presented last");

  // the frame being drawn is not the one shown
  CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 255), 1.0f, 0));
  CHECK(device->GetFrontBufferData(0, front.Get()));
  expect(surface_pixel(front.Get(), 10, 10, pixel) && is_colour(pixel, 0, 255, 0),
         "the front buffer changed before the next Present");

  {
    ComPtr<IDirect3DSwapChain9> chain;
    CHECK(device->GetSwapChain(0, &chain));
    CHECK(chain->GetFrontBufferData(front.Get()));
    expect(surface_pixel(front.Get(), 20, 20, pixel) && is_colour(pixel, 0, 255, 0),
           "the swap chain's front buffer is not the frame the device presented");
    expect(chain->GetFrontBufferData(nullptr) == D3DERR_INVALIDCALL, "the chain took a null destination");
  }
  expect(device->GetFrontBufferData(1, front.Get()) == D3DERR_INVALIDCALL, "GetFrontBufferData took a second chain");
  expect(device->GetFrontBufferData(0, nullptr) == D3DERR_INVALIDCALL, "GetFrontBufferData took a null surface");
  {
    ComPtr<IDirect3DSurface9> gpu;
    CHECK(device->CreateOffscreenPlainSurface(kSize, kSize, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &gpu, nullptr));
    expect(device->GetFrontBufferData(0, gpu.Get()) == D3DERR_INVALIDCALL,
           "GetFrontBufferData wrote a default-pool surface");
  }

  // an additional chain keeps its own front buffer
  {
    D3DPRESENT_PARAMETERS extra = rig.parameters(D3DSWAPEFFECT_DISCARD, D3DMULTISAMPLE_NONE);
    extra.EnableAutoDepthStencil = FALSE;
    // a window carries one chain, so the additional chain has a window of its own
    HWND second_window = CreateWindowA("d3d9_chain", "d3d9_chain2", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 80, 0, 96, 96,
                                       nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
    extra.hDeviceWindow = second_window;
    ComPtr<IDirect3DSwapChain9> second;
    if (SUCCEEDED(device->CreateAdditionalSwapChain(&extra, &second))) {
      ComPtr<IDirect3DSurface9> second_back;
      CHECK(second->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &second_back));
      expect(second->GetFrontBufferData(front.Get()) == D3DERR_INVALIDCALL,
             "an additional chain had a front buffer before it presented");
      CHECK(device->ColorFill(second_back.Get(), nullptr, D3DCOLOR_XRGB(200, 100, 50)));
      CHECK(second->Present(nullptr, nullptr, nullptr, nullptr, 0));
      CHECK(second->GetFrontBufferData(front.Get()));
      expect(surface_pixel(front.Get(), 5, 5, pixel) && is_colour(pixel, 200, 100, 50),
             "the additional chain's front buffer is not its own frame");
    } else {
      expect(false, "no additional swap chain");
    }
    second.Reset();
    if (second_window)
      DestroyWindow(second_window);
  }

  // asking for a back buffer does not choose the frame Present shows. The chain has two buffers of distinguishable
  // contents, and the application asks for them in an order that ends on buffer 1; Present still shows buffer 0, as
  // wined3d_present shows swapchain->back_buffers[0]. A second Present, after asking for buffer 1 only, shows it too.
  {
    D3DPRESENT_PARAMETERS multi = rig.parameters(D3DSWAPEFFECT_DISCARD, D3DMULTISAMPLE_NONE);
    multi.EnableAutoDepthStencil = FALSE;
    multi.BackBufferCount = 2;
    HWND multi_window = CreateWindowA("d3d9_chain", "d3d9_chain3", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 160, 0, 96, 96,
                                      nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
    multi.hDeviceWindow = multi_window;
    ComPtr<IDirect3DSwapChain9> chain;
    if (multi_window && SUCCEEDED(device->CreateAdditionalSwapChain(&multi, &chain))) {
      ComPtr<IDirect3DSurface9> zero, one, again;
      CHECK(chain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &zero));
      CHECK(device->ColorFill(zero.Get(), nullptr, D3DCOLOR_XRGB(1, 2, 3)));
      CHECK(chain->GetBackBuffer(1, D3DBACKBUFFER_TYPE_MONO, &one));
      CHECK(device->ColorFill(one.Get(), nullptr, D3DCOLOR_XRGB(200, 100, 50)));
      CHECK(chain->GetBackBuffer(1, D3DBACKBUFFER_TYPE_MONO, &again));
      expect(again.Get() == one.Get(), "GetBackBuffer(1) returned a different surface");
      again.Reset();
      CHECK(chain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &again));
      expect(again.Get() == zero.Get(), "GetBackBuffer(0) returned a different surface");
      again.Reset();
      CHECK(chain->Present(nullptr, nullptr, nullptr, nullptr, 0));
      CHECK(chain->GetFrontBufferData(front.Get()));
      expect(surface_pixel(front.Get(), 5, 5, pixel) && is_colour(pixel, 1, 2, 3),
             "the additional chain presented the last buffer asked for, not its buffer 0");
      CHECK(chain->GetBackBuffer(1, D3DBACKBUFFER_TYPE_MONO, &one));
      CHECK(chain->Present(nullptr, nullptr, nullptr, nullptr, 0));
      CHECK(chain->GetFrontBufferData(front.Get()));
      expect(surface_pixel(front.Get(), 5, 5, pixel) && is_colour(pixel, 1, 2, 3),
             "retrieving buffer 1 changed the additional chain's presented frame");
    } else {
      expect(false, "no two-buffer additional swap chain");
    }
    chain.Reset();
    if (multi_window)
      DestroyWindow(multi_window);
  }

  // ---- swap effects ----
  {
    D3DPRESENT_PARAMETERS copy = rig.parameters(D3DSWAPEFFECT_COPY, D3DMULTISAMPLE_NONE);
    CHECK(device->Reset(&copy));
    ComPtr<IDirect3DSwapChain9> chain;
    CHECK(device->GetSwapChain(0, &chain));
    D3DPRESENT_PARAMETERS got{};
    CHECK(chain->GetPresentParameters(&got));
    expect(got.SwapEffect == D3DSWAPEFFECT_COPY, "the chain does not report the COPY effect it was reset to");

    // a COPY chain's Present leaves the back buffer as it was: the clear, and what was drawn over it
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(10, 200, 30), 1.0f, 0));
    CHECK(draw_triangle(device));
    ComPtr<IDirect3DSurface9> back;
    CHECK(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back));
    expect(read_pixel(device, back.Get(), 20, 20, pixel) && is_colour(pixel, 255, 255, 255),
           "the triangle is not in the COPY chain's back buffer");
    CHECK(device->Present(nullptr, nullptr, nullptr, nullptr));
    expect(read_pixel(device, back.Get(), 20, 20, pixel) && is_colour(pixel, 255, 255, 255),
           "a COPY chain's Present changed the triangle in its back buffer");
    expect(read_pixel(device, back.Get(), 2, 2, pixel) && is_colour(pixel, 10, 200, 30),
           "a COPY chain's Present changed the clear in its back buffer");
    expect(read_pixel(device, back.Get(), 62, 62, pixel) && is_colour(pixel, 10, 200, 30),
           "a COPY chain's Present changed the clear in its back buffer's last pixel");
    CHECK(device->GetFrontBufferData(0, front.Get()));
    expect(surface_pixel(front.Get(), 20, 20, pixel) && is_colour(pixel, 255, 255, 255) &&
               surface_pixel(front.Get(), 2, 2, pixel) && is_colour(pixel, 10, 200, 30),
           "a COPY chain's front buffer is not the frame it presented");
    // a second frame drawn on the preserved one
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(90, 91, 92), 1.0f, 0));
    CHECK(device->Present(nullptr, nullptr, nullptr, nullptr));
    expect(read_pixel(device, back.Get(), 2, 2, pixel) && is_colour(pixel, 90, 91, 92),
           "a COPY chain's second frame is not kept either");
    back.Reset();
  }
  {
    D3DPRESENT_PARAMETERS flip = rig.parameters(D3DSWAPEFFECT_FLIP, D3DMULTISAMPLE_NONE);
    CHECK(device->Reset(&flip));
    ComPtr<IDirect3DSwapChain9> chain;
    CHECK(device->GetSwapChain(0, &chain));
    D3DPRESENT_PARAMETERS got{};
    CHECK(chain->GetPresentParameters(&got));
    expect(got.SwapEffect == D3DSWAPEFFECT_FLIP, "the chain does not report the FLIP effect it was reset to");
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(1, 2, 3), 1.0f, 0));
    CHECK(device->Present(nullptr, nullptr, nullptr, nullptr));
    CHECK(device->GetFrontBufferData(0, front.Get()));
    expect(surface_pixel(front.Get(), 7, 7, pixel) && is_colour(pixel, 1, 2, 3), "a FLIP chain did not show its frame");
    // and a frame after it draws and shows too
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(4, 5, 6), 1.0f, 0));
    CHECK(device->Present(nullptr, nullptr, nullptr, nullptr));
    CHECK(device->GetFrontBufferData(0, front.Get()));
    expect(surface_pixel(front.Get(), 7, 7, pixel) && is_colour(pixel, 4, 5, 6), "a FLIP chain's next frame was not shown");
  }

  // ---- a multisampled chain ----
  const D3DMULTISAMPLE_TYPE samples = D3DMULTISAMPLE_4_SAMPLES;
  DWORD quality = 0;
  const bool have_msaa = SUCCEEDED(rig.d3d9->CheckDeviceMultiSampleType(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, TRUE,
                                                                        samples, &quality)) &&
                         quality > 0;
  if (have_msaa) {
    // only a discard chain is multisampled, and the chain is as it was after the refusal
    D3DPRESENT_PARAMETERS bad = rig.parameters(D3DSWAPEFFECT_FLIP, samples);
    expect(device->Reset(&bad) == D3DERR_INVALIDCALL, "Reset to a multisampled FLIP chain succeeded");

    D3DPRESENT_PARAMETERS msaa = rig.parameters(D3DSWAPEFFECT_DISCARD, samples);
    CHECK(device->Reset(&msaa));
    ComPtr<IDirect3DSwapChain9> chain;
    CHECK(device->GetSwapChain(0, &chain));
    D3DPRESENT_PARAMETERS got{};
    CHECK(chain->GetPresentParameters(&got));
    expect(got.MultiSampleType == samples, "the chain does not report the multisample count it was reset to");

    ComPtr<IDirect3DSurface9> back, depth;
    CHECK(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back));
    CHECK(device->GetDepthStencilSurface(&depth));
    D3DSURFACE_DESC back_desc{}, depth_desc{};
    CHECK(back->GetDesc(&back_desc));
    CHECK(depth->GetDesc(&depth_desc));
    expect(back_desc.MultiSampleType == samples && back_desc.Width == kSize && back_desc.Height == kSize,
           "the back buffer of a multisampled chain is not multisampled and the chain's size");
    expect(depth_desc.MultiSampleType == samples, "the automatic depth-stencil surface is not multisampled");
    expect(device->GetRenderTargetData(back.Get(), front.Get()) == D3DERR_INVALIDCALL,
           "a multisampled back buffer was read back without a resolve");

    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0));
    CHECK(draw_triangle(device));

    // the resolve: inside the triangle is white, outside black, and the hypotenuse has pixels it covers in part
    ComPtr<IDirect3DSurface9> resolved;
    CHECK(device->CreateRenderTarget(kSize, kSize, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr));
    CHECK(device->StretchRect(back.Get(), nullptr, resolved.Get(), nullptr, D3DTEXF_POINT));
    ComPtr<IDirect3DSurface9> staging;
    CHECK(device->CreateOffscreenPlainSurface(kSize, kSize, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &staging, nullptr));
    CHECK(device->GetRenderTargetData(resolved.Get(), staging.Get()));
    expect(surface_pixel(staging.Get(), 10, 10, pixel) && is_colour(pixel, 255, 255, 255),
           "the resolved multisampled frame is not white inside the triangle");
    expect(surface_pixel(staging.Get(), 58, 58, pixel) && is_colour(pixel, 0, 0, 0),
           "the resolved multisampled frame is not black outside the triangle");
    unsigned partial = 0;
    {
      D3DLOCKED_RECT locked{};
      CHECK(staging->LockRect(&locked, nullptr, D3DLOCK_READONLY));
      for (UINT y = 0; y < kSize; y++)
        for (UINT x = 0; x < kSize; x++) {
          const BYTE *at = static_cast<const BYTE *>(locked.pBits) + (size_t)y * locked.Pitch + (size_t)x * 4;
          if (at[0] != 0 && at[0] != 255)
            partial++;
        }
      staging->UnlockRect();
    }
    expect(partial > 20, "a multisampled draw has no pixels of partial coverage along its edge");

    // Present resolves the back buffer into the frame shown
    CHECK(device->Present(nullptr, nullptr, nullptr, nullptr));
    CHECK(device->GetFrontBufferData(0, front.Get()));
    expect(surface_pixel(front.Get(), 10, 10, pixel) && is_colour(pixel, 255, 255, 255),
           "the frame a multisampled chain showed is not white inside the triangle");
    expect(surface_pixel(front.Get(), 58, 58, pixel) && is_colour(pixel, 0, 0, 0),
           "the frame a multisampled chain showed is not black outside the triangle");
    unsigned shown_partial = 0;
    {
      D3DLOCKED_RECT locked{};
      CHECK(front->LockRect(&locked, nullptr, D3DLOCK_READONLY));
      for (UINT y = 0; y < kSize; y++)
        for (UINT x = 0; x < kSize; x++) {
          const BYTE *at = static_cast<const BYTE *>(locked.pBits) + (size_t)y * locked.Pitch + (size_t)x * 4;
          if (at[0] != 0 && at[0] != 255)
            shown_partial++;
        }
      front->UnlockRect();
    }
    expect(shown_partial > 20, "the frame a multisampled chain showed is not resolved");
    // the back buffer is the device's and the next frame draws into it
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 128), 1.0f, 0));
    CHECK(device->Present(nullptr, nullptr, nullptr, nullptr));
    CHECK(device->GetFrontBufferData(0, front.Get()));
    expect(surface_pixel(front.Get(), 10, 10, pixel) && is_colour(pixel, 0, 0, 128),
           "the multisampled chain's second frame was not shown");

    // and back to a single-sampled chain
    resolved.Reset();
    staging.Reset();
    back.Reset();
    depth.Reset();
    chain.Reset();
    D3DPRESENT_PARAMETERS plain = rig.parameters(D3DSWAPEFFECT_DISCARD, D3DMULTISAMPLE_NONE);
    CHECK(device->Reset(&plain));
    CHECK(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back));
    CHECK(back->GetDesc(&back_desc));
    expect(back_desc.MultiSampleType == D3DMULTISAMPLE_NONE, "the back buffer is still multisampled after a Reset to 1x");
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(9, 8, 7), 1.0f, 0));
    expect(read_pixel(device, back.Get(), 3, 3, pixel) && is_colour(pixel, 9, 8, 7),
           "the single-sampled back buffer after a multisampled chain is not the clear");
    back.Reset();
  }

  // ---- a lockable render target ----
  {
    ComPtr<IDirect3DSurface9> back_buffer, depth;
    CHECK(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer));
    CHECK(device->GetDepthStencilSurface(&depth));

    ComPtr<IDirect3DSurface9> locked_target;
    CHECK(device->CreateRenderTarget(16, 16, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, TRUE, &locked_target, nullptr));
    D3DSURFACE_DESC target_desc{};
    CHECK(locked_target->GetDesc(&target_desc));
    expect((target_desc.Usage & D3DUSAGE_RENDERTARGET) != 0 && target_desc.Pool == D3DPOOL_DEFAULT,
           "a lockable render target is not a default-pool render target");

    CHECK(device->SetDepthStencilSurface(nullptr));
    CHECK(device->SetRenderTarget(0, locked_target.Get()));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255, 0, 0), 1.0f, 0));

    // what was drawn is what the lock reads
    D3DLOCKED_RECT locked{};
    CHECK(locked_target->LockRect(&locked, nullptr, D3DLOCK_READONLY));
    {
      const BYTE *at = static_cast<const BYTE *>(locked.pBits) + 5 * (size_t)locked.Pitch + 5 * 4;
      expect(is_colour(at, 255, 0, 0), "a lock of a render target does not read the clear");
      const BYTE *corner = static_cast<const BYTE *>(locked.pBits) + 15 * (size_t)locked.Pitch + 15 * 4;
      expect(is_colour(corner, 255, 0, 0), "a lock of a render target does not read its last pixel");
    }
    D3DLOCKED_RECT second{};
    expect(locked_target->LockRect(&second, nullptr, 0) == D3DERR_INVALIDCALL, "a render target locked twice");
    CHECK(locked_target->UnlockRect());
    expect(locked_target->UnlockRect() == D3DERR_INVALIDCALL, "a render target unlocked without a lock");

    // what is written is what the target holds: the whole of it and a rectangle of it
    CHECK(locked_target->LockRect(&locked, nullptr, 0));
    {
      BYTE *at = static_cast<BYTE *>(locked.pBits) + 3 * (size_t)locked.Pitch + 3 * 4;
      at[0] = 0;
      at[1] = 255;
      at[2] = 0;
      at[3] = 255;
    }
    CHECK(locked_target->UnlockRect());
    const RECT part{8, 8, 12, 12};
    CHECK(locked_target->LockRect(&locked, &part, 0));
    {
      BYTE *at = static_cast<BYTE *>(locked.pBits) + (size_t)locked.Pitch + 4; // (9, 9)
      at[0] = 255;
      at[1] = 0;
      at[2] = 0;
      at[3] = 255;
    }
    CHECK(locked_target->UnlockRect());
    expect(read_pixel(device, locked_target.Get(), 3, 3, pixel) && is_colour(pixel, 0, 255, 0),
           "a pixel written through a lock of a render target is not in the target");
    expect(read_pixel(device, locked_target.Get(), 9, 9, pixel) && is_colour(pixel, 0, 0, 255),
           "a pixel written through a rectangle's lock is not at the rectangle's place");
    expect(read_pixel(device, locked_target.Get(), 4, 4, pixel) && is_colour(pixel, 255, 0, 0),
           "a lock of a render target changed a pixel it did not write");

    // a read-only lock writes nothing back, even if the application scribbles on the bytes
    CHECK(locked_target->LockRect(&locked, nullptr, D3DLOCK_READONLY));
    static_cast<BYTE *>(locked.pBits)[0] = 77;
    CHECK(locked_target->UnlockRect());
    expect(read_pixel(device, locked_target.Get(), 0, 0, pixel) && is_colour(pixel, 255, 0, 0),
           "a read-only lock of a render target wrote its bytes back");

    // the target is still a render target: a draw after the lock lands on it, and the lock reads that
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(1, 2, 3), 1.0f, 0));
    CHECK(locked_target->LockRect(&locked, nullptr, D3DLOCK_READONLY));
    expect(is_colour(static_cast<const BYTE *>(locked.pBits) + 3 * (size_t)locked.Pitch + 3 * 4, 1, 2, 3),
           "a clear after a lock is not what the next lock reads");
    CHECK(locked_target->UnlockRect());

    // a target that is not lockable has no bytes to lock
    ComPtr<IDirect3DSurface9> plain_target;
    CHECK(device->CreateRenderTarget(16, 16, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &plain_target, nullptr));
    expect(plain_target->LockRect(&locked, nullptr, 0) == D3DERR_INVALIDCALL, "a render target that is not lockable locked");
    if (have_msaa) {
      ComPtr<IDirect3DSurface9> refused;
      expect(device->CreateRenderTarget(16, 16, D3DFMT_A8R8G8B8, samples, 0, TRUE, &refused, nullptr) == D3DERR_INVALIDCALL,
             "a lockable multisampled render target was made");
    }

    CHECK(device->SetRenderTarget(0, back_buffer.Get()));
    CHECK(device->SetDepthStencilSurface(depth.Get()));
  }

  printf("%s: %u wrong of %u checks%s\n", wrong ? "failed" : "passed", wrong, checks,
         have_msaa ? "" : " (multisampling not available)");
  return wrong != 0;
}
