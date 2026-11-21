// contract: MetalFX upscaling through Mullion's D3D11 context extension (IMTLD3D11ContextExt, which Mullion's NGX
// bridge records DLSS with, src/nvngx/nvngx.cpp) writes an image whose content is its input's: a uniform image with no
// motion comes out that color in every texel, within one unit of the 8-bit output, as tests/d3d12/d3d12_metalfx.cpp
// asks of D3D12. the motion vectors are given in each format the extension takes, typed or typeless, in a texture a
// pass writes them to, as a render target or with unordered access. they are zero bits, which is no motion however a
// format's bits are read. the extension reads them through a view of a float format, which Metal makes only of a
// texture with its format view (MTLTextureUsagePixelFormatView): its validation refuses the view otherwise
// (tests/run.py --validate). the content size is 0, which is the color's. each upscale writes a new output, all zero
// before, so one that writes nothing is seen
#include "d3d11_test.hpp"
#include "../../src/d3d11/d3d11_interfaces.hpp"
#include <cstdlib>

int
main() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                          nullptr, &context));
  ComPtr<IMTLD3D11ContextExt1> ext;
  step("the context's MetalFX extension");
  CHECK(context.As(&ext));
  BOOL supported = FALSE;
  CHECK(ext->CheckFeatureSupport(MTL_FEATURE_METALFX_TEMPORAL_SCALER, &supported, sizeof(supported)));
  if (!supported) {
    printf("skipped: no MetalFX temporal scaler on this device\n");
    return 77;
  }

  const UINT width = 256, height = 144, out_width = width * 2, out_height = height * 2;
  // a texture of `format` with `texel` (`bytes` of it) in each texel
  auto texture = [&](DXGI_FORMAT format, UINT w, UINT h, UINT bind, const void *texel, UINT bytes) {
    std::vector<BYTE> data(size_t(w) * h * bytes);
    for (size_t i = 0; i < data.size(); i += bytes)
      memcpy(&data[i], texel, bytes);
    D3D11_TEXTURE2D_DESC desc{w, h, 1, 1, format, {1, 0}, D3D11_USAGE_DEFAULT, bind};
    D3D11_SUBRESOURCE_DATA initial{data.data(), w * bytes};
    ComPtr<ID3D11Texture2D> out;
    device->CreateTexture2D(&desc, &initial, &out);
    return out;
  };
  const BYTE rgba[4] = {200, 100, 50, 255}, zero[16] = {};
  const float half = 0.5f, one = 1.0f;
  auto color = texture(DXGI_FORMAT_R8G8B8A8_UNORM, width, height, D3D11_BIND_SHADER_RESOURCE, rgba, 4);
  auto depth = texture(DXGI_FORMAT_R32_FLOAT, width, height, D3D11_BIND_SHADER_RESOURCE, &half, 4);
  auto exposure = texture(DXGI_FORMAT_R32_FLOAT, 1, 1, D3D11_BIND_SHADER_RESOURCE, &one, 4);
  D3D11_TEXTURE2D_DESC staging_desc{out_width, out_height, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0},
                                    D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ};
  ComPtr<ID3D11Texture2D> staging;
  CHECK(color && depth && exposure ? S_OK : E_FAIL);
  CHECK(device->CreateTexture2D(&staging_desc, nullptr, &staging));

  const struct {
    DXGI_FORMAT format;
    const char *name;
    UINT bytes;
  } formats[] = {
      {DXGI_FORMAT_R16G16_UINT, "R16G16_UINT", 4},
      {DXGI_FORMAT_R16G16_TYPELESS, "R16G16_TYPELESS", 4},
      {DXGI_FORMAT_R16G16_FLOAT, "R16G16_FLOAT", 4},
      {DXGI_FORMAT_R16G16_UNORM, "R16G16_UNORM", 4},
      {DXGI_FORMAT_R16G16_SNORM, "R16G16_SNORM", 4},
      {DXGI_FORMAT_R16G16_SINT, "R16G16_SINT", 4},
      {DXGI_FORMAT_R32G32_TYPELESS, "R32G32_TYPELESS", 8},
      {DXGI_FORMAT_R32G32_FLOAT, "R32G32_FLOAT", 8},
      {DXGI_FORMAT_R32G32_UINT, "R32G32_UINT", 8},
      {DXGI_FORMAT_R32G32_SINT, "R32G32_SINT", 8},
      {DXGI_FORMAT_R16G16B16A16_TYPELESS, "R16G16B16A16_TYPELESS", 8},
      {DXGI_FORMAT_R16G16B16A16_FLOAT, "R16G16B16A16_FLOAT", 8},
      {DXGI_FORMAT_R16G16B16A16_UINT, "R16G16B16A16_UINT", 8},
      {DXGI_FORMAT_R16G16B16A16_UNORM, "R16G16B16A16_UNORM", 8},
      {DXGI_FORMAT_R16G16B16A16_SNORM, "R16G16B16A16_SNORM", 8},
      {DXGI_FORMAT_R16G16B16A16_SINT, "R16G16B16A16_SINT", 8},
  };
  for (auto &[format, name, bytes] : formats) {
    for (UINT bind : {D3D11_BIND_UNORDERED_ACCESS, D3D11_BIND_RENDER_TARGET}) {
      step("motion vectors in %s, %s", name, bind == D3D11_BIND_RENDER_TARGET ? "a render target" : "unordered access");
      auto motion = texture(format, width, height, D3D11_BIND_SHADER_RESOURCE | bind, zero, bytes);
      auto output = texture(DXGI_FORMAT_R8G8B8A8_UNORM, out_width, out_height, D3D11_BIND_UNORDERED_ACCESS, zero, 4);
      if (!expect(motion && output, "a texture was refused"))
        continue;
      MTL_TEMPORAL_UPSCALE_D3D11_DESC desc{};
      desc.InReset = TRUE;
      desc.Color = color.Get();
      desc.Depth = depth.Get();
      desc.MotionVector = motion.Get();
      desc.ExposureTexture = exposure.Get();
      desc.Output = output.Get();
      desc.MotionVectorScaleX = desc.MotionVectorScaleY = 1;
      desc.PreExposure = 1;
      ext->TemporalUpscale(&desc);
      context->CopyResource(staging.Get(), output.Get());
      D3D11_MAPPED_SUBRESOURCE mapped;
      if (!expect(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "the output was not read"))
        continue;
      UINT apart = 0;
      for (UINT y = 0; y < out_height; y++)
        for (UINT x = 0; x < out_width; x++)
          for (UINT c = 0; c < 3; c++)
            apart += std::abs(int(((const BYTE *)mapped.pData)[y * mapped.RowPitch + x * 4 + c]) - int(rgba[c])) > 1;
      context->Unmap(staging.Get(), 0);
      expect(!apart, "%u of %u color channels are more than one unit from the input's", apart,
             out_width * out_height * 3);
    }
  }
  return verdict();
}
