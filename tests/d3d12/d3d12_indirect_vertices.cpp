// contract: a vertex buffer view argument of a command signature binds its slot for the command's draw
// (D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW: "the slot of the vertex buffer view"), as IASetVertexBuffers does,
// whichever slots the input layout uses and whichever of them the signature names; the slots it does not name keep
// what the command list bound.
// the input layout uses slots with gaps. every buffer holds one value for all its vertices, a triangle covers the
// target, and its pixels are the three slots' values.
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
struct V { float4 p : SV_Position; nointerpolation uint values : VALUES; };
V vs(uint a : A, uint b : B, uint c : C, uint id : SV_VertexID) {
  V v;
  float2 uv = float2((id << 1) & 2, id & 2);
  v.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
  v.values = a | b << 8 | c << 16;
  return v;
}
uint ps(V v) : SV_Target { return v.values; }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto vs = compiler.compile(hlsl, "vs", "vs"), ps = compiler.compile(hlsl, "ps", "ps");
  if (vs.empty() || ps.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  auto rs = root_signature(device.Get(), {0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT});
  // the layout's slots, with gaps before and between them
  const UINT slots[] = {1, 3, 6}, count = std::size(slots), vertices = 3;
  const D3D12_INPUT_ELEMENT_DESC elements[] = {
      {"A", 0, DXGI_FORMAT_R32_UINT, slots[0]}, {"B", 0, DXGI_FORMAT_R32_UINT, slots[1]}, {"C", 0, DXGI_FORMAT_R32_UINT, slots[2]}};
  const DXGI_FORMAT format = DXGI_FORMAT_R32_UINT;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = rs.Get();
  desc.VS = bytecode(vs), desc.PS = bytecode(ps);
  desc.InputLayout = {elements, count};
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1, desc.RTVFormats[0] = format;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));

  const UINT size = 4;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1, format, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)));
  auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 bytes;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);

  // two sets of vertex buffers: what the list binds, and what a command brings. buffer n of a set holds set + n
  const UINT bound = 0x10, brought = 0x90, stride = sizeof(UINT), buffer_bytes = vertices * stride;
  auto data = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 2 * count * buffer_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  UINT *words;
  CHECK(data->Map(0, nullptr, (void **)&words));
  for (UINT set = 0; set < 2; set++)
    for (UINT n = 0; n < count; n++)
      std::fill_n(words + (set * count + n) * vertices, vertices, (set ? brought : bound) + n);
  auto view = [&](UINT set, UINT n) {
    return D3D12_VERTEX_BUFFER_VIEW{data->GetGPUVirtualAddress() + (set * count + n) * buffer_bytes, buffer_bytes, stride};
  };

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  // each case: the slots (bits over the layout's) a command brings, through a signature or, as a control, directly
  struct Arguments {
    D3D12_VERTEX_BUFFER_VIEW views[count];
    D3D12_DRAW_ARGUMENTS draw;
  };
  unsigned failures = 0, cases = 0;
  for (UINT named = 1; named < 1u << count; named++)
    for (bool indirect : {false, true}) {
      std::vector<D3D12_INDIRECT_ARGUMENT_DESC> arguments;
      Arguments values{};
      UINT want = 0;
      for (UINT n = 0; n < count; n++) {
        want |= ((named >> n & 1 ? brought : bound) + n) << 8 * n;
        if (!(named >> n & 1))
          continue;
        D3D12_INDIRECT_ARGUMENT_DESC argument{D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW};
        argument.VertexBuffer.Slot = slots[n];
        arguments.push_back(argument);
        values.views[arguments.size() - 1] = view(1, n);
      }
      arguments.push_back({D3D12_INDIRECT_ARGUMENT_TYPE_DRAW});
      // the draw's arguments follow the views the signature has
      const UINT views_bytes = (arguments.size() - 1) * sizeof(D3D12_VERTEX_BUFFER_VIEW);
      std::vector<char> packed(views_bytes + sizeof(D3D12_DRAW_ARGUMENTS));
      const D3D12_DRAW_ARGUMENTS draw{vertices, 1, 0, 0};
      memcpy(packed.data(), values.views, views_bytes);
      memcpy(packed.data() + views_bytes, &draw, sizeof(draw));
      D3D12_COMMAND_SIGNATURE_DESC signature_desc{(UINT)packed.size(), (UINT)arguments.size(), arguments.data()};
      ComPtr<ID3D12CommandSignature> signature;
      CHECK(device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signature)));
      auto commands = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, packed.size(), D3D12_RESOURCE_STATE_GENERIC_READ);
      void *mapped;
      CHECK(commands->Map(0, nullptr, &mapped));
      memcpy(mapped, packed.data(), packed.size());

      CHECK(forget(readback.Get()));
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), pso.Get()));
      D3D12_VIEWPORT viewport{0, 0, (float)size, (float)size, 0, 1};
      D3D12_RECT scissor{0, 0, (LONG)size, (LONG)size};
      list->SetGraphicsRootSignature(rs.Get());
      list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
      list->RSSetViewports(1, &viewport);
      list->RSSetScissorRects(1, &scissor);
      list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      for (UINT n = 0; n < count; n++) {
        auto v = view(!indirect && (named >> n & 1), n);
        list->IASetVertexBuffers(slots[n], 1, &v);
      }
      if (indirect)
        list->ExecuteIndirect(signature.Get(), 1, commands.Get(), 0, nullptr, 0);
      else
        list->DrawInstanced(vertices, 1, 0, 0);
      transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
          dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      const UINT *pixels;
      CHECK(readback->Map(0, nullptr, (void **)&pixels));
      cases++;
      if (pixels[0] != want && failures++ < 12)
        printf("slots %#x %s: the values drawn are %#x, want %#x\n", named, indirect ? "through a signature" : "bound directly", pixels[0], want);
      readback->Unmap(0, nullptr);
    }
  printf("%s: %u wrong of %u draws\n", failures ? "failed" : "passed", failures, cases);
  return failures != 0;
}
