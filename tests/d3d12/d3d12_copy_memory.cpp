// contract: a command allocator holds "the allocations ... backing a command list" (ID3D12CommandAllocator), so what it
// keeps for a copy is of the copy's order. a depth texture copied into a color texture of its type group goes by its
// bytes (CopyTextureRegion: the formats need only be "compatible"; D32_FLOAT and R32_FLOAT are both R32_TYPELESS), and
// the memory those bytes take, DXGI's CurrentUsage of a committed buffer of them ("current video memory usage, in
// bytes", DXGI_QUERY_VIDEO_MEMORY_INFO), is the measure: allocators that each copied, allocators that copied every
// frame in turn, and an allocator whose last lists copied less than earlier ones keep at most twice what their last
// lists copied. every copy lands: each frame's colors hold the depths its list cleared before each copy
#include "d3d12_test.hpp"
#include <dxgi1_4.h>

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler))
    return 77;
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<IDXGIFactory4> factory;
  ComPtr<IDXGIAdapter3> adapter;
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  CHECK(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)));
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  ComPtr<ID3D12CommandQueue> queue;
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12Fence> fence;
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
  UINT64 fenced = 0;
  auto event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  auto finish = [&] {
    queue->Signal(fence.Get(), ++fenced);
    fence->SetEventOnCompletion(fenced, event);
    WaitForSingleObject(event, INFINITE);
  };
  auto usage = [&]() -> INT64 {
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info);
    return info.CurrentUsage;
  };

  // copies of a small depth texture, `copies` to a list, into the slots of a color row, and of a large one
  const UINT small = 64, large = 1024, copies = 8;
  const D3D12_HEAP_PROPERTIES gpu{D3D12_HEAP_TYPE_DEFAULT};
  auto texture = [&](UINT width, UINT height, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                     D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear) {
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, format, {1, 0},
                             D3D12_TEXTURE_LAYOUT_UNKNOWN, flags};
    ComPtr<ID3D12Resource> made;
    device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &desc, state, clear, IID_PPV_ARGS(&made));
    return made;
  };
  D3D12_CLEAR_VALUE cleared{DXGI_FORMAT_D32_FLOAT, {.DepthStencil = {0, 0}}};
  auto depth = texture(small, small, DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                       D3D12_RESOURCE_STATE_DEPTH_WRITE, &cleared),
       big_depth = texture(large, large, DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                           D3D12_RESOURCE_STATE_COPY_SOURCE, &cleared),
       row = texture(small * copies, small, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE,
                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr),
       big_color = texture(large, large, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, nullptr);
  if (!depth || !big_depth || !row || !big_color) {
    printf("failed: textures\n");
    return 1;
  }
  ComPtr<ID3D12DescriptorHeap> dsv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
  CHECK(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsv_heap)));
  auto dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateDepthStencilView(depth.Get(), nullptr, dsv);
  auto row_desc = row->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 row_bytes;
  device->GetCopyableFootprints(&row_desc, 0, 1, 0, &footprint, nullptr, nullptr, &row_bytes);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, row_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  // what the bytes of one small copy take. nothing is released until the end: Metal frees what a released object held
  // once the command buffers that used it go, which would take from what a later step measures
  INT64 before = usage();
  auto one = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, UINT64(small) * small * sizeof(float),
                    D3D12_RESOURCE_STATE_COMMON);
  const INT64 copied = usage() - before;
  if (!readback || copied <= 0) {
    printf("failed: a buffer of %u bytes takes %lld bytes\n", small * small * 4, copied);
    return 1;
  }

  ComPtr<ID3D12GraphicsCommandList> list;
  // a list of `count` copies of the small depth, each cleared first to the depth of its frame and slot
  auto value = [&](UINT frame, UINT slot) { return float(frame * copies + slot + 1) / float(64 * copies); };
  auto record = [&](ID3D12CommandAllocator *allocator, UINT frame, UINT count) {
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator, nullptr));
    for (UINT slot = 0; slot < count; slot++) {
      list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, value(frame, slot), 0, 0, nullptr);
      transition(list.Get(), depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION src{depth.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
          dst{row.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      list->CopyTextureRegion(&dst, slot * small, 0, 0, &src, nullptr);
      transition(list.Get(), depth.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }
    transition(list.Get(), row.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{row.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
        dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(list.Get(), row.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    CHECK(list->Close());
    ID3D12CommandList *lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    finish();
    return 0;
  };
  // the slots a list copied hold its depths
  auto check = [&](UINT frame, UINT count) {
    const char *mapped;
    if (!expect(SUCCEEDED(readback->Map(0, nullptr, (void **)&mapped)), "readback does not map"))
      return;
    for (UINT slot = 0; slot < count; slot++)
      for (UINT y = 0; y < small; y += small - 1) {
        float got = ((const float *)(mapped + footprint.Offset + y * footprint.Footprint.RowPitch))[slot * small];
        expect(got == value(frame, slot), "frame %u slot %u row %u holds %g, want %g", frame, slot, y, got,
               value(frame, slot));
      }
    readback->Unmap(0, nullptr);
  };
  ComPtr<ID3D12CommandAllocator> warm;
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&warm)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, warm.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  // the queue's first work, which is not the allocators'
  if (record(warm.Get(), 0, 1))
    return 1;
  check(0, 1);

  step("allocators that each copied once");
  std::vector<ComPtr<ID3D12CommandAllocator>> allocators(copies);
  {
    before = usage();
    for (UINT i = 0; i < copies; i++) {
      CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[i])));
      if (record(allocators[i].Get(), i, 1))
        return 1;
      check(i, 1);
    }
    INT64 kept = usage() - before;
    expect(kept <= 2 * INT64(copies) * copied, "%u allocators that copied %lld bytes each keep %lld bytes", copies,
           copied, kept);
  }

  step("allocators that copied in turn, frame after frame");
  {
    const UINT turns = 3, frames = 4 * turns;
    for (UINT i = 0; i < turns; i++)
      CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators.emplace_back())));
    before = usage();
    for (UINT frame = 0; frame < frames; frame++) {
      if (record(allocators[copies + frame % turns].Get(), frame, copies))
        return 1;
      check(frame, copies);
    }
    INT64 kept = usage() - before;
    expect(kept <= 2 * INT64(turns * copies) * copied,
           "%u allocators that copied %u times %lld bytes each frame in turn keep %lld bytes", turns, copies, copied,
           kept);
  }

  step("an allocator whose last lists copied less than its first");
  {
    auto &allocator = allocators.emplace_back();
    CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    before = usage();
    CHECK(list->Reset(allocator.Get(), nullptr));
    D3D12_TEXTURE_COPY_LOCATION src{big_depth.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
        dst{big_color.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    CHECK(list->Close());
    ID3D12CommandList *lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    finish();
    for (UINT frame = 0; frame < 2; frame++) {
      if (record(allocator.Get(), frame, 1))
        return 1;
      check(frame, 1);
    }
    INT64 kept = usage() - before;
    expect(kept <= 2 * copied, "an allocator whose last lists copied %lld bytes keeps %lld bytes", copied, kept);
  }
  CloseHandle(event);
  return verdict();
}
