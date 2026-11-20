// contract: an aliasing barrier orders the work before it on a queue against the work after it on that queue, for
// resources that share memory: "a usage transition between two resources that occupy the same memory" (Microsoft
// Learn, D3D12_RESOURCE_ALIASING_BARRIER), whose before and after may be NULL, "which indicates that any placed or
// reserved resource could cause aliasing". the order holds across ExecuteCommandLists calls, as all work on a queue
// runs in the order it was submitted (Executing and synchronizing command lists).
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_resource_aliasing_barrier
// a reserved buffer's tiles map a heap's memory that a placed buffer also covers. each round, one submission fills
// the memory through the placed buffer and ends with the aliasing barrier; the next clears its head through the
// reserved buffer and copies the placed buffer back after another: the head has the clear's value and the rest the
// fill's. the two submissions go to the queue before the CPU waits, so only the barrier keeps them in order. a second
// round puts a queue Wait on a fence the CPU signals only later between them, which the queue holds the second at.
#include "d3d12_test.hpp"

int
main(int argc, char **argv) {
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
  if (options.TiledResourcesTier == D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED) {
    printf("skipped: no tiled resources\n");
    return 77;
  }
  ComPtr<ID3D12CommandQueue> queue;
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocators[2];
  ComPtr<ID3D12GraphicsCommandList> lists[2];
  for (UINT i = 0; i < 2; i++) {
    CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocators[i])));
    CHECK(device->CreateCommandList(0, queue_desc.Type, allocators[i].Get(), nullptr, IID_PPV_ARGS(&lists[i])));
    CHECK(lists[i]->Close());
  }
  ComPtr<ID3D12Fence> fence, gate;
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));

  const UINT tile = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES, tiles = 4, bytes = tiles * tile;
  // the head reaches past the first tile, so the clear and the fill meet inside the second
  const UINT head = tile + 16;
  D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_BUFFER, 0, bytes, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                           D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
  D3D12_HEAP_DESC heap_desc{bytes, {D3D12_HEAP_TYPE_DEFAULT}, 0, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS};
  ComPtr<ID3D12Heap> heap;
  ComPtr<ID3D12Resource> sparse, placed;
  CHECK(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap)));
  CHECK(device->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&sparse)));
  CHECK(device->CreatePlacedResource(heap.Get(), 0, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                     IID_PPV_ARGS(&placed)));
  for (UINT i = 0; i < tiles; i++) {
    D3D12_TILED_RESOURCE_COORDINATE coordinate{i};
    D3D12_TILE_RANGE_FLAGS flag = D3D12_TILE_RANGE_FLAG_NONE;
    queue->UpdateTileMappings(sparse.Get(), 1, &coordinate, nullptr, heap.Get(), 1, &flag, &i, nullptr,
                              D3D12_TILE_MAPPING_FLAG_NONE);
  }
  constexpr UINT rounds = 64;
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto readback =
      buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, UINT64(bytes) * rounds, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(upload && readback, "the buffers are refused"))
    return verdict();
  const BYTE filled = 0x11;
  void *mapped;
  CHECK(upload->Map(0, nullptr, &mapped));
  memset(mapped, filled, bytes);
  upload->Unmap(0, nullptr);
  CHECK(forget(readback.Get()));
  ComPtr<ID3D12DescriptorHeap> cpu, gpu;
  D3D12_DESCRIPTOR_HEAP_DESC heap_views{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1};
  CHECK(device->CreateDescriptorHeap(&heap_views, IID_PPV_ARGS(&cpu)));
  heap_views.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  CHECK(device->CreateDescriptorHeap(&heap_views, IID_PPV_ARGS(&gpu)));
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{DXGI_FORMAT_R32_UINT, D3D12_UAV_DIMENSION_BUFFER};
  uav.Buffer.NumElements = bytes / 4;
  device->CreateUnorderedAccessView(sparse.Get(), nullptr, &uav, cpu->GetCPUDescriptorHandleForHeapStart());
  device->CreateUnorderedAccessView(sparse.Get(), nullptr, &uav, gpu->GetCPUDescriptorHandleForHeapStart());

  const D3D12_RESOURCE_BARRIER alias{D3D12_RESOURCE_BARRIER_TYPE_ALIASING};
  UINT64 signalled = 0;
  for (bool waited : {false, true}) {
    CHECK(forget(readback.Get()));
    for (UINT round = 0; round < rounds; round++) {
      step("round %u%s", round, waited ? ", a queue wait between the submissions" : "");
      for (auto &allocator : allocators)
        CHECK(allocator->Reset());
      // the fill, ending at the barrier
      CHECK(lists[0]->Reset(allocators[0].Get(), nullptr));
      lists[0]->CopyBufferRegion(placed.Get(), 0, upload.Get(), 0, bytes);
      lists[0]->ResourceBarrier(1, &alias);
      CHECK(lists[0]->Close());
      // the clear through the reserved buffer, and the placed buffer copied back
      CHECK(lists[1]->Reset(allocators[1].Get(), nullptr));
      ID3D12DescriptorHeap *heaps[] = {gpu.Get()};
      lists[1]->SetDescriptorHeaps(1, heaps);
      const UINT value[4] = {0x33333333u + round * 0x01010101u};
      const D3D12_RECT rect{0, 0, LONG(head / 4), 1};
      lists[1]->ClearUnorderedAccessViewUint(gpu->GetGPUDescriptorHandleForHeapStart(),
                                             cpu->GetCPUDescriptorHandleForHeapStart(), sparse.Get(), value, 1, &rect);
      lists[1]->ResourceBarrier(1, &alias);
      transition(lists[1].Get(), placed.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
      lists[1]->CopyBufferRegion(readback.Get(), UINT64(bytes) * round, placed.Get(), 0, bytes);
      transition(lists[1].Get(), placed.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
      CHECK(lists[1]->Close());
      ID3D12CommandList *first[] = {lists[0].Get()}, *second[] = {lists[1].Get()};
      queue->ExecuteCommandLists(1, first);
      if (waited)
        CHECK(queue->Wait(gate.Get(), signalled + 1));
      queue->ExecuteCommandLists(1, second);
      CHECK(queue->Signal(fence.Get(), ++signalled));
      if (waited)
        CHECK(gate->Signal(signalled));
      CHECK(fence->SetEventOnCompletion(signalled, nullptr));
    }
    BYTE *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    UINT stale = 0;
    for (UINT round = 0; round < rounds; round++) {
      const BYTE cleared = BYTE(0x33 + round);
      auto bytes_of = got + UINT64(bytes) * round;
      for (UINT i = 0; i < bytes; i++)
        if (bytes_of[i] != (i < head ? cleared : filled)) {
          if (!stale++)
            printf("round %u byte %u is %02x, want %02x\n", round, i, bytes_of[i], i < head ? cleared : filled);
          break;
        }
    }
    readback->Unmap(0, nullptr);
    expect(!stale, "%u of %u rounds read out of order%s", stale, rounds, waited ? " with a queue wait between" : "");
  }
  return verdict();
}
