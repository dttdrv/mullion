// contract: a custom heap whose CPU pages are write-back is what GetCustomHeapProperties gives for a readback heap, and
// applications make every heap that way (bgfx does on Windows). queries resolved into a buffer of it read as they do
// from a readback heap: timestamps that do not go back in time, and pipeline statistics of zero over bytes that held
// something else. resolving into it costs what resolving into a readback heap costs: a list that ends and resolves
// 2048 timestamps one by one runs, the second time, within 4 times the same list into a readback buffer plus 20 ms.
#include "d3d12_test.hpp"

int
main(int argc, char **argv) {
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  const UINT stamps = 2048, statistics = 4;
  const UINT64 bytes = stamps * sizeof(UINT64);
  D3D12_HEAP_PROPERTIES custom;
  device->GetCustomHeapProperties(&custom, 0, D3D12_HEAP_TYPE_READBACK);
  if (custom.Type != D3D12_HEAP_TYPE_CUSTOM || custom.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_WRITE_BACK) {
    printf("failed: GetCustomHeapProperties(READBACK) is type %d, CPU pages %d\n", custom.Type, custom.CPUPageProperty);
    return 1;
  }
  D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_BUFFER, 0, bytes, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                           D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
  ComPtr<ID3D12Resource> custom_buffer;
  CHECK(device->CreateCommittedResource(
      &custom, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&custom_buffer)
  ));
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);

  ComPtr<ID3D12QueryHeap> timestamps, pipeline;
  D3D12_QUERY_HEAP_DESC timestamp_desc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, stamps},
      pipeline_desc{D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, statistics};
  CHECK(device->CreateQueryHeap(&timestamp_desc, IID_PPV_ARGS(&timestamps)));
  CHECK(device->CreateQueryHeap(&pipeline_desc, IID_PPV_ARGS(&pipeline)));

  unsigned failures = 0;
  LARGE_INTEGER frequency;
  QueryPerformanceFrequency(&frequency);
  // ends and resolves every timestamp into `dst`, twice; `ms` is the second run's time
  auto resolve_stamps = [&](ID3D12Resource *dst, double &ms) -> HRESULT {
    HRESULT hr;
    for (int run = 0; run < 2; run++) {
      if (FAILED(hr = forget(dst)) || FAILED(hr = allocator->Reset()) || FAILED(hr = list->Reset(allocator.Get(), nullptr)))
        return hr;
      for (UINT i = 0; i < stamps; i++) {
        list->EndQuery(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i);
        list->ResolveQueryData(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i, 1, dst, i * sizeof(UINT64));
      }
      LARGE_INTEGER start, end;
      QueryPerformanceCounter(&start);
      if (FAILED(hr = submit(device.Get(), queue.Get(), list.Get())))
        return hr;
      QueryPerformanceCounter(&end);
      ms = double(end.QuadPart - start.QuadPart) * 1000 / frequency.QuadPart;
    }
    return S_OK;
  };
  double readback_ms = 0, custom_ms = 0;
  CHECK(resolve_stamps(readback.Get(), readback_ms));
  CHECK(resolve_stamps(custom_buffer.Get(), custom_ms));

  UINT64 *values;
  CHECK(custom_buffer->Map(0, nullptr, (void **)&values));
  for (UINT i = 0; i < stamps; i++)
    if ((values[i] == ~0ull || (i && values[i] < values[i - 1])) && failures++ < 12)
      printf("timestamp %u in the custom heap: %llu after %llu\n", i, (unsigned long long)values[i],
             (unsigned long long)(i ? values[i - 1] : 0));
  custom_buffer->Unmap(0, nullptr);
  const double limit_ms = 4 * readback_ms + 20;
  if (custom_ms > limit_ms && failures++ < 12)
    printf("2048 resolves into the custom heap took %.1f ms, more than %.1f ms (%.1f into a readback heap)\n",
           custom_ms, limit_ms, readback_ms);

  CHECK(forget(custom_buffer.Get()));
  CHECK(allocator->Reset());
  CHECK(list->Reset(allocator.Get(), nullptr));
  for (UINT i = 0; i < statistics; i++) {
    list->BeginQuery(pipeline.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, i);
    list->EndQuery(pipeline.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, i);
  }
  list->ResolveQueryData(pipeline.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, statistics, custom_buffer.Get(), 0);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  const UINT64 *counts;
  CHECK(custom_buffer->Map(0, nullptr, (void **)&counts));
  const size_t words = statistics * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS) / sizeof(UINT64);
  for (size_t i = 0; i < words; i++)
    if (counts[i] && failures++ < 12)
      printf("pipeline statistics in the custom heap: word %zu is %llu, want 0\n", i, (unsigned long long)counts[i]);
  custom_buffer->Unmap(0, nullptr);

  printf("%s: %u wrong results; 2048 resolves in %.1f ms into a custom heap, %.1f ms into a readback heap\n",
         failures ? "failed" : "passed", failures, custom_ms, readback_ms);
  return failures ? 1 : 0;
}
