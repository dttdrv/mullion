// contract: a shader that reads a buffer view of a descriptor table past the view's end reads 0, and one that
// stores there stores nothing. raw buffers: "Out of bounds addressing on u#/t# of any given 32-bit component
// returns 0 for that component" (D3D11.3 22.4.10) and "Out of bounds addressing on u# means nothing is written to
// the out of bounds memory" (22.4.11); structured buffers the same by the element's index (22.4.12, 22.4.13); typed
// buffers by ld and store_uav_typed (22.4.6, 22.4.9). a root signature of version 1.0 keeps this for the views of
// its tables (DirectX-Specs, Resource Binding: only static descriptors of version 1.1 give it up). engines rely on
// it: Unreal's vertex shaders read the GPU scene by an instance's number, and the number of an instance that is
// not there reads zeros and nothing else happens.
// a structured, a raw and a typed buffer of ELEMENTS elements each, every word different and none 0; the views
// begin FIRST elements in and have COUNT. a vertex shader, a pixel shader and a compute shader read element
// `index` of each view; a pixel shader and a compute shader store `value` there. the indices: the view's first and
// last element, the first past it (still in the buffer, where the bytes are not 0), the first past the buffer, far
// past it, the index whose byte offset wraps 32 bits to 0, and the largest. held to: what was read is the element
// or 0, and after a store every word of the three buffers is what it was, but the one stored to when it is in
// the view.
#include "d3d12_test.hpp"
#include <algorithm>

