// contract: D3D11.3 17.7.1 requires logic operations on every renderable UINT format when exposed; 17.1 lists
// all 16 bitwise truth tables. 17.15: "The masks simply limit writes." source: D3D11_3_FunctionalSpec.htm.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_logic_op
// header-derived UINT layouts and the CPU truth tables in om_logic.hpp check pixels, not pipeline creation alone.
#include "d3d12_test.hpp"
#include "../om_logic.hpp"

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto vs = compiler.compile(om::logic_hlsl, "vs", "vs"), ps = compiler.compile(om::logic_hlsl, "ps", "ps");
  if (!expect(!vs.empty() && !ps.empty(), "logic HLSL did not compile"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
  if (!options.OutputMergerLogicOp) {
    printf("skipped: output merger logic ops not supported\n");
    return 77;
  }
  D3D12_ROOT_PARAMETER param{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
  param.Constants.Num32BitValues = 4;
  auto rs = root_signature(device.Get(), {1, &param});
  if (!expect(bool(rs), "root signature missing"))
    return verdict();
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator, bundle_allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&bundle_allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  D3D12_INDIRECT_ARGUMENT_DESC argument{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW};
  D3D12_COMMAND_SIGNATURE_DESC sd{sizeof(D3D12_DRAW_ARGUMENTS), 1, &argument};
  ComPtr<ID3D12CommandSignature> signature;
  CHECK(device->CreateCommandSignature(&sd, nullptr, IID_PPV_ARGS(&signature)));
  auto indirect =
      buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(D3D12_DRAW_ARGUMENTS), D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(bool(indirect), "indirect buffer missing"))
    return verdict();
  D3D12_DRAW_ARGUMENTS *arguments;
  CHECK(indirect->Map(0, nullptr, (void **)&arguments));
  *arguments = {3, 1, 0, 0};
  indirect->Unmap(0, nullptr);
  const auto formats = om::logic_formats();
  if (!expect(!formats.empty(), "no UINT formats read from header"))
    return verdict();
  UINT exercised = 0;
  for (auto f : formats) {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{f.value};
    CHECK(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)));
    if (!(support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET))
      continue;
    exercised++;
    expect(
        support.Support2 & D3D12_FORMAT_SUPPORT2_OUTPUT_MERGER_LOGIC_OP,
        "renderable UINT format %u lacks logic-op support bit", f.value
    );
    constexpr UINT count = D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT;
    D3D12_RESOURCE_DESC td{
        D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        0,
        om::logic_width,
        UINT(std::size(om::logic_patterns)),
        1,
        1,
        f.value,
        {1, 0},
        D3D12_TEXTURE_LAYOUT_UNKNOWN,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
    };
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT at;
    UINT64 bytes;
    device->GetCopyableFootprints(&td, 0, 1, 0, &at, nullptr, nullptr, &bytes);
    const UINT64 placement = (bytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) /
                             D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
    auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, placement * count, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, placement * count, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!expect(upload && readback, "upload/readback missing"))
      return verdict();
    unsigned char *mapped;
    CHECK(upload->Map(0, nullptr, (void **)&mapped));
    for (UINT slot = 0; slot < count; slot++) {
      auto initial = om::logic_seed(f, slot);
      for (UINT y = 0; y < std::size(om::logic_patterns); y++)
        memcpy(
            mapped + slot * placement + at.Offset + y * at.Footprint.RowPitch,
            initial.data() + y * om::logic_width * f.stride, om::logic_width * f.stride
        );
    }
    upload->Unmap(0, nullptr);
    for (UINT path = 0; path < 3; path++) {
      step("logic format %u, path %u", f.value, path);
      CHECK(allocator->Reset());
      CHECK(bundle_allocator->Reset());
      CHECK(list->Reset(allocator.Get(), nullptr));
      CHECK(forget(readback.Get()));
      ComPtr<ID3D12DescriptorHeap> heap;
      D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, count};
      CHECK(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
      ComPtr<ID3D12Resource> textures[count];
      D3D12_CPU_DESCRIPTOR_HANDLE views[count];
      D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
      for (UINT slot = 0; slot < count; slot++) {
        CHECK(device->CreateCommittedResource(
            &props, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&textures[slot])
        ));
        D3D12_TEXTURE_COPY_LOCATION from{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
            to{textures[slot].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        from.PlacedFootprint = at;
        from.PlacedFootprint.Offset += slot * placement;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        transition(
            list.Get(), textures[slot].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET
        );
        views[slot] = heap->GetCPUDescriptorHandleForHeapStart();
        views[slot].ptr += slot * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        device->CreateRenderTargetView(textures[slot].Get(), nullptr, views[slot]);
      }
      list->OMSetRenderTargets(count, views, FALSE, nullptr);
      D3D12_VIEWPORT viewport{0, 0, float(om::logic_width), float(std::size(om::logic_patterns)), 0, 1};
      list->RSSetViewports(1, &viewport);
      std::vector<ComPtr<ID3D12PipelineState>> pipelines;
      std::vector<ComPtr<ID3D12GraphicsCommandList>> bundles;
      for (UINT op = D3D12_LOGIC_OP_CLEAR; op <= D3D12_LOGIC_OP_OR_INVERTED; op++)
        for (UINT mask = 0; mask < om::logic_masks; mask++) {
          D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
          p.pRootSignature = rs.Get();
          p.VS = bytecode(vs);
          p.PS = bytecode(ps);
          p.NumRenderTargets = count;
          for (auto &format : p.RTVFormats)
            format = f.value;
          p.SampleDesc = {1, 0};
          p.SampleMask = ~0u;
          p.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
          p.RasterizerState.DepthClipEnable = TRUE;
          p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
          p.BlendState.RenderTarget[0].LogicOpEnable = TRUE;
          p.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP(op);
          p.BlendState.RenderTarget[0].RenderTargetWriteMask = UINT8(mask);
          auto &pipeline = pipelines.emplace_back();
          CHECK(device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&pipeline)));
          for (UINT y = 0; y < std::size(om::logic_patterns); y++) {
            UINT x = op * om::logic_masks + mask + 1;
            D3D12_RECT rect{LONG(x), LONG(y), LONG(x + 1), LONG(y + 1)};
            list->RSSetScissorRects(1, &rect);
            auto record = [&](ID3D12GraphicsCommandList *commands) {
              commands->SetPipelineState(pipeline.Get());
              commands->SetGraphicsRootSignature(rs.Get());
              UINT source[4]{};
              for (UINT c = 0; c < f.channels; c++)
                source[c] = om::logic_source(f, y, c);
              commands->SetGraphicsRoot32BitConstants(0, std::size(source), source, 0);
              commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
              if (path == 2)
                commands->ExecuteIndirect(signature.Get(), 1, indirect.Get(), 0, nullptr, 0);
              else
                commands->DrawInstanced(3, 1, 0, 0);
            };
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
          }
        }
      for (UINT slot = 0; slot < count; slot++) {
        transition(
            list.Get(), textures[slot].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE
        );
        D3D12_TEXTURE_COPY_LOCATION from{textures[slot].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
            to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        to.PlacedFootprint = at;
        to.PlacedFootprint.Offset += slot * placement;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
      }
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      const unsigned char *got;
      CHECK(readback->Map(0, nullptr, (void **)&got));
      for (UINT slot = 0; slot < count; slot++) {
        step("logic format %u, path %u, slot %u: bytes", f.value, path, slot);
        om::check_logic(f, slot, got + slot * placement + at.Offset, at.Footprint.RowPitch);
      }
      readback->Unmap(0, nullptr);
    }
  }
  expect(exercised, "no renderable UINT format exercised");
  return verdict();
}
