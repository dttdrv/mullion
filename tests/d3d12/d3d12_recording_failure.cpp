// contract: Close returns "E_OUTOFMEMORY if the operating system ran out of memory during recording"; a failed
// list never records again: "Calling Reset will result in the same error being returned" (Microsoft Learn,
// ID3D12GraphicsCommandList::Close and ::Reset). earlier lists keep their own recordings, a failed list encodes
// nothing, and a fresh allocator can record again. fill the device through one immediate write per allocator,
// bounded by the smaller of physical memory and process address space in system allocation units.
// "All APIs on ID3D12GraphicsCommandList do not hold references to the objects passed into those APIs"
// (Creating and recording command lists and bundles). check COM counts and native backing separately: resource
// counts do not count internal texture views. once a submission completes, DXGI's CurrentUsage ("current video
// memory usage, in bytes", DXGI_QUERY_VIDEO_MEMORY_INFO) returns to its baseline after resources go: before then
// Metal may still hold them, as a residency set gives an allocation up at its next commit.
// a valid native resolve followed by an invalid write mode must leave no view held, at Close, Reset or Release.
// deferred post-build information also records on the queue: an exhausted private recording leaves the output
// unchanged twice, then the same query on a fresh allocator writes the same bytes as its successful control.
#include "d3d12_test.hpp"
#include <dxgi1_4.h>
#include <array>
#include <algorithm>

