// contract: every advertised vertex format converts the input bytes before the vertex shader reads them.
// "The starting n-bit value is converted to float (0.0f, 1.0f, 2.0f, etc.) and then divided by (2n-1)."
// (D3D11.3 3.2.3.5). "the return is 0 in all non-missing components of the format" defines buffer-end reads
// (8.19.2); 19.1.3.3 supplies missing components. the CPU reference derives signed and float conversions from
// section 3.2 and component layout from 19.1.3.1. D3D12_VERTEX_BUFFER_VIEW.SizeInBytes defines the bound extent.
// stream output retains these bits for direct, bundle and indirect draws, both shader front ends, packed and
// padded vertex layouts. the final vertex is out of bounds, and the untouched output guard detects extra writes.
#include "d3d12_test.hpp"
#include "../depth_formats.hpp"
#include "../format_values.hpp"

int
main(int argc, char **argv) {
  using namespace format_values;
  Compiler compiler;
  if (!front_end(argc, argv, compiler))
    return 77;
  std::string codes[3];
  for (UINT kind = 0; kind < std::size(codes); kind++) {
    codes[kind] = compiler.compile(vertices_hlsl, "vs", "vs", defines(kind, 0));
    if (!expect(!codes[kind].empty(), "vertex HLSL did not compile"))
      return verdict();
  }
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  auto root = root_signature(
      device.Get(),
      {0, nullptr, 0, nullptr,
       D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT}
  );
  if (!expect(bool(root), "root signature was not created"))
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
  D3D12_COMMAND_SIGNATURE_DESC sd{sizeof(D3D12_DRAW_ARGUMENTS), 1, &argument};
  CHECK(device->CreateCommandSignature(&sd, nullptr, IID_PPV_ARGS(&signature)));
  UINT tested = 0;
  for (const auto &f : formats()) {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT caps{f.id};
    HRESULT support = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &caps, sizeof(caps));
    step("%s vertex capability", f.name.c_str());
    if (required_vertex(f))
      expect(
          SUCCEEDED(support) && (caps.Support1 & D3D12_FORMAT_SUPPORT1_IA_VERTEX_BUFFER),
          "required vertex format is not advertised (D3D11.3 19.1.4)"
      );
    if (FAILED(support) || !(caps.Support1 & D3D12_FORMAT_SUPPORT1_IA_VERTEX_BUFFER))
      continue;
    auto bytes = inputs(f);
    const UINT count = UINT(bytes.size() / f.bytes), output_stride = sizeof(Pixel);
    const UINT output_bytes = (count + 2) * output_stride, total_bytes = output_bytes + sizeof(UINT64);
    std::vector<uint8_t> zero(f.bytes);
    for (UINT padding : {0u, UINT(sizeof(Pixel))}) {
      const UINT stride = f.bytes + 2 * padding, offset = padding;
      std::vector<uint8_t> interleaved(offset + count * stride, uint8_t(~0u));
      for (UINT i = 0; i < count; i++)
        memcpy(interleaved.data() + offset + i * stride + padding, bytes.data() + i * f.bytes, f.bytes);
      auto vertices =
          buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, interleaved.size(), D3D12_RESOURCE_STATE_GENERIC_READ);
      auto args =
          buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(D3D12_DRAW_ARGUMENTS), D3D12_RESOURCE_STATE_GENERIC_READ);
      auto initial = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, total_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
      auto output = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, total_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
      auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, total_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
      if (!expect(vertices && args && initial && output && readback, "vertex buffers were not created"))
        return verdict();
      void *mapped;
      CHECK(vertices->Map(0, nullptr, &mapped));
      memcpy(mapped, interleaved.data(), interleaved.size());
      vertices->Unmap(0, nullptr);
      CHECK(args->Map(0, nullptr, &mapped));
      const D3D12_DRAW_ARGUMENTS draw{count + 1, 1, 0, 0};
      memcpy(mapped, &draw, sizeof(draw));
      args->Unmap(0, nullptr);
      CHECK(initial->Map(0, nullptr, &mapped));
      memset(mapped, 0xff, output_bytes);
      const UINT64 empty = 0;
      memcpy(static_cast<uint8_t *>(mapped) + output_bytes, &empty, sizeof(empty));
      initial->Unmap(0, nullptr);
      const D3D12_INPUT_ELEMENT_DESC element{"DATA", 0, f.id, 0, padding, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA};
      const D3D12_SO_DECLARATION_ENTRY entry{0, "VALUE", 0, 0, UINT8(std::tuple_size_v<Pixel>), 0};
      D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{root.Get(), bytecode(codes[f.shader_kind()])};
      pd.InputLayout = {&element, 1};
      pd.StreamOutput = {&entry, 1, &output_stride, 1, D3D12_SO_NO_RASTERIZED_STREAM};
      pd.SampleMask = ~0u;
      pd.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
      pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
      pd.SampleDesc = {1, 0};
      ComPtr<ID3D12PipelineState> pipeline;
      CHECK(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pipeline)));
      D3D12_VERTEX_BUFFER_VIEW vbv{
          vertices->GetGPUVirtualAddress() + offset, UINT(interleaved.size()) - offset, stride
      };
      D3D12_STREAM_OUTPUT_BUFFER_VIEW sov{
          output->GetGPUVirtualAddress(), output_bytes, output->GetGPUVirtualAddress() + output_bytes
      };
      for (UINT path = 0; path < 3; path++) {
        step("%s, stride %u, offsets %u, path %u", f.name.c_str(), stride, padding, path);
        CHECK(allocator->Reset());
        CHECK(list->Reset(allocator.Get(), pipeline.Get()));
        CHECK(forget(readback.Get()));
        if (path)
          transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(output.Get(), initial.Get());
        transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
        list->SOSetTargets(0, 1, &sov);
        auto bind = [&](ID3D12GraphicsCommandList *commands) {
          commands->SetGraphicsRootSignature(root.Get());
          commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
          commands->IASetVertexBuffers(0, 1, &vbv);
        };
        bind(list.Get());
        if (path == 2) {
          CHECK(bundle_allocator->Reset());
          CHECK(bundle->Reset(bundle_allocator.Get(), pipeline.Get()));
          bind(bundle.Get());
          bundle->DrawInstanced(count + 1, 1, 0, 0);
          CHECK(bundle->Close());
          list->ExecuteBundle(bundle.Get());
        } else if (path == 1)
          list->ExecuteIndirect(signature.Get(), 1, args.Get(), 0, nullptr, 0);
        else
          list->DrawInstanced(count + 1, 1, 0, 0);
        list->SOSetTargets(0, 0, nullptr);
        transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyResource(readback.Get(), output.Get());
        CHECK(submit(device.Get(), queue.Get(), list.Get()));
        CHECK(readback->Map(0, nullptr, &mapped));
        bool same = true;
        for (UINT i = 0; i <= count; i++) {
          Pixel value;
          memcpy(value.data(), static_cast<uint8_t *>(mapped) + i * sizeof(Pixel), sizeof(Pixel));
          if (!loaded(f, i == count ? zero.data() : bytes.data() + i * f.bytes, value)) {
            expect(false, "vertex %u conversion or buffer-end default differs", i);
            same = false;
            break;
          }
        }
        Pixel guard{~0u, ~0u, ~0u, ~0u};
        same &= expect(
            !memcmp(static_cast<uint8_t *>(mapped) + (count + 1) * sizeof(Pixel), guard.data(), sizeof(guard)),
            "stream output wrote beyond the draw"
        );
        UINT64 filled;
        memcpy(&filled, static_cast<uint8_t *>(mapped) + output_bytes, sizeof(filled));
        same &= expect(filled == UINT64(count + 1) * output_stride, "stream output size %llu is wrong", filled);
        readback->Unmap(0, nullptr);
        if (!same)
          return verdict();
        tested++;
      }
    }
  }
  expect(tested != 0, "no vertex formats were exercised");
  return verdict();
}
