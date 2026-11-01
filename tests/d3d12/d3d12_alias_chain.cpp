// contract: an aliasing barrier orders the work before it on a queue against the work after it on that queue, for
// resources that share memory: "a usage transition between two resources that occupy the same memory" (Microsoft
// Learn, D3D12_RESOURCE_ALIASING_BARRIER), whose before and after may be NULL, "which indicates that any placed or
// reserved resource could cause aliasing". each step of a chain reads one region of a heap and writes the other, all
// through placed buffers on even steps and through a reserved buffer whose tiles map the regions in another order on
// odd ones, with an aliasing barrier between steps: a step reads what the one before wrote through the other
// resource. a step is a slow dispatch, a copy, or a draw whose pixels run the step through UAVs, in one list or
// across lists and submissions; and with or without an unrelated dispatch between a step and the barrier after it,
// which then begins the list a step that ends one is followed by. argv[2] "child": the same in a process that encodes every run of passes on several
// threads (MULLION_D3D12_PARALLEL_COMMANDS, MULLION_D3D12_ENCODE_THREADS); the test starts it itself.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_resource_aliasing_barrier
#include "d3d12_test.hpp"
#include <algorithm>

static constexpr UINT tile = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES, tile_words = tile / 4;
// two tiles a region, regions A (heap tiles 0, 1) and B (2, 3); the reserved buffer's tiles 0 and 1 map B's tiles
// backwards, its 2 and 3 A's
static constexpr UINT region_tiles = 2, words = region_tiles * tile_words, region = words * 4, width = 256;
static constexpr UINT mapping[2 * region_tiles] = {3, 2, 1, 0};
// so many iterations a thread that a step which did not wait for the one before would read before it wrote
static constexpr UINT work = 1u << 12;

static const char hlsl[] = R"hlsl(
cbuffer Step : register(b0) { uint salt; uint work; uint mask; };
RWByteAddressBuffer source : register(u0);
RWByteAddressBuffer destination : register(u1);
void run(uint id) {
  uint v = id;
  [loop] for (uint i = 0; i < work; i++)
    v = v * 1664525u + 1013904223u;
  uint from = (id * 7u + salt) % WORDS;
  destination.Store(id * 4, source.Load(from * 4) * 3u + salt + (v & mask));
}
[numthreads(64, 1, 1)] void cs(uint3 id : SV_DispatchThreadID) {
  run(id.x);
}
float4 vs(uint id : SV_VertexID) : SV_Position {
  return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0, 1);
}
uint ps(float4 position : SV_Position) : SV_Target {
  run(uint(position.y) * WIDTH + uint(position.x));
  return 0;
}
)hlsl";

enum Kind { Dispatch, Copy, Draw };
struct Case {
  const char *name;
  // the kind of every step whose index is a multiple of `every`, the others dispatches; every how many steps the
  // chain is split into another list, executed in the same call (0: one list) or in a call of its own; and whether an
  // unrelated dispatch comes before each barrier
  Kind kind;
  UINT every, split;
  bool calls, between;
};
static const Case cases[] = {
    {"dispatches", Dispatch, 1, 0, false},
    {"dispatches and copies", Copy, 3, 0, false},
    {"dispatches and draws", Draw, 4, 0, false},
    {"dispatches in lists of one call", Dispatch, 1, 5, false},
    {"dispatches and copies in calls of their own", Copy, 2, 7, true},
    {"dispatches and draws in calls of their own", Draw, 3, 4, true},
    {"dispatches and draws, another dispatch before each barrier", Draw, 2, 0, false, true},
    {"dispatches and copies in calls of their own, each beginning with another dispatch and the barrier", Copy, 2, 3,
     true, true},
};

// the heap word a resource's word of a region is: placed buffers cover the regions in order, the reserved buffer's
// tiles map them as `mapping` says
static UINT
physical(bool reserved, UINT region_index, UINT word) {
  if (!reserved)
    return region_index * words + word;
  UINT virtual_word = (region_index ? 0 : words) + word;
  return mapping[virtual_word / tile_words] * tile_words + virtual_word % tile_words;
}

