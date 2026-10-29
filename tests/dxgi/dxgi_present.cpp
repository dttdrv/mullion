// contract: Present and Present1 validate synchronization and flags before
// changing the presentation sequence. "The sync interval passed in to Present
// (or Present1) must be 0." (DXGI_PRESENT_ALLOW_TEARING.)
// https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-present
// the same section requires ALLOW_TEARING at creation and windowed
// presentation; violations return INVALID_CALL. valid sync intervals are 0..4
// (IDXGISwapChain::Present, SyncInterval).
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-present
// GetLastPresentCount counts accepted non-TEST calls, including calls pending
// on a queue fence (Wine's dlls/dxgi/tests/dxgi.c,
// test_swapchain_present_count); failed calls and TEST leave count and buffer
// index alone.
#include "dxgi_test.hpp"
#include <array>
#include <cmath>
#include <limits>

int
main() {
  constexpr UINT maximum_sync_interval = 4;
  ComPtr<IDXGIFactory5> factory;
  CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
  BOOL tearing = FALSE;
  CHECK(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing)));
  HWND window = CreateWindowA(
      "static", "dxgi_present", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 128, 128, nullptr, nullptr,
      GetModuleHandleA(nullptr), nullptr
  );
  if (!expect(window != nullptr, "CreateWindow: %lu", GetLastError()))
    return verdict();
  UpdateWindow(window);
  for (UINT api : dxgi_apis()) {
    DxgiDevice device;
    CHECK(device.create(api));
    for (auto effect :
         {DXGI_SWAP_EFFECT_DISCARD, DXGI_SWAP_EFFECT_SEQUENTIAL, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL,
          DXGI_SWAP_EFFECT_FLIP_DISCARD}) {
      const bool flip = effect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL || effect == DXGI_SWAP_EFFECT_FLIP_DISCARD;
      if (api == 12 && !flip)
        continue;
      for (bool allow : {false, true}) {
        if (allow && !tearing)
          continue;
        step("D3D%u effect %u allow tearing %u: create", api, effect, allow);
        DXGI_SWAP_CHAIN_DESC1 desc{3,
                                   5,
                                   DXGI_FORMAT_R8G8B8A8_UNORM,
                                   FALSE,
                                   {1, 0},
                                   DXGI_USAGE_RENDER_TARGET_OUTPUT,
                                   2,
                                   DXGI_SCALING_STRETCH,
                                   effect};
        desc.Flags = allow ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        ComPtr<IDXGISwapChain1> first;
        HRESULT hr = factory->CreateSwapChainForHwnd(device.input.Get(), window, &desc, nullptr, nullptr, &first);
        if (allow && !flip) {
          expect(FAILED(hr) && !first, "tearing bitblt chain: %#lx", hr);
          continue;
        }
        if (!expect(hr == S_OK && first, "create: %#lx", hr))
          continue;
        ComPtr<IDXGISwapChain3> chain;
        CHECK(first.As(&chain));
        UINT initial = std::numeric_limits<UINT>::max();
        CHECK(chain->GetLastPresentCount(&initial));
        expect(!initial, "new chain present count %u", initial);
        auto present = [&](bool extended, UINT interval, UINT flags, HRESULT wanted) {
          step(
              "D3D%u effect %u allow %u Present%s interval %u flags %#x", api, effect, allow, extended ? "1" : "",
              interval, flags
          );
          UINT before = 0, after = 0;
          CHECK(chain->GetLastPresentCount(&before));
          UINT index = chain->GetCurrentBackBufferIndex();
          DXGI_PRESENT_PARAMETERS parameters{};
          HRESULT hr = extended ? chain->Present1(interval, flags, &parameters) : chain->Present(interval, flags);
          const bool busy = wanted == S_OK && (flags & DXGI_PRESENT_DO_NOT_WAIT) && hr == DXGI_ERROR_WAS_STILL_DRAWING;
          expect(hr == wanted || busy, "Present: %#lx, expected %#lx", hr, wanted);
          CHECK(chain->GetLastPresentCount(&after));
          const bool accepted = wanted == S_OK && !busy && !(flags & DXGI_PRESENT_TEST);
          expect(after == before + accepted, "present count %u -> %u, expected increment %u", before, after, accepted);
          if (api == 12)
            expect(
                chain->GetCurrentBackBufferIndex() == (index + accepted) % desc.BufferCount, "buffer index %u -> %u",
                index, chain->GetCurrentBackBufferIndex()
            );
          CHECK(device.idle());
          return 0;
        };
        for (bool extended : {false, true}) {
          for (UINT interval = 0; interval <= maximum_sync_interval + 1; interval++) {
            if (present(extended, interval, 0, interval <= maximum_sync_interval ? S_OK : DXGI_ERROR_INVALID_CALL))
              return 1;
            if (present(
                    extended, interval, DXGI_PRESENT_TEST,
                    interval <= maximum_sync_interval ? S_OK : DXGI_ERROR_INVALID_CALL
                ))
              return 1;
            if (present(
                    extended, interval, DXGI_PRESENT_ALLOW_TEARING, !interval && allow ? S_OK : DXGI_ERROR_INVALID_CALL
                ))
              return 1;
          }
          if (present(extended, std::numeric_limits<UINT>::max(), DXGI_PRESENT_TEST, DXGI_ERROR_INVALID_CALL))
            return 1;
          for (UINT flag :
               {UINT(DXGI_PRESENT_STEREO_PREFER_RIGHT), UINT(DXGI_PRESENT_STEREO_TEMPORARY_MONO),
                UINT(DXGI_PRESENT_RESTRICT_TO_OUTPUT)})
            if (present(extended, 0, flag, DXGI_ERROR_INVALID_CALL))
              return 1;
          if (present(extended, 0, DXGI_PRESENT_DO_NOT_WAIT, S_OK))
            return 1;
          if (flip)
            if (present(extended, 0, DXGI_PRESENT_RESTART, S_OK))
              return 1;
          if (allow)
            for (UINT flags :
                 {UINT(DXGI_PRESENT_ALLOW_TEARING | DXGI_PRESENT_DO_NOT_WAIT),
                  UINT(DXGI_PRESENT_ALLOW_TEARING | DXGI_PRESENT_RESTART),
                  UINT(DXGI_PRESENT_ALLOW_TEARING | DXGI_PRESENT_RESTART | DXGI_PRESENT_DO_NOT_WAIT)})
              if (present(extended, 0, flags, S_OK))
                return 1;
        }
        step("D3D%u effect %u HWND matrix transform refusal", api, effect);
        // SetMatrixTransform/GetMatrixTransform are restricted to composition
        // swap chains.
        // https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-setmatrixtransform
        DXGI_MATRIX_3X2_F matrix{1, 0, 0, 1, 0, 0};
        expect(chain->SetMatrixTransform(&matrix) == DXGI_ERROR_INVALID_CALL, "HWND matrix set accepted");
        expect(chain->GetMatrixTransform(&matrix) == DXGI_ERROR_INVALID_CALL, "HWND matrix get accepted");
        step("D3D%u effect %u source-size bounds and retained state", api, effect);
        // SetSourceSize: both axes must be positive and no greater than their
        // corresponding back-buffer axis.
        // https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-setsourcesize
        UINT source_width = 0, source_height = 0;
        CHECK(chain->GetSourceSize(&source_width, &source_height));
        expect(source_width == desc.Width && source_height == desc.Height, "initial source size");
        for (UINT w = 1; w <= desc.Width; w++)
          for (UINT h = 1; h <= desc.Height; h++) {
            CHECK(chain->SetSourceSize(w, h));
            CHECK(chain->GetSourceSize(&source_width, &source_height));
            expect(source_width == w && source_height == h, "source-size roundtrip");
          }
        for (UINT w : {0u, desc.Width, desc.Width + 1, std::numeric_limits<UINT>::max()})
          for (UINT h : {0u, desc.Height, desc.Height + 1, std::numeric_limits<UINT>::max()}) {
            bool valid = w && h && w <= desc.Width && h <= desc.Height;
            expect(chain->SetSourceSize(w, h) == (valid ? S_OK : E_INVALIDARG), "source-size validation %u,%u", w, h);
            CHECK(chain->GetSourceSize(&source_width, &source_height));
            expect(source_width == desc.Width && source_height == desc.Height, "refused source size changed state");
          }
        // SetRotation requires a flip chain. all four defined rotations must
        // roundtrip before presenting.
        // https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgiswapchain1-setrotation
        for (auto rotation :
             {DXGI_MODE_ROTATION_IDENTITY, DXGI_MODE_ROTATION_ROTATE90, DXGI_MODE_ROTATION_ROTATE180,
              DXGI_MODE_ROTATION_ROTATE270}) {
          expect(chain->SetRotation(rotation) == (flip ? S_OK : DXGI_ERROR_INVALID_CALL), "rotation setter");
          DXGI_MODE_ROTATION got = DXGI_MODE_ROTATION_UNSPECIFIED;
          CHECK(chain->GetRotation(&got));
          expect(got == (flip ? rotation : DXGI_MODE_ROTATION_IDENTITY), "rotation state");
          if (present(true, 0, 0, S_OK))
            return 1;
        }
        if (flip)
          CHECK(chain->SetRotation(DXGI_MODE_ROTATION_IDENTITY));
        step("D3D%u effect %u each background channel and floating-point edge", api, effect);
        // SetBackgroundColor rejects members "outside the range from 0.0
        // through 1.0".
        // https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgiswapchain1-setbackgroundcolor
        const DXGI_RGBA base{0.25f, 0.5f, 0.75f, 1};
        for (UINT channel = 0; channel < 4; channel++)
          for (float value :
               {0.f, -0.f, std::numeric_limits<float>::denorm_min(), 0.5f, 1.f,
                -std::numeric_limits<float>::denorm_min(), std::nextafter(1.f, 2.f),
                std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                std::numeric_limits<float>::quiet_NaN()}) {
            CHECK(chain->SetBackgroundColor(&base));
            std::array<float, 4> channels{base.r, base.g, base.b, base.a};
            channels[channel] = value;
            DXGI_RGBA requested{channels[0], channels[1], channels[2], channels[3]}, got{};
            bool valid = value >= 0 && value <= 1;
            expect(chain->SetBackgroundColor(&requested) == (valid ? S_OK : E_INVALIDARG), "background validation");
            CHECK(chain->GetBackgroundColor(&got));
            const auto &wanted = valid ? requested : base;
            expect(
                got.r == wanted.r && got.g == wanted.g && got.b == wanted.b && got.a == wanted.a,
                "background channel or failed-set state changed"
            );
          }
        expect(
            chain->SetBackgroundColor(nullptr) == E_INVALIDARG && chain->GetBackgroundColor(nullptr) == E_INVALIDARG,
            "NULL background"
        );
        if (allow) {
          step("D3D%u fullscreen tearing refusal", api);
          CHECK(chain->SetFullscreenState(TRUE, nullptr));
          CHECK(chain->ResizeBuffers(0, desc.Width, desc.Height, DXGI_FORMAT_UNKNOWN, desc.Flags));
          for (bool extended : {false, true})
            if (present(extended, 0, DXGI_PRESENT_ALLOW_TEARING, DXGI_ERROR_INVALID_CALL))
              return 1;
          CHECK(chain->SetFullscreenState(FALSE, nullptr));
          CHECK(chain->ResizeBuffers(0, desc.Width, desc.Height, DXGI_FORMAT_UNKNOWN, desc.Flags));
        }
        step("D3D%u effect %u frame statistics", api, effect);
        DXGI_FRAME_STATISTICS stats{};
        hr = chain->GetFrameStatistics(&stats);
        if (!flip)
          expect(
              FAILED(hr) || (hr == S_OK && !stats.PresentCount && !stats.PresentRefreshCount &&
                             !stats.SyncRefreshCount && !stats.SyncQPCTime.QuadPart && !stats.SyncGPUTime.QuadPart),
              "windowed bitblt statistics: %#lx", hr
          );
        else {
          ComPtr<IDXGIOutput> output;
          CHECK(chain->GetContainingOutput(&output));
          if (hr == DXGI_ERROR_FRAME_STATISTICS_DISJOINT) {
            CHECK(chain->Present(1, 0));
            CHECK(device.idle());
            CHECK(output->WaitForVBlank());
            hr = chain->GetFrameStatistics(&stats);
          }
          expect(hr == S_OK || hr == DXGI_ERROR_FRAME_STATISTICS_DISJOINT, "flip statistics: %#lx", hr);
          if (hr == DXGI_ERROR_FRAME_STATISTICS_DISJOINT)
            printf("frame statistics remain disjoint; timing fields were not checked\n");
          if (hr == S_OK) {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            expect(
                stats.SyncQPCTime.QuadPart > 0 && stats.SyncQPCTime.QuadPart <= now.QuadPart,
                "statistics have no valid vblank QPC"
            );
            expect(
                !stats.SyncGPUTime.QuadPart && stats.PresentRefreshCount <= stats.SyncRefreshCount,
                "statistics have a nonzero reserved GPU time or contradict the refresh sequence"
            );
          }
        }
        CHECK(device.idle());
      }
    }
  }
  DestroyWindow(window);
  return verdict();
}
