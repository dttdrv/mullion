// contract: a Direct3D 12 swap chain's buffers, as DXGI's documentation has them.
// - GetCurrentBackBufferIndex is the buffer the next Present shows: 0 for new buffers, one further (around the
//   buffers) after each Present, and 0 again after ResizeBuffers (IDXGISwapChain3::GetCurrentBackBufferIndex).
// - ResizeBuffers keeps the buffer count for 0, the format for DXGI_FORMAT_UNKNOWN and takes the window's client size
//   for a size of 0; GetDesc1 then says what the buffers are, and a buffer has that size. a format no swap chain has
//   is refused and leaves the swap chain as it was, and so is a change of DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING, which
//   "can't be added or removed" by it (IDXGISwapChain::ResizeBuffers).
// - an output lists display modes for a format a swap chain can have, also for one of 16-bit floats
//   (IDXGIOutput::GetDisplayModeList), and an adapter's memory fits the SIZE_T that holds it.
// - D3D12EnableExperimentalFeatures of no features succeeds.
#include "d3d12_test.hpp"
#include <dxgi1_6.h>

int
main(int, char **) {
  unsigned wrong = 0, checks = 0;
  auto expect = [&](bool ok, const char *what) {
    checks++;
    if (!ok && wrong++ < 16)
      printf("%s\n", what);
  };
  expect(D3D12EnableExperimentalFeatures(0, nullptr, nullptr, nullptr) == S_OK, "enabling no experimental features fails");
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<IDXGIFactory4> factory;
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));

  const UINT client = 192, buffers = 3;
  const DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
  WNDCLASSA wc{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr, "d3d12_swapchain"};
  RegisterClassA(&wc);
  RECT rect{0, 0, (LONG)client, (LONG)client};
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  auto window = CreateWindowA(
      "d3d12_swapchain", "d3d12_swapchain", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, rect.right - rect.left,
      rect.bottom - rect.top, nullptr, nullptr, wc.hInstance, nullptr
  );
  DXGI_SWAP_CHAIN_DESC1 sc_desc{client, client, format, FALSE, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT, buffers,
                                DXGI_SCALING_STRETCH, DXGI_SWAP_EFFECT_FLIP_DISCARD};
  ComPtr<IDXGISwapChain1> sc1;
  ComPtr<IDXGISwapChain3> swapchain;
  CHECK(factory->CreateSwapChainForHwnd(queue.Get(), window, &sc_desc, nullptr, nullptr, &sc1));
  CHECK(sc1.As(&swapchain));

  // what the swap chain says its buffers are, and what its first buffer is
  auto is = [&](UINT width, UINT height, UINT count, DXGI_FORMAT buffer_format) {
    DXGI_SWAP_CHAIN_DESC1 desc{};
    ComPtr<ID3D12Resource> buffer, past;
    if (FAILED(swapchain->GetDesc1(&desc)) || FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&buffer))))
      return false;
    auto resource = buffer->GetDesc();
    return desc.Width == width && desc.Height == height && desc.BufferCount == count && desc.Format == buffer_format &&
           resource.Width == width && resource.Height == height && resource.Format == buffer_format &&
           SUCCEEDED(swapchain->GetBuffer(count - 1, IID_PPV_ARGS(&past))) &&
           FAILED(swapchain->GetBuffer(count, IID_PPV_ARGS(&past)));
  };
  expect(is(client, client, buffers, format), "a new swap chain is not what it was created as");
  expect(swapchain->GetCurrentBackBufferIndex() == 0, "a new swap chain's current buffer is not its first");
  for (UINT frame = 1; frame <= buffers + 1; frame++) {
    CHECK(swapchain->Present(0, 0));
    expect(swapchain->GetCurrentBackBufferIndex() == frame % buffers, "Present does not go on to the next buffer");
  }
  // after a present that left the index past the first buffer
  const UINT wider = client + 64, taller = client + 32, fewer = buffers - 1;
  CHECK(swapchain->ResizeBuffers(fewer, wider, taller, DXGI_FORMAT_B8G8R8A8_UNORM, 0));
  expect(is(wider, taller, fewer, DXGI_FORMAT_B8G8R8A8_UNORM), "resized buffers are not what was asked for");
  expect(swapchain->GetCurrentBackBufferIndex() == 0, "the current buffer of resized buffers is not the first");
  CHECK(swapchain->Present(0, 0));
  expect(swapchain->GetCurrentBackBufferIndex() == 1 % fewer, "Present of resized buffers does not go on to the next");
  // what is kept
  RECT area;
  GetClientRect(window, &area);
  CHECK(swapchain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0));
  expect(is(area.right, area.bottom, fewer, DXGI_FORMAT_B8G8R8A8_UNORM),
         "a resize to nothing new does not keep the count and format and take the window's size");
  // what is refused
  // a size of 0 is the window's in that direction alone ("If you specify zero, DXGI will use the width of the client
  // area of the target window", and so the height)
  CHECK(swapchain->ResizeBuffers(0, 0, taller, DXGI_FORMAT_UNKNOWN, 0));
  expect(is(area.right, taller, fewer, DXGI_FORMAT_B8G8R8A8_UNORM), "a resize to a height alone does not take the window's width with it");
  CHECK(swapchain->ResizeBuffers(0, wider, 0, DXGI_FORMAT_UNKNOWN, 0));
  expect(is(wider, area.bottom, fewer, DXGI_FORMAT_B8G8R8A8_UNORM), "a resize to a width alone does not take the window's height with it");
  CHECK(swapchain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0));
  expect(FAILED(swapchain->ResizeBuffers(0, wider, taller, DXGI_FORMAT_D32_FLOAT, 0)) &&
             is(area.right, area.bottom, fewer, DXGI_FORMAT_B8G8R8A8_UNORM),
         "a resize to a format no swap chain has is taken, or changes the swap chain");
  expect(swapchain->ResizeBuffers(0, wider, taller, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) == DXGI_ERROR_INVALID_CALL &&
             is(area.right, area.bottom, fewer, DXGI_FORMAT_B8G8R8A8_UNORM),
         "a resize adds tearing to a swap chain created without it");

  // modes and memory
  ComPtr<IDXGIAdapter1> adapter;
  ComPtr<IDXGIOutput> output;
  CHECK(factory->EnumAdapters1(0, &adapter));
  DXGI_ADAPTER_DESC1 adapter_desc{};
  CHECK(adapter->GetDesc1(&adapter_desc));
  expect(adapter_desc.DedicatedVideoMemory > 0, "the adapter has no video memory");
  if (SUCCEEDED(adapter->EnumOutputs(0, &output))) {
    UINT modes = 0, float_modes = 0;
    CHECK(output->GetDisplayModeList(format, 0, &modes, nullptr));
    CHECK(output->GetDisplayModeList(DXGI_FORMAT_R16G16B16A16_FLOAT, 0, &float_modes, nullptr));
    expect(modes > 0, "the output has no display modes");
    expect(float_modes == modes, "the output's display modes for 16-bit float buffers are not its modes");
  }
  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
