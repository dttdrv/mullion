// contract: the color-space tag controls interpretation, and HDR metadata does
// not rewrite back-buffer bytes. "Float-valued display formats (currently only
// DXGI_FORMAT_R16G16B16A16_FLOAT) contain linear-valued data." (Converting data
// for the color space.)
// https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/converting-data-color-space
// SetHDRMetaData "does not change how pixel values are interpreted by Windows
// or monitors."
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_5/nf-dxgi1_5-idxgiswapchain4-sethdrmetadata
// D3D11.3 3.2.3.8: "The following is the ideal FLOAT -> SRGB conversion."
// all half encodings, including signed zero, infinities, NaNs and denormals, and every
// UNORM8/UNORM10 channel code are copied without a shader, read back byte for byte,
// and presented through Mullion's SDR GDI path. its BGRA8 bitmap must implement
// these conversions before StretchDIBits; the pixel oracle is specific to this
// local path, not a precision guarantee for Windows composition or HDR scanout.
// PQ checks source-byte preservation.
#include "dxgi_test.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

int
main() {
  SetEnvironmentVariableA("MULLION_CONFIG", "dxgi.presentThroughGDI=True");
  // D3D11.3 3.2.2: binary16 has one sign bit, five exponent bits and ten
  // fraction bits.
  constexpr UINT half_exponent_bits = 5, half_fraction_bits = 10;
  constexpr UINT half_bias = (1u << (half_exponent_bits - 1)) - 1;
  constexpr UINT half_one = half_bias << half_fraction_bits;
  constexpr UINT rgb10_bits = 10, alpha2_bits = 2;
  constexpr UINT rgb10_max = (1u << rgb10_bits) - 1, alpha2_max = (1u << alpha2_bits) - 1;
  constexpr UINT byte_max = std::numeric_limits<BYTE>::max(), row_width = 128;
  auto half = [](UINT word) {
    constexpr UINT exponent_max = (1u << half_exponent_bits) - 1;
    UINT exponent = (word >> half_fraction_bits) & exponent_max, fraction = word & ((1u << half_fraction_bits) - 1);
    double value = exponent == exponent_max
                       ? (fraction ? std::numeric_limits<double>::quiet_NaN() : std::numeric_limits<double>::infinity())
                   : exponent
                       ? std::ldexp(1.0 + double(fraction) / (1u << half_fraction_bits), int(exponent) - int(half_bias))
                       : std::ldexp(double(fraction), 1 - int(half_bias) - int(half_fraction_bits));
    return word & (1u << (half_exponent_bits + half_fraction_bits)) ? -value : value;
  };
  auto encode = [](double value) {
    constexpr double threshold = 0.0031308, scale_1 = 12.92, scale_2 = 1.055, offset = 0.055;
    constexpr double exponent = 1.0 / 2.4;
    return value <= threshold ? value * scale_1 : scale_2 * std::pow(value, exponent) - offset;
  };
  WNDCLASSA wc{0,       DefWindowProcA, 0,       0,       GetModuleHandleA(nullptr),
               nullptr, nullptr,        nullptr, nullptr, "dxgi_color_space"};
  if (!expect(RegisterClassA(&wc), "RegisterClass"))
    return verdict();
  ComPtr<IDXGIFactory2> factory;
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  for (UINT api : dxgi_apis())
    for (bool deferred : {false, true}) {
      if (deferred && api != 11)
        continue;
      DxgiDevice device;
      CHECK(device.create(api));
      for (auto format :
           {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM,
            DXGI_FORMAT_R16G16B16A16_FLOAT}) {
        bool floating = format == DXGI_FORMAT_R16G16B16A16_FLOAT;
        UINT samples = floating                                  ? UINT(std::numeric_limits<UINT16>::max()) + 1
                       : format == DXGI_FORMAT_R10G10B10A2_UNORM ? rgb10_max + 1
                                                                 : byte_max + 1;
        UINT height = (samples + row_width - 1) / row_width;
        UINT texel = floating ? sizeof(UINT16[4]) : sizeof(UINT);
        std::vector<BYTE> input(row_width * height * texel);
        std::vector<std::array<double, 3>> decoded(row_width * height);
        for (UINT i = 0; i < decoded.size(); i++) {
          UINT code = i < samples ? i : 0;
          if (floating) {
            const UINT16 pixel[] = {
                UINT16(code), UINT16(code ^ (1u << (half_exponent_bits + half_fraction_bits))), UINT16(code ^ half_one),
                UINT16(code)
            };
            memcpy(input.data() + i * texel, pixel, sizeof(pixel));
            decoded[i] = {half(pixel[0]), half(pixel[1]), half(pixel[2])};
          } else if (format == DXGI_FORMAT_R10G10B10A2_UNORM) {
            UINT blue = code ^ (rgb10_max / 3);
            UINT word = code | ((rgb10_max - code) << rgb10_bits) | (blue << (rgb10_bits * 2)) |
                        ((code & alpha2_max) << (rgb10_bits * 3));
            memcpy(input.data() + i * texel, &word, sizeof(word));
            decoded[i] = {double(code) / rgb10_max, double(rgb10_max - code) / rgb10_max, double(blue) / rgb10_max};
          } else {
            BYTE pixel[] = {BYTE(code), BYTE(byte_max - code), BYTE(code ^ (byte_max / 3)), BYTE(code)};
            decoded[i] = {double(pixel[0]) / byte_max, double(pixel[1]) / byte_max, double(pixel[2]) / byte_max};
            if (format == DXGI_FORMAT_B8G8R8A8_UNORM)
              std::swap(pixel[0], pixel[2]);
            memcpy(input.data() + i * texel, pixel, sizeof(pixel));
          }
        }
        HWND window = CreateWindowA(
            wc.lpszClassName, wc.lpszClassName, WS_POPUP | WS_VISIBLE, 0, 0, row_width, height, nullptr, nullptr,
            wc.hInstance, nullptr
        );
        if (!expect(window != nullptr, "CreateWindow"))
          return verdict();
        UpdateWindow(window);
        DXGI_SWAP_CHAIN_DESC1 desc{
            row_width,
            height,
            format,
            FALSE,
            {1, 0},
            DXGI_USAGE_RENDER_TARGET_OUTPUT,
            2,
            DXGI_SCALING_STRETCH,
            DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL
        };
        ComPtr<IDXGISwapChain1> first;
        CHECK(factory->CreateSwapChainForHwnd(device.input.Get(), window, &desc, nullptr, nullptr, &first));
        ComPtr<IDXGISwapChain4> chain;
        CHECK(first.As(&chain));
        auto transfer = [&](bool write) -> HRESULT {
          HRESULT hr;
          std::vector<BYTE> output(input.size());
          if (device.d12) {
            ComPtr<ID3D12Resource> back;
            if (FAILED(hr = chain->GetBuffer(chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back))))
              return hr;
            auto resource_desc = back->GetDesc();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            UINT64 size = 0;
            device.d12->GetCopyableFootprints(&resource_desc, 0, 1, 0, &footprint, nullptr, nullptr, &size);
            auto upload = buffer(device.d12.Get(), D3D12_HEAP_TYPE_UPLOAD, size, D3D12_RESOURCE_STATE_GENERIC_READ);
            auto readback = buffer(device.d12.Get(), D3D12_HEAP_TYPE_READBACK, size, D3D12_RESOURCE_STATE_COPY_DEST);
            if (!upload || !readback)
              return E_OUTOFMEMORY;
            BYTE *mapped = nullptr;
            if (FAILED(hr = upload->Map(0, nullptr, reinterpret_cast<void **>(&mapped))))
              return hr;
            for (UINT y = 0; y < height; y++)
              memcpy(
                  mapped + y * footprint.Footprint.RowPitch, input.data() + y * row_width * texel, row_width * texel
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
            D3D12_TEXTURE_COPY_LOCATION source{
                upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}
            },
                target{back.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
                staging{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
            if (write) {
              transition(list.Get(), back.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
              list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
              transition(list.Get(), back.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
            } else
              transition(list.Get(), back.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->CopyTextureRegion(&staging, 0, 0, 0, &target, nullptr);
            transition(list.Get(), back.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
            if (FAILED(hr = submit(device.d12.Get(), device.queue.Get(), list.Get())) ||
                FAILED(hr = readback->Map(0, nullptr, reinterpret_cast<void **>(&mapped))))
              return hr;
            for (UINT y = 0; y < height; y++)
              memcpy(
                  output.data() + y * row_width * texel, mapped + y * footprint.Footprint.RowPitch, row_width * texel
              );
            readback->Unmap(0, nullptr);
          } else if (device.d11) {
            ComPtr<ID3D11Texture2D> back, staging;
            if (FAILED(hr = chain->GetBuffer(0, IID_PPV_ARGS(&back))))
              return hr;
            D3D11_TEXTURE2D_DESC texture{};
            back->GetDesc(&texture);
            texture.Usage = D3D11_USAGE_STAGING;
            texture.BindFlags = texture.MiscFlags = 0;
            texture.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(hr = device.d11->CreateTexture2D(&texture, nullptr, &staging)))
              return hr;
            auto context = deferred ? device.deferred.Get() : device.immediate.Get();
            if (write)
              context->UpdateSubresource(back.Get(), 0, nullptr, input.data(), row_width * texel, 0);
            context->CopyResource(staging.Get(), back.Get());
            if (deferred) {
              ComPtr<ID3D11CommandList> commands;
              if (FAILED(hr = context->FinishCommandList(FALSE, &commands)))
                return hr;
              device.immediate->ExecuteCommandList(commands.Get(), FALSE);
            }
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(hr = device.immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
              return hr;
            for (UINT y = 0; y < height; y++)
              memcpy(
                  output.data() + y * row_width * texel, static_cast<BYTE *>(mapped.pData) + y * mapped.RowPitch,
                  row_width * texel
              );
            device.immediate->Unmap(staging.Get(), 0);
          } else {
            ComPtr<ID3D10Texture2D> back, staging;
            if (FAILED(hr = chain->GetBuffer(0, IID_PPV_ARGS(&back))))
              return hr;
            D3D10_TEXTURE2D_DESC texture{};
            back->GetDesc(&texture);
            texture.Usage = D3D10_USAGE_STAGING;
            texture.BindFlags = texture.MiscFlags = 0;
            texture.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
            if (FAILED(hr = device.d10->CreateTexture2D(&texture, nullptr, &staging)))
              return hr;
            if (write)
              device.d10->UpdateSubresource(back.Get(), 0, nullptr, input.data(), row_width * texel, 0);
            device.d10->CopyResource(staging.Get(), back.Get());
            D3D10_MAPPED_TEXTURE2D mapped{};
            if (FAILED(hr = staging->Map(0, D3D10_MAP_READ, 0, &mapped)))
              return hr;
            for (UINT y = 0; y < height; y++)
              memcpy(
                  output.data() + y * row_width * texel, static_cast<BYTE *>(mapped.pData) + y * mapped.RowPitch,
                  row_width * texel
              );
            staging->Unmap(0);
          }
          expect(output == input, "color-space/metadata operation changed source bytes");
          return S_OK;
        };
        for (auto space :
             {DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709,
              DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020}) {
          step("D3D%u deferred %u format %u color space %u, %u codes", api, deferred, format, space, samples);
          UINT support = 0;
          CHECK(chain->CheckColorSpaceSupport(space, &support));
          expect(
              !(support & ~(DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT |
                            DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_OVERLAY_PRESENT)),
              "unknown support flags %#x", support
          );
          CHECK(transfer(true));
          HRESULT set = chain->SetColorSpace1(space);
          if (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)
            expect(set == S_OK, "advertised color space refused: %#lx", set);
          if (FAILED(set)) {
            CHECK(transfer(false));
            continue;
          }
          // CheckColorSpaceSupport explicitly permits setting an unadvertised
          // space; it must then report PRESENT.
          CHECK(chain->CheckColorSpaceSupport(space, &support));
          expect(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT, "set color space not reported supported");
          CHECK(chain->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_NONE, 0, nullptr));
          CHECK(transfer(false));
          if (space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
            ComPtr<IDXGIOutput> containing;
            ComPtr<IDXGIOutput6> output;
            CHECK(chain->GetContainingOutput(&containing));
            CHECK(containing.As(&output));
            DXGI_OUTPUT_DESC1 screen{};
            CHECK(output->GetDesc1(&screen));
            DXGI_HDR_METADATA_HDR10 metadata{};
            // HDR10 coordinates use units of 1/50000; only minimum luminance uses units of 1/10000 nit.
            // https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_5/ns-dxgi1_5-dxgi_hdr_metadata_hdr10
            constexpr double coordinates_per_one = 50000, minimum_units_per_nit = 10000;
            bool bounded = std::isfinite(screen.MaxLuminance) && screen.MaxLuminance >= 0 &&
                           double(screen.MaxLuminance) <= std::numeric_limits<UINT>::max() &&
                           std::isfinite(screen.MinLuminance) && screen.MinLuminance >= 0 &&
                           double(screen.MinLuminance) * minimum_units_per_nit <= std::numeric_limits<UINT>::max() &&
                           std::isfinite(screen.MaxFullFrameLuminance) && screen.MaxFullFrameLuminance >= 0;
            for (const float *primary : {screen.RedPrimary, screen.GreenPrimary, screen.BluePrimary, screen.WhitePoint})
              for (UINT axis = 0; axis < ARRAYSIZE(metadata.WhitePoint); axis++)
                bounded &= std::isfinite(primary[axis]) && primary[axis] >= 0 && primary[axis] <= 1;
            if (!expect(bounded, "output values cannot form bounded HDR metadata"))
              return verdict();
            for (UINT axis = 0; axis < ARRAYSIZE(metadata.WhitePoint); axis++) {
              metadata.RedPrimary[axis] = UINT16(std::lround(screen.RedPrimary[axis] * coordinates_per_one));
              metadata.GreenPrimary[axis] = UINT16(std::lround(screen.GreenPrimary[axis] * coordinates_per_one));
              metadata.BluePrimary[axis] = UINT16(std::lround(screen.BluePrimary[axis] * coordinates_per_one));
              metadata.WhitePoint[axis] = UINT16(std::lround(screen.WhitePoint[axis] * coordinates_per_one));
            }
            metadata.MaxMasteringLuminance = UINT(std::llround(screen.MaxLuminance));
            metadata.MinMasteringLuminance = UINT(std::llround(screen.MinLuminance * minimum_units_per_nit));
            metadata.MaxContentLightLevel =
                UINT16(std::min<double>(screen.MaxLuminance, std::numeric_limits<UINT16>::max()));
            metadata.MaxFrameAverageLightLevel =
                UINT16(std::min<double>(screen.MaxFullFrameLuminance, std::numeric_limits<UINT16>::max()));
            CHECK(chain->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(metadata), &metadata));
            CHECK(transfer(false));
            CHECK(chain->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_NONE, 0, nullptr));
            CHECK(transfer(false));
          }
          CHECK(chain->Present(0, 0));
          CHECK(device.idle());
          if (space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
            continue;
          HDC dc = GetDC(window);
          if (!expect(dc != nullptr, "GetDC"))
            return verdict();
          UINT wrong = 0;
          for (UINT i = 0; i < decoded.size(); i++) {
            COLORREF color = GetPixel(dc, i % row_width, i / row_width);
            const UINT got[] = {GetRValue(color), GetGValue(color), GetBValue(color)};
            bool same = color != CLR_INVALID;
            for (UINT c = 0; c < ARRAYSIZE(got); c++) {
              double expected = decoded[i][c];
              expected = std::isnan(expected) ? 0 : std::clamp(expected, 0.0, 1.0);
              if (space == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709)
                expected = encode(expected);
              // D3D11.3 3.2.3.6 and 3.2.3.8 permit 0.6 ULP on the integer side.
              constexpr double unorm_tolerance = 0.6;
              same &= std::abs(got[c] - expected * byte_max) <= unorm_tolerance;
            }
            if (!same && !wrong++)
              expect(false, "display code %u: %#lx", i, color);
          }
          ReleaseDC(window, dc);
          expect(!wrong, "%u color-space pixels differ", wrong);
        }
        CHECK(device.idle());
        chain.Reset();
        first.Reset();
        DestroyWindow(window);
      }
    }
  UnregisterClassA(wc.lpszClassName, wc.hInstance);
  return verdict();
}
