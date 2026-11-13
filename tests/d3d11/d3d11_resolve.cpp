// contract: resolves preserve pixels and select the explicit source slice, destination mip/slice and typed format.
// "Only a whole Subresource can be resolved, so both Subresources must be the same dimensions." (D3D11.3 5.6.6).
// "The algorithm to resolve multiple samples to one pixel is implementation dependent." all samples of each pixel
// therefore hold the same value; adjacent pixels and slices differ. "If both Resource are prestructured+typeless,
// then they must be equal formats, and the ResolveFormat may be any format compatable with the typeless format
// and supporting resolve." (5.6.6). all four typed/typeless pairs run for each advertised resolve format/count.
// CPU encoding uses normalized endpoints (3.2.3.6/3.2.3.8) and the floating exponent/mantissa layouts (3.1.3/5/6).
#include "d3d11_test.hpp"
#include "../copy_formats.hpp"
#include <d3d10_1.h>
#include <d3d11_1.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <climits>

static const char hlsl[] = R"hlsl(
cbuffer Params : register(b0) { uint salt; };
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 position : SV_Position) : SV_Target {
  uint x = uint(position.x), y = uint(position.y);
  return float4((x / (salt + 1) + salt) % 2, (y / (salt + 1) + salt) % 2,
                ((x + y + salt) / 2) % 2, (x / (salt + 1) + 2 * y + salt) % 2);
}
)hlsl";