static const char hlsl[] = R"hlsl(
struct Row { uint a, b; };
cbuffer Constants : register(b0) { uint index; uint value; };
StructuredBuffer<Row> rows : register(t0);
ByteAddressBuffer raw : register(t1);
Buffer<uint> typed : register(t2);
RWStructuredBuffer<Row> rows_out : register(u0);
RWByteAddressBuffer raw_out : register(u1);
RWBuffer<uint> typed_out : register(u2);
RWStructuredBuffer<uint4> result : register(u3);
uint4 reads() {
  Row row = rows[index];
  return uint4(row.a, row.b, raw.Load(4 * index), typed[index]);
}
void stores() {
  rows_out[index].b = value;
  raw_out.Store(4 * index, value);
  typed_out[index] = value;
}
struct V { float4 pos : SV_Position; nointerpolation uint4 read : READ; };
V corner(uint id, uint4 read) {
  V v;
  v.pos = float4(float2(id & 1, id >> 1) * 4 - 1, 0, 1);
  v.read = read;
  return v;
}
V vs(uint id : SV_VertexID) { return corner(id, 0); }
V vs_reads(uint id : SV_VertexID) { return corner(id, reads()); }
uint4 ps_passes(V v) : SV_Target { return v.read; }
uint4 ps_reads(V v) : SV_Target { return reads(); }
uint4 ps_stores(V v) : SV_Target { stores(); return 0; }
[numthreads(1, 1, 1)] void cs_reads() { result[0] = reads(); }
[numthreads(1, 1, 1)] void cs_stores() { stores(); }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  // the buffers in the order of the shader's registers, and the words an element of each has
  enum { Rows, Raw, Typed, Kinds };
  const UINT elements = 16, first = 4, count = 8, word = sizeof(UINT), words_of[Kinds] = {2, 1, 1};
  const DXGI_FORMAT format_of[Kinds] = {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_UINT};
  // buffer `kind` of the read ones (0) or the stored ones (1): where its words lie in one array of all, and its words
  auto begins = [&](UINT stored, UINT kind) {
    UINT at = 0;
    for (UINT before = 0; before < stored * Kinds + kind; before++)
      at += elements * words_of[before % Kinds];
    return at;
  };
  const UINT all_words = begins(2, 0);
  std::vector<UINT> pattern(all_words);
  for (UINT i = 0; i < all_words; i++)
    pattern[i] = i + 1;

  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_DESCRIPTOR_RANGE read_range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, Kinds}, stored_range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, Kinds};
  D3D12_ROOT_PARAMETER params[4] = {{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}, {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
                                    {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[0].DescriptorTable = {1, &read_range};
  params[1].DescriptorTable = {1, &stored_range};
  params[2].Constants = {0, 0, 2};
  params[3].Descriptor = {Kinds, 0};
  auto rs = root_signature(device.Get(), {(UINT)std::size(params), params});

  // what reads or stores, and in which stage
  struct Way {
    const char *what, *vs, *ps, *cs;
    bool stores;
    ComPtr<ID3D12PipelineState> pso;
  } ways[] = {{"a vertex shader reads", "vs_reads", "ps_passes"}, {"a pixel shader reads", "vs", "ps_reads"}, {"a compute shader reads", nullptr, nullptr, "cs_reads"},
              {"a pixel shader stores", "vs", "ps_stores", nullptr, true}, {"a compute shader stores", nullptr, nullptr, "cs_stores", true}};
  const DXGI_FORMAT target_format = DXGI_FORMAT_R32G32B32A32_UINT;
  for (auto &way : ways) {
    if (way.cs) {
      auto cs = compiler.compile(hlsl, way.cs, "cs");
      D3D12_COMPUTE_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(cs)};
      CHECK(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&way.pso)));
      continue;
    }
    auto vs = compiler.compile(hlsl, way.vs, "vs"), ps = compiler.compile(hlsl, way.ps, "ps");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = rs.Get();
    desc.VS = bytecode(vs), desc.PS = bytecode(ps);
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = ~0u;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1, desc.RTVFormats[0] = target_format;
    desc.SampleDesc = {1, 0};
    CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&way.pso)));
  }

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));

  // the words of all six buffers as they begin, the buffers, and their views: FIRST elements in, of COUNT elements
  const UINT64 all_bytes = all_words * word;
  auto staged = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, all_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto back = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, all_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  void *mapped;
  CHECK(staged->Map(0, nullptr, &mapped));
  memcpy(mapped, pattern.data(), all_bytes);
  ComPtr<ID3D12DescriptorHeap> views, rtv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC views_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2 * Kinds, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE},
      rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&views_desc, IID_PPV_ARGS(&views)));
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)));
  const UINT apart = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  const auto reading = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, storing = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  ComPtr<ID3D12Resource> buffers[2][Kinds];
  for (UINT stored = 0; stored < 2; stored++)
    for (UINT kind = 0; kind < Kinds; kind++) {
      const UINT64 bytes = elements * words_of[kind] * word;
      auto &made = buffers[stored][kind] = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COPY_DEST,
                                                  stored ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
      list->CopyBufferRegion(made.Get(), 0, staged.Get(), begins(stored, kind) * word, bytes);
      transition(list.Get(), made.Get(), D3D12_RESOURCE_STATE_COPY_DEST, stored ? storing : reading);
      auto handle = views->GetCPUDescriptorHandleForHeapStart();
      handle.ptr += (stored * Kinds + kind) * apart;
      const UINT stride = kind == Rows ? words_of[kind] * word : 0;
      if (stored) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{format_of[kind], D3D12_UAV_DIMENSION_BUFFER};
        view.Buffer = {first, count, stride, 0, kind == Raw ? D3D12_BUFFER_UAV_FLAG_RAW : D3D12_BUFFER_UAV_FLAG_NONE};
        device->CreateUnorderedAccessView(made.Get(), nullptr, &view, handle);
      } else {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{format_of[kind], D3D12_SRV_DIMENSION_BUFFER, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
        view.Buffer = {first, count, stride, kind == Raw ? D3D12_BUFFER_SRV_FLAG_RAW : D3D12_BUFFER_SRV_FLAG_NONE};
        device->CreateShaderResourceView(made.Get(), &view, handle);
      }
    }
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  const D3D12_HEAP_PROPERTIES gpu{D3D12_HEAP_TYPE_DEFAULT};
  const D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 1, 1, 1, 1, target_format, {1, 0},
                                        D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)));
  auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 target_bytes;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &target_bytes);
  const UINT read_words = 4;
  auto result = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, read_words * word, storing, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto read_back = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, std::max<UINT64>(target_bytes, read_words * word), D3D12_RESOURCE_STATE_COPY_DEST);

  // past the view and in the buffer, past the buffer, far past it, where the byte offset of a row wraps to 0, the largest
  const UINT indices[] = {0, count - 1, count, elements - first, 1u << 20, UINT((1ull << 32) / (words_of[Rows] * word)), ~0u};
  for (auto &way : ways)
    for (UINT index : indices) {
      step("%s element %#x of views of %u elements", way.what, index, count);
      // a value no word has
      const UINT value = 0x80000000u + index % count;
      if (FAILED(forget(back.Get())) || FAILED(forget(read_back.Get())) || FAILED(allocator->Reset()) ||
          FAILED(list->Reset(allocator.Get(), way.pso.Get()))) {
        expect(false, "the list could not be begun");
        continue;
      }
      // the stored buffers as they began
      for (UINT kind = 0; kind < Kinds; kind++) {
        auto to = buffers[1][kind].Get();
        transition(list.Get(), to, storing, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(to, 0, staged.Get(), begins(1, kind) * word, elements * words_of[kind] * word);
        transition(list.Get(), to, D3D12_RESOURCE_STATE_COPY_DEST, storing);
      }
      ID3D12DescriptorHeap *bound[] = {views.Get()};
      auto read_table = views->GetGPUDescriptorHandleForHeapStart(), stored_table = read_table;
      stored_table.ptr += Kinds * apart;
      const UINT constants[] = {index, value};
      list->SetDescriptorHeaps(1, bound);
      if (way.cs) {
        list->SetComputeRootSignature(rs.Get());
        list->SetComputeRootDescriptorTable(0, read_table);
        list->SetComputeRootDescriptorTable(1, stored_table);
        list->SetComputeRoot32BitConstants(2, std::size(constants), constants, 0);
        list->SetComputeRootUnorderedAccessView(3, result->GetGPUVirtualAddress());
        list->Dispatch(1, 1, 1);
        transition(list.Get(), result.Get(), storing, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(read_back.Get(), 0, result.Get(), 0, read_words * word);
        transition(list.Get(), result.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, storing);
      } else {
        const D3D12_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
        const D3D12_RECT scissor{0, 0, 1, 1};
        list->SetGraphicsRootSignature(rs.Get());
        list->SetGraphicsRootDescriptorTable(0, read_table);
        list->SetGraphicsRootDescriptorTable(1, stored_table);
        list->SetGraphicsRoot32BitConstants(2, std::size(constants), constants, 0);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
        transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
            to{read_back.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
      }
      for (UINT kind = 0; kind < Kinds; kind++) {
        auto from = buffers[1][kind].Get();
        transition(list.Get(), from, storing, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(back.Get(), begins(1, kind) * word, from, 0, elements * words_of[kind] * word);
        transition(list.Get(), from, D3D12_RESOURCE_STATE_COPY_SOURCE, storing);
      }
      HRESULT ran = submit(device.Get(), queue.Get(), list.Get());
      const UINT *read, *left;
      if (!expect(ran == S_OK, "the device did not live through it: %08lx", ran) || FAILED(read_back->Map(0, nullptr, (void **)&read)) ||
          FAILED(back->Map(0, nullptr, (void **)&left)))
        return verdict();
      // the element's words, in the read buffers and in the stored ones; a raw buffer is addressed in bytes, which
      // the shader counts in 32 bits
      const UINT raw_index = word * index / word;
      auto in_view = [&](UINT kind) { return (kind == Raw ? raw_index : index) < count; };
      auto element = [&](UINT stored, UINT kind) { return begins(stored, kind) + (first + (kind == Raw ? raw_index : index)) * words_of[kind]; };
      std::vector<UINT> want = pattern;
      if (way.stores) {
        for (UINT kind = 0; kind < Kinds; kind++)
          if (in_view(kind))
            want[element(1, kind) + words_of[kind] - 1] = value;
      } else {
        const UINT want_read[read_words] = {in_view(Rows) ? pattern[element(0, Rows)] : 0, in_view(Rows) ? pattern[element(0, Rows) + 1] : 0,
                                            in_view(Raw) ? pattern[element(0, Raw)] : 0, in_view(Typed) ? pattern[element(0, Typed)] : 0};
        expect(!memcmp(read, want_read, sizeof(want_read)), "read %#x %#x from the rows, %#x raw and %#x typed, want %#x %#x, %#x and %#x", read[0],
               read[1], read[2], read[3], want_read[0], want_read[1], want_read[2], want_read[3]);
      }
      unsigned wrong = 0;
      for (UINT i = begins(1, 0); i < all_words; i++)
        if (left[i] != want[i] && wrong++ < 4)
          expect(false, "word %u of the stored buffers is %#x, want %#x", i - begins(1, 0), left[i], want[i]);
      expect(wrong <= 4, "and %u more words", wrong - 4);
      read_back->Unmap(0, nullptr);
      back->Unmap(0, nullptr);
    }
  return verdict();
}
