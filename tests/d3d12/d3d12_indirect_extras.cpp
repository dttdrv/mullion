// contract: an indexed draw's indices past its view read 0 ("the return is 0", D3D11.3 8.19.2), whatever else the
// same ExecuteIndirect draws: each of its commands draws as a direct draw of the same arguments, and those past the
// count buffer's value draw nothing (Microsoft Learn, Indirect Drawing). every command here reads the view's four
// indices and two past it, so vertex 0 draws twice a command; with the count below MaxCommandCount, the commands past
// the count are not run while those before them are. each vertex is a point at its own pixel, and additive blending
// counts its draws
#include "d3d12_test.hpp"
#include <iterator>

static const char hlsl[] = R"hlsl(
float4 vs(uint column : COLUMN) : SV_Position { return float4((column + 0.5) * 2 / WIDTH - 1, 0, 0, 1); }
float ps() : SV_Target { return 1; }
)hlsl";

int
main(int argc, char **argv) {
  const UINT indices[] = {1, 2, 3, 4}, width = std::size(indices) + 1, past = 2, most = 65536, repeats = 8;
  step("compiling shaders");
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const std::vector<std::string> defines = {"WIDTH=" + std::to_string(width)};
  auto vs = compiler.compile(hlsl, "vs", "vs", defines), ps = compiler.compile(hlsl, "ps", "ps", defines);
  if (!expect(!vs.empty() && !ps.empty(), "HLSL did not compile"))
    return verdict();
  step("creating device and graphics pipeline");
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  auto rs = root_signature(
      device.Get(), {0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT}
  );
  const D3D12_INPUT_ELEMENT_DESC element{"COLUMN", 0, DXGI_FORMAT_R32_UINT};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = rs.Get();
  desc.VS = bytecode(vs), desc.PS = bytecode(ps);
  desc.InputLayout = {&element, 1};
  auto &blend = desc.BlendState.RenderTarget[0];
  blend.BlendEnable = TRUE;
  blend.SrcBlend = blend.DestBlend = blend.SrcBlendAlpha = blend.DestBlendAlpha = D3D12_BLEND_ONE;
  blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
  blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
  desc.NumRenderTargets = 1, desc.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, 1, 1, 1, desc.RTVFormats[0], {1, 0},
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
  auto vertices = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, width * sizeof(UINT), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto ib = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(indices), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto commands = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, most * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) + sizeof(UINT),
                         D3D12_RESOURCE_STATE_GENERIC_READ);
  UINT *columns;
  CHECK(vertices->Map(0, nullptr, (void **)&columns));
  for (UINT i = 0; i < width; i++)
    columns[i] = i;
  vertices->Unmap(0, nullptr);
  void *mapped;
  CHECK(ib->Map(0, nullptr, &mapped));
  memcpy(mapped, indices, sizeof(indices));
  ib->Unmap(0, nullptr);
  const D3D12_VERTEX_BUFFER_VIEW vertex_view{vertices->GetGPUVirtualAddress(), UINT(width * sizeof(UINT)), sizeof(UINT)};
  const D3D12_INDEX_BUFFER_VIEW index_view{ib->GetGPUVirtualAddress(), sizeof(indices), DXGI_FORMAT_R32_UINT};
  const UINT count_offset = most * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
  D3D12_INDIRECT_ARGUMENT_DESC argument{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED};
  D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(D3D12_DRAW_INDEXED_ARGUMENTS), 1, &argument};
  ComPtr<ID3D12CommandSignature> signature;
  CHECK(device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signature)));
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  // counts below, at and past a SIMD group's lanes and a threadgroup's, and up to MaxCommandCount: the resolver's
  // threads past the count are in threadgroups that may run after those before it
  for (UINT count : {1u, 31u, 32u, 33u, 1024u, 1025u, most / 2, most - 1, most}) {
    step("%u of %u commands, %u times", count, most, repeats);
    CHECK(commands->Map(0, nullptr, &mapped));
    for (UINT i = 0; i < most; i++)
      static_cast<D3D12_DRAW_INDEXED_ARGUMENTS *>(mapped)[i] = {UINT(std::size(indices)) + past, 1, 0, 0, 0};
    *reinterpret_cast<UINT *>(static_cast<char *>(mapped) + count_offset) = count;
    commands->Unmap(0, nullptr);
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), pso.Get()));
    CHECK(forget(readback.Get()));
    const float clear[4] = {};
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    D3D12_VIEWPORT viewport{0, 0, float(width), 1, 0, 1};
    D3D12_RECT scissor{0, 0, LONG(width), 1};
    list->SetGraphicsRootSignature(rs.Get());
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    list->IASetVertexBuffers(0, 1, &vertex_view);
    list->IASetIndexBuffer(&index_view);
    for (UINT r = 0; r < repeats; r++)
      list->ExecuteIndirect(signature.Get(), most, commands.Get(), 0, commands.Get(), count_offset);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
        dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    float want[width] = {float(past * count * repeats)};
    for (auto index : indices)
      want[index] += float(count * repeats);
    const float *pixels;
    CHECK(readback->Map(0, nullptr, (void **)&pixels));
    for (UINT x = 0; x < width; x++)
      expect(pixels[x] == want[x], "pixel %u counts %.0f draws of its vertex, want %.0f", x, pixels[x], want[x]);
    readback->Unmap(0, nullptr);
  }
  return verdict();
}
