// contract: unsupported formats return E_FAIL and zero bits ("returns E_FAIL if the described format does not
// exist", Microsoft Learn, ID3D11Device::CheckFormatSupport, Return value). unsupported sample combinations have
// zero quality levels ("Hardware can report 0 quality levels for a given format + sample count", D3D11.3 19.2.3).
// Wine d3d11.c:test_check_multisample_quality_levels records
// E_FAIL for 0 or more than D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT, and leaves the output alone for an invalid format.
// Direct3D 10 is reached through D3D10CreateDevice1 as Wine's d3d10core witness reaches it, with the same results.
// a positive quality count promises texture storage: "the format and sample count combination is supported for
// the device" (Microsoft Learn, ID3D11Device::CheckMultisampleQualityLevels). NV12 uses even dimensions
// (DXGI_FORMAT, NV12). D3D11.3 19.1.4 requires shader loads for integer formats and depth/stencil for D16_UNORM;
// Microsoft Learn, format support for feature level 9.3, disallows display scan-out for the two wider formats.
#include "d3d11_test.hpp"
#include <d3d11_2.h>
#include <d3d10_1.h>
#include "../depth_formats.hpp"

int
main() {
  const struct { DXGI_FORMAT format; const char *name; } formats[] = {DXGI_FORMATS};
  for (auto level : {D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_11_0}) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                          &device, nullptr, &context));
    ComPtr<ID3D11Device2> device2;
    CHECK(device.As(&device2));
    for (auto [format, name] : formats) {
      step("D3D11 level %#x, format %#x: direct and feature queries", level, format);
      UINT bits = ~0u;
      HRESULT hr = device->CheckFormatSupport(format, &bits);
      expect((hr == S_OK && bits) || (hr == E_FAIL && !bits), "hr %08lx, support %#x", hr, bits);
      D3D11_FEATURE_DATA_FORMAT_SUPPORT query{format, ~0u};
      HRESULT feature_hr = device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT, &query, sizeof(query));
      expect(feature_hr == hr && query.OutFormatSupport == bits, "direct and feature queries differ");
      D3D11_FEATURE_DATA_FORMAT_SUPPORT2 query2{format, ~0u};
      feature_hr = device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &query2, sizeof(query2));
      expect(feature_hr == hr && (SUCCEEDED(hr) || !query2.OutFormatSupport2), "support2 hr %08lx, bits %#x",
             feature_hr, query2.OutFormatSupport2);
      if (level < D3D_FEATURE_LEVEL_10_0)
        expect(!(bits & (D3D11_FORMAT_SUPPORT_BUFFER | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE_COMPARISON)),
               "feature level 9 reports buffer or comparison sampling: %#x", bits);
      if (level < D3D_FEATURE_LEVEL_10_1)
        expect(!(bits & (D3D11_FORMAT_SUPPORT_SHADER_GATHER | D3D11_FORMAT_SUPPORT_SHADER_GATHER_COMPARISON)),
               "gather below feature level 10.1: %#x", bits);
      std::string type = name;
      if ((type.ends_with("_UINT") || type.ends_with("_SINT")) && !type.starts_with("DXGI_FORMAT_D")) {
        expect(!(bits & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE), "integer format reports shader sampling");
        if (level >= D3D_FEATURE_LEVEL_10_0)
          expect(hr == S_OK && (bits & D3D11_FORMAT_SUPPORT_SHADER_LOAD), "integer format has no shader loads");
      }
      if (format == DXGI_FORMAT_D16_UNORM)
        expect(hr == S_OK && (bits & D3D11_FORMAT_SUPPORT_DEPTH_STENCIL),
               "required depth format has no depth/stencil support");
      if (format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R10G10B10A2_UNORM)
        expect(!!(bits & D3D11_FORMAT_SUPPORT_DISPLAY) == (level >= D3D_FEATURE_LEVEL_10_0),
               "display support disagrees with feature level: %#x", bits);
      if (format == DXGI_FORMAT_R8G8B8A8_UNORM)
        expect(bits & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE, "required normalized format has no shader sampling");
      for (UINT size : {0u, UINT(sizeof(query) - 1), UINT(sizeof(query) + 1)}) {
        auto before = query;
        expect(device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT, &query, size) == E_INVALIDARG,
               "format support accepts size %u", size);
        expect(query.InFormat == before.InFormat && query.OutFormatSupport == before.OutFormatSupport,
               "rejected size %u changed format support", size);
        auto before2 = query2;
        expect(device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &query2, size) == E_INVALIDARG,
               "format support2 accepts size %u", size);
        expect(query2.InFormat == before2.InFormat && query2.OutFormatSupport2 == before2.OutFormatSupport2,
               "rejected size %u changed format support2", size);
      }
    }
    for (UINT count = 0; count <= D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT + 1; count++) {
      for (auto format : {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_NV12}) {
        step("D3D11 level %#x, format %#x, samples %u", level, format, count);
        UINT quality = ~0u, with_flags = ~0u;
        HRESULT hr = device->CheckMultisampleQualityLevels(format, count, &quality);
        HRESULT want = !count || count > D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT ? E_FAIL : S_OK;
        expect(hr == want && (want == S_OK || !quality), "hr %08lx, quality %u", hr, quality);
        expect(device2->CheckMultisampleQualityLevels1(format, count, 0, &with_flags) == hr && with_flags == quality,
               "the two multisample queries differ");
        if (count > 1 && format != DXGI_FORMAT_R8G8B8A8_UNORM)
          expect(!quality, "unknown or compressed format grants multisampling");
        if (count == 1 && format == DXGI_FORMAT_R8G8B8A8_UNORM)
          expect(quality == 1, "one sample has %u quality levels", quality);
        if (count == 1 && format == DXGI_FORMAT_NV12 && quality) {
          D3D11_TEXTURE2D_DESC desc{2, 2, 1, 1, format, {count, 0}, D3D11_USAGE_DEFAULT};
          ComPtr<ID3D11Texture2D> texture;
          expect(device->CreateTexture2D(&desc, nullptr, &texture) == S_OK,
                 "single-sample quality promises storage that cannot be created");
        }
        if (count == 4 && format == DXGI_FORMAT_R8G8B8A8_UNORM && level >= D3D_FEATURE_LEVEL_10_1)
          expect(quality > 0, "required 4x MSAA has no quality levels");
        if (count > 1)
          expect(device2->CheckMultisampleQualityLevels1(format, count,
                     D3D11_CHECK_MULTISAMPLE_QUALITY_LEVELS_TILED_RESOURCE, &with_flags) == hr && !with_flags,
                 "a tiled multisampled texture has quality levels");
      }
    }
    for (auto format : {DXGI_FORMAT(DXGI_FORMAT_B4G4R4A4_UNORM + 1), DXGI_FORMAT(DXGI_FORMAT_V408 + 1),
                        DXGI_FORMAT_FORCE_UINT}) {
      step("D3D11 level %#x, invalid format %#x", level, format);
      UINT quality = ~0u, bits = ~0u;
      expect(device->CheckMultisampleQualityLevels(format, 2, &quality) == E_INVALIDARG && quality == ~0u,
             "invalid format changed quality levels: %u", quality);
      expect(device->CheckFormatSupport(format, &bits) == E_FAIL && !bits,
             "invalid format did not fail with zero bits");
    }
    expect(device->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, 1, nullptr) == E_INVALIDARG,
           "null quality output accepted");
  }
  HMODULE dll = LoadLibraryA("d3d10_1.dll");
  auto create = reinterpret_cast<decltype(&D3D10CreateDevice1)>((void *)GetProcAddress(dll, "D3D10CreateDevice1"));
  if (!expect(create != nullptr, "D3D10CreateDevice1 is unavailable"))
    return verdict();
  for (auto level : {D3D10_FEATURE_LEVEL_10_0, D3D10_FEATURE_LEVEL_10_1}) {
    ComPtr<ID3D10Device1> device;
    CHECK(create(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, level, D3D10_1_SDK_VERSION, &device));
    for (auto [format, name] : formats) {
      step("D3D10 level %#x, format %#x", level, format);
      UINT bits = ~0u;
      HRESULT hr = device->CheckFormatSupport(format, &bits);
      expect(hr == S_OK || (hr == E_FAIL && !bits), "hr %08lx, support %#x", hr, bits);
      std::string type = name;
      if ((type.ends_with("_UINT") || type.ends_with("_SINT")) && !type.starts_with("DXGI_FORMAT_D"))
        expect(hr == S_OK && (bits & D3D10_FORMAT_SUPPORT_SHADER_LOAD), "integer format has no shader loads");
      if (format == DXGI_FORMAT_D16_UNORM)
        expect(hr == S_OK && (bits & D3D10_FORMAT_SUPPORT_DEPTH_STENCIL),
               "required depth format has no depth/stencil support");
      if (level == D3D10_FEATURE_LEVEL_10_0)
        expect(!(bits & D3D10_FORMAT_SUPPORT_SHADER_GATHER), "gather on feature level 10.0: %#x", bits);
    }
    for (UINT count = 0; count <= D3D10_MAX_MULTISAMPLE_SAMPLE_COUNT + 1; count++) {
      for (auto format : {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_NV12}) {
        step("D3D10 level %#x, format %#x, samples %u", level, format, count);
        UINT quality = ~0u;
        HRESULT hr = device->CheckMultisampleQualityLevels(format, count, &quality);
        HRESULT want = !count || count > D3D10_MAX_MULTISAMPLE_SAMPLE_COUNT ? E_FAIL : S_OK;
        expect(hr == want && (want == S_OK || !quality), "hr %08lx, quality %u", hr, quality);
        if (count > 1 && format != DXGI_FORMAT_R8G8B8A8_UNORM)
          expect(!quality, "unknown or compressed format grants multisampling");
        if (count == 1 && format == DXGI_FORMAT_R8G8B8A8_UNORM)
          expect(quality == 1, "one sample has %u quality levels", quality);
        if (count == 1 && format == DXGI_FORMAT_NV12 && quality) {
          D3D10_TEXTURE2D_DESC desc{2, 2, 1, 1, format, {count, 0}, D3D10_USAGE_DEFAULT};
          ComPtr<ID3D10Texture2D> texture;
          expect(device->CreateTexture2D(&desc, nullptr, &texture) == S_OK,
                 "single-sample quality promises storage that cannot be created");
        }
        if (count == 4 && format == DXGI_FORMAT_R8G8B8A8_UNORM && level == D3D10_FEATURE_LEVEL_10_1)
          expect(quality > 0, "required 4x MSAA has no quality levels");
      }
    }
    step("D3D10 invalid format and output");
    UINT quality = ~0u;
    expect(device->CheckMultisampleQualityLevels(DXGI_FORMAT_FORCE_UINT, 2, &quality) == E_INVALIDARG && quality == ~0u,
           "invalid format changed quality levels: %u", quality);
    expect(device->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, 1, nullptr) == E_INVALIDARG,
           "null quality output accepted");
  }
  FreeLibrary(dll);
  return verdict();
}
