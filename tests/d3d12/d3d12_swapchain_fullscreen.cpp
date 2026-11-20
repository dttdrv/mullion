// contract: fullscreen creation enters the output and window state that GetDesc and GetFullscreenState report;
// windowed creation and leaving fullscreen preserve the window, and release drops the creation's references.
// "Set it to NULL to create a windowed swap chain." (Microsoft Learn, CreateSwapChainForHwnd, pFullscreenDesc:
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforhwnd).
// "TRUE if the swap chain is in windowed mode; otherwise, FALSE." (DXGI_SWAP_CHAIN_FULLSCREEN_DESC, Windowed:
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/ns-dxgi1_2-dxgi_swap_chain_fullscreen_desc).
// "when the mode is full screen; otherwise NULL." (GetFullscreenState, ppTarget:
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-getfullscreenstate).
// "contains the majority of the client area of the target window." (GetContainingOutput:
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-getcontainingoutput).
// "the swap chain will be created in windowed mode and DXGI_STATUS_OCCLUDED will be returned." (CreateSwapChain,
// Remarks: https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgifactory-createswapchain).
// Wine's dxgi test_create_swapchain and test_swapchain_fullscreen_state witness the output identity and restored
// rectangle on Windows. the current output mode is the only requested mode, including when ALLOW_MODE_SWITCH is
// set: the test is safe without Wine's EmulateModeset and avoids win32u's make_ratio on unlike resolutions.
#include "d3d12_test.hpp"
#include <d3d11.h>
#include <dxgi1_4.h>
#include <climits>

