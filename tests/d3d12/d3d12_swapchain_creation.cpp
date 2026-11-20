// contract: both HWND swap-chain creation calls require a direct queue and refuse the desktop window with
// E_ACCESSDENIED. "For Direct3D 12 this is a pointer to a direct command queue" (Microsoft Learn,
// IDXGIFactory::CreateSwapChain and IDXGIFactory2::CreateSwapChainForHwnd, pDevice). the desktop result is witnessed
// by Wine's dlls/d3d12/tests/d3d12.c, test_desktop_window: "ok(hr == E_ACCESSDENIED"; Direct3D 10 and 11's desktop
// blt swap chains are allowed by their test_desktop_window, so this restriction belongs to Direct3D 12.
// "the runtime obtains the size from the output window" for zero dimensions (CreateSwapChainForHwnd, Remarks).
// every size combination meets the same window and queue restrictions for both supported flip effects; two is the
// minimum flip-model buffer count (Microsoft Learn, Direct3D 12 Swap Chains, Swap effects).
// DXGI_SWAP_CHAIN_DESC1: "you must set the BufferCount member to from two to sixteen."
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/ns-dxgi1_2-dxgi_swap_chain_desc1
// that page also requires SampleDesc {1,0}. CreateSwapChainForHwnd: "Only one flip presentation model swap chain
// ... at a time may be associated with an HWND."
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforhwnd
#include "d3d12_test.hpp"
#include <dxgi1_4.h>
#include <limits>

