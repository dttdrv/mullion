// contract: see ../tessellator.hpp, here through Direct3D 12.
#include "d3d12_test.hpp"
#include "../tessellator.hpp"

using namespace tessellator;

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER parameters[2] = {{D3D12_ROOT_PARAMETER_TYPE_CBV}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  parameters[1].Descriptor.ShaderRegister = 1;
  auto rs = root_signature(device.Get(), {2, parameters});
  if (!rs) {
    printf("failed: root signature\n");
    return 1;
  }
  const DXGI_FORMAT format = DXGI_FORMAT_R32G32_FLOAT;
  // `topology`: the hull shader's output; `log`: with the pixel shader that logs its points; `layered`: with the
  // geometry shader that passes triangles on to layers
  uint32_t instances = 0, fill = 0;
  auto pipeline = [&](Domain domain, Partitioning partitioning, const char *topology, bool log, bool layered = false) {
    // the log is bound where the points are drawn
    auto d = defines(domain, partitioning, topology, instances, log, false, layered, fill);
    auto vs = compiler.compile(hlsl, "vs", "vs", d), hs = compiler.compile(hlsl, "hs", "hs", d),
         ds = compiler.compile(hlsl, "ds", "ds", d),
         ps = compiler.compile(hlsl, layered ? "ps_layer" : log ? "ps_log" : "ps_cover", "ps", d);
    auto gs = instances || layered ? compiler.compile(hlsl, "gs", "gs", d) : vs;
    ComPtr<ID3D12PipelineState> pso;
    if (vs.empty() || hs.empty() || ds.empty() || ps.empty() || gs.empty())
      return pso;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = rs.Get();
    desc.VS = bytecode(vs);
    desc.HS = bytecode(hs);
    desc.DS = bytecode(ds);
    if (instances || layered)
      desc.GS = bytecode(gs);
    desc.PS = bytecode(ps);
    // every triangle and line adds to its pixels
    desc.BlendState.RenderTarget[0] = {TRUE,  FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ONE, D3D12_BLEND_OP_ADD,
                                       D3D12_BLEND_ONE, D3D12_BLEND_ONE, D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP,
                                       D3D12_COLOR_WRITE_ENABLE_ALL};
    desc.SampleMask = ~0u;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_BACK};
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = format;
    desc.SampleDesc = {1, 0};
    device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
    return pso;
  };

  ComPtr<ID3D12Resource> target;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, format, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)));
  auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 target_bytes;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &target_bytes);
  auto pixels = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, target_bytes, D3D12_RESOURCE_STATE_COPY_DEST);

  auto constants = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 4096, D3D12_RESOURCE_STATE_GENERIC_READ);
  Factors *factors;
  CHECK(constants->Map(0, nullptr, (void **)&factors));

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  // a draw's arguments, for the draws that go through ExecuteIndirect
  D3D12_INDIRECT_ARGUMENT_DESC argument{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW};
  D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(D3D12_DRAW_ARGUMENTS), 1, &argument};
  ComPtr<ID3D12CommandSignature> signature;
  CHECK(device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signature)));
  auto arguments = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(D3D12_DRAW_ARGUMENTS), D3D12_RESOURCE_STATE_GENERIC_READ);
  D3D12_DRAW_ARGUMENTS *indirect_draw;
  CHECK(arguments->Map(0, nullptr, (void **)&indirect_draw));

  // draws `count` patches with the pipeline, the log bound if there is one, and leaves the target readable in `pixels`;
  // an indirect draw is the same draw from a buffer
  auto draw = [&](ID3D12PipelineState *pso, UINT count, ID3D12Resource *log, bool indirect = false) {
    if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), pso)))
      return E_FAIL;
    const float clear[4] = {};
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    D3D12_VIEWPORT viewport{0, 0, (float)width, (float)height, 0, 1};
    D3D12_RECT scissor{0, 0, (LONG)width, (LONG)height};
    list->SetGraphicsRootSignature(rs.Get());
    list->SetGraphicsRootConstantBufferView(0, constants->GetGPUVirtualAddress());
    if (log)
      list->SetGraphicsRootUnorderedAccessView(1, log->GetGPUVirtualAddress());
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST);
    *indirect_draw = {count, 1, 0, 0};
    if (indirect)
      list->ExecuteIndirect(signature.Get(), 1, arguments.Get(), 0, nullptr, 0);
    else
      list->DrawInstanced(count, 1, 0, 0);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
        dst{pixels.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    return submit(device.Get(), queue.Get(), list.Get());
  };
  auto pixel = [&](const char *mapped, UINT x, UINT y) {
    return (const float *)(mapped + footprint.Offset + y * footprint.Footprint.RowPitch) + 2 * x;
  };

  unsigned failures = 0, draws = 0, product_draws = 0;
  size_t total = 0;
  // without a geometry shader, then with one of two instances, then with a domain shader of many output registers
  const uint32_t most = filled_outputs - outputs;
  for (auto [geometry_instances, filled] : {std::pair{0u, 0u}, std::pair{2u, 0u}, std::pair{0u, most}})
  for (Domain domain : {reference::isoline, reference::triangle, reference::quad})
    for (Partitioning partitioning :
         {reference::integer, reference::pow2, reference::fractional_odd, reference::fractional_even}) {
      instances = geometry_instances;
      fill = filled;
      std::string name = std::string(domain_names[domain]) + ", " + partitioning_names[partitioning] +
                         (instances ? ", geometry shader" : "") + (fill ? ", wide outputs" : "");
      // points
      auto pso = pipeline(domain, partitioning, "point", true);
      if (!pso) {
        printf("failed: %s: no pipeline with point output\n", name.c_str());
        return 1;
      }
      auto patches_drawn = draw_patches(domain, 1 + domain * 4 + partitioning);
      memcpy(factors, patches_drawn.data(), patches_drawn.size() * sizeof(Factors));
      // the points, and room to see more than there should be
      const UINT64 words = log_header + 4 * (points_of(domain, partitioning, patches_drawn) + 64);
      auto log = buffer(
          device.Get(), D3D12_HEAP_TYPE_DEFAULT, words * 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
      );
      auto zeros = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, words * 4, D3D12_RESOURCE_STATE_GENERIC_READ);
      auto read = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, words * 4, D3D12_RESOURCE_STATE_COPY_DEST);
      void *mapped;
      CHECK(zeros->Map(0, nullptr, &mapped));
      memset(mapped, 0, words * 4);
      zeros->Unmap(0, nullptr);
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), nullptr));
      transition(list.Get(), log.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
      list->CopyResource(log.Get(), zeros.Get());
      transition(list.Get(), log.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      CHECK(draw(pso.Get(), patches, log.Get()));
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), nullptr));
      transition(list.Get(), log.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyResource(read.Get(), log.Get());
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      CHECK(read->Map(0, nullptr, &mapped));
      std::vector<uint32_t> logged(words);
      memcpy(logged.data(), mapped, words * 4);
      read->Unmap(0, nullptr);
      size_t points_before = total;
      uint32_t geometry_runs = logged[1];
      failures += check_points(name.c_str(), domain, partitioning, patches_drawn, logged, total);
      if (geometry_runs != instances * (total - points_before)) {
        printf("%s: the geometry shader ran %u times, want %u for each of %zu points\n", name.c_str(), geometry_runs, instances, total - points_before);
        failures++;
      }
      // every instance's points are on the target
      char *read_pixels;
      CHECK(pixels->Map(0, nullptr, (void **)&read_pixels));
      double sum = 0;
      for (UINT y = 0; y < height; y++)
        for (UINT x = 0; x < width; x++)
          sum += pixel(read_pixels, x, y)[0];
      pixels->Unmap(0, nullptr);
      if (sum != double(weight(instances)) * (total - points_before)) {
        printf("%s: the points add %g, want %u each of %zu\n", name.c_str(), sum, weight(instances), total - points_before);
        failures++;
      }
      draws++;

      // lines
      if (domain == reference::isoline) {
        pso = pipeline(domain, partitioning, "line", false);
        if (!pso) {
          printf("failed: %s: no pipeline with line output\n", name.c_str());
          return 1;
        }
        for (auto &patch : lines_alone) {
          factors[0] = patch;
          CHECK(draw(pso.Get(), 1, nullptr));
          char *read_pixels;
          CHECK(pixels->Map(0, nullptr, (void **)&read_pixels));
          unsigned wrong = 0;
          for (UINT y = 0; y < height; y++)
            for (UINT x = 0; x < width; x++) {
              auto got = pixel(read_pixels, x, y);
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
          pixels->Unmap(0, nullptr);
          failures += wrong;
          draws++;
        }
        continue;
      }
      // triangles
      for (bool clockwise : {true, false}) {
        pso = pipeline(domain, partitioning, clockwise ? "triangle_cw" : "triangle_ccw", false);
        if (!pso) {
          printf("failed: %s: no pipeline with triangle output\n", name.c_str());
          return 1;
        }
        // every patch alone, then the fullest as many at once
        for (size_t i = 0; i < std::size(alone) + fullest; i++) {
          auto &patch = alone[i % std::size(alone)];
          UINT count = i < std::size(alone) ? 1 : patches;
          std::fill_n(factors, count, patch);
          // the counter-clockwise ones through ExecuteIndirect
          CHECK(draw(pso.Get(), count, nullptr, !clockwise));
          char *read_pixels;
          CHECK(pixels->Map(0, nullptr, (void **)&read_pixels));
          unsigned wrong = 0;
          // which triangle is over a pixel, where they are large enough to tell
          std::vector<float> products;
          if (clockwise && products_tell(patch)) {
            products = expected_products(domain, partitioning, patch);
            product_draws++;
          }
          for (UINT y = 0; y < height; y++)
            for (UINT x = 0; x < width; x++) {
              float got = pixel(read_pixels, x, y)[0],
                    want = weight(instances) * expected_cover(domain, partitioning, patch, clockwise, x, y);
              float product = pixel(read_pixels, x, y)[1], want_product = products.empty() ? NAN : products[y * width + x];
              if (want > 0 && !std::isnan(want_product) && !(std::abs(product - count * want_product) <= count * product_tolerance) &&
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
          pixels->Unmap(0, nullptr);
          failures += wrong;
          draws++;
        }
      }
    }
  // layers: every triangle goes to its leading vertex's
  unsigned mixed = 0, layered_draws = 0;
  {
    D3D12_RESOURCE_DESC layered_desc = target_desc;
    layered_desc.DepthOrArraySize = layers;
    ComPtr<ID3D12Resource> layered;
    CHECK(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &layered_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&layered)
    ));
    ComPtr<ID3D12DescriptorHeap> layered_heap;
    CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&layered_heap)));
    auto layered_rtv = layered_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(layered.Get(), nullptr, layered_rtv);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[layers];
    UINT64 layered_bytes;
    device->GetCopyableFootprints(&layered_desc, 0, layers, 0, footprints, nullptr, nullptr, &layered_bytes);
    auto layered_pixels = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, layered_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    instances = fill = 0;
    for (Domain domain : {reference::triangle, reference::quad})
      for (Partitioning partitioning :
           {reference::integer, reference::pow2, reference::fractional_odd, reference::fractional_even}) {
        std::string name = std::string(domain_names[domain]) + ", " + partitioning_names[partitioning] + ", layers";
        auto pso = pipeline(domain, partitioning, "triangle_cw", false, true);
        if (!pso) {
          printf("failed: %s: no pipeline\n", name.c_str());
          return 1;
        }
        for (auto &patch : alone) {
          factors[0] = patch;
          CHECK(allocator->Reset());
          CHECK(list->Reset(allocator.Get(), pso.Get()));
          const float clear[4] = {};
          list->ClearRenderTargetView(layered_rtv, clear, 0, nullptr);
          D3D12_VIEWPORT viewport{0, 0, (float)width, (float)height, 0, 1};
          D3D12_RECT scissor{0, 0, (LONG)width, (LONG)height};
          list->SetGraphicsRootSignature(rs.Get());
          list->SetGraphicsRootConstantBufferView(0, constants->GetGPUVirtualAddress());
          list->OMSetRenderTargets(1, &layered_rtv, FALSE, nullptr);
          list->RSSetViewports(1, &viewport);
          list->RSSetScissorRects(1, &scissor);
          list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST);
          // every patch both ways over the partitionings: directly, and through ExecuteIndirect
          *indirect_draw = {1, 1, 0, 0};
          if ((&patch - alone + partitioning) % 2)
            list->ExecuteIndirect(signature.Get(), 1, arguments.Get(), 0, nullptr, 0);
          else
            list->DrawInstanced(1, 1, 0, 0);
          transition(list.Get(), layered.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
          for (UINT layer = 0; layer < layers; layer++) {
            D3D12_TEXTURE_COPY_LOCATION src{layered.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = layer}},
                dst{layered_pixels.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprints[layer]}};
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
          }
          transition(list.Get(), layered.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
          CHECK(submit(device.Get(), queue.Get(), list.Get()));
          char *read_pixels;
          CHECK(layered_pixels->Map(0, nullptr, (void **)&read_pixels));
          std::vector<float> drawn[layers];
          for (UINT layer = 0; layer < layers; layer++)
            for (UINT y = 0; y < height; y++) {
              auto row = (const float *)(read_pixels + footprints[layer].Offset + y * footprints[layer].Footprint.RowPitch);
              drawn[layer].insert(drawn[layer].end(), row, row + 2 * width);
            }
          layered_pixels->Unmap(0, nullptr);
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
