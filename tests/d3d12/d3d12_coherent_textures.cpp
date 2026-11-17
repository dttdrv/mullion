// contract: what a group writes to a globallycoherent texture, the group that counts last through an atomic on
// another globallycoherent texture reads there (globallycoherent "causes memory barriers and syncs to flush data across
// the entire GPU such that other groups can see writes", HLSL, RWTexture2D). this is the shape of FSR 3's single
// pass downsampler: each group writes its 32 texels at ((group.x << 5) | lane, group.y), adds 1 to a counter texel,
// and the last group reads every texel. Apple's compiler for the M5 Ultra once refused that shader in Metal
// 4's AIR (dttdrv/mullion#238). every texel gets a value of its own, so a write the last group misses shows as what
// the texel held before or as another texel's value
#include "d3d12_test.hpp"

static constexpr UINT lanes = 32, groups_x = 64, groups_y = 64, width = groups_x * lanes, height = groups_y;
static const char hlsl[] = R"hlsl(
globallycoherent RWTexture2D<uint> image : register(u0);
globallycoherent RWTexture2D<uint> counter : register(u1);
RWStructuredBuffer<uint> result : register(u2);
groupshared bool last;
uint value(uint2 p) { return ((p.x * 0x9e3779b9u) ^ (p.y * 0x85ebca6bu)) | 1; }
[numthreads(1, 1, 1)] void clear() {
  counter[uint2(0, 0)] = 0;
  result[0] = 0;
  result[1] = 0;
}
[numthreads(LANES, 1, 1)] void reduce(uint lane : SV_GroupIndex, uint2 group : SV_GroupID) {
  image[uint2((group.x << 5) | lane, group.y)] = value(uint2((group.x << 5) | lane, group.y));
  AllMemoryBarrierWithGroupSync();
  if (lane == 0) {
    uint old;
    InterlockedAdd(counter[uint2(0, 0)], 1, old);
    last = old == GROUPS - 1;
  }
  GroupMemoryBarrierWithGroupSync();
  if (!last)
    return;
  uint wrong = 0;
  for (uint i = lane; i < WIDTH * HEIGHT; i += LANES) {
    uint2 p = uint2(i % WIDTH, i / WIDTH);
    wrong += image[p] != value(p);
  }
  InterlockedAdd(result[0], wrong);
  InterlockedAdd(result[1], 1);
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  std::vector<std::string> defines{"LANES=" + std::to_string(lanes), "GROUPS=" + std::to_string(groups_x * groups_y),
                                   "WIDTH=" + std::to_string(width), "HEIGHT=" + std::to_string(height)};
  auto clear = compiler.compile(hlsl, "clear", "cs", defines), reduce = compiler.compile(hlsl, "reduce", "cs", defines);
  if (!expect(!clear.empty() && !reduce.empty(), "shaders compile"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2};
  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[0].DescriptorTable = {1, &range};
  params[1].Descriptor = {2, 0};
  auto rs = root_signature(device.Get(), {2, params});
  ComPtr<ID3D12PipelineState> clear_pso, reduce_pso;
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{rs.Get(), bytecode(clear)};
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&clear_pso)));
  pso_desc.CS = bytecode(reduce);
  step("the downsampler's pipeline is made");
  if (!expect(SUCCEEDED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&reduce_pso))), "pipeline"))
    return verdict();

  D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0},
                           D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
  ComPtr<ID3D12Resource> image, counter;
  CHECK(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&image)));
  desc.Width = desc.Height = 1;
  CHECK(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&counter)));
  auto result = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, 8, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 8, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(result && readback, "buffers are made"))
    return verdict();
  CHECK(forget(readback.Get()));

  ComPtr<ID3D12DescriptorHeap> heap;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)));
  auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateUnorderedAccessView(image.Get(), nullptr, nullptr, cpu);
  cpu.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  device->CreateUnorderedAccessView(counter.Get(), nullptr, nullptr, cpu);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), clear_pso.Get(),
                                  IID_PPV_ARGS(&list)));
  ID3D12DescriptorHeap *heaps[] = {heap.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(rs.Get());
  list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
  list->SetComputeRootUnorderedAccessView(1, result->GetGPUVirtualAddress());
  list->Dispatch(1, 1, 1);
  D3D12_RESOURCE_BARRIER uav{D3D12_RESOURCE_BARRIER_TYPE_UAV};
  list->ResourceBarrier(1, &uav);
  list->SetPipelineState(reduce_pso.Get());
  list->Dispatch(groups_x, groups_y, 1);
  transition(list.Get(), result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyResource(readback.Get(), result.Get());
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  const UINT *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  step("the last group reads what every group wrote");
  expect(got[1] == lanes, "%u lanes read the image, expected the last group's %u", got[1], lanes);
  expect(!got[0], "%u of %u texels read wrong", got[0], width * height);
  readback->Unmap(0, nullptr);
  return verdict();
}
