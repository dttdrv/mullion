// contract: IDirect3DSurface9::GetContainer names the object a surface belongs to -- its texture for a level, its swap
// chain for a back buffer -- and a standalone surface has none. IDirect3DResource9's private data is per GUID, holds
// either bytes or an AddRef'd IUnknown (D3DSPD_IUNKNOWN), and reports its size, a short buffer and a missing entry the
// way D3D9 does.
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

static const GUID kBytes = {0x11111111, 0x2222, 0x3333, {0x44, 0x44, 0x55, 0x55, 0x66, 0x66, 0x77, 0x77}};
static const GUID kMissing = {0x99999999, 0x8888, 0x7777, {0x66, 0x66, 0x55, 0x55, 0x44, 0x44, 0x33, 0x33}};

int
main() {
  unsigned wrong = 0, checks = 0;
  auto expect = [&](bool ok, const char *what) {
    checks++;
    if (!ok && wrong++ < 16)
      printf("wrong: %s\n", what);
  };

  const UINT client = 64;
  WNDCLASSA window_class{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr,
                         "d3d9_containers"};
  RegisterClassA(&window_class);
  RECT window_rect{0, 0, (LONG)client, (LONG)client};
  AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);
  HWND window = CreateWindowA("d3d9_containers", "d3d9_containers", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0,
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

  // a texture level's container is its texture, and the surface keeps the texture alive after the application's
  // texture reference goes away
  ComPtr<IDirect3DTexture9> texture;
  CHECK(device->CreateTexture(8, 8, 2, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr));
  ComPtr<IDirect3DSurface9> level;
  CHECK(texture->GetSurfaceLevel(1, &level));
  {
    ComPtr<IDirect3DTexture9> container;
    expect(SUCCEEDED(level->GetContainer(__uuidof(IDirect3DTexture9), &container)) && container == texture,
           "a texture level's container is not its texture");
    ComPtr<IDirect3DBaseTexture9> base;
    expect(SUCCEEDED(level->GetContainer(__uuidof(IDirect3DBaseTexture9), &base)) && base,
           "a texture level's container is not a base texture");
    ComPtr<IDirect3DSurface9> not_a_swap_chain;
    expect(level->GetContainer(__uuidof(IDirect3DSwapChain9), &not_a_swap_chain) == E_NOINTERFACE &&
               !not_a_swap_chain,
           "a texture level named a swap chain");
  }
  texture.Reset();
  {
    ComPtr<IDirect3DTexture9> container;
    expect(SUCCEEDED(level->GetContainer(__uuidof(IDirect3DTexture9), &container)) && container,
           "a surface's texture did not survive the application's texture reference");
  }
  level.Reset();

  // a back buffer's container is its swap chain
  {
    ComPtr<IDirect3DSurface9> back_buffer;
    CHECK(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer));
    ComPtr<IDirect3DSwapChain9> chain, container;
    CHECK(device->GetSwapChain(0, &chain));
    expect(SUCCEEDED(back_buffer->GetContainer(__uuidof(IDirect3DSwapChain9), &container)) && container == chain,
           "a back buffer's container is not its swap chain");
    ComPtr<IDirect3DTexture9> not_a_texture;
    expect(back_buffer->GetContainer(__uuidof(IDirect3DTexture9), &not_a_texture) == E_NOINTERFACE && !not_a_texture,
           "a back buffer named a texture");
  }

  // a standalone render target has no container
  {
    ComPtr<IDirect3DSurface9> target;
    CHECK(device->CreateRenderTarget(8, 8, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &target, nullptr));
    ComPtr<IDirect3DTexture9> container;
    expect(target->GetContainer(__uuidof(IDirect3DTexture9), &container) == E_NOINTERFACE && !container,
           "a standalone render target named a container");
    HANDLE *unused = nullptr;
    (void)unused;
  }

  // a buffer's private data: bytes by GUID, its size, a short buffer and a missing entry
  {
    ComPtr<IDirect3DVertexBuffer9> buffer;
    CHECK(device->CreateVertexBuffer(64, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &buffer, nullptr));
    const char bytes[] = "private data";
    const DWORD size = sizeof(bytes);
    CHECK(buffer->SetPrivateData(kBytes, bytes, size, 0));

    DWORD got = 0;
    expect(SUCCEEDED(buffer->GetPrivateData(kBytes, nullptr, &got)) && got == size,
           "private data did not report its size");
    char read[sizeof(bytes)]{};
    got = sizeof(read);
    expect(SUCCEEDED(buffer->GetPrivateData(kBytes, read, &got)) && got == size && !memcmp(read, bytes, size),
           "private data did not read back");

    char too_small[2]{};
    got = sizeof(too_small);
    expect(buffer->GetPrivateData(kBytes, too_small, &got) == D3DERR_MOREDATA,
           "a short private-data buffer was not D3DERR_MOREDATA");

    got = 0;
    expect(buffer->GetPrivateData(kMissing, nullptr, &got) == D3DERR_NOTFOUND,
           "missing private data was not D3DERR_NOTFOUND");

    // D3DSPD_IUNKNOWN holds an AddRef'd pointer, and GetPrivateData hands it back AddRef'd
    ComPtr<IDirect3DVertexBuffer9> other;
    CHECK(device->CreateVertexBuffer(64, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &other, nullptr));
    IUnknown *as_unknown = other.Get();
    CHECK(buffer->SetPrivateData(kMissing, &as_unknown, sizeof(as_unknown), D3DSPD_IUNKNOWN));
    IUnknown *read_unknown = nullptr;
    got = sizeof(read_unknown);
    expect(SUCCEEDED(buffer->GetPrivateData(kMissing, &read_unknown, &got)) && read_unknown == as_unknown,
           "IUnknown private data did not read back");
    if (read_unknown)
      read_unknown->Release();

    expect(SUCCEEDED(buffer->FreePrivateData(kBytes)), "freeing private data failed");
    expect(buffer->FreePrivateData(kBytes) == D3DERR_NOTFOUND, "freeing missing private data was not NOTFOUND");
    expect(SUCCEEDED(buffer->FreePrivateData(kMissing)), "freeing IUnknown private data failed");
  }

  // the same private data on a texture and one of its surfaces
  {
    ComPtr<IDirect3DTexture9> stored;
    CHECK(device->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &stored, nullptr));
    const DWORD value = 0x12345678;
    CHECK(stored->SetPrivateData(kBytes, &value, sizeof(value), 0));
    DWORD got = 0;
    expect(SUCCEEDED(stored->GetPrivateData(kBytes, nullptr, &got)) && got == sizeof(value),
           "texture private data did not report its size");

    ComPtr<IDirect3DSurface9> surface;
    CHECK(stored->GetSurfaceLevel(0, &surface));
    CHECK(surface->SetPrivateData(kBytes, &value, sizeof(value), 0));
    DWORD read = 0;
    got = sizeof(read);
    expect(SUCCEEDED(surface->GetPrivateData(kBytes, &read, &got)) && read == value,
           "surface private data did not read back");
  }

  device.Reset();
  d3d9.Reset();
  DestroyWindow(window);

  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
