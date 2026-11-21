// contract: eviction preserves data and GPU addresses, and residency completion gates later GPU use.
// "resource descriptors and other GPU virtual address references are not invalidated after Evict"
// (MakeResident); "EnqueueMakeResident signals a fence when the work is done" (Microsoft Learn).
// placed resources borrow their owning heap's residency. actual physical page-out is not observable here.
// residency applies to shader-visible descriptor heaps; a retained UAV must still clear the addressed buffer.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-makeresident
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device3-enqueuemakeresident
// https://learn.microsoft.com/en-us/windows/win32/direct3d12/creating-descriptor-heaps
#include "d3d12_test.hpp"

int main(int, char **) {
  ComPtr<ID3D12Device3> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC q{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, q.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  constexpr UINT width = 7, height = 5, words = 257;
  const UINT64 data_bytes = words * sizeof(UINT);
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, data_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 2 * data_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  auto query_result = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, sizeof(UINT64), D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(upload && readback && query_result, "residency test buffers"))
    return verdict();
  UINT *input;
  CHECK(upload->Map(0, nullptr, (void **)&input));
  for (UINT i = 0; i < words; i++)
    input[i] = i ^ (i << 16) ^ ~words;
  upload->Unmap(0, nullptr);
  ComPtr<ID3D12Fence> fence;
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
  UINT64 value = UINT64(1) << (sizeof(UINT) * 8);
  for (auto creation : {D3D12_HEAP_FLAG_NONE, D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT}) {
    D3D12_RESOURCE_DESC td{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, DXGI_FORMAT_R32_FLOAT,
                           {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    auto info = device->GetResourceAllocationInfo(0, 1, &td);
    D3D12_HEAP_DESC hd{info.SizeInBytes, {D3D12_HEAP_TYPE_DEFAULT}, info.Alignment,
                       D3D12_HEAP_FLAGS(creation | D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES)};
    ComPtr<ID3D12Heap> heap;
    ComPtr<ID3D12Resource> texture, data;
    CHECK(device->CreateHeap(&hd, IID_PPV_ARGS(&heap)));
    CHECK(device->CreatePlacedResource(heap.Get(), 0, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                       IID_PPV_ARGS(&texture)));
    D3D12_RESOURCE_DESC bd{D3D12_RESOURCE_DIMENSION_BUFFER, 0, data_bytes, 1, 1, 1, DXGI_FORMAT_UNKNOWN,
                           {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
    CHECK(device->CreateCommittedResource(&props, creation, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(&data)));
    ComPtr<ID3D12DescriptorHeap> rtvs;
    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
    auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(texture.Get(), nullptr, rtv);
    ComPtr<ID3D12DescriptorHeap> visible, cpu_views;
    D3D12_DESCRIPTOR_HEAP_DESC view_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1,
                                         D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    CHECK(device->CreateDescriptorHeap(&view_desc, IID_PPV_ARGS(&visible)));
    view_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    CHECK(device->CreateDescriptorHeap(&view_desc, IID_PPV_ARGS(&cpu_views)));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32_UINT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = words;
    auto cpu_uav = cpu_views->GetCPUDescriptorHandleForHeapStart();
    device->CreateUnorderedAccessView(data.Get(), nullptr, &uav, cpu_uav);
    device->CopyDescriptorsSimple(1, visible->GetCPUDescriptorHandleForHeapStart(), cpu_uav, view_desc.Type);
    ComPtr<ID3D12QueryHeap> queries;
    D3D12_QUERY_HEAP_DESC query_desc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 1};
    CHECK(device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&queries)));
    ID3D12Pageable *objects[] = {heap.Get(), data.Get(), visible.Get(), queries.Get()};
    if (creation == D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT)
      CHECK(device->MakeResident(2, objects));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT at;
    UINT64 bytes;
    device->GetCopyableFootprints(&td, 0, 1, 0, &at, nullptr, nullptr, &bytes);
    auto pixels = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!expect(pixels != nullptr, "pixel readback"))
      return verdict();
    float clear[4] = {0.5f};
    UINT64 before, after, cpu;
    CHECK(queue->GetClockCalibration(&before, &cpu));
    CHECK(forget(query_result.Get()));
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), nullptr));
    list->CopyBufferRegion(data.Get(), 0, upload.Get(), 0, data_bytes);
    transition(list.Get(), data.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 1, query_result.Get(), 0);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    CHECK(queue->GetClockCalibration(&after, &cpu));
    UINT64 *stamp;
    CHECK(query_result->Map(0, nullptr, (void **)&stamp));
    const UINT64 original = *stamp;
    expect(original >= before && original <= after, "initial query timestamp %llu outside %llu..%llu", original,
           before, after);
    query_result->Unmap(0, nullptr);
    const auto address = data->GetGPUVirtualAddress();
    const D3D12_RESIDENCY_PRIORITY priorities[] = {D3D12_RESIDENCY_PRIORITY_MINIMUM, D3D12_RESIDENCY_PRIORITY_LOW,
                                                  D3D12_RESIDENCY_PRIORITY_NORMAL, D3D12_RESIDENCY_PRIORITY_HIGH,
                                                  D3D12_RESIDENCY_PRIORITY_MAXIMUM};
    for (UINT round = 0; round < std::size(priorities); round++) {
      step("creation flags %#x, priority %#x, %s residency", creation, priorities[round],
           round & 1 ? "synchronous" : "queued");
      D3D12_RESIDENCY_PRIORITY requested[std::size(objects)];
      for (auto &p : requested)
        p = priorities[round];
      CHECK(device->SetResidencyPriority(std::size(objects), objects, requested));
      CHECK(device->Evict(std::size(objects), objects));
      if (round & 1) {
        CHECK(device->MakeResident(std::size(objects), objects));
      } else {
        auto flags = round ? D3D12_RESIDENCY_FLAG_DENY_OVERBUDGET : D3D12_RESIDENCY_FLAG_NONE;
        CHECK(device->EnqueueMakeResident(flags, std::size(objects), objects, fence.Get(), ++value));
        CHECK(queue->Wait(fence.Get(), value));
      }
      expect(data->GetGPUVirtualAddress() == address, "eviction changed the GPU address");
      CHECK(forget(readback.Get()));
      CHECK(forget(query_result.Get()));
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), nullptr));
      const float preserved = clear[0];
      list->CopyBufferRegion(readback.Get(), 0, data.Get(), 0, data_bytes);
      transition(list.Get(), data.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      ID3D12DescriptorHeap *heaps[]{visible.Get()};
      list->SetDescriptorHeaps(std::size(heaps), heaps);
      UINT cleared[4]{round + 1};
      list->ClearUnorderedAccessViewUint(visible->GetGPUDescriptorHandleForHeapStart(), cpu_uav, data.Get(),
                                          cleared, 0, nullptr);
      transition(list.Get(), data.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyBufferRegion(readback.Get(), data_bytes, data.Get(), 0, data_bytes);
      transition(list.Get(), data.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
      list->CopyBufferRegion(data.Get(), 0, upload.Get(), 0, data_bytes);
      transition(list.Get(), data.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
      transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION src{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      D3D12_TEXTURE_COPY_LOCATION dst{pixels.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = at}};
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
      list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 1, query_result.Get(), 0);
      clear[0] = float(round + 1) / 8;
      list->ClearRenderTargetView(rtv, clear, 0, nullptr);
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      if (!(round & 1))
        expect(fence->GetCompletedValue() == value, "residency completion value lost bits: %llu, want %llu",
               fence->GetCompletedValue(), value);
      UINT *got;
      CHECK(readback->Map(0, nullptr, (void **)&got));
      for (UINT i = 0; i < words; i++) {
        expect(got[i] == (i ^ (i << 16) ^ ~words), "resident buffer word %u changed", i);
        expect(got[words + i] == cleared[0], "resident descriptor cleared word %u to %u, want %u", i,
               got[words + i], cleared[0]);
      }
      readback->Unmap(0, nullptr);
      UINT8 *texels;
      CHECK(pixels->Map(0, nullptr, (void **)&texels));
      for (UINT y = 0; y < height; y++)
        for (UINT x = 0; x < width; x++) {
          float pixel;
          memcpy(&pixel, texels + at.Offset + y * at.Footprint.RowPitch + x * sizeof(pixel), sizeof(pixel));
          expect(pixel == preserved, "resident pixel %u,%u changed", x, y);
        }
      pixels->Unmap(0, nullptr);
      UINT64 *count;
      CHECK(query_result->Map(0, nullptr, (void **)&count));
      expect(*count == original, "resident query returned %llu, want %llu", *count, original);
      query_result->Unmap(0, nullptr);
    }
  }
  return verdict();
}
