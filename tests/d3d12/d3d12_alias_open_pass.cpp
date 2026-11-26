// contract: an aliasing barrier orders the work before it on a queue against the work after it, for resources that
// share memory (D3D12_RESOURCE_ALIASING_BARRIER, whose before and after may be NULL: "any placed or reserved resource
// could cause aliasing"), when it falls between two draws of one render pass that write only its target. a slow
// dispatch writes a region of a heap through a placed buffer; then, in the same list and with no other barrier, a
// draw writes the target, the barrier comes, and a draw whose pixels read the region through another placed buffer
// on the same memory, or through a reserved buffer whose tiles map it in another order, writes what it read to the
// target. every pixel is what the dispatch wrote. also with the barrier naming two unrelated buffers, with many
// barriers, and in a list of several such passes. argv[2] "child": the same in a process that encodes every run of
// passes on several threads (MULLION_D3D12_PARALLEL_COMMANDS, MULLION_D3D12_ENCODE_THREADS); the test starts it itself.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_resource_aliasing_barrier
#include "d3d12_test.hpp"

static constexpr UINT tile = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES, tile_words = tile / 4;
// two tiles; the reserved buffer's tile 0 maps heap tile 1 and its tile 1 heap tile 0
static constexpr UINT tiles = 2, words = tiles * tile_words, bytes = words * 4, width = 256, height = words / width;
static constexpr UINT mapping[tiles] = {1, 0};
// so many iterations a thread that a draw which did not wait for the dispatch would read before it wrote
static constexpr UINT work = 1u << 13;

static const char hlsl[] = R"hlsl(
cbuffer Step : register(b0) { uint salt; uint work; uint reserved; };
RWByteAddressBuffer destination : register(u0);
ByteAddressBuffer source : register(t0);
[numthreads(64, 1, 1)] void cs(uint3 id : SV_DispatchThreadID) {
  uint v = id.x;
  [loop] for (uint i = 0; i < work; i++)
    v = v * 1664525u + 1013904223u;
  destination.Store(id.x * 4, id.x * 2654435761u + salt + (v & 0));
}
float4 vs(uint id : SV_VertexID) : SV_Position {
  return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0, 1);
}
uint ps_fill(float4 position : SV_Position) : SV_Target {
  return 0xdeadbeef;
}
uint ps_read(float4 position : SV_Position) : SV_Target {
  uint word = uint(position.y) * WIDTH + uint(position.x);
  // the reserved buffer's tiles are the heap's in another order
  if (reserved)
    word ^= TILE_WORDS;
  return source.Load(word * 4);
}
)hlsl";

struct Case {
  const char *name;
  bool reserved;
  // the barrier names two unrelated buffers; how many barriers it is; how many passes of the list
  bool named;
  UINT barriers, passes;
};
static const Case cases[] = {
    {"placed", false, false, 1, 1},
    {"reserved", true, false, 1, 1},
    {"placed, the barrier naming other buffers", false, true, 1, 1},
    {"reserved, the barrier naming other buffers", true, true, 1, 1},
    {"placed, 50 barriers", false, false, 50, 1},
    {"reserved, 50 barriers", true, false, 50, 1},
    {"placed, 6 passes", false, false, 3, 6},
    {"reserved, 6 passes", true, false, 3, 6},
};

