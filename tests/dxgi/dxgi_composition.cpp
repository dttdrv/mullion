// contract: composition creation accepts a valid flip chain and rejects invalid
// descriptors and HWND-only calls. "DXGI_ERROR_INVALID_CALL if the calling
// application provided invalid data" (CreateSwapChainForComposition).
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforcomposition
// "Only scale and translation components are allowed in the matrix."
// (SetMatrixTransform, pMatrix.)
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-setmatrixtransform
#include "dxgi_test.hpp"
#include <array>

int
main() {
  ComPtr<IDXGIFactory2> factory;
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  for (UINT api : dxgi_apis()) {
    DxgiDevice device;
    CHECK(device.create(api));
    const DXGI_SWAP_CHAIN_DESC1 valid{
        3,
        5,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        FALSE,
        {1, 0},
        DXGI_USAGE_RENDER_TARGET_OUTPUT,
        2,
        DXGI_SCALING_STRETCH,
        DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL,
        DXGI_ALPHA_MODE_PREMULTIPLIED
    };
    step("D3D%u failed composition creation clears a populated output slot", api);
    auto sentinel = reinterpret_cast<IDXGISwapChain1 *>(factory.Get());
    IDXGISwapChain1 *raw = sentinel;
    HRESULT failed = factory->CreateSwapChainForComposition(nullptr, &valid, nullptr, &raw);
    expect(FAILED(failed) && !raw, "failed composition creation did not clear its output");
    if (SUCCEEDED(failed) && raw && raw != sentinel)
      raw->Release();
    for (UINT invalid = 0; invalid < 8; invalid++) {
      step("D3D%u invalid composition descriptor %u", api, invalid);
      auto desc = valid;
      IUnknown *input = device.input.Get();
      const DXGI_SWAP_CHAIN_DESC1 *pointer = &desc;
      switch (invalid) {
      case 0:
        input = nullptr;
        break;
      case 1:
        pointer = nullptr;
        break;
      case 2:
        desc.Width = 0;
        break;
      case 3:
        desc.Height = 0;
        break;
      case 4:
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        break;
      case 5:
        desc.Scaling = DXGI_SCALING_NONE;
        break;
      case 6:
        desc.BufferCount = 1;
        break;
      case 7:
        desc.SampleDesc.Count = 2;
        break;
      }
      ComPtr<IDXGISwapChain1> chain;
      HRESULT hr = factory->CreateSwapChainForComposition(input, pointer, nullptr, &chain);
      expect(hr == DXGI_ERROR_INVALID_CALL && !chain, "invalid composition creation: %#lx", hr);
    }
    step("D3D%u NULL composition output", api);
    expect(
        factory->CreateSwapChainForComposition(device.input.Get(), &valid, nullptr, nullptr) == DXGI_ERROR_INVALID_CALL,
        "NULL composition output accepted"
    );
    step("D3D%u non-CoreWindow object refused", api);
    ComPtr<IDXGISwapChain1> unsupported;
    HRESULT hr =
        factory->CreateSwapChainForCoreWindow(device.input.Get(), factory.Get(), &valid, nullptr, &unsupported);
    expect(FAILED(hr) && !unsupported, "non-CoreWindow creation: %#lx", hr);
    for (auto format :
         {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM,
          DXGI_FORMAT_R16G16B16A16_FLOAT}) {
      step("D3D%u composition format %u", api, format);
      auto desc = valid;
      desc.Format = format;
      ComPtr<IDXGISwapChain1> chain;
      hr = factory->CreateSwapChainForComposition(device.input.Get(), &desc, nullptr, &chain);
      if (!expect(hr == S_OK && chain, "valid composition creation: %#lx", hr))
        continue;
      DXGI_SWAP_CHAIN_DESC1 got{};
      CHECK(chain->GetDesc1(&got));
      expect(
          got.Width == desc.Width && got.Height == desc.Height && got.Format == desc.Format &&
              got.BufferCount == desc.BufferCount && got.AlphaMode == desc.AlphaMode &&
              got.SwapEffect == desc.SwapEffect && got.Scaling == desc.Scaling,
          "composition descriptor"
      );
      HWND window = nullptr;
      ComPtr<IDXGIOutput> output;
      DXGI_MODE_DESC mode{desc.Width, desc.Height, {}, desc.Format};
      expect(FAILED(chain->GetHwnd(&window)) && !window, "composition chain has an HWND");
      expect(FAILED(chain->GetContainingOutput(&output)) && !output, "composition chain has a containing output");
      expect(FAILED(chain->SetFullscreenState(TRUE, nullptr)), "composition chain entered fullscreen");
      expect(FAILED(chain->ResizeTarget(&mode)), "composition chain resized HWND target");
      ComPtr<IDXGISwapChain2> chain2;
      CHECK(chain.As(&chain2));
      DXGI_MATRIX_3X2_F matrix{};
      CHECK(chain2->GetMatrixTransform(&matrix));
      const DXGI_MATRIX_3X2_F identity{1, 0, 0, 1, 0, 0};
      expect(!memcmp(&matrix, &identity, sizeof(matrix)), "initial composition transform is not identity");
      for (const auto &wanted :
           {identity, DXGI_MATRIX_3X2_F{0.5f, 0, 0, 2, 3, -5}, DXGI_MATRIX_3X2_F{2, 0, 0, 0.5f, -3, 5}}) {
        expect(chain2->SetMatrixTransform(&wanted) == S_OK, "valid composition transform");
        CHECK(chain2->GetMatrixTransform(&matrix));
        expect(!memcmp(&matrix, &wanted, sizeof(matrix)), "composition matrix roundtrip");
      }
      expect(chain2->SetMatrixTransform(nullptr) == E_INVALIDARG, "NULL composition transform");
      for (bool shear : {false, true}) {
        auto invalid = identity;
        if (shear)
          invalid._12 = 1;
        else
          invalid._21 = 1;
        expect(chain2->SetMatrixTransform(&invalid) == E_INVALIDARG, "composition shear accepted");
      }
      CHECK(device.idle());
    }
  }
  return verdict();
}
