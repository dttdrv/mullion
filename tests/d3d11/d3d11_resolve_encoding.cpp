// contract: a resolve averages its samples in the encoding its format names, whichever view of the texture drew
// them, and completes after the application releases its textures. "If both Resource are prestructured+typeless,
// then they must be equal formats, and the ResolveFormat may be any format compatable with the typeless format and
// supporting resolve." (D3D11.3 5.6.6). two of each pixel's four samples hold 1 in red and two hold 0, which UNORM
// and sRGB store as the same bits: their mean as UNORM is half the range, and as sRGB the encoding of 0.5
// (3.2.3.8), within the 0.6 ULP a conversion may be off (3.2.3.6). Wine's test_multisample_resolve has the same of
// Windows for a triangle that covers half of a pixel.
#include "d3d11_test.hpp"
#include <cmath>

static const char hlsl[] = R"hlsl(
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(uint sample : SV_SampleIndex) : SV_Target { return float4(sample < 2, 0, 1, 1); }
)hlsl";

int
main() {
  auto vs = compile(hlsl, "vs", "vs"), ps = compile(hlsl, "ps", "ps");
  if (!vs || !ps)
    return 1;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                         nullptr, &immediate));
  ComPtr<ID3D11VertexShader> vertex;
  ComPtr<ID3D11PixelShader> pixel;
  CHECK(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex));
  CHECK(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel));
  constexpr UINT size = 16, samples = 4;
  UINT qualities = 0;
  CHECK(device->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, samples, &qualities));
  if (!expect(qualities != 0, "no %u sample R8G8B8A8_UNORM, which feature level 10_1 requires", samples))
    return verdict();
  const DXGI_FORMAT encodings[] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};
  // held: the textures outlive the read. copied: released after a copy of the destination. bare: released with
  // nothing reading the destination. several rounds, so that memory a released texture had is used again
  enum { held, copied, bare };
  for (UINT round = 0; round < 8; round++)
    for (UINT mode : {held, copied, bare})
      for (auto drawn : encodings)
        for (auto resolved : encodings) {
          step("round %u mode %u: drawn as format %u, resolved as format %u", round, mode, drawn, resolved);
          D3D11_TEXTURE2D_DESC desc{size, size, 1, 1, DXGI_FORMAT_R8G8B8A8_TYPELESS, {samples, 0},
                                    D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
          ComPtr<ID3D11Texture2D> source, dest, staging;
          CHECK(device->CreateTexture2D(&desc, nullptr, &source));
          desc.SampleDesc.Count = 1;
          desc.BindFlags = 0;
          CHECK(device->CreateTexture2D(&desc, nullptr, &dest));
          desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
          desc.Usage = D3D11_USAGE_STAGING;
          desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
          CHECK(device->CreateTexture2D(&desc, nullptr, &staging));
          D3D11_RENDER_TARGET_VIEW_DESC view_desc{drawn, D3D11_RTV_DIMENSION_TEXTURE2DMS};
          ComPtr<ID3D11RenderTargetView> view;
          CHECK(device->CreateRenderTargetView(source.Get(), &view_desc, &view));
          immediate->ClearState();
          immediate->VSSetShader(vertex.Get(), nullptr, 0);
          immediate->PSSetShader(pixel.Get(), nullptr, 0);
          immediate->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          const D3D11_VIEWPORT viewport{0, 0, float(size), float(size), 0, 1};
          immediate->RSSetViewports(1, &viewport);
          immediate->OMSetRenderTargets(1, view.GetAddressOf(), nullptr);
          immediate->Draw(3, 0);
          immediate->ResolveSubresource(dest.Get(), 0, source.Get(), 0, resolved);
          if (mode != bare)
            immediate->CopyResource(staging.Get(), dest.Get());
          if (mode != held) {
            immediate->ClearState();
            view.Reset();
            source.Reset();
            dest.Reset();
          }
          if (mode == bare) {
            immediate->Flush();
            continue;
          }
          D3D11_MAPPED_SUBRESOURCE mapped;
          CHECK(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
          // 3.2.3.8's encoding of a linear 0.5, which is above its linear segment
          const double half = resolved == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? 1.055 * std::pow(0.5, 1 / 2.4) - 0.055 : 0.5;
          const double red = 255 * half;
          bool ok = true;
          for (UINT y = 0; y < size && ok; y++)
            for (UINT x = 0; x < size && ok; x++) {
              auto got = static_cast<unsigned char *>(mapped.pData) + y * mapped.RowPitch + x * 4;
              ok = expect(std::fabs(got[0] - red) <= 0.6 && got[1] == 0 && got[2] == 0xff && got[3] == 0xff,
                          "pixel (%u,%u) is %02x %02x %02x %02x, want red within 0.6 of %.1f, then 00 ff ff", x, y,
                          got[0], got[1], got[2], got[3], red);
            }
          immediate->Unmap(staging.Get(), 0);
        }
  // the queued resolves have run once a later command's result is back
  auto word = buffer(device.Get(), sizeof(uint32_t), 0, 0x1234abcd);
  if (!expect(!!word, "the last buffer was not created"))
    return verdict();
  auto words = read(device.Get(), immediate.Get(), word.Get());
  expect(words.size() == 1 && words[0] == 0x1234abcd, "the last read did not return its word");
  return verdict();
}
