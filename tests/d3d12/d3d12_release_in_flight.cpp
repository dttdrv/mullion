// contract: the work a queue was given runs as recorded when the application releases, right after
// ExecuteCommandLists, the resources and the heap that work reads and writes, and the process goes on. Direct3D 12
// does not allow the release ("Applications must not release ... until the GPU has finished", and the debug layer's
// OBJECT_DELETED_WHILE_STILL_IN_USE), Windows' drivers mostly survive it, and games do it: vkd3d-proton keeps a
// resource released in flight until the pending submissions complete for them (libs/vkd3d/resource.c,
// d3d12_device_add_queue_timeline_deferred_decref; its test_resource_retain_workaround is this case). here a list
// copies 64 upload buffers and a texture's worth of bytes into two outputs; one output stays, and everything else is
// released as soon as the list is given: committed buffers, a placed buffer with its heap, and a texture. the output
// that stays has every byte, twice: after a wait, and with a second list recorded while the first may still run.
#include "d3d12_test.hpp"
#include <vector>

int
main() {
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  ComPtr<ID3D12Fence> fence;
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
  auto event = CreateEventA(nullptr, FALSE, FALSE, nullptr);

  constexpr UINT kInputs = 64, kWords = 4096, kBytes = kWords * sizeof(UINT), kSide = 64;
  // the texture's texels follow the inputs' words in the output
  constexpr UINT64 kTotal = UINT64(kInputs) * kBytes + kSide * kSide * sizeof(UINT);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, kTotal, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(readback != nullptr, "readback buffer"))
    return verdict();
  UINT64 signal = 0;

  for (UINT round = 0; round < 8; round++) {
    step("round %u", round);
    // word w of input i is i * kWords + w + round + 1, texel t of the texture follows on
    std::vector<ComPtr<ID3D12Resource>> inputs(kInputs);
    std::vector<UINT> words(kWords);
    for (UINT i = 0; i < kInputs; i++) {
      inputs[i] = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, kBytes, D3D12_RESOURCE_STATE_GENERIC_READ);
      if (!expect(inputs[i] != nullptr, "input %u", i))
        return verdict();
      for (UINT w = 0; w < kWords; w++)
        words[w] = i * kWords + w + round + 1;
      void *mapped;
      CHECK(inputs[i]->Map(0, nullptr, &mapped));
      memcpy(mapped, words.data(), kBytes);
      inputs[i]->Unmap(0, nullptr);
    }
    D3D12_HEAP_DESC heap_desc{kTotal + D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT, {D3D12_HEAP_TYPE_DEFAULT}, 0,
                              D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS};
    ComPtr<ID3D12Heap> heap;
    CHECK(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap)));
    D3D12_RESOURCE_DESC buffer_desc{D3D12_RESOURCE_DIMENSION_BUFFER, 0, kTotal, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                                    D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
    ComPtr<ID3D12Resource> placed, kept, texture;
    CHECK(device->CreatePlacedResource(heap.Get(), 0, &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                       IID_PPV_ARGS(&placed)));
    kept = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, kTotal, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_RESOURCE_DESC texture_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, kSide, kSide, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0}};
    D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
    CHECK(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &texture_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                          nullptr, IID_PPV_ARGS(&texture)));
    if (!expect(kept != nullptr, "the output that stays"))
      return verdict();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT texels{UINT64(kInputs) * kBytes, {DXGI_FORMAT_R32_UINT, kSide, kSide, 1, kSide * sizeof(UINT)}};
    D3D12_TEXTURE_COPY_LOCATION of_texture{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};

    CHECK(forget(readback.Get()));
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), nullptr));
    // the inputs into the placed buffer, its first texels' worth into the texture, and all of it into the one that
    // stays: every copy reads or writes something released below
    for (UINT i = 0; i < kInputs; i++)
      list->CopyBufferRegion(placed.Get(), UINT64(i) * kBytes, inputs[i].Get(), 0, kBytes);
    transition(list.Get(), placed.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from_placed{placed.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = texels}};
    from_placed.PlacedFootprint.Offset = 0;
    list->CopyTextureRegion(&of_texture, 0, 0, 0, &from_placed, nullptr);
    list->CopyBufferRegion(kept.Get(), 0, placed.Get(), 0, UINT64(kInputs) * kBytes);
    transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION to_kept{kept.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = texels}};
    list->CopyTextureRegion(&to_kept, 0, 0, 0, &of_texture, nullptr);
    CHECK(list->Close());
    ID3D12CommandList *lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    // released while the list is in flight
    for (auto &input : inputs)
      input.Reset();
    placed.Reset();
    heap.Reset();
    texture.Reset();
    // odd rounds record the readback at once, while the first list may still run; even rounds wait for it
    if (!(round & 1)) {
      CHECK(queue->Signal(fence.Get(), ++signal));
      CHECK(fence->SetEventOnCompletion(signal, event));
      WaitForSingleObject(event, INFINITE);
    }
    ComPtr<ID3D12CommandAllocator> second_allocator;
    ComPtr<ID3D12GraphicsCommandList> second;
    CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&second_allocator)));
    CHECK(device->CreateCommandList(0, queue_desc.Type, second_allocator.Get(), nullptr, IID_PPV_ARGS(&second)));
    transition(second.Get(), kept.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    second->CopyResource(readback.Get(), kept.Get());
    CHECK(submit(device.Get(), queue.Get(), second.Get()));
    const UINT *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    unsigned wrong = 0;
    for (UINT64 w = 0; w < kTotal / sizeof(UINT); w++) {
      // the texels are the placed buffer's first, which are input 0's
      UINT want = UINT(w < UINT64(kInputs) * kWords ? w : w - UINT64(kInputs) * kWords) + round + 1;
      if (got[w] != want && wrong++ < 4)
        printf("  word %llu is %u, want %u\n", w, got[w], want);
    }
    readback->Unmap(0, nullptr);
    expect(!wrong, "round %u: %u of %llu words wrong", round, wrong, kTotal / sizeof(UINT));
  }
  CloseHandle(event);
  return verdict();
}
