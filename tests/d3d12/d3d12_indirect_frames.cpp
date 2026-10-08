// contract: each indirect draw reads its own arguments when the GPU reaches it, after the preceding writer, and
// allocator memory is reused only after its frame's fence. ExecuteIndirect must "instruct the GPU to interpret the
// contents of the indirect argument buffer according to the format defined by a particular command signature"
// (Microsoft Learn, Indirect Drawing, Command Signatures). "If pCountBuffer is NULL, the MaxCommandCount specifies
// the exact number of operations which will be performed", starting at
// "pArgumentBuffer->GetBase() + ArgumentBufferOffset" (ID3D12GraphicsCommandList::ExecuteIndirect, Parameters/Remarks).
// "the runtime and driver assume that the graphics processing unit (GPU) is no longer executing any command lists
// that have recorded commands with the command allocator. So you should ensure that you don't call Reset until the
// GPU is done executing command lists associated with the allocator." (ID3D12CommandAllocator::Reset, Remarks).
// sources: https://learn.microsoft.com/en-us/windows/win32/direct3d12/indirect-drawing
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-executeindirect
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandallocator-reset
// call i owns three columns, rotated by the pass/frame seed, and draws zero to two points in zero to two rows. an
// indexed draw reverses the columns through its indices and a signed base vertex. additive pixels count draws and
// sum their call number, seed and row, fetched from per-instance vertex data at StartInstanceLocation + instance
// (D3D11.3 8.4.1/8.6.1: "Slot[s].StrideInBytes*StartInstanceLocation").
// stale, misplaced, missing and repeated draws differ. every pass is copied before the buffer is written again;
// all snapshots are checked after the last frame is submitted. a queue fence holds the first two frames until
// both are submitted, so allocator storage is still pending while the next frame records. only a full
// allocator pool waits between frames, and its oldest fence must pass before Reset. root constants at the root
// signature's limit make each pass cross the allocator's system-sized recording blocks, without a guessed count.
#include "d3d12_test.hpp"
#include <algorithm>
#include <cstddef>

struct Slot {
  float before[4];
  D3D12_DRAW_INDEXED_ARGUMENTS arguments;
  float after[4];
};

