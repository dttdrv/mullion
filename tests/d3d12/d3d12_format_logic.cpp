// contract: "Logic ops are supported only on renderable UINT formats. Implementations that expose Logic Ops
// support must support them for all renderable UINT formats." (D3D11.3 17.7.1; D3D12 inherits the format rules).
// D3D12_LOGIC_OP lists the same sixteen Boolean equations as 17.1. "The masks simply limit writes." (17.15).
// every component bit receives all four operand pairs and all-bit controls, with operands confined to the
// native format range (17.7.1 allows truncation or clamping above it). all operations and channel masks must
// produce the CPU's exact native bits through direct/indirect/bundle draws, on both shader front ends.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_logic_op
#include "d3d12_test.hpp"
#include "../depth_formats.hpp"
#include "../format_logic.hpp"

using namespace format_logic;

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler))
    return 77;
  auto vertex = compiler.compile(format_logic::hlsl, "vs", "vs"),
       pixel = compiler.compile(format_logic::hlsl, "ps", "ps");
  if (!expect(!vertex.empty() && !pixel.empty(), "logic HLSL did not compile"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
  D3D12_ROOT_PARAMETER parameter{D3D12_ROOT_PARAMETER_TYPE_SRV};
  auto root = root_signature(device.Get(), {1, &parameter});
  if (!expect(bool(root), "logic root signature was not created"))
    return verdict();
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator, bundle_allocator;
  ComPtr<ID3D12GraphicsCommandList> list, bundle;
  D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&bundle_allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(device->CreateCommandList(
      0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bundle_allocator.Get(), nullptr, IID_PPV_ARGS(&bundle)
  ));
  CHECK(list->Close());
  CHECK(bundle->Close());
  ComPtr<ID3D12CommandSignature> signature;
  D3D12_INDIRECT_ARGUMENT_DESC argument{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW};
  D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(D3D12_DRAW_ARGUMENTS), 1, &argument};
  CHECK(device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signature)));
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
  ComPtr<ID3D12DescriptorHeap> heap;
  D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
  auto rtv = heap->GetCPUDescriptorHandleForHeapStart();
  UINT checked = 0;
  auto typed = formats();
  const struct {
    DXGI_FORMAT id;
    const char *name;
  } candidates[] = {DXGI_FORMATS};
  for (auto candidate : candidates) {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{candidate.id};
    HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
    if (FAILED(hr)) {
      expect(
          hr == E_FAIL && !support.Support1 && !support.Support2, "%s unsupported query returned %08lx", candidate.name,
          hr
      );
      continue;
    }
    auto layout = std::find_if(typed.begin(), typed.end(), [&](const auto &f) { return f.id == candidate.id; });
    const bool legal =
        layout != typed.end() && layout->kind == Unsigned && (support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET);
    expect(
        bool(support.Support2 & D3D12_FORMAT_SUPPORT2_OUTPUT_MERGER_LOGIC_OP) ==
            (legal && bool(options.OutputMergerLogicOp)),
        "%s logic capability disagrees with format/global support", candidate.name
    );
    if (!legal)
      continue;
    const auto &f = *layout;
    checked++;
    std::vector<Pixel> source;
    std::vector<uint8_t> destination;
    inputs(f, source, destination);
    const UINT count = UINT(source.size());
    auto source_buffer = upload(source.data(), source.size() * sizeof(Pixel));
    D3D12_RESOURCE_DESC td{
        D3D12_RESOURCE_DIMENSION_TEXTURE2D,     0, count, 1, 1, 1, f.id, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
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
    std::vector<uint8_t> padded(bytes);
    memcpy(padded.data() + footprint.Offset, destination.data(), destination.size());
    auto initial = upload(padded.data(), bytes);
    auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!expect(source_buffer && initial && readback, "logic data buffers were not created"))
      return verdict();
    auto state = D3D12_RESOURCE_STATE_COPY_DEST;
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
    auto &blend = pd.BlendState.RenderTarget[0];
    blend.LogicOpEnable = TRUE;
    blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    for (UINT op = D3D12_LOGIC_OP_CLEAR; op <= D3D12_LOGIC_OP_OR_INVERTED; op++)
      for (UINT mask = 0; mask <= D3D12_COLOR_WRITE_ENABLE_ALL; mask++) {
        blend.LogicOp = D3D12_LOGIC_OP(op);
        blend.RenderTargetWriteMask = UINT8(mask);
        ComPtr<ID3D12PipelineState> pso;
        hr = device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso));
        step("%s op %u mask %#x", f.name.c_str(), op, mask);
        if (!options.OutputMergerLogicOp) {
          expect(FAILED(hr) && !pso, "unsupported logic pipeline creation returned %08lx", hr);
          continue;
        }
        if (!expect(hr == S_OK && pso, "supported logic pipeline creation returned %08lx", hr))
          return verdict();
        for (UINT path = 0; path < 3; path++) {
          CHECK(forget(readback.Get()));
          CHECK(allocator->Reset());
          CHECK(list->Reset(allocator.Get(), pso.Get()));
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
          D3D12_VIEWPORT viewport{0, 0, float(count), 1, 0, 1};
          D3D12_RECT scissor{0, 0, LONG(count), 1};
          list->RSSetViewports(1, &viewport);
          list->RSSetScissorRects(1, &scissor);
          if (path == 0)
            list->DrawInstanced(3, 1, 0, 0);
          else if (path == 1)
            list->ExecuteIndirect(signature.Get(), 1, indirect.Get(), 0, nullptr, 0);
          else {
            CHECK(bundle_allocator->Reset());
            CHECK(bundle->Reset(bundle_allocator.Get(), pso.Get()));
            bind(bundle.Get());
            bundle->DrawInstanced(3, 1, 0, 0);
            CHECK(bundle->Close());
            list->ExecuteBundle(bundle.Get());
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
          for (UINT i = 0; i < count; i++)
            if (!matches(
                    f, op, mask, source[i], destination.data() + i * f.bytes,
                    static_cast<const uint8_t *>(mapped) + footprint.Offset + i * f.bytes
                )) {
              expect(false, "path %u pixel %u differs from Boolean equation or channel mask", path, i);
              same = false;
              break;
            }
          readback->Unmap(0, nullptr);
          if (!same)
            return verdict();
        }
      }
  }
  expect(checked != 0, "no renderable UINT format was checked");
  return verdict();
}
