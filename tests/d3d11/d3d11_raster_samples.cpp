// contract: D3D11.3 19.2.4: "Standard sample patterns are defined for the following sample counts:
// 1(trivial), 2, 4, 8, 16." 22.4.22: "If sampleIndex is out of bounds a zero vector is returned."
// ../raster_samples.hpp supplies independent per-sample triangle/point/clip coverage and ordered query values.
// immediate and deferred contexts read every sample through Texture2DMS, without resolving away sample identity.
#include "d3d11_test.hpp"
#include <d3d11_3.h>
#include "../raster_samples.hpp"
#include <cstddef>
#include <map>

int main() {
  using namespace raster_samples;
  struct Shaders {
    ComPtr<ID3DBlob> vs, clip, sample, index, modifier, varying, centroid, perspective, perspective_centroid, depth,
        query, output, forced, uav, cs, single_cs;
  };
  std::map<unsigned, Shaders> shaders;
  if (!raster::reference_contract())
    return 1;
  step("compile all sample shaders before device creation");
  for (unsigned count : {1u, 2u, 4u, 8u, 16u}) {
    auto &s = shaders[count];
    std::vector<std::string> defs = {"WIDTH=" + std::to_string(width), "COUNT=" + std::to_string(count),
                                     "STORAGE_COUNT=" + std::to_string(count)};
    s.vs = compile(raster::hlsl, "vs", "vs", {"DISTANCES=0"});
    s.clip = compile(raster::hlsl, "vs", "vs", {"DISTANCES=1"});
    s.sample = compile(raster_samples::hlsl, "ps_sample", "ps", defs);
    s.index = compile(raster_samples::hlsl, "ps_index", "ps", defs);
    s.modifier = compile(raster_samples::hlsl, "ps_modifier", "ps", defs);
    s.varying = compile(raster_samples::hlsl, "ps_varying", "ps", defs);
    s.centroid = compile(raster_samples::hlsl, "ps_centroid", "ps", defs);
    s.depth = compile(raster_samples::hlsl, "ps_depth", "ps", defs);
    defs.push_back("PERSPECTIVE=1");
    s.perspective = compile(raster_samples::hlsl, "ps_varying", "ps", defs);
    s.perspective_centroid = compile(raster_samples::hlsl, "ps_centroid", "ps", defs);
    defs.pop_back();
    s.query = compile(raster_samples::hlsl, "ps_query", "ps", defs);
    s.output = compile(raster_samples::hlsl, "ps_export", "ps", defs);
    s.cs = compile(raster_samples::hlsl, "cs", "cs", defs);
    s.uav = compile(raster_samples::hlsl, "ps_uav", "ps", defs);
    s.forced = compile(raster_samples::hlsl, "ps_forced", "ps", defs);
    defs.back() = "STORAGE_COUNT=1";
    s.single_cs = compile(raster_samples::hlsl, "cs", "cs", defs);
    if (!s.vs || !s.clip || !s.sample || !s.index || !s.modifier || !s.varying || !s.centroid || !s.perspective ||
        !s.perspective_centroid || !s.depth || !s.query || !s.output || !s.forced || !s.uav || !s.cs || !s.single_cs)
      return 1;
  }
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, UINT(std::size(levels)),
                          D3D11_SDK_VERSION, &device, nullptr, &immediate));
  ComPtr<ID3D11Device1> device1;
  CHECK(device.As(&device1));
  CHECK(device->CreateDeferredContext(0, &deferred));
  // DXGI_SAMPLE_DESC: "The valid range is between zero and one less than the level returned" by the query.
  // https://learn.microsoft.com/en-us/windows/win32/api/dxgicommon/ns-dxgicommon-dxgi_sample_desc
  step("sample-count boundaries and quality-level resource validation");
  for (UINT count : {0u, 1u, 2u, 3u, 4u, 8u, 16u, UINT(D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT),
                     UINT(D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT + 1), UINT_MAX}) {
    UINT levels = 0;
    HRESULT query = device->CheckMultisampleQualityLevels(DXGI_FORMAT_R32G32B32A32_FLOAT, count, &levels);
    bool supported = SUCCEEDED(query) && levels != 0;
    if (!count || count > D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT)
      expect(!supported, "invalid sample count%u advertised %u qualities", count, levels);
    std::vector<UINT> qualities = {0, levels};
    if (levels > 1)
      qualities.push_back(levels - 1);
    for (UINT quality : qualities) {
      D3D11_TEXTURE2D_DESC td{width,
                              height,
                              1,
                              1,
                              DXGI_FORMAT_R32G32B32A32_FLOAT,
                              {count, quality},
                              D3D11_USAGE_DEFAULT,
                              D3D11_BIND_RENDER_TARGET};
      ComPtr<ID3D11Texture2D> texture;
      HRESULT hr = device->CreateTexture2D(&td, nullptr, &texture);
      bool valid = count && count <= D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT && supported && quality < levels;
      expect(valid ? hr == S_OK && texture : FAILED(hr) && !texture,
             "sample count%u quality%u of%u, query%#lx create%#lx", count, quality, levels, query, hr);
    }
  }
  D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
  CHECK(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options)));
  if (device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1) {
    expect(options.UAVOnlyRenderingForcedSampleCount, "FL11_1 does not report UAV-only forced sample counts");
    expect(options.MultisampleRTVWithForcedSampleCountOne, "FL11_1 does not report forced1 MSAA RTV support");
  }
  D3D11_DEPTH_STENCIL_DESC depth_desc{};
  depth_desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
  ComPtr<ID3D11DepthStencilState> no_depth;
  CHECK(device->CreateDepthStencilState(&depth_desc, &no_depth));
  D3D11_INPUT_ELEMENT_DESC elements[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vertex, position), D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"DISTANCE", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vertex, distance), D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"DISTANCE", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vertex, distance) + sizeof(Color),
       D3D11_INPUT_PER_VERTEX_DATA, 0}};
  D3D11_BUFFER_DESC bd{sizeof(Vertex) * Triangle, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER};
  ComPtr<ID3D11Buffer> vb;
  CHECK(device->CreateBuffer(&bd, nullptr, &vb));
  const auto geometry = raster_samples::cases();
  for (auto &[count, s] : shaders)
    for (bool centered : {false, true})
      for (auto mode : {RenderMode::Regular, RenderMode::RtvForced, RenderMode::RtvForcedMsaa, RenderMode::UavForced,
                        RenderMode::UavDefault}) {
        const bool forced = mode != RenderMode::Regular, forced_msaa = mode == RenderMode::RtvForcedMsaa,
                   uav_only = mode == RenderMode::UavForced || mode == RenderMode::UavDefault;
        if (forced_msaa && count == 1)
          continue;
        if (forced_msaa && !options.MultisampleRTVWithForcedSampleCountOne) {
          printf("unsupported: forced1 with multisampled RTVs\n");
          continue;
        }
        if (mode == RenderMode::UavDefault && count != 1)
          continue;
        if (centered && (forced || count == 1))
          continue;
        if (forced && !forced_msaa &&
            (count == 2 || (!uav_only && count > 1 && device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_1) ||
             (uav_only && count == 16 && device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_1)))
          continue;
        if (uav_only && count > 1 && !options.UAVOnlyRenderingForcedSampleCount) {
          printf("unsupported: UAV-only forced sample counts at feature level %#x\n", device->GetFeatureLevel());
          continue;
        }
        const UINT storage_count = forced && !forced_msaa ? 1 : count;
        UINT quality = 0;
        const DXGI_FORMAT format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        CHECK(device->CheckMultisampleQualityLevels(format, storage_count, &quality));
        if (storage_count == 1 || storage_count == 4)
          expect(quality > 0, "required %ux RGBA32_FLOAT support was not reported", count);
        if (!quality) {
          printf("unsupported: %ux RGBA32_FLOAT\n", count);
          continue;
        }
        auto all = forced ? forced_jobs(forced_msaa ? count : 1) : jobs(count, centered);
        if (uav_only)
          for (auto &j : all)
            j.pass = Uav;
        ComPtr<ID3D11VertexShader> vs, clip;
        ComPtr<ID3D11InputLayout> layout;
        ComPtr<ID3D11PixelShader> sample, index, modifier, varying, centroid, perspective, perspective_centroid, depth,
            query, output, force, uav_ps;
        ComPtr<ID3D11ComputeShader> cs;
        CHECK(device->CreateVertexShader(s.vs->GetBufferPointer(), s.vs->GetBufferSize(), nullptr, &vs));
        CHECK(device->CreateVertexShader(s.clip->GetBufferPointer(), s.clip->GetBufferSize(), nullptr, &clip));
        CHECK(device->CreateInputLayout(elements, std::size(elements), s.clip->GetBufferPointer(),
                                        s.clip->GetBufferSize(), &layout));
        CHECK(device->CreatePixelShader(s.sample->GetBufferPointer(), s.sample->GetBufferSize(), nullptr, &sample));
        CHECK(device->CreatePixelShader(s.index->GetBufferPointer(), s.index->GetBufferSize(), nullptr, &index));
        CHECK(
            device->CreatePixelShader(s.modifier->GetBufferPointer(), s.modifier->GetBufferSize(), nullptr, &modifier));
        CHECK(device->CreatePixelShader(s.varying->GetBufferPointer(), s.varying->GetBufferSize(), nullptr, &varying));
        CHECK(
            device->CreatePixelShader(s.centroid->GetBufferPointer(), s.centroid->GetBufferSize(), nullptr, &centroid));
        CHECK(device->CreatePixelShader(s.perspective->GetBufferPointer(), s.perspective->GetBufferSize(), nullptr,
                                        &perspective));
        CHECK(device->CreatePixelShader(s.perspective_centroid->GetBufferPointer(),
                                        s.perspective_centroid->GetBufferSize(), nullptr, &perspective_centroid));
        CHECK(device->CreatePixelShader(s.depth->GetBufferPointer(), s.depth->GetBufferSize(), nullptr, &depth));
        CHECK(device->CreatePixelShader(s.query->GetBufferPointer(), s.query->GetBufferSize(), nullptr, &query));
        CHECK(device->CreatePixelShader(s.output->GetBufferPointer(), s.output->GetBufferSize(), nullptr, &output));
        CHECK(device->CreatePixelShader(s.uav->GetBufferPointer(), s.uav->GetBufferSize(), nullptr, &uav_ps));
        CHECK(device->CreatePixelShader(s.forced->GetBufferPointer(), s.forced->GetBufferSize(), nullptr, &force));
        auto cs_code = forced && !forced_msaa ? s.single_cs : s.cs;
        CHECK(device->CreateComputeShader(cs_code->GetBufferPointer(), cs_code->GetBufferSize(), nullptr, &cs));
        D3D11_TEXTURE2D_DESC td{width,
                                height,
                                1,
                                1,
                                format,
                                {storage_count, storage_count == 1
                                                    ? 0u
                                                    : UINT(centered ? D3D11_CENTER_MULTISAMPLE_PATTERN
                                                                    : D3D11_STANDARD_MULTISAMPLE_PATTERN)},
                                D3D11_USAGE_DEFAULT,
                                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE};
        ComPtr<ID3D11Texture2D> target, positions;
        ComPtr<ID3D11RenderTargetView> rtv;
        ComPtr<ID3D11ShaderResourceView> target_srv, positions_srv;
        // a sample count the device reports is one it creates: a refusal is a wrong result, not the test's end
        step("%ux center %u mode %u targets", count, centered, unsigned(mode));
        if (!expect(SUCCEEDED(device->CreateTexture2D(&td, nullptr, &target)), "%ux render target was refused",
                    storage_count))
          continue;
        CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (!expect(SUCCEEDED(device->CreateTexture2D(&td, nullptr, &positions)), "%ux texture was refused",
                    storage_count))
          continue;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{format, D3D11_SRV_DIMENSION_TEXTURE2DMS};
        CHECK(device->CreateShaderResourceView(target.Get(), &sd, &target_srv));
        CHECK(device->CreateShaderResourceView(positions.Get(), &sd, &positions_srv));
        const UINT slot = width * height * storage_count * sizeof(Color);
        bd = {slot,         D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
              sizeof(Color)};
        ComPtr<ID3D11Buffer> result, staging;
        ComPtr<ID3D11UnorderedAccessView> uav;
        CHECK(device->CreateBuffer(&bd, nullptr, &result));
        CHECK(device->CreateUnorderedAccessView(result.Get(), nullptr, &uav));
        bd = {UINT(slot * all.size()), D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ};
        CHECK(device->CreateBuffer(&bd, nullptr, &staging));
        D3D11_RASTERIZER_DESC1 rd{D3D11_FILL_SOLID, D3D11_CULL_NONE};
        rd.DepthClipEnable = TRUE;
        rd.ForcedSampleCount = forced_msaa ? 1 : forced && mode != RenderMode::UavDefault ? count : 0;
        ComPtr<ID3D11RasterizerState1> states[4];
        bool refused = false;
        for (unsigned flags = 0; flags < (forced ? 1u : std::size(states)); flags++) {
          rd.MultisampleEnable = flags & 1;
          rd.AntialiasedLineEnable = flags >> 1;
          // D3D11.3 3.5.6.1 requires every forced count up to 16, but only those the GPU has are implemented: the
          // others are refused, and their mode is not drawn
          step("forced %ux rasterizer state %u", rd.ForcedSampleCount, flags);
          UINT levels = 0;
          CHECK(device->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, std::max(rd.ForcedSampleCount, 1u), &levels));
          const HRESULT created = device1->CreateRasterizerState1(&rd, &states[flags]);
          if (!levels) {
            expect(created == E_INVALIDARG, "a forced count this GPU lacks gave HRESULT %#lx", created);
            refused = true;
            break;
          }
          if ((refused = !expect(SUCCEEDED(created), "it was refused")))
            break;
          D3D11_RASTERIZER_DESC1 got{};
          states[flags]->GetDesc1(&got);
          expect(!memcmp(&got, &rd, sizeof(rd)), "GetDesc1 changed forced count or line fields");
          ComPtr<ID3D11RasterizerState2> state2;
          if (SUCCEEDED(states[flags].As(&state2))) {
            D3D11_RASTERIZER_DESC2 two;
            state2->GetDesc2(&two);
            expect(!memcmp(&two, &rd, sizeof(rd)) &&
                       two.ConservativeRaster == D3D11_CONSERVATIVE_RASTERIZATION_MODE_OFF,
                   "GetDesc2 changed forced count or conservative default");
          }
        }
        if (refused)
          continue;
        for (auto context : {immediate.Get(), deferred.Get()}) {
          for (size_t n = 0; n < all.size(); n++) {
            auto &j = all[n];
            auto &c = geometry[j.geometry];
            step("%s center %u %ux job %zu geometry %zu pass %u mask %#x",
                 context == immediate.Get() ? "immediate" : "deferred", centered, count, n, j.geometry, j.pass, j.mask);
            std::array<Vertex, Triangle> vertices{};
            std::copy(c.vertices.begin(), c.vertices.end(), vertices.begin());
            context->UpdateSubresource(vb.Get(), 0, nullptr, vertices.data(), 0, 0);
            const UINT stride = sizeof(Vertex), offset = 0;
            context->IASetVertexBuffers(0, 1, vb.GetAddressOf(), &stride, &offset);
            context->IASetInputLayout(layout.Get());
            context->IASetPrimitiveTopology(c.shape == Point  ? D3D11_PRIMITIVE_TOPOLOGY_POINTLIST
                                            : c.shape == Line ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST
                                                              : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(c.mode == Clip ? clip.Get() : vs.Get(), nullptr, 0);
            context->PSSetShader(j.pass == Uav                                ? uav_ps.Get()
                                 : j.pass == PerspectiveVarying               ? perspective.Get()
                                 : j.pass == PerspectiveCentroid              ? perspective_centroid.Get()
                                 : j.pass == PositionDepth                    ? depth.Get()
                                 : j.pass == Varying                          ? varying.Get()
                                 : j.pass == Centroid                         ? centroid.Get()
                                 : (j.pass == Forced || j.pass == ForcedMsaa) ? force.Get()
                                 : j.pass == IndexOnly                        ? index.Get()
                                 : j.pass == ModifierOnly                     ? modifier.Get()
                                 : j.pass == Shading                          ? sample.Get()
                                 : j.pass == Export                           ? output.Get()
                                                                              : query.Get(),
                                 nullptr, 0);
            auto source = j.pass == Unbound ? nullptr : positions_srv.Get();
            context->PSSetShaderResources(0, 1, &source);
            context->RSSetState(states[forced ? 0 : unsigned(c.multisample) | (unsigned(c.aa) << 1)].Get());
            context->OMSetDepthStencilState(no_depth.Get(), 0);
            D3D11_VIEWPORT vp{0, 0, width, height, 0, 1};
            context->RSSetViewports(1, &vp);
            context->OMSetBlendState(nullptr, nullptr, j.mask);
            if (uav_only) {
              const UINT clear[lanes] = {};
              context->ClearUnorderedAccessViewUint(uav.Get(), clear);
              context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 1, uav.GetAddressOf(),
                                                                 nullptr);
            } else {
              context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
              const float clear[lanes] = {};
              context->ClearRenderTargetView(rtv.Get(), clear);
            }
            context->Draw(c.vertices.size(), 0);
            ID3D11UnorderedAccessView *no_uav = nullptr;
            context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 1, &no_uav, nullptr);
            if (!uav_only) {
              context->CSSetShader(cs.Get(), nullptr, 0);
              context->CSSetShaderResources(0, 1, target_srv.GetAddressOf());
              context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
              context->Dispatch(width, height, 1);
              ID3D11ShaderResourceView *unbound = nullptr;
              context->CSSetShaderResources(0, 1, &unbound);
              context->CSSetUnorderedAccessViews(0, 1, &no_uav, nullptr);
            }
            D3D11_BOX box{0, 0, 0, slot, 1, 1};
            context->CopySubresourceRegion(staging.Get(), 0, n * slot, 0, 0, result.Get(), 0, &box);
          }
          if (context == deferred.Get()) {
            ComPtr<ID3D11CommandList> commands;
            CHECK(deferred->FinishCommandList(FALSE, &commands));
            immediate->ExecuteCommandList(commands.Get(), FALSE);
          }
          D3D11_MAPPED_SUBRESOURCE mapped;
          CHECK(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
          auto data = static_cast<const Color *>(mapped.pData);
          for (size_t n = 0; n < all.size(); n++) {
            auto &j = all[n];
            step("read %ux job %zu pass %u mask %#x", count, n, j.pass, j.mask);
            raster_samples::compare(data + n * width * height * storage_count, geometry[j.geometry], count, j.mask,
                                    j.pass, centered);
          }
          immediate->Unmap(staging.Get(), 0);
        }
      }
  return verdict();
}
