// contract: uint UAV clears copy channel bits; float and ClearView clears apply the view format's conversions.
// "copying the lower ni bits from each array element i to the corresponding channel" (D3D11.3 5.2.3.2).
// FLOAT to UNORM/SNORM maps NaN to zero and clamps infinities, with "0.6f ULP" tolerance on the integer side
// (3.2.3.4/3.2.3.6). sRGB conversion uses 3.2.3.8's transfer function, except alpha remains linear (19.1.4).
// ClearView integer inputs have "fractions rounded to zero, out of range/INF values clamped to target range,
// NaN to 0" (5.2.3.3). viewport/scissor do not restrict full-view clears (5.2.3.1).
// 3.2.2: "Round-to-zero must be used during conversion to another float format"; finite overflow gives
// "the maximum representable (appropriately signed) value results, NOT including signed infinity".
// 3.1.5/6 define five exponent bits and preserved denorms for 16/11/10-bit formats; unsigned negatives clamp to 0.
// ClearView with null rectangles covers the full view: "NULL means clear the entire view." (5.2.3.3); two
// rectangles that tile the view clear every texel of it as well.
// SDK format names supply channel order and widths; every physical channel is checked over every texel.
#include "d3d11_test.hpp"
#include "../copy_formats.hpp"
#include "../clear_values.hpp"
#include <d3d11_1.h>

