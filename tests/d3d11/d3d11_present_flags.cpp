// contract: DXGI_PRESENT_DO_NOT_WAIT returns DXGI_ERROR_WAS_STILL_DRAWING without blocking when the present queue
// is occupied (Microsoft Learn, IDXGISwapChain::Present). the buffer sequence is unchanged on that failure.
#include "d3d11_test.hpp"
#include <dxgi.h>
#include <dxgi1_2.h>

int
main() {
  WNDCLASSA wc{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr,
               "d3d11_present_flags"};
  RegisterClassA(&wc);
  HWND window = CreateWindowA("d3d11_present_flags", "d3d11_present_flags", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr,
                              nullptr, wc.hInstance, nullptr);
  if (!window) {
    printf("failed: no window\n");
    return 1;
  }
  DXGI_SWAP_CHAIN_DESC desc{{64, 64, {60, 1}, DXGI_FORMAT_R8G8B8A8_UNORM}, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT, 2,
                            window, TRUE, DXGI_SWAP_EFFECT_DISCARD, 0};
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<IDXGISwapChain> swapchain;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &desc,
                                      &swapchain, &device, nullptr, &context));
  ComPtr<IDXGIDevice1> dxgi;
  CHECK(device.As(&dxgi));
  CHECK(dxgi->SetMaximumFrameLatency(1));
  ComPtr<IDXGISwapChain1> swapchain1;
  CHECK(swapchain.As(&swapchain1));
  expect(SUCCEEDED(swapchain1->Present(0, 0)), "the first Present failed");
  UINT present_before = 0, present_after = 0;
  CHECK(swapchain->GetLastPresentCount(&present_before));
  const DWORD began = GetTickCount();
  HRESULT busy = S_OK;
  for (UINT i = 0; i < 64; i++) {
    busy = swapchain1->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
    if (busy == DXGI_ERROR_WAS_STILL_DRAWING)
      break;
  }
  const DWORD elapsed = GetTickCount() - began;
  CHECK(swapchain->GetLastPresentCount(&present_after));
  expect(busy == DXGI_ERROR_WAS_STILL_DRAWING || elapsed < 200,
         "DO_NOT_WAIT neither returned WAS_STILL_DRAWING nor returned promptly");
  expect(elapsed < 2000, "DO_NOT_WAIT blocked instead of failing");
  if (busy == DXGI_ERROR_WAS_STILL_DRAWING)
    expect(present_before == present_after, "DO_NOT_WAIT changed the present count from %u to %u", present_before,
           present_after);
  expect(SUCCEEDED(swapchain1->Present(0, 0)), "Present after DO_NOT_WAIT did not recover");
  return verdict();
}
