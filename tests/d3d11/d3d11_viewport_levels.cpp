// contract: a viewport whose top left corner is a fraction below zero draws the pixel at its bottom right corner before
// feature level 11_0 and not from 11_0 on, and every pixel above and left of that row and column at each level. this
// is what Windows does in Wine's test_negative_viewports (d3d11.c): viewports at x and y -0.4 and -1/128, 640 x 480
// on a 640 x 480 target, a quad over all of it; "for feature levels greater than or equal to 11_0, a negative top
// left corner shifts the bottom right corner by a whole integer. it seems that floor() is used to round viewport
// corners to integers". from 11_0 a viewport scissors to the pixels its floored extents hold (D3D11.3 15.6.1). the
// target is cleared white, and every pixel the contract names is read.
#include "d3d11_test.hpp"

static const char hlsl[] = R"hlsl(
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps() : SV_Target { return float4(0, 1, 0, 1); }
)hlsl";

int
main() {
  auto vs_code = compile(hlsl, "vs", "vs_4_0"), ps_code = compile(hlsl, "ps", "ps_4_0");
  if (!expect(vs_code && ps_code, "HLSL did not compile"))
    return verdict();
  const UINT width = 640, height = 480;
  const uint32_t white = 0xffffffff, green = 0xff00ff00;
  const float clear[4] = {1, 1, 1, 1};
  for (auto level : {D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1}) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                            nullptr, &context));
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    CHECK(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs));
    CHECK(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &ps));
    D3D11_TEXTURE2D_DESC desc{width, height, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                              D3D11_BIND_RENDER_TARGET};
    ComPtr<ID3D11Texture2D> target, staging;
    ComPtr<ID3D11RenderTargetView> rtv;
    CHECK(device->CreateTexture2D(&desc, nullptr, &target));
    CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(device->CreateTexture2D(&desc, nullptr, &staging));
    const bool floored = level >= D3D_FEATURE_LEVEL_11_0;
    for (float corner : {-0.4f, -1.0f / 128}) {
      step("feature level 0x%x, a viewport at (%g, %g)", level, corner, corner);
      D3D11_VIEWPORT viewport{corner, corner, (float)width, (float)height, 0, 1};
      context->ClearRenderTargetView(rtv.Get(), clear);
      context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
      context->RSSetViewports(1, &viewport);
      context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      context->VSSetShader(vs.Get(), nullptr, 0);
      context->PSSetShader(ps.Get(), nullptr, 0);
      context->Draw(3, 0);
      context->CopyResource(staging.Get(), target.Get());
      D3D11_MAPPED_SUBRESOURCE mapped;
      CHECK(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
      auto pixel = [&](UINT x, UINT y) {
        uint32_t value;
        memcpy(&value, static_cast<const char *>(mapped.pData) + y * mapped.RowPitch + x * 4, 4);
        return value;
      };
      UINT wrong = 0;
      for (UINT y = 0; y + 1 < height; y++)
        for (UINT x = 0; x + 1 < width; x++)
          if (pixel(x, y) != green && wrong++ < 4)
            expect(false, "(%u, %u) is 0x%08x, want 0x%08x", x, y, pixel(x, y), green);
      expect(!wrong, "%u pixels above and left of the last row and column are not drawn", wrong);
      auto want = floored ? white : green;
      expect(pixel(width - 1, height - 1) == want, "(%u, %u) is 0x%08x, want 0x%08x", width - 1, height - 1,
             pixel(width - 1, height - 1), want);
      context->Unmap(staging.Get(), 0);
    }
  }
  return verdict();
}
