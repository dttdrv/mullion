// contract: a flip swap chain accepts exactly DESC1's four display formats, independently of the creation API.
// "you must set the Format member to DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM,
// DXGI_FORMAT_R8G8B8A8_UNORM, or DXGI_FORMAT_R10G10B10A10_UNORM" (DXGI_SWAP_CHAIN_DESC1, Remarks).
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/ns-dxgi1_2-dxgi_swap_chain_desc1
// that page's last spelling is a typo: dxgiformat.h defines R10G10B10A2_UNORM. ResizeBuffers keeps UNKNOWN,
// and a rejected format leaves every descriptor field and the existing resource identity unchanged.
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-resizebuffers
// dxgi_color_space supplies positive byte/pixel checks for all four formats; this test supplies the complement.
// each back-buffer axis is also constrained by feature level: "Width, Height, and DepthOrArraySize must be
// between 1 and the maximum dimension supported for the particular feature level and texture dimension."
// (D3D12_RESOURCE_DESC, Textures; D3D11.3 21, Texture2D U/V Dimension.)
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_resource_desc
// "you can associate only one flip presentation model swap chain at a time with an HWND" across factories and APIs.
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforhwnd
#include "dxgi_test.hpp"
#include "../depth_formats.hpp"
#include <array>
#include <barrier>
#include <limits>
#include <thread>