int
main() {
  auto vs = compile(hlsl, "vs", "vs"), ps = compile(hlsl, "ps", "ps");
  if (!vs || !ps)
    return 1;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                         &device, nullptr, &immediate));
  CHECK(device->CreateDeferredContext(0, &deferred));
  ComPtr<ID3D11VertexShader> vertex;
  ComPtr<ID3D11PixelShader> pixel;
  CHECK(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex));
  CHECK(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel));
  ComPtr<ID3D11Device1> device1;
  ComPtr<ID3D11DeviceContext1> immediate1;
  CHECK(device.As(&device1));
  CHECK(immediate.As(&immediate1));
  ComPtr<ID3DDeviceContextState> compatibility;
  CHECK(device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D10Device1),
                                         nullptr, &compatibility));
  ComPtr<ID3D10Device1> legacy;
  CHECK(device.As(&legacy));
  using Row = std::array<UINT, 4>;
  auto constants = buffer(device.Get(), sizeof(Row), D3D11_BIND_CONSTANT_BUFFER);
  if (!expect(!!constants, "resolve constants were not created"))
    return verdict();
  constexpr UINT width = 7, height = 5, layers = 3, mips = 3;
  constexpr unsigned char sentinel = 0x5a;
  UINT exercised = 0;
  for (const auto &format : copy_formats) {
    const bool floating = strstr(format.name, "_FLOAT"), normalized = strstr(format.name, "_UNORM");
    if (format.depth || format.unit != 1 || (!floating && !normalized) || strstr(format.name, "TYPELESS") ||
        strchr(format.family, 'X'))
      continue;
    UINT support = 0;
    const HRESULT support_hr = device->CheckFormatSupport(format.id, &support);
    if (!expect(support_hr == S_OK || (support_hr == E_FAIL && !support),
                "format %s support: HRESULT %#lx, bits %#x", format.name, support_hr, support))
      return verdict();
    if (support_hr == E_FAIL)
      continue;
    if (!(support & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE))
      continue;
    struct Channel { UINT component, bits; };
    std::vector<Channel> channels;
    constexpr char components[] = "RGBA";
    for (const char *at = format.family; *at;) {
      const char channel = *at++;
      char *end;
      const UINT bits = UINT(strtoul(at, &end, 10));
      const char *component = strchr(components, channel);
      if (!expect(component && end != at && bits <= sizeof(UINT) * CHAR_BIT,
                  "unsupported SDK channel layout %s", format.family))
        return verdict();
      channels.push_back({UINT(component - components), bits});
      at = end;
    }
    auto typeless = std::find_if(std::begin(copy_formats), std::end(copy_formats), [&](const auto &candidate) {
      return !strcmp(candidate.family, format.family) && strstr(candidate.name, "_TYPELESS") && !candidate.depth;
    });
    for (UINT samples = 2; samples <= D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT; samples *= 2) {
      UINT qualities = 0;
      CHECK(device->CheckMultisampleQualityLevels(format.id, samples, &qualities));
      if (!qualities)
        continue;
      for (UINT types = 0; types < 4; types++) {
        if (types && typeless == std::end(copy_formats))
          continue;
        for (UINT mip : {0u, mips - 1}) {
          D3D11_TEXTURE2D_DESC src_desc{width, height, 1, layers, types & 1 ? typeless->id : format.id,
                                         {samples, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
          D3D11_TEXTURE2D_DESC dst_desc{width << mip, height << mip, mips, layers,
                                         types & 2 ? typeless->id : format.id, {1, 0}, D3D11_USAGE_DEFAULT};
          ComPtr<ID3D11Texture2D> source, dest, staging;
          CHECK(device->CreateTexture2D(&src_desc, nullptr, &source));
          CHECK(device->CreateTexture2D(&dst_desc, nullptr, &dest));
          dst_desc.Usage = D3D11_USAGE_STAGING;
          dst_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
          CHECK(device->CreateTexture2D(&dst_desc, nullptr, &staging));
          for (UINT layer = 0; layer < layers; layer++) {
            D3D11_RENDER_TARGET_VIEW_DESC desc{format.id, D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY};
            desc.Texture2DMSArray = {layer, 1};
            ComPtr<ID3D11RenderTargetView> view;
            CHECK(device->CreateRenderTargetView(source.Get(), &desc, &view));
            const Row params{layer};
            immediate->UpdateSubresource(constants.Get(), 0, nullptr, params.data(), 0, 0);
            immediate->ClearState();
            immediate->VSSetShader(vertex.Get(), nullptr, 0);
            immediate->PSSetShader(pixel.Get(), nullptr, 0);
            immediate->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
            immediate->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            const D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
            immediate->RSSetViewports(1, &viewport);
            immediate->OMSetRenderTargets(1, view.GetAddressOf(), nullptr);
            immediate->Draw(3, 0);
          }
          immediate->ClearState();
          for (UINT route = 0; route < 3; route++)
            for (UINT src_layer : {0u, layers - 1}) {
              const UINT dst_layer = layers - 1 - src_layer;
              const UINT dst_sub = D3D11CalcSubresource(mip, dst_layer, mips);
              step("resolve %s samples %u types %u mip %u slice %u -> %u route %u", format.name,
                   samples, types, mip, src_layer, dst_layer, route);
              for (UINT sub = 0; sub < mips * layers; sub++) {
                const UINT w = std::max(1u, dst_desc.Width >> (sub % mips));
                const UINT h = std::max(1u, dst_desc.Height >> (sub % mips));
                std::vector<unsigned char> bytes(w * h * format.bytes, sentinel);
                immediate->UpdateSubresource(dest.Get(), sub, nullptr, bytes.data(), w * format.bytes, 0);
              }
              if (route == 2) {
                ComPtr<ID3D10Resource> src10, dst10;
                CHECK(source.As(&src10));
                CHECK(dest.As(&dst10));
                ComPtr<ID3DDeviceContextState> previous;
                immediate1->SwapDeviceContextState(compatibility.Get(), &previous);
                legacy->ResolveSubresource(dst10.Get(), dst_sub, src10.Get(), src_layer, format.id);
                immediate1->SwapDeviceContextState(previous.Get(), nullptr);
              } else {
                (route ? deferred.Get() : immediate.Get())->ResolveSubresource(dest.Get(), dst_sub, source.Get(),
                                                                                src_layer, format.id);
                if (route) {
                  ComPtr<ID3D11CommandList> commands;
                  CHECK(deferred->FinishCommandList(FALSE, &commands));
                  immediate->ExecuteCommandList(commands.Get(), FALSE);
                }
              }
              immediate->CopyResource(staging.Get(), dest.Get());
              for (UINT sub = 0; sub < layers * mips; sub++) {
                const UINT w = std::max(1u, dst_desc.Width >> (sub % mips));
                const UINT h = std::max(1u, dst_desc.Height >> (sub % mips));
                D3D11_MAPPED_SUBRESOURCE mapped;
                CHECK(immediate->Map(staging.Get(), sub, D3D11_MAP_READ, 0, &mapped));
                bool ok = true;
                for (UINT y = 0; y < h && ok; y++)
                  for (UINT x = 0; x < w && ok; x++) {
                    std::vector<unsigned char> want(format.bytes, sentinel);
                    if (sub == dst_sub) {
                      std::fill(want.begin(), want.end(), 0);
                      const UINT values[] = {(x / (src_layer + 1) + src_layer) % 2,
                                             (y / (src_layer + 1) + src_layer) % 2,
                                             ((x + y + src_layer) / 2) % 2,
                                             (x / (src_layer + 1) + 2 * y + src_layer) % 2};
                      UINT at = 0;
                      for (auto channel : channels) {
                        UINT64 encoded = values[channel.component] ? (UINT64(1) << channel.bits) - 1 : 0;
                        if (floating && values[channel.component]) {
                          const UINT exponent_bits = channel.bits == 32 ? 8 : 5; // D3D11.3 3.1.3, 3.1.5, 3.1.6
                          const UINT sign_bits = channel.bits >= 16 ? 1 : 0;
                          encoded = ((UINT64(1) << (exponent_bits - 1)) - 1) <<
                                    (channel.bits - exponent_bits - sign_bits);
                        }
                        for (UINT bit = 0; bit < channel.bits; bit++, at++)
                          want[at / CHAR_BIT] |= ((encoded >> bit) & 1) << (at % CHAR_BIT);
                      }
                    }
                    const auto got = static_cast<unsigned char *>(mapped.pData) + y * mapped.RowPitch + x * format.bytes;
                    ok = expect(!memcmp(got, want.data(), want.size()), "sub %u pixel (%u,%u) differs: bytes %02x %02x, want %02x %02x",
                                sub, x, y, got[0], format.bytes > 1 ? got[1] : 0, want[0], format.bytes > 1 ? want[1] : 0);
                  }
                immediate->Unmap(staging.Get(), sub);
                if (!ok)
                  return verdict();
              }
              exercised++;
            }
        }
      }
    }
  }
  expect(exercised != 0, "no advertised resolve configuration was exercised");
  return verdict();
}
