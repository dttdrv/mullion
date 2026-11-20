// contract: CPU-inaccessible heaps cannot be mapped, and accessible buffers support nested persistent mappings.
// "The CPU cannot access the heap" (D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE); Map: "Nested Map calls are supported
// and are ref-counted" and "the pointer returned is never offset by any values in pReadRange" (Microsoft Learn).
// bytes written while mapped must reach the GPU, and fenced GPU writes must reach a still-mapped readback buffer.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_cpu_page_property
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12resource-map
#include "d3d12_test.hpp"
#include <algorithm>

int main(int, char **) {
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC q{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, q.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  SYSTEM_INFO system;
  GetSystemInfo(&system);
  const UINT64 alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
  const D3D12_HEAP_PROPERTIES properties[] = {
      {D3D12_HEAP_TYPE_DEFAULT}, {D3D12_HEAP_TYPE_UPLOAD}, {D3D12_HEAP_TYPE_READBACK},
      {D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE, D3D12_MEMORY_POOL_L0},
      {D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE, D3D12_MEMORY_POOL_L0},
      {D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK, D3D12_MEMORY_POOL_L0}};
  for (const auto &props : properties)
    for (bool placed : {false, true})
      for (UINT64 size : {UINT64(1), UINT64(system.dwPageSize - 1), UINT64(system.dwPageSize),
                          UINT64(system.dwPageSize + 1), alignment - 1, alignment, alignment + 1}) {
        step("heap %u page %u, %s buffer, %llu bytes", props.Type, props.CPUPageProperty,
             placed ? "placed" : "committed", size);
        const bool accessible = props.Type != D3D12_HEAP_TYPE_DEFAULT &&
                                props.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
        const bool readback_heap = props.Type == D3D12_HEAP_TYPE_READBACK;
        const auto state = readback_heap ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_GENERIC_READ;
        D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN,
                                 {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
        ComPtr<ID3D12Heap> heap;
        ComPtr<ID3D12Resource> resource;
        if (placed) {
          D3D12_HEAP_DESC hd{alignment + (size + alignment - 1) / alignment * alignment, props, alignment,
                             D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS};
          CHECK(device->CreateHeap(&hd, IID_PPV_ARGS(&heap)));
          CHECK(device->CreatePlacedResource(heap.Get(), alignment, &desc, state, nullptr, IID_PPV_ARGS(&resource)));
        } else {
          CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                IID_PPV_ARGS(&resource)));
        }
        UINT8 sentinel;
        void *first = &sentinel;
        HRESULT hr = resource->Map(0, nullptr, &first);
        if (!accessible) {
          expect(FAILED(hr), "CPU-inaccessible heap mapped: %08lx, %p", hr, first);
          if (props.Type == D3D12_HEAP_TYPE_DEFAULT)
            expect(hr == E_INVALIDARG && first == &sentinel, "DEFAULT Map changed the failure result or output");
          if (SUCCEEDED(hr))
            resource->Unmap(0, nullptr);
          hr = resource->Map(0, nullptr, nullptr);
          expect(FAILED(hr), "inaccessible heap accepted a null output Map");
          if (SUCCEEDED(hr))
            resource->Unmap(0, nullptr);
          continue;
        }
        if (!expect(hr == S_OK && first, "accessible Map: %08lx, %p", hr, first))
          continue;
        const D3D12_RANGE ranges[] = {{0, 0}, {size_t(size / 2), size_t(size)}, {size_t(size), 0}};
        for (const auto &range : ranges) {
          void *nested = nullptr;
          hr = resource->Map(0, &range, &nested);
          expect(hr == S_OK && nested == first, "nested range %zu..%zu changed the base pointer", range.Begin, range.End);
          if (SUCCEEDED(hr))
            resource->Unmap(0, &range);
          CHECK(resource->Map(0, &range, nullptr));
          resource->Unmap(0, nullptr);
        }
        void *invalid = &sentinel;
        hr = resource->Map(1, nullptr, &invalid);
        expect(hr == E_INVALIDARG && invalid == &sentinel, "buffer subresource 1: %08lx, output %p", hr, invalid);
        if (SUCCEEDED(hr))
          resource->Unmap(1, nullptr);
        auto seed = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, size, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto result = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, size, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!expect(seed && result, "copy buffers"))
          return verdict();
        UINT8 *input;
        CHECK(seed->Map(0, nullptr, (void **)&input));
        for (UINT round = 0; round < 2; round++) {
          auto value = [=](UINT64 i) { return UINT8(i ^ (i >> 8) ^ (i >> 16) ^ (round + 1)); };
          for (UINT64 i = 0; i < size; i++) {
            input[i] = value(i);
            if (!readback_heap)
              static_cast<UINT8 *>(first)[i] = value(i);
          }
          CHECK(allocator->Reset());
          CHECK(list->Reset(allocator.Get(), nullptr));
          if (readback_heap)
            list->CopyBufferRegion(resource.Get(), 0, seed.Get(), 0, size);
          else
            list->CopyBufferRegion(result.Get(), 0, resource.Get(), 0, size);
          CHECK(submit(device.Get(), queue.Get(), list.Get()));
          UINT8 *got = static_cast<UINT8 *>(first);
          if (!readback_heap)
            CHECK(result->Map(0, nullptr, (void **)&got));
          UINT64 bad = size;
          for (UINT64 i = 0; i < size; i++)
            if (got[i] != value(i)) {
              bad = i;
              break;
            }
          expect(bad == size, "persistent round %u first wrong byte %llu of %llu", round, bad, size);
          if (!readback_heap)
            result->Unmap(0, nullptr);
        }
        seed->Unmap(0, nullptr);
        resource->Unmap(0, nullptr);
      }
  return verdict();
}