static const char hlsl[] = R"hlsl(
cbuffer Constants : register(b0) { uint seed; uint number; };
RWByteAddressBuffer commands : register(u0);
void command(uint i) {
  uint turn = i + seed, slot = turn % CALLS, indexed = (turn - 1) % 2;
  uint first_instance = i / 2 % 2;
  int base_vertex = -1 - int(seed / 4 % 2);
  uint at = BASE + i * SLOT_BYTES;
  commands.Store4(at, asuint(float4(i + 0.25, seed + 0.5, i + 0.75, seed + 0.25)));
  commands.Store4(at + ARG_OFFSET, uint4(turn % 3, turn / 3 % 3,
                  COLUMNS * (indexed ? CALLS - 1 - slot : slot), indexed ? asuint(base_vertex) : first_instance));
  commands.Store(at + ARG_OFFSET + ARG_BYTES - WORD_BYTES, indexed ? first_instance : asuint(float(turn) + 0.5));
  commands.Store4(at + ARG_OFFSET + ARG_BYTES, asuint(float4(seed + 0.75, i + 0.5, seed + 0.25, i + 0.75)));
}
[numthreads(1, 1, 1)] void cs(uint3 id : SV_DispatchThreadID) { command(id.x); }
float4 writer(uint id : SV_VertexID) : SV_Position {
  return float4((id + 0.5) * 2 / WIDTH - 1, 1 - 1.0 / ROWS, 0, 1);
}
float4 stores(float4 at : SV_Position) : SV_Target { command(uint(at.x)); return 0; }
struct V { float4 at : SV_Position; nointerpolation uint row : ROW; };
V vs(float column : COLUMN, float row : ROW) {
  V v;
  v.at = float4((column + 0.5) * 2 / WIDTH - 1, 1 - (row + 0.5) * 2 / ROWS, 0, 1);
  v.row = row;
  return v;
}
float4 ps(V v) : SV_Target { return float4(1, number, seed, v.row + 1); }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  SYSTEM_INFO system;
  GetSystemInfo(&system);
  const UINT words = D3D12_MAX_ROOT_COST - sizeof(D3D12_GPU_VIRTUAL_ADDRESS) / sizeof(UINT);
  const UINT columns = 3, calls = system.dwAllocationGranularity / (words * sizeof(UINT)) + 1,
             width = columns * calls, rows = 4;
  const UINT lists_per_frame = 2, passes_per_list = 3, passes = lists_per_frame * passes_per_list, frames = 5;
  const UINT pooled_frames = 2, snapshots = frames * passes, base = system.dwAllocationGranularity;
  const UINT64 argument_bytes = base + calls * sizeof(Slot);
  const std::vector<std::string> defines = {"CALLS=" + std::to_string(calls),
                                            "COLUMNS=" + std::to_string(columns),
                                            "WIDTH=" + std::to_string(width),
                                            "ROWS=" + std::to_string(rows),
                                            "BASE=" + std::to_string(base),
                                            "SLOT_BYTES=" + std::to_string(sizeof(Slot)),
                                            "ARG_OFFSET=" + std::to_string(offsetof(Slot, arguments)),
                                            "ARG_BYTES=" + std::to_string(sizeof(D3D12_DRAW_INDEXED_ARGUMENTS)),
                                            "WORD_BYTES=" + std::to_string(sizeof(UINT))};
  auto cs = compiler.compile(hlsl, "cs", "cs", defines), vs = compiler.compile(hlsl, "vs", "vs", defines),
       ps = compiler.compile(hlsl, "ps", "ps", defines), writer = compiler.compile(hlsl, "writer", "vs", defines),
       stores = compiler.compile(hlsl, "stores", "ps", defines);
  if (cs.empty() || vs.empty() || ps.empty() || writer.empty() || stores.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER params[] = {{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[0].Constants = {0, 0, words};
  auto rs = root_signature(device.Get(), {UINT(std::size(params)), params, 0, nullptr,
                                          D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT});
  if (!expect(bool(rs), "root signature could not be made"))
    return verdict();
  const DXGI_FORMAT format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  const D3D12_INPUT_ELEMENT_DESC elements[] = {
      {"COLUMN", 0, DXGI_FORMAT_R32_FLOAT},
      {"ROW", 0, DXGI_FORMAT_R32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1}};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = rs.Get();
  desc.VS = bytecode(vs), desc.PS = bytecode(ps);
  desc.BlendState.RenderTarget[0] = {TRUE,
                                     FALSE,
                                     D3D12_BLEND_ONE,
                                     D3D12_BLEND_ONE,
                                     D3D12_BLEND_OP_ADD,
                                     D3D12_BLEND_ONE,
                                     D3D12_BLEND_ONE,
                                     D3D12_BLEND_OP_ADD,
                                     D3D12_LOGIC_OP_NOOP,
                                     D3D12_COLOR_WRITE_ENABLE_ALL};
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.InputLayout = {elements, UINT(std::size(elements))};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
  desc.NumRenderTargets = 1, desc.RTVFormats[0] = format;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> draws, writes, dispatches;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&draws)));
  desc.VS = bytecode(writer), desc.PS = bytecode(stores), desc.InputLayout = {};
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&writes)));
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc{rs.Get(), bytecode(cs)};
  CHECK(device->CreateComputePipelineState(&compute_desc, IID_PPV_ARGS(&dispatches)));
  const D3D12_INDIRECT_ARGUMENT_DESC kinds[] = {{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW},
                                                {D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED}};
  ComPtr<ID3D12CommandSignature> signatures[std::size(kinds)];
  for (UINT kind = 0; kind < std::size(kinds); kind++) {
    D3D12_COMMAND_SIGNATURE_DESC signature{
        UINT(kind ? sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) : sizeof(D3D12_DRAW_ARGUMENTS)), 1, &kinds[kind]};
    CHECK(device->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&signatures[kind])));
  }

  auto arguments = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, argument_bytes, D3D12_RESOURCE_STATE_COMMON,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto staged = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, argument_bytes, D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto zeros = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, argument_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto vertices =
      buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, (width + rows) * sizeof(float), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto indices = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, width * sizeof(UINT), D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(arguments && staged && zeros && vertices && indices, "buffers could not be made"))
    return verdict();
  void *nothing;
  float *column;
  UINT *index;
  CHECK(zeros->Map(0, nullptr, &nothing));
  memset(nothing, 0, argument_bytes);
  zeros->Unmap(0, nullptr);
  CHECK(vertices->Map(0, nullptr, (void **)&column));
  CHECK(indices->Map(0, nullptr, (void **)&index));
  for (UINT i = 0; i < width; i++)
    column[i] = i, index[i] = width - i;
  for (UINT i = 0; i < rows; i++)
    column[width + i] = i;
  vertices->Unmap(0, nullptr), indices->Unmap(0, nullptr);
  const D3D12_VERTEX_BUFFER_VIEW vbvs[] = {
      {vertices->GetGPUVirtualAddress(), UINT(width * sizeof(float)), sizeof(float)},
      {vertices->GetGPUVirtualAddress() + width * sizeof(float), UINT(rows * sizeof(float)), sizeof(float)}};
  const D3D12_INDEX_BUFFER_VIEW ibv{indices->GetGPUVirtualAddress(), UINT(width * sizeof(UINT)), DXGI_FORMAT_R32_UINT};
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{
      D3D12_RESOURCE_DIMENSION_TEXTURE2D,     0, width, rows, 1, 1, format, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> targets[passes];
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, passes};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)));
  D3D12_CPU_DESCRIPTOR_HANDLE rtvs[passes];
  for (UINT pass = 0; pass < passes; pass++) {
    CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                          nullptr, IID_PPV_ARGS(&targets[pass])));
    rtvs[pass] = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtvs[pass].ptr += pass * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    device->CreateRenderTargetView(targets[pass].Get(), nullptr, rtvs[pass]);
  }
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 bytes;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  const UINT64 snapshot_bytes = (bytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) /
                                D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
  auto readback =
      buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, snapshots * snapshot_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(bool(readback), "readback could not be made"))
    return verdict();
  CHECK(forget(readback.Get()));

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12Fence> fence, recorded;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&recorded)));
  struct Pooled {
    ComPtr<ID3D12CommandAllocator> allocator;
    UINT64 last = 0;
  };
  std::vector<Pooled> pool;
  ComPtr<ID3D12GraphicsCommandList> lists[lists_per_frame];
  CHECK(queue->Wait(recorded.Get(), 1));
  auto wait = [&](UINT64 value) {
    auto event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!expect(event != nullptr, "fence event could not be made"))
      return false;
    const HRESULT hr = fence->SetEventOnCompletion(value, event);
    const DWORD result = SUCCEEDED(hr) ? WaitForSingleObject(event, INFINITE) : WAIT_FAILED;
    CloseHandle(event);
    return expect(result == WAIT_OBJECT_0 && device->GetDeviceRemovedReason() == S_OK,
                  "fence %llu: wait %lu, event %08lx, device %08lx", value, result, hr,
                  device->GetDeviceRemovedReason());
  };
  const auto common = D3D12_RESOURCE_STATE_COMMON, writing = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
             reading = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, copied = D3D12_RESOURCE_STATE_COPY_SOURCE;
  std::vector<UINT> constants(words);
  UINT reused = 0;
  for (UINT frame = 0; frame < frames; frame++) {
    step("frame %u: %u lists, %u passes each, %u indirect calls per pass", frame, lists_per_frame, passes_per_list,
         calls);
    auto available =
        std::find_if(pool.begin(), pool.end(), [&](auto &p) { return p.last <= fence->GetCompletedValue(); });
    if (available == pool.end() && pool.size() == pooled_frames) {
      available = std::min_element(pool.begin(), pool.end(), [](auto &a, auto &b) { return a.last < b.last; });
      if (!wait(available->last))
        ExitProcess(verdict());
    }
    if (available == pool.end()) {
      pool.emplace_back();
      available = pool.end() - 1;
      CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&available->allocator)));
    } else {
      CHECK(available->allocator->Reset());
      reused++;
    }
    ID3D12CommandList *batch[lists_per_frame];
    for (UINT l = 0; l < lists_per_frame; l++) {
      if (!lists[l]) {
        CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, available->allocator.Get(), nullptr,
                                        IID_PPV_ARGS(&lists[l])));
      } else {
        CHECK(lists[l]->Reset(available->allocator.Get(), nullptr));
      }
      auto list = lists[l].Get();
      if (!frame && !l) {
        transition(list, arguments.Get(), common, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(arguments.Get(), 0, zeros.Get(), 0, argument_bytes);
        transition(list, arguments.Get(), D3D12_RESOURCE_STATE_COPY_DEST, common);
      }
      const D3D12_VIEWPORT viewport{0, 0, (float)width, (float)rows, 0, 1};
      const D3D12_RECT scissor{0, 0, (LONG)width, (LONG)rows};
      list->SetGraphicsRootSignature(rs.Get());
      list->SetComputeRootSignature(rs.Get());
      list->RSSetViewports(1, &viewport);
      list->RSSetScissorRects(1, &scissor);
      list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
      list->IASetVertexBuffers(0, std::size(vbvs), vbvs);
      list->IASetIndexBuffer(&ibv);
      for (UINT p = 0; p < passes_per_list; p++) {
        const UINT pass = l * passes_per_list + p, snapshot = frame * passes + pass;
        constants[0] = snapshot + 1;
        const float clear[4] = {};
        list->ClearRenderTargetView(rtvs[pass], clear, 0, nullptr);
        list->OMSetRenderTargets(1, &rtvs[pass], FALSE, nullptr);
        if (p == 1) {
          transition(list, arguments.Get(), common, writing);
          list->SetPipelineState(writes.Get());
          list->SetGraphicsRoot32BitConstants(0, words, constants.data(), 0);
          list->SetGraphicsRootUnorderedAccessView(1, arguments->GetGPUVirtualAddress());
          list->DrawInstanced(calls, 1, 0, 0);
          transition(list, arguments.Get(), writing, reading);
        } else {
          auto into = p == 2 ? staged.Get() : arguments.Get();
          transition(list, into, common, writing);
          list->SetPipelineState(dispatches.Get());
          list->SetComputeRoot32BitConstants(0, words, constants.data(), 0);
          list->SetComputeRootUnorderedAccessView(1, into->GetGPUVirtualAddress());
          list->Dispatch(calls, 1, 1);
          transition(list, into, writing, p == 2 ? copied : reading);
          if (p == 2) {
            transition(list, arguments.Get(), common, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyBufferRegion(arguments.Get(), base, staged.Get(), base, calls * sizeof(Slot));
            transition(list, arguments.Get(), D3D12_RESOURCE_STATE_COPY_DEST, reading);
            transition(list, staged.Get(), copied, common);
          }
        }
        list->SetPipelineState(draws.Get());
        for (UINT i = 0; i < calls; i++) {
          constants[1] = i + 1;
          list->SetGraphicsRoot32BitConstants(0, words, constants.data(), 0);
          list->ExecuteIndirect(signatures[(i + snapshot) % std::size(kinds)].Get(), 1, arguments.Get(),
                                base + i * sizeof(Slot) + offsetof(Slot, arguments), nullptr, 0);
        }
        transition(list, arguments.Get(), reading, common);
        transition(list, targets[pass].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, copied);
        D3D12_TEXTURE_COPY_LOCATION from{targets[pass].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
            to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
        to.PlacedFootprint.Offset += snapshot * snapshot_bytes;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        transition(list, targets[pass].Get(), copied, D3D12_RESOURCE_STATE_RENDER_TARGET);
      }
      CHECK(list->Close());
      batch[l] = list;
    }
    queue->ExecuteCommandLists(lists_per_frame, batch);
    available->last = frame + 1;
    CHECK(queue->Signal(fence.Get(), available->last));
    if (frame + 1 == pooled_frames)
      CHECK(recorded->Signal(1));
  }
  if (!wait(frames))
    ExitProcess(verdict());
  expect(reused != 0, "no allocator was reused");
  const char *pixels;
  CHECK(readback->Map(0, nullptr, (void **)&pixels));
  for (UINT snapshot = 0; snapshot < snapshots; snapshot++) {
    step("read frame %u, list %u, writer %u", snapshot / passes, snapshot % passes / passes_per_list,
         snapshot % passes_per_list);
    std::vector<float> want(width * rows * 4);
    const UINT seed = snapshot + 1;
    for (UINT i = 0; i < calls; i++) {
      const UINT turn = i + seed, slot = turn % calls, kind = (i + snapshot) % std::size(kinds);
      const UINT start = columns * (kind ? calls - 1 - slot : slot);
      const INT base_vertex = -1 - INT(seed / 4 % 2);
      for (UINT vertex = 0; vertex < turn % 3; vertex++)
        for (UINT instance = 0; instance < turn / 3 % 3; instance++) {
          const UINT x = kind ? width - (start + vertex) + base_vertex : start + vertex, y = i / 2 % 2 + instance;
          auto pixel = &want[4 * (y * width + x)];
          pixel[0]++, pixel[1] += i + 1, pixel[2] += seed, pixel[3] += y + 1;
        }
    }
    UINT wrong = 0;
    for (UINT y = 0; y < rows; y++)
      for (UINT x = 0; x < width; x++) {
        auto got =
            (const float *)(pixels + snapshot * snapshot_bytes + footprint.Offset + y * footprint.Footprint.RowPitch) +
            4 * x;
        auto pixel = &want[4 * (y * width + x)];
        for (UINT channel = 0; channel < 4; channel++)
          if (got[channel] != pixel[channel] && wrong++ < 4)
            expect(false, "pixel %u,%u channel %u: %g, want %g", x, y, channel, got[channel], pixel[channel]);
      }
    expect(!wrong, "%u wrong pixel components", wrong);
  }
  readback->Unmap(0, nullptr);
  return verdict();
}
