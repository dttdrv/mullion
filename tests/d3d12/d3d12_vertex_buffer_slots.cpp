// contract: IASetVertexBuffers sets slots StartSlot to StartSlot + NumViews - 1 of the
// D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT (32) there are (Microsoft Learn, ID3D12GraphicsCommandList::
// IASetVertexBuffers). a call whose range goes past the last slot is invalid; it is dropped, as vkd3d-proton and vkd3d
// drop it ("Invalid start slot %u / view count %u"), and changes no slot. a call with no views keeps the bindings, as
// native drivers do (vkd3d-proton: "Native drivers appear to ignore this call. Buffer bindings are kept as-is.").
// a point reads slot 0 and slot 31, which hold 4 words each, and writes them to two targets: after calls of 2 views
// from slot 31, 3 from slot 30, 1 from slot 32 and none from slot 0, both still read what was bound before.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-iasetvertexbuffers
// https://github.com/HansKristian-Work/vkd3d-proton/blob/master/libs/vkd3d/command.c
#include "d3d12_test.hpp"

static const char hlsl[] = R"(
struct V { float4 pos : SV_Position; nointerpolation uint4 a : A; nointerpolation uint4 b : B; };
V vs(uint4 a : A, uint4 b : B) {
  V v;
  v.pos = float4(0, 0, 0, 1);
  v.a = a;
  v.b = b;
  return v;
}
struct O { uint4 a : SV_Target0; uint4 b : SV_Target1; };
O ps(V v) {
  O o;
  o.a = v.a;
  o.b = v.b;
  return o;
}
)";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto vs = compiler.compile(hlsl, "vs", "vs"), ps = compiler.compile(hlsl, "ps", "ps");
  if (!expect(!vs.empty() && !ps.empty(), "the shaders do not compile"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  auto rs = root_signature(device.Get(), {0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT});
  if (!expect(bool(rs), "the root signature is refused"))
    return verdict();

  constexpr UINT last = D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT - 1;
  const D3D12_INPUT_ELEMENT_DESC elements[] = {
      {"A", 0, DXGI_FORMAT_R32G32B32A32_UINT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA},
      {"B", 0, DXGI_FORMAT_R32G32B32A32_UINT, last, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA},
  };
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs), bytecode(ps)};
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = desc.BlendState.RenderTarget[1].RenderTargetWriteMask =
      D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.BlendState.IndependentBlendEnable = TRUE;
  desc.SampleMask = UINT_MAX;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.InputLayout = {elements, UINT(std::size(elements))};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
  desc.NumRenderTargets = 2;
  desc.RTVFormats[0] = desc.RTVFormats[1] = DXGI_FORMAT_R32G32B32A32_UINT;
  desc.SampleDesc.Count = 1;
  ComPtr<ID3D12PipelineState> pipeline;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)));

  // three vertex buffers of one vertex each: what slot 0 and slot 31 are bound to, and what the calls name
  constexpr UINT vertex = 16;
  const UINT32 words[3][4] = {{1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 11, 12}};
  auto vertices = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(words), D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(bool(vertices), "the vertex buffer is refused"))
    return verdict();
  void *mapped;
  CHECK(vertices->Map(0, nullptr, &mapped));
  memcpy(mapped, words, sizeof(words));
  vertices->Unmap(0, nullptr);
  D3D12_VERTEX_BUFFER_VIEW views[3];
  for (UINT i = 0; i < 3; i++)
    views[i] = {vertices->GetGPUVirtualAddress() + i * vertex, vertex, vertex};
  const D3D12_VERTEX_BUFFER_VIEW other[3] = {views[2], views[2], views[2]};

  const D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
  const D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 1, 1, 1, 1,
                                        DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> targets[2];
  for (auto &target : targets)
    CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &target_desc,
                                          D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)));
  ComPtr<ID3D12DescriptorHeap> rtvs;
  const D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  D3D12_CPU_DESCRIPTOR_HANDLE rtv[2] = {rtvs->GetCPUDescriptorHandleForHeapStart(),
                                        rtvs->GetCPUDescriptorHandleForHeapStart()};
  rtv[1].ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  for (UINT i = 0; i < 2; i++)
    device->CreateRenderTargetView(targets[i].Get(), nullptr, rtv[i]);
  const UINT pitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 2 * pitch, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(bool(readback), "the readback buffer is refused"))
    return verdict();

  const struct {
    UINT start, count;
    bool views;
  } calls[] = {{last, 2, true}, {last - 1, 3, true}, {last + 1, 1, true}, {0, 1, false}};
  for (auto &call : calls) {
    step("IASetVertexBuffers(%u, %u, %s)", call.start, call.count, call.views ? "views" : "NULL");
    CHECK(forget(readback.Get()));
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), nullptr));
    const UINT zero[4] = {};
    for (auto &view : rtv)
      list->ClearRenderTargetView(view, (const float *)zero, 0, nullptr);
    list->OMSetRenderTargets(2, rtv, FALSE, nullptr);
    const D3D12_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
    const D3D12_RECT scissor{0, 0, 1, 1};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->SetGraphicsRootSignature(rs.Get());
    list->SetPipelineState(pipeline.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    list->IASetVertexBuffers(0, 1, &views[0]);
    list->IASetVertexBuffers(last, 1, &views[1]);
    list->IASetVertexBuffers(call.start, call.count, call.views ? other : nullptr);
    list->DrawInstanced(1, 1, 0, 0);
    for (UINT i = 0; i < 2; i++) {
      transition(list.Get(), targets[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION src{targets[i].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
          dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
      dst.PlacedFootprint = {i * pitch, {DXGI_FORMAT_R32G32B32A32_UINT, 1, 1, 1, pitch}};
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      transition(list.Get(), targets[i].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    BYTE *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    for (UINT i = 0; i < 2; i++) {
      const UINT32 *value = (const UINT32 *)(got + i * pitch);
      expect(!memcmp(value, words[i], sizeof(words[i])), "slot %u reads %u %u %u %u, want %u %u %u %u",
             i ? last : 0, value[0], value[1], value[2], value[3], words[i][0], words[i][1], words[i][2], words[i][3]);
    }
    readback->Unmap(0, nullptr);
  }
  return verdict();
}