int
main() {
  ComPtr<ID3D12Device5> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandAllocator> allocator;
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  auto target = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, sizeof(UINT), D3D12_RESOURCE_STATE_COPY_DEST);
  auto result = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, sizeof(UINT64), D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(target && result, "buffers were not made"))
    return verdict();
  ComPtr<IDXGIFactory4> factory;
  ComPtr<IDXGIAdapter3> adapter;
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  CHECK(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)));
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> settling;
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&settling)));
  ComPtr<ID3D12GraphicsCommandList> settle;
  CHECK(device->CreateCommandList(0, queue_desc.Type, settling.Get(), nullptr, IID_PPV_ARGS(&settle)));
  CHECK(settle->Close());
  auto usage = [&](DXGI_QUERY_VIDEO_MEMORY_INFO &info) {
    HRESULT hr = execute(device.Get(), queue.Get(), settle.Get());
    return FAILED(hr) ? hr : adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info);
  };
  for (bool invalid : {false, true})
    for (bool reset : {false, true}) {
      step("native resolve references: invalid Close %u, allocator Reset %u", invalid, reset);
      CHECK(allocator->Reset());
      DXGI_QUERY_VIDEO_MEMORY_INFO before{}, after{};
      CHECK(usage(before));
      D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
      D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 8, 8, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
                               {4, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
      ComPtr<ID3D12Resource> source, destination;
      CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RESOLVE_SOURCE,
                                            nullptr, IID_PPV_ARGS(&source)));
      desc.SampleDesc.Count = 1;
      CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RESOLVE_DEST,
                                            nullptr, IID_PPV_ARGS(&destination)));
      ComPtr<ID3D12GraphicsCommandList2> list;
      CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                      IID_PPV_ARGS(&list)));
      list->ResolveSubresource(destination.Get(), 0, source.Get(), 0, desc.Format);
      if (invalid) {
        D3D12_WRITEBUFFERIMMEDIATE_PARAMETER param{target->GetGPUVirtualAddress(), 1};
        auto mode = D3D12_WRITEBUFFERIMMEDIATE_MODE(D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT + 1);
        list->WriteBufferImmediate(1, &param, &mode);
      }
      expect(list->Close() == (invalid ? E_INVALIDARG : S_OK), "Close returned the wrong result");
      for (auto resource : {source.Get(), destination.Get()}) {
        resource->AddRef();
        expect(resource->Release() == 1, "resource retained through its COM interface");
      }
      source.Reset();
      destination.Reset();
      if (invalid) {
        CHECK(usage(after));
        expect(after.CurrentUsage == before.CurrentUsage, "failed Close retains %llu bytes of texture backing",
               after.CurrentUsage - before.CurrentUsage);
        expect(list->Reset(allocator.Get(), nullptr) == E_INVALIDARG, "failed list Reset succeeded");
      }
      list.Reset();
      if (reset) {
        CHECK(allocator->Reset());
      } else {
        allocator.Reset();
      }
      CHECK(usage(after));
      expect(after.CurrentUsage == before.CurrentUsage, "allocator cleanup retains %llu bytes of texture backing",
             after.CurrentUsage - before.CurrentUsage);
      if (!allocator)
        CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    }

  D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options)));
  const bool raytracing = options.RaytracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
  ComPtr<ID3D12Resource> structure, scratch, information;
  ComPtr<ID3D12GraphicsCommandList4> later;
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC info{};
  D3D12_GPU_VIRTUAL_ADDRESS address = 0;
  if (raytracing) {
    step("build an empty bottom-level structure before arena exhaustion");
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO sizes{};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &sizes);
    structure = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, sizes.ResultDataMaxSizeInBytes,
                       D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    scratch = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, sizes.ScratchDataSizeInBytes,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    information = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT,
                         sizeof(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_TOOLS_VISUALIZATION_DESC),
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!expect(structure && scratch && information, "ray-tracing buffers were not made"))
      return verdict();
    CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&later)));
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{structure->GetGPUVirtualAddress(), inputs};
    build.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    later->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
    transition(later.Get(), information.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    std::array<D3D12_WRITEBUFFERIMMEDIATE_PARAMETER, sizeof(UINT64) / sizeof(UINT)> initial{};
    for (UINT i = 0; i < initial.size(); i++)
      initial[i] = {information->GetGPUVirtualAddress() + i * sizeof(UINT), 0};
    later->WriteBufferImmediate(initial.size(), initial.data(), nullptr);
    transition(later.Get(), information.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    CHECK(submit(device.Get(), queue.Get(), later.Get()));
    CHECK(allocator->Reset());
    CHECK(later->Reset(allocator.Get(), nullptr));
    info = {information->GetGPUVirtualAddress(),
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_TOOLS_VISUALIZATION};
    address = structure->GetGPUVirtualAddress();
    for (UINT i = 0; i < 2; i++)
      later->EmitRaytracingAccelerationStructurePostbuildInfo(&info, 1, &address);
    transition(later.Get(), information.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    later->CopyBufferRegion(result.Get(), 0, information.Get(), 0, sizeof(UINT64));
    transition(later.Get(), information.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    CHECK(later->Close());
  } else {
    printf("queue-private recording cases unavailable: ray tracing is not supported\n");
  }
  ComPtr<ID3D12GraphicsCommandList2> kept;
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&kept)));
  D3D12_WRITEBUFFERIMMEDIATE_PARAMETER param{target->GetGPUVirtualAddress(), 1};
  kept->WriteBufferImmediate(1, &param, nullptr);
  transition(kept.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  kept->CopyBufferRegion(result.Get(), 0, target.Get(), 0, sizeof(param.Value));
  transition(kept.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  CHECK(kept->Close());

  ComPtr<ID3D12GraphicsCommandList> reads;
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&reads)));
  transition(reads.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  reads->CopyBufferRegion(result.Get(), 0, target.Get(), 0, sizeof(param.Value));
  transition(reads.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  CHECK(reads->Close());

  SYSTEM_INFO system;
  GetSystemInfo(&system);
  MEMORYSTATUSEX memory{sizeof(memory)};
  GlobalMemoryStatusEx(&memory);
  const UINT64 blocks = std::min(memory.ullTotalPhys, UINT64(system.lpMaximumApplicationAddress) -
                                                      UINT64(system.lpMinimumApplicationAddress)) /
                        system.dwAllocationGranularity;
  std::vector<ComPtr<ID3D12CommandAllocator>> fillers;
  ComPtr<ID3D12GraphicsCommandList2> filler;
  HRESULT closed = S_OK;
  param.Value = 2;
  for (UINT64 i = 0; SUCCEEDED(closed) && i < blocks; i++) {
    if (i % system.dwPageSize == 0)
      step("fill recording arena: allocator %llu", i);
    CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&fillers.emplace_back())));
    if (filler) {
      CHECK(filler->Reset(fillers.back().Get(), nullptr));
    } else {
      CHECK(device->CreateCommandList(0, queue_desc.Type, fillers.back().Get(), nullptr, IID_PPV_ARGS(&filler)));
    }
    filler->WriteBufferImmediate(1, &param, nullptr);
    closed = filler->Close();
  }
  if (!expect(closed == E_OUTOFMEMORY, "arena never exhausted: Close %08lx after %zu allocators", closed,
              fillers.size()))
    return verdict();
  expect(filler->Reset(fillers.back().Get(), nullptr) == E_OUTOFMEMORY, "exhausted list Reset succeeded");
  filler.Reset();
  fillers.pop_back();

  step("failed Close preserves earlier lists on the same allocator");
  ComPtr<ID3D12GraphicsCommandList2> rejected;
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&rejected)));
  rejected->WriteBufferImmediate(1, &param, nullptr);
  auto invalid_mode = D3D12_WRITEBUFFERIMMEDIATE_MODE(D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT + 1);
  rejected->WriteBufferImmediate(1, &param, &invalid_mode);
  expect(rejected->Close() == E_INVALIDARG, "invalid recording Close succeeded");
  step("earlier recording runs while the arena is full");
  CHECK(execute(device.Get(), queue.Get(), kept.Get()));
  CHECK(execute(device.Get(), queue.Get(), rejected.Get()));
  CHECK(execute(device.Get(), queue.Get(), reads.Get()));
  const UINT *word;
  const UINT64 *got;
  CHECK(result->Map(0, nullptr, (void **)&word));
  expect(*word == 1, "retained recording changed: %u", *word);
  result->Unmap(0, nullptr);
  ComPtr<ID3D12CommandAllocator> failed_allocator;
  ComPtr<ID3D12GraphicsCommandList2> failed;
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&failed_allocator)));
  CHECK(device->CreateCommandList(0, queue_desc.Type, failed_allocator.Get(), nullptr, IID_PPV_ARGS(&failed)));
  failed->WriteBufferImmediate(1, &param, nullptr);
  expect(failed->Close() == E_OUTOFMEMORY, "fresh list unexpectedly fitted in the full arena");
  step("public failed recording encodes nothing");
  CHECK(execute(device.Get(), queue.Get(), failed.Get()));
  CHECK(execute(device.Get(), queue.Get(), reads.Get()));
  CHECK(result->Map(0, nullptr, (void **)&word));
  expect(*word == 1, "failed public recording replaced the retained value: %u", *word);
  result->Unmap(0, nullptr);
  CHECK(failed_allocator->Reset());
  expect(failed->Reset(failed_allocator.Get(), nullptr) == E_OUTOFMEMORY,
         "failed list recovered after allocator Reset");

  if (raytracing) {
    step("two exhausted private recordings encode nothing");
    CHECK(execute(device.Get(), queue.Get(), later.Get()));
    CHECK(result->Map(0, nullptr, (void **)&got));
    expect(*got == 0, "failed private recording wrote %llu", *got);
    result->Unmap(0, nullptr);
  }
  fillers.clear();

  step("fresh list after exhaustion writes the right bytes");
  ComPtr<ID3D12CommandAllocator> fresh;
  ComPtr<ID3D12GraphicsCommandList4> next;
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&fresh)));
  CHECK(device->CreateCommandList(0, queue_desc.Type, fresh.Get(), nullptr, IID_PPV_ARGS(&next)));
  param.Value = 3;
  next->WriteBufferImmediate(1, &param, nullptr);
  transition(next.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  next->CopyBufferRegion(result.Get(), 0, target.Get(), 0, sizeof(param.Value));
  transition(next.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  CHECK(submit(device.Get(), queue.Get(), next.Get()));
  CHECK(result->Map(0, nullptr, (void **)&word));
  expect(*word == param.Value, "fresh recording wrote %u, want %u", *word, param.Value);
  result->Unmap(0, nullptr);
  if (raytracing) {
    UINT64 control = 0;
    for (UINT repeat = 0; repeat < 2; repeat++) {
      step("private list after exhaustion: repeat %u", repeat);
      CHECK(fresh->Reset());
      CHECK(next->Reset(fresh.Get(), nullptr));
      transition(next.Get(), information.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                 D3D12_RESOURCE_STATE_COPY_DEST);
      std::array<D3D12_WRITEBUFFERIMMEDIATE_PARAMETER, sizeof(UINT64) / sizeof(UINT)> initial{};
      for (UINT i = 0; i < initial.size(); i++)
        initial[i] = {information->GetGPUVirtualAddress() + i * sizeof(UINT), 0};
      next->WriteBufferImmediate(initial.size(), initial.data(), nullptr);
      transition(next.Get(), information.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      next->EmitRaytracingAccelerationStructurePostbuildInfo(&info, 1, &address);
      transition(next.Get(), information.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                 D3D12_RESOURCE_STATE_COPY_SOURCE);
      next->CopyBufferRegion(result.Get(), 0, information.Get(), 0, sizeof(UINT64));
      transition(next.Get(), information.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      CHECK(submit(device.Get(), queue.Get(), next.Get()));
      CHECK(result->Map(0, nullptr, (void **)&got));
      expect(*got >= sizeof(D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_TOOLS_VISUALIZATION_HEADER),
             "private recording wrote no visualization header: %llu", *got);
      if (repeat)
        expect(*got == control, "private list reuse changed the information: %llu, want %llu", *got, control);
      control = *got;
      result->Unmap(0, nullptr);
    }
  }
  return verdict();
}
