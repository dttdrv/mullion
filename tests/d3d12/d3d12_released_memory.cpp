// contract: a released object gives back the GPU memory it took. DXGI's CurrentUsage is the "current video memory
// usage, in bytes" (DXGI_QUERY_VIDEO_MEMORY_INFO); a residency set holds what it lists and gives an allocation up at
// its next commit, which a submission makes, so usage is read after one. each kind of object the device keeps
// resident is made and released a first time, which leaves what the device keeps for good (stand-ins, pools), then
// three times more: each of those must leave usage where it was, or the kind keeps memory of every one released.
// the kinds: committed buffers and textures, a heap with a buffer and a texture placed on it, a reserved buffer with
// a tile mapped and unmapped, shader-visible descriptor heaps, each kind of query heap, a local root signature with a static
// sampler, and a queue of its own that mapped and unmapped a tile
#include "d3d12_test.hpp"
#include <dxgi1_4.h>
#include <functional>

int
main() {
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<IDXGIFactory4> factory;
  ComPtr<IDXGIAdapter3> adapter;
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  CHECK(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5)));
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  ComPtr<ID3D12CommandQueue> queue;
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  ComPtr<ID3D12GraphicsCommandList> empty;
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&empty)));
  CHECK(empty->Close());
  auto usage = [&](ID3D12CommandQueue *on) -> UINT64 {
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (FAILED(execute(device.Get(), on, empty.Get())) ||
        FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
      return ~0ull;
    return info.CurrentUsage;
  };

  const D3D12_HEAP_PROPERTIES gpu{D3D12_HEAP_TYPE_DEFAULT};
  const D3D12_RESOURCE_DESC buffer_desc{D3D12_RESOURCE_DIMENSION_BUFFER, 0, D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES, 1, 1, 1,
                                        DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR},
      texture_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 64, 64, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}};
  // a reserved buffer of one tile, mapped onto a heap of its own on `on` and unmapped, which then finishes its work
  auto mapped = [&](ID3D12CommandQueue *on) {
    D3D12_HEAP_DESC heap_desc{D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES, gpu, 0, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS};
    ComPtr<ID3D12Heap> heap;
    ComPtr<ID3D12Resource> reserved;
    if (!expect(SUCCEEDED(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap))) &&
                    SUCCEEDED(device->CreateReservedResource(&buffer_desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                             IID_PPV_ARGS(&reserved))),
                "no heap or reserved buffer"))
      return;
    D3D12_TILED_RESOURCE_COORDINATE at{};
    D3D12_TILE_REGION_SIZE one{1};
    on->UpdateTileMappings(reserved.Get(), 1, &at, &one, heap.Get(), 1, nullptr, nullptr, nullptr,
                           D3D12_TILE_MAPPING_FLAG_NONE);
    // and unmapped again, which Mullion settles with the device's spare tile
    D3D12_TILE_RANGE_FLAGS null = D3D12_TILE_RANGE_FLAG_NULL;
    on->UpdateTileMappings(reserved.Get(), 1, &at, &one, nullptr, 1, &null, nullptr, nullptr,
                           D3D12_TILE_MAPPING_FLAG_NONE);
    expect(usage(on) != ~0ull, "the mapping's queue did not finish");
  };
  struct Kind {
    const char *name;
    bool supported;
    std::function<bool()> make;
  };
  const Kind kinds[] = {
      {"a committed buffer", true,
       [&] { return bool(buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, 1 << 20, D3D12_RESOURCE_STATE_COMMON)); }},
      {"a committed texture", true,
       [&] {
         ComPtr<ID3D12Resource> texture;
         return SUCCEEDED(device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &texture_desc,
                                                          D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&texture)));
       }},
      {"a heap with a buffer and a texture placed on it", options.ResourceHeapTier >= D3D12_RESOURCE_HEAP_TIER_2,
       [&] {
         D3D12_HEAP_DESC heap_desc{1 << 20, gpu, 0, D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES};
         ComPtr<ID3D12Heap> heap;
         ComPtr<ID3D12Resource> placed_buffer, placed_texture;
         return SUCCEEDED(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap))) &&
                SUCCEEDED(device->CreatePlacedResource(heap.Get(), 0, &buffer_desc, D3D12_RESOURCE_STATE_COMMON,
                                                       nullptr, IID_PPV_ARGS(&placed_buffer))) &&
                SUCCEEDED(device->CreatePlacedResource(heap.Get(), 1 << 19, &texture_desc, D3D12_RESOURCE_STATE_COMMON,
                                                       nullptr, IID_PPV_ARGS(&placed_texture)));
       }},
      {"a reserved buffer with a tile mapped and unmapped", options.TiledResourcesTier != D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED,
       [&] { return mapped(queue.Get()), true; }},
      {"a queue of its own that mapped and unmapped a tile", options.TiledResourcesTier != D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED,
       [&] {
         ComPtr<ID3D12CommandQueue> own;
         if (FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&own))))
           return false;
         return mapped(own.Get()), true;
       }},
      {"shader-visible descriptor heaps", true,
       [&] {
         D3D12_DESCRIPTOR_HEAP_DESC views{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4096, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE},
             samplers{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 256, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
         ComPtr<ID3D12DescriptorHeap> view_heap, sampler_heap;
         return SUCCEEDED(device->CreateDescriptorHeap(&views, IID_PPV_ARGS(&view_heap))) &&
                SUCCEEDED(device->CreateDescriptorHeap(&samplers, IID_PPV_ARGS(&sampler_heap)));
       }},
      {"query heaps of each kind", true,
       [&] {
         for (auto type : {D3D12_QUERY_HEAP_TYPE_OCCLUSION, D3D12_QUERY_HEAP_TYPE_TIMESTAMP,
                           D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, D3D12_QUERY_HEAP_TYPE_SO_STATISTICS}) {
           D3D12_QUERY_HEAP_DESC desc{type, 1024};
           ComPtr<ID3D12QueryHeap> heap;
           if (!expect(SUCCEEDED(device->CreateQueryHeap(&desc, IID_PPV_ARGS(&heap))), "no query heap of type %u", type))
             return false;
         }
         return true;
       }},
      {"a local root signature with a static sampler", options5.RaytracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED,
       [&] {
         D3D12_STATIC_SAMPLER_DESC sampler{D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                                           D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, 0, 1,
                                           D3D12_COMPARISON_FUNC_NEVER, D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK, 0,
                                           D3D12_FLOAT32_MAX, 0, 0, D3D12_SHADER_VISIBILITY_ALL};
         return bool(root_signature(device.Get(), {0, nullptr, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE}));
       }},
  };
  for (auto &kind : kinds) {
    if (!kind.supported) {
      step("%s: not supported by the device", kind.name);
      continue;
    }
    step("%s, made and released once", kind.name);
    if (!expect(kind.make(), "%s could not be made", kind.name))
      continue;
    UINT64 before = usage(queue.Get());
    for (int round = 1; round <= 3; round++) {
      step("%s, made and released again (%d)", kind.name, round);
      kind.make();
      UINT64 after = usage(queue.Get());
      expect(after == before, "%s keeps %lld bytes after it is released", kind.name, (long long)(after - before));
    }
  }
  return verdict();
}
