// contract: see ../tessellator.hpp, here through Direct3D 11.
#include "d3d11_test.hpp"
#include "../tessellator.hpp"

using namespace tessellator;

int
main() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context));

  const DXGI_FORMAT format = DXGI_FORMAT_R32G32_FLOAT;
  D3D11_TEXTURE2D_DESC texture_desc{width, height, 1, 1, format, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
  ComPtr<ID3D11Texture2D> target, target_read;
  ComPtr<ID3D11RenderTargetView> rtv;
  CHECK(device->CreateTexture2D(&texture_desc, nullptr, &target));
  CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
  texture_desc.Usage = D3D11_USAGE_STAGING;
  texture_desc.BindFlags = 0;
  texture_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  CHECK(device->CreateTexture2D(&texture_desc, nullptr, &target_read));
  // every triangle and line adds to its pixels
  D3D11_BLEND_DESC blend_desc{};
  blend_desc.RenderTarget[0] = {TRUE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE,
                                D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL};
  ComPtr<ID3D11BlendState> blend;
  CHECK(device->CreateBlendState(&blend_desc, &blend));
  D3D11_RASTERIZER_DESC raster_desc{D3D11_FILL_SOLID, D3D11_CULL_BACK};
  raster_desc.DepthClipEnable = TRUE;
  ComPtr<ID3D11RasterizerState> raster;
  CHECK(device->CreateRasterizerState(&raster_desc, &raster));
  D3D11_BUFFER_DESC constants_desc{sizeof(Factors) * patches, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
  ComPtr<ID3D11Buffer> constants;
  CHECK(device->CreateBuffer(&constants_desc, nullptr, &constants));
  D3D11_VIEWPORT viewport{0, 0, (float)width, (float)height, 0, 1};
  context->RSSetState(raster.Get());
  context->RSSetViewports(1, &viewport);
  context->OMSetBlendState(blend.Get(), nullptr, ~0u);
  context->HSSetConstantBuffers(0, 1, constants.GetAddressOf());
  // what a geometry shader's instance adds to a pixel, 1 + i, from the geometry stage's bindings: 1 in a constant
  // buffer and i in a buffer
  const uint32_t most_instances = 2;
  const float first[4] = {1};
  std::vector<float> steps(most_instances);
  for (uint32_t i = 0; i < most_instances; i++)
    steps[i] = i;
  D3D11_BUFFER_DESC first_desc{sizeof(first), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER};
  D3D11_BUFFER_DESC steps_desc{(UINT)(steps.size() * sizeof(float)), D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE};
  D3D11_SUBRESOURCE_DATA first_data{first}, steps_data{steps.data()};
  ComPtr<ID3D11Buffer> first_buffer, steps_buffer;
  CHECK(device->CreateBuffer(&first_desc, &first_data, &first_buffer));
  CHECK(device->CreateBuffer(&steps_desc, &steps_data, &steps_buffer));
  D3D11_SHADER_RESOURCE_VIEW_DESC steps_view_desc{DXGI_FORMAT_R32_FLOAT, D3D11_SRV_DIMENSION_BUFFER};
  steps_view_desc.Buffer = {0, most_instances};
  ComPtr<ID3D11ShaderResourceView> steps_view;
  CHECK(device->CreateShaderResourceView(steps_buffer.Get(), &steps_view_desc, &steps_view));
  context->GSSetConstantBuffers(1, 1, first_buffer.GetAddressOf());
  context->GSSetShaderResources(2, 1, steps_view.GetAddressOf());
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST);

  // binds the shaders of a domain and partitioning; `topology`: the hull shader's output, `log`: with the pixel
  // shader that logs its points, `layered`: with the geometry shader that passes triangles on to layers
  uint32_t instances = 0, fill = 0;
  auto bind = [&](Domain domain, Partitioning partitioning, const char *topology, bool log, bool layered = false) {
    // feature level 11_0 has unordered access in the pixel shader alone
    auto d = defines(domain, partitioning, topology, instances, false, true, layered, fill);
    auto vs_code = compile(hlsl, "vs", "vs", d), hs_code = compile(hlsl, "hs", "hs", d),
         ds_code = compile(hlsl, "ds", "ds", d),
         ps_code = compile(hlsl, layered ? "ps_layer" : log ? "ps_log" : "ps_cover", "ps", d);
    auto gs_code = instances || layered ? compile(hlsl, "gs", "gs", d) : vs_code;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11HullShader> hs;
    ComPtr<ID3D11DomainShader> ds;
    ComPtr<ID3D11GeometryShader> gs;
    ComPtr<ID3D11PixelShader> ps;
    if (!vs_code || !hs_code || !ds_code || !ps_code || !gs_code ||
        (gs_code != vs_code && FAILED(device->CreateGeometryShader(gs_code->GetBufferPointer(), gs_code->GetBufferSize(), nullptr, &gs))) ||
        FAILED(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs)) ||
        FAILED(device->CreateHullShader(hs_code->GetBufferPointer(), hs_code->GetBufferSize(), nullptr, &hs)) ||
        FAILED(device->CreateDomainShader(ds_code->GetBufferPointer(), ds_code->GetBufferSize(), nullptr, &ds)) ||
        FAILED(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &ps)))
      return false;
    context->VSSetShader(vs.Get(), nullptr, 0);
    context->HSSetShader(hs.Get(), nullptr, 0);
    context->DSSetShader(ds.Get(), nullptr, 0);
    context->GSSetShader(gs.Get(), nullptr, 0);
    context->PSSetShader(ps.Get(), nullptr, 0);
    return true;
  };
  // the arguments of a draw of every patch, for DrawInstancedIndirect
  const D3D11_DRAW_INSTANCED_INDIRECT_ARGS every_patch{patches, 1, 0, 0};
  D3D11_BUFFER_DESC arguments_desc{sizeof(every_patch), D3D11_USAGE_IMMUTABLE, 0, 0, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS};
  D3D11_SUBRESOURCE_DATA arguments_data{&every_patch};
  ComPtr<ID3D11Buffer> arguments;
  CHECK(device->CreateBuffer(&arguments_desc, &arguments_data, &arguments));
  // draws `count` patches to the target, or to the one given, the log bound if there is one. every patch at once is
  // drawn from arguments in a buffer
  auto draw = [&](UINT count, ID3D11UnorderedAccessView *log, ID3D11RenderTargetView *to = nullptr) {
    const float clear[4] = {};
    to = to ? to : rtv.Get();
    context->ClearRenderTargetView(to, clear);
    context->OMSetRenderTargetsAndUnorderedAccessViews(1, &to, nullptr, 1, log ? 1 : 0, &log, nullptr);
    if (count == every_patch.VertexCountPerInstance)
      context->DrawInstancedIndirect(arguments.Get(), 0);
    else
      context->Draw(count, 0);
  };
  // maps the target's pixels
  auto read_target = [&](D3D11_MAPPED_SUBRESOURCE &mapped) {
    context->CopyResource(target_read.Get(), target.Get());
    return context->Map(target_read.Get(), 0, D3D11_MAP_READ, 0, &mapped);
  };
  auto pixel = [&](const D3D11_MAPPED_SUBRESOURCE &mapped, UINT x, UINT y) {
    return (const float *)((const char *)mapped.pData + y * mapped.RowPitch) + 2 * x;
  };

  unsigned failures = 0, draws = 0, product_draws = 0;
  size_t total = 0;
  // without a geometry shader, then with one of as many instances as have weights, then with a domain shader of
  // many output registers
  const uint32_t most = filled_outputs - outputs;
  for (auto [geometry_instances, filled] : {std::pair{0u, 0u}, std::pair{most_instances, 0u}, std::pair{0u, most}})
  for (Domain domain : {reference::isoline, reference::triangle, reference::quad})
    for (Partitioning partitioning :
         {reference::integer, reference::pow2, reference::fractional_odd, reference::fractional_even}) {
      instances = geometry_instances;
      fill = filled;
      std::string name = std::string(domain_names[domain]) + ", " + partitioning_names[partitioning] +
                         (instances ? ", geometry shader" : "") + (fill ? ", wide outputs" : "");
      // points
      if (!bind(domain, partitioning, "point", true)) {
        printf("failed: %s: no shaders with point output\n", name.c_str());
        return 1;
      }
      auto patches_drawn = draw_patches(domain, 1 + domain * 4 + partitioning);
      context->UpdateSubresource(constants.Get(), 0, nullptr, patches_drawn.data(), 0, 0);
      // the count, the points, and room to see more than there should be
      const UINT words = log_header + 4 * (points_of(domain, partitioning, patches_drawn) + 64);
      D3D11_BUFFER_DESC log_desc{words * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                                 D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS};
      std::vector<uint32_t> zeros(words);
      D3D11_SUBRESOURCE_DATA zero_data{zeros.data()};
      ComPtr<ID3D11Buffer> log;
      CHECK(device->CreateBuffer(&log_desc, &zero_data, &log));
      D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc{DXGI_FORMAT_R32_TYPELESS, D3D11_UAV_DIMENSION_BUFFER};
      uav_desc.Buffer = {0, words, D3D11_BUFFER_UAV_FLAG_RAW};
      ComPtr<ID3D11UnorderedAccessView> uav;
      CHECK(device->CreateUnorderedAccessView(log.Get(), &uav_desc, &uav));
      draw(patches, uav.Get());
      // every instance's points are on the target
      D3D11_MAPPED_SUBRESOURCE points_mapped;
      CHECK(read_target(points_mapped));
      double sum = 0;
      for (UINT y = 0; y < height; y++)
        for (UINT x = 0; x < width; x++)
          sum += pixel(points_mapped, x, y)[0];
      context->Unmap(target_read.Get(), 0);
      draw(0, nullptr); // unbinds the log
      size_t points_before = total;
      failures += check_points(name.c_str(), domain, partitioning, patches_drawn, read(device.Get(), context.Get(), log.Get()), total);
      if (sum != double(weight(instances)) * (total - points_before)) {
        printf("%s: the points add %g, want %u each of %zu\n", name.c_str(), sum, weight(instances), total - points_before);
        failures++;
      }
      draws++;

      // lines
      if (domain == reference::isoline) {
        if (!bind(domain, partitioning, "line", false)) {
          printf("failed: %s: no shaders with line output\n", name.c_str());
          return 1;
        }
        for (auto &patch : lines_alone) {
          std::vector<Factors> one(patches, patch);
          context->UpdateSubresource(constants.Get(), 0, nullptr, one.data(), 0, 0);
          draw(1, nullptr);
          D3D11_MAPPED_SUBRESOURCE mapped;
          CHECK(read_target(mapped));
          unsigned wrong = 0;
          for (UINT y = 0; y < height; y++)
            for (UINT x = 0; x < width; x++) {
              auto got = pixel(mapped, x, y);
              auto want = expected_line(partitioning, patch, x, y);
              bool match = !want.on ? got[0] == 0
                                    : got[0] == weight(instances) &&
                                          (!want.decided || std::abs(got[1] - want.square) <= 1e-4f);
              if (!match && wrong++ < 2)
                printf(
                    "%s, lines %g of detail %g: pixel %u,%u has %g lines and %g, want %d and %g\n", name.c_str(),
                    patch[0], patch[1], x, y, got[0], got[1], want.on, want.square
                );
            }
          context->Unmap(target_read.Get(), 0);
          failures += wrong;
          draws++;
        }
        continue;
      }
      // triangles
      for (bool clockwise : {true, false}) {
        if (!bind(domain, partitioning, clockwise ? "triangle_cw" : "triangle_ccw", false)) {
          printf("failed: %s: no shaders with triangle output\n", name.c_str());
          return 1;
        }
        // every patch alone, then the fullest as many at once
        for (size_t i = 0; i < std::size(alone) + fullest; i++) {
          auto &patch = alone[i % std::size(alone)];
          UINT count = i < std::size(alone) ? 1 : patches;
          std::vector<Factors> many(patches, patch);
          context->UpdateSubresource(constants.Get(), 0, nullptr, many.data(), 0, 0);
          draw(count, nullptr);
          D3D11_MAPPED_SUBRESOURCE mapped;
          CHECK(read_target(mapped));
          unsigned wrong = 0;
          // which triangle is over a pixel, where they are large enough to tell
          std::vector<float> products;
          if (clockwise && products_tell(patch)) {
            products = expected_products(domain, partitioning, patch);
            product_draws++;
          }
          for (UINT y = 0; y < height; y++)
            for (UINT x = 0; x < width; x++) {
              float got = pixel(mapped, x, y)[0],
                    want = weight(instances) * expected_cover(domain, partitioning, patch, clockwise, x, y);
              float product = pixel(mapped, x, y)[1], want_product = products.empty() ? NAN : products[y * width + x];
              if (want > 0 && !std::isnan(want_product) && std::abs(product - count * want_product) > count * product_tolerance &&
                  wrong++ < 2)
                printf(
                    "%s, %u of factors %g %g %g %g, inside %g %g: pixel %u,%u has product %g, want %g\n", name.c_str(),
                    count, patch[0], patch[1], patch[2], patch[3], patch[4], patch[5], x, y, product, count * want_product
                );
              if (want >= 0 && got != count * want && wrong++ < 2)
                printf(
                    "%s, %s, %u of factors %g %g %g %g, inside %g %g: pixel %u,%u is covered %g times, want %g\n",
                    name.c_str(), clockwise ? "clockwise" : "counter-clockwise", count, patch[0], patch[1], patch[2],
                    patch[3], patch[4], patch[5], x, y, got, count * want
                );
            }
          context->Unmap(target_read.Get(), 0);
          failures += wrong;
          draws++;
        }
      }
    }
  // layers: every triangle goes to its leading vertex's
  unsigned mixed = 0, layered_draws = 0;
  {
    D3D11_TEXTURE2D_DESC layered_desc{width, height, 1, layers, format, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
    ComPtr<ID3D11Texture2D> layered;
    ComPtr<ID3D11RenderTargetView> layered_rtv;
    CHECK(device->CreateTexture2D(&layered_desc, nullptr, &layered));
    CHECK(device->CreateRenderTargetView(layered.Get(), nullptr, &layered_rtv));
    instances = fill = 0;
    for (Domain domain : {reference::triangle, reference::quad})
      for (Partitioning partitioning :
           {reference::integer, reference::pow2, reference::fractional_odd, reference::fractional_even}) {
        std::string name = std::string(domain_names[domain]) + ", " + partitioning_names[partitioning] + ", layers";
        if (!bind(domain, partitioning, "triangle_cw", false, true)) {
          printf("failed: %s: no shaders\n", name.c_str());
          return 1;
        }
        for (auto &patch : alone) {
          std::vector<Factors> one(patches, patch);
          context->UpdateSubresource(constants.Get(), 0, nullptr, one.data(), 0, 0);
          draw(1, nullptr, layered_rtv.Get());
          std::vector<float> drawn[layers];
          for (UINT layer = 0; layer < layers; layer++) {
            context->CopySubresourceRegion(target_read.Get(), 0, 0, 0, 0, layered.Get(), layer, nullptr);
            D3D11_MAPPED_SUBRESOURCE mapped;
            CHECK(context->Map(target_read.Get(), 0, D3D11_MAP_READ, 0, &mapped));
            for (UINT y = 0; y < height; y++)
              drawn[layer].insert(drawn[layer].end(), pixel(mapped, 0, y), pixel(mapped, width, y));
            context->Unmap(target_read.Get(), 0);
          }
          failures += check_layers(name.c_str(), domain, partitioning, patch, drawn);
          mixed += mixed_layers(domain, partitioning, patch);
          layered_draws++;
        }
      }
    if (!mixed) {
      printf("failed: no triangle has corners of more than one layer\n");
      return 1;
    }
  }
  // a point through the geometry shader alone, then in the same pass patches through the tessellator and it: both
  // find the geometry stage's bindings, and add what their instances weigh for each point
  {
    const Domain domain = reference::quad;
    const Partitioning partitioning = reference::integer;
    instances = most_instances;
    auto point_code = compile(hlsl, "vs_point", "vs", defines(domain, partitioning, "point", instances, false, true));
    ComPtr<ID3D11VertexShader> point, vs;
    ComPtr<ID3D11HullShader> hs;
    ComPtr<ID3D11DomainShader> ds;
    if (!bind(domain, partitioning, "point", false) || !point_code ||
        FAILED(device->CreateVertexShader(point_code->GetBufferPointer(), point_code->GetBufferSize(), nullptr, &point))) {
      printf("failed: no shaders for a point without a tessellator\n");
      return 1;
    }
    auto patches_drawn = draw_patches(domain, 1);
    context->UpdateSubresource(constants.Get(), 0, nullptr, patches_drawn.data(), 0, 0);
    context->VSGetShader(&vs, nullptr, nullptr);
    context->HSGetShader(&hs, nullptr, nullptr);
    context->DSGetShader(&ds, nullptr, nullptr);
    context->VSSetShader(point.Get(), nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    draw(1, nullptr);
    context->VSSetShader(vs.Get(), nullptr, 0);
    context->HSSetShader(hs.Get(), nullptr, 0);
    context->DSSetShader(ds.Get(), nullptr, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST);
    context->Draw(patches, 0);
    D3D11_MAPPED_SUBRESOURCE mapped;
    CHECK(read_target(mapped));
    double sum = 0;
    for (UINT y = 0; y < height; y++)
      for (UINT x = 0; x < width; x++)
        sum += pixel(mapped, x, y)[0];
    context->Unmap(target_read.Get(), 0);
    size_t points = 1 + points_of(domain, partitioning, patches_drawn);
    if (sum != double(weight(instances)) * points) {
      printf("a point, then patches: the points add %g, want %u each of %zu\n", sum, weight(instances), points);
      failures++;
    }
    draws += 2;
  }
  if (failures) {
    printf("failed: %u wrong\n", failures);
    return 1;
  }
  printf(
      "passed: %zu points of %u patches as the reference's, %u draws, %u with the reference's triangles, %u into "
      "layers with %u triangles of mixed ones\n",
      total, patches * 24, draws, product_draws, layered_draws, mixed
  );
  return 0;
}
