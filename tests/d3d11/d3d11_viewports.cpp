// contract: see ../viewports.hpp, here through Direct3D 11, where the rasterizer state turns the scissor test on:
// every case with it on, and with it off, when a pixel needs only its viewport ("if disabled, any pixel on the
// RenderTarget(s) can be drawn to", D3D11.3 15.7).
#include "d3d11_test.hpp"
#include "../viewports.hpp"

using namespace viewports;

int
main() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context));
  auto vs_code = compile(hlsl, "vs", "vs"), gs_code = compile(hlsl, "gs", "gs"), ps_code = compile(hlsl, "ps", "ps");
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11GeometryShader> gs;
  ComPtr<ID3D11PixelShader> ps;
  if (!vs_code || !gs_code || !ps_code) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  CHECK(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs));
  CHECK(device->CreateGeometryShader(gs_code->GetBufferPointer(), gs_code->GetBufferSize(), nullptr, &gs));
  CHECK(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &ps));

  D3D11_TEXTURE2D_DESC texture_desc{width, height, 1, 1, DXGI_FORMAT_R32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
  ComPtr<ID3D11Texture2D> target, target_read;
  ComPtr<ID3D11RenderTargetView> rtv;
  CHECK(device->CreateTexture2D(&texture_desc, nullptr, &target));
  CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
  texture_desc.Usage = D3D11_USAGE_STAGING;
  texture_desc.BindFlags = 0;
  texture_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  CHECK(device->CreateTexture2D(&texture_desc, nullptr, &target_read));
  struct Constants {
    UINT index;
    float value;
    UINT padding[2];
  };
  D3D11_BUFFER_DESC constants_desc{sizeof(Constants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
  ComPtr<ID3D11Buffer> constants;
  CHECK(device->CreateBuffer(&constants_desc, nullptr, &constants));
  ComPtr<ID3D11RasterizerState> raster[2];
  for (BOOL scissoring : {FALSE, TRUE}) {
    D3D11_RASTERIZER_DESC desc{D3D11_FILL_SOLID, D3D11_CULL_NONE};
    desc.DepthClipEnable = TRUE;
    desc.ScissorEnable = scissoring;
    CHECK(device->CreateRasterizerState(&desc, &raster[scissoring]));
  }
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->VSSetShader(vs.Get(), nullptr, 0);
  context->PSSetShader(ps.Get(), nullptr, 0);
  context->GSSetConstantBuffers(0, 1, constants.GetAddressOf());
  context->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
  context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);

  const auto all = cases();
  unsigned failures = 0, drawn = 0;
  for (bool scissoring : {true, false})
    for (size_t c = 0; c < all.size(); c++) {
      const float clear[4] = {};
      context->ClearRenderTargetView(rtv.Get(), clear);
      context->RSSetState(raster[scissoring].Get());
      for (size_t n = 0; n < all[c].size(); n++) {
        auto &draw = all[c][n];
        if (draw.viewports) {
          std::vector<D3D11_VIEWPORT> set;
          for (auto part : *draw.viewports) {
            auto r = rectangle(part);
            set.push_back({(float)r.left, (float)r.top, float(r.right - r.left), float(r.bottom - r.top), 0, 1});
          }
          context->RSSetViewports(set.size(), set.data());
        }
        if (draw.scissors) {
          std::vector<D3D11_RECT> set;
          for (auto part : *draw.scissors) {
            auto r = rectangle(part);
            set.push_back({r.left, r.top, r.right, r.bottom});
          }
          context->RSSetScissorRects(set.size(), set.data());
        }
        Constants of{draw.index == plain ? 0 : draw.index, float(n + 1)};
        context->UpdateSubresource(constants.Get(), 0, nullptr, &of, 0, 0);
        context->GSSetShader(draw.index == plain ? nullptr : gs.Get(), nullptr, 0);
        context->Draw(3, 0);
      }
      context->CopyResource(target_read.Get(), target.Get());
      D3D11_MAPPED_SUBRESOURCE mapped;
      CHECK(context->Map(target_read.Get(), 0, D3D11_MAP_READ, 0, &mapped));
      auto want = expected(all[c], scissoring);
      drawn += std::count_if(want.begin(), want.end(), [](float v) { return v != 0; });
      failures += check(scissoring ? "" : "without the scissor test, ", c, want, (const char *)mapped.pData, mapped.RowPitch);
      context->Unmap(target_read.Get(), 0);
    }
  printf("%s: %u wrong pixels in %zu cases, %u pixels drawn\n", failures ? "failed" : "passed", failures, 2 * all.size(), drawn);
  return failures != 0;
}
