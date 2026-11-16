// contract: ClearUnorderedAccessViewUint "Sets all the elements in a unordered-access view (UAV) to the specified
// integer values" (Microsoft Learn), and a buffer view's FirstElement is 64 bits wide, "The zero-based index of the
// first element to be accessed" (D3D12_BUFFER_UAV): a view that starts 2 GiB or more into a buffer is cleared there.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-clearunorderedaccessviewuint
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_buffer_uav
// a buffer a little over 2 GiB has a raw view and a structured view of a few elements that start just past 2 GiB,
// where a byte offset no longer fits a signed 32-bit number. the words around each view hold a sentinel, the clear's
// value has four different bytes, and the words before, in and after the view are read back; the whole view is
// cleared, then one rectangle of it. the first words of the buffer are read too: a clear at the offset's low bits
// lands there. a device that cannot make the buffer skips.
#include "d3d12_test.hpp"

int
main(int, char **) {
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  constexpr UINT64 start = UINT64(1) << 31, window = 256, size = start + 2 * window;
  constexpr UINT words = window / sizeof(UINT), guard = 8, stride = 12, sentinel = 0xa5c3e17bu, value = 0x04030201u;
  auto target = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, size, D3D12_RESOURCE_STATE_COPY_DEST,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  if (!target) {
    printf("skipped: the device made no buffer of %llu bytes\n", size);
    return 77;
  }
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, window, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 2 * window, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(upload && readback, "copy buffers were not created"))
    return verdict();
  UINT *mapped;
  CHECK(upload->Map(0, nullptr, (void **)&mapped));
  std::fill_n(mapped, words, sentinel);
  upload->Unmap(0, nullptr);

  ComPtr<ID3D12DescriptorHeap> cpu, gpu;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&cpu)));
  heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&gpu)));
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  // the view's elements start `guard` words past 2 GiB, a whole number of elements into the buffer
  const struct {
    const char *name;
    DXGI_FORMAT format;
    UINT stride, elements;
    D3D12_BUFFER_UAV_FLAGS flags;
  } views[] = {
      {"raw", DXGI_FORMAT_R32_TYPELESS, sizeof(UINT), 16, D3D12_BUFFER_UAV_FLAG_RAW},
      {"structured", DXGI_FORMAT_UNKNOWN, stride, 4, D3D12_BUFFER_UAV_FLAG_NONE}
  };
  for (auto &view : views)
    for (bool rectangle : {false, true}) {
      step("%s view, %s", view.name, rectangle ? "one rectangle" : "the whole view");
      // the first element at or past 2 GiB + guard words
      const UINT64 first = (start + guard * sizeof(UINT) + view.stride - 1) / view.stride;
      const UINT64 first_word = first * view.stride / sizeof(UINT) - start / sizeof(UINT);
      const UINT per_element = view.stride / sizeof(UINT);
      D3D12_UNORDERED_ACCESS_VIEW_DESC desc{view.format, D3D12_UAV_DIMENSION_BUFFER};
      desc.Buffer = {first, view.elements, view.format == DXGI_FORMAT_UNKNOWN ? view.stride : 0, 0, view.flags};
      device->CreateUnorderedAccessView(target.Get(), nullptr, &desc, cpu->GetCPUDescriptorHandleForHeapStart());
      device->CopyDescriptorsSimple(
          1, gpu->GetCPUDescriptorHandleForHeapStart(), cpu->GetCPUDescriptorHandleForHeapStart(), heap_desc.Type
      );
      const D3D12_RECT rect{1, 0, LONG(view.elements) - 1, 1};
      CHECK(forget(readback.Get()));
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), nullptr));
      list->CopyBufferRegion(target.Get(), 0, upload.Get(), 0, window);
      list->CopyBufferRegion(target.Get(), start, upload.Get(), 0, window);
      transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      ID3D12DescriptorHeap *heaps[] = {gpu.Get()};
      list->SetDescriptorHeaps(1, heaps);
      const UINT values[4] = {value, value, value, value};
      list->ClearUnorderedAccessViewUint(
          gpu->GetGPUDescriptorHandleForHeapStart(), cpu->GetCPUDescriptorHandleForHeapStart(), target.Get(), values,
          rectangle, rectangle ? &rect : nullptr
      );
      transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyBufferRegion(readback.Get(), 0, target.Get(), 0, window);
      list->CopyBufferRegion(readback.Get(), window, target.Get(), start, window);
      transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      const UINT *got;
      CHECK(readback->Map(0, nullptr, (void **)&got));
      const UINT64 from = first_word + (rectangle ? rect.left : 0) * per_element,
                   to = first_word + (rectangle ? rect.right : view.elements) * per_element;
      for (UINT word = 0; word < words; word++)
        expect(got[word] == sentinel, "word %u of the buffer is %08x, expected the sentinel", word, got[word]);
      for (UINT word = 0; word < words; word++) {
        UINT want = word >= from && word < to ? value : sentinel;
        expect(got[words + word] == want, "word %u past 2 GiB is %08x, expected %08x", word, got[words + word], want);
      }
      readback->Unmap(0, nullptr);
    }
  return verdict();
}
