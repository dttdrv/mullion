// contract: what a device of Direct3D 9's this milestone makes is, and what its
// frames are.
// - Direct3DCreate9 makes an object with the adapter DXGI has, which is this
// build's Metal device: the same name,
//   vendor and device ids. its caps are a HAL's, the texture size the Metal
//   device's, and nothing a device without shaders or a fixed function pipeline
//   does not have.
// - CreateDevice makes a device of D3DDEVTYPE_HAL with an implicit swap chain
// for a window, whose back buffer is the
//   size and format of the present parameters.
// - a frame is the colour the back buffer was cleared to, read back out of the
// back buffer before Present and out of
//   the window after it, for four frames in a row: DXGI's flip model turns the
//   chain's buffers over at Present, so every frame binds again and the fourth
//   frame is drawn like the first.
// the window's pixels are read through GDI, which has them when frames get to
// the window through GDI: the test asks for that (dxgi.presentThroughGDI),
// since nothing reads a Metal view's pixels back.
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

// how much of a window's client area, and how many attempts, a presented frame
// is looked for in
static bool window_shows(HWND window, const float colour[3]) {
  RECT area;
  GetClientRect(window, &area);
  const UINT across = 8;
  for (UINT attempt = 0; attempt < 250; attempt++) {
    MSG message;
    while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE))
      DispatchMessageA(&message);
    Sleep(20);
    unsigned differ = 0;
    HDC dc = GetDC(window);
    for (UINT y = area.bottom / across / 2; y < (UINT)area.bottom; y += area.bottom / across)
      for (UINT x = area.right / across / 2; x < (UINT)area.right; x += area.right / across) {
        const COLORREF got = GetPixel(dc, x, y);
        const int channels[3] = {(int)GetRValue(got), (int)GetGValue(got), (int)GetBValue(got)};
        bool same = true;
        for (int c = 0; c < 3; c++)
          same &= std::fabs(channels[c] - colour[c] * 255) <= 1;
        if (!same && differ++ < 2 && attempt == 249)
          printf("the window's pixel %u,%u is %d %d %d, want %.0f %.0f %.0f\n", x, y, channels[0], channels[1],
                 channels[2], colour[0] * 255, colour[1] * 255, colour[2] * 255);
      }
    ReleaseDC(window, dc);
    if (!differ)
      return true;
  }
  return false;
}