int
main() {
  const DWORD timeout_ms = 60000;
  auto finished = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  if (!expect(finished != nullptr, "could not create watchdog event"))
    return verdict();
  auto watchdog = CreateThread(
      nullptr, 0,
      [](void *event) WINAPI -> DWORD {
        if (WaitForSingleObject(event, timeout_ms) != WAIT_OBJECT_0) {
          printf("failed: creation or teardown did not finish within %lu ms\n", timeout_ms);
          TerminateProcess(GetCurrentProcess(), 1);
        }
        return 0;
      },
      finished, 0, nullptr);
  if (!expect(watchdog != nullptr, "could not create watchdog"))
    return verdict();
  auto references = [](IUnknown *object) {
    object->AddRef();
    return object->Release();
  };
  {
    ComPtr<IDXGIFactory4> factory;
    CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    CHECK(factory->EnumAdapters1(0, &adapter));
    ComPtr<ID3D11Device> device11;
    CHECK(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                            &device11, nullptr, nullptr));
    ComPtr<ID3D12Device> device12;
    CHECK(make_device(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12)));
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    CHECK(device12->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
    UINT outputs = 0;
    for (UINT index = 0;; index++) {
      ComPtr<IDXGIOutput> output;
      HRESULT hr = adapter->EnumOutputs(index, &output);
      if (hr == DXGI_ERROR_NOT_FOUND)
        break;
      CHECK(hr);
      DXGI_OUTPUT_DESC output_desc{};
      CHECK(output->GetDesc(&output_desc));
      if (!output_desc.AttachedToDesktop)
        continue;
      outputs++;
      DEVMODEW current{}, registry{};
      current.dmSize = sizeof(current);
      registry.dmSize = sizeof(registry);
      if (!expect(EnumDisplaySettingsW(output_desc.DeviceName, ENUM_CURRENT_SETTINGS, &current) &&
                      EnumDisplaySettingsW(output_desc.DeviceName, ENUM_REGISTRY_SETTINGS, &registry),
                  "could not query output %u's display mode", index))
        return verdict();
      // R8G8B8A8_UNORM occupies a UINT32 (DXGI_FORMAT); leaving fullscreen restores the registry mode.
      if (!expect(current.dmBitsPerPel == sizeof(UINT32) * CHAR_BIT && current.dmPelsWidth == registry.dmPelsWidth &&
                      current.dmPelsHeight == registry.dmPelsHeight && current.dmBitsPerPel == registry.dmBitsPerPel &&
                      current.dmDisplayFrequency == registry.dmDisplayFrequency,
                  "output %u's current and restored modes differ; fullscreen is unsafe", index))
        return verdict();
      DXGI_MODE_DESC requested{
          current.dmPelsWidth, current.dmPelsHeight, {current.dmDisplayFrequency, 1}, DXGI_FORMAT_R8G8B8A8_UNORM};
      DXGI_MODE_DESC mode{};
      CHECK(output->FindClosestMatchingMode(&requested, &mode, device11.Get()));
      if (!expect(mode.Width == current.dmPelsWidth && mode.Height == current.dmPelsHeight &&
                      mode.RefreshRate.Denominator &&
                      mode.RefreshRate.Numerator / mode.RefreshRate.Denominator == current.dmDisplayFrequency,
                  "output %u's matching mode would change the display", index))
        return verdict();
      // "Finds the display mode that most closely matches the requested display mode" (Microsoft Learn,
      // IDXGIOutput::FindClosestMatchingMode): a rate below every mode's still has a closest mode, of the requested
      // size and no faster than the current one.
      DXGI_MODE_DESC slow = requested, slowest{};
      slow.RefreshRate = {1, UINT_MAX};
      expect(output->FindClosestMatchingMode(&slow, &slowest, device11.Get()) == S_OK &&
                 slowest.Width == requested.Width && slowest.Height == requested.Height &&
                 slowest.RefreshRate.Numerator <= UINT64(current.dmDisplayFrequency) * slowest.RefreshRate.Denominator,
             "output %u has no mode closest to a rate below them all", index);
      MONITORINFO monitor{sizeof(monitor)};
      if (!expect(GetMonitorInfoW(output_desc.Monitor, &monitor), "could not query output %u's work area", index))
        return verdict();
      const RECT work = monitor.rcWork;
      HWND window = CreateWindowA("static", "d3d12_swapchain_fullscreen", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                  work.left + (work.right - work.left) / 4, work.top + (work.bottom - work.top) / 4,
                                  (work.right - work.left) / 2, (work.bottom - work.top) / 2, nullptr, nullptr,
                                  GetModuleHandleA(nullptr), nullptr);
      if (!expect(window != nullptr, "could not create the window"))
        return verdict();
      RECT initial{};
      if (!expect(GetWindowRect(window, &initial), "could not read the window rectangle"))
        return verdict();
      for (int effect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; effect <= DXGI_SWAP_EFFECT_FLIP_DISCARD; effect++)
        for (UINT flags : {0u, UINT(DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH)})
          for (bool legacy : {false, true}) {
            bool entered11 = false, entered12 = false;
            for (BOOL windowed : {FALSE, TRUE})
              for (bool defaults : {false, true}) {
                if (defaults && (legacy || !windowed))
                  continue;
                for (bool native_refresh : {false, true})
                  for (bool d3d12 : {false, true}) {
                    step("output=%u mode=%ux%u@%u/%u effect=%d flags=%#x call=%s windowed=%d defaults=%d "
                         "native_refresh=%d D3D%u: create",
                         index, mode.Width, mode.Height, mode.RefreshRate.Numerator, mode.RefreshRate.Denominator,
                         effect, flags, legacy ? "CreateSwapChain" : "CreateSwapChainForHwnd", windowed, defaults,
                         native_refresh, d3d12 ? 12 : 11);
                    IUnknown *owner = d3d12 ? static_cast<IUnknown *>(queue.Get()) : device11.Get();
                    const auto owner_refs = references(owner), factory_refs = references(factory.Get());
                    // two is the minimum flip-model count (Microsoft Learn, DXGI_SWAP_CHAIN_DESC1, BufferCount).
                    DXGI_SWAP_CHAIN_DESC1 desc{mode.Width,
                                               mode.Height,
                                               mode.Format,
                                               FALSE,
                                               {1, 0},
                                               DXGI_USAGE_RENDER_TARGET_OUTPUT,
                                               2,
                                               DXGI_SCALING_STRETCH,
                                               DXGI_SWAP_EFFECT(effect),
                                               DXGI_ALPHA_MODE_IGNORE,
                                               flags};
                    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs{native_refresh ? DXGI_RATIONAL{} : mode.RefreshRate,
                                                       mode.ScanlineOrdering, mode.Scaling, windowed};
                    ComPtr<IDXGISwapChain> chain;
                    if (legacy) {
                      DXGI_SWAP_CHAIN_DESC old{
                          {desc.Width, desc.Height, fs.RefreshRate, desc.Format, fs.ScanlineOrdering, fs.Scaling},
                          desc.SampleDesc,
                          desc.BufferUsage,
                          desc.BufferCount,
                          window,
                          fs.Windowed,
                          desc.SwapEffect,
                          desc.Flags};
                      hr = factory->CreateSwapChain(owner, &old, &chain);
                    } else {
                      ComPtr<IDXGISwapChain1> made;
                      hr = factory->CreateSwapChainForHwnd(owner, window, &desc, defaults ? nullptr : &fs, nullptr,
                                                           &made);
                      if (made)
                        CHECK(made.As(&chain));
                    }
                    expect(hr == S_OK || (!windowed && hr == DXGI_STATUS_OCCLUDED), "creation returned %#lx", hr);
                    if (!expect(chain != nullptr, "creation did not return a swap chain"))
                      return verdict();
                    const bool entered = !windowed && hr == S_OK;
                    (d3d12 ? entered12 : entered11) |= entered;
                    const std::string variant = trace::doing;
                    auto state = [&](bool fullscreen) {
                      DXGI_SWAP_CHAIN_DESC got{};
                      expect(chain->GetDesc(&got) == S_OK, "GetDesc failed");
                      expect(got.Windowed == !fullscreen && got.OutputWindow == window &&
                                 got.BufferDesc.Width == desc.Width && got.BufferDesc.Height == desc.Height &&
                                 got.BufferCount == desc.BufferCount && got.Flags == flags &&
                                 got.BufferDesc.Format == desc.Format &&
                                 got.BufferDesc.RefreshRate.Numerator == (defaults ? 0u : fs.RefreshRate.Numerator) &&
                                 got.BufferDesc.RefreshRate.Denominator == (defaults ? 0u : fs.RefreshRate.Denominator),
                             "GetDesc disagrees with the created state");
                      ComPtr<IDXGISwapChain1> chain1;
                      if (expect(SUCCEEDED(chain.As(&chain1)), "could not query IDXGISwapChain1")) {
                        DXGI_SWAP_CHAIN_FULLSCREEN_DESC got_fs{};
                        expect(chain1->GetFullscreenDesc(&got_fs) == S_OK && got_fs.Windowed == !fullscreen,
                               "GetFullscreenDesc disagrees with the created state");
                      }
                      ComPtr<IDXGIOutput> containing;
                      if (expect(chain->GetContainingOutput(&containing) == S_OK && containing,
                                 "GetContainingOutput did not return an output")) {
                        DXGI_OUTPUT_DESC got_output{};
                        expect(containing->GetDesc(&got_output) == S_OK && got_output.Monitor == output_desc.Monitor,
                               "containing output is not the window's monitor");
                      }
                      for (bool get_state : {false, true})
                        for (bool get_target : {false, true}) {
                          BOOL got_fullscreen = !fullscreen;
                          ComPtr<IDXGIOutput> target = output;
                          expect(chain->GetFullscreenState(get_state ? &got_fullscreen : nullptr,
                                                           get_target ? target.ReleaseAndGetAddressOf() : nullptr) ==
                                     S_OK,
                                 "GetFullscreenState failed");
                          if (get_state)
                            expect(got_fullscreen == fullscreen, "GetFullscreenState disagrees with GetDesc");
                          if (get_target)
                            expect(fullscreen ? target && target.Get() == containing.Get() : !target,
                                   "fullscreen target disagrees with containing output or is not null when windowed");
                        }
                      RECT rect{};
                      expect(GetWindowRect(window, &rect), "could not read window rectangle");
                      const RECT wanted = fullscreen ? output_desc.DesktopCoordinates : initial;
                      expect(EqualRect(&rect, &wanted), "window rect (%ld,%ld)-(%ld,%ld), expected (%ld,%ld)-(%ld,%ld)",
                             rect.left, rect.top, rect.right, rect.bottom, wanted.left, wanted.top, wanted.right,
                             wanted.bottom);
                    };
                    state(entered);
                    step("%s: leave fullscreen", variant.c_str());
                    expect(chain->SetFullscreenState(FALSE, nullptr) == S_OK, "could not leave fullscreen");
                    state(false);
                    step("%s: release", variant.c_str());
                    expect(chain.Detach()->Release() == 0, "swap chain retained references");
                    expect(references(owner) == owner_refs && references(factory.Get()) == factory_refs,
                           "creation or teardown leaked owner or factory references");
                    RECT after{};
                    expect(GetWindowRect(window, &after) && EqualRect(&after, &initial), "release changed the window");
                    DEVMODEW after_mode{};
                    after_mode.dmSize = sizeof(after_mode);
                    expect(EnumDisplaySettingsW(output_desc.DeviceName, ENUM_CURRENT_SETTINGS, &after_mode) &&
                               after_mode.dmPelsWidth == current.dmPelsWidth &&
                               after_mode.dmPelsHeight == current.dmPelsHeight &&
                               after_mode.dmBitsPerPel == current.dmBitsPerPel &&
                               after_mode.dmDisplayFrequency == current.dmDisplayFrequency,
                           "creation or teardown changed the display mode");
                  }
              }
            expect(entered11 && entered12,
                   "output=%u effect=%d flags=%#x %s: fullscreen was not exercised by both APIs", index, effect, flags,
                   legacy ? "CreateSwapChain" : "CreateSwapChainForHwnd");
          }
      expect(DestroyWindow(window), "could not destroy the window");
    }
    expect(outputs != 0, "no attached output was tested");
    step("device and factory teardown");
  }
  SetEvent(finished);
  expect(WaitForSingleObject(watchdog, timeout_ms) == WAIT_OBJECT_0, "watchdog did not finish");
  CloseHandle(watchdog);
  CloseHandle(finished);
  return verdict();
}