int
main() {
  const UINT buffer_count = 2;
  WNDCLASSA wc{0,       DefWindowProcA, 0,       0,       GetModuleHandleA(nullptr),
               nullptr, nullptr,        nullptr, nullptr, "d3d12_swapchain_creation"};
  if (!expect(RegisterClassA(&wc) != 0, "could not register the window class"))
    return verdict();
  HWND window = CreateWindowA(
      wc.lpszClassName, wc.lpszClassName, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
      CW_USEDEFAULT, nullptr, nullptr, wc.hInstance, nullptr
  );
  if (!expect(window != nullptr, "could not create the window"))
    return verdict();

  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<IDXGIFactory4> factory;
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  {
    D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    ComPtr<ID3D12CommandQueue> queue;
    CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
    for (bool legacy : {false, true}) {
      auto create = [&](const DXGI_SWAP_CHAIN_DESC1 &desc, ComPtr<IDXGISwapChain> &chain) {
        if (legacy) {
          DXGI_SWAP_CHAIN_DESC old{{desc.Width, desc.Height, {}, desc.Format}, desc.SampleDesc, desc.BufferUsage,
                                   desc.BufferCount, window, TRUE, desc.SwapEffect, desc.Flags};
          return factory->CreateSwapChain(queue.Get(), &old, &chain);
        }
        ComPtr<IDXGISwapChain1> made;
        HRESULT hr = factory->CreateSwapChainForHwnd(queue.Get(), window, &desc, nullptr, nullptr, &made);
        if (made)
          expect(made.As(&chain) == S_OK, "swapchain interface");
        return hr;
      };
      for (auto effect : {DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, DXGI_SWAP_EFFECT_FLIP_DISCARD}) {
        DXGI_SWAP_CHAIN_DESC1 desc{3, 5, DXGI_FORMAT_R8G8B8A8_UNORM, FALSE, {1, 0},
                                   DXGI_USAGE_RENDER_TARGET_OUTPUT, buffer_count};
        desc.SwapEffect = effect;
        for (UINT count = 0; count <= DXGI_MAX_SWAP_CHAIN_BUFFERS + 1; count++) {
          step("%s effect %u buffer count %u", legacy ? "legacy" : "HWND", effect, count);
          desc.BufferCount = count;
          ComPtr<IDXGISwapChain> chain;
          HRESULT hr = create(desc, chain);
          bool valid = count >= buffer_count && count <= DXGI_MAX_SWAP_CHAIN_BUFFERS;
          expect(valid ? hr == S_OK : FAILED(hr), "count validation: %#lx", hr);
          expect(bool(chain) == valid, "count validation output");
          if (!valid || !chain)
            continue;
          for (UINT i = 0; i < count; i++) {
            ComPtr<ID3D12Resource> back;
            CHECK(chain->GetBuffer(i, IID_PPV_ARGS(&back)));
            auto got = back->GetDesc();
            expect(got.Width == desc.Width && got.Height == desc.Height && got.Format == desc.Format &&
                       got.SampleDesc.Count == desc.SampleDesc.Count && got.SampleDesc.Quality == desc.SampleDesc.Quality,
                   "buffer %u descriptor", i);
          }
          ComPtr<ID3D12Resource> past;
          expect(FAILED(chain->GetBuffer(count, IID_PPV_ARGS(&past))) && !past, "GetBuffer past count");
          for (bool extended : {false, true}) {
            ComPtr<IDXGISwapChain3> chain3;
            CHECK(chain.As(&chain3));
            const UINT node = 1;
            IUnknown *present_queue = queue.Get();
            HRESULT resized = extended ? chain3->ResizeBuffers1(1, desc.Width, desc.Height, DXGI_FORMAT_UNKNOWN,
                                                                 0, &node, &present_queue)
                                       : chain->ResizeBuffers(1, desc.Width, desc.Height, DXGI_FORMAT_UNKNOWN, 0);
            expect(FAILED(resized), "one-buffer flip ResizeBuffers%s: %#lx", extended ? "1" : "", resized);
            DXGI_SWAP_CHAIN_DESC got{};
            CHECK(chain->GetDesc(&got));
            expect(got.BufferCount == count, "refused resize changed buffer count");
          }
        }
        desc.BufferCount = buffer_count;
        for (UINT samples : {0u, 1u, 2u, UINT(D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT),
                             UINT(D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT) + 1, std::numeric_limits<UINT>::max()})
          for (UINT quality : {0u, 1u, std::numeric_limits<UINT>::max()}) {
            step("%s effect %u samples %u quality %u", legacy ? "legacy" : "HWND", effect, samples, quality);
            desc.SampleDesc = {samples, quality};
            ComPtr<IDXGISwapChain> chain;
            HRESULT hr = create(desc, chain);
            const bool valid = samples == 1 && !quality;
            expect(valid ? hr == S_OK : FAILED(hr), "sample validation: %#lx", hr);
            expect(bool(chain) == valid, "sample validation output");
          }
        desc.SampleDesc = {1, 0};
        step("%s effect %u duplicate HWND and release/recreate", legacy ? "legacy" : "HWND", effect);
        ComPtr<IDXGISwapChain> first, duplicate;
        CHECK(create(desc, first));
        HRESULT hr = create(desc, duplicate);
        expect(FAILED(hr) && !duplicate, "second flip chain for HWND: %#lx", hr);
        duplicate.Reset();
        first.Reset();
        CHECK(create(desc, first));
      }
    }
  }
  for (int type = D3D12_COMMAND_LIST_TYPE_DIRECT; type <= D3D12_COMMAND_LIST_TYPE_COPY; type++) {
    if (type == D3D12_COMMAND_LIST_TYPE_BUNDLE)
      continue;
    D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE(type)};
    ComPtr<ID3D12CommandQueue> queue;
    CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
    ComPtr<IUnknown> unknown;
    CHECK(queue.As(&unknown));
    queue->AddRef();
    const ULONG references = queue->Release();
    for (HWND target : {window, GetDesktopWindow(), HWND(nullptr), window}) {
      RECT area{};
      if (!expect(GetClientRect(target ? target : window, &area) != 0, "could not get the client area"))
        continue;
      for (UINT width : {0u, UINT(area.right), UINT(area.right) + 1})
        for (UINT height : {0u, UINT(area.bottom), UINT(area.bottom) + 1})
          for (int effect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; effect <= DXGI_SWAP_EFFECT_FLIP_DISCARD; effect++)
            for (bool legacy : {false, true}) {
              step("queue type %d, target %s, size %ux%u, effect %d, %s", type,
                   !target            ? "null"
                   : target == window ? "window"
                                      : "desktop",
                   width, height, effect, legacy ? "CreateSwapChain" : "CreateSwapChainForHwnd");
              HRESULT wanted = !target || type != D3D12_COMMAND_LIST_TYPE_DIRECT ? DXGI_ERROR_INVALID_CALL
                               : target == window                                ? S_OK
                                                                                 : E_ACCESSDENIED;
              DXGI_SWAP_CHAIN_DESC1 desc{width,       height, DXGI_FORMAT_R8G8B8A8_UNORM,
                                         FALSE,       {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT,
                                         buffer_count};
              desc.SwapEffect = DXGI_SWAP_EFFECT(effect);
              ComPtr<IDXGISwapChain> swapchain;
              HRESULT hr;
              if (legacy) {
                DXGI_SWAP_CHAIN_DESC old{{width, height, {}, desc.Format},
                                         desc.SampleDesc,
                                         desc.BufferUsage,
                                         desc.BufferCount,
                                         target,
                                         TRUE,
                                         desc.SwapEffect};
                hr = factory->CreateSwapChain(unknown.Get(), &old, &swapchain);
              } else {
                ComPtr<IDXGISwapChain1> made;
                hr = factory->CreateSwapChainForHwnd(unknown.Get(), target, &desc, nullptr, nullptr, &made);
                if (made)
                  CHECK(made.As(&swapchain));
              }
              expect(hr == wanted, "got HRESULT %#lx, expected %#lx", hr, wanted);
              expect(bool(swapchain) == (wanted == S_OK), "unexpected swap-chain output");
              if (hr == S_OK && swapchain) {
                DXGI_SWAP_CHAIN_DESC got{};
                CHECK(swapchain->GetDesc(&got));
                expect(got.OutputWindow == target && got.BufferDesc.Width == (width ? width : UINT(area.right)) &&
                           got.BufferDesc.Height == (height ? height : UINT(area.bottom)),
                       "the returned window or buffer size differs");
              }
              swapchain.Reset();
              queue->AddRef();
              expect(queue->Release() == references, "creation or release retained the queue");
            }
    }
  }
  DestroyWindow(window);
  UnregisterClassA(wc.lpszClassName, wc.hInstance);
  return verdict();
}