int main(int argc, char **argv) {
  // a Metal view's pixels are nobody's to read, so a run that presents through one asks that every frame reached
  // Present, not what the window shows (dxgi.presentThroughGDI is how the window's pixels are read)
  const bool read_window = argc < 2 || std::strcmp(argv[1], "--metal-view") != 0;
  unsigned wrong = 0, checks = 0;
  auto expect = [&](bool ok, const char *what) {
    checks++;
    if (!ok && wrong++ < 16)
      printf("%s\n", what);
  };
  if (read_window)
    SetEnvironmentVariableA("MULLION_CONFIG", "dxgi.presentThroughGDI=True");

  // the adapter this build's Direct3D 9 is over
  ComPtr<IDXGIFactory1> factory;
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  ComPtr<IDXGIAdapter1> adapter;
  CHECK(factory->EnumAdapters1(0, &adapter));
  DXGI_ADAPTER_DESC1 adapter_desc{};
  CHECK(adapter->GetDesc1(&adapter_desc));

  ComPtr<IDirect3D9> d3d9;
  d3d9.Attach(Direct3DCreate9(D3D_SDK_VERSION));
  if (!d3d9) {
    printf("failed: Direct3DCreate9 made no object\n");
    return 1;
  }
  expect(d3d9->GetAdapterCount() >= 1, "the object has no adapter");

  // the adapter is the Metal device's, as DXGI describes it
  D3DADAPTER_IDENTIFIER9 identifier{};
  CHECK(d3d9->GetAdapterIdentifier(0, 0, &identifier));
  char adapter_name[128] = {};
  WideCharToMultiByte(CP_ACP, 0, adapter_desc.Description, -1, adapter_name, sizeof(adapter_name), nullptr, nullptr);
  expect(std::strcmp(identifier.Description, adapter_name) == 0,
         "the adapter is not the one DXGI has: its description differs");
  expect(identifier.VendorId == adapter_desc.VendorId && identifier.DeviceId == adapter_desc.DeviceId,
         "the adapter's vendor or device id is not the Metal device's");
  expect(identifier.Description[0] != 0 && identifier.Driver[0] != 0, "the adapter has no description or driver");

  D3DCAPS9 caps{};
  CHECK(d3d9->GetDeviceCaps(0, D3DDEVTYPE_HAL, &caps));
  expect(caps.DeviceType == D3DDEVTYPE_HAL && caps.AdapterOrdinal == 0, "the caps are not of the adapter's HAL device");
  expect(caps.MaxTextureWidth >= 4096 && caps.MaxTextureHeight >= 4096,
         "the largest texture is not the Metal device's");
  expect((caps.PresentationIntervals & D3DPRESENT_INTERVAL_ONE) != 0, "a frame cannot wait for one refresh");
  // the shader models this build's translator certifies (d3d9_caps.cpp holds the detail)
  expect(caps.VertexShaderVersion == D3DVS_VERSION(2, 0) && caps.PixelShaderVersion == D3DPS_VERSION(2, 0),
         "the shader models the translator certifies are not reported");

  D3DDISPLAYMODE mode{};
  CHECK(d3d9->GetAdapterDisplayMode(0, &mode));
  expect(mode.Width != 0 && mode.Height != 0 && mode.Format == D3DFMT_X8R8G8B8,
         "the display mode is not a windowed chain's");
  CHECK(d3d9->CheckDeviceType(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DFMT_X8R8G8B8, TRUE));
  expect(d3d9->CheckDeviceType(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DFMT_X8R8G8B8, FALSE) == D3DERR_NOTAVAILABLE,
         "a fullscreen chain this build cannot show is reported as one it can");
  expect(d3d9->CheckDeviceType(0, D3DDEVTYPE_REF, D3DFMT_X8R8G8B8, D3DFMT_X8R8G8B8, TRUE) == D3DERR_INVALIDCALL,
         "a device this build is not is reported as one it is");

  // the window a device presents to
  const UINT client = 192;
  WNDCLASSA window_class{0,       DefWindowProcA, 0,       0,       GetModuleHandleA(nullptr),
                         nullptr, nullptr,        nullptr, nullptr, "d3d9_present"};
  RegisterClassA(&window_class);
  RECT window_rect{0, 0, (LONG)client, (LONG)client};
  AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);
  HWND window = CreateWindowA("d3d9_present", "d3d9_present", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0,
                              window_rect.right - window_rect.left, window_rect.bottom - window_rect.top, nullptr,
                              nullptr, window_class.hInstance, nullptr);
  if (!window) {
    printf("failed: no window\n");
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

  // the device's own object and chain
  ComPtr<IDirect3D9> owner;
  CHECK(device->GetDirect3D(&owner));
  expect(owner.Get() == d3d9.Get(), "the device's object is not the one that made it");
  D3DDEVICE_CREATION_PARAMETERS creation{};
  CHECK(device->GetCreationParameters(&creation));
  expect(creation.AdapterOrdinal == 0 && creation.DeviceType == D3DDEVTYPE_HAL && creation.hFocusWindow == window &&
             creation.BehaviorFlags == D3DCREATE_HARDWARE_VERTEXPROCESSING,
         "the device's creation parameters are not what it was made with");
  expect(device->GetNumberOfSwapChains() == 1, "a device without additional chains has a count other than one");
  ComPtr<IDirect3DSwapChain9> chain;
  CHECK(device->GetSwapChain(0, &chain));
  D3DPRESENT_PARAMETERS chain_parameters{};
  CHECK(chain->GetPresentParameters(&chain_parameters));
  expect(chain_parameters.BackBufferWidth == client && chain_parameters.BackBufferHeight == client &&
             chain_parameters.BackBufferFormat == D3DFMT_X8R8G8B8 && chain_parameters.Windowed,
         "the implicit chain's present parameters are not the device's");
  ComPtr<IDirect3DDevice9> chain_device;
  CHECK(chain->GetDevice(&chain_device));
  expect(chain_device.Get() == device.Get(), "the chain's device is not the one that owns it");
  D3DCAPS9 device_caps{};
  CHECK(device->GetDeviceCaps(&device_caps));
  expect(device_caps.MaxTextureWidth == caps.MaxTextureWidth, "the device's caps are not its adapter's");

  // the surface a frame is read back into
  ComPtr<IDirect3DSurface9> readback;
  CHECK(device->CreateOffscreenPlainSurface(client, client, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr));
  ComPtr<IDirect3DSurface9> pool_surface;
  expect(device->CreateOffscreenPlainSurface(16, 16, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &pool_surface, nullptr) ==
             D3DERR_INVALIDCALL,
         "a default pool surface is reported as one this build makes");

  const D3DCOLOR colours[4] = {D3DCOLOR_XRGB(255, 0, 0), D3DCOLOR_XRGB(0, 255, 0), D3DCOLOR_XRGB(0, 0, 255),
                               D3DCOLOR_XRGB(255, 255, 0)};
  for (unsigned frame = 0; frame < 4; frame++) {
    // every frame binds its back buffer: a flip model chain turned it over at
    // the last Present
    ComPtr<IDirect3DSurface9> back_buffer;
    CHECK(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer));
    D3DSURFACE_DESC back_buffer_desc{};
    CHECK(back_buffer->GetDesc(&back_buffer_desc));
    expect(back_buffer_desc.Width == client && back_buffer_desc.Height == client &&
               back_buffer_desc.Format == D3DFMT_X8R8G8B8 && back_buffer_desc.Type == D3DRTYPE_SURFACE &&
               back_buffer_desc.Pool == D3DPOOL_DEFAULT,
           "the back buffer is not the chain's");
    ComPtr<IDirect3DSurface9> render_target;
    CHECK(device->GetRenderTarget(0, &render_target));
    expect(render_target.Get() == back_buffer.Get(), "the render target is not the back buffer");

    CHECK(device->BeginScene());
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, colours[frame], 1.0f, 0));
    CHECK(device->EndScene());

    // the frame's bytes, out of the back buffer
    CHECK(device->GetRenderTargetData(back_buffer.Get(), readback.Get()));
    D3DLOCKED_RECT locked{};
    CHECK(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY));
    const D3DCOLOR want = colours[frame];
    D3DCOLOR first_texel = 0;
    unsigned different = 0;
    for (UINT y = 8; y < client; y += 32)
      for (UINT x = 8; x < client; x += 32) {
        const DWORD *row = (const DWORD *)((const char *)locked.pBits + (size_t)y * locked.Pitch);
        const D3DCOLOR got = row[x];
        // an X8R8G8B8 target has no alpha of its own to compare (it is 255
        // after a clear)
        if (x == 8 && y == 8)
          first_texel = got;
        if (((got ^ want) & 0x00FFFFFF) != 0 && different++ == 0)
          printf("frame %u: the back buffer's texel %u,%u is %08lx, want %08lx\n", frame, x, y, (unsigned long)got,
                 (unsigned long)want);
      }
    expect(different == 0, "a frame's back buffer is not the colour it was cleared to");
    CHECK(readback->UnlockRect());

    CHECK(device->Present(nullptr, nullptr, nullptr, nullptr));

    // the frame's colour, on the window
    bool the_window_shows_it = false;
    if (read_window) {
      const float colour[3] = {((want >> 16) & 0xFF) / 255.0f, ((want >> 8) & 0xFF) / 255.0f, (want & 0xFF) / 255.0f};
      the_window_shows_it = window_shows(window, colour);
      expect(the_window_shows_it, "the window does not show the frame that was presented");
    }
    printf("frame %u: back buffer's texel 8,8 is %08lx, window %s\n", frame, (unsigned long)first_texel,
           !read_window          ? "not read (a Metal view)"
           : the_window_shows_it ? "shows it"
                                 : "does not show it");
  }

  expect(device->TestCooperativeLevel() == S_OK, "the device is not cooperative after its frames");
  expect(device->GetBackBuffer(0, 1, D3DBACKBUFFER_TYPE_MONO, nullptr) == D3DERR_INVALIDCALL,
         "a back buffer this chain does not have is handed out");

  chain.Reset();
  device.Reset();
  owner.Reset();
  d3d9.Reset();
  DestroyWindow(window);

  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
