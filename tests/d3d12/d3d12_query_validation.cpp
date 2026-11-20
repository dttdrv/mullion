// contract: invalid query resolve arguments fail recording, before invalid work can reach the GPU.
// ResolveQueryData: "The core runtime will validate" index/count ranges, eight-byte destination alignment,
// buffer dimension/size, and query compatibility with the heap and list. Close returns "E_INVALIDARG if an
// invalid argument was passed to the command list API during recording" (Microsoft Learn).
// the heap's queries are completed first; only valid setup is submitted. CASE=n selects one recording witness.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-resolvequerydata
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-close
// Queries lists heap and command-list compatibility; result widths come from their public result structures.
// https://learn.microsoft.com/en-us/windows/win32/direct3d12/queries
#include "d3d12_test.hpp"
#include <limits>

int main(int argc, char **argv) {
  int selected = -1;
  for (int i = 2; i < argc; i++)
    if (!strncmp(argv[i], "CASE=", 5))
      selected = strtol(argv[i] + 5, nullptr, 0);
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  constexpr UINT queries = 3;
  constexpr UINT64 result_size = sizeof(UINT64), buffer_bytes = 4 * result_size;
  auto result = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, buffer_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(result != nullptr, "query destination"))
    return verdict();
  ComPtr<ID3D12QueryHeap> heap;
  D3D12_QUERY_HEAP_DESC hd{D3D12_QUERY_HEAP_TYPE_OCCLUSION, queries};
  CHECK(device->CreateQueryHeap(&hd, IID_PPV_ARGS(&heap)));
  {
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    D3D12_COMMAND_QUEUE_DESC q{D3D12_COMMAND_LIST_TYPE_DIRECT};
    CHECK(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
    CHECK(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&allocator)));
    CHECK(device->CreateCommandList(0, q.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
    for (UINT i = 0; i < queries; i++) {
      list->BeginQuery(heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, i);
      list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, i);
    }
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
  }
  struct Case {
    const char *name;
    UINT first, count;
    UINT64 offset;
    D3D12_QUERY_TYPE query = D3D12_QUERY_TYPE_OCCLUSION;
    D3D12_COMMAND_LIST_TYPE list = D3D12_COMMAND_LIST_TYPE_DIRECT;
    bool texture = false, valid = false, missing_heap = false, missing_buffer = false;
  } cases[] = {
      {"first query", 0, 1, 0, D3D12_QUERY_TYPE_OCCLUSION, D3D12_COMMAND_LIST_TYPE_DIRECT, false, true},
      {"last query", queries - 1, 1, buffer_bytes - result_size, D3D12_QUERY_TYPE_OCCLUSION,
       D3D12_COMMAND_LIST_TYPE_DIRECT, false, true},
      {"all queries", 0, queries, result_size, D3D12_QUERY_TYPE_OCCLUSION,
       D3D12_COMMAND_LIST_TYPE_DIRECT, false, true},
      {"empty range", 0, 0, 0, D3D12_QUERY_TYPE_OCCLUSION, D3D12_COMMAND_LIST_TYPE_DIRECT, false, true},
      {"empty range at heap and buffer ends", queries, 0, buffer_bytes, D3D12_QUERY_TYPE_OCCLUSION,
       D3D12_COMMAND_LIST_TYPE_DIRECT, false, true},
      {"empty range beyond heap", queries + 1, 0, 0},
      {"first beyond heap", queries, 1, 0},
      {"count beyond heap", 0, queries + 1, 0},
      {"range beyond heap", queries - 1, 2, 0},
      {"maximum first", std::numeric_limits<UINT>::max(), 1, 0},
      {"maximum count", 1, std::numeric_limits<UINT>::max(), 0},
      {"unaligned first byte", 0, 1, 1},
      {"unaligned before result", 0, 1, result_size - 1},
      {"unaligned after result", 0, 1, result_size + 1},
      {"first beyond destination", 0, 1, buffer_bytes},
      {"count beyond destination", 0, queries, 2 * result_size},
      {"maximum offset", 0, 1, std::numeric_limits<UINT64>::max()},
      {"aligned offset overflow", 0, 1, std::numeric_limits<UINT64>::max() - (result_size - 1)},
      {"timestamp in occlusion heap", 0, 1, 0, D3D12_QUERY_TYPE_TIMESTAMP},
      {"statistics in occlusion heap", 0, 1, 0, D3D12_QUERY_TYPE_PIPELINE_STATISTICS},
      {"SO statistics in occlusion heap", 0, 1, 0, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0},
      {"occlusion on compute", 0, 1, 0, D3D12_QUERY_TYPE_OCCLUSION, D3D12_COMMAND_LIST_TYPE_COMPUTE},
      {"occlusion on copy", 0, 1, 0, D3D12_QUERY_TYPE_OCCLUSION, D3D12_COMMAND_LIST_TYPE_COPY},
      {"texture destination", 0, 1, 0, D3D12_QUERY_TYPE_OCCLUSION, D3D12_COMMAND_LIST_TYPE_DIRECT, true},
      {"binary occlusion", 0, 1, 0, D3D12_QUERY_TYPE_BINARY_OCCLUSION, D3D12_COMMAND_LIST_TYPE_DIRECT, false, true},
      {"mesh statistics in occlusion heap", 0, 1, 0, D3D12_QUERY_TYPE_PIPELINE_STATISTICS1},
      {"SO stream 1 in occlusion heap", 0, 1, 0, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM1},
      {"SO stream 2 in occlusion heap", 0, 1, 0, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM2},
      {"SO stream 3 in occlusion heap", 0, 1, 0, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM3},
      {"invalid query type", 0, 1, 0, D3D12_QUERY_TYPE(std::numeric_limits<UINT>::max())},
      {"missing heap", 0, 1, 0, D3D12_QUERY_TYPE_OCCLUSION, D3D12_COMMAND_LIST_TYPE_DIRECT, false, false, true},
      {"missing destination", 0, 1, 0, D3D12_QUERY_TYPE_OCCLUSION, D3D12_COMMAND_LIST_TYPE_DIRECT,
       false, false, false, true},
  };
  ComPtr<ID3D12Resource> texture;
  D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
  // a byte-range check alone must not reject the texture before its dimension is checked.
  constexpr UINT64 texture_texels = buffer_bytes;
  D3D12_RESOURCE_DESC td{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, texture_texels, 4, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0}};
  CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST,
                                        nullptr, IID_PPV_ARGS(&texture)));
  for (UINT i = 0; i < std::size(cases); i++) {
    if (selected >= 0 && UINT(selected) != i)
      continue;
    const auto &c = cases[i];
    step("CASE=%u: %s", i, c.name);
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    CHECK(device->CreateCommandAllocator(c.list, IID_PPV_ARGS(&allocator)));
    CHECK(device->CreateCommandList(0, c.list, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
    list->ResolveQueryData(c.missing_heap ? nullptr : heap.Get(), c.query, c.first, c.count,
                           c.missing_buffer ? nullptr : c.texture ? texture.Get() : result.Get(), c.offset);
    HRESULT hr = list->Close();
    expect(hr == (c.valid ? S_OK : E_INVALIDARG), "Close returned %08lx, want %08lx", hr,
           c.valid ? S_OK : E_INVALIDARG);
  }
  if (selected >= 0) {
    expect(UINT(selected) < std::size(cases), "CASE=%d does not exist", selected);
    return verdict();
  }
  struct Query {
    D3D12_QUERY_HEAP_TYPE heap;
    D3D12_QUERY_TYPE type;
    UINT64 bytes;
  } types[] = {
      {D3D12_QUERY_HEAP_TYPE_OCCLUSION, D3D12_QUERY_TYPE_OCCLUSION, sizeof(UINT64)},
      {D3D12_QUERY_HEAP_TYPE_OCCLUSION, D3D12_QUERY_TYPE_BINARY_OCCLUSION, sizeof(UINT64)},
      {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, D3D12_QUERY_TYPE_TIMESTAMP, sizeof(UINT64)},
      {D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP, D3D12_QUERY_TYPE_TIMESTAMP, sizeof(UINT64)},
      {D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, D3D12_QUERY_TYPE_PIPELINE_STATISTICS,
       sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS)},
      {D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS1, D3D12_QUERY_TYPE_PIPELINE_STATISTICS1,
       sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS1)},
      {D3D12_QUERY_HEAP_TYPE_SO_STATISTICS, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0,
       sizeof(D3D12_QUERY_DATA_SO_STATISTICS)},
      {D3D12_QUERY_HEAP_TYPE_SO_STATISTICS, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM1,
       sizeof(D3D12_QUERY_DATA_SO_STATISTICS)},
      {D3D12_QUERY_HEAP_TYPE_SO_STATISTICS, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM2,
       sizeof(D3D12_QUERY_DATA_SO_STATISTICS)},
      {D3D12_QUERY_HEAP_TYPE_SO_STATISTICS, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM3,
       sizeof(D3D12_QUERY_DATA_SO_STATISTICS)},
  };
  D3D12_FEATURE_DATA_D3D12_OPTIONS3 options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &options, sizeof(options)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS9 mesh_options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9, &mesh_options, sizeof(mesh_options)));
  for (auto &query : types) {
    if (query.heap == D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS1 && !mesh_options.MeshShaderPipelineStatsSupported) {
      printf("unsupported: mesh pipeline statistics query cases\n");
      continue;
    }
    if (query.heap == D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP && !options.CopyQueueTimestampQueriesSupported) {
      printf("unsupported: copy queue timestamp query cases\n");
      continue;
    }
    D3D12_QUERY_HEAP_DESC desc{query.heap, queries};
    ComPtr<ID3D12QueryHeap> typed_heap;
    CHECK(device->CreateQueryHeap(&desc, IID_PPV_ARGS(&typed_heap)));
    auto destination = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 2 * query.bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!expect(bool(destination), "typed query destination"))
      return verdict();
    const auto producer = query.heap == D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP ? D3D12_COMMAND_LIST_TYPE_COPY
                                                                               : D3D12_COMMAND_LIST_TYPE_DIRECT;
    {
      ComPtr<ID3D12CommandQueue> queue;
      ComPtr<ID3D12CommandAllocator> allocator;
      ComPtr<ID3D12GraphicsCommandList> list;
      D3D12_COMMAND_QUEUE_DESC q{producer};
      CHECK(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
      CHECK(device->CreateCommandAllocator(producer, IID_PPV_ARGS(&allocator)));
      CHECK(device->CreateCommandList(0, producer, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
      for (UINT i = 0; i < queries; i++) {
        if (query.type != D3D12_QUERY_TYPE_TIMESTAMP)
          list->BeginQuery(typed_heap.Get(), query.type, i);
        list->EndQuery(typed_heap.Get(), query.type, i);
      }
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
    }
    for (auto kind : {D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COMPUTE,
                      D3D12_COMMAND_LIST_TYPE_COPY, D3D12_COMMAND_LIST_TYPE_BUNDLE})
      for (UINT count : {0u, 1u, 2u, queries})
        for (UINT64 offset : {UINT64(0), query.bytes, 2 * query.bytes}) {
          step("heap %u query %u list %u count %u offset %llu", query.heap, query.type, kind, count, offset);
          ComPtr<ID3D12CommandAllocator> allocator;
          ComPtr<ID3D12GraphicsCommandList> list;
          CHECK(device->CreateCommandAllocator(kind, IID_PPV_ARGS(&allocator)));
          CHECK(device->CreateCommandList(0, kind, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
          list->ResolveQueryData(typed_heap.Get(), query.type, 0, count, destination.Get(), offset);
          const bool compatible = query.heap == D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP
                                      ? kind == D3D12_COMMAND_LIST_TYPE_COPY
                                      : kind == D3D12_COMMAND_LIST_TYPE_DIRECT ||
                                            (query.type == D3D12_QUERY_TYPE_TIMESTAMP &&
                                             kind == D3D12_COMMAND_LIST_TYPE_COMPUTE);
          const bool valid = compatible && UINT64(count) * query.bytes + offset <= 2 * query.bytes;
          expect(list->Close() == (valid ? S_OK : E_INVALIDARG), "typed resolve returned the wrong Close result");
        }
  }
  return verdict();
}
