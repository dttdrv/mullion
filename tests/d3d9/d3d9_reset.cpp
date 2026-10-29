// contract: IDirect3DDevice9::Reset resizes and reformats a windowed chain, loses the device's state, and refuses to
// run while the application still holds the implicit chain's back buffer or a depth-stencil surface.
// - Reset with a back buffer or a depth-stencil surface still referenced by the application is D3DERR_INVALIDCALL, and
//   the chain is as it was; after the references are released it succeeds.
// - a windowed BackBufferFormat of D3DFMT_UNKNOWN is legal: the chain keeps the format it had (Microsoft Learn,
//   IDirect3DDevice9::Reset: "D3DFMT_UNKNOWN can be specified for the windowed mode back buffer format").
// - a width or height of zero is the window's client area.
// - the caller's structure reports BackBufferWidth, BackBufferHeight and BackBufferCount as zero and BackBufferFormat
//   as D3DFMT_UNKNOWN when the call returns; the back buffer, the viewport and the swap chain's present parameters are
//   the new size.
// - all state is lost: the render states are the defaults, the textures, streams and shaders set are unbound, and a
//   chain with EnableAutoDepthStencil has a depth-stencil surface of the new size with depth testing on.
// - a fullscreen chain, a multisampled chain, a non-discard swap effect and a format this build has none are refused
//   and leave the windowed chain as it was (src/d3d9/README says what is not there yet).
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

struct Vertex {
  float x, y, z, rhw;
  D3DCOLOR colour;
};

// the first pixel of a surface, read back through a system-memory surface of its own size and format
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

