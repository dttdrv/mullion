// contract: swap-chain flags compose without losing their restrictions or the
// back buffer's requested uses. "This flag requires the use of a
// DXGI_SWAP_EFFECT_FLIP_* swap effect." (ALLOW_TEARING.) "This flag is not
// applicable for Direct3D 12." (GDI_COMPATIBLE.)
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi/ne-dxgi-dxgi_swap_chain_flag
// waitable bitblt rejection follows Windows observations in Wine's
// test_frame_latency_event. all combinations of ALLOW_MODE_SWITCH,
// FRAME_LATENCY_WAITABLE_OBJECT and ALLOW_TEARING reach both creation entry
// points and both optional shader-input/unordered-access usage. GDI writes are
// presented and read from the window.
// GetDC requires GDI compatibility: "otherwise this method fails." its formats are BGRA8 UNORM and SRGB,
// and GDI_COMPATIBLE textures "cannot use ... multisampling" (D3D11_RESOURCE_MISC_FLAG).
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgisurface1-getdc
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_resource_misc_flag
#include "dxgi_test.hpp"

int
main() {
  SetEnvironmentVariableA("MULLION_CONFIG", "dxgi.presentThroughGDI=True");
  constexpr UINT side = 8;
  ComPtr<IDXGIFactory5> factory;
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  BOOL tearing = FALSE;
  CHECK(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing)));
  HWND window = CreateWindowA(
      "static", "dxgi_flags", WS_POPUP | WS_VISIBLE, 0, 0, side, side, nullptr, nullptr, GetModuleHandleA(nullptr),
      nullptr
  );
  if (!expect(window != nullptr, "CreateWindow"))
    return verdict();
  UpdateWindow(window);
  for (UINT api : dxgi_apis()) {
    DxgiDevice device;
    CHECK(device.create(api));
    for (auto effect :
         {DXGI_SWAP_EFFECT_DISCARD, DXGI_SWAP_EFFECT_SEQUENTIAL, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL,
          DXGI_SWAP_EFFECT_FLIP_DISCARD})
      for (bool legacy : {false, true})
        for (bool mode_switch : {false, true})
          for (bool waitable : {false, true})
            for (bool allow_tearing : {false, true})
              for (UINT usage :
                   {UINT(DXGI_USAGE_RENDER_TARGET_OUTPUT),
                    UINT(DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT),
                    UINT(DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_UNORDERED_ACCESS),
                    UINT(DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT | DXGI_USAGE_UNORDERED_ACCESS)}) {
                if (device.d10 && (usage & DXGI_USAGE_UNORDERED_ACCESS))
                  continue;
                UINT flags = (mode_switch ? DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH : 0) |
                             (waitable ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0) |
                             (allow_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
                step("D3D%u effect %u legacy %u flags %#x usage %#x", api, effect, legacy, flags, usage);
                const bool flip = effect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL || effect == DXGI_SWAP_EFFECT_FLIP_DISCARD;
                const bool valid =
                    (api != 12 || flip) && (!(waitable || allow_tearing) || flip) && (!allow_tearing || tearing);
                DXGI_SWAP_CHAIN_DESC1 desc{side,  side, DXGI_FORMAT_R8G8B8A8_UNORM, FALSE,  {1, 0},
                                           usage, 2,    DXGI_SCALING_STRETCH,       effect, DXGI_ALPHA_MODE_UNSPECIFIED,
                                           flags};
                ComPtr<IDXGISwapChain> made;
                HRESULT hr;
                if (legacy) {
                  DXGI_SWAP_CHAIN_DESC old{{side, side, {}, desc.Format},
                                           desc.SampleDesc,
                                           usage,
                                           desc.BufferCount,
                                           window,
                                           TRUE,
                                           effect,
                                           flags};
                  hr = factory->CreateSwapChain(device.input.Get(), &old, &made);
                } else {
                  ComPtr<IDXGISwapChain1> first;
                  hr = factory->CreateSwapChainForHwnd(device.input.Get(), window, &desc, nullptr, nullptr, &first);
                  if (first)
                    CHECK(first.As(&made));
                }
                expect(valid ? hr == S_OK : FAILED(hr), "creation flags: %#lx", hr);
                expect(bool(made) == valid, "creation output disagrees with flags");
                if (!valid || !made)
                  continue;
                ComPtr<IDXGISwapChain3> chain;
                CHECK(made.As(&chain));
                DXGI_SWAP_CHAIN_DESC1 got{};
                CHECK(chain->GetDesc1(&got));
                expect(
                    got.Flags == flags && got.BufferUsage == usage && got.SwapEffect == effect,
                    "flags or usage changed in descriptor"
                );
                if (api == 12) {
                  ComPtr<ID3D12Resource> back;
                  CHECK(chain->GetBuffer(0, IID_PPV_ARGS(&back)));
                  auto resource = back->GetDesc();
                  expect(
                      (resource.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) &&
                          bool(resource.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ==
                              bool(usage & DXGI_USAGE_UNORDERED_ACCESS),
                      "D3D12 resource usage flags"
                  );
                } else {
                  UINT binds = D3D11_BIND_RENDER_TARGET |
                               (usage & DXGI_USAGE_SHADER_INPUT ? D3D11_BIND_SHADER_RESOURCE : 0) |
                               (usage & DXGI_USAGE_UNORDERED_ACCESS ? D3D11_BIND_UNORDERED_ACCESS : 0);
                  if (device.d11) {
                    ComPtr<ID3D11Texture2D> back;
                    CHECK(chain->GetBuffer(0, IID_PPV_ARGS(&back)));
                    D3D11_TEXTURE2D_DESC resource{};
                    back->GetDesc(&resource);
                    expect(resource.BindFlags == binds, "D3D11 back-buffer bind flags");
                  } else {
                    ComPtr<ID3D10Texture2D> back;
                    CHECK(chain->GetBuffer(0, IID_PPV_ARGS(&back)));
                    D3D10_TEXTURE2D_DESC resource{};
                    back->GetDesc(&resource);
                    expect(
                        (resource.BindFlags & (D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE)) ==
                            (binds & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE)),
                        "D3D10 buffer bind flags"
                    );
                  }
                }
                HANDLE handle = chain->GetFrameLatencyWaitableObject();
                expect(bool(handle) == waitable, "readiness flag not implemented");
                if (handle) {
                  expect(WaitForSingleObject(handle, 0) == WAIT_OBJECT_0, "no initial readiness");
                  CloseHandle(handle);
                }
                CHECK(chain->Present(0, allow_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0));
                CHECK(device.idle());
                CHECK(chain->ResizeBuffers(0, side, side, DXGI_FORMAT_UNKNOWN, flags));
                expect(
                    chain->ResizeBuffers(
                        0, side, side, DXGI_FORMAT_UNKNOWN, flags ^ DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
                    ) == DXGI_ERROR_INVALID_CALL,
                    "resize changed lifetime tearing flag"
                );
                CHECK(chain->GetDesc1(&got));
                expect(got.Flags == flags, "failed flag resize changed descriptor");
                if (flip) {
                  // the waitable flag also cannot be added or removed with ResizeBuffers (DXGI 1.3, Step 1).
                  // https://learn.microsoft.com/en-us/windows/uwp/gaming/reduce-latency-with-dxgi-1-3-swap-chains
                  expect(
                      FAILED(chain->ResizeBuffers(
                          0, side, side, DXGI_FORMAT_UNKNOWN, flags ^ DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT
                      )),
                      "resize changed lifetime waitable flag"
                  );
                  CHECK(chain->GetDesc1(&got));
                  expect(got.Flags == flags, "failed waitable resize changed descriptor");
                }
              }
    step("D3D%u stereo creation agrees with advertised windowed support", api);
    // FALSE does not prohibit stereo creation; DXGI may reduce it to mono on this output.
    // https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-iswindowedstereoenabled
    DXGI_SWAP_CHAIN_DESC1 stereo_desc{
        side,
        side,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        TRUE,
        {1, 0},
        DXGI_USAGE_RENDER_TARGET_OUTPUT,
        2,
        DXGI_SCALING_STRETCH,
        DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL
    };
    {
      ComPtr<IDXGISwapChain1> stereo;
      HRESULT hr = factory->CreateSwapChainForHwnd(device.input.Get(), window, &stereo_desc, nullptr, nullptr, &stereo);
      if (factory->IsWindowedStereoEnabled())
        expect(hr == S_OK && stereo, "advertised stereo creation: %#lx", hr);
      expect(SUCCEEDED(hr) ? bool(stereo) : !stereo, "stereo creation output");
      if (stereo) {
        DXGI_SWAP_CHAIN_DESC1 got{};
        CHECK(stereo->GetDesc1(&got));
        expect(got.Stereo, "stereo descriptor was lost");
        if (!stereo->IsTemporaryMonoSupported()) {
          // DXGI_PRESENT: unsupported temporary mono returns INVALID_CALL without displaying the frame.
          UINT before = 0, after = 0;
          CHECK(stereo->GetLastPresentCount(&before));
          DXGI_PRESENT_PARAMETERS parameters{};
          expect(
              stereo->Present(0, DXGI_PRESENT_STEREO_TEMPORARY_MONO) == DXGI_ERROR_INVALID_CALL,
              "unsupported temporary mono Present accepted"
          );
          expect(
              stereo->Present1(0, DXGI_PRESENT_STEREO_TEMPORARY_MONO, &parameters) == DXGI_ERROR_INVALID_CALL,
              "unsupported temporary mono Present1 accepted"
          );
          CHECK(stereo->GetLastPresentCount(&after));
          expect(after == before, "unsupported temporary mono changed present count");
        }
        if (device.d12) {
          ComPtr<ID3D12Resource> back;
          CHECK(stereo->GetBuffer(0, IID_PPV_ARGS(&back)));
          expect(back->GetDesc().DepthOrArraySize == 2, "stereo buffer does not have two eyes");
        } else if (device.d11) {
          ComPtr<ID3D11Texture2D> back;
          CHECK(stereo->GetBuffer(0, IID_PPV_ARGS(&back)));
          D3D11_TEXTURE2D_DESC resource{};
          back->GetDesc(&resource);
          expect(resource.ArraySize == 2, "stereo buffer does not have two eyes");
        } else {
          ComPtr<ID3D10Texture2D> back;
          CHECK(stereo->GetBuffer(0, IID_PPV_ARGS(&back)));
          D3D10_TEXTURE2D_DESC resource{};
          back->GetDesc(&resource);
          expect(resource.ArraySize == 2, "stereo buffer does not have two eyes");
        }
      }
    }
    // a flip Present permanently excludes native GDI on its HWND, even after
    // that swap chain is released.
    DestroyWindow(window);
    window = CreateWindowA(
        "static", "dxgi_flags_gdi", WS_POPUP | WS_VISIBLE, 0, 0, side, side, nullptr, nullptr,
        GetModuleHandleA(nullptr), nullptr
    );
    if (!expect(window != nullptr, "CreateWindow GDI"))
      return verdict();
    UpdateWindow(window);
    step("D3D%u GDI-compatible back buffer", api);
    DXGI_SWAP_CHAIN_DESC1 desc{
        side,
        side,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        FALSE,
        {1, 0},
        DXGI_USAGE_RENDER_TARGET_OUTPUT,
        2,
        DXGI_SCALING_STRETCH,
        DXGI_SWAP_EFFECT_SEQUENTIAL
    };
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE;
    if (api == 12)
      desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    ComPtr<IDXGISwapChain1> chain;
    HRESULT hr = factory->CreateSwapChainForHwnd(device.input.Get(), window, &desc, nullptr, nullptr, &chain);
    if (api == 12) {
      expect(SUCCEEDED(hr) ? bool(chain) : !chain, "D3D12 GDI creation output: %#lx", hr);
      if (chain) {
        ComPtr<IDXGISurface1> surface;
        HRESULT queried = chain->GetBuffer(0, IID_PPV_ARGS(&surface));
        if (SUCCEEDED(queried) && surface) {
          HDC dc = nullptr;
          expect(FAILED(surface->GetDC(FALSE, &dc)) && !dc, "D3D12 exposed a usable GDI back buffer");
        } else
          expect(queried == E_NOINTERFACE && !surface, "D3D12 surface query: %#lx", queried);
      }
      continue;
    }
    if (!expect(hr == S_OK && chain, "GDI chain: %#lx", hr))
      continue;
    for (auto format : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM}) {
      auto invalid = desc;
      invalid.Format = format;
      invalid.SampleDesc.Count = format == DXGI_FORMAT_B8G8R8A8_UNORM ? 2 : 1;
      ComPtr<IDXGISwapChain1> refused;
      HRESULT made =
          factory->CreateSwapChainForHwnd(device.input.Get(), window, &invalid, nullptr, nullptr, &refused);
      expect(FAILED(made) && !refused, "invalid GDI format/multisampling accepted: %#lx", made);
    }
    for (auto format : {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB})
      for (UINT flags :
           {0u, UINT(DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE), 0u, UINT(DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE)}) {
        step("D3D%u GDI format %u resized flags %#x", api, format, flags);
        CHECK(chain->ResizeBuffers(0, side, side, format, flags));
        ComPtr<IDXGISurface1> surface;
        CHECK(chain->GetBuffer(0, IID_PPV_ARGS(&surface)));
        HDC dc = nullptr;
        hr = surface->GetDC(FALSE, &dc);
        if (!flags) {
          expect(hr == DXGI_ERROR_INVALID_CALL && !dc, "unflagged GDI GetDC: %#lx", hr);
          continue;
        }
        if (expect(hr == S_OK && dc, "GDI GetDC: %#lx", hr)) {
          const COLORREF color = RGB(0, 255, 0);
          RECT rect{0, 0, side, side};
          HBRUSH brush = CreateSolidBrush(color);
          expect(brush && FillRect(dc, &rect, brush), "GDI paint");
          if (brush)
            DeleteObject(brush);
          CHECK(surface->ReleaseDC(nullptr));
          CHECK(chain->Present(0, 0));
          CHECK(device.idle());
          HDC shown = GetDC(window);
          expect(shown && GetPixel(shown, side / 2, side / 2) == color, "GDI pixels were not presented");
          if (shown)
            ReleaseDC(window, shown);
        }
      }
  }
  DestroyWindow(window);
  return verdict();
}
