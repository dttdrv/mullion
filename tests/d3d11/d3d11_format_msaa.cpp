// contract: every advertised multisample color format/count/quality creates a target with distinct readable samples.
// "the format and sample count combination is supported for the device" (Microsoft Learn,
// ID3D11Device::CheckMultisampleQualityLevels). "a simple average of the samples" (D3D11.3 19.2.4).
// CPU palettes use native encodings and 19.1.2 invertibility; each sample must decode that exact palette entry.
// resolved memory follows the CPU mean and 3.2 conversion rules. quality zero/last, standard and center patterns
// run through immediate/deferred contexts; source and resolved targets have their own readback checks.
// center patterns use pixel-frequency coverage masks: sample-frequency execution is undefined there (19.2.4.1).
// unsupported counts and qualities at/past the advertised range must fail creation (19.2.3/.4).
#include "d3d11_test.hpp"
#include "../depth_formats.hpp"
#include "../format_msaa.hpp"
#include "../metal_samples.hpp"

int
main() {
  using namespace format_values;
  ComPtr<ID3DBlob> vertex = compile(format_msaa::hlsl, "vs", "vs", defines(0, 0)), pixels[3], centers[3], reads[3];
  for (UINT kind = 0; kind < std::size(pixels); kind++) {
    pixels[kind] = compile(format_msaa::hlsl, "ps", "ps", defines(kind, 0));
    centers[kind] = compile(format_msaa::hlsl, "ps_center", "ps", defines(kind, 0));
    reads[kind] = compile(format_msaa::hlsl, "cs", "cs", defines(kind, 0));
    if (!expect(pixels[kind] && centers[kind] && reads[kind], "MSAA HLSL did not compile"))
      return verdict();
  }
  if (!expect(bool(vertex), "vertex HLSL did not compile"))
    return verdict();
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &immediate
  ));
  CHECK(device->CreateDeferredContext(0, &deferred));
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps[3], center_ps[3];
  ComPtr<ID3D11ComputeShader> cs[3];
  CHECK(device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &vs));
  for (UINT kind = 0; kind < std::size(ps); kind++) {
    CHECK(
        device->CreatePixelShader(pixels[kind]->GetBufferPointer(), pixels[kind]->GetBufferSize(), nullptr, &ps[kind])
    );
    CHECK(device->CreatePixelShader(
        centers[kind]->GetBufferPointer(), centers[kind]->GetBufferSize(), nullptr, &center_ps[kind]
    ));
    CHECK(
        device->CreateComputeShader(reads[kind]->GetBufferPointer(), reads[kind]->GetBufferSize(), nullptr, &cs[kind])
    );
  }
  D3D11_RASTERIZER_DESC rd{D3D11_FILL_SOLID, D3D11_CULL_NONE};
  rd.DepthClipEnable = TRUE;
  rd.MultisampleEnable = TRUE;
  ComPtr<ID3D11RasterizerState> raster;
  CHECK(device->CreateRasterizerState(&rd, &raster));
  UINT tested = 0;
  for (const auto &f : formats()) {
    UINT caps = 0;
    if (FAILED(device->CheckFormatSupport(f.id, &caps)) || !(caps & D3D11_FORMAT_SUPPORT_RENDER_TARGET))
      continue;
    if (f.kind == Shared) {
      expect(false, "RGB9E5 advertises forbidden rendering (19.3.2)");
      continue;
    }
    auto refused = [&](UINT samples, UINT quality) {
      D3D11_TEXTURE2D_DESC td{1, 1, 1, 1, f.id, {samples, quality}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
      ComPtr<ID3D11Texture2D> target;
      HRESULT hr = device->CreateTexture2D(&td, nullptr, &target);
      return expect(
          FAILED(hr) && !target, "%s accepted invalid count %u quality %#x: %08lx", f.name.c_str(), samples, quality, hr
      );
    };
    for (UINT samples : {0u, D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT + 1u, UINT_MAX})
      refused(samples, 0);
    // section 19.6 makes MSAA, sample loads and resolve optional for these resurrected formats.
    const bool optional = f.id == DXGI_FORMAT_B5G5R5A1_UNORM || f.id == DXGI_FORMAT_B4G4R4A4_UNORM;
    const bool load = caps & D3D11_FORMAT_SUPPORT_MULTISAMPLE_LOAD;
    for (UINT samples = 1; samples <= D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT; samples++) {
      UINT quality = 0;
      CHECK(device->CheckMultisampleQualityLevels(f.id, samples, &quality));
      constexpr UINT optional_8x_bits = 128; // D3D11.3 19.2.5 makes 8x optional at 128 bits, and 4x required
      if (samples == 1 || (!optional && (samples == 4 || (samples == 8 && metal_has_8x_samples() && f.bytes * CHAR_BIT < optional_8x_bits))))
        expect(quality != 0, "%s has no required %ux quality levels", f.name.c_str(), samples);
      if (!quality) {
        refused(samples, 0);
        continue;
      }
      for (UINT q : {quality, quality + (quality != UINT_MAX)})
        if (q != D3D11_STANDARD_MULTISAMPLE_PATTERN && q != D3D11_CENTER_MULTISAMPLE_PATTERN)
          refused(samples, q);
      if (samples == 1)
        continue;
      step("%s %u samples: advertised capabilities", f.name.c_str(), samples);
      expect(caps & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET, "quality levels without multisample target support");
      if (!optional)
        expect(load, "multisample target has no required load capability");
      if (!optional && !f.shader_kind())
        expect(
            caps & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE,
            "normalized/float MSAA format has no required resolve (19.2.3)"
        );
      auto raw = format_msaa::palette(f, samples);
      auto values = format_msaa::values(f, raw);
      std::array<float, 4> clear;
      if (!expect(format_msaa::clear_value(f, raw, clear), "target seed aliases a sample's expected value"))
        return verdict();
      std::vector<UINT> qualities{0, quality - 1};
      constexpr UINT max_fixed_samples = 16; // D3D11.3 19.2.4 defines fixed patterns for 1, 2, 4, 8 and 16
      if (samples <= max_fixed_samples && std::has_single_bit(samples)) {
        qualities.push_back(D3D11_STANDARD_MULTISAMPLE_PATTERN);
        qualities.push_back(D3D11_CENTER_MULTISAMPLE_PATTERN);
      }
      std::sort(qualities.begin(), qualities.end());
      qualities.erase(std::unique(qualities.begin(), qualities.end()), qualities.end());
      for (UINT q : qualities)
        for (bool listed : {false, true}) {
          const UINT width = 3, height = 3, kind = f.shader_kind();
          const bool center = q == D3D11_CENTER_MULTISAMPLE_PATTERN;
          const bool fixed = center || q == D3D11_STANDARD_MULTISAMPLE_PATTERN;
          step("%s samples %u quality %#x %s", f.name.c_str(), samples, q, listed ? "deferred" : "immediate");
          auto context = listed ? deferred.Get() : immediate.Get();
          context->ClearState();
          D3D11_TEXTURE2D_DESC td{
              width,
              height,
              1,
              1,
              f.id,
              {samples, q},
              D3D11_USAGE_DEFAULT,
              D3D11_BIND_RENDER_TARGET | (load ? D3D11_BIND_SHADER_RESOURCE : 0u)
          };
          ComPtr<ID3D11Texture2D> target, resolved, staging;
          ComPtr<ID3D11RenderTargetView> rtv;
          ComPtr<ID3D11ShaderResourceView> srv, source_srv;
          CHECK(device->CreateTexture2D(&td, nullptr, &target));
          CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
          if (load)
            CHECK(device->CreateShaderResourceView(target.Get(), nullptr, &srv));
          context->ClearRenderTargetView(rtv.Get(), clear.data());
          D3D11_BUFFER_DESC bd{
              UINT(values.size() * sizeof(Pixel)),   D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0,
              D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, sizeof(Pixel)
          };
          D3D11_SUBRESOURCE_DATA data{values.data()};
          ComPtr<ID3D11Buffer> source, constants, result;
          CHECK(device->CreateBuffer(&bd, &data, &source));
          CHECK(device->CreateShaderResourceView(source.Get(), nullptr, &source_srv));
          UINT shape[4] = {width, height, samples, 0};
          bd = {sizeof(shape), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
          data.pSysMem = shape;
          CHECK(device->CreateBuffer(&bd, &data, &constants));
          std::vector<Pixel> poison(width * height * samples, Pixel{~0u, ~0u, ~0u, ~0u});
          bd = {UINT(poison.size() * sizeof(Pixel)),   D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, sizeof(Pixel)};
          data.pSysMem = poison.data();
          CHECK(device->CreateBuffer(&bd, &data, &result));
          ComPtr<ID3D11UnorderedAccessView> uav;
          CHECK(device->CreateUnorderedAccessView(result.Get(), nullptr, &uav));
          context->VSSetShader(vs.Get(), nullptr, 0);
          context->PSSetShader(center ? center_ps[kind].Get() : ps[kind].Get(), nullptr, 0);
          context->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
          context->PSSetShaderResources(1, 1, source_srv.GetAddressOf());
          context->RSSetState(raster.Get());
          D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
          context->RSSetViewports(1, &viewport);
          context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
          context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          for (UINT s = 0; s < (center ? samples : 1); s++) {
            shape[3] = s;
            if (center)
              context->UpdateSubresource(constants.Get(), 0, nullptr, shape, 0, 0);
            context->Draw(3, 0);
          }
          context->OMSetRenderTargets(0, nullptr, nullptr);
          if (load) {
            context->CSSetShader(cs[kind].Get(), nullptr, 0);
            context->CSSetShaderResources(0, 1, srv.GetAddressOf());
            context->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
            context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
            context->Dispatch(width, height, 1);
          }
          context->ClearState();
          if (fixed && (caps & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE)) {
            if (!expect(!kind, "integer format advertises average resolve"))
              return verdict();
            td.SampleDesc = {1, 0};
            td.BindFlags = 0;
            CHECK(device->CreateTexture2D(&td, nullptr, &resolved));
            td.Usage = D3D11_USAGE_STAGING;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            CHECK(device->CreateTexture2D(&td, nullptr, &staging));
            context->ResolveSubresource(resolved.Get(), 0, target.Get(), 0, f.id);
            context->CopyResource(staging.Get(), resolved.Get());
          }
          if (listed) {
            ComPtr<ID3D11CommandList> commands;
            CHECK(deferred->FinishCommandList(FALSE, &commands));
            immediate->ExecuteCommandList(commands.Get(), FALSE);
          }
          if (load) {
            auto got = read(device.Get(), immediate.Get(), result.Get());
            if (!expect(got.size() == poison.size() * std::tuple_size_v<Pixel>, "sample readback incomplete"))
              return verdict();
            for (UINT i = 0; i < poison.size(); i++) {
              Pixel have;
              memcpy(have.data(), got.data() + i * have.size(), sizeof(have));
              if (!expect(
                      loaded(f, raw.data() + (i % samples) * f.bytes, have), "pixel %u sample %u changed value/index",
                      i / samples, i % samples
                  ))
                return verdict();
            }
          }
          if (staging) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            CHECK(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
            bool same = true;
            for (UINT y = 0; y < height; y++)
              for (UINT x = 0; x < width; x++)
                same &= format_msaa::resolved(
                    f, raw, values, static_cast<const uint8_t *>(mapped.pData) + y * mapped.RowPitch + x * f.bytes
                );
            immediate->Unmap(staging.Get(), 0);
            if (!expect(same, "resolve did not encode the mean of every sample"))
              return verdict();
          }
          tested++;
        }
    }
  }
  expect(tested != 0, "no multisample formats were exercised");
  return verdict();
}
