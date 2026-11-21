// contract: D3D11.3 16.12.1 requires "Depth/Stencil tests and
// depth/stencil writes before executing the Pixel Shader" for forced early
// shaders, and "nothing downstream further impacts the count." 16.12.2
// disables early rejection for default UAV shaders. 16.13 suppresses UAV writes
// after discard. D3D12 retains these modes (Depth Bounds Test, Early Testing).
// color, depth, stencil, UAV counters and occlusion must agree in every path.
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
// https://microsoft.github.io/DirectX-Specs/d3d/DepthBoundsTest.html
#include "d3d12_test.hpp"
#include "../om_order.hpp"

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const std::vector<std::string> defines{"COLOR_VALUE=" + std::to_string(om::order_color)};
  auto vs = compiler.compile(om::order_hlsl, "vs", "vs", defines);
  std::string ps[std::size(om::order_entries)];
  for (UINT i = 0; i < std::size(ps); i++) {
    ps[i] = compiler.compile(om::order_hlsl, om::order_entries[i], "ps", defines);
    if (!expect(!ps[i].empty(), "order HLSL %s did not compile", om::order_entries[i]))
      return verdict();
  }
  if (!expect(!vs.empty(), "order vertex HLSL did not compile"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  const auto cases = om::order_cases();
  const UINT width = om::order_width, height = UINT(cases.size());
  const UINT64 counter_bytes = 2 * width * height * sizeof(UINT);
  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[0].Constants.Num32BitValues = 4;
  params[1].Descriptor.ShaderRegister = 1;
  auto rs = root_signature(device.Get(), {UINT(std::size(params)), params});
  if (!expect(bool(rs), "order root signature missing"))
    return verdict();
  D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC color_desc{
      D3D12_RESOURCE_DIMENSION_TEXTURE2D,
      0,
      width,
      height,
      1,
      1,
      DXGI_FORMAT_R32_UINT,
      {1, 0},
      D3D12_TEXTURE_LAYOUT_UNKNOWN,
      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
  };
  auto depth_desc = color_desc;
  depth_desc.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT color_at{}, planes[2]{};
  UINT64 total, row_bytes[2];
  device->GetCopyableFootprints(&color_desc, 0, 1, 0, &color_at, nullptr, nullptr, &total);
  const UINT64 plane_base = (total + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) /
                            D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
  device->GetCopyableFootprints(&depth_desc, 0, std::size(planes), plane_base, planes, nullptr, row_bytes, nullptr);
  const UINT64 counters_at = planes[1].Offset + UINT64(planes[1].Footprint.RowPitch) * height;
  const UINT64 counts_at = counters_at + counter_bytes;
  auto readback = buffer(
      device.Get(), D3D12_HEAP_TYPE_READBACK, counts_at + cases.size() * sizeof(UINT64), D3D12_RESOURCE_STATE_COPY_DEST
  );
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, counter_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto indirect =
      buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(D3D12_DRAW_ARGUMENTS), D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(readback && upload && indirect, "order transfer buffers missing"))
    return verdict();
  void *mapped;
  CHECK(upload->Map(0, nullptr, &mapped));
  memset(mapped, 0, counter_bytes);
  upload->Unmap(0, nullptr);
  CHECK(indirect->Map(0, nullptr, &mapped));
  *static_cast<D3D12_DRAW_ARGUMENTS *>(mapped) = {3, 1, 0, 0};
  indirect->Unmap(0, nullptr);
  D3D12_INDIRECT_ARGUMENT_DESC argument{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW};
  D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(D3D12_DRAW_ARGUMENTS), 1, &argument};
  ComPtr<ID3D12CommandSignature> signature;
  CHECK(device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signature)));
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator, bundle_allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&bundle_allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  ComPtr<ID3D12QueryHeap> queries;
  D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_OCCLUSION, UINT(cases.size())};
  CHECK(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&queries)));
  std::vector<ComPtr<ID3D12PipelineState>> pipelines(cases.size());
  for (UINT i = 0; i < cases.size(); i++) {
    const auto &k = cases[i];
    auto s = om::order_state(k);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature = rs.Get();
    p.VS = bytecode(vs);
    p.PS = bytecode(ps[k.shader]);
    p.NumRenderTargets = 1;
    p.RTVFormats[0] = color_desc.Format;
    p.DSVFormat = depth_desc.Format;
    p.SampleDesc = {1, 0};
    p.SampleMask = ~0u;
    p.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    p.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    p.RasterizerState.DepthClipEnable = TRUE;
    p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.DepthStencilState = {
        s.DepthEnable,
        D3D12_DEPTH_WRITE_MASK(s.DepthWriteMask),
        D3D12_COMPARISON_FUNC(s.DepthFunc),
        s.StencilEnable,
        s.StencilReadMask,
        s.StencilWriteMask
    };
    p.DepthStencilState.FrontFace = p.DepthStencilState.BackFace = {
        D3D12_STENCIL_OP(s.FrontFace.StencilFailOp), D3D12_STENCIL_OP(s.FrontFace.StencilDepthFailOp),
        D3D12_STENCIL_OP(s.FrontFace.StencilPassOp), D3D12_COMPARISON_FUNC(s.FrontFace.StencilFunc)
    };
    HRESULT hr = device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&pipelines[i]));
    if (!expect(hr == S_OK && pipelines[i], "order case %u PSO returned %08lx", i, hr))
      return verdict();
  }
  for (UINT path = 0; path < 3; path++) {
    step("output ordering, path %u: initialize", path);
    CHECK(allocator->Reset());
    CHECK(bundle_allocator->Reset());
    CHECK(list->Reset(allocator.Get(), nullptr));
    CHECK(forget(readback.Get()));
    ComPtr<ID3D12Resource> color, depth;
    CHECK(device->CreateCommittedResource(
        &props, D3D12_HEAP_FLAG_NONE, &color_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&color)
    ));
    CHECK(device->CreateCommittedResource(
        &props, D3D12_HEAP_FLAG_NONE, &depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&depth)
    ));
    auto counters = buffer(
        device.Get(), D3D12_HEAP_TYPE_DEFAULT, counter_bytes, D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
    );
    if (!expect(bool(counters), "order counter buffer missing"))
      return verdict();
    list->CopyBufferRegion(counters.Get(), 0, upload.Get(), 0, counter_bytes);
    transition(list.Get(), counters.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ComPtr<ID3D12DescriptorHeap> rtvs, dsvs;
    D3D12_DESCRIPTOR_HEAP_DESC rh{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1}, dh{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
    CHECK(device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&rtvs)));
    CHECK(device->CreateDescriptorHeap(&dh, IID_PPV_ARGS(&dsvs)));
    auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart(), dsv = dsvs->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(color.Get(), nullptr, rtv);
    device->CreateDepthStencilView(depth.Get(), nullptr, dsv);
    const float black[4]{};
    list->ClearRenderTargetView(rtv, black, 0, nullptr);
    list->ClearDepthStencilView(
        dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, om::order_stored_depth, om::order_stencil, 0, nullptr
    );
    list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    std::vector<ComPtr<ID3D12GraphicsCommandList>> bundles;
    for (UINT y = 0; y < cases.size(); y++) {
      const auto &k = cases[y];
      step("output ordering, path %u, case %u", path, y);
      D3D12_VIEWPORT viewport{0, float(y), float(width), 1, 0, 1};
      D3D12_RECT scissor{1, LONG(y), LONG(width - 1), LONG(y + 1)};
      list->RSSetViewports(1, &viewport);
      list->RSSetScissorRects(1, &scissor);
      auto record = [&](ID3D12GraphicsCommandList *commands) {
        commands->SetPipelineState(pipelines[y].Get());
        commands->SetGraphicsRootSignature(rs.Get());
        UINT words[] = {std::bit_cast<UINT>(k.raster_depth), std::bit_cast<UINT>(1 - k.raster_depth), k.discard, width};
        commands->SetGraphicsRoot32BitConstants(0, std::size(words), words, 0);
        commands->SetGraphicsRootUnorderedAccessView(1, counters->GetGPUVirtualAddress());
        commands->OMSetStencilRef(om::order_reference);
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (path == 2)
          commands->ExecuteIndirect(signature.Get(), 1, indirect.Get(), 0, nullptr, 0);
        else
          commands->DrawInstanced(3, 1, 0, 0);
      };
      list->BeginQuery(queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, y);
      if (path == 1) {
        auto &bundle = bundles.emplace_back();
        CHECK(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bundle_allocator.Get(), nullptr, IID_PPV_ARGS(&bundle)
        ));
        record(bundle.Get());
        CHECK(bundle->Close());
        list->ExecuteBundle(bundle.Get());
      } else
        record(list.Get());
      list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, y);
    }
    list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0, cases.size(), readback.Get(), counts_at);
    transition(list.Get(), counters.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(list.Get(), color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(list.Get(), depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), counters_at, counters.Get(), 0, counter_bytes);
    for (UINT copy = 0; copy <= std::size(planes); copy++) {
      D3D12_TEXTURE_COPY_LOCATION from{copy ? depth.Get() : color.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
          to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
      from.SubresourceIndex = copy ? copy - 1 : 0;
      to.PlacedFootprint = copy ? planes[copy - 1] : color_at;
      list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    const unsigned char *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    for (UINT y = 0; y < cases.size(); y++) {
      step("output ordering, path %u, case %u: readback", path, y);
      UINT64 want_count = 0, count;
      for (UINT x = 0; x < width; x++) {
        auto want = om::order_result(cases[y], x);
        want_count += want.occlusion;
        UINT color_word, depth_word, before, after;
        memcpy(&color_word, got + color_at.Offset + y * color_at.Footprint.RowPitch + x * sizeof(UINT), sizeof(UINT));
        memcpy(
            &depth_word, got + planes[0].Offset + y * planes[0].Footprint.RowPitch + x * (row_bytes[0] / width),
            sizeof(UINT)
        );
        auto stencil = got[planes[1].Offset + y * planes[1].Footprint.RowPitch + x * (row_bytes[1] / width)];
        auto p = got + counters_at + 2 * (y * width + x) * sizeof(UINT);
        memcpy(&before, p, sizeof(UINT));
        memcpy(&after, p + sizeof(UINT), sizeof(UINT));
        expect(
            color_word == want.color && depth_word == std::bit_cast<UINT>(want.depth) && stencil == want.stencil &&
                before == want.before && after == want.after,
            "pixel %u: color %u/%u, depth %#x/%#x, stencil %u/%u, UAV %u,%u/%u,%u", x, color_word, want.color,
            depth_word, std::bit_cast<UINT>(want.depth), stencil, want.stencil, before, after, want.before, want.after
        );
      }
      memcpy(&count, got + counts_at + y * sizeof(UINT64), sizeof(count));
      expect(count == want_count, "occlusion %llu/%llu", count, want_count);
    }
    readback->Unmap(0, nullptr);
  }
  return verdict();
}
