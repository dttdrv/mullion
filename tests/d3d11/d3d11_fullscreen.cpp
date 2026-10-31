// contract: fullscreen transitions select an output and preserve the application's window state; rejected calls
// leave it untouched. "If you pass FALSE to Fullscreen, then you must set this parameter to NULL."
// (Microsoft Learn, IDXGISwapChain::SetFullscreenState, pTarget).
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-setfullscreenstate
// ResizeTarget resizes the window or the fullscreen output, and "only ResizeBuffers can change the back buffers"
// (Microsoft Learn, IDXGISwapChain::ResizeTarget, Remarks).
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-resizetarget
// "If either Width or Height are 0, both must be 0." (Microsoft Learn, IDXGIOutput::FindClosestMatchingMode).
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgioutput-findclosestmatchingmode
// the exact styles and saved rectangle are Windows' results recorded by Wine's test_swapchain_window_styles and
// test_resize_fullscreen in dlls/dxgi/tests/dxgi.c. its test_swapchain_window_messages records nested transitions:
// a call for the current state succeeds, and a call for the opposite state returns MODE_CHANGE_IN_PROGRESS.
// ResizeTarget returns that status during a fullscreen/windowed transition (ResizeTarget, Return value).
// frame changes require SetWindowPos with "the SWP_FRAMECHANGED flag for the cache to be updated properly"
// (Microsoft Learn, SetWindowLongW, Remarks).
// https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowlongw
// ALLOW_MODE_SWITCH: "Set this flag to enable an application to switch modes by calling IDXGISwapChain::ResizeTarget."
// without it the display stays in its desktop mode (DXGI_SWAP_CHAIN_FLAG, ALLOW_MODE_SWITCH).
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi/ne-dxgi-dxgi_swap_chain_flag
// leaving fullscreen restores the registry mode, as Wine's test_mode_change records, even if entry used another.
// SetMaximumFrameLatency is "only valid for use on swap chains created with" FRAME_LATENCY_WAITABLE_OBJECT
// (Microsoft Learn, IDXGISwapChain2::SetMaximumFrameLatency, Remarks); Wine's test_frame_latency_event records
// the same requirement for its getter and preservation of its output on refusal.
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-setmaximumframelatency
// the range is DXVK's independent D3D11SwapChain::SetFrameLatency reference: "MaxLatency == 0 || MaxLatency >
// DXGI_MAX_SWAP_CHAIN_BUFFERS" returns DXGI_ERROR_INVALID_CALL; Wine's test_frame_latency_event also checks zero.
// https://github.com/doitsujin/dxvk/blob/master/src/d3d11/d3d11_swapchain.cpp
// gamma capabilities are "only supported while in full-screen mode" (IDXGIOutput::GetGammaControlCapabilities).
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgioutput-getgammacontrolcapabilities
// colour-space setting agrees with advertised PRESENT support, as Wine's test_colour_space_support records.
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiswapchain3-checkcolorspacesupport
// MakeWindowAssociation takes "One or more of the following values", the DXGI_MWA flags; its getter returns
// NULL even after a successful association, as Wine's test_window_association records.
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgifactory-makewindowassociation
#include "d3d11_test.hpp"
#include <d3d10.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <algorithm>
#include <limits>
#include <numeric>
#include <random>

static bool test_nested = true, destroy_on_style = false;
static UINT buffer_width, buffer_height;
static UINT nested_messages;

