// contract: MetalFX upscaling through Mullion's D3D11 context extension (IMTLD3D11ContextExt, which Mullion's NGX
// bridge records DLSS with, src/nvngx/nvngx.cpp) reads motion vectors as DLSS reads its inputs, through their format
// ("DLSS uses formatted reads so should handle most input buffer formats", DLSS Programming Guide 3.3), and writes an
// image whose content is its input's, as tests/d3d12/d3d12_metalfx.cpp asks of D3D12. a uniform image with no motion
// comes out that color in every texel, within one unit of the 8-bit output, with motion vectors in each format that
// holds floats: float, typeless (taken as the float format of its width), normalized, and four 8-bit normalized
// channels as Marvel's Spider-Man 2 gives them; integer ones hold no float to read and are refused, which writes
// nothing. each is in a texture a pass writes it to, as a render target or with unordered access, or one only its
// initial data fills, as a copy would. they are zero bits, which is no motion however a format's bits are read. one
// read through a view of another format needs Metal's format view (MTLTextureUsagePixelFormatView): its validation
// refuses the view otherwise (tests/run.py --validate). motion vectors at the output resolution (DLSS without
// NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) are taken too. and a light square on dark that moves between a frame in reset
// and the next comes out the same image whether its motion is given as float pixels, as the same motion at the output
// resolution, or as signed normalized values in units of the input's size with the scale that makes them pixels; half
// the motion gives another image, the test's own check that the image shows the motion. the content size is 0, which is the color's. each upscale writes a new
// output, all zero before, so one that writes nothing is seen
#include "d3d11_test.hpp"
#include "../../src/d3d11/d3d11_interfaces.hpp"
#include <cmath>
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
  // a texture of `format` whose texel is `inside` (`bytes` of it) in the square of `side` texels at x, y and `texel`
  // elsewhere
  auto texture = [&](DXGI_FORMAT format, UINT w, UINT h, UINT bind, const void *texel, UINT bytes,
                     const void *inside = nullptr, UINT x = 0, UINT y = 0, UINT side = 0) {
    std::vector<BYTE> data(size_t(w) * h * bytes);
    for (UINT ty = 0; ty < h; ty++)
      for (UINT tx = 0; tx < w; tx++)
        memcpy(&data[(size_t(ty) * w + tx) * bytes],
               inside && tx - x < side && ty - y < side ? inside : texel, bytes);
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

  // the output's texels, tightly, of an upscale of `frame` with `motion` and the scale that makes its values pixels
  auto upscaled = [&](ID3D11Texture2D *frame, ID3D11Texture2D *motion, BOOL reset, float scale_x, float scale_y,
                      BOOL at_output = FALSE) {
    std::vector<BYTE> texels;
    auto output = texture(DXGI_FORMAT_R8G8B8A8_UNORM, out_width, out_height, D3D11_BIND_UNORDERED_ACCESS, zero, 4);
    if (!expect(frame && motion && output, "a texture was refused"))
      return texels;
    MTL_TEMPORAL_UPSCALE_D3D11_DESC desc{};
    desc.InReset = reset;
    desc.Color = frame;
    desc.Depth = depth.Get();
    desc.MotionVector = motion;
    desc.MotionVectorInDisplayRes = at_output;
    desc.ExposureTexture = exposure.Get();
    desc.Output = output.Get();
    desc.MotionVectorScaleX = scale_x;
    desc.MotionVectorScaleY = scale_y;
    desc.PreExposure = 1;
    ext->TemporalUpscale(&desc);
    context->CopyResource(staging.Get(), output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (!expect(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "the output was not read"))
      return texels;
    for (UINT y = 0; y < out_height; y++) {
      auto row = (const BYTE *)mapped.pData + y * mapped.RowPitch;
      texels.insert(texels.end(), row, row + out_width * 4);
    }
    context->Unmap(staging.Get(), 0);
    return texels;
  };
  // how many color channels of an image are more than one unit from `want`'s
  auto off = [&](const std::vector<BYTE> &texels, const BYTE *want) {
    UINT count = 0;
    for (size_t i = 0; i < texels.size(); i++)
      count += i % 4 != 3 && std::abs(int(texels[i]) - int(want[i % 4])) > 1;
    return count;
  };

  const struct {
    DXGI_FORMAT format;
    const char *name;
    UINT bytes;
    bool taken;
  } formats[] = {
      {DXGI_FORMAT_R16G16_UINT, "R16G16_UINT", 4, true},
      {DXGI_FORMAT_R16G16_TYPELESS, "R16G16_TYPELESS", 4, true},
      {DXGI_FORMAT_R16G16_FLOAT, "R16G16_FLOAT", 4, true},
      {DXGI_FORMAT_R16G16_UNORM, "R16G16_UNORM", 4, true},
      {DXGI_FORMAT_R16G16_SNORM, "R16G16_SNORM", 4, true},
      {DXGI_FORMAT_R16G16_SINT, "R16G16_SINT", 4, false},
      {DXGI_FORMAT_R32G32_TYPELESS, "R32G32_TYPELESS", 8, true},
      {DXGI_FORMAT_R32G32_FLOAT, "R32G32_FLOAT", 8, true},
      {DXGI_FORMAT_R32G32_UINT, "R32G32_UINT", 8, true},
      {DXGI_FORMAT_R32G32_SINT, "R32G32_SINT", 8, false},
      {DXGI_FORMAT_R16G16B16A16_TYPELESS, "R16G16B16A16_TYPELESS", 8, true},
      {DXGI_FORMAT_R16G16B16A16_FLOAT, "R16G16B16A16_FLOAT", 8, true},
      {DXGI_FORMAT_R16G16B16A16_UINT, "R16G16B16A16_UINT", 8, true},
      {DXGI_FORMAT_R16G16B16A16_UNORM, "R16G16B16A16_UNORM", 8, true},
      {DXGI_FORMAT_R16G16B16A16_SNORM, "R16G16B16A16_SNORM", 8, true},
      {DXGI_FORMAT_R16G16B16A16_SINT, "R16G16B16A16_SINT", 8, false},
      {DXGI_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM", 4, true},
  };
  for (auto &[format, name, bytes, taken] : formats) {
    for (UINT bind : {UINT(D3D11_BIND_UNORDERED_ACCESS), UINT(D3D11_BIND_RENDER_TARGET), 0u}) {
      step("motion vectors in %s, %s", name,
           bind == D3D11_BIND_RENDER_TARGET ? "a render target" : bind ? "unordered access" : "a shader resource alone");
      auto motion = texture(format, width, height, D3D11_BIND_SHADER_RESOURCE | bind, zero, bytes);
      auto image = upscaled(color.Get(), motion.Get(), TRUE, 1, 1);
      if (image.empty())
        continue;
      if (taken)
        expect(!off(image, rgba), "%u of %u color channels are more than one unit from the input's", off(image, rgba),
               out_width * out_height * 3);
      else
        expect(!off(image, zero), "integer motion vectors were taken: %u of %u color channels were written",
               off(image, zero), out_width * out_height * 3);
    }
  }

  step("a frame of one color with motion vectors at the output resolution");
  auto display = texture(DXGI_FORMAT_R32G32_FLOAT, out_width, out_height, D3D11_BIND_SHADER_RESOURCE, zero, 8);
  auto image = upscaled(color.Get(), display.Get(), TRUE, 1, 1, TRUE);
  expect(!image.empty() && !off(image, rgba), "%u of %u color channels are more than one unit from the input's",
         off(image, rgba), out_width * out_height * 3);

  // a light square on dark moves `by` input pixels between a frame in reset and the next; its motion vectors point
  // back to where it was
  const BYTE dark[4] = {64, 128, 192, 255}, light[4] = {200, 20, 100, 255};
  const UINT side = 32, x = 64, y = 56, by = 12;
  auto start = texture(DXGI_FORMAT_R8G8B8A8_UNORM, width, height, D3D11_BIND_SHADER_RESOURCE, dark, 4, light, x, y, side);
  auto moved = texture(DXGI_FORMAT_R8G8B8A8_UNORM, width, height, D3D11_BIND_SHADER_RESOURCE, dark, 4, light, x + by, y,
                       side);
  const float there[2] = {-float(by), 0}, halved[2] = {-float(by) / 2, 0},
              there_output[2] = {-float(by) * out_width / width, 0};
  const short there_snorm[2] = {short(std::lround(-float(by) / width * 32767)), 0};
  // both frames' vectors in one format, so one scaler upscales both
  auto moving = [&](DXGI_FORMAT format, const void *motion, UINT bytes, float scale_x, float scale_y,
                    BOOL at_output = FALSE) {
    const UINT times = at_output ? out_width / width : 1;
    auto still = texture(format, width * times, height * times, D3D11_BIND_SHADER_RESOURCE, zero, bytes);
    auto vectors = texture(format, width * times, height * times, D3D11_BIND_SHADER_RESOURCE, zero, bytes, motion,
                           (x + by) * times, y * times, side * times);
    upscaled(start.Get(), still.Get(), TRUE, 1, 1, at_output);
    return upscaled(moved.Get(), vectors.Get(), FALSE, scale_x, scale_y, at_output);
  };
  step("a square that moves %u pixels, its motion given as float pixels", by);
  auto floats = moving(DXGI_FORMAT_R32G32_FLOAT, there, 8, 1, 1);
  step("the same motion given at the output resolution");
  auto at_output = moving(DXGI_FORMAT_R32G32_FLOAT, there_output, 8, 1, 1, TRUE);
  step("half that motion given as float pixels");
  auto other = moving(DXGI_FORMAT_R32G32_FLOAT, halved, 8, 1, 1);
  step("the same motion given as signed normalized values in units of the input's size");
  auto normal = moving(DXGI_FORMAT_R16G16_SNORM, there_snorm, 4, float(width), float(height));
  if (expect(!floats.empty() && !at_output.empty() && !other.empty() && !normal.empty(), "no image")) {
    auto apart = [](const std::vector<BYTE> &a, const std::vector<BYTE> &b) {
      UINT count = 0;
      for (size_t i = 0; i < a.size() && i < b.size(); i++)
        count += i % 4 != 3 && std::abs(int(a[i]) - int(b[i])) > 1;
      return count;
    };
    expect(apart(floats, other), "half the motion gives the same image: the motion vectors do not show in it");
    expect(!apart(floats, at_output), "%u channels differ between the motion given at the input resolution and at the output's",
           apart(floats, at_output));
    expect(!apart(floats, normal), "%u channels differ between the motion given as floats and as normalized values",
           apart(floats, normal));
  }
  return verdict();
}
