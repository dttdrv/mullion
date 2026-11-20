// contract: "The min and max blend ops ignore D3D11_BLEND modes" (D3D11.3 17.1); other operations use
// the selected color/alpha factors, including the second source (17.1/.2/.6; D3D12_BLEND/BLEND_OP).
// "For all formats, there is a clamp to the RenderTarget range after blend, before writing values out
// to memory." (17.5). all advertised blendable formats receive every legal factor pair, every operation,
// independent color/alpha operations and every write mask. exact binary/small-integer data distinguishes
// mappings, clamps, missing destination alpha and second-source values without assuming optional fixed-point
// intermediate precision. unsigned small floats receive no negative inputs because their input clamp is
// optional (17.5/19.3.4). float arithmetic preserves generated signed zeros. DXBC and DXIL share the oracle.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_blend
#include "d3d12_test.hpp"
#include "../depth_formats.hpp"
#include "../format_blend.hpp"

using namespace format_blend;

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler))
    return 77;
  auto vertex = compiler.compile(format_blend::hlsl, "vs", "vs"),
       pixel = compiler.compile(format_blend::hlsl, "ps", "ps");
  if (!expect(!vertex.empty() && !pixel.empty(), "blend HLSL did not compile"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS13 options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS13, &options, sizeof(options)));
  auto cases = equations(options.AlphaBlendFactorSupported);
  if (!expect(cases.size() <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION, "blend cases exceed the image limit"))
    return verdict();
  D3D12_ROOT_PARAMETER parameter{D3D12_ROOT_PARAMETER_TYPE_SRV};
  auto root = root_signature(device.Get(), {1, &parameter});
  if (!expect(bool(root), "blend root signature was not created"))
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
  auto upload = [&](const void *data, UINT64 bytes) {
    auto resource = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!resource)
      return resource;
    void *mapped;
    if (FAILED(resource->Map(0, nullptr, &mapped)))
      return ComPtr<ID3D12Resource>{};
    memcpy(mapped, data, bytes);
    resource->Unmap(0, nullptr);
    return resource;
  };
  const D3D12_DRAW_ARGUMENTS draw{3, 1, 0, 0};
  auto indirect = upload(&draw, sizeof(draw));
  if (!expect(bool(indirect), "draw arguments were not uploaded"))
    return verdict();
  ComPtr<ID3D12CommandSignature> signature;
  D3D12_INDIRECT_ARGUMENT_DESC argument{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW};
  D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(draw), 1, &argument};
  CHECK(device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signature)));
  ComPtr<ID3D12DescriptorHeap> heap;
  D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
  auto rtv = heap->GetCPUDescriptorHandleForHeapStart();
  UINT tested = 0;
  for (const auto &f : formats()) {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{f.id};
    HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
    if (FAILED(hr)) {
      expect(
          hr == E_FAIL && !support.Support1 && !support.Support2, "%s unsupported query returned %08lx", f.name.c_str(),
          hr
      );
      continue;
    }
    if (!(support.Support1 & D3D12_FORMAT_SUPPORT1_BLENDABLE))
      continue;
    if (!expect(
            f.kind != Unsigned && f.kind != Signed && f.kind != Shared &&
                (support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET),
            "%s invalid blend capability", f.name.c_str()
        ))
      continue;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root.Get();
    pd.VS = bytecode(vertex);
    pd.PS = bytecode(pixel);
    pd.SampleMask = ~0u;
    pd.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = f.id;
    pd.SampleDesc.Count = 1;
    if (!options.AlphaBlendFactorSupported)
      for (auto unsupported : {D3D12_BLEND_ALPHA_FACTOR, D3D12_BLEND_INV_ALPHA_FACTOR}) {
        pd.BlendState.RenderTarget[0] = {
            TRUE,
            FALSE,
            unsupported,
            D3D12_BLEND_ZERO,
            D3D12_BLEND_OP_ADD,
            D3D12_BLEND_ONE,
            D3D12_BLEND_ZERO,
            D3D12_BLEND_OP_ADD,
            D3D12_LOGIC_OP_NOOP,
            D3D12_COLOR_WRITE_ENABLE_ALL
        };
        ComPtr<ID3D12PipelineState> rejected;
        HRESULT hr = device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&rejected));
        expect(FAILED(hr) && !rejected, "unsupported alpha-factor pipeline returned %08lx", hr);
      }
    std::vector<ComPtr<ID3D12PipelineState>> states(cases.size());
    for (UINT row = 0; row < cases.size(); row++) {
      const auto &e = cases[row];
      pd.BlendState.RenderTarget[0] = {
          TRUE,
          FALSE,
          D3D12_BLEND(e.source),
          D3D12_BLEND(e.destination),
          D3D12_BLEND_OP(e.operation),
          D3D12_BLEND(e.alpha_source),
          D3D12_BLEND(e.alpha_destination),
          D3D12_BLEND_OP(e.alpha_operation),
          D3D12_LOGIC_OP_NOOP,
          UINT8(e.write_mask)
      };
      CHECK(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&states[row])));
    }
    const UINT height = UINT(cases.size());
    D3D12_RESOURCE_DESC td{
        D3D12_RESOURCE_DIMENSION_TEXTURE2D,     0, width, height, 1, 1, f.id, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
    };
    D3D12_HEAP_PROPERTIES properties{D3D12_HEAP_TYPE_DEFAULT};
    ComPtr<ID3D12Resource> target;
    CHECK(device->CreateCommittedResource(
        &properties, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&target)
    ));
    device->CreateRenderTargetView(target.Get(), nullptr, rtv);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT64 bytes;
    device->GetCopyableFootprints(&td, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!expect(bool(readback), "blend readback buffer was not created"))
      return verdict();
    auto state = D3D12_RESOURCE_STATE_COPY_DEST;
    for (UINT phase = 0; phase < phase_count; phase++) {
      std::vector<Source> source;
      std::vector<uint8_t> destination;
      inputs(f, phase, source, destination);
      auto source_buffer = upload(source.data(), source.size() * sizeof(Source));
      std::vector<uint8_t> padded(bytes);
      for (UINT row = 0; row < height; row++)
        memcpy(
            padded.data() + footprint.Offset + row * footprint.Footprint.RowPitch, destination.data(),
            destination.size()
        );
      auto initial = upload(padded.data(), bytes);
      if (!expect(source_buffer && initial, "blend data was not uploaded"))
        return verdict();
      for (UINT path = 0; path < 3; path++) {
        step("%s phase %u path %u: %u blend equations/masks", f.name.c_str(), phase, path, height);
        CHECK(forget(readback.Get()));
        CHECK(allocator->Reset());
        CHECK(list->Reset(allocator.Get(), nullptr));
        if (state != D3D12_RESOURCE_STATE_COPY_DEST)
          transition(list.Get(), target.Get(), state, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION from{initial.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
            to{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        from.PlacedFootprint = footprint;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
        auto bind = [&](ID3D12GraphicsCommandList *commands) {
          commands->SetGraphicsRootSignature(root.Get());
          commands->SetGraphicsRootShaderResourceView(0, source_buffer->GetGPUVirtualAddress());
          commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        };
        bind(list.Get());
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        D3D12_RECT scissor{0, 0, LONG(width), LONG(height)};
        list->RSSetScissorRects(1, &scissor);
        const auto constant = constants(phase);
        list->OMSetBlendFactor(constant.data());
        std::vector<ComPtr<ID3D12GraphicsCommandList>> bundles;
        if (path == 2) {
          CHECK(bundle_allocator->Reset());
          bundles.resize(height);
        }
        for (UINT row = 0; row < height; row++) {
          D3D12_VIEWPORT viewport{0, float(row), float(width), 1, 0, 1};
          list->RSSetViewports(1, &viewport);
          if (path == 2) {
            CHECK(device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bundle_allocator.Get(), states[row].Get(),
                IID_PPV_ARGS(&bundles[row])
            ));
            bind(bundles[row].Get());
            bundles[row]->DrawInstanced(3, 1, 0, 0);
            CHECK(bundles[row]->Close());
            list->ExecuteBundle(bundles[row].Get());
          } else {
            list->SetPipelineState(states[row].Get());
            if (path == 1)
              list->ExecuteIndirect(signature.Get(), 1, indirect.Get(), 0, nullptr, 0);
            else
              list->DrawInstanced(3, 1, 0, 0);
          }
        }
        transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        state = D3D12_RESOURCE_STATE_COPY_SOURCE;
        from = {target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        to = {readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        to.PlacedFootprint = footprint;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        CHECK(submit(device.Get(), queue.Get(), list.Get()));
        void *mapped;
        CHECK(readback->Map(0, nullptr, &mapped));
        bool same = true;
        for (UINT row = 0; same && row < height; row++)
          for (UINT x = 0; x < width; x++)
            if (!matches(
                    f, cases[row], phase, source[x], destination.data() + x * f.bytes,
                    static_cast<const uint8_t *>(mapped) + footprint.Offset + row * footprint.Footprint.RowPitch +
                        x * f.bytes
                )) {
              const auto &e = cases[row];
              expect(
                  false, "row %u pixel %u: ops %u,%u factors %u,%u alpha %u,%u mask %#x differs", row, x, e.operation,
                  e.alpha_operation, e.source, e.destination, e.alpha_source, e.alpha_destination, e.write_mask
              );
              same = false;
              break;
            }
        readback->Unmap(0, nullptr);
        tested++;
        if (!same)
          return verdict();
      }
    }
  }
  expect(tested != 0, "no blendable format ran");
  return verdict();
}
