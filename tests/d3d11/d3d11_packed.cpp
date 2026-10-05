// contract: the sub-sampled formats (D3D11.3 19.4): "The G component is taken from the currently addressed pixel
// value. The R component is taken from the current pixel value for even x resource addresses, and from the previous
// ('-1'th) x dimension pixel value for odd x resource addresses. The B component is taken from the next ('+1'th) x
// dimension pixel value for even x resource addresses, and from the current pixel value for odd x". two pixels are
// four bytes, in the order the format's name gives them, and alpha, which they do not have, reads 1.
// a compute shader loads every texel of a texture of numbered bytes. a format the device says it does not have
// (ID3D11Device::CheckFormatSupport: "E_FAIL if the described format is not supported") cannot be made a texture of.
#include "d3d11_test.hpp"
#include <cmath>

static const char hlsl[] = R"hlsl(
Texture2D<float4> packed : register(t0);
RWStructuredBuffer<float4> loaded : register(u0);
[numthreads(1, 1, 1)] void cs() {
  for (uint y = 0; y < HEIGHT; y++)
    for (uint x = 0; x < WIDTH; x++)
      loaded[y * WIDTH + x] = packed.Load(int3(x, y, 0));
}
)hlsl";

int
main() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context));
  const UINT width = 6, height = 3;
  auto code = compile(hlsl, "cs", "cs", {"WIDTH=" + std::to_string(width), "HEIGHT=" + std::to_string(height)});
  if (!code) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D11ComputeShader> cs;
  CHECK(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs));
  D3D11_BUFFER_DESC out_desc{width * height * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                             D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16};
  ComPtr<ID3D11Buffer> out;
  ComPtr<ID3D11UnorderedAccessView> uav;
  CHECK(device->CreateBuffer(&out_desc, nullptr, &out));
  CHECK(device->CreateUnorderedAccessView(out.Get(), nullptr, &uav));
  context->CSSetShader(cs.Get(), nullptr, 0);
  context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);

  // which of a pair's four bytes is R, the even pixel's G, B and the odd pixel's G
  const struct {
    DXGI_FORMAT format;
    const char *name;
    UINT r, g_even, b, g_odd;
  } formats[] = {{DXGI_FORMAT_R8G8_B8G8_UNORM, "R8G8_B8G8_UNORM", 0, 1, 2, 3},
                 {DXGI_FORMAT_G8R8_G8B8_UNORM, "G8R8_G8B8_UNORM", 1, 0, 3, 2}};
  std::vector<uint8_t> bytes(width * 2 * height);
  for (UINT i = 0; i < bytes.size(); i++)
    bytes[i] = 17 + 23 * i;
  unsigned wrong = 0, without = 0;
  for (auto &f : formats) {
    D3D11_TEXTURE2D_DESC desc{width, height, 1, 1, f.format, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
    D3D11_SUBRESOURCE_DATA data{bytes.data(), width * 2};
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    UINT support = 0;
    if (FAILED(device->CheckFormatSupport(f.format, &support)) || !(support & D3D11_FORMAT_SUPPORT_TEXTURE2D)) {
      if (SUCCEEDED(device->CreateTexture2D(&desc, &data, &texture)) && wrong++ < 4)
        printf("%s: a texture of a format the device does not have\n", f.name);
      without++;
      continue;
    }
    CHECK(device->CreateTexture2D(&desc, &data, &texture));
    CHECK(device->CreateShaderResourceView(texture.Get(), nullptr, &view));
    context->CSSetShaderResources(0, 1, view.GetAddressOf());
    context->Dispatch(1, 1, 1);
    auto got = read(device.Get(), context.Get(), out.Get());
    for (UINT y = 0; y < height; y++)
      for (UINT x = 0; x < width; x++) {
        const uint8_t *pair = &bytes[(y * width + (x & ~1u)) * 2];
        const long want[4] = {pair[f.r], pair[x & 1 ? f.g_odd : f.g_even], pair[f.b], UINT8_MAX};
        long have[4];
        for (UINT c = 0; c < 4; c++) {
          float channel;
          memcpy(&channel, &got[(y * width + x) * 4 + c], sizeof(channel));
          have[c] = std::lround(channel * UINT8_MAX);
        }
        if (memcmp(have, want, sizeof(want)) && wrong++ < 4)
          printf("%s: texel %u,%u is %ld %ld %ld %ld, want %ld %ld %ld %ld\n", f.name, x, y, have[0], have[1], have[2],
                 have[3], want[0], want[1], want[2], want[3]);
      }
  }
  printf("%s: %u wrong texels of %zu formats, %u of which the device does not have\n", wrong ? "failed" : "passed", wrong,
         std::size(formats), without);
  return wrong != 0;
}
