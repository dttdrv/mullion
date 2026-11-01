// contract: enhanced barriers express aliasing as an access to or from D3D12_BARRIER_ACCESS_NO_ACCESS ("Barriers may
// set AccessAfter to D3D12_BARRIER_ACCESS_NO_ACCESS to indicate the subresource is being 'deactivated'", and a later
// one's AccessBefore of NO_ACCESS activates the other resource; Enhanced Barriers spec, Resource Aliasing), and work on
// the resource deactivated completes before the one activated is used, as across a legacy ALIASING barrier.
// 1. two placed render targets at one offset of a heap: the first is cleared to red, a texture barrier group hands it
//    to NO_ACCESS and the second from NO_ACCESS (LayoutBefore UNDEFINED, with DISCARD), and the second is cleared to
//    green. it reads all green.
// 2. a reserved buffer whose tiles map the heap a placed buffer is at: what a copy writes through one, a copy after a
//    buffer barrier group (the writer to NO_ACCESS, the reader from it) reads through the other, both ways.
// https://microsoft.github.io/DirectX-Specs/d3d/D3D12EnhancedBarriers.html
#include "d3d12_test.hpp"

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  ComPtr<ID3D12Device10> device;
  if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
    printf("skipped: no ID3D12Device10\n");
    return 77;
  }
  D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options12, sizeof(options12)));
  if (!options12.EnhancedBarriersSupported) {
    printf("skipped: no enhanced barriers\n");
    return 77;
  }
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
  ComPtr<ID3D12CommandQueue> queue;
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  ComPtr<ID3D12GraphicsCommandList7> list;
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  const UINT pitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
  constexpr UINT64 tile = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, tile, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(bool(readback), "the readback buffer is refused"))
    return verdict();

  {
    step("aliased render targets cleared across a texture barrier group");
    constexpr UINT size = 16;
    const D3D12_RESOURCE_DESC1 desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1,
                                    DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                    D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    auto info = device->GetResourceAllocationInfo(0, 1, (const D3D12_RESOURCE_DESC *)&desc);
    const D3D12_HEAP_DESC heap_desc{info.SizeInBytes, {D3D12_HEAP_TYPE_DEFAULT}, info.Alignment,
                                    D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES};
    ComPtr<ID3D12Heap> heap;
    CHECK(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap)));
    ComPtr<ID3D12Resource> textures[2];
    for (auto &texture : textures)
      CHECK(device->CreatePlacedResource2(heap.Get(), 0, &desc, D3D12_BARRIER_LAYOUT_RENDER_TARGET, nullptr, 0,
                                          nullptr, IID_PPV_ARGS(&texture)));
    ComPtr<ID3D12DescriptorHeap> rtvs;
    const D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2};
    CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[2] = {rtvs->GetCPUDescriptorHandleForHeapStart(),
                                          rtvs->GetCPUDescriptorHandleForHeapStart()};
    rtv[1].ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT i = 0; i < 2; i++)
      device->CreateRenderTargetView(textures[i].Get(), nullptr, rtv[i]);

    CHECK(forget(readback.Get()));
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), nullptr));
    const float red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1};
    list->ClearRenderTargetView(rtv[0], red, 0, nullptr);
    D3D12_TEXTURE_BARRIER barriers[2]{};
    barriers[0] = {D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_RENDER_TARGET,
                   D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_LAYOUT_RENDER_TARGET,
                   D3D12_BARRIER_LAYOUT_RENDER_TARGET, textures[0].Get(), {0xffffffff}};
    barriers[1] = {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_NO_ACCESS,
                   D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_UNDEFINED,
                   D3D12_BARRIER_LAYOUT_RENDER_TARGET, textures[1].Get(), {0xffffffff},
                   D3D12_TEXTURE_BARRIER_FLAG_DISCARD};
    D3D12_BARRIER_GROUP group{D3D12_BARRIER_TYPE_TEXTURE, 2};
    group.pTextureBarriers = barriers;
    list->Barrier(1, &group);
    list->ClearRenderTargetView(rtv[1], green, 0, nullptr);
    D3D12_TEXTURE_BARRIER copy{D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_SYNC_COPY,
                               D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_ACCESS_COPY_SOURCE,
                               D3D12_BARRIER_LAYOUT_RENDER_TARGET, D3D12_BARRIER_LAYOUT_COPY_SOURCE, textures[1].Get(),
                               {0xffffffff}};
    group.NumBarriers = 1;
    group.pTextureBarriers = &copy;
    list->Barrier(1, &group);
    D3D12_TEXTURE_COPY_LOCATION src{textures[1].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
        dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint = {0, {DXGI_FORMAT_R8G8B8A8_UNORM, size, size, 1, pitch}};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    BYTE *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    UINT wrong = 0;
    for (UINT y = 0; y < size; y++)
      for (UINT x = 0; x < size; x++) {
        UINT32 texel;
        memcpy(&texel, got + y * pitch + x * 4, 4);
        if (texel != 0xff00ff00u && !wrong++)
          printf("texel %u,%u is %08x, want ff00ff00\n", x, y, texel);
      }
    readback->Unmap(0, nullptr);
    expect(!wrong, "%u wrong texels", wrong);
  }

  if (options.TiledResourcesTier < D3D12_TILED_RESOURCES_TIER_1) {
    printf("no reserved resources\n");
    return verdict();
  }
  const D3D12_RESOURCE_DESC1 desc{D3D12_RESOURCE_DIMENSION_BUFFER, 0, tile, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
  const D3D12_HEAP_DESC heap_desc{tile, {D3D12_HEAP_TYPE_DEFAULT}, 0, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS};
  ComPtr<ID3D12Heap> heap;
  CHECK(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap)));
  ComPtr<ID3D12Resource> placed, reserved;
  CHECK(device->CreatePlacedResource2(heap.Get(), 0, &desc, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, 0, nullptr,
                                      IID_PPV_ARGS(&placed)));
  CHECK(device->CreateReservedResource2((const D3D12_RESOURCE_DESC *)&desc, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr,
                                        nullptr, 0, nullptr, IID_PPV_ARGS(&reserved)));
  queue->UpdateTileMappings(reserved.Get(), 1, nullptr, nullptr, heap.Get(), 1, nullptr, nullptr, nullptr,
                            D3D12_TILE_MAPPING_FLAG_NONE);
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 2 * tile, D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(bool(upload), "the upload buffer is refused"))
    return verdict();
  UINT32 *words;
  CHECK(upload->Map(0, nullptr, (void **)&words));
  for (UINT i = 0; i < 2 * tile / 4; i++)
    words[i] = i + 1;
  upload->Unmap(0, nullptr);

  for (UINT round = 0; round < 4; round++) {
    // even rounds write through the placed buffer and read through the reserved one, odd ones the other way
    auto writer = round & 1 ? reserved.Get() : placed.Get(), reader = round & 1 ? placed.Get() : reserved.Get();
    const UINT64 from = (round & 2) ? tile : 0;
    step("round %u: a copy through the %s buffer, read through the other after a buffer barrier group", round,
         round & 1 ? "reserved" : "placed");
    CHECK(forget(readback.Get()));
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), nullptr));
    D3D12_BUFFER_BARRIER barriers[2] = {
        {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_NO_ACCESS,
         D3D12_BARRIER_ACCESS_COPY_DEST, writer, 0, UINT64_MAX},
    };
    D3D12_BARRIER_GROUP group{D3D12_BARRIER_TYPE_BUFFER, 1};
    group.pBufferBarriers = barriers;
    list->Barrier(1, &group);
    list->CopyBufferRegion(writer, 0, upload.Get(), from, tile);
    barriers[0] = {D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_COPY_DEST,
                   D3D12_BARRIER_ACCESS_NO_ACCESS, writer, 0, UINT64_MAX};
    barriers[1] = {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_NO_ACCESS,
                   D3D12_BARRIER_ACCESS_COPY_SOURCE, reader, 0, UINT64_MAX};
    group.NumBarriers = 2;
    list->Barrier(1, &group);
    list->CopyBufferRegion(readback.Get(), 0, reader, 0, tile);
    barriers[0] = {D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_COPY_SOURCE,
                   D3D12_BARRIER_ACCESS_NO_ACCESS, reader, 0, UINT64_MAX};
    group.NumBarriers = 1;
    list->Barrier(1, &group);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    UINT32 *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    UINT wrong = 0;
    for (UINT i = 0; i < tile / 4; i++) {
      UINT32 want = UINT32(from / 4 + i + 1);
      if (got[i] != want && !wrong++)
        printf("word %u is %u, want %u\n", i, got[i], want);
    }
    readback->Unmap(0, nullptr);
    expect(!wrong, "%u wrong words", wrong);
  }
  return verdict();
}
