// contract: D3D10 and 10.1 expose the same indexed back-buffer and resize-reference rules as D3D11.
// GetBuffer: "If the swap chain's swap effect is DXGI_SWAP_EFFECT_DISCARD, this method can only access the first
// buffer". https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-getbuffer D3D11.3 6.3.9.2:
// "Present rotates the identities of the back buffers"; sequential buffers are read-only except index zero.
// ResizeBuffers requires releasing direct and indirect references, including views and bindings.
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-resizebuffers
// D3D11.3 5.2.3.1 clears convert to the fully qualified view format; 3.2.3.8 supplies FLOAT->SRGB and 0.6 ULP.
// Wine's Windows test_swapchain_views allows D3D10 to refuse UNORM-backbuffer/sRGB-view reinterpretation.
// an accepted view must still perform the conversion; refusal is printed as an unchecked view path.
#include "dxgi_test.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

int
main() {
  ComPtr<IDXGIFactory2> factory;
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  HWND window = CreateWindowA(
      "static", "dxgi_buffers", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 64, 64, nullptr, nullptr,
      GetModuleHandleA(nullptr), nullptr
  );
  if (!expect(window != nullptr, "CreateWindow"))
    return verdict();
  UpdateWindow(window);
  constexpr UINT width = 3, height = 5;
  constexpr BYTE maximum = std::numeric_limits<BYTE>::max();
  for (UINT api : {10u, 101u}) {
    DxgiDevice device;
    CHECK(device.create(api));
    D3D10_TEXTURE2D_DESC staging_desc{
        width, height, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D10_USAGE_STAGING, 0, D3D10_CPU_ACCESS_READ
    };
    ComPtr<ID3D10Texture2D> staging;
    CHECK(device.d10->CreateTexture2D(&staging_desc, nullptr, &staging));
    auto bytes = [&](ID3D10Texture2D *back, double expected) -> HRESULT {
      device.d10->CopyResource(staging.Get(), back);
      D3D10_MAPPED_TEXTURE2D mapped{};
      HRESULT hr = staging->Map(0, D3D10_MAP_READ, 0, &mapped);
      if (FAILED(hr))
        return hr;
      constexpr UINT channels = 4;
      constexpr double unorm_tolerance = 0.6;
      for (UINT y = 0; y < height; y++)
        for (UINT x = 0; x < width; x++) {
          const BYTE *got = static_cast<BYTE *>(mapped.pData) + y * mapped.RowPitch + x * channels;
          expect(
              std::abs(got[0] - expected) <= unorm_tolerance && !got[1] && !got[2] && got[3] == maximum,
              "D3D%u pixel %u,%u = %u,%u,%u,%u, expected red %g", api, x, y, got[0], got[1], got[2], got[3], expected
          );
        }
      staging->Unmap(0);
      return S_OK;
    };
    for (bool srgb : {false, true}) {
      step("D3D%u back-buffer %s RTV conversion", api, srgb ? "sRGB" : "UNORM");
      DXGI_SWAP_CHAIN_DESC1 desc{
          width,
          height,
          staging_desc.Format,
          FALSE,
          {1, 0},
          DXGI_USAGE_RENDER_TARGET_OUTPUT,
          2,
          DXGI_SCALING_STRETCH,
          DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL
      };
      ComPtr<IDXGISwapChain1> chain;
      CHECK(factory->CreateSwapChainForHwnd(device.input.Get(), window, &desc, nullptr, nullptr, &chain));
      ComPtr<ID3D10Texture2D> back;
      CHECK(chain->GetBuffer(0, IID_PPV_ARGS(&back)));
      D3D10_RENDER_TARGET_VIEW_DESC view_desc{
          srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : desc.Format, D3D10_RTV_DIMENSION_TEXTURE2D
      };
      ComPtr<ID3D10RenderTargetView> view;
      HRESULT hr = device.d10->CreateRenderTargetView(back.Get(), &view_desc, &view);
      if (srgb && hr == E_INVALIDARG) {
        printf("D3D%u typed back-buffer sRGB view refused; conversion path was not checked\n", api);
        continue;
      }
      CHECK(hr);
      if (!expect(view != nullptr, "accepted RTV has no interface"))
        return verdict();
      std::vector<float> values;
      for (UINT code = 0; code <= maximum; code++)
        values.push_back(float(code) / maximum);
      for (float value :
           {-0.f, std::numeric_limits<float>::denorm_min(), -std::numeric_limits<float>::denorm_min(),
            std::numeric_limits<float>::lowest(), std::numeric_limits<float>::max(),
            std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
            std::numeric_limits<float>::quiet_NaN()})
        values.push_back(value);
      for (float value : values) {
        const float color[] = {value, 0, 0, 1};
        device.d10->ClearRenderTargetView(view.Get(), color);
        double expected = std::isnan(value) ? 0 : std::clamp<double>(value, 0, 1);
        if (srgb) {
          constexpr double threshold = 0.0031308, linear_scale = 12.92, nonlinear_scale = 1.055, offset = 0.055;
          expected = expected <= threshold ? linear_scale * expected
                                           : nonlinear_scale * std::pow(expected, 1.0 / 2.4) - offset;
        }
        CHECK(bytes(back.Get(), expected * maximum));
      }
    }
    // four samples: the flip effects refuse them, DISCARD and SEQUENTIAL take them (Wine's test_swapchain_parameters,
    // on Windows)
    for (auto effect :
         {DXGI_SWAP_EFFECT_DISCARD, DXGI_SWAP_EFFECT_SEQUENTIAL, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL,
          DXGI_SWAP_EFFECT_FLIP_DISCARD}) {
      step("D3D%u effect %u with four samples", api, effect);
      bool flip = effect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL || effect == DXGI_SWAP_EFFECT_FLIP_DISCARD;
      DXGI_SWAP_CHAIN_DESC1 desc{width,    height, staging_desc.Format,  FALSE, {4, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT,
                                 2,        DXGI_SCALING_STRETCH, effect};
      ComPtr<IDXGISwapChain1> chain;
      HRESULT hr = factory->CreateSwapChainForHwnd(device.input.Get(), window, &desc, nullptr, nullptr, &chain);
      expect(hr == (flip ? DXGI_ERROR_INVALID_CALL : S_OK) && bool(chain) == !flip, "multisampled creation: %#lx", hr);
      chain.Reset();
      CHECK(device.idle());
    }
    for (auto effect :
         {DXGI_SWAP_EFFECT_DISCARD, DXGI_SWAP_EFFECT_SEQUENTIAL, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL,
          DXGI_SWAP_EFFECT_FLIP_DISCARD})
      for (bool legacy : {false, true})
        for (UINT count = 0; count <= DXGI_MAX_SWAP_CHAIN_BUFFERS + 1; count++) {
          step("D3D%u effect %u legacy %u count %u", api, effect, legacy, count);
          bool sequential = effect == DXGI_SWAP_EFFECT_SEQUENTIAL || effect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
          bool flip = effect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL || effect == DXGI_SWAP_EFFECT_FLIP_DISCARD;
          bool valid = count && count <= DXGI_MAX_SWAP_CHAIN_BUFFERS && (!flip || count >= 2);
          DXGI_SWAP_CHAIN_DESC1 desc{
              width,
              height,
              staging_desc.Format,
              FALSE,
              {1, 0},
              DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT,
              count,
              DXGI_SCALING_STRETCH,
              effect
          };
          ComPtr<IDXGISwapChain> chain;
          HRESULT hr;
          if (legacy) {
            DXGI_SWAP_CHAIN_DESC old{
                {width, height, {}, desc.Format}, desc.SampleDesc, desc.BufferUsage, count, window, TRUE, effect
            };
            hr = factory->CreateSwapChain(device.input.Get(), &old, &chain);
          } else {
            ComPtr<IDXGISwapChain1> first;
            hr = factory->CreateSwapChainForHwnd(device.input.Get(), window, &desc, nullptr, nullptr, &first);
            if (first)
              CHECK(first.As(&chain));
          }
          expect(
              hr == (valid ? S_OK : DXGI_ERROR_INVALID_CALL) && bool(chain) == valid, "buffer count creation: %#lx", hr
          );
          if (!valid || !chain)
            continue;
          UINT accessible = sequential ? count : 1;
          std::vector<ComPtr<ID3D10Texture2D>> backs(accessible);
          for (UINT index = 0; index <= count; index++) {
            ComPtr<ID3D10Texture2D> back;
            HRESULT get = chain->GetBuffer(index, IID_PPV_ARGS(&back));
            bool available = index < accessible;
            expect(
                get == (available ? S_OK : DXGI_ERROR_INVALID_CALL) && bool(back) == available,
                "buffer %u accessibility: %#lx", index, get
            );
            if (!available || !back)
              continue;
            backs[index] = back;
            for (UINT before = 0; before < index; before++)
              expect(backs[before] != back, "distinct buffer indices alias");
            ComPtr<ID3D10Texture2D> again;
            CHECK(chain->GetBuffer(index, IID_PPV_ARGS(&again)));
            expect(again == back, "GetBuffer identity changed");
            ComPtr<IDXGIResource> resource;
            CHECK(back.As(&resource));
            DXGI_USAGE usage = 0;
            CHECK(resource->GetUsage(&usage));
            expect(
                usage == (desc.BufferUsage | DXGI_USAGE_BACK_BUFFER | (index ? DXGI_USAGE_READ_ONLY : 0) |
                          (sequential ? 0 : DXGI_USAGE_DISCARD_ON_PRESENT)),
                "back-buffer usage"
            );
            ComPtr<ID3D10RenderTargetView> view;
            expect(
                device.d10->CreateRenderTargetView(back.Get(), nullptr, &view) == (index ? E_INVALIDARG : S_OK),
                "read-only render-target view"
            );
          }
          ComPtr<ID3D10RenderTargetView> view;
          CHECK(device.d10->CreateRenderTargetView(backs[0].Get(), nullptr, &view));
          std::vector<BYTE> expected(accessible);
          for (UINT frame = 0; frame <= count * 2; frame++) {
            BYTE value = BYTE(frame + 1);
            const float color[] = {float(value) / maximum, 0, 0, 1};
            device.d10->ClearRenderTargetView(view.Get(), color);
            expected[0] = value;
            CHECK(bytes(backs[0].Get(), value));
            CHECK(chain->Present(0, 0));
            if (sequential) {
              std::rotate(expected.begin(), expected.begin() + 1, expected.end());
              if (frame + 1 >= count)
                for (UINT index = 0; index < accessible; index++)
                  CHECK(bytes(backs[index].Get(), expected[index]));
            }
          }
          view.Reset();
          backs.clear();
          ComPtr<ID3D10Texture2D> retained;
          CHECK(chain->GetBuffer(0, IID_PPV_ARGS(&retained)));
          auto refuses = [&] {
            expect(
                chain->ResizeBuffers(0, width + 1, height + 1, DXGI_FORMAT_UNKNOWN, 0) == DXGI_ERROR_INVALID_CALL,
                "resize accepted outstanding reference"
            );
            DXGI_SWAP_CHAIN_DESC got{};
            expect(
                chain->GetDesc(&got) == S_OK && got.BufferDesc.Width == width && got.BufferDesc.Height == height &&
                    got.BufferCount == count,
                "refused resize changed state"
            );
          };
          refuses();
          ComPtr<ID3D10ShaderResourceView> srv;
          CHECK(device.d10->CreateShaderResourceView(retained.Get(), nullptr, &srv));
          retained.Reset();
          refuses();
          srv.Reset();
          CHECK(chain->GetBuffer(0, IID_PPV_ARGS(&retained)));
          CHECK(device.d10->CreateRenderTargetView(retained.Get(), nullptr, &view));
          retained.Reset();
          refuses();
          device.d10->OMSetRenderTargets(1, view.GetAddressOf(), nullptr);
          view.Reset();
          refuses();
          device.d10->ClearState();
          CHECK(device.idle());
          CHECK(chain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0));
          CHECK(device.idle());
        }
  }
  DestroyWindow(window);
  return verdict();
}
