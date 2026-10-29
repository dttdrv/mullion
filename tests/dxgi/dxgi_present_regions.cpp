// contract: Present1 combines previous-frame scroll pixels with this frame's
// dirty pixels, which take priority. "The scroll rectangle and the list of
// dirty rectangles could overlap. In this situation, the dirty rectangles take
// priority." (DXGI_PRESENT_PARAMETERS, Remarks.)
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/ns-dxgi1_2-dxgi_present_parameters
// pScrollRect is the destination: "the region on the current frame that is filled with scrolled content."
// the CPU retains the previous displayed image. only reported dirty rectangles
// are written to the current buffer; two distinct full frames initialize the
// buffers first, so showing an old back buffer cannot pass by coincidence. the
// existing GDI presentation path exposes the composed pixels. staging Map and
// queue fences finish GPU work before the window is read; there are no polling
// delays. no shader compiler is involved in these copy-only checks.
// a stereo chain keeps both eyes the same way. reduced to mono, as an output
// without windowed stereo is, it shows the left eye, or the right one when
// "the stereo present must be reduced to mono" under DXGI_PRESENT_STEREO_PREFER_RIGHT.
// https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-present
#include "dxgi_test.hpp"
#include <algorithm>
#include <array>

int
main() {
  SetEnvironmentVariableA("MULLION_CONFIG", "dxgi.presentThroughGDI=True");
  constexpr UINT width = 17, height = 13, channels = 4;
  using Pixel = std::array<BYTE, channels>;
  using Image = std::array<Pixel, width * height>;
  const RECT whole{0, 0, width, height};
  WNDCLASSA wc{0,       DefWindowProcA, 0,       0,       GetModuleHandleA(nullptr),
               nullptr, nullptr,        nullptr, nullptr, "dxgi_present_regions"};
  if (!expect(RegisterClassA(&wc), "RegisterClass: %lu", GetLastError()))
    return verdict();
  ComPtr<IDXGIFactory2> factory;
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  for (BOOL stereo : {FALSE, TRUE})
    for (UINT api : dxgi_apis())
      for (bool deferred : {false, true}) {
        // a stereo output shows no single eye to read back
        if ((deferred && api != 11) || (stereo && factory->IsWindowedStereoEnabled()))
          continue;
        DxgiDevice device;
        CHECK(device.create(api));
        HWND window = CreateWindowA(
            wc.lpszClassName, wc.lpszClassName, WS_POPUP | WS_VISIBLE, 0, 0, width, height, nullptr, nullptr,
            wc.hInstance, nullptr
        );
        if (!expect(window != nullptr, "CreateWindow: %lu", GetLastError()))
          return verdict();
        UpdateWindow(window);
        DXGI_SWAP_CHAIN_DESC1 desc{
            width,
            height,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            stereo,
            {1, 0},
            DXGI_USAGE_RENDER_TARGET_OUTPUT,
            2,
            DXGI_SCALING_STRETCH,
            DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL
        };
        ComPtr<IDXGISwapChain1> first;
        CHECK(factory->CreateSwapChainForHwnd(device.input.Get(), window, &desc, nullptr, nullptr, &first));
        ComPtr<IDXGISwapChain3> chain;
        CHECK(first.As(&chain));
        const UINT eyes = stereo ? 2 : 1;
        std::array<Image, 2> previous{}, data{};
        UINT frame = 0;
        // the right eye's pattern is three steps on, so every pixel tells the eyes apart
        auto pattern = [&] {
          for (UINT eye = 0; eye < eyes; eye++)
            for (UINT y = 0; y < height; y++)
              for (UINT x = 0; x < width; x++) {
                UINT bits = frame + x + width * y + 3 * eye;
                data[eye][y * width + x] = {
                    BYTE((bits & 1) ? 255 : 0), BYTE((bits & 2) ? 255 : 0), BYTE((bits & 4) ? 255 : 0), 255
                };
              }
        };
        auto write = [&](const std::vector<RECT> &rects) -> HRESULT {
          HRESULT hr;
          if (device.d12) {
            ComPtr<ID3D12Resource> back;
            if (FAILED(hr = chain->GetBuffer(chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back))))
              return hr;
            auto resource_desc = back->GetDesc();
            std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 2> footprints{};
            UINT64 size = 0;
            device.d12->GetCopyableFootprints(&resource_desc, 0, eyes, 0, footprints.data(), nullptr, nullptr, &size);
            auto upload = buffer(device.d12.Get(), D3D12_HEAP_TYPE_UPLOAD, size, D3D12_RESOURCE_STATE_GENERIC_READ);
            if (!upload)
              return E_OUTOFMEMORY;
            BYTE *mapped = nullptr;
            if (FAILED(hr = upload->Map(0, nullptr, reinterpret_cast<void **>(&mapped))))
              return hr;
            for (UINT eye = 0; eye < eyes; eye++)
              for (UINT y = 0; y < height; y++)
                memcpy(
                    mapped + footprints[eye].Offset + y * footprints[eye].Footprint.RowPitch, &data[eye][y * width],
                    width * sizeof(Pixel)
                );
            upload->Unmap(0, nullptr);
            ComPtr<ID3D12CommandAllocator> allocator;
            ComPtr<ID3D12GraphicsCommandList> list;
            if (FAILED(
                    hr = device.d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))
                ) ||
                FAILED(
                    hr = device.d12->CreateCommandList(
                        0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)
                    )
                ))
              return hr;
            transition(list.Get(), back.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
            for (UINT eye = 0; eye < eyes; eye++) {
              D3D12_TEXTURE_COPY_LOCATION source{
                  upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprints[eye]}
              },
                  target{back.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = eye}};
              for (const auto &r : rects) {
                D3D12_BOX box{UINT(r.left), UINT(r.top), 0, UINT(r.right), UINT(r.bottom), 1};
                list->CopyTextureRegion(&target, r.left, r.top, 0, &source, &box);
              }
            }
            transition(list.Get(), back.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
            return submit(device.d12.Get(), device.queue.Get(), list.Get());
          }
          if (device.d11) {
            ComPtr<ID3D11Texture2D> back;
            if (FAILED(hr = chain->GetBuffer(0, IID_PPV_ARGS(&back))))
              return hr;
            auto context = deferred ? device.deferred.Get() : device.immediate.Get();
            for (UINT eye = 0; eye < eyes; eye++)
              for (const auto &r : rects) {
                D3D11_BOX box{UINT(r.left), UINT(r.top), 0, UINT(r.right), UINT(r.bottom), 1};
                context->UpdateSubresource(
                    back.Get(), eye, &box, &data[eye][r.top * width + r.left], width * sizeof(Pixel), 0
                );
              }
            if (deferred) {
              ComPtr<ID3D11CommandList> commands;
              if (FAILED(hr = context->FinishCommandList(FALSE, &commands)))
                return hr;
              device.immediate->ExecuteCommandList(commands.Get(), FALSE);
            }
            return S_OK;
          }
          ComPtr<ID3D10Texture2D> back;
          if (FAILED(hr = chain->GetBuffer(0, IID_PPV_ARGS(&back))))
            return hr;
          for (UINT eye = 0; eye < eyes; eye++)
            for (const auto &r : rects) {
              D3D10_BOX box{UINT(r.left), UINT(r.top), 0, UINT(r.right), UINT(r.bottom), 1};
              device.d10->UpdateSubresource(
                  back.Get(), eye, &box, &data[eye][r.top * width + r.left], width * sizeof(Pixel), 0
              );
            }
          return S_OK;
        };
        auto shows = [&](const Image &expected) {
          HDC dc = GetDC(window);
          if (!expect(dc != nullptr, "GetDC"))
            return;
          UINT wrong = 0;
          for (UINT y = 0; y < height; y++)
            for (UINT x = 0; x < width; x++) {
              const auto &p = expected[y * width + x];
              COLORREF got = GetPixel(dc, x, y), wanted = RGB(p[0], p[1], p[2]);
              if (got != wanted && !wrong++)
                expect(false, "display pixel %u,%u = %#lx, expected %#lx", x, y, got, wanted);
            }
          ReleaseDC(window, dc);
          expect(!wrong, "%u displayed pixels differ", wrong);
        };
        auto present = [&](std::vector<RECT> dirty, RECT *scroll = nullptr, POINT *offset = nullptr) {
          // the eyes take turns, so each one's kept pixels are seen after a frame that showed the other
          const UINT shown = frame % eyes;
          step(
              "D3D%u deferred %u stereo %u eye %u frame %u dirty %zu scroll %ld,%ld", api, deferred, stereo, shown,
              frame, dirty.size(), offset ? offset->x : 0, offset ? offset->y : 0
          );
          auto expected = previous;
          if (scroll && offset)
            for (UINT eye = 0; eye < eyes; eye++)
              for (LONG y = scroll->top; y < scroll->bottom; y++)
                for (LONG x = scroll->left; x < scroll->right; x++)
                  expected[eye][y * width + x] = previous[eye][(y - offset->y) * width + x - offset->x];
          pattern();
          const std::vector<RECT> written = dirty.empty() ? std::vector<RECT>{whole} : dirty;
          for (UINT eye = 0; eye < eyes; eye++)
            for (const auto &r : written)
              for (LONG y = r.top; y < r.bottom; y++)
                for (LONG x = r.left; x < r.right; x++)
                  expected[eye][y * width + x] = data[eye][y * width + x];
          HRESULT hr = write(written);
          if (!expect(hr == S_OK, "write back buffer: %#lx", hr))
            return false;
          DXGI_PRESENT_PARAMETERS parameters{UINT(dirty.size()), dirty.data(), scroll, offset};
          hr = chain->Present1(0, shown ? DXGI_PRESENT_STEREO_PREFER_RIGHT : 0, &parameters);
          if (!expect(hr == S_OK, "Present1: %#lx", hr))
            return false;
          hr = device.idle();
          if (!expect(hr == S_OK, "finish presentation: %#lx", hr))
            return false;
          shows(expected[shown]);
          previous = expected;
          frame++;
          return true;
        };
        for (UINT i = 0; i < desc.BufferCount; i++)
          if (!present({}))
            return verdict();
        for (auto rects :
             {std::vector<RECT>{{0, 0, 1, 1}}, std::vector<RECT>{{LONG(width - 1), LONG(height - 1), width, height}},
              std::vector<RECT>{{0, 0, LONG(width / 2), height}, {LONG(width / 2), 0, width, height}},
              std::vector<RECT>{
                  {1, 1, LONG(width - 1), LONG(height - 1)}, {LONG(width / 2), LONG(height / 2), width, height}
              }})
          if (!present(rects))
            return verdict();
        for (POINT offset :
             {POINT{1, 0}, POINT{-1, 0}, POINT{0, 1}, POINT{0, -1}, POINT{1, 1}, POINT{-1, -1}, POINT{0, 0}}) {
          RECT destination{1, 1, LONG(width - 1), LONG(height - 1)};
          std::vector<RECT> dirty;
          for (RECT r :
               {RECT{0, 0, width, destination.top}, RECT{0, destination.bottom, width, height},
                RECT{0, destination.top, destination.left, destination.bottom},
                RECT{destination.right, destination.top, width, destination.bottom},
                RECT{LONG(width / 2), LONG(height / 2), LONG(width / 2 + 1), LONG(height / 2 + 1)}})
            if (r.left < r.right && r.top < r.bottom)
              dirty.push_back(r);
          if (!present(dirty, &destination, &offset))
            return verdict();
        }
        CHECK(device.idle());
        chain.Reset();
        first.Reset();
        DestroyWindow(window);
      }
  UnregisterClassA(wc.lpszClassName, wc.hInstance);
  return verdict();
}
