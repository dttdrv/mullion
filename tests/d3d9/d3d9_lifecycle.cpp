// contract: Direct3D 9's cooperative level is the health of the device the GPU draws with. A device whose frames failed
// is lost, TestCooperativeLevel answers D3DERR_DEVICELOST while it cannot be reset and D3DERR_DEVICENOTRESET once it
// can, and the calls that draw or present answer D3DERR_DEVICELOST
// (IDirect3DDevice9::TestCooperativeLevel: "D3DERR_DEVICELOST ... The device has been lost but cannot be reset at this
// time"; "D3DERR_DEVICENOTRESET ... The device has been lost but can be reset at this time");
// https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-testcooperativelevel
//
// Reset is the road back: it recreates the device, loses the state, drops the default-pool resources the device held,
// and keeps what D3D9 keeps, which is the managed pool and its contents (IDirect3DDevice9::Reset: "all state
// information is lost"; "D3DPOOL_MANAGED resources ... are not affected by a device reset");
// https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-reset
//
// The loss is the Direct3D 11 device's removal (tests/d3d11/d3d11_device_removal.cpp is that device's own contract), and
// MULLION_D3D11_FAULT_DEVICE=1 makes the run's first completed command buffer one the GPU failed. tests/d3d9/meson.build
// runs this test three times: live, lost (the level, the draws and Present), and --reset (the whole sequence through
// D3DERR_DEVICENOTRESET and a Reset that draws again).
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

static const DWORD kFVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;

struct Vertex {
  float x, y, z, rhw;
  D3DCOLOR colour;
};

struct TexturedVertex {
  float x, y, z, rhw;
  D3DCOLOR colour;
  float u, v;
};

// the colour at the middle of the frame, out of the back buffer
static DWORD
centre_pixel(IDirect3DDevice9 *device, UINT size) {
  ComPtr<IDirect3DSurface9> back, readback;
  if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)))
    return 0xdeadbeef;
  if (FAILED(device->CreateOffscreenPlainSurface(size, size, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr)))
    return 0xdeadbeef;
  if (FAILED(device->GetRenderTargetData(back.Get(), readback.Get())))
    return 0xdeadbeef;
  D3DLOCKED_RECT pixels{};
  if (FAILED(readback->LockRect(&pixels, nullptr, D3DLOCK_READONLY)))
    return 0xdeadbeef;
  DWORD pixel = 0xdeadbeef;
  if (pixels.pBits)
    pixel = *(const DWORD *)((const char *)pixels.pBits + (size / 2) * pixels.Pitch + (size / 2) * 4);
  readback->UnlockRect();
  return pixel;
}

