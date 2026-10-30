// contract: a Direct3D 9 resource holds a reference on its device, so a resource the application kept after releasing
// the application's device reference still has a device to answer GetDevice with, and the device is not freed under it.
// The device owns its bound resources and its implicit swap chain; on its last public reference it lets them go, so the
// private references the two directions hold do not keep each other alive.
// - an unbound buffer, a state block, a swap chain and a texture-level surface each outlive the application's device
//   reference and return a non-NULL device that can be released.
// - a buffer bound as the stream source does the same, and the device is freed when the last of the application and
//   the device lets go of it.
#include <windows.h>
#include <d3d9.h>
#include <wrl/client.h>
#include <cstdio>

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
                         "d3d9_lifetime"};
  RegisterClassA(&window_class);
  RECT window_rect{0, 0, (LONG)client, (LONG)client};
  AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);
  HWND window = CreateWindowA("d3d9_lifetime", "d3d9_lifetime", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0,
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

  // an unbound vertex buffer outlives the application's device reference
  {
    ComPtr<IDirect3DDevice9> device;
    CHECK(d3d9->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &present, &device));
    ComPtr<IDirect3DVertexBuffer9> buffer;
    CHECK(device->CreateVertexBuffer(sizeof(Vertex), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, kFVF, D3DPOOL_DEFAULT,
                                     &buffer, nullptr));
    device.Reset();
    ComPtr<IDirect3DDevice9> from_child;
    expect(SUCCEEDED(buffer->GetDevice(&from_child)) && from_child,
           "an unbound buffer lost its device when the application released it");
    from_child.Reset();
    buffer.Reset();
  }

  // a buffer bound as the stream source: the device holds it, so the application's release of either order is safe
  {
    ComPtr<IDirect3DDevice9> device;
    CHECK(d3d9->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &present, &device));
    ComPtr<IDirect3DVertexBuffer9> buffer;
    CHECK(device->CreateVertexBuffer(sizeof(Vertex), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, kFVF, D3DPOOL_DEFAULT,
                                     &buffer, nullptr));
    CHECK(device->SetStreamSource(0, buffer.Get(), 0, sizeof(Vertex)));
    device.Reset();
    ComPtr<IDirect3DDevice9> from_child;
    expect(SUCCEEDED(buffer->GetDevice(&from_child)) && from_child,
           "a bound buffer lost its device when the application released it");
    from_child.Reset();
    buffer.Reset();
  }

  // a state block, a chain and a texture level's surface each outlive the application's device reference
  {
    ComPtr<IDirect3DDevice9> device;
    CHECK(d3d9->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &present, &device));

    ComPtr<IDirect3DStateBlock9> block;
    CHECK(device->CreateStateBlock(D3DSBT_ALL, &block));
    ComPtr<IDirect3DSwapChain9> chain;
    CHECK(device->GetSwapChain(0, &chain));
    ComPtr<IDirect3DTexture9> texture;
    CHECK(device->CreateTexture(8, 8, 2, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr));
    ComPtr<IDirect3DSurface9> level;
    CHECK(texture->GetSurfaceLevel(1, &level));

    device.Reset();

    ComPtr<IDirect3DDevice9> from_block, from_chain, from_level;
    expect(SUCCEEDED(block->GetDevice(&from_block)) && from_block, "a state block lost its device");
    expect(SUCCEEDED(chain->GetDevice(&from_chain)) && from_chain, "a swap chain lost its device");
    expect(SUCCEEDED(level->GetDevice(&from_level)) && from_level, "a texture level's surface lost its device");
    from_block.Reset();
    from_chain.Reset();
    from_level.Reset();

    level.Reset();
    texture.Reset();
    chain.Reset();
    block.Reset();
  }

  // the implicit back buffer surface is the device's, and a retained one still names it
  {
    ComPtr<IDirect3DDevice9> device;
    CHECK(d3d9->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &present, &device));
    ComPtr<IDirect3DSurface9> back_buffer;
    CHECK(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer));
    device.Reset();
    ComPtr<IDirect3DDevice9> from_surface;
    expect(SUCCEEDED(back_buffer->GetDevice(&from_surface)) && from_surface, "a back buffer lost its device");
    from_surface.Reset();
    back_buffer.Reset();
  }

  d3d9.Reset();
  DestroyWindow(window);

  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
