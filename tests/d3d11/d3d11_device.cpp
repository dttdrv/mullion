// contract: what a Direct3D 11 device answers about itself, as the documentation has it.
// - ID3D11Device::CheckMultisampleQualityLevels: "if this method returns S_OK and pNumQualityLevels receives a positive
//   value, then the format and sample count combination is supported for the device": a texture of every format and
//   count it grants can be created; one sample is every creatable format's; a tiled multisampled texture is none's.
// - IDXGIDevice1::SetMaximumFrameLatency: "the value defaults to 3, but can range from 1 to 16. A value of 0 will
//   reset latency to the default"; outside that it is DXGI_ERROR_INVALID_CALL.
// - the device has no counters (D3D11_COUNTER_INFO: LastDeviceDependentCounter 0), so none can be created
//   (ID3D11Device::CreateCounter: E_INVALIDARG for an out-of-range counter), and keeps the exception mode it is given.
// - a query is ID3D11Asynchronous; the queries that are predicates are ID3D11Predicate, and only those are made by
//   CreatePredicate.
// - a staging texture mapped with D3D11_MAP_FLAG_DO_NOT_WAIT while a copy to it is pending gives
//   DXGI_ERROR_WAS_STILL_DRAWING until the copy is done, and then its data: asking again is enough
//   (ID3D11DeviceContext::Map).
#include "d3d11_test.hpp"
#include <d3d11_2.h>
#include <dxgi1_2.h>