int
main() {
  unsigned wrong = 0, checks = 0;
  auto expect = [&](bool ok, const char *what) {
    checks++;
    if (!ok && wrong++ < 20)
      printf("wrong: %s\n", what);
  };

  WNDCLASSA window_class{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr,
                         "d3d9_reset"};
  RegisterClassA(&window_class);
  RECT window_rect{0, 0, 64, 64};
  AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);
  HWND window = CreateWindowA("d3d9_reset", "d3d9_reset", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0,
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
  present.EnableAutoDepthStencil = TRUE;
  present.AutoDepthStencilFormat = D3DFMT_D24S8;
  present.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

  ComPtr<IDirect3DDevice9> device;
  CHECK(d3d9->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &present, &device));

  ComPtr<IDirect3DSurface9> back_buffer;
  CHECK(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer));
  D3DSURFACE_DESC desc{};
  CHECK(back_buffer->GetDesc(&desc));
  expect(desc.Width == 64 && desc.Height == 64, "the chain was not made at 64x64");

  // the state a reset loses: a render state, a texture, a stream source and the transform pipeline's declaration
  ComPtr<IDirect3DTexture9> texture;
  CHECK(device->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr));
  ComPtr<IDirect3DVertexBuffer9> stream;
  CHECK(device->CreateVertexBuffer(sizeof(Vertex), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                                   D3DFVF_XYZRHW | D3DFVF_DIFFUSE, D3DPOOL_DEFAULT, &stream, nullptr));
  CHECK(device->SetTexture(0, texture.Get()));
  CHECK(device->SetStreamSource(0, stream.Get(), 0, sizeof(Vertex)));
  CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW));

  // a live back buffer is what Reset refuses, and the refusal leaves the chain as it was
  D3DPRESENT_PARAMETERS next = present;
  next.BackBufferWidth = 32;
  next.BackBufferHeight = 40;
  const HRESULT refused_buffer = device->Reset(&next);
  expect(refused_buffer == D3DERR_INVALIDCALL, "Reset accepted an application-held back buffer");
  expect(next.BackBufferWidth == 32 && next.BackBufferHeight == 40,
         "a refused Reset changed the caller's present parameters");
  D3DSURFACE_DESC after{};
  CHECK(back_buffer->GetDesc(&after));
  expect(after.Width == 64 && after.Height == 64, "a refused Reset changed the back buffer");
  back_buffer.Reset();

  // arguments D3D9 refuses before it touches the chain
  expect(device->Reset(nullptr) == D3DERR_INVALIDCALL, "Reset(NULL) succeeded");
  {
    D3DPRESENT_PARAMETERS fullscreen = present;
    fullscreen.BackBufferWidth = 64;
    fullscreen.BackBufferHeight = 64;
    fullscreen.Windowed = FALSE;
    expect(device->Reset(&fullscreen) == D3DERR_INVALIDCALL, "Reset to a fullscreen chain succeeded");
  }
  {
    D3DPRESENT_PARAMETERS multisampled = present;
    multisampled.BackBufferWidth = 64;
    multisampled.BackBufferHeight = 64;
    multisampled.MultiSampleType = D3DMULTISAMPLE_2_SAMPLES;
    expect(device->Reset(&multisampled) == D3DERR_INVALIDCALL, "Reset to a multisampled chain succeeded");
  }
  {
    D3DPRESENT_PARAMETERS copy = present;
    copy.BackBufferWidth = 64;
    copy.BackBufferHeight = 64;
    copy.SwapEffect = D3DSWAPEFFECT_COPY;
    expect(device->Reset(&copy) == D3DERR_INVALIDCALL, "Reset to a copy chain succeeded");
  }
  {
    D3DPRESENT_PARAMETERS bad_format = present;
    bad_format.BackBufferWidth = 64;
    bad_format.BackBufferHeight = 64;
    bad_format.BackBufferFormat = D3DFMT_P8;
    expect(device->Reset(&bad_format) == D3DERR_INVALIDCALL, "Reset to a format this build has none of succeeded");
  }

  // a windowed format of UNKNOWN keeps the format the chain had, and 0x0 is the window's client area
  {
    D3DPRESENT_PARAMETERS windowed = present;
    windowed.BackBufferWidth = 0;
    windowed.BackBufferHeight = 0;
    windowed.BackBufferFormat = D3DFMT_UNKNOWN;
    windowed.BackBufferCount = 0;
    CHECK(device->Reset(&windowed));
    expect(windowed.BackBufferWidth == 0 && windowed.BackBufferHeight == 0,
           "Reset left the caller's back buffer size set");
    expect(windowed.BackBufferCount == 0, "Reset left the caller's back buffer count set");
    expect(windowed.BackBufferFormat == D3DFMT_UNKNOWN, "Reset left the caller's back buffer format set");

    ComPtr<IDirect3DSurface9> resized;
    CHECK(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &resized));
    D3DSURFACE_DESC resized_desc{};
    CHECK(resized->GetDesc(&resized_desc));
    RECT client{};
    GetClientRect(window, &client);
    const UINT client_width = (UINT)(client.right - client.left);
    const UINT client_height = (UINT)(client.bottom - client.top);
    expect(resized_desc.Width == client_width && resized_desc.Height == client_height,
           "a 0x0 Reset did not take the window's client area");
    expect(resized_desc.Format == D3DFMT_X8R8G8B8, "a UNKNOWN windowed format did not keep the chain's format");

    D3DVIEWPORT9 viewport{};
    CHECK(device->GetViewport(&viewport));
    expect(viewport.X == 0 && viewport.Y == 0 && viewport.Width == client_width && viewport.Height == client_height,
           "the viewport is not the whole of the reset chain's back buffer");

    ComPtr<IDirect3DSwapChain9> chain;
    CHECK(device->GetSwapChain(0, &chain));
    D3DPRESENT_PARAMETERS reported{};
    CHECK(chain->GetPresentParameters(&reported));
    expect(reported.BackBufferWidth == client_width && reported.BackBufferHeight == client_height,
           "the swap chain reports the old size after Reset");
    expect(reported.BackBufferFormat == D3DFMT_X8R8G8B8, "the swap chain reports the old format after Reset");
    expect(reported.BackBufferCount >= 2, "the swap chain reports fewer buffers than a flip chain has");

    // the state is the defaults again
    DWORD value = 0;
    CHECK(device->GetRenderState(D3DRS_CULLMODE, &value));
    expect(value == D3DCULL_CCW, "Reset kept the cull mode");
    CHECK(device->GetRenderState(D3DRS_ZENABLE, &value));
    expect(value == TRUE, "a chain with an automatic depth-stencil surface lost depth testing");
    ComPtr<IDirect3DBaseTexture9> bound;
    CHECK(device->GetTexture(0, &bound));
    expect(!bound, "Reset kept a texture bound");
    ComPtr<IDirect3DVertexBuffer9> bound_stream;
    UINT offset = 0, stride = 0;
    CHECK(device->GetStreamSource(0, &bound_stream, &offset, &stride));
    expect(!bound_stream, "Reset kept a stream source bound");

    ComPtr<IDirect3DSurface9> depth;
    CHECK(device->GetDepthStencilSurface(&depth));
    expect(depth != nullptr, "a chain with an automatic depth-stencil surface lost it");
    if (depth) {
      D3DSURFACE_DESC depth_desc{};
      CHECK(depth->GetDesc(&depth_desc));
      expect(depth_desc.Width == client_width && depth_desc.Height == client_height,
             "the automatic depth surface is not the new size");
      expect(depth_desc.Pool == D3DPOOL_DEFAULT, "the automatic depth surface is not default-pool");
    }

    // and a clear reads back at the new size
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(255, 0, 0), 1.0f, 0));
    BYTE pixel[4] = {};
    expect(read_pixel(device.Get(), resized.Get(), 0, 0, pixel), "the reset back buffer did not read back");
    expect(pixel[2] == 255 && pixel[1] == 0 && pixel[0] == 0, "the reset back buffer's clear is not red");
  }

  // an explicit format, a size and no automatic depth: the chain takes them, and depth testing is off again
  {
    D3DPRESENT_PARAMETERS explicit_parameters = present;
    explicit_parameters.BackBufferWidth = 48;
    explicit_parameters.BackBufferHeight = 24;
    explicit_parameters.BackBufferFormat = D3DFMT_A8R8G8B8;
    explicit_parameters.BackBufferCount = 0;
    explicit_parameters.EnableAutoDepthStencil = FALSE;
    explicit_parameters.AutoDepthStencilFormat = D3DFMT_UNKNOWN;
    CHECK(device->Reset(&explicit_parameters));
    expect(explicit_parameters.BackBufferFormat == D3DFMT_UNKNOWN,
           "Reset left the caller's explicit back buffer format set");

    ComPtr<IDirect3DSurface9> resized;
    CHECK(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &resized));
    D3DSURFACE_DESC resized_desc{};
    CHECK(resized->GetDesc(&resized_desc));
    expect(resized_desc.Width == 48 && resized_desc.Height == 24, "Reset did not take the size it was given");
    expect(resized_desc.Format == D3DFMT_A8R8G8B8, "Reset did not take the format it was given");
    DWORD value = 0;
    CHECK(device->GetRenderState(D3DRS_ZENABLE, &value));
    expect(value == FALSE, "a chain without an automatic depth-stencil surface has depth testing on");
    ComPtr<IDirect3DSurface9> depth;
    CHECK(device->GetDepthStencilSurface(&depth));
    expect(!depth, "a chain without an automatic depth-stencil surface kept one");
  }

  // a depth-stencil surface the application holds is refused too, as D3D9's Reset requires it released
  {
    ComPtr<IDirect3DSurface9> explicit_depth;
    CHECK(device->CreateDepthStencilSurface(48, 24, D3DFMT_D16, D3DMULTISAMPLE_NONE, 0, TRUE, &explicit_depth, nullptr));
    CHECK(device->SetDepthStencilSurface(explicit_depth.Get()));
    D3DPRESENT_PARAMETERS same = present;
    same.BackBufferWidth = 48;
    same.BackBufferHeight = 24;
    same.BackBufferFormat = D3DFMT_A8R8G8B8;
    same.EnableAutoDepthStencil = FALSE;
    expect(device->Reset(&same) == D3DERR_INVALIDCALL, "Reset accepted an application-held depth-stencil surface");
    explicit_depth.Reset();
    CHECK(device->SetDepthStencilSurface(nullptr));
    CHECK(device->Reset(&same));
  }

  device.Reset();
  d3d9.Reset();
  DestroyWindow(window);

  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