int
main() {
  SetEnvironmentVariableA("MULLION_CONFIG", "dxgi.presentThroughGDI=True");
  const struct {
    DXGI_FORMAT value;
    const char *name;
  } formats[] = {DXGI_FORMATS, {DXGI_FORMAT_FORCE_UINT, "DXGI_FORMAT_FORCE_UINT"}};
  auto allowed = [](DXGI_FORMAT format) {
    return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM ||
           format == DXGI_FORMAT_R10G10B10A2_UNORM || format == DXGI_FORMAT_R16G16B16A16_FLOAT;
  };
  ComPtr<IDXGIFactory2> factory;
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  HWND window = CreateWindowA(
      "static", "dxgi_formats", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr
  );
  if (!expect(window != nullptr, "CreateWindow"))
    return verdict();
  for (UINT api : dxgi_apis()) {
    DxgiDevice device;
    CHECK(device.create(api));
    DxgiDevice other_device;
    CHECK(other_device.create(api == 12 ? 11 : 12));
    ComPtr<IDXGIFactory2> other_factory;
    CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&other_factory)));
    auto marker = [&](IUnknown *buffer, bool write, UINT64 &value) -> HRESULT {
      auto transfer = [&](auto *object) {
        UINT size = sizeof(value);
        return write ? object->SetPrivateData(__uuidof(IDXGISwapChain), size, &value)
                     : object->GetPrivateData(__uuidof(IDXGISwapChain), &size, &value);
      };
      HRESULT hr;
      if (device.d12) {
        ComPtr<ID3D12Object> object;
        hr = buffer->QueryInterface(IID_PPV_ARGS(&object));
        return FAILED(hr) ? hr : transfer(object.Get());
      }
      ComPtr<IDXGIObject> object;
      hr = buffer->QueryInterface(IID_PPV_ARGS(&object));
      return FAILED(hr) ? hr : transfer(object.Get());
    };
    for (auto effect : {DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, DXGI_SWAP_EFFECT_FLIP_DISCARD})
      for (bool legacy : {false, true}) {
        DXGI_SWAP_CHAIN_DESC1 desc{3,
                                   5,
                                   DXGI_FORMAT_R8G8B8A8_UNORM,
                                   FALSE,
                                   {1, 0},
                                   DXGI_USAGE_RENDER_TARGET_OUTPUT,
                                   2,
                                   DXGI_SCALING_STRETCH,
                                   effect};
        auto create = [&](DXGI_FORMAT format, ComPtr<IDXGISwapChain> &chain) {
          if (legacy) {
            DXGI_SWAP_CHAIN_DESC old{{desc.Width, desc.Height, {}, format},
                                     desc.SampleDesc,
                                     desc.BufferUsage,
                                     desc.BufferCount,
                                     window,
                                     TRUE,
                                     effect};
            return factory->CreateSwapChain(device.input.Get(), &old, &chain);
          }
          auto requested = desc;
          requested.Format = format;
          ComPtr<IDXGISwapChain1> first;
          HRESULT hr =
              factory->CreateSwapChainForHwnd(device.input.Get(), window, &requested, nullptr, nullptr, &first);
          if (first)
            expect(first.As(&chain) == S_OK, "swap-chain QI");
          return hr;
        };
        // the feature-level texture dimension limits apply to each back-buffer axis independently.
        UINT limit = api == 12   ? D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION
                     : api == 11 ? D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION
                                 : D3D10_REQ_TEXTURE2D_U_OR_V_DIMENSION;
        RECT client{};
        CHECK(GetClientRect(window, &client) ? S_OK : E_FAIL);
        for (bool vertical : {false, true})
          for (UINT extent : {0u, 1u, limit, limit + 1, std::numeric_limits<UINT>::max()}) {
            desc.Width = vertical ? 1 : extent;
            desc.Height = vertical ? extent : 1;
            step("D3D%u effect %u legacy %u extent %u vertical %u", api, effect, legacy, extent, vertical);
            ComPtr<IDXGISwapChain> chain;
            HRESULT hr = create(desc.Format, chain);
            bool valid = extent <= limit;
            expect(valid ? hr == S_OK && chain : FAILED(hr) && !chain, "dimension creation: %#lx", hr);
            if (valid && chain) {
              DXGI_SWAP_CHAIN_DESC got{};
              CHECK(chain->GetDesc(&got));
              UINT expected_width = desc.Width ? desc.Width : client.right;
              UINT expected_height = desc.Height ? desc.Height : client.bottom;
              expect(
                  got.BufferDesc.Width == expected_width && got.BufferDesc.Height == expected_height,
                  "created dimensions do not match the requested/client axes"
              );
              CHECK(chain->ResizeBuffers(0, vertical ? 1 : limit, vertical ? limit : 1, DXGI_FORMAT_UNKNOWN, 0));
              expected_width = vertical ? 1 : limit;
              expected_height = vertical ? limit : 1;
              CHECK(chain->GetDesc(&got));
              expect(
                  got.BufferDesc.Width == expected_width && got.BufferDesc.Height == expected_height,
                  "successful boundary resize has incorrect dimensions"
              );
              ComPtr<IDXGISwapChain3> extended_chain;
              CHECK(chain.As(&extended_chain));
              for (bool extended : {false, true}) {
                if (extended && !device.d12)
                  continue;
                std::vector<UINT> nodes(desc.BufferCount, 1);
                std::vector<IUnknown *> queues(desc.BufferCount, device.queue.Get());
                for (UINT oversized : {limit + 1, std::numeric_limits<UINT>::max()}) {
                  ComPtr<IUnknown> old_buffer;
                  CHECK(chain->GetBuffer(0, IID_PPV_ARGS(&old_buffer)));
                  auto identity = reinterpret_cast<uintptr_t>(old_buffer.Get());
                  UINT64 stamp = oversized;
                  CHECK(marker(old_buffer.Get(), true, stamp));
                  old_buffer.Reset();
                  HRESULT resized = extended
                                        ? extended_chain->ResizeBuffers1(
                                              0, vertical ? 1 : oversized, vertical ? oversized : 1,
                                              DXGI_FORMAT_UNKNOWN, 0, nodes.data(), queues.data()
                                          )
                                        : chain->ResizeBuffers(
                                              0, vertical ? 1 : oversized, vertical ? oversized : 1,
                                              DXGI_FORMAT_UNKNOWN, 0
                                          );
                  expect(
                      FAILED(resized), "resize%s exceeded texture dimension limit %u", extended ? "1" : "", oversized
                  );
                  CHECK(chain->GetDesc(&got));
                  expect(
                      got.BufferDesc.Width == expected_width && got.BufferDesc.Height == expected_height,
                      "failed oversized resize changed dimensions"
                  );
                  CHECK(chain->GetBuffer(0, IID_PPV_ARGS(&old_buffer)));
                  UINT64 value = 0;
                  expect(
                      reinterpret_cast<uintptr_t>(old_buffer.Get()) == identity, "failed size resize replaced buffer"
                  );
                  expect(
                      marker(old_buffer.Get(), false, value) == S_OK && value == stamp,
                      "failed size resize lost resource data"
                  );
                }
              }
            }
          }
        desc.Width = 3;
        desc.Height = 5;
        for (auto [format, name] : formats) {
          step("D3D%u effect %u legacy %u create %s", api, effect, legacy, name);
          ComPtr<IDXGISwapChain> chain;
          HRESULT hr = create(format, chain);
          expect(hr == (allowed(format) ? S_OK : DXGI_ERROR_INVALID_CALL), "format creation: %#lx", hr);
          expect(bool(chain) == allowed(format), "format creation output");
          if (allowed(format) && chain) {
            DXGI_SWAP_CHAIN_DESC got{};
            CHECK(chain->GetDesc(&got));
            expect(
                got.BufferDesc.Format == format && got.BufferDesc.Width == desc.Width &&
                    got.BufferDesc.Height == desc.Height && got.BufferCount == desc.BufferCount,
                "created format descriptor"
            );
            ComPtr<IDXGISwapChain> duplicate;
            HRESULT repeated = create(format, duplicate);
            expect(repeated == DXGI_ERROR_INVALID_CALL && !duplicate, "second flip chain on the HWND: %#lx", repeated);
            duplicate.Reset();
            auto requested = desc;
            requested.Format = format;
            for (IUnknown *input : {device.input.Get(), other_device.input.Get()}) {
              ComPtr<IDXGISwapChain1> other;
              repeated = other_factory->CreateSwapChainForHwnd(input, window, &requested, nullptr, nullptr, &other);
              expect(
                  repeated == DXGI_ERROR_INVALID_CALL && !other,
                  "other factory/frontend accepted a second flip chain: %#lx", repeated
              );
            }
            chain.Reset();
            CHECK(create(format, chain));
          }
        }
        step("D3D%u effect %u concurrent flip HWND reservation", api, effect);
        {
          std::barrier start(3);
          std::array<ComPtr<IDXGISwapChain1>, 2> chains;
          std::array<HRESULT, 2> results{};
          std::array<std::thread, 2> callers;
          for (UINT i = 0; i < callers.size(); i++)
            callers[i] = std::thread([&, i] {
              start.arrive_and_wait();
              results[i] = (i ? other_factory.Get() : factory.Get())->CreateSwapChainForHwnd(
                  i ? other_device.input.Get() : device.input.Get(), window, &desc, nullptr, nullptr, &chains[i]
              );
            });
          start.arrive_and_wait();
          for (auto &caller : callers)
            caller.join();
          expect(
              (results[0] == S_OK && chains[0] && results[1] == DXGI_ERROR_INVALID_CALL && !chains[1]) ||
                  (results[1] == S_OK && chains[1] && results[0] == DXGI_ERROR_INVALID_CALL && !chains[0]),
              "concurrent creation did not reserve exactly once: %#lx, %#lx", results[0], results[1]
          );
        }
        ComPtr<IDXGISwapChain> base;
        CHECK(create(desc.Format, base));
        ComPtr<IDXGISwapChain3> chain;
        CHECK(base.As(&chain));
        std::vector<UINT> nodes(desc.BufferCount, 1);
        std::vector<IUnknown *> queues(desc.BufferCount, device.queue.Get());
        for (auto [format, name] : formats)
          for (bool extended : {false, true}) {
            if (extended && api != 12)
              continue;
            step("D3D%u effect %u legacy %u resize%s %s", api, effect, legacy, extended ? "1" : "", name);
            DXGI_SWAP_CHAIN_DESC before{}, after{};
            CHECK(chain->GetDesc(&before));
            ComPtr<IUnknown> old_buffer;
            CHECK(chain->GetBuffer(0, IID_PPV_ARGS(&old_buffer)));
            auto identity = reinterpret_cast<uintptr_t>(old_buffer.Get());
            UINT64 stamp = (UINT64(api) << std::numeric_limits<UINT>::digits) | UINT(format);
            CHECK(marker(old_buffer.Get(), true, stamp));
            old_buffer.Reset();
            HRESULT hr = extended ? chain->ResizeBuffers1(
                                        0, desc.Width, desc.Height, format, 0, nodes.data(), queues.data()
                                    )
                                  : chain->ResizeBuffers(0, desc.Width, desc.Height, format, 0);
            const bool valid = allowed(format) || format == DXGI_FORMAT_UNKNOWN;
            expect(hr == (valid ? S_OK : DXGI_ERROR_INVALID_CALL), "format resize: %#lx", hr);
            CHECK(chain->GetDesc(&after));
            expect(
                after.BufferDesc.Format ==
                        (valid && format != DXGI_FORMAT_UNKNOWN ? format : before.BufferDesc.Format) &&
                    after.BufferDesc.Width == before.BufferDesc.Width &&
                    after.BufferDesc.Height == before.BufferDesc.Height && after.BufferCount == before.BufferCount &&
                    after.SwapEffect == before.SwapEffect && after.Flags == before.Flags,
                "failed resize or UNKNOWN changed descriptor"
            );
            if (!valid) {
              CHECK(chain->GetBuffer(0, IID_PPV_ARGS(&old_buffer)));
              expect(
                  reinterpret_cast<uintptr_t>(old_buffer.Get()) == identity, "failed resize replaced buffer identity"
              );
              UINT64 value = 0;
              HRESULT stored = marker(old_buffer.Get(), false, value);
              expect(stored == S_OK && value == stamp, "failed resize lost the old resource's marker");
            }
          }
      }
  }
  DestroyWindow(window);
  return verdict();
}
