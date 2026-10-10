// contract: a graphics pipeline stream with no topology subobject uses triangles and streams complete primitives.
// "the runtime will provide a default value for it" (Microsoft Learn, D3D12_PIPELINE_STATE_STREAM_DESC, Remarks);
// CD3DX12_PIPELINE_STATE_STREAM_PARSE_HELPER sets that value to D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE.
// "incomplete primitives are never written out." (D3D11.3 14). explicit point, line and triangle overrides,
// lists and strips, and counts below, at and past an object group's capacity return the expected vertex bytes.
// without stream output, "Primitives will be completely discarded" when every vertex has a negative cull distance
// (D3D11.3 15.4.2): a full-target triangle with positive distances draws; one with negative distances does not.
#include "d3d12_test.hpp"
#include "../../src/airconv/airconv_public.h"
#include <cstddef>
#include <cstdint>

static const char hlsl[] = R"hlsl(
struct O { float4 position : SV_Position; uint value : VALUE; };
O vs(uint id : SV_VertexID) {
  O o;
  o.position = float4(0, 0, 0, 1);
  o.value = id + 1;
  return o;
}
struct C { float4 position : SV_Position; float distance : SV_CullDistance; };
C vs_cull(uint id : SV_VertexID) {
  C o;
  o.position = float4(float2((id << 1) & 2, id & 2) * float2(2, -2) + float2(-1, 1), 0, 1);
  o.distance = VISIBLE ? 1 : -1;
  return o;
}
uint ps() : SV_Target { return 1; }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto vs = compiler.compile(hlsl, "vs", "vs", {"VISIBLE=1"});
  auto ps = compiler.compile(hlsl, "ps", "ps", {"VISIBLE=1"});
  std::string cull_vs[2];
  for (UINT visible = 0; visible < std::size(cull_vs); visible++)
    cull_vs[visible] = compiler.compile(hlsl, "vs_cull", "vs", {"VISIBLE=" + std::to_string(visible)});
  if (!expect(!vs.empty() && !ps.empty() && !cull_vs[0].empty() && !cull_vs[1].empty(), "HLSL did not compile"))
    return verdict();
  ComPtr<ID3D12Device2> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  auto rs = root_signature(device.Get(), {0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT});
  if (!expect(!!rs, "root signature"))
    return verdict();
  const D3D12_SO_DECLARATION_ENTRY entry{0, "VALUE", 0, 0, 1, 0};
  const UINT stride = sizeof(uint32_t);
  struct Stream {
    struct alignas(void *) {
      D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
      ID3D12RootSignature *value;
    } root;
    struct alignas(void *) {
      D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
      D3D12_SHADER_BYTECODE value;
    } vertex;
    struct alignas(void *) {
      D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
      D3D12_STREAM_OUTPUT_DESC value;
    } output;
    struct alignas(void *) {
      D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
      D3D12_PRIMITIVE_TOPOLOGY_TYPE value;
    } topology;
  } stream{{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, rs.Get()},
           {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, bytecode(vs)},
           {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT, {&entry, 1, &stride, 1, D3D12_SO_NO_RASTERIZED_STREAM}},
           {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE}};
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  for (UINT type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT; type <= D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; type++) {
    const UINT corners = type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT  ? 1
                         : type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE ? 2
                                                                      : 3;
    stream.topology.value = D3D12_PRIMITIVE_TOPOLOGY_TYPE(type);
    for (bool omitted : {true, false}) {
      if (omitted && type != D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE)
        continue;
      step("topology=%u omitted=%u create stream-output pipeline", type, omitted);
      D3D12_PIPELINE_STATE_STREAM_DESC desc{omitted ? offsetof(Stream, topology) : sizeof(stream), &stream};
      ComPtr<ID3D12PipelineState> pso;
      HRESULT hr = device->CreatePipelineState(&desc, IID_PPV_ARGS(&pso));
      if (!expect(hr == S_OK, "CreatePipelineState returned %#lx", hr))
        continue;
      for (bool strip : {false, true}) {
        if (strip && corners == 1)
          continue;
        auto topology = corners == 1 ? D3D_PRIMITIVE_TOPOLOGY_POINTLIST
                        : corners == 2
                            ? (strip ? D3D_PRIMITIVE_TOPOLOGY_LINESTRIP : D3D_PRIMITIVE_TOPOLOGY_LINELIST)
                            : (strip ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        for (UINT vertices = 0; vertices <= 4 * SM50_GEOMETRY_WARP_THREADS + corners; vertices++) {
          step("topology=%u omitted=%u strip=%u vertices=%u read stream", type, omitted, strip, vertices);
          std::vector<uint32_t> want;
          const UINT primitives = strip ? (vertices >= corners ? vertices - corners + 1 : 0) : vertices / corners;
          for (UINT i = 0; i < primitives; i++)
            for (UINT j = 0; j < corners; j++) {
              const UINT corner = strip && corners == 3 && (i & 1) && j ? 3 - j : j;
              want.push_back((strip ? i : i * corners) + corner + 1);
            }
          const UINT64 bytes = (want.size() + 1) * stride,
                       filled_at = (bytes + sizeof(uint64_t) - 1) / sizeof(uint64_t) * sizeof(uint64_t),
                       total = filled_at + sizeof(uint64_t);
          auto target = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, total, D3D12_RESOURCE_STATE_COPY_DEST);
          auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);
          auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
          if (!expect(target && upload && readback, "buffers"))
            return verdict();
          void *mapped;
          CHECK(upload->Map(0, nullptr, &mapped));
          memset(mapped, 0xff, total);
          memset((char *)mapped + filled_at, 0, sizeof(uint64_t));
          upload->Unmap(0, nullptr);
          CHECK(forget(readback.Get()));
          CHECK(allocator->Reset());
          CHECK(list->Reset(allocator.Get(), pso.Get()));
          list->CopyResource(target.Get(), upload.Get());
          transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
          list->SetGraphicsRootSignature(rs.Get());
          list->IASetPrimitiveTopology(topology);
          D3D12_STREAM_OUTPUT_BUFFER_VIEW view{target->GetGPUVirtualAddress(), bytes,
                                               target->GetGPUVirtualAddress() + filled_at};
          list->SOSetTargets(0, 1, &view);
          list->DrawInstanced(vertices, 1, 0, 0);
          transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
          list->CopyResource(readback.Get(), target.Get());
          CHECK(submit(device.Get(), queue.Get(), list.Get()));
          uint32_t *got;
          CHECK(readback->Map(0, nullptr, (void **)&got));
          for (UINT i = 0; i < want.size(); i++)
            expect(got[i] == want[i], "vertex=%u got=%u want=%u", i, got[i], want[i]);
          expect(got[want.size()] == ~0u, "past stream: got=%#x", got[want.size()]);
          uint64_t filled;
          memcpy(&filled, (char *)got + filled_at, sizeof(filled));
          expect(filled == want.size() * stride, "filled=%llu want=%zu", (unsigned long long)filled,
                 want.size() * stride);
          readback->Unmap(0, nullptr);
        }
      }
    }
  }
  struct CullStream {
    decltype(Stream::root) root;
    decltype(Stream::vertex) vertex, pixel;
    struct alignas(void *) {
      D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
      D3D12_RT_FORMAT_ARRAY value;
    } target;
    struct alignas(void *) {
      D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
      D3D12_DEPTH_STENCIL_DESC value;
    } depth;
    decltype(Stream::topology) topology;
  } cull_stream{{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, rs.Get()},
                {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, {}},
                {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, bytecode(ps)},
                {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, {{DXGI_FORMAT_R32_UINT}, 1}},
                {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, {}},
                {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE}};
  D3D12_HEAP_PROPERTIES properties{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC texture_desc{
      D3D12_RESOURCE_DIMENSION_TEXTURE2D,     0, 1, 1, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &texture_desc,
                                        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)));
  ComPtr<ID3D12DescriptorHeap> heap;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)));
  auto rtv = heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 total;
  device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(!!readback, "cull readback"))
    return verdict();
  for (UINT visible = 0; visible < std::size(cull_vs); visible++) {
    cull_stream.vertex.value = bytecode(cull_vs[visible]);
    for (bool omitted : {true, false}) {
      step("without stream output visible=%u omitted=%u draw cull-distance triangle", visible, omitted);
      D3D12_PIPELINE_STATE_STREAM_DESC desc{omitted ? offsetof(CullStream, topology) : sizeof(cull_stream),
                                            &cull_stream};
      ComPtr<ID3D12PipelineState> pso;
      HRESULT hr = device->CreatePipelineState(&desc, IID_PPV_ARGS(&pso));
      if (!expect(hr == S_OK, "CreatePipelineState returned %#lx", hr))
        continue;
      CHECK(forget(readback.Get()));
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), pso.Get()));
      list->SetGraphicsRootSignature(rs.Get());
      list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      D3D12_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
      D3D12_RECT rect{0, 0, 1, 1};
      list->RSSetViewports(1, &viewport);
      list->RSSetScissorRects(1, &rect);
      list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
      const float clear[4]{};
      list->ClearRenderTargetView(rtv, clear, 0, nullptr);
      list->DrawInstanced(3, 1, 0, 0);
      transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
          from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      to.PlacedFootprint = footprint;
      list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
      transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      uint32_t *got;
      CHECK(readback->Map(0, nullptr, (void **)&got));
      expect(got[footprint.Offset / sizeof(*got)] == visible, "cull pixel got=%u want=%u",
             got[footprint.Offset / sizeof(*got)], visible);
      readback->Unmap(0, nullptr);
    }
  }
  return verdict();
}
