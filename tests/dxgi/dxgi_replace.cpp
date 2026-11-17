// contract: a window has one flip model swap chain at a time, and takes the next as soon as the one before is released:
// "you must first destroy the previous swap chain" is all that is asked ("DXGI flip model", Microsoft Learn,
// https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-flip-model). Wine's test_swapchain_present_count
// does it on Windows with a Direct3D 12 queue and no wait in between: a chain is made, presents, is released to no
// reference, and the next creation on the window succeeds. a second chain while the first lives is refused. nothing
// here waits for the device between the release and the creation, which is the point
#include "dxgi_test.hpp"

int
main() {
  HWND window = CreateWindowA(
      "static", "dxgi_replace", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 128, 128, nullptr, nullptr,
      GetModuleHandleA(nullptr), nullptr
  );
  if (!expect(window != nullptr, "CreateWindow: %lu", GetLastError()))
    return verdict();
  UpdateWindow(window);
  ComPtr<IDXGIFactory2> factory;
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  for (UINT api : {11u, 12u}) {
    DxgiDevice device;
    CHECK(device.create(api));
    DXGI_SWAP_CHAIN_DESC1 desc{64,   64, DXGI_FORMAT_R8G8B8A8_UNORM, FALSE, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT,
                               2,    DXGI_SCALING_STRETCH,       DXGI_SWAP_EFFECT_FLIP_DISCARD};
    for (UINT presents : {0u, 1u, 3u})
      for (UINT round = 0; round < 6; round++) {
        step("D3D%u: a chain that presented %u times is replaced, round %u", api, presents, round);
        desc.Flags = round & 1 ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0;
        IDXGISwapChain1 *chain = nullptr;
        HRESULT hr = factory->CreateSwapChainForHwnd(device.input.Get(), window, &desc, nullptr, nullptr, &chain);
        if (!expect(hr == S_OK && chain, "creation after a released chain: %#lx", hr))
          return verdict();
        ComPtr<IDXGISwapChain1> second;
        hr = factory->CreateSwapChainForHwnd(device.input.Get(), window, &desc, nullptr, nullptr, &second);
        expect(FAILED(hr) && !second, "a second chain beside a living one: %#lx", hr);
        for (UINT i = 0; i < presents; i++)
          CHECK(chain->Present(0, 0));
        // what Wine's test also asks of the chain before it lets it go: an interval past 4, and a present that only tests
        if (round >= 2) {
          expect(chain->Present(10, 0) == DXGI_ERROR_INVALID_CALL, "an interval of 10 is taken");
          expect(chain->Present(0, DXGI_PRESENT_TEST) == S_OK, "a test present fails");
        }
        if (round >= 4)
          CHECK(chain->Present(0, 0));
        expect(chain->Release() == 0, "the chain is still referenced");
      }
    // Wine's own order: the queue waits for a fence when the chain presents, the fence is signaled, and the chain is
    // released and replaced at once, while its present may still be the queue's to run
    if (api == 12)
      for (UINT round = 0; round < 8; round++) {
        step("D3D12: a chain whose present waits behind a fence is replaced, round %u", round);
        ComPtr<ID3D12Fence> fence;
        CHECK(device.d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        IDXGISwapChain1 *chain = nullptr;
        desc.Flags = 0;
        HRESULT hr = factory->CreateSwapChainForHwnd(device.input.Get(), window, &desc, nullptr, nullptr, &chain);
        if (!expect(hr == S_OK && chain, "creation after a chain whose present waited: %#lx", hr))
          return verdict();
        CHECK(device.queue->Wait(fence.Get(), 1));
        CHECK(chain->Present(0, 0));
        CHECK(fence->Signal(1));
        // what the release returns is not asked: Wine's test does not look at it
        chain->Release();
      }
    CHECK(device.idle());
  }
  DestroyWindow(window);
  return verdict();
}