int
main() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                         &device, nullptr, &immediate));
  CHECK(device->CreateDeferredContext(0, &deferred));
  ComPtr<ID3D11DeviceContext1> contexts[2];
  CHECK(immediate.As(&contexts[0]));
  CHECK(deferred.As(&contexts[1]));
  D3D11_RASTERIZER_DESC raster{D3D11_FILL_SOLID, D3D11_CULL_NONE};
  raster.ScissorEnable = TRUE;
  ComPtr<ID3D11RasterizerState> scissored;
  CHECK(device->CreateRasterizerState(&raster, &scissored));
  constexpr UINT width = 3, height = 2;
  const UINT bit_values[] = {0, 1, UINT_MAX, UINT(1) << (sizeof(UINT) * CHAR_BIT - 1), 0xa53c19e7};
  UINT exercised = 0, buffer_views = 0;
  for (const auto &format : copy_formats) {
    const auto kind = clear_values::kind(format);
    const bool snorm = kind.snorm, unorm = kind.unorm, sint = kind.sint, uint = kind.uint, floating = kind.floating;
    if (format.depth || format.unit != 1 || strstr(format.name, "TYPELESS") || strchr(format.family, 'X') ||
        !(snorm || unorm || sint || uint || floating))
      continue;
    UINT support = 0;
    const HRESULT support_hr = device->CheckFormatSupport(format.id, &support);
    if (!expect(support_hr == S_OK || (support_hr == E_FAIL && !support),
                "format %s support: HRESULT %#lx, bits %#x", format.name, support_hr, support))
      return verdict();
    if (support_hr == E_FAIL)
      continue;
    const auto channels = clear_values::channels(format);
    if (channels.empty())
      return verdict();
    const auto values = clear_values::values(kind, channels);
    for (UINT method = 0; method < 6; method++) {
      const bool unordered = method == 0 || method == 1 || method == 2;
      const bool clear_view = method == 2 || method >= 4;
      if (!(support & (unordered ? D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW :
                                   D3D11_FORMAT_SUPPORT_RENDER_TARGET)))
        continue;
      if ((sint || uint) && !clear_view && method != 0)
        continue;
      D3D11_TEXTURE2D_DESC desc{width, height, 1, 1, format.id, {1, 0}, D3D11_USAGE_DEFAULT,
                                 unordered ? D3D11_BIND_UNORDERED_ACCESS : D3D11_BIND_RENDER_TARGET};
      ComPtr<ID3D11Texture2D> target, staging;
      CHECK(device->CreateTexture2D(&desc, nullptr, &target));
      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      CHECK(device->CreateTexture2D(&desc, nullptr, &staging));
      ComPtr<ID3D11UnorderedAccessView> uav;
      ComPtr<ID3D11RenderTargetView> rtv;
      ComPtr<ID3D11View> view;
      if (unordered) {
        CHECK(device->CreateUnorderedAccessView(target.Get(), nullptr, &uav));
        view = uav;
      } else {
        CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
        view = rtv;
      }
      for (UINT route = 0; route < std::size(contexts); route++)
        for (UINT index = 0; index < (method ? std::size(values) : std::size(bit_values)); index++) {
          std::array<float, 4> color;
          std::array<UINT, 4> bits;
          for (UINT c = 0; c < color.size(); c++) {
            color[c] = values[(index + c) % std::size(values)];
            bits[c] = bit_values[(index + c) % std::size(bit_values)];
          }
          step("clear %s method %u route %u values %u", format.name, method, route, index);
          auto context = contexts[route].Get();
          const D3D11_VIEWPORT viewport{0, 0, 0, 0, 0, 1};
          const D3D11_RECT empty{};
          context->RSSetViewports(1, &viewport);
          context->RSSetScissorRects(1, &empty);
          context->RSSetState(scissored.Get());
          if (method == 0)
            context->ClearUnorderedAccessViewUint(uav.Get(), bits.data());
          else if (method == 1)
            context->ClearUnorderedAccessViewFloat(uav.Get(), color.data());
          else if (method == 5) {
            const D3D11_RECT halves[] = {{0, 0, LONG(width / 2), LONG(height)}, {LONG(width / 2), 0, LONG(width), LONG(height)}};
            context->ClearView(view.Get(), color.data(), halves, UINT(std::size(halves)));
          } else if (clear_view)
            context->ClearView(view.Get(), color.data(), nullptr, 0);
          else
            context->ClearRenderTargetView(rtv.Get(), color.data());
          context->CopyResource(staging.Get(), target.Get());
          if (route) {
            ComPtr<ID3D11CommandList> commands;
            CHECK(deferred->FinishCommandList(FALSE, &commands));
            immediate->ExecuteCommandList(commands.Get(), FALSE);
          }
          D3D11_MAPPED_SUBRESOURCE mapped;
          CHECK(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
          bool ok = true;
          for (UINT y = 0; y < height && ok; y++)
            for (UINT x = 0; x < width && ok; x++) {
              auto pixel = static_cast<unsigned char *>(mapped.pData) + y * mapped.RowPitch + x * format.bytes;
              if (method) {
                ok &= clear_values::check(pixel, kind, channels, color, x, y);
                continue;
              }
              UINT at = 0;
              for (auto channel : channels) {
                UINT64 encoded = 0;
                for (UINT bit = 0; bit < channel.bits; bit++, at++)
                  encoded |= UINT64((pixel[at / CHAR_BIT] >> (at % CHAR_BIT)) & 1) << bit;
                const UINT64 mask = (UINT64(1) << channel.bits) - 1;
                ok &= expect(encoded == (bits[channel.component] & mask), "pixel (%u,%u) channel %u bits %#llx", x, y,
                             channel.component, encoded);
              }
            }
          immediate->Unmap(staging.Get(), 0);
          exercised++;
        }
    }
    // a typed view of a buffer takes the same bits, in its own elements only (Wine's
    // test_clear_buffer_unordered_access_view clears an R8G8B8A8_SINT one with values past a signed byte)
    if ((support & D3D11_FORMAT_SUPPORT_BUFFER) && (support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW)) {
      constexpr UINT elements = 16, first = 5, count = 6;
      const std::vector<unsigned char> seed(elements * format.bytes, 0x5a);
      D3D11_BUFFER_DESC desc{UINT(seed.size()), D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS};
      const D3D11_SUBRESOURCE_DATA data{seed.data()};
      ComPtr<ID3D11Buffer> target, staging;
      CHECK(device->CreateBuffer(&desc, &data, &target));
      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      CHECK(device->CreateBuffer(&desc, nullptr, &staging));
      D3D11_UNORDERED_ACCESS_VIEW_DESC view_desc{format.id, D3D11_UAV_DIMENSION_BUFFER};
      view_desc.Buffer = {first, count, 0};
      ComPtr<ID3D11UnorderedAccessView> uav;
      CHECK(device->CreateUnorderedAccessView(target.Get(), &view_desc, &uav));
      for (UINT route = 0; route < std::size(contexts); route++)
        for (UINT index = 0; index < std::size(bit_values); index++) {
          std::array<UINT, 4> bits;
          for (UINT c = 0; c < bits.size(); c++)
            bits[c] = bit_values[(index + c) % std::size(bit_values)];
          step("clear %s buffer view route %u values %u", format.name, route, index);
          auto context = contexts[route].Get();
          context->UpdateSubresource(target.Get(), 0, nullptr, seed.data(), 0, 0);
          context->ClearUnorderedAccessViewUint(uav.Get(), bits.data());
          context->CopyResource(staging.Get(), target.Get());
          if (route) {
            ComPtr<ID3D11CommandList> commands;
            CHECK(deferred->FinishCommandList(FALSE, &commands));
            immediate->ExecuteCommandList(commands.Get(), FALSE);
          }
          D3D11_MAPPED_SUBRESOURCE mapped;
          CHECK(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
          bool ok = true;
          for (UINT element = 0; element < elements && ok; element++) {
            auto pixel = static_cast<unsigned char *>(mapped.pData) + element * format.bytes;
            if (element < first || element >= first + count) {
              for (UINT byte = 0; byte < format.bytes && ok; byte++)
                ok &= expect(pixel[byte] == 0x5a, "element %u outside the view, byte %u is %#x", element, byte, pixel[byte]);
              continue;
            }
            UINT at = 0;
            for (auto channel : channels) {
              UINT64 encoded = 0;
              for (UINT bit = 0; bit < channel.bits; bit++, at++)
                encoded |= UINT64((pixel[at / CHAR_BIT] >> (at % CHAR_BIT)) & 1) << bit;
              const UINT64 mask = (UINT64(1) << channel.bits) - 1;
              ok &= expect(encoded == (bits[channel.component] & mask), "element %u channel %u bits %#llx", element,
                           channel.component, encoded);
            }
          }
          immediate->Unmap(staging.Get(), 0);
          buffer_views++;
        }
    }
  }
  expect(exercised != 0, "no clear conversion was exercised");
  expect(buffer_views != 0, "no typed buffer view was cleared");
  return verdict();
}