static int
run(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const std::vector<std::string> defines = {"WIDTH=" + std::to_string(width), "TILE_WORDS=" + std::to_string(tile_words)};
  auto cs = compiler.compile(hlsl, "cs", "cs", defines), vs = compiler.compile(hlsl, "vs", "vs", defines),
       ps_fill = compiler.compile(hlsl, "ps_fill", "ps", defines),
       ps_read = compiler.compile(hlsl, "ps_read", "ps", defines);
  if (!expect(!cs.empty() && !vs.empty() && !ps_fill.empty() && !ps_read.empty(), "HLSL did not compile"))
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
                                        {D3D12_ROOT_PARAMETER_TYPE_SRV}};
  parameters[0].Constants.Num32BitValues = 3;
  auto signature = root_signature(device.Get(), {UINT(std::size(parameters)), parameters});
  CHECK(signature ? S_OK : E_FAIL);
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc{signature.Get(), bytecode(cs)};
  ComPtr<ID3D12PipelineState> compute, fill, read;
  CHECK(device->CreateComputePipelineState(&compute_desc, IID_PPV_ARGS(&compute)));
  D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics_desc{};
  graphics_desc.pRootSignature = signature.Get();
  graphics_desc.VS = bytecode(vs);
  graphics_desc.PS = bytecode(ps_fill);
  graphics_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  graphics_desc.SampleMask = ~0u;
  graphics_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  graphics_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  graphics_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  graphics_desc.NumRenderTargets = 1;
  graphics_desc.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
  graphics_desc.SampleDesc.Count = 1;
  CHECK(device->CreateGraphicsPipelineState(&graphics_desc, IID_PPV_ARGS(&fill)));
  graphics_desc.PS = bytecode(ps_read);
  CHECK(device->CreateGraphicsPipelineState(&graphics_desc, IID_PPV_ARGS(&read)));

  D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, DXGI_FORMAT_R32_UINT,
                                  {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &target_desc,
                                        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)));
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  device->CreateRenderTargetView(target.Get(), nullptr, rtvs->GetCPUDescriptorHandleForHeapStart());

  D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_BUFFER, 0, bytes, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                           D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
  D3D12_HEAP_DESC heap_desc{bytes, {D3D12_HEAP_TYPE_DEFAULT}, 0, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS};
  ComPtr<ID3D12Heap> heap;
  // the dispatch's buffer and the draw's, on the same memory
  ComPtr<ID3D12Resource> written, alias, sparse;
  CHECK(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap)));
  CHECK(device->CreatePlacedResource(heap.Get(), 0, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                     IID_PPV_ARGS(&written)));
  CHECK(device->CreatePlacedResource(heap.Get(), 0, &desc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&alias)));
  CHECK(device->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&sparse)));
  for (UINT i = 0; i < tiles; i++) {
    D3D12_TILED_RESOURCE_COORDINATE coordinate{i};
    D3D12_TILE_RANGE_FLAGS flag = D3D12_TILE_RANGE_FLAG_NONE;
    queue->UpdateTileMappings(sparse.Get(), 1, &coordinate, nullptr, heap.Get(), 1, &flag, &mapping[i], nullptr,
                              D3D12_TILE_MAPPING_FLAG_NONE);
  }
  // what a barrier that names resources names
  auto other_a = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_STATE_COMMON),
       other_b = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_STATE_COMMON);
  const UINT64 pitch = width * 4;
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, pitch * height, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(readback && other_a && other_b, "the buffers are refused"))
    return verdict();
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  ComPtr<ID3D12Fence> fence;
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
  UINT64 signalled = 0;

  for (UINT round = 0; round < 3; round++)
    for (auto &c : cases) {
      step("%s, round %u", c.name, round);
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), nullptr));
      auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
      const D3D12_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
      const D3D12_RECT scissor{0, 0, LONG(width), LONG(height)};
      D3D12_RESOURCE_BARRIER barrier{D3D12_RESOURCE_BARRIER_TYPE_ALIASING};
      if (c.named)
        barrier.Aliasing = {other_a.Get(), other_b.Get()};
      UINT salt = 0;
      for (UINT pass = 0; pass < c.passes; pass++) {
        salt = (round * 16 + pass) * 0x9e3779b9u + UINT(&c - cases);
        const UINT constants[3] = {salt, work, c.reserved};
        // the slow dispatch, with no barrier after it but the aliasing one in the pass
        list->SetComputeRootSignature(signature.Get());
        list->SetPipelineState(compute.Get());
        list->SetComputeRoot32BitConstants(0, 3, constants, 0);
        list->SetComputeRootUnorderedAccessView(1, written->GetGPUVirtualAddress());
        list->Dispatch(words / 64, 1, 1);
        list->SetGraphicsRootSignature(signature.Get());
        list->SetGraphicsRoot32BitConstants(0, 3, constants, 0);
        list->SetGraphicsRootShaderResourceView(2, (c.reserved ? sparse : alias)->GetGPUVirtualAddress());
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->SetPipelineState(fill.Get());
        list->DrawInstanced(3, 1, 0, 0);
        for (UINT b = 0; b < c.barriers; b++) {
          list->ResourceBarrier(1, &barrier);
          if (b + 1 < c.barriers)
            list->DrawInstanced(3, 1, 0, 0);
        }
        list->SetPipelineState(read.Get());
        list->DrawInstanced(3, 1, 0, 0);
        // the next pass's dispatch writes the memory this pass's draws read
        D3D12_RESOURCE_BARRIER after{D3D12_RESOURCE_BARRIER_TYPE_ALIASING};
        if (pass + 1 < c.passes)
          list->ResourceBarrier(1, &after);
      }
      transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION destination{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
      destination.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_UINT, width, height, 1, UINT(pitch)};
      D3D12_TEXTURE_COPY_LOCATION source{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
      transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
      CHECK(list->Close());
      ID3D12CommandList *lists[] = {list.Get()};
      queue->ExecuteCommandLists(1, lists);
      CHECK(queue->Signal(fence.Get(), ++signalled));
      CHECK(fence->SetEventOnCompletion(signalled, nullptr));
      CHECK(device->GetDeviceRemovedReason());
      UINT *got;
      CHECK(readback->Map(0, nullptr, (void **)&got));
      UINT wrong = 0, first = ~0u;
      for (UINT i = 0; i < words; i++)
        if (got[i] != i * 2654435761u + salt && wrong++ == 0)
          first = i;
      readback->Unmap(0, nullptr);
      expect(!wrong, "%s, round %u: %u of %u pixels are not what the dispatch wrote (pixel %u: %#x, want %#x)", c.name,
             round, wrong, words, first, wrong ? got[first] : 0, wrong ? first * 2654435761u + salt : 0);
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