static int
run(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const std::vector<std::string> defines = {"WORDS=" + std::to_string(words), "WIDTH=" + std::to_string(width)};
  auto cs = compiler.compile(hlsl, "cs", "cs", defines), vs = compiler.compile(hlsl, "vs", "vs", defines),
       ps = compiler.compile(hlsl, "ps", "ps", defines);
  if (!expect(!cs.empty() && !vs.empty() && !ps.empty(), "HLSL did not compile"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
  if (options.TiledResourcesTier == D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED) {
    printf("skipped: no tiled resources\n");
    return 77;
  }
  ComPtr<ID3D12CommandQueue> queue;
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));

  D3D12_ROOT_PARAMETER parameters[3] = {{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}, {D3D12_ROOT_PARAMETER_TYPE_UAV},
                                        {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  parameters[0].Constants.Num32BitValues = 3;
  parameters[2].Descriptor.ShaderRegister = 1;
  auto signature = root_signature(device.Get(), {UINT(std::size(parameters)), parameters});
  CHECK(signature ? S_OK : E_FAIL);
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc{signature.Get(), bytecode(cs)};
  ComPtr<ID3D12PipelineState> compute, graphics;
  CHECK(device->CreateComputePipelineState(&compute_desc, IID_PPV_ARGS(&compute)));
  D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics_desc{};
  graphics_desc.pRootSignature = signature.Get();
  graphics_desc.VS = bytecode(vs);
  graphics_desc.PS = bytecode(ps);
  graphics_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  graphics_desc.SampleMask = ~0u;
  graphics_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  graphics_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  graphics_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  graphics_desc.NumRenderTargets = 1;
  graphics_desc.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
  graphics_desc.SampleDesc.Count = 1;
  CHECK(device->CreateGraphicsPipelineState(&graphics_desc, IID_PPV_ARGS(&graphics)));

  // the target the draws' pixels are, one a word of a region
  D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, words / width, 1, 1,
                                  DXGI_FORMAT_R32_UINT, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                  D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &target_desc,
                                        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)));
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  device->CreateRenderTargetView(target.Get(), nullptr, rtvs->GetCPUDescriptorHandleForHeapStart());

  const UINT bytes = 2 * region;
  D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_BUFFER, 0, region, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                           D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
  D3D12_HEAP_DESC heap_desc{bytes, {D3D12_HEAP_TYPE_DEFAULT}, 0, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS};
  ComPtr<ID3D12Heap> heap;
  ComPtr<ID3D12Resource> placed[2], sparse;
  CHECK(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap)));
  for (UINT i = 0; i < 2; i++)
    CHECK(device->CreatePlacedResource(heap.Get(), i * region, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                       IID_PPV_ARGS(&placed[i])));
  desc.Width = bytes;
  CHECK(device->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&sparse)));
  for (UINT i = 0; i < std::size(mapping); i++) {
    D3D12_TILED_RESOURCE_COORDINATE coordinate{i};
    D3D12_TILE_RANGE_FLAGS flag = D3D12_TILE_RANGE_FLAG_NONE;
    queue->UpdateTileMappings(sparse.Get(), 1, &coordinate, nullptr, heap.Get(), 1, &flag, &mapping[i], nullptr,
                              D3D12_TILE_MAPPING_FLAG_NONE);
  }
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  // what the unrelated dispatches read and write
  auto scratch = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(upload && readback && scratch, "the buffers are refused"))
    return verdict();
  constexpr UINT lists_max = 20;
  ComPtr<ID3D12CommandAllocator> allocators[lists_max];
  ComPtr<ID3D12GraphicsCommandList> lists[lists_max];
  for (UINT i = 0; i < lists_max; i++) {
    CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocators[i])));
    CHECK(device->CreateCommandList(0, queue_desc.Type, allocators[i].Get(), nullptr, IID_PPV_ARGS(&lists[i])));
    CHECK(lists[i]->Close());
  }
  ComPtr<ID3D12Fence> fence;
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
  UINT64 signalled = 0;

  const D3D12_RESOURCE_BARRIER alias{D3D12_RESOURCE_BARRIER_TYPE_ALIASING};
  constexpr UINT steps = 48, rounds = 4;
  std::vector<UINT> memory(2 * words);
  for (auto &c : cases)
    for (UINT round = 0; round < rounds; round++) {
      step("%s, round %u", c.name, round);
      for (UINT i = 0; i < 2 * words; i++)
        memory[i] = i * 2654435761u + round;
      void *mapped;
      CHECK(upload->Map(0, nullptr, &mapped));
      memcpy(mapped, memory.data(), bytes);
      upload->Unmap(0, nullptr);
      CHECK(forget(readback.Get()));
      UINT used = 0;
      std::vector<ID3D12CommandList *> pending;
      auto list = [&] { return lists[used - 1].Get(); };
      auto open = [&]() -> HRESULT {
        HRESULT hr = allocators[used]->Reset();
        if (SUCCEEDED(hr))
          hr = lists[used]->Reset(allocators[used].Get(), nullptr);
        used++;
        return hr;
      };
      auto flush = [&](bool call) -> HRESULT {
        HRESULT hr = list()->Close();
        pending.push_back(list());
        if (call) {
          queue->ExecuteCommandLists(UINT(pending.size()), pending.data());
          pending.clear();
        }
        return hr;
      };
      CHECK(open());
      // the regions filled through the placed buffers
      for (UINT i = 0; i < 2; i++) {
        transition(list(), placed[i].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        list()->CopyBufferRegion(placed[i].Get(), 0, upload.Get(), i * region, region);
        transition(list(), placed[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      }
      for (UINT s = 0; s < steps; s++) {
        if (!c.between)
          list()->ResourceBarrier(1, &alias);
        if (c.split && s && s % c.split == 0) {
          CHECK(flush(c.calls));
          CHECK(open());
        }
        if (c.between) {
          const UINT constants[3] = {s, 1, 0};
          list()->SetComputeRootSignature(signature.Get());
          list()->SetPipelineState(compute.Get());
          list()->SetComputeRoot32BitConstants(0, 3, constants, 0);
          list()->SetComputeRootUnorderedAccessView(1, scratch->GetGPUVirtualAddress());
          list()->SetComputeRootUnorderedAccessView(2, scratch->GetGPUVirtualAddress() + region);
          list()->Dispatch(1, 1, 1);
          list()->ResourceBarrier(1, &alias);
        }
        // even steps read and write through the placed buffers, odd ones through the reserved buffer
        bool reserved = s & 1;
        UINT from = s & 1, to = from ^ 1, salt = s * 0x9e3779b9u + round;
        auto address = [&](UINT region_index) {
          return reserved ? sparse->GetGPUVirtualAddress() + (region_index ? 0 : region)
                          : placed[region_index]->GetGPUVirtualAddress();
        };
        Kind kind = s % c.every == 0 ? c.kind : Dispatch;
        std::vector<UINT> next = memory;
        if (kind == Copy) {
          // a copy through the buffers, whole: the reserved buffer's regions as offsets of it
          auto source = reserved ? sparse.Get() : placed[from].Get(),
               destination = reserved ? sparse.Get() : placed[to].Get();
          UINT64 source_offset = reserved ? (from ? 0 : region) : 0, destination_offset = reserved ? (to ? 0 : region) : 0;
          if (source == destination) {
            transition(list(), source, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
          } else {
            transition(list(), source, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(list(), destination, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
          }
          list()->CopyBufferRegion(destination, destination_offset, source, source_offset, region);
          if (source == destination) {
            transition(list(), source, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          } else {
            transition(list(), source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            transition(list(), destination, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          }
          for (UINT i = 0; i < words; i++)
            next[physical(reserved, to, i)] = memory[physical(reserved, from, i)];
        } else {
          UINT constants[3] = {salt, work, 0};
          if (kind == Dispatch) {
            list()->SetComputeRootSignature(signature.Get());
            list()->SetPipelineState(compute.Get());
            list()->SetComputeRoot32BitConstants(0, 3, constants, 0);
            list()->SetComputeRootUnorderedAccessView(1, address(from));
            list()->SetComputeRootUnorderedAccessView(2, address(to));
            list()->Dispatch(words / 64, 1, 1);
          } else {
            list()->SetGraphicsRootSignature(signature.Get());
            list()->SetPipelineState(graphics.Get());
            list()->SetGraphicsRoot32BitConstants(0, 3, constants, 0);
            list()->SetGraphicsRootUnorderedAccessView(1, address(from));
            list()->SetGraphicsRootUnorderedAccessView(2, address(to));
            auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
            list()->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            const D3D12_VIEWPORT viewport{0, 0, float(width), float(words / width), 0, 1};
            const D3D12_RECT scissor{0, 0, LONG(width), LONG(words / width)};
            list()->RSSetViewports(1, &viewport);
            list()->RSSetScissorRects(1, &scissor);
            list()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list()->DrawInstanced(3, 1, 0, 0);
          }
          for (UINT i = 0; i < words; i++)
            next[physical(reserved, to, i)] = memory[physical(reserved, from, (i * 7u + salt) % words)] * 3u + salt;
        }
        memory = std::move(next);
      }
      list()->ResourceBarrier(1, &alias);
      for (UINT i = 0; i < 2; i++) {
        transition(list(), placed[i].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list()->CopyBufferRegion(readback.Get(), i * region, placed[i].Get(), 0, region);
        transition(list(), placed[i].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      }
      CHECK(flush(true));
      CHECK(queue->Signal(fence.Get(), ++signalled));
      CHECK(fence->SetEventOnCompletion(signalled, nullptr));
      CHECK(device->GetDeviceRemovedReason());
      UINT *got;
      CHECK(readback->Map(0, nullptr, (void **)&got));
      UINT wrong = 0, first = 0;
      for (UINT i = 0; i < 2 * words; i++)
        if (got[i] != memory[i] && !wrong++)
          first = i;
      expect(!wrong, "%u of %u words wrong, the first heap word %u: %#x, expected %#x", wrong, 2 * words, first,
             got[first], memory[first]);
      readback->Unmap(0, nullptr);
    }
  return verdict();
}

int
main(int argc, char **argv) {
  if (argc > 2 && !strcmp(argv[2], "child"))
    return run(argc, argv);
  int result = run(argc, argv);
  if (result != 0)
    return result;
  step("every run of passes encoded on several threads");
  char exe[MAX_PATH];
  CHECK(GetModuleFileNameA(nullptr, exe, sizeof(exe)) ? S_OK : E_FAIL);
  SetEnvironmentVariableA("MULLION_D3D12_PARALLEL_COMMANDS", "1");
  SetEnvironmentVariableA("MULLION_D3D12_ENCODE_THREADS", "4");
  auto command = std::string("\"") + exe + "\" " + (argc > 1 ? argv[1] : "dxil") + " child";
  STARTUPINFOA startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  CHECK(CreateProcessA(exe, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process) ? S_OK
                                                                                                             : E_FAIL);
  DWORD waited = WaitForSingleObject(process.hProcess, 300000), code = 1;
  if (waited != WAIT_OBJECT_0)
    TerminateProcess(process.hProcess, 1);
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  expect(waited == WAIT_OBJECT_0 && code == 0, "the child's result %lu, wait %lu", code, waited);
  return verdict();
}
