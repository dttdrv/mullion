// contract: creating a swap chain for a window that is gone, or that was never one, must not crash in the PE/Unix
// Metal-view bridge. CreateSwapChainForHwnd requires a non-null HWND (DXGI_ERROR_INVALID_CALL) but a destroyed HWND
// is still non-null; winemetal's CreateMetalViewFromHWND used to dereference get_win_data's null result.
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforhwnd
#include "d3d11_test.hpp"
#include <dxgi1_2.h>

int
main() {
  unsigned wrong = 0, checks = 0;
  auto expect = [&](bool ok, const char *what) {
    checks++;
    if (!ok && wrong++ < 16)
      printf("wrong: %s\n", what);
  };

  WNDCLASSA wc{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr, "d3d11_hwnd"};
  RegisterClassA(&wc);
  HWND window = CreateWindowA("d3d11_hwnd", "d3d11_hwnd", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
                              wc.hInstance, nullptr);
  if (!window) {
    printf("failed: no window\n");
    return 1;
  }

  ComPtr<ID3D11Device> device;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                          nullptr));
  ComPtr<IDXGIDevice> dxgi_device;
  CHECK(device.As(&dxgi_device));
  ComPtr<IDXGIAdapter> adapter;
  CHECK(dxgi_device->GetAdapter(&adapter));
  ComPtr<IDXGIFactory2> factory;
  {
    ComPtr<IDXGIFactory> factory1;
    CHECK(adapter->GetParent(IID_PPV_ARGS(&factory1)));
    CHECK(factory1.As(&factory));
  }

  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = 64;
  desc.Height = 64;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

  // a destroyed HWND is still a non-null handle: the bridge must fail or fall back, not crash
  DestroyWindow(window);
  expect(!IsWindow(window), "DestroyWindow left a window");
  ComPtr<IDXGISwapChain1> destroyed_chain;
  HRESULT destroyed = factory->CreateSwapChainForHwnd(device.Get(), window, &desc, nullptr, nullptr, &destroyed_chain);
  expect(destroyed_chain.Get() == nullptr || SUCCEEDED(destroyed),
         "CreateSwapChainForHwnd of a destroyed HWND crashed or left a chain without succeeding");
  destroyed_chain.Reset();

  HWND garbage = (HWND)(uintptr_t)0x1234;
  expect(!IsWindow(garbage), "0x1234 is a window");
  ComPtr<IDXGISwapChain1> garbage_chain;
  HRESULT missing = factory->CreateSwapChainForHwnd(device.Get(), garbage, &desc, nullptr, nullptr, &garbage_chain);
  expect(garbage_chain.Get() == nullptr || SUCCEEDED(missing),
         "CreateSwapChainForHwnd of a handle that was never a window crashed or left a chain without succeeding");
  (void)destroyed;
  (void)missing;

  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