int
main(int argc, char **argv) {
  unsigned wrong = 0, checks = 0;
  auto expect = [&](bool ok, const char *what) {
    checks++;
    if (!ok && wrong++ < 16)
      printf("wrong: %s\n", what);
  };

  const bool lost_run = GetEnvironmentVariableA("MULLION_D3D11_FAULT_DEVICE", nullptr, 0) > 0;
  const bool reset_run = argc > 1 && !strcmp(argv[1], "--reset");

  const UINT client = 64;
  WNDCLASSA window_class{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr,
                         "d3d9_lifecycle"};
  RegisterClassA(&window_class);
  RECT window_rect{0, 0, (LONG)client, (LONG)client};
  AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);
  HWND window = CreateWindowA("d3d9_lifecycle", "d3d9_lifecycle", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0,
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
  CHECK(device->SetFVF(kFVF));

  // a triangle in the middle of the frame, wound the way the default cull mode draws it
  const Vertex vertices[3] = {{8.0f, 8.0f, 0.5f, 1.0f, 0xffff00ffu},
                              {56.0f, 8.0f, 0.5f, 1.0f, 0xffff00ffu},
                              {8.0f, 56.0f, 0.5f, 1.0f, 0xffff00ffu}};
  const TexturedVertex quad[4] = {{0.0f, 0.0f, 0.5f, 1.0f, 0xffffffffu, 0.0f, 0.0f},
                                  {64.0f, 0.0f, 0.5f, 1.0f, 0xffffffffu, 1.0f, 0.0f},
                                  {0.0f, 64.0f, 0.5f, 1.0f, 0xffffffffu, 0.0f, 1.0f},
                                  {64.0f, 64.0f, 0.5f, 1.0f, 0xffffffffu, 1.0f, 1.0f}};

  if (reset_run) {
    // ---- the sequence a title that survives a failure runs -------------------------------------------------------
    // a managed texture whose contents must survive the device the frames failed on: a solid red 2x2 level
    ComPtr<IDirect3DTexture9> managed;
    CHECK(device->CreateTexture(2, 2, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, &managed, nullptr));
    {
      D3DLOCKED_RECT locked{};
      CHECK(managed->LockRect(0, &locked, nullptr, 0));
      for (int y = 0; y < 2; y++)
        for (int x = 0; x < 2; x++)
          *(DWORD *)((char *)locked.pBits + y * locked.Pitch + x * 4) = 0xffff0000u;
      CHECK(managed->UnlockRect(0));
    }
    // a default-pool texture the device holds (and drops at Reset), and a frame on a live device
    ComPtr<IDirect3DTexture9> default_pool;
    CHECK(device->CreateTexture(2, 2, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &default_pool, nullptr));
    CHECK(device->SetTexture(0, default_pool.Get()));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000, 1.0f, 0));
    expect(SUCCEEDED(device->Present(nullptr, nullptr, nullptr, nullptr)), "a live device refused Present");
    expect(device->TestCooperativeLevel() == S_OK, "the live device is not S_OK");

    // the frames fail: Present answers D3DERR_DEVICELOST
    HRESULT hr = S_OK;
    for (unsigned attempt = 0; attempt < 500 && SUCCEEDED(hr); attempt++) {
      hr = device->Present(nullptr, nullptr, nullptr, nullptr);
      if (SUCCEEDED(hr))
        Sleep(10);
    }
    expect(hr == D3DERR_DEVICELOST, "Present of the faulted device did not answer D3DERR_DEVICELOST");
    expect(device->TestCooperativeLevel() == D3DERR_DEVICENOTRESET,
           "a lost device that can be reset did not say D3DERR_DEVICENOTRESET");

    // while it is lost: a default-pool resource and a draw are refused, and a Reset naming a window that is not one is
    // refused, which leaves the device where it was
    {
      ComPtr<IDirect3DTexture9> refused;
      expect(device->CreateTexture(2, 2, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &refused, nullptr) == D3DERR_DEVICELOST,
             "a lost device made a default-pool texture");
      expect(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0) == D3DERR_DEVICELOST,
             "Clear of a lost device was not refused as lost");
      D3DPRESENT_PARAMETERS invalid = present;
      invalid.hDeviceWindow = (HWND)0x1234;
      expect(device->Reset(&invalid) == D3DERR_INVALIDCALL, "Reset naming a window that is not one was not refused");
      expect(device->TestCooperativeLevel() == D3DERR_DEVICENOTRESET, "a refused Reset changed the device's level");
    }

    // the Reset that recovers it, from the present parameters the chain is to have
    default_pool.Reset();
    D3DPRESENT_PARAMETERS again = present;
    again.hDeviceWindow = window;
    expect(SUCCEEDED(device->Reset(&again)), "Reset did not recover the lost device");
    expect(device->TestCooperativeLevel() == S_OK, "the recovered device is not S_OK");
    expect(again.BackBufferWidth == 0 && again.BackBufferCount == 0 && again.BackBufferFormat == D3DFMT_UNKNOWN,
           "Reset did not report the fields it leaves to be queried");

    // all state is lost: nothing the device had bound before the loss is still bound
    {
      ComPtr<IDirect3DBaseTexture9> bound;
      CHECK(device->GetTexture(0, bound.GetAddressOf()));
      expect(bound.Get() == nullptr, "Reset kept the texture the device had bound");
    }

    // and it draws again with a managed texture written before the loss: the texels survived the new device
    CHECK(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1));
    CHECK(device->SetTexture(0, managed.Get()));
    CHECK(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
    CHECK(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
    CHECK(device->BeginScene());
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff0000ff, 1.0f, 0));
    expect(SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(TexturedVertex))),
           "the recovered device refused a draw");
    CHECK(device->EndScene());
    const DWORD pixel = centre_pixel(device.Get(), client);
    expect((pixel & 0x00ffffffu) == 0x00ff0000u,
           "the managed texture's texels did not survive Reset, or the draw did not put them on the frame");
    expect(SUCCEEDED(device->Present(nullptr, nullptr, nullptr, nullptr)), "the recovered device refused Present");
    expect(device->TestCooperativeLevel() == S_OK, "the device that drew and presented after a Reset is not S_OK");
    printf("lifecycle: lost, D3DERR_DEVICENOTRESET, Reset, and a managed texture drawn again with pixel %06lx\n",
           (unsigned long)pixel);
  } else if (!lost_run) {
    // a live device: the cooperative level is S_OK, and the calls a frame is made of answer it
    expect(device->TestCooperativeLevel() == S_OK, "a live device is lost");
    expect(SUCCEEDED(device->BeginScene()), "a live device refused BeginScene");
    expect(device->BeginScene() == D3DERR_INVALIDCALL, "a second BeginScene was not refused");
    expect(SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000, 1.0f, 0)),
           "a live device refused Clear");
    expect(SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, vertices, sizeof(Vertex))),
           "a live device refused a draw");
    expect(SUCCEEDED(device->EndScene()), "a live device refused EndScene");
    expect(device->EndScene() == D3DERR_INVALIDCALL, "a second EndScene was not refused");
    expect(SUCCEEDED(device->Present(nullptr, nullptr, nullptr, nullptr)), "a live device refused Present");
    expect(device->TestCooperativeLevel() == S_OK, "a device that drew a frame is lost");
    printf("lifecycle: a live device drew and presented\n");
  } else {
    // the frame after the failure: Present answers D3DERR_DEVICELOST. the fault is taken when a command buffer
    // completes, so the loop presents until it does (bounded: a test deadline, not an API limit)
    HRESULT hr = S_OK;
    for (unsigned attempt = 0; attempt < 500 && SUCCEEDED(hr); attempt++) {
      hr = device->Present(nullptr, nullptr, nullptr, nullptr);
      if (SUCCEEDED(hr))
        Sleep(10);
    }
    expect(hr == D3DERR_DEVICELOST,
           "Present of a device whose frames failed answered something other than D3DERR_DEVICELOST");

    // the level says so, and it stays so: the first removal is the device's
    expect(device->TestCooperativeLevel() == D3DERR_DEVICENOTRESET,
           "a lost device that can be reset did not say D3DERR_DEVICENOTRESET");
    expect(device->TestCooperativeLevel() == D3DERR_DEVICENOTRESET, "the lost device's level changed");

    // and the calls that draw or present answer it rather than drawing into a device that is gone
    expect(device->BeginScene() == D3DERR_DEVICELOST, "BeginScene of a lost device was not refused as lost");
    expect(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000, 1.0f, 0) == D3DERR_DEVICELOST,
           "Clear of a lost device was not refused as lost");
    expect(device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1) == D3DERR_DEVICELOST,
           "DrawPrimitive of a lost device was not refused as lost");
    expect(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1) == D3DERR_DEVICELOST,
           "DrawIndexedPrimitive of a lost device was not refused as lost");
    expect(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, vertices, sizeof(Vertex)) == D3DERR_DEVICELOST,
           "DrawPrimitiveUP of a lost device was not refused as lost");
    expect(device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 3, 1, nullptr, D3DFMT_INDEX16, vertices,
                                          sizeof(Vertex)) == D3DERR_DEVICELOST,
           "DrawIndexedPrimitiveUP of a lost device was not refused as lost");
    printf("lifecycle: the device was lost, and the draws answered D3DERR_DEVICELOST\n");
  }

  device.Reset();
  d3d9.Reset();
  DestroyWindow(window);
  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
