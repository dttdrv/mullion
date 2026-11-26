// contract: see ../ia_formats.hpp for required/optional formats, conversion rules and enumerated input encodings.
// D3D11.3 14.2.1: stream output "must simply dump out the 32 bits (per component) of data out unaltered".
// compare all converted words and the filled-size counter, including untouched guard words, through direct and
// bundled draws. the counter is a 32-bit quantity; one pass puts a nonzero guard immediately after it.
// https://learn.microsoft.com/en-us/windows/win32/direct3d12/stream-output-counters
// the runner compiles the same input signatures through DXBC and DXIL.
#include "d3d12_test.hpp"
#include "../ia_formats.hpp"

int
main(int argc, char **argv) {
  using namespace ia_formats;
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  step("compile all shaders before creating a device");
  const char *entries[] = {"as_float", "as_uint", "as_int"};
  std::string codes[std::size(entries)];
  for (UINT kind = 0; kind < std::size(entries); kind++) {
    codes[kind] = compiler.compile(hlsl, entries[kind], "vs");
    if (!expect(!codes[kind].empty(), "HLSL did not compile"))
      return verdict();
  }
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  auto root = root_signature(device.Get(), {0, nullptr, 0, nullptr,
      D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT});
  if (!expect(bool(root), "root signature creation failed"))
    return verdict();
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator, bundle_allocator;
  ComPtr<ID3D12GraphicsCommandList> list, bundle;
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&bundle_allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bundle_allocator.Get(), nullptr, IID_PPV_ARGS(&bundle)));
  CHECK(list->Close());
  CHECK(bundle->Close());
  const UINT output_stride = sizeof(UINT[4]), sentinel = 0xa5a5a5a5;
  const D3D12_SO_DECLARATION_ENTRY declaration{0, "VALUE", 0, 0, 4, 0};
  for (auto format : other_formats()) {
    step("non-IA format %u: capability and rejected pipeline", UINT(format));
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
    const HRESULT queried = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
    expect(queried == S_OK || queried == E_INVALIDARG || queried == E_FAIL, "format query returned %#lx", queried);
    if (queried == S_OK)
      expect(!(support.Support1 & D3D12_FORMAT_SUPPORT1_IA_VERTEX_BUFFER), "non-IA format advertised for vertex input");
    const D3D12_INPUT_ELEMENT_DESC element{"DATA", 0, format};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{root.Get(), bytecode(codes[0])};
    desc.InputLayout = {&element, 1};
    desc.StreamOutput = {&declaration, 1, &output_stride, 1, D3D12_SO_NO_RASTERIZED_STREAM};
    desc.SampleMask = ~0u;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    desc.SampleDesc = {1, 0};
    ComPtr<ID3D12PipelineState> pso;
    const HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
    expect(hr == E_INVALIDARG || hr == E_FAIL, "non-IA pipeline returned %#lx", hr);
    expect(!pso, "non-IA format returned a pipeline");
  }
  for (const auto &f : formats) {
    step("%s: capability and pipeline", f.name);
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{f.format};
    const HRESULT queried = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
    expect(queried == S_OK || (!f.required && (queried == E_INVALIDARG || queried == E_FAIL)), "format query returned %#lx", queried);
    const bool supported = queried == S_OK && (support.Support1 & D3D12_FORMAT_SUPPORT1_IA_VERTEX_BUFFER);
    expect(!f.required || supported, "required vertex format is not advertised");
    const UINT kind = f.kind == Uint ? 1 : f.kind == Sint ? 2 : 0;
    for (const auto &input_layout : layouts(f, D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, D3D12_APPEND_ALIGNED_ELEMENT,
                                            D3D12_STANDARD_MAXIMUM_ELEMENT_ALIGNMENT_BYTE_MULTIPLE)) {
      step("%s: %s", f.name, input_layout.name);
      std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
      for (const auto &e : input_layout.elements)
        elements.push_back({e.name, 0, e.format, e.slot, e.offset, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0});
      D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{root.Get(), bytecode(codes[kind])};
      desc.InputLayout = {elements.data(), UINT(elements.size())};
      desc.StreamOutput = {&declaration, 1, &output_stride, 1, D3D12_SO_NO_RASTERIZED_STREAM};
      desc.SampleMask = ~0u;
      desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
      desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
      desc.SampleDesc = {1, 0};
      ComPtr<ID3D12PipelineState> pso;
      const HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
      expect(supported ? hr == S_OK : FAILED(hr), "pipeline returned %#lx, advertised=%u", hr, supported);
      expect(bool(pso) == SUCCEEDED(hr), "wrong pipeline out parameter");
      if (!supported || FAILED(hr))
        continue;
      auto bytes = data(f);
      bytes.insert(bytes.begin(), input_layout.binding_offset + input_layout.element_offset, uint8_t(sentinel));
      const UINT vertices = count(f), input_stride = stride(f), output_bytes = (vertices + 1) * output_stride;
      const UINT counter_bytes = 2 * sizeof(UINT);
      auto input = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes.size(), D3D12_RESOURCE_STATE_GENERIC_READ);
      auto initial = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, counter_bytes + output_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
      auto output = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, output_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
      auto counter = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, counter_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
      auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, output_bytes + counter_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
      if (!expect(input && initial && output && counter && readback, "buffer creation failed"))
        return verdict();
      void *mapped;
      CHECK(input->Map(0, nullptr, &mapped));
      memcpy(mapped, bytes.data(), bytes.size());
      input->Unmap(0, nullptr);
      CHECK(initial->Map(0, nullptr, &mapped));
      memset(mapped, 0, counter_bytes);
      std::vector<UINT> fill(output_bytes / sizeof(UINT), sentinel);
      memcpy(static_cast<char *>(mapped) + counter_bytes, fill.data(), output_bytes);
      initial->Unmap(0, nullptr);
      const D3D12_VERTEX_BUFFER_VIEW vb{input->GetGPUVirtualAddress() + input_layout.binding_offset,
                                       UINT(bytes.size()) - input_layout.binding_offset, input_stride};
      const D3D12_STREAM_OUTPUT_BUFFER_VIEW so{output->GetGPUVirtualAddress(), output_bytes, counter->GetGPUVirtualAddress()};
      for (UINT path : {0u, 1u, 2u}) {
        const bool bundled = path == 1, guarded = path == 2;
        if (guarded && (f.format != DXGI_FORMAT_R8G8B8A8_UNORM || input_layout.slot != 0))
          continue;
        if (guarded) {
          CHECK(initial->Map(0, nullptr, &mapped));
          memcpy(static_cast<char *>(mapped) + sizeof(UINT), &sentinel, sizeof(sentinel));
          initial->Unmap(0, nullptr);
        }
        step("%s: %u encodings, bundled=%u, counter guard=%u", f.name, vertices, bundled, guarded);
        CHECK(forget(readback.Get()));
        CHECK(allocator->Reset());
        CHECK(list->Reset(allocator.Get(), pso.Get()));
        list->CopyBufferRegion(output.Get(), 0, initial.Get(), counter_bytes, output_bytes);
        list->CopyBufferRegion(counter.Get(), 0, initial.Get(), 0, counter_bytes);
        transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
        transition(list.Get(), counter.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
        list->SetGraphicsRootSignature(root.Get());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
        list->IASetVertexBuffers(input_layout.slot, 1, &vb);
        list->SOSetTargets(0, 1, &so);
        if (bundled) {
          CHECK(bundle_allocator->Reset());
          CHECK(bundle->Reset(bundle_allocator.Get(), pso.Get()));
          bundle->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
          bundle->DrawInstanced(vertices, 1, 0, 0);
          CHECK(bundle->Close());
          list->ExecuteBundle(bundle.Get());
        } else {
          list->DrawInstanced(vertices, 1, 0, 0);
        }
        list->SOSetTargets(0, 0, nullptr);
        transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(list.Get(), counter.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, output_bytes);
        list->CopyBufferRegion(readback.Get(), output_bytes, counter.Get(), 0, counter_bytes);
        transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        transition(list.Get(), counter.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        CHECK(submit(device.Get(), queue.Get(), list.Get()));
        const UINT *got;
        CHECK(readback->Map(0, nullptr, (void **)&got));
        UINT wrong = 0;
        for (UINT v = 0; v < vertices; v++)
          for (UINT c = 0; c < 4; c++) {
            const UINT out = f.bgra && c < 3 ? 2 - c : c;
            if (!matches(f, v, c, got[v * 4 + out]) && wrong++ < 4)
              expect(false, "vertex %u component %u is %#x, want %#x", v, out, got[v * 4 + out], expected(f, v, c));
          }
        expect(wrong <= 4, "%u additional wrong components", wrong > 4 ? wrong - 4 : 0);
        for (UINT c = 0; c < 4; c++)
          expect(got[vertices * 4 + c] == sentinel, "wrote beyond the last vertex");
        UINT filled[2];
        memcpy(filled, reinterpret_cast<const char *>(got) + output_bytes, sizeof(filled));
        expect(filled[0] == vertices * output_stride, "filled-size is %u, want %u", filled[0], vertices * output_stride);
        expect(filled[1] == (guarded ? sentinel : 0), "word after filled-size changed to %#x", filled[1]);
        readback->Unmap(0, nullptr);
      }
    }
  }
  return verdict();
}
