// contract: a command list holds whatever an application records in it: there is no limit on a list's draws or on
// the root arguments they take (ID3D12GraphicsCommandList; a list that cannot be recorded fails Close with
// E_OUTOFMEMORY, it does not run something else). one list draws a point to every pixel of a target, each with its
// own root constants, as many as a root signature holds (D3D12_MAX_ROOT_COST), and every pixel gets its draw's
// constants folded into one word. the allocator then records another list after Reset, which draws over one pixel.
// a list that could not be recorded takes nothing from the lists before it: "each list [owns] a portion of the
// allocator", also the lists an allocator recorded earlier and has not run yet (Creating and recording command
// lists and bundles), and "if an error was encountered during recording, the error code is returned" by Close.
// an allocator's memory is what its lists record ("the allocations ... backing a command list",
// ID3D12CommandAllocator): allocators that recorded nothing do not add memory of the machine's order to what the
// process has committed, which an application that budgets by it would take for its own.
#include "d3d12_test.hpp"
#include <psapi.h>

static const char hlsl[] = R"hlsl(
cbuffer C : register(b0) { uint4 words[WORDS / 4]; };
float4 vs() : SV_Position {
  // the draw's pixel is its first word
  float2 at = (float2(words[0].x % WIDTH, words[0].x / WIDTH) + 0.5) / float2(WIDTH, HEIGHT);
  return float4(at.x * 2 - 1, 1 - at.y * 2, 0, 1);
}
uint ps() : SV_Target {
  uint fold = 0;
  for (uint i = 0; i < WORDS; i++)
    fold += words[i / 4][i % 4] * (2 * i + 1);
  return fold;
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const UINT width = 256, height = 256, words = D3D12_MAX_ROOT_COST;
  std::vector<std::string> defines{"WIDTH=" + std::to_string(width), "HEIGHT=" + std::to_string(height),
                                   "WORDS=" + std::to_string(words)};
  auto vs = compiler.compile(hlsl, "vs", "vs", defines), ps = compiler.compile(hlsl, "ps", "ps", defines);
  if (vs.empty() || ps.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER param{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
  param.Constants.Num32BitValues = words;
  auto rs = root_signature(device.Get(), {1, &param});
  if (!rs) {
    printf("failed: root signature\n");
    return 1;
  }
  const DXGI_FORMAT format = DXGI_FORMAT_R32_UINT;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs), bytecode(ps)};
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = format;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));

  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, format, {1, 0},
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
  auto pixels = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  // word `k` of the draw to pixel `index` in round `round`, and what the pixel shader folds a draw's words into
  auto word = [&](UINT round, UINT index, UINT k) { return k ? (index + round) * 2654435761u + k : index; };
  auto fold = [&](UINT round, UINT index) {
    UINT sum = 0;
    for (UINT k = 0; k < words; k++)
      sum += word(round, index, k) * (2 * k + 1);
    return sum;
  };
  // records draws to pixels [first, last) that leave the target readable in `pixels`
  auto record = [&](ID3D12GraphicsCommandList *list, UINT round, UINT first, UINT last) {
    D3D12_VIEWPORT viewport{0, 0, (float)width, (float)height, 0, 1};
    D3D12_RECT scissor{0, 0, (LONG)width, (LONG)height};
    list->SetGraphicsRootSignature(rs.Get());
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    std::vector<UINT> values(words);
    for (UINT index = first; index < last; index++) {
      for (UINT k = 0; k < words; k++)
        values[k] = word(round, index, k);
      list->SetGraphicsRoot32BitConstants(0, words, values.data(), 0);
      list->DrawInstanced(1, 1, 0, 0);
    }
    transition(list, target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
        dst{pixels.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(list, target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
  };
  auto draw = [&](UINT round, UINT first, UINT last) {
    HRESULT hr;
    if (FAILED(hr = allocator->Reset()) || FAILED(hr = list->Reset(allocator.Get(), pso.Get())))
      return hr;
    record(list.Get(), round, first, last);
    return submit(device.Get(), queue.Get(), list.Get());
  };
  unsigned wrong = 0;
  // every pixel holds the fold of the round that drew it last: `round` in [again_first, again_last), else the first
  auto check = [&](const char *what, UINT again_first, UINT again_last, UINT round = 1) -> HRESULT {
    const char *mapped;
    HRESULT hr = pixels->Map(0, nullptr, (void **)&mapped);
    if (FAILED(hr))
      return hr;
    for (UINT index = 0; index < width * height; index++) {
      UINT got = *(const UINT *)(mapped + footprint.Offset + index / width * footprint.Footprint.RowPitch + index % width * 4);
      UINT want = fold(index >= again_first && index < again_last ? round : 0, index);
      if (got != want && wrong++ < 4)
        printf("%s: pixel %u holds %08x, want %08x\n", what, index, got, want);
    }
    pixels->Unmap(0, nullptr);
    return S_OK;
  };
  CHECK(draw(0, 0, width * height));
  CHECK(check("one list", 0, 0));
  // the allocator's next list, after the long one
  const UINT again = width * height / 2;
  CHECK(draw(1, again, again + 1));
  CHECK(check("the next list", again, again + 1));
  // idle allocators, as many as a game with a list per thread and frame has
  {
    auto committed = [] {
      PROCESS_MEMORY_COUNTERS counters{sizeof(counters)};
      GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters));
      return (UINT64)counters.PagefileUsage;
    };
    MEMORYSTATUSEX memory{sizeof(memory)};
    GlobalMemoryStatusEx(&memory);
    const UINT idle = 64;
    UINT64 before = committed();
    std::vector<ComPtr<ID3D12CommandAllocator>> allocators(idle);
    std::vector<ComPtr<ID3D12GraphicsCommandList>> lists(idle);
    for (UINT i = 0; i < idle; i++) {
      CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[i])));
      CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[i].Get(), nullptr, IID_PPV_ARGS(&lists[i])));
    }
    UINT64 added = committed() - before;
    if (added >= memory.ullTotalPhys) {
      printf("%u idle allocators commit %llu MB, on a machine of %llu MB\n", idle, added >> 20, memory.ullTotalPhys >> 20);
      wrong++;
    }

    // no room to record in. the allocator records a long list and keeps it; allocators with a list each take what the
    // device has, as far as it has an end: no more of them than the machine's memory has blocks of address space
    const UINT kept = again + 1;
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), pso.Get()));
    record(list.Get(), 2, 0, kept);
    CHECK(list->Close());
    SYSTEM_INFO system;
    GetSystemInfo(&system);
    std::vector<ComPtr<ID3D12CommandAllocator>> fillers;
    HRESULT filled = S_OK;
    CHECK(lists[0]->Close());
    for (UINT64 i = 0; SUCCEEDED(filled) && i < memory.ullTotalPhys / system.dwAllocationGranularity; i++) {
      CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&fillers.emplace_back())));
      CHECK(lists[0]->Reset(fillers.back().Get(), pso.Get()));
      record(lists[0].Get(), 3, 0, 1);
      filled = lists[0]->Close();
    }
    if (FAILED(filled)) {
      // the allocator's next list, as long as the first, has no room either
      ComPtr<ID3D12GraphicsCommandList> next;
      CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&next)));
      record(next.Get(), 4, 0, kept);
      HRESULT closed = next->Close();
      if ((filled != E_OUTOFMEMORY || closed != E_OUTOFMEMORY) && wrong++ < 4)
        printf("lists without room to record in close with %#lx and %#lx, want E_OUTOFMEMORY\n", filled, closed);
      CHECK(execute(device.Get(), queue.Get(), list.Get()));
      CHECK(check("the list recorded before there was no room", 0, kept, 2));
      // the list that did not fit has nothing in it to run, for an application that runs it without a look at
      // what Close said; and with room again, a list records
      CHECK(execute(device.Get(), queue.Get(), next.Get()));
      fillers.clear();
      next.Reset();
      CHECK(draw(2, kept, kept + 1));
      CHECK(check("the list that did not fit, run, and a list after it", 0, kept + 1, 2));
    } else {
      printf("%zu allocators with a list each found no end to what the device records in\n", fillers.size());
    }
  }
  printf("%s: %u wrong pixels of %u draws with %u root constants each in one list\n", wrong ? "failed" : "passed", wrong,
         width * height, words);
  return wrong != 0;
}
