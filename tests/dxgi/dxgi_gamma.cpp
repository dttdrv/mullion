// contract: gamma control belongs to the swap chain's active fullscreen output.
// "Calling this method is only supported while in full-screen mode."
// (IDXGIOutput::SetGammaControl, Remarks.)
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgioutput-setgammacontrol
// GetGammaControl and GetGammaControlCapabilities have the same restriction.
// the curve's input positions and output range come from
// GetGammaControlCapabilities; Scale and Offset are varied only when
// advertised.
// https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/using-gamma-correction
#include "dxgi_test.hpp"
#include <cmath>

int
main() {
  ComPtr<IDXGIFactory1> factory;
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  for (UINT api : dxgi_apis()) {
    DxgiDevice device;
    CHECK(device.create(api));
    ComPtr<IDXGIAdapter1> adapter;
    CHECK(factory->EnumAdapters1(0, &adapter));
    for (UINT index = 0;; index++) {
      step("D3D%u gamma output %u", api, index);
      ComPtr<IDXGIOutput> output, active, twin;
      HRESULT hr = adapter->EnumOutputs(index, &output);
      if (hr == DXGI_ERROR_NOT_FOUND)
        break;
      CHECK(hr);
      DXGI_OUTPUT_DESC desc{};
      CHECK(output->GetDesc(&desc));
      HWND window = CreateWindowA(
          "static", "dxgi_gamma", WS_OVERLAPPEDWINDOW | WS_VISIBLE, desc.DesktopCoordinates.left,
          desc.DesktopCoordinates.top, 128, 128, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr
      );
      if (!expect(window != nullptr, "CreateWindow: %lu", GetLastError()))
        return verdict();
      UpdateWindow(window);
      DXGI_SWAP_CHAIN_DESC swap{
          {3, 5, {}, DXGI_FORMAT_R8G8B8A8_UNORM}, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT, 2, window, TRUE,
          DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL
      };
      ComPtr<IDXGISwapChain> chain;
      hr = factory->CreateSwapChain(device.input.Get(), &swap, &chain);
      if (!expect(hr == S_OK && chain, "create gamma chain: %#lx", hr)) {
        DestroyWindow(window);
        continue;
      }
      DXGI_GAMMA_CONTROL_CAPABILITIES caps{};
      DXGI_GAMMA_CONTROL original{}, set{}, got{};
      set.Scale = {1, 1, 1};
      for (UINT point = 0; point < ARRAYSIZE(set.GammaCurve); point++) {
        float value = float(point) / (ARRAYSIZE(set.GammaCurve) - 1);
        set.GammaCurve[point] = {value, value, value};
      }
      expect(FAILED(output->GetGammaControlCapabilities(&caps)), "windowed gamma capabilities accepted");
      HRESULT windowed_get = output->GetGammaControl(&got);
      expect(FAILED(windowed_get), "windowed gamma getter accepted");
      if (SUCCEEDED(windowed_get))
        set = got;
      expect(FAILED(output->SetGammaControl(&set)), "windowed gamma setter accepted");
      hr = chain->SetFullscreenState(TRUE, output.Get());
      if (!expect(hr == S_OK, "enter gamma fullscreen: %#lx", hr)) {
        chain->SetFullscreenState(FALSE, nullptr);
        chain.Reset();
        DestroyWindow(window);
        continue;
      }
      // DXGI best practices, "Gamma and DXGI": fullscreen creates a new active
      // output object.
      bool contained = expect(chain->GetContainingOutput(&active) == S_OK && active, "active fullscreen output") &&
                       expect(chain->GetContainingOutput(&twin) == S_OK && twin, "second active output query");
      HRESULT capabilities = contained ? active->GetGammaControlCapabilities(&caps) : E_FAIL;
      if (capabilities == DXGI_ERROR_UNSUPPORTED) {
        printf("unsupported: gamma on D3D%u output %u\n", api, index);
        expect(
            FAILED(active->SetGammaControl(&set)) && FAILED(active->GetGammaControl(&got)),
            "unsupported gamma control accepted"
        );
        expect(chain->SetFullscreenState(FALSE, nullptr) == S_OK, "leave unsupported gamma fullscreen");
        chain.Reset();
        DestroyWindow(window);
        continue;
      }
      bool usable =
          contained && expect(capabilities == S_OK, "fullscreen capabilities: %#lx", capabilities) &&
          expect(
              caps.NumGammaControlPoints > 0 && caps.NumGammaControlPoints <= ARRAYSIZE(caps.ControlPointPositions) &&
                  caps.NumGammaControlPoints <= ARRAYSIZE(set.GammaCurve),
              "invalid gamma point count"
          );
      bool saved = contained && expect(active->GetGammaControl(&original) == S_OK, "save original gamma");
      usable = usable && expect(
                             std::isfinite(caps.MinConvertedValue) && std::isfinite(caps.MaxConvertedValue) &&
                                 caps.MinConvertedValue <= caps.MaxConvertedValue,
                             "invalid converted range"
                         );
      if (usable && saved) {
        if (caps.ScaleAndOffsetSupported) {
          set.Scale = {0.5f, 0.75f, 1};
          set.Offset = {0.25f, 0, 0};
        }
        for (UINT i = 0; i < caps.NumGammaControlPoints; i++) {
          float at = caps.ControlPointPositions[i];
          expect(std::isfinite(at) && (!i || at > caps.ControlPointPositions[i - 1]), "invalid control point %u", i);
          float u = caps.NumGammaControlPoints > 1 ? float(i) / (caps.NumGammaControlPoints - 1) : 0;
          set.GammaCurve[i] = {
              std::lerp(caps.MinConvertedValue, caps.MaxConvertedValue, u),
              std::lerp(caps.MinConvertedValue, caps.MaxConvertedValue, 1 - u),
              std::lerp(caps.MinConvertedValue, caps.MaxConvertedValue, u * u / 2)
          };
        }
        expect(active->SetGammaControl(&set) == S_OK, "set gamma curve");
        expect(twin->GetGammaControl(&got) == S_OK, "read gamma through another object of this monitor");
        expect(
            !memcmp(&set.Scale, &got.Scale, sizeof(set.Scale)) && !memcmp(&set.Offset, &got.Offset, sizeof(set.Offset)),
            "gamma scale/offset roundtrip"
        );
        for (UINT i = 0; i < caps.NumGammaControlPoints; i++)
          expect(!memcmp(&set.GammaCurve[i], &got.GammaCurve[i], sizeof(DXGI_RGB)), "gamma curve point %u", i);
        expect(
            active->SetGammaControl(nullptr) == DXGI_ERROR_INVALID_CALL &&
                active->GetGammaControl(nullptr) == DXGI_ERROR_INVALID_CALL &&
                active->GetGammaControlCapabilities(nullptr) == DXGI_ERROR_INVALID_CALL,
            "NULL gamma arguments"
        );
      }
      if (saved)
        expect(active->SetGammaControl(&original) == S_OK, "restore original gamma before exit");
      expect(chain->SetFullscreenState(FALSE, nullptr) == S_OK, "leave gamma fullscreen");
      if (active) {
        expect(FAILED(active->GetGammaControl(&got)), "gamma remains accessible after fullscreen exit");
        expect(FAILED(active->GetGammaControlCapabilities(&caps)), "gamma capabilities remain accessible after exit");
        expect(FAILED(active->SetGammaControl(&set)), "gamma remains writable after fullscreen exit");
      }
      CHECK(device.idle());
      chain.Reset();
      DestroyWindow(window);
    }
  }
  return verdict();
}