int
main() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context));
  unsigned wrong = 0, checks = 0;
  auto expect = [&](bool ok, const std::string &what) {
    checks++;
    if (!ok && wrong++ < 16)
      printf("%s\n", what.c_str());
  };

  // multisample quality levels, over every format the header names
  unsigned granted = 0;
  for (UINT f = DXGI_FORMAT_UNKNOWN + 1; f <= DXGI_FORMAT_B4G4R4A4_UNORM; f++) {
    auto format = DXGI_FORMAT(f);
    UINT support = 0;
    if (FAILED(device->CheckFormatSupport(format, &support)) || !(support & D3D11_FORMAT_SUPPORT_TEXTURE2D))
      continue;
    // bound as what the format can be multisampled as
    UINT bind = support & D3D11_FORMAT_SUPPORT_DEPTH_STENCIL ? D3D11_BIND_DEPTH_STENCIL
                : support & D3D11_FORMAT_SUPPORT_RENDER_TARGET ? D3D11_BIND_RENDER_TARGET
                                                               : 0;
    for (UINT samples : {1u, 2u, 4u, 8u}) {
      UINT levels = ~0u;
      HRESULT hr = device->CheckMultisampleQualityLevels(format, samples, &levels);
      if (FAILED(hr) || !levels || (samples > 1 && !bind))
        continue;
      granted++;
      D3D11_TEXTURE2D_DESC desc{16, 16, 1, 1, format, {samples, 0}, D3D11_USAGE_DEFAULT, samples > 1 ? bind : D3D11_BIND_SHADER_RESOURCE};
      if (samples == 1 && !(support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE) && !(support & D3D11_FORMAT_SUPPORT_SHADER_LOAD))
        desc.BindFlags = bind;
      ComPtr<ID3D11Texture2D> texture;
      expect(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)),
             "format " + std::to_string(f) + " has quality levels for " + std::to_string(samples) + " samples, and no texture of them");
    }
  }
  expect(granted > 0, "no format has quality levels");
  {
    UINT levels = ~0u;
    expect(device->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, 4, nullptr) == E_INVALIDARG,
           "quality levels are written to no pointer");
    expect(SUCCEEDED(device->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, 0, &levels)) && levels == 0,
           "no samples have quality levels");
    ComPtr<ID3D11Device2> device2;
    if (SUCCEEDED(device.As(&device2))) {
      levels = ~0u;
      expect(SUCCEEDED(device2->CheckMultisampleQualityLevels1(
                 DXGI_FORMAT_R8G8B8A8_UNORM, 4, D3D11_CHECK_MULTISAMPLE_QUALITY_LEVELS_TILED_RESOURCE, &levels
             )) && levels == 0,
             "a tiled multisampled texture has quality levels");
    }
  }

  // frame latency
  {
    ComPtr<IDXGIDevice1> dxgi;
    CHECK(device.As(&dxgi));
    const UINT documented_default = 3, most = 16;
    UINT latency = 0;
    expect(SUCCEEDED(dxgi->GetMaximumFrameLatency(&latency)) && latency == documented_default, "the frame latency does not default to 3");
    expect(SUCCEEDED(dxgi->SetMaximumFrameLatency(most)) && SUCCEEDED(dxgi->GetMaximumFrameLatency(&latency)) && latency == most,
           "the largest frame latency is not kept");
    expect(dxgi->SetMaximumFrameLatency(most + 1) == DXGI_ERROR_INVALID_CALL &&
               SUCCEEDED(dxgi->GetMaximumFrameLatency(&latency)) && latency == most,
           "a frame latency past the largest is taken");
    expect(SUCCEEDED(dxgi->SetMaximumFrameLatency(0)) && SUCCEEDED(dxgi->GetMaximumFrameLatency(&latency)) && latency == documented_default,
           "a frame latency of 0 is not the default again");
    expect(dxgi->GetMaximumFrameLatency(nullptr) == DXGI_ERROR_INVALID_CALL, "the frame latency is written to no pointer");
  }

  // counters and the exception mode
  {
    D3D11_COUNTER_INFO info{D3D11_COUNTER_DEVICE_DEPENDENT_0, 1, 1};
    device->CheckCounterInfo(&info);
    expect(info.LastDeviceDependentCounter == 0 && info.NumSimultaneousCounters == 0, "the device says it has counters");
    D3D11_COUNTER_DESC counter_desc{D3D11_COUNTER_DEVICE_DEPENDENT_0};
    ComPtr<ID3D11Counter> counter;
    expect(device->CreateCounter(&counter_desc, &counter) == E_INVALIDARG && !counter, "a counter the device does not have is created");
    expect(device->SetExceptionMode(D3D11_RAISE_FLAG_DRIVER_INTERNAL_ERROR) == S_OK &&
               device->GetExceptionMode() == D3D11_RAISE_FLAG_DRIVER_INTERNAL_ERROR && device->SetExceptionMode(0) == S_OK &&
               device->GetExceptionMode() == 0,
           "the exception mode set is not the one got");
  }

  // queries and predicates
  {
    D3D11_QUERY_DESC occlusion{D3D11_QUERY_OCCLUSION}, is_predicate{D3D11_QUERY_OCCLUSION_PREDICATE};
    ComPtr<ID3D11Query> query;
    ComPtr<ID3D11Predicate> predicate, none;
    ComPtr<ID3D11Asynchronous> asynchronous;
    CHECK(device->CreateQuery(&occlusion, &query));
    expect(SUCCEEDED(query.As(&asynchronous)), "a query is not ID3D11Asynchronous");
    expect(FAILED(query.As(&none)), "an occlusion query is a predicate");
    expect(device->CreatePredicate(&occlusion, &none) == E_INVALIDARG, "an occlusion query is made a predicate");
    expect(SUCCEEDED(device->CreatePredicate(&is_predicate, &predicate)) && predicate, "no predicate of an occlusion predicate query");
    ComPtr<ID3D11Query> as_query;
    expect(predicate && SUCCEEDED(predicate.As(&as_query)), "a predicate is not a query");
  }

  // a staging texture whose copy is pending
  {
    const UINT size = 64, texel = 0x11223344;
    std::vector<UINT> texels(size * size, texel);
    D3D11_TEXTURE2D_DESC desc{size, size, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
    D3D11_SUBRESOURCE_DATA initial{texels.data(), size * 4};
    ComPtr<ID3D11Texture2D> source, staging;
    CHECK(device->CreateTexture2D(&desc, &initial, &source));
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(device->CreateTexture2D(&desc, nullptr, &staging));
    context->CopyResource(staging.Get(), source.Get());
    // asked again without a flush, for no longer than a slow copy takes
    const UINT attempts = 5000;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = DXGI_ERROR_WAS_STILL_DRAWING;
    UINT asked = 0;
    for (; asked < attempts && hr == DXGI_ERROR_WAS_STILL_DRAWING; asked++) {
      hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
      if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
        Sleep(1);
    }
    expect(hr == S_OK, "a staging texture asked for without waiting is never ready");
    if (hr == S_OK) {
      expect(*(const UINT *)mapped.pData == texel && *((const UINT *)((const char *)mapped.pData + (size - 1) * mapped.RowPitch) + size - 1) == texel,
             "a staging texture mapped without waiting does not hold the copy");
      context->Unmap(staging.Get(), 0);
    }
  }
  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
