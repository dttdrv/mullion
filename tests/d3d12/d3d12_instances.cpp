// contract: an instanced draw reads its per-instance data from element StartInstanceLocation + InstanceID /
// InstanceDataStepRate of the slot's buffer, and from element StartInstanceLocation for every instance at a step
// rate of 0 (D3D11.3 8.4.1 and 8.6.1: the address begins at "Slot[s].StrideInBytes*StartInstanceLocation" and
// moves a stride on when the step counter, which begins at the rate, has come down to 1). per-vertex data comes
// from element StartVertexLocation + VertexID, or BaseVertexLocation + the index (8.6.1). SV_InstanceID "starts at
// 0 for the first instance" and SV_VertexID starts at 0 or "represents the index value", whatever the starts are
// (8.18, 8.16). ExecuteIndirect draws as the direct draw with its arguments does.
// engines lean on all of it at once: Unreal finds a draw's instances in its GPU scene through a per-instance
// element (InstanceIdOffset) that only StartInstanceLocation selects, with SV_InstanceID added in the shader.
// a point a vertex and instance. a vertex's element gives its column, an instance's element at rate 1 its row,
// and the pixel takes the instance's elements at rate 2 and rate 0, SV_InstanceID + 1 and SV_VertexID + 1. columns
// and rows are permutations of the elements, so every element has a pixel of its own; every pixel of the target is
// held to the draw's arguments: nothing else is drawn. draws with and without starts, indexed with a base vertex
// above and below 0, each direct and through ExecuteIndirect.
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
struct In {
  uint column : COLUMN; uint row : ROW; uint second : SECOND; uint still : STILL;
  uint instance : SV_InstanceID; uint vertex : SV_VertexID;
};
struct V { float4 pos : SV_Position; nointerpolation uint4 got : GOT; };
V vs(In i) {
  V v;
  v.pos = float4((i.column + 0.5) * 2 / WIDE - 1, 1 - (i.row + 0.5) * 2 / HIGH, 0, 1);
  v.got = uint4(i.second, i.still, i.instance + 1, i.vertex + 1);
  return v;
}
uint4 ps(V v) : SV_Target { return v.got; }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  // as many vertex elements as columns and instance elements as rows; the slots after the vertices' step at these rates
  const UINT wide = 8, high = 16, word = sizeof(UINT), rates[] = {1, 2, 0}, slots = 1 + std::size(rates);
  const std::vector<std::string> defines = {"WIDE=" + std::to_string(wide), "HIGH=" + std::to_string(high)};
  auto vs = compiler.compile(hlsl, "vs", "vs", defines), ps = compiler.compile(hlsl, "ps", "ps", defines);
  if (vs.empty() || ps.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  // what the elements hold: a column or a row that no other element has (the factors share no divisor with the
  // sizes), and for the other slots the slot and the element
  auto held = [=](UINT slot, UINT element) {
    return slot == 0 ? (element * 3 + 1) % wide : slot == 1 ? (element * 5 + 2) % high : (slot << 8) + element;
  };
  // the indices: from 1 to WIDE - 3, so that base vertices from -1 to 2 have a vertex for each
  const UINT indices = wide;
  auto index_at = [=](UINT at) { return 1 + at * 2 % (wide - 3); };

  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  auto rs = root_signature(device.Get(), {0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT});
  const char *names[] = {"COLUMN", "ROW", "SECOND", "STILL"};
  D3D12_INPUT_ELEMENT_DESC elements[slots];
  for (UINT slot = 0; slot < slots; slot++)
    elements[slot] = {names[slot], 0, DXGI_FORMAT_R32_UINT, slot, 0,
                      slot ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, slot ? rates[slot - 1] : 0};
  const DXGI_FORMAT format = DXGI_FORMAT_R32G32B32A32_UINT;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = rs.Get();
  desc.VS = bytecode(vs), desc.PS = bytecode(ps);
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.InputLayout = {elements, slots};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
  desc.NumRenderTargets = 1, desc.RTVFormats[0] = format;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));
  ComPtr<ID3D12CommandSignature> signatures[2];
  const D3D12_INDIRECT_ARGUMENT_DESC kinds[2] = {{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW}, {D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED}};
  const UINT strides[2] = {sizeof(D3D12_DRAW_ARGUMENTS), sizeof(D3D12_DRAW_INDEXED_ARGUMENTS)};
  for (UINT indexed = 0; indexed < 2; indexed++) {
    D3D12_COMMAND_SIGNATURE_DESC signature{strides[indexed], 1, &kinds[indexed]};
    CHECK(device->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&signatures[indexed])));
  }

  // one buffer: the slots' elements one after another, then the indices, then room for a draw's arguments
  const UINT elements_of[slots] = {wide, high, high, high};
  UINT begins[slots + 2] = {};
  for (UINT slot = 0; slot < slots; slot++)
    begins[slot + 1] = begins[slot] + elements_of[slot];
  begins[slots + 1] = begins[slots] + indices;
  const UINT64 arguments_at = begins[slots + 1] * word;
  auto data = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, arguments_at + strides[1], D3D12_RESOURCE_STATE_GENERIC_READ);
  UINT *words;
  CHECK(data->Map(0, nullptr, (void **)&words));
  for (UINT slot = 0; slot < slots; slot++)
    for (UINT element = 0; element < elements_of[slot]; element++)
      words[begins[slot] + element] = held(slot, element);
  for (UINT at = 0; at < indices; at++)
    words[begins[slots] + at] = index_at(at);
  D3D12_VERTEX_BUFFER_VIEW views[slots];
  for (UINT slot = 0; slot < slots; slot++)
    views[slot] = {data->GetGPUVirtualAddress() + begins[slot] * word, elements_of[slot] * word, word};
  const D3D12_INDEX_BUFFER_VIEW index_view{data->GetGPUVirtualAddress() + begins[slots] * word, indices * word, DXGI_FORMAT_R32_UINT};

  const D3D12_HEAP_PROPERTIES gpu{D3D12_HEAP_TYPE_DEFAULT};
  const D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, wide, high, 1, 1, format, {1, 0},
                                        D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)));
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)));
  auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 target_bytes;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &target_bytes);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, target_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  // a draw's arguments in D3D12_DRAW_INDEXED_ARGUMENTS' order; one that is not indexed has no base vertex
  struct Draw {
    UINT count, instances, start;
    INT base;
    UINT start_instance;
    bool indexed;
  };
  const Draw draws[] = {{3, 5, 0, 0, 0, false}, {3, 5, 2, 0, 3, false}, {3, 5, 0, 0, 0, true}, {3, 5, 1, 2, 3, true}, {3, 5, 2, -1, 6, true}};
  for (auto &draw : draws)
    for (bool indirect : {false, true}) {
      step("%s%s of %u from %u in %u instances from %u, base vertex %d", indirect ? "ExecuteIndirect: " : "", draw.indexed ? "indices" : "vertices",
           draw.count, draw.start, draw.instances, draw.start_instance, draw.base);
      std::vector<UINT> want(4 * wide * high);
      for (UINT instance = 0; instance < draw.instances; instance++)
        for (UINT i = 0; i < draw.count; i++) {
          const UINT id = draw.indexed ? index_at(draw.start + i) : i, vertex = draw.indexed ? draw.base + id : draw.start + i;
          // the instance's element at each rate: the start, and a step every `rate` instances
          auto element = [&](UINT slot) { return draw.start_instance + (rates[slot - 1] ? instance / rates[slot - 1] : 0); };
          auto pixel = &want[4 * (held(1, element(1)) * wide + held(0, vertex))];
          pixel[0] = held(2, element(2)), pixel[1] = held(3, element(3)), pixel[2] = instance + 1, pixel[3] = id + 1;
        }
      if (FAILED(forget(readback.Get())) || FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), pso.Get()))) {
        expect(false, "the list could not be begun");
        continue;
      }
      const float nothing[4] = {};
      const D3D12_VIEWPORT viewport{0, 0, (float)wide, (float)high, 0, 1};
      const D3D12_RECT scissor{0, 0, (LONG)wide, (LONG)high};
      const D3D12_DRAW_ARGUMENTS plain{draw.count, draw.instances, draw.start, draw.start_instance};
      const D3D12_DRAW_INDEXED_ARGUMENTS with_indices{draw.count, draw.instances, draw.start, draw.base, draw.start_instance};
      memcpy((char *)words + arguments_at, draw.indexed ? (const void *)&with_indices : &plain, strides[draw.indexed]);
      list->ClearRenderTargetView(rtv, nothing, 0, nullptr);
      list->SetGraphicsRootSignature(rs.Get());
      list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
      list->RSSetViewports(1, &viewport);
      list->RSSetScissorRects(1, &scissor);
      list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
      list->IASetVertexBuffers(0, slots, views);
      list->IASetIndexBuffer(&index_view);
      if (indirect)
        list->ExecuteIndirect(signatures[draw.indexed].Get(), 1, data.Get(), arguments_at, nullptr, 0);
      else if (draw.indexed)
        list->DrawIndexedInstanced(draw.count, draw.instances, draw.start, draw.base, draw.start_instance);
      else
        list->DrawInstanced(draw.count, draw.instances, draw.start, draw.start_instance);
      transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
          to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
      list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
      transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
      HRESULT ran = submit(device.Get(), queue.Get(), list.Get());
      const char *out;
      if (!expect(ran == S_OK, "the device after the draw: %08lx", ran) || FAILED(readback->Map(0, nullptr, (void **)&out)))
        return verdict();
      unsigned wrong = 0;
      for (UINT row = 0; row < high; row++)
        for (UINT column = 0; column < wide; column++) {
          const UINT *got = (const UINT *)(out + row * footprint.Footprint.RowPitch) + 4 * column, *pixel = &want[4 * (row * wide + column)];
          if (memcmp(got, pixel, 4 * word) && wrong++ < 4)
            expect(false, "column %u, row %u: elements %#x and %#x, instance %d, vertex %d; want %#x and %#x, %d, %d (0 and -1: not drawn)", column,
                   row, got[0], got[1], (int)got[2] - 1, (int)got[3] - 1, pixel[0], pixel[1], (int)pixel[2] - 1, (int)pixel[3] - 1);
        }
      expect(wrong <= 4, "and %u more pixels", wrong - 4);
      readback->Unmap(0, nullptr);
    }
  return verdict();
}