static LRESULT CALLBACK
window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  auto swapchain = reinterpret_cast<IDXGISwapChain *>(GetWindowLongPtrW(window, GWLP_USERDATA));
  static bool nested = false;
  if (swapchain && message == WM_SIZE) {
    DXGI_SWAP_CHAIN_DESC desc{};
    expect(swapchain->GetDesc(&desc) == S_OK && desc.BufferDesc.Width == buffer_width &&
               desc.BufferDesc.Height == buffer_height,
           "window message changed back buffer size");
  }
  if (destroy_on_style && message == WM_STYLECHANGING) {
    destroy_on_style = false;
    SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    DestroyWindow(window);
    return 0;
  }
  if (swapchain && test_nested && (message == WM_STYLECHANGING || message == WM_WINDOWPOSCHANGING)) {
    if (expect(!nested, "window procedure reentered by SetFullscreenState")) {
      nested = true;
      nested_messages++;
      BOOL fullscreen = FALSE;
      expect(swapchain->GetFullscreenState(&fullscreen, nullptr) == S_OK, "GetFullscreenState in message %u", message);
      expect(swapchain->SetFullscreenState(fullscreen, nullptr) == S_OK, "nested call for current state");
      expect(swapchain->SetFullscreenState(!fullscreen, nullptr) == DXGI_STATUS_MODE_CHANGE_IN_PROGRESS,
             "nested call for opposite state");
      DXGI_MODE_DESC mode{};
      expect(swapchain->ResizeTarget(&mode) == DXGI_STATUS_MODE_CHANGE_IN_PROGRESS,
             "resize during fullscreen transition");
      ComPtr<IDXGIOutput> output;
      if (expect(swapchain->GetContainingOutput(&output) == S_OK, "containing output in message %u", message))
        expect(swapchain->SetFullscreenState(FALSE, output.Get()) == DXGI_ERROR_INVALID_CALL,
               "invalid target has priority over nested transition");
      nested = false;
    }
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

int
main(int argc, char **argv) {
  const unsigned seed = argc > 1 ? strtoul(argv[1], nullptr, 0) : std::minstd_rand::default_seed;
  std::minstd_rand random(seed);
  WNDCLASSW wc{};
  wc.lpfnWndProc = window_proc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"d3d11_fullscreen";
  if (!expect(RegisterClassW(&wc) != 0, "register window class"))
    return verdict();
#ifdef TEST_D3D12
  const auto versions = {10u, 11u, 12u};
#else
  // the library builds no Direct3D 12 here (src/meson.build)
  const auto versions = {10u, 11u};
#endif
  for (UINT version : versions) {
    step("seed=%u Direct3D=%u device", seed, version);
    ComPtr<IUnknown> device;
    ComPtr<ID3D12Device> d3d12;
    if (version == 10) {
      auto module = LoadLibraryW(L"d3d10.dll");
      auto create = module ? reinterpret_cast<decltype(&D3D10CreateDevice)>(GetProcAddress(module, "D3D10CreateDevice"))
                           : nullptr;
      if (!expect(create != nullptr, "D3D10CreateDevice entry"))
        return verdict();
      ComPtr<ID3D10Device> d3d10;
      CHECK(create(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, D3D10_SDK_VERSION, &d3d10));
      CHECK(d3d10.As(&device));
    } else if (version == 11) {
      ComPtr<ID3D11Device> d3d11;
      CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d3d11,
                              nullptr, nullptr));
      CHECK(d3d11.As(&device));
    } else {
      auto module = LoadLibraryW(L"d3d12.dll");
      auto create =
          module ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(module, "D3D12CreateDevice")) : nullptr;
      if (!expect(create != nullptr, "D3D12CreateDevice entry"))
        return verdict();
      CHECK(create(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d12)));
      D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
      ComPtr<ID3D12CommandQueue> queue;
      CHECK(d3d12->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
      CHECK(queue.As(&device));
    }
    ComPtr<IDXGIFactory4> factory;
    CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter> adapter;
    if (version == 12) {
      CHECK(factory->EnumAdapterByLuid(d3d12->GetAdapterLuid(), IID_PPV_ARGS(&adapter)));
    } else {
      ComPtr<IDXGIDevice> dxgi_device;
      CHECK(device.As(&dxgi_device));
      CHECK(dxgi_device->GetAdapter(&adapter));
    }
    for (UINT index = 0;; index++) {
      ComPtr<IDXGIOutput> output;
      auto hr = adapter->EnumOutputs(index, &output);
      if (hr == DXGI_ERROR_NOT_FOUND) {
        expect(index != 0, "adapter has no output");
        break;
      }
      CHECK(hr);
      DXGI_OUTPUT_DESC output_desc{};
      CHECK(output->GetDesc(&output_desc));
      DEVMODEW desktop{};
      desktop.dmSize = sizeof(desktop);
      if (!expect(EnumDisplaySettingsW(output_desc.DeviceName, ENUM_CURRENT_SETTINGS, &desktop), "desktop mode"))
        return verdict();
      DEVMODEW registry{};
      registry.dmSize = sizeof(registry);
      if (!expect(EnumDisplaySettingsW(output_desc.DeviceName, ENUM_REGISTRY_SETTINGS, &registry), "registry mode"))
        return verdict();
      const LONG width = output_desc.DesktopCoordinates.right - output_desc.DesktopCoordinates.left;
      const LONG height = output_desc.DesktopCoordinates.bottom - output_desc.DesktopCoordinates.top;
      UINT mode_count = 0;
      CHECK(output->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, 0, &mode_count, nullptr));
      std::vector<DXGI_MODE_DESC> modes(mode_count);
      CHECK(output->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, 0, &mode_count, modes.data()));
      modes.resize(mode_count);
      if (!expect(!modes.empty(), "output has no enumerated mode"))
        return verdict();
      for (bool waitable : {false, true}) {
        step("seed=%u Direct3D=%u output=%u waitable=%u", seed, version, index, waitable);
        auto window = CreateWindowW(wc.lpszClassName, L"validation", 0, output_desc.DesktopCoordinates.left,
                                    output_desc.DesktopCoordinates.top, width / 2, height / 2, nullptr, nullptr,
                                    wc.hInstance, nullptr);
        if (!expect(window != nullptr, "create validation window"))
          return verdict();
        expect(factory->GetWindowAssociation(nullptr) == DXGI_ERROR_INVALID_CALL, "NULL association output");
        for (UINT flags = 0; flags <= DXGI_MWA_VALID; flags++) {
          expect(factory->MakeWindowAssociation(window, flags) == S_OK, "valid association flags %u", flags);
          HWND associated = window;
          expect(factory->GetWindowAssociation(&associated) == S_OK && !associated, "associated window %u", flags);
          expect(factory->MakeWindowAssociation(nullptr, flags) == S_OK, "NULL association %u", flags);
        }
        for (UINT bit = 0; bit < std::numeric_limits<UINT>::digits; bit++) {
          const UINT flags = UINT(1) << bit;
          if (flags & ~DXGI_MWA_VALID)
            expect(factory->MakeWindowAssociation(window, flags) == DXGI_ERROR_INVALID_CALL,
                   "invalid association flags %#x", flags);
        }
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferDesc = modes.front();
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.OutputWindow = window;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.Flags = waitable ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0;
        ComPtr<IDXGISwapChain> swapchain;
        CHECK(factory->CreateSwapChain(device.Get(), &desc, &swapchain));
        ComPtr<IDXGISwapChain3> swapchain3;
        CHECK(swapchain.As(&swapchain3));
        UINT latency = std::numeric_limits<UINT>::max();
        hr = swapchain3->GetMaximumFrameLatency(&latency);
        expect(hr == (waitable ? S_OK : DXGI_ERROR_INVALID_CALL) &&
                   latency == (waitable ? 1u : std::numeric_limits<UINT>::max()),
               "initial frame latency");
        UINT previous = latency;
        for (UINT requested = 0; requested <= DXGI_MAX_SWAP_CHAIN_BUFFERS + 1; requested++) {
          step("seed=%u Direct3D=%u output=%u waitable=%u latency=%u", seed, version, index, waitable, requested);
          const bool valid = waitable && requested && requested <= DXGI_MAX_SWAP_CHAIN_BUFFERS;
          expect(swapchain3->SetMaximumFrameLatency(requested) == (valid ? S_OK : DXGI_ERROR_INVALID_CALL),
                 "frame latency validation");
          if (valid)
            previous = requested;
          latency = std::numeric_limits<UINT>::max();
          expect(swapchain3->GetMaximumFrameLatency(&latency) == (waitable ? S_OK : DXGI_ERROR_INVALID_CALL) &&
                     latency == previous,
                 "frame latency after setter");
        }
        for (UINT value = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
             value <= DXGI_COLOR_SPACE_YCBCR_STUDIO_G24_TOPLEFT_P2020 + 1; value++) {
          step("seed=%u Direct3D=%u output=%u waitable=%u colour=%u", seed, version, index, waitable, value);
          const auto colour = DXGI_COLOR_SPACE_TYPE(value);
          UINT support = 0;
          expect(swapchain3->CheckColorSpaceSupport(colour, &support) == S_OK, "colour space support");
          if (colour == DXGI_COLOR_SPACE_RESERVED)
            expect(!support, "reserved colour space support");
          // CheckColorSpaceSupport allows setting unadvertised spaces; a successful set must then report support.
          HRESULT set = swapchain3->SetColorSpace1(colour);
          if (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)
            expect(set == S_OK, "advertised colour space refused");
          if (SUCCEEDED(set)) {
            expect(swapchain3->CheckColorSpaceSupport(colour, &support) == S_OK &&
                       (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT),
                   "set colour space not reported supported");
          }
        }
        expect(swapchain3->SetColorSpace1(DXGI_COLOR_SPACE_CUSTOM) == E_INVALIDARG, "custom colour space");
        swapchain3.Reset();
        swapchain.Reset();
        DestroyWindow(window);
        expect(factory->MakeWindowAssociation(window, 0) == DXGI_ERROR_INVALID_CALL, "destroyed association window");
      }
      for (bool mode_switch : {false, true})
        for (bool created_fullscreen : {false, true})
          for (bool explicit_target : {false, true})
            for (bool visible : {false, true}) {
              step("seed=%u Direct3D=%u output=%u switch=%u created=%u target=%u visible=%u", seed, version, index,
                   mode_switch, created_fullscreen, explicit_target, visible);
              CHECK(output->GetDesc(&output_desc));
              if (!expect(EnumDisplaySettingsW(output_desc.DeviceName, ENUM_CURRENT_SETTINGS, &desktop),
                          "desktop mode"))
                return verdict();
              LONG width = output_desc.DesktopCoordinates.right - output_desc.DesktopCoordinates.left;
              LONG height = output_desc.DesktopCoordinates.bottom - output_desc.DesktopCoordinates.top;
              // stock Wine's win32u asserts (make_ratio, dlls/win32u/sysparams.c) when the DPI times the physical size
              // over the mode's does not reduce below 65536 on an axis, and the process never ends: no such mode
              auto held = [dpi = UINT64(GetDpiForSystem()), &registry](const DXGI_MODE_DESC &mode) {
                auto reduces = [&](UINT64 physical, UINT64 size) {
                  return dpi * physical / std::gcd(dpi * physical, size) < 65536;
                };
                return reduces(registry.dmPelsWidth, mode.Width) && reduces(registry.dmPelsHeight, mode.Height);
              };
              auto available = modes;
              available.erase(std::remove_if(available.begin(), available.end(), [&](const auto &mode) {
                                return (mode.Width == desktop.dmPelsWidth && mode.Height == desktop.dmPelsHeight) ||
                                       !held(mode);
                              }),
                              available.end());
              if (!expect(!available.empty(), "no enumerated mode differs from the desktop"))
                return verdict();
              const auto alternate = available[random() % available.size()];
              step("seed=%u Direct3D=%u output=%u switch=%u created=%u target=%u visible=%u mode=%ux%u@%u/%u", seed,
                   version, index, mode_switch, created_fullscreen, explicit_target, visible, alternate.Width,
                   alternate.Height, alternate.RefreshRate.Numerator, alternate.RefreshRate.Denominator);
              auto window = CreateWindowExW(
                  WS_EX_APPWINDOW | WS_EX_TOOLWINDOW | WS_EX_CONTEXTHELP | WS_EX_DLGMODALFRAME |
                      (explicit_target ? WS_EX_TOPMOST : 0),
                  wc.lpszClassName, L"fullscreen", WS_OVERLAPPEDWINDOW | WS_POPUP | (visible ? WS_VISIBLE : 0),
                  output_desc.DesktopCoordinates.left + width / 4, output_desc.DesktopCoordinates.top + height / 4,
                  width / 2, height / 2, nullptr, nullptr, wc.hInstance, nullptr);
              if (!expect(window != nullptr, "create window"))
                return verdict();
              RECT saved{}, actual{}, client{};
              GetWindowRect(window, &saved);
              const LONG style = GetWindowLongW(window, GWL_STYLE), exstyle = GetWindowLongW(window, GWL_EXSTYLE);
              DXGI_SWAP_CHAIN_DESC desc{};
              desc.BufferDesc = alternate;
              desc.SampleDesc.Count = 1;
              desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
              desc.BufferCount = version == 12 ? 2 : 1;
              desc.OutputWindow = window;
              desc.Windowed = !created_fullscreen;
              desc.SwapEffect = version == 12 ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_DISCARD;
              desc.Flags = mode_switch ? DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH : 0;
              SetEnvironmentVariableA("MULLION_METALFX_SPATIAL_SWAPCHAIN", created_fullscreen ? "1" : "0");
              ComPtr<IDXGISwapChain> swapchain;
              hr = factory->CreateSwapChain(device.Get(), &desc, &swapchain);
              if (expect(hr == S_OK, "CreateSwapChain returned %#lx", hr)) {
                buffer_width = desc.BufferDesc.Width;
                buffer_height = desc.BufferDesc.Height;
                SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(swapchain.Get()));
                expect(swapchain->ResizeTarget(nullptr) == DXGI_ERROR_INVALID_CALL, "NULL resize target");
                expect(swapchain->SetFullscreenState(FALSE, output.Get()) == DXGI_ERROR_INVALID_CALL,
                       "FALSE with a target accepted");
                BOOL fullscreen = FALSE;
                ComPtr<IDXGIOutput> target;
                expect(swapchain->GetFullscreenState(&fullscreen, &target) == S_OK &&
                           !!fullscreen == created_fullscreen && !!target == created_fullscreen,
                       "created fullscreen state and target");
                DXGI_GAMMA_CONTROL_CAPABILITIES caps{};
                expect(output->GetGammaControlCapabilities(&caps) ==
                           (created_fullscreen ? S_OK : DXGI_ERROR_INVALID_CALL),
                       "created gamma ownership");
                expect(output->GetGammaControlCapabilities(nullptr) == DXGI_ERROR_INVALID_CALL,
                       "NULL gamma capabilities");
                hr = swapchain->SetFullscreenState(TRUE, explicit_target ? output.Get() : nullptr);
                if (expect(hr == S_OK, "enter fullscreen returned %#lx", hr)) {
                  expect(output->GetGammaControlCapabilities(&caps) == S_OK, "fullscreen gamma ownership");
                  target.Reset();
                  expect(swapchain->GetFullscreenState(&fullscreen, &target) == S_OK && fullscreen && target,
                         "fullscreen state and target");
                  DXGI_OUTPUT_DESC selected{};
                  if (target) {
                    expect(target->GetDesc(&selected) == S_OK && selected.Monitor == output_desc.Monitor,
                           "fullscreen selected a different monitor");
                    expect(!explicit_target || target.Get() == output.Get(), "explicit target identity");
                  }
                  expect(selected.DesktopCoordinates.right - selected.DesktopCoordinates.left ==
                             LONG(mode_switch ? alternate.Width : desktop.dmPelsWidth) &&
                             selected.DesktopCoordinates.bottom - selected.DesktopCoordinates.top ==
                             LONG(mode_switch ? alternate.Height : desktop.dmPelsHeight),
                         "fullscreen output mode");
                  expect(GetWindowRect(window, &actual) && EqualRect(&actual, &selected.DesktopCoordinates),
                         "fullscreen bounds");
                  expect(GetWindowLongW(window, GWL_STYLE) == (style & ~(WS_POPUP | WS_OVERLAPPEDWINDOW)),
                         "fullscreen style");
                  expect(GetWindowLongW(window, GWL_EXSTYLE) ==
                             ((exstyle &
                               ~(WS_EX_DLGMODALFRAME | WS_EX_TOOLWINDOW | WS_EX_OVERLAPPEDWINDOW | WS_EX_CONTEXTHELP)) |
                              WS_EX_TOPMOST),
                         "fullscreen extended style");
                  DEVMODEW current{};
                  current.dmSize = sizeof(current);
                  expect(EnumDisplaySettingsW(output_desc.DeviceName, ENUM_CURRENT_SETTINGS, &current) &&
                             current.dmPelsWidth == (mode_switch ? alternate.Width : desktop.dmPelsWidth) &&
                             current.dmPelsHeight == (mode_switch ? alternate.Height : desktop.dmPelsHeight) &&
                             current.dmDisplayFrequency ==
                                 (mode_switch ? alternate.RefreshRate.Numerator / alternate.RefreshRate.Denominator
                                              : desktop.dmDisplayFrequency),
                         "unexpected display mode change");
                  expect(swapchain->SetFullscreenState(TRUE, nullptr) == S_OK, "repeated fullscreen entry");
                  hr = swapchain->SetFullscreenState(FALSE, nullptr);
                  expect(hr == S_OK, "leave fullscreen returned %#lx", hr);
                  expect(output->GetGammaControlCapabilities(&caps) == DXGI_ERROR_INVALID_CALL,
                         "windowed gamma ownership");
                  expect(EnumDisplaySettingsW(output_desc.DeviceName, ENUM_CURRENT_SETTINGS, &current) &&
                             current.dmPelsWidth == registry.dmPelsWidth &&
                             current.dmPelsHeight == registry.dmPelsHeight &&
                             current.dmDisplayFrequency == registry.dmDisplayFrequency,
                         "restored display mode");
                  expect(GetWindowRect(window, &actual) && EqualRect(&actual, &saved), "restored window bounds");
                  expect(GetWindowLongW(window, GWL_STYLE) == style && GetWindowLongW(window, GWL_EXSTYLE) == exstyle,
                         "restored window styles");
                  desktop = current;
                  expect(output->GetDesc(&output_desc) == S_OK, "restored output description");
                  width = output_desc.DesktopCoordinates.right - output_desc.DesktopCoordinates.left;
                  height = output_desc.DesktopCoordinates.bottom - output_desc.DesktopCoordinates.top;
                  for (UINT change = 0; change < std::numeric_limits<UINT>::digits; change++) {
                    step("seed=%u Direct3D=%u output=%u switch=%u created=%u target=%u visible=%u resize=%u", seed,
                         version, index, mode_switch, created_fullscreen, explicit_target, visible, change);
                    DXGI_MODE_DESC mode{};
                    mode.Width = width / 4 + random() % (width / 4);
                    mode.Height = height / 4 + random() % (height / 4);
                    GetWindowRect(window, &saved);
                    RECT expected{0, 0, static_cast<LONG>(mode.Width), static_cast<LONG>(mode.Height)};
                    AdjustWindowRectEx(&expected, style, FALSE, exstyle);
                    OffsetRect(&expected, saved.left - expected.left, saved.top - expected.top);
                    const UINT before = nested_messages;
                    hr = swapchain->ResizeTarget(&mode);
                    expect(hr == S_OK, "windowed ResizeTarget");
                    expect(nested_messages > before, "ResizeTarget invoked no transition callback");
                    expect(swapchain->GetFullscreenState(&fullscreen, nullptr) == S_OK && !fullscreen,
                           "nested fullscreen entry changed windowed resize state");
                    expect(GetWindowRect(window, &actual) && EqualRect(&actual, &expected), "windowed resize bounds");
                    expect(GetClientRect(window, &client) && client.right == LONG(mode.Width) &&
                               client.bottom == LONG(mode.Height),
                           "windowed resize client size");
                    DXGI_SWAP_CHAIN_DESC got{};
                    expect(swapchain->GetDesc(&got) == S_OK && got.BufferDesc.Width == desc.BufferDesc.Width &&
                               got.BufferDesc.Height == desc.BufferDesc.Height,
                           "ResizeTarget changed back buffer size");
                  }
                  for (bool changed_style : {false, true})
                    for (bool moved : {false, true}) {
                      step("seed=%u Direct3D=%u output=%u switch=%u created=%u target=%u visible=%u styles=%u moved=%u",
                           seed, version, index, mode_switch, created_fullscreen, explicit_target, visible,
                           changed_style, moved);
                      GetWindowRect(window, &saved);
                      const LONG original_style = GetWindowLongW(window, GWL_STYLE);
                      const LONG original_exstyle = GetWindowLongW(window, GWL_EXSTYLE);
                      expect(swapchain->SetFullscreenState(TRUE, output.Get()) == S_OK, "reenter fullscreen");
                      test_nested = false;
                      if (changed_style) {
                        SetWindowLongW(window, GWL_STYLE,
                                       WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | (visible ? WS_VISIBLE : 0));
                        SetWindowLongW(window, GWL_EXSTYLE, 0);
                      }
                      SetWindowPos(window, HWND_NOTOPMOST, saved.left, saved.top, saved.right - saved.left + 1,
                                   saved.bottom - saved.top + 1,
                                   SWP_FRAMECHANGED | (moved ? 0 : SWP_NOMOVE | SWP_NOSIZE));
                      if (moved)
                        GetWindowRect(window, &saved);
                      const LONG saved_style =
                          changed_style || moved ? GetWindowLongW(window, GWL_STYLE) : original_style;
                      const LONG saved_exstyle =
                          changed_style || moved ? GetWindowLongW(window, GWL_EXSTYLE) : original_exstyle;
                      test_nested = true;
                      DXGI_MODE_DESC mode{desktop.dmPelsWidth, desktop.dmPelsHeight,
                                          {desktop.dmDisplayFrequency, 1}, desc.BufferDesc.Format};
                      hr = swapchain->ResizeTarget(&mode);
                      expect(hr == S_OK, "fullscreen ResizeTarget after application changes");
                      expect(GetWindowRect(window, &actual) && EqualRect(&actual, &output_desc.DesktopCoordinates),
                             "fullscreen ResizeTarget bounds");
                      expect(GetClientRect(window, &client) && client.right == width && client.bottom == height,
                             "fullscreen ResizeTarget decorations");
                      if (mode_switch) {
                        ComPtr<IDXGISwapChain1> swapchain1;
                        DXGI_SWAP_CHAIN_FULLSCREEN_DESC before{}, after{};
                        if (expect(swapchain.As(&swapchain1) == S_OK, "IDXGISwapChain1")) {
                          expect(swapchain1->GetFullscreenDesc(&before) == S_OK, "description before failed resize");
                          for (bool zero_width : {false, true}) {
                            mode = desc.BufferDesc;
                            if (zero_width)
                              mode.Width = 0;
                            else
                              mode.Height = 0;
                            mode.RefreshRate = {desktop.dmDisplayFrequency + 1, 1};
                            mode.Scaling = DXGI_MODE_SCALING_CENTERED;
                            mode.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE;
                            expect(swapchain->ResizeTarget(&mode) == DXGI_ERROR_INVALID_CALL, "invalid mode accepted");
                            expect(swapchain1->GetFullscreenDesc(&after) == S_OK, "description after failed resize");
                            expect(before.RefreshRate.Numerator == after.RefreshRate.Numerator &&
                                       before.RefreshRate.Denominator == after.RefreshRate.Denominator &&
                                       before.Scaling == after.Scaling &&
                                       before.ScanlineOrdering == after.ScanlineOrdering,
                                   "failed ResizeTarget changed description");
                          }
                        }
                      }
                      expect(swapchain->SetFullscreenState(FALSE, nullptr) == S_OK, "final leave fullscreen");
                      expect(GetWindowRect(window, &actual) && EqualRect(&actual, &saved),
                             "application's saved window bounds");
                      expect(GetWindowLongW(window, GWL_STYLE) == saved_style &&
                                 GetWindowLongW(window, GWL_EXSTYLE) == saved_exstyle,
                             "application's saved window styles");
                    }
                }
                swapchain->SetFullscreenState(FALSE, nullptr);
                destroy_on_style = true;
                expect(swapchain->SetFullscreenState(TRUE, output.Get()) == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE,
                       "failed window transition reported success");
                destroy_on_style = false;
                target.Reset();
                expect(swapchain->GetFullscreenState(&fullscreen, &target) == S_OK && !fullscreen && !target,
                       "failed window transition published fullscreen state");
                expect(output->GetGammaControlCapabilities(&caps) == DXGI_ERROR_INVALID_CALL,
                       "failed window transition published gamma ownership");
                SetWindowLongPtrW(window, GWLP_USERDATA, 0);
                if (IsWindow(window))
                  DestroyWindow(window);
                expect(swapchain->SetFullscreenState(TRUE, nullptr) == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE,
                       "destroyed window entered fullscreen");
                expect(swapchain->SetFullscreenState(FALSE, nullptr) == S_OK, "destroyed window left fullscreen");
              } else
                DestroyWindow(window);
            }
    }
  }
  UnregisterClassW(wc.lpszClassName, wc.hInstance);
  return verdict();
}
