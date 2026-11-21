// contract: an imported heap uses the caller's allocation, including GPU writes to those same bytes; an UPLOAD
// write-watch heap tracks CPU writes. Microsoft Learn, OpenExistingHeapFromAddress: "Creates a special-purpose
// diagnostic heap in system memory from an address"; D3D12_HEAP_FLAG_ALLOW_WRITE_WATCH: "The heap supports
// MEM_WRITE_WATCH functionality". GetWriteWatch "retrieves the addresses of the pages that are written to".
// vkd3d-proton's test_open_heap_from_address records rejection of an interior address and Map returning the
// original address. its heap description checks say "Expected Heap desc to have SHARED_CROSS_ADAPTER flag set."
// and "Expected heap desc to have SHARED flag set." (d3d12_win32_exclusive.c:114-115, also 177-178 for file mappings).
// sizes cross placement and allocation boundaries, one page below both; GPU copies in both directions compare the
// entire allocation.
#include "d3d12_test.hpp"
#include <random>

int
main(int argc, char **argv) {
  const UINT seed = argc > 2 ? strtoul(argv[2], nullptr, 0) : 1;
  std::minstd_rand random(seed);
  ComPtr<ID3D12Device3> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC q{};
  CHECK(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
  SYSTEM_INFO system;
  GetSystemInfo(&system);
  // the system's own write watch: one written page is one dirty page on Windows, but Wine on a host of larger pages
  // (16KB, Apple silicon without 4KB pages) can only report each host page it saw written, its neighbors with it.
  // there the write watch checks below cannot hold whatever the library does, and are left out
  bool exact_write_watch = false;
  if (auto probe = (BYTE *)VirtualAlloc(nullptr, system.dwAllocationGranularity, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH,
                                        PAGE_READWRITE)) {
    void *dirty[64];
    ULONG_PTR count = std::size(dirty);
    DWORD granularity;
    ResetWriteWatch(probe, system.dwAllocationGranularity);
    probe[system.dwPageSize] = 1;
    exact_write_watch = !GetWriteWatch(0, probe, system.dwAllocationGranularity, dirty, &count, &granularity) &&
                        count == 1 && dirty[0] == probe + system.dwPageSize;
    VirtualFree(probe, 0, MEM_RELEASE);
  }
  if (!exact_write_watch)
    printf("note: the system's write watch reports more than the written page; write watch heaps are not checked\n");
  // one page too, smaller than a buffer's 64KB placement alignment: "HeapOffset plus the resource size must be smaller
  // than or equal to the heap size" (ID3D12Device::CreatePlacedResource), and a buffer's size is its width, not its
  // 64KB allocation size (Starfield places one at the start of a heap it imported from a page, dttdrv/mullion#32)
  // https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createplacedresource
  for (SIZE_T size : {SIZE_T(system.dwPageSize), SIZE_T(D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT),
                      SIZE_T(D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT * 3)}) {
    for (bool mapping : {false, true}) {
      step("seed %u, imported bytes %zu, file mapping %u", seed, size, mapping);
      HANDLE file =
          mapping ? CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, size, nullptr) : nullptr;
      void *address = mapping ? MapViewOfFile(file, FILE_MAP_ALL_ACCESS, 0, 0, size)
                              : VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
      if (!expect(address != nullptr, "allocation failed"))
        return verdict();
      std::vector<BYTE> expected(size);
      for (auto &value : expected)
        value = BYTE(random());
      memcpy(address, expected.data(), size);
      ComPtr<ID3D12Heap> heap;
      HRESULT hr = mapping ? device->OpenExistingHeapFromFileMapping(file, IID_PPV_ARGS(&heap))
                           : device->OpenExistingHeapFromAddress(address, IID_PPV_ARGS(&heap));
      if (expect(hr == S_OK && heap, "heap import failed: %08lx", hr)) {
        auto desc = heap->GetDesc();
        expect(desc.SizeInBytes == size && (desc.Flags & D3D12_HEAP_FLAG_SHARED) &&
                   (desc.Flags & D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER),
               "imported heap description changed");
        ComPtr<ID3D12Heap> interior;
        expect(device->OpenExistingHeapFromAddress((BYTE *)address + system.dwPageSize, IID_PPV_ARGS(&interior)) ==
                       E_INVALIDARG &&
                   !interior,
               "interior allocation accepted");
        for (UINT64 offset = 0; offset < size; offset += D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT) {
          step("seed %u, imported bytes %zu, file mapping %u, placement %llu", seed, size, mapping, offset);
          const UINT64 bytes = size - offset;
          D3D12_RESOURCE_DESC resource_desc{D3D12_RESOURCE_DIMENSION_BUFFER,
                                            0,
                                            bytes,
                                            1,
                                            1,
                                            1,
                                            DXGI_FORMAT_UNKNOWN,
                                            {1, 0},
                                            D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                            D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER};
          ComPtr<ID3D12Resource> resource;
          CHECK(device->CreatePlacedResource(heap.Get(), offset, &resource_desc, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                             nullptr, IID_PPV_ARGS(&resource)));
          void *mapped;
          CHECK(resource->Map(0, nullptr, &mapped));
          if (!mapping)
            expect(mapped == (BYTE *)address + offset, "mapping does not alias the allocation at offset %llu", offset);
          expect(!memcmp(mapped, expected.data() + offset, bytes), "mapping does not contain the imported bytes");
          resource->Unmap(0, nullptr);
          auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
          if (!expect(bool(readback), "readback buffer not created"))
            return verdict();
          ComPtr<ID3D12CommandAllocator> allocator;
          ComPtr<ID3D12GraphicsCommandList> list;
          CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
          CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                          IID_PPV_ARGS(&list)));
          list->CopyBufferRegion(readback.Get(), 0, resource.Get(), 0, bytes);
          CHECK(submit(device.Get(), queue.Get(), list.Get()));
          CHECK(readback->Map(0, nullptr, &mapped));
          expect(!memcmp(mapped, expected.data() + offset, bytes), "GPU did not read the imported bytes");
          readback->Unmap(0, nullptr);
          auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
          if (!expect(bool(upload), "upload buffer not created"))
            return verdict();
          CHECK(upload->Map(0, nullptr, &mapped));
          for (SIZE_T i = 0; i < bytes; i++) {
            expected[offset + i] = BYTE(~expected[offset + i]);
            ((BYTE *)mapped)[i] = expected[offset + i];
          }
          upload->Unmap(0, nullptr);
          CHECK(allocator->Reset());
          CHECK(list->Reset(allocator.Get(), nullptr));
          transition(list.Get(), resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
          list->CopyBufferRegion(resource.Get(), 0, upload.Get(), 0, bytes);
          CHECK(submit(device.Get(), queue.Get(), list.Get()));
          expect(!memcmp(address, expected.data(), size), "GPU writes did not reach the application's allocation");
        }
        heap.Reset();
      }
      if (mapping) {
        UnmapViewOfFile(address);
        CloseHandle(file);
      } else
        VirtualFree(address, 0, MEM_RELEASE);
    }
    for (bool placed : {false, true}) {
      if (!exact_write_watch)
        break;
      step("seed %u, write watch bytes %zu, placed %u", seed, size, placed);
      D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_UPLOAD};
      D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                               D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
      ComPtr<ID3D12Resource> resource;
      ComPtr<ID3D12Heap> heap;
      if (placed) {
        D3D12_HEAP_DESC heap_desc{size, props, 0,
                                  D3D12_HEAP_FLAG_ALLOW_WRITE_WATCH | D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS};
        CHECK(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap)));
        CHECK(device->CreatePlacedResource(heap.Get(), 0, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                           IID_PPV_ARGS(&resource)));
      } else
        CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_ALLOW_WRITE_WATCH, &desc,
                                              D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&resource)));
      BYTE *mapped;
      CHECK(resource->Map(0, nullptr, (void **)&mapped));
      if (expect(!ResetWriteWatch(mapped, size), "ResetWriteWatch failed: %lu", GetLastError())) {
        std::vector<void *> dirty(size / system.dwPageSize);
        for (UINT pass = 0; pass < 2; pass++) {
          for (SIZE_T i = pass; i < dirty.size(); i += 2)
            mapped[i * system.dwPageSize] = BYTE(random());
          ULONG_PTR count = dirty.size();
          DWORD granularity;
          if (!expect(!GetWriteWatch(WRITE_WATCH_FLAG_RESET, mapped, size, dirty.data(), &count, &granularity),
                      "GetWriteWatch failed"))
            break;
          if (!expect(count <= dirty.size(), "dirty page count exceeds the output array"))
            break;
          expect(granularity == system.dwPageSize && count == (dirty.size() + 1 - pass) / 2,
                 "wrong dirty page count or size");
          for (SIZE_T i = 0; i < count; i++)
            expect(dirty[i] == mapped + (2 * i + pass) * system.dwPageSize, "dirty page %zu changed", i);
          count = dirty.size();
          expect(!GetWriteWatch(0, mapped, size, dirty.data(), &count, &granularity) && count == 0,
                 "write watch did not reset");
        }
      }
      resource->Unmap(0, nullptr);
    }
  }
  return verdict();
}
