// contract: ExecuteIndirect consumes the arguments the preceding stream-output draw wrote, after its barrier.
// "observable rendering results must match results produced by serial processing of tasks" (D3D11.3 functional
// specification, 4.2); "the hardware must simply dump out the 32 bits (per component) of data out unaltered"
// (14.2.1). ExecuteIndirect must "instruct the GPU to interpret the contents of the indirect argument buffer
// according to the format defined by a particular command signature" (Microsoft Learn, Indirect Drawing).
#include "d3d12_test.hpp"
#include "../../src/airconv/airconv_public.h"
#include <array>
#include <chrono>

static const char hlsl[] = R"hlsl(
cbuffer Constants : register(b0) { uint turn; };
RWStructuredBuffer<uint> commands : register(u0);
struct O { float4 pos : SV_Position; uint4 args : ARGS; uint tail : TAIL; };
O arguments(uint id) {
  O o;
  o.pos = float4(0, 0, 0, 1);
  o.args = uint4(1, 1 + (id + turn) % ROWS, 2 * id + (turn & 1), 0);
  o.tail = 0;
  return o;
}
O vs_writes(uint id : SV_VertexID) { return arguments(id); }
[maxvertexcount(1)]
void gs(point O v[1], inout PointStream<O> s) { s.Append(v[0]); }
float4 vs(float column : COLUMN, uint row : SV_InstanceID) : SV_Position {
  return float4((column + 0.5) * 2 / COLUMNS - 1, 1 - (row + 0.5) * 2 / ROWS, 0, 1);
}
uint ps() : SV_Target { return turn; }
uint ps_writes(float4 at : SV_Position) : SV_Target {
  uint id = at.x;
  O o = arguments(id);
  commands[WORDS * id] = o.args.x;
  commands[WORDS * id + 1] = o.args.y;
  commands[WORDS * id + 2] = o.args.z;
  commands[WORDS * id + 3] = o.args.w;
  if (WORDS > 4)
    commands[WORDS * id + 4] = o.tail;
  return 0;
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  constexpr std::array<UINT, 4> counts = {
      SM50_GEOMETRY_WARP_THREADS - 1, SM50_GEOMETRY_WARP_THREADS, SM50_GEOMETRY_WARP_THREADS + 1,
      4 * SM50_GEOMETRY_WARP_THREADS + 1
  };
  const UINT commands = counts.back(), columns = 2 * commands, rows = 3;
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS12 options{};
  bool enhanced = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options, sizeof(options))) &&
                  options.EnhancedBarriersSupported;
  if (!enhanced)
    printf("skipped: enhanced barriers are not supported; legacy cases still run\n");
  D3D12_ROOT_PARAMETER params[] = {{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[0].Constants = {0, 0, 1};
  auto rs = root_signature(
      device.Get(), {(UINT)std::size(params), params, 0, nullptr,
                     D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                         D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT}
  );
  if (!expect(bool(rs), "the root signature was not created"))
    return verdict();
  const DXGI_FORMAT format = DXGI_FORMAT_R32_UINT;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, columns, rows, 1, 1, format, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 pixel_bytes;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &pixel_bytes);
  const UINT64 pixel_stride = (pixel_bytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) /
                             D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
  auto pixels = buffer(
      device.Get(), D3D12_HEAP_TYPE_READBACK, counts.size() * pixel_stride, D3D12_RESOURCE_STATE_COPY_DEST
  );
  auto vertices = buffer(
      device.Get(), D3D12_HEAP_TYPE_UPLOAD, columns * sizeof(float), D3D12_RESOURCE_STATE_GENERIC_READ
  );
  auto indices = buffer(
      device.Get(), D3D12_HEAP_TYPE_UPLOAD, columns * sizeof(UINT16), D3D12_RESOURCE_STATE_GENERIC_READ
  );
  auto unrelated = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, sizeof(UINT), D3D12_RESOURCE_STATE_COPY_SOURCE);
  if (!expect(pixels && vertices && indices && unrelated, "pixel and input buffers were not created"))
    return verdict();
  float *vertex;
  UINT16 *index;
  CHECK(vertices->Map(0, nullptr, (void **)&vertex));
  CHECK(indices->Map(0, nullptr, (void **)&index));
  for (UINT i = 0; i < columns; i++) {
    vertex[i] = i;
    index[i] = columns - 1 - i;
  }
  vertices->Unmap(0, nullptr);
  indices->Unmap(0, nullptr);
  const D3D12_VERTEX_BUFFER_VIEW vbv{vertices->GetGPUVirtualAddress(), columns * sizeof(float), sizeof(float)};
  const D3D12_INDEX_BUFFER_VIEW ibv{indices->GetGPUVirtualAddress(), columns * sizeof(UINT16), DXGI_FORMAT_R16_UINT};
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12GraphicsCommandList7> list7;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  if (enhanced) {
    CHECK(list.As(&list7));
  }

  for (UINT indexed = 0; indexed < 2; indexed++) {
    const UINT stride = indexed ? sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) : sizeof(D3D12_DRAW_ARGUMENTS);
    const UINT words = stride / sizeof(UINT);
    const std::vector<std::string> defines = {
        "WORDS=" + std::to_string(words), "COLUMNS=" + std::to_string(columns), "ROWS=" + std::to_string(rows)
    };
    auto vs = compiler.compile(hlsl, "vs", "vs", defines), ps = compiler.compile(hlsl, "ps", "ps", defines),
         vs_writes = compiler.compile(hlsl, "vs_writes", "vs", defines),
         gs = compiler.compile(hlsl, "gs", "gs", defines),
         ps_writes = compiler.compile(hlsl, "ps_writes", "ps", defines);
    if (!expect(
            !vs.empty() && !ps.empty() && !vs_writes.empty() && !gs.empty() && !ps_writes.empty(),
            "HLSL did not compile"
        ))
      return verdict();
    const D3D12_INPUT_ELEMENT_DESC element{"COLUMN", 0, DXGI_FORMAT_R32_FLOAT};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs), bytecode(ps)};
    desc.InputLayout = {&element, 1};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = ~0u;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = format;
    desc.SampleDesc = {1, 0};
    ComPtr<ID3D12PipelineState> draws, writers[3];
    CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&draws)));
    desc.PS = bytecode(ps_writes);
    CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&writers[2])));
    const D3D12_SO_DECLARATION_ENTRY entries[] = {{0, "ARGS", 0, 0, 4, 0}, {0, "TAIL", 0, 0, 1, 0}};
    desc.VS = bytecode(vs_writes);
    desc.PS = {};
    desc.InputLayout = {};
    desc.StreamOutput = {entries, 1 + indexed, &stride, 1, D3D12_SO_NO_RASTERIZED_STREAM};
    CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&writers[0])));
    desc.GS = bytecode(gs);
    CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&writers[1])));
    ComPtr<ID3D12CommandSignature> signature;
    D3D12_INDIRECT_ARGUMENT_DESC argument{
        indexed ? D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED : D3D12_INDIRECT_ARGUMENT_TYPE_DRAW
    };
    D3D12_COMMAND_SIGNATURE_DESC signature_desc{stride, 1, &argument};
    CHECK(device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signature)));
    const UINT64 argument_bytes = UINT64(commands) * stride;
    const UINT64 filled_at = (argument_bytes + alignof(UINT64) - 1) / alignof(UINT64) * alignof(UINT64);
    const UINT64 total = filled_at + sizeof(UINT64);
    auto written = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, total, D3D12_RESOURCE_STATE_COMMON,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto readback = buffer(
        device.Get(), D3D12_HEAP_TYPE_READBACK, counts.size() * total, D3D12_RESOURCE_STATE_COPY_DEST
    );
    if (!expect(written && upload && readback, "argument buffers were not created"))
      return verdict();
    std::vector<UINT> want(total / sizeof(UINT));
    auto arguments = [&](UINT turn, UINT count) {
      for (UINT i = 0; i < count; i++) {
        UINT start = 2 * i + (turn & 1), instances = 1 + (i + turn) % rows;
        if (indexed) {
          D3D12_DRAW_INDEXED_ARGUMENTS args{1, instances, start, 0, 0};
          memcpy(want.data() + i * words, &args, sizeof(args));
        } else {
          D3D12_DRAW_ARGUMENTS args{1, instances, start, 0};
          memcpy(want.data() + i * words, &args, sizeof(args));
        }
      }
    };
    arguments(0, commands);
    void *init;
    CHECK(upload->Map(0, nullptr, &init));
    memcpy(init, want.data(), total);
    upload->Unmap(0, nullptr);

    for (UINT form = 0; form < (enhanced ? 3u : 1u); form++)
      for (UINT writer = 0; writer < std::size(writers); writer++) {
        step("%s arguments, %s writer, %s barrier", indexed ? "indexed" : "non-indexed",
             writer == 0 ? "vertex stream-output" : writer == 1 ? "geometry stream-output" : "pixel UAV control",
             form == 0 ? "legacy" : form == 1 ? "enhanced buffer" : "enhanced global");
        CHECK(allocator->Reset());
        CHECK(list->Reset(allocator.Get(), nullptr));
        CHECK(forget(readback.Get()));
        CHECK(forget(pixels.Get()));
        list->CopyBufferRegion(written.Get(), 0, upload.Get(), 0, total);
        const auto writing = writer == 2 ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_STREAM_OUT;
        const auto access = writer == 2 ? D3D12_BARRIER_ACCESS_UNORDERED_ACCESS : D3D12_BARRIER_ACCESS_STREAM_OUTPUT;
        const auto sync = writer == 2 ? D3D12_BARRIER_SYNC_PIXEL_SHADING : D3D12_BARRIER_SYNC_VERTEX_SHADING;
        auto barrier = [&](D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after, D3D12_BARRIER_SYNC sync_before,
                           D3D12_BARRIER_SYNC sync_after, D3D12_BARRIER_ACCESS access_before,
                           D3D12_BARRIER_ACCESS access_after) {
          if (!form) {
            D3D12_RESOURCE_BARRIER barriers[] = {{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION},
                                                {D3D12_RESOURCE_BARRIER_TYPE_TRANSITION},
                                                {D3D12_RESOURCE_BARRIER_TYPE_TRANSITION}};
            barriers[0].Transition = {unrelated.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                      D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
            barriers[1].Transition = {unrelated.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE};
            barriers[2].Transition = {written.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
            list->ResourceBarrier(std::size(barriers), barriers);
          } else {
            D3D12_BUFFER_BARRIER buffers[] = {
                {D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_SOURCE,
                 D3D12_BARRIER_ACCESS_COPY_SOURCE, unrelated.Get(), 0, UINT64_MAX},
                {sync_before, sync_after, access_before, access_after, written.Get(), 0, UINT64_MAX}
            };
            D3D12_GLOBAL_BARRIER globals[] = {
                {D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_SOURCE,
                 D3D12_BARRIER_ACCESS_COPY_SOURCE},
                {sync_before, sync_after, access_before, access_after}
            };
            D3D12_BARRIER_GROUP groups[] = {{D3D12_BARRIER_TYPE_BUFFER, 1},
                                          {form == 1 ? D3D12_BARRIER_TYPE_BUFFER : D3D12_BARRIER_TYPE_GLOBAL,
                                           UINT(form == 1 ? std::size(buffers) : std::size(globals))}};
            groups[0].pBufferBarriers = buffers;
            if (form == 1)
              groups[1].pBufferBarriers = buffers;
            else
              groups[1].pGlobalBarriers = globals;
            list7->Barrier(std::size(groups), groups);
          }
        };
        list->SetGraphicsRootSignature(rs.Get());
        list->SetGraphicsRootUnorderedAccessView(1, written->GetGPUVirtualAddress());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
        list->IASetVertexBuffers(0, 1, &vbv);
        list->IASetIndexBuffer(&ibv);
        const D3D12_VIEWPORT viewport{0, 0, (float)columns, (float)rows, 0, 1};
        const D3D12_RECT scissor{0, 0, (LONG)columns, (LONG)rows};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        D3D12_STREAM_OUTPUT_BUFFER_VIEW view{written->GetGPUVirtualAddress(), argument_bytes,
                                            written->GetGPUVirtualAddress() + filled_at};
        list->SOSetTargets(0, 1, &view);
        for (UINT round = 0; round < counts.size(); round++) {
          UINT turn = round + 1;
          if (round) {
            barrier(D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_BARRIER_SYNC_COPY,
                    D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_SOURCE, D3D12_BARRIER_ACCESS_COPY_DEST);
            list->CopyBufferRegion(written.Get(), filled_at, upload.Get(), filled_at, sizeof(UINT64));
          }
          barrier(D3D12_RESOURCE_STATE_COPY_DEST, writing, D3D12_BARRIER_SYNC_COPY, sync,
                  D3D12_BARRIER_ACCESS_COPY_DEST, access);
          const float clear[4] = {};
          list->ClearRenderTargetView(rtv, clear, 0, nullptr);
          list->SetGraphicsRoot32BitConstants(0, 1, &turn, 0);
          list->SetPipelineState(writers[writer].Get());
          list->DrawInstanced(counts[round], 1, 0, 0);
          barrier(writing, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, sync, D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,
                  access, D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
          list->SetPipelineState(draws.Get());
          list->ExecuteIndirect(signature.Get(), counts[round], written.Get(), 0, nullptr, 0);
          barrier(D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COPY_SOURCE,
                  D3D12_BARRIER_SYNC_EXECUTE_INDIRECT, D3D12_BARRIER_SYNC_COPY,
                  D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT, D3D12_BARRIER_ACCESS_COPY_SOURCE);
          list->CopyBufferRegion(readback.Get(), round * total, written.Get(), 0, total);
          transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
          D3D12_TEXTURE_COPY_LOCATION from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
              to{pixels.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
          to.PlacedFootprint = footprint;
          to.PlacedFootprint.Offset = round * pixel_stride;
          list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
          transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        CHECK(list->Close());
        ComPtr<ID3D12Fence> fence;
        CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        auto event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (!expect(event != nullptr, "the completion event was not created"))
          return verdict();
        ID3D12CommandList *lists[] = {list.Get()};
        queue->ExecuteCommandLists(std::size(lists), lists);
        HRESULT hr = queue->Signal(fence.Get(), 1);
        if (SUCCEEDED(hr))
          hr = fence->SetEventOnCompletion(1, event);
        const auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::seconds(10)).count();
        auto waited = FAILED(hr) ? WAIT_FAILED : WaitForSingleObject(event, timeout);
        CloseHandle(event);
        CHECK(hr);
        if (!expect(waited == WAIT_OBJECT_0, "GPU completion wait returned %#lx", waited))
          return verdict();
        CHECK(device->GetDeviceRemovedReason());
        const UINT *got;
        const char *drawn;
        CHECK(readback->Map(0, nullptr, (void **)&got));
        CHECK(pixels->Map(0, nullptr, (void **)&drawn));
        std::fill(want.begin(), want.end(), 0);
        arguments(0, commands);
        for (UINT round = 0; round < counts.size(); round++) {
          UINT turn = round + 1;
          arguments(turn, counts[round]);
          UINT64 filled = writer == 2 ? 0 : UINT64(counts[round]) * stride;
          memcpy(reinterpret_cast<char *>(want.data()) + filled_at, &filled, sizeof(filled));
          for (UINT word = 0; word < want.size(); word++)
            expect(got[round * want.size() + word] == want[word], "round %u argument word %u is %u, want %u",
                   turn, word, got[round * want.size() + word], want[word]);
          std::vector<UINT> picture(columns * rows);
          for (UINT i = 0; i < counts[round]; i++) {
            UINT start = 2 * i + (turn & 1), column = indexed ? columns - 1 - start : start;
            for (UINT row = 0; row < 1 + (i + turn) % rows; row++)
              picture[row * columns + column] = turn;
          }
          for (UINT y = 0; y < rows; y++)
            for (UINT x = 0; x < columns; x++) {
              auto row = reinterpret_cast<const UINT *>(
                  drawn + round * pixel_stride + y * footprint.Footprint.RowPitch
              );
              auto value = row[x];
              expect(value == picture[y * columns + x], "round %u pixel (%u, %u) is %u, want %u",
                     turn, x, y, value, picture[y * columns + x]);
            }
        }
        readback->Unmap(0, nullptr);
        pixels->Unmap(0, nullptr);
      }
  }
  return verdict();
}
