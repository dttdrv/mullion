// contract: a successful heap contains the requested number of whole query results. Count "Specifies the number
// of queries the heap should contain." (D3D12_QUERY_HEAP_DESC, Microsoft Learn); every pipeline statistics member
// is UINT64 (D3D11.3 20.4.7). An array beyond the process address space cannot be allocated as a smaller array.
// a range that holds one dispatch of a group of THREADS threads counts that many compute invocations and nothing
// else ("CSInvocations must honor Compute Shader invocations from both Dispatch() and DispatchIndirect() APIs",
// 20.4.7): resolving the first and last queries must write that inside the destination guards, which a heap shorter
// than asked loses at its end. counts straddle the 32-bit byte-length boundary even in a 64-bit process.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_query_heap_desc
#include "d3d12_test.hpp"
#include <algorithm>
#include <cstddef>
#include <limits>
#include <vector>

static const char hlsl[] = R"hlsl(
[numthreads(THREADS, 1, 1)]
void cs() {}
)hlsl";

int
main(int argc, char **argv) {
  const UINT threads = 4;
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  auto cs = compiler.compile(hlsl, "cs", "cs", {"THREADS=" + std::to_string(threads)});
  if (!expect(!cs.empty(), "the compute shader does not compile"))
    return verdict();
  auto rs = root_signature(device.Get(), D3D12_ROOT_SIGNATURE_DESC{});
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute{rs.Get(), bytecode(cs)};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&pso)));
  for (auto type : {D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS1}) {
    const UINT stride = type == D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS
                            ? sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS)
                            : sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS1);
    const UINT first_overflow = std::numeric_limits<UINT>::max() / stride + 1;
    const auto query_type = type == D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS
                                ? D3D12_QUERY_TYPE_PIPELINE_STATISTICS
                                : D3D12_QUERY_TYPE_PIPELINE_STATISTICS1;
    // one dispatch's result: its threads in CSInvocations, zero in every other member
    std::vector<BYTE> wanted(stride);
    const UINT64 invocations = threads;
    memcpy(wanted.data() + (type == D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS
                                ? offsetof(D3D12_QUERY_DATA_PIPELINE_STATISTICS, CSInvocations)
                                : offsetof(D3D12_QUERY_DATA_PIPELINE_STATISTICS1, CSInvocations)),
           &invocations, sizeof(invocations));
    for (UINT count : {1u, first_overflow - 1, first_overflow, first_overflow + 1}) {
      step("heap type %u, count %u, result bytes %llu", type, count, UINT64(count) * stride);
      D3D12_QUERY_HEAP_DESC desc{type, count};
      ComPtr<ID3D12QueryHeap> heap;
      const HRESULT hr = device->CreateQueryHeap(&desc, IID_PPV_ARGS(&heap));
      if (hr == E_OUTOFMEMORY && count != 1) {
        expect(!heap, "failed allocation returned a heap");
        continue;
      }
      if (!expect(hr == S_OK && heap, "heap type %u count %u: %#lx", type, count, hr))
        return verdict();
      auto results = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 4 * stride, D3D12_RESOURCE_STATE_COPY_DEST);
      if (!expect(bool(results), "readback buffer could not be made"))
        return verdict();
      CHECK(forget(results.Get()));
      ComPtr<ID3D12CommandAllocator> allocator;
      ComPtr<ID3D12GraphicsCommandList> list;
      CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
      CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
      list->SetComputeRootSignature(rs.Get());
      UINT destination = 1;
      for (UINT index : {0u, count - 1}) {
        list->BeginQuery(heap.Get(), query_type, index);
        list->Dispatch(1, 1, 1);
        list->EndQuery(heap.Get(), query_type, index);
        list->ResolveQueryData(heap.Get(), query_type, index, 1, results.Get(), UINT64(destination++) * stride);
      }
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      const BYTE *bytes;
      CHECK(results->Map(0, nullptr, (void **)&bytes));
      expect(std::equal(wanted.begin(), wanted.end(), bytes + stride), "the first query lost its dispatch");
      expect(std::equal(wanted.begin(), wanted.end(), bytes + 2 * stride), "the last query lost its dispatch");
      expect(std::all_of(bytes, bytes + stride, [](BYTE value) { return value == 0xff; }) &&
                 std::all_of(bytes + 3 * stride, bytes + 4 * stride, [](BYTE value) { return value == 0xff; }),
             "resolve overwrote a guard");
      results->Unmap(0, nullptr);
    }
  }
  return verdict();
}
