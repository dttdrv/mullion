// contract: a compute shader bound through root SRV and UAV descriptors and a CBV table reads a raw buffer,
// exchanges values through group shared memory across a group barrier, and writes a structured buffer; every
// element equals the same expression evaluated on the CPU from the same input. the CBV views only the first part of
// its buffer, and rows past the view read 0 (D3D11.3 7.5). the table is the last descriptor of a heap of 2,000,000,
// the heap Unreal Engine 5 makes.
#include "d3d12_test.hpp"
#include <algorithm>
#include <bit>

static const char hlsl[] = R"hlsl(
ByteAddressBuffer src : register(t0);
RWStructuredBuffer<uint4> dst : register(u0);
cbuffer C : register(b0) { uint4 rows[ROWS]; };
groupshared uint shared_[GROUP];
[numthreads(GROUP, 1, 1)]
void cs(uint3 tid : SV_DispatchThreadID, uint gi : SV_GroupIndex, uint3 gid : SV_GroupID) {
  uint a = src.Load(tid.x * 4);
  shared_[gi] = a;
  GroupMemoryBarrierWithGroupSync();
  dst[tid.x] = uint4(a * 3 + gid.x, max(a, rows[gi % ROWS].x), asuint(float(a) * 0.5f), shared_[(gi + 1) % GROUP]);
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  // the shader declares `rows` constant rows; the CBV views the smallest legal size, which holds fewer
  const UINT group = 64, groups = 2, n = group * groups, rows = 32,
             view_bytes = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, rows_in_view = view_bytes / 16;
  auto cs = compiler.compile(hlsl, "cs", "cs", {"GROUP=" + std::to_string(group), "ROWS=" + std::to_string(rows)});
  if (cs.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }

  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_DESCRIPTOR_RANGE cbvs{D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1};
  D3D12_ROOT_PARAMETER params[3] = {
      {D3D12_ROOT_PARAMETER_TYPE_SRV}, {D3D12_ROOT_PARAMETER_TYPE_UAV}, {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}
  };
  params[2].DescriptorTable = {1, &cbvs};
  auto rs = root_signature(device.Get(), {3, params});
  if (!rs) {
    printf("failed: root signature\n");
    return 1;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{rs.Get(), bytecode(cs)};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)));

  // input crosses every row's threshold and stays below 2^24, where float(a) * 0.5 is exact; every row is nonzero
  UINT input[n], thresholds[rows][4] = {};
  for (UINT i = 0; i < n; i++)
    input[i] = i * i + 5;
  for (UINT r = 0; r < rows; r++)
    thresholds[r][0] = 100 + r * 97;

  auto src = buffer(
      device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(input) + sizeof(thresholds), D3D12_RESOURCE_STATE_GENERIC_READ
  );
  auto dst = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, n * 16, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, n * 16, D3D12_RESOURCE_STATE_COPY_DEST);
  char *mapped;
  CHECK(src->Map(0, nullptr, (void **)&mapped));
  // the thresholds go first, so the CBV starts at a placement-aligned address
  memcpy(mapped, thresholds, sizeof(thresholds));
  memcpy(mapped + sizeof(thresholds), input, sizeof(input));

  ComPtr<ID3D12DescriptorHeap> heap;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2000000, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
  };
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)));
  const UINT64 last = UINT64(heap_desc.NumDescriptors - 1) * device->GetDescriptorHandleIncrementSize(heap_desc.Type);
  auto table_cpu = heap->GetCPUDescriptorHandleForHeapStart();
  auto table_gpu = heap->GetGPUDescriptorHandleForHeapStart();
  table_cpu.ptr += last;
  table_gpu.ptr += last;
  D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{src->GetGPUVirtualAddress(), view_bytes};
  device->CreateConstantBufferView(&cbv, table_cpu);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  ID3D12DescriptorHeap *heaps[] = {heap.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(rs.Get());
  list->SetComputeRootShaderResourceView(0, src->GetGPUVirtualAddress() + sizeof(thresholds));
  list->SetComputeRootUnorderedAccessView(1, dst->GetGPUVirtualAddress());
  list->SetComputeRootDescriptorTable(2, table_gpu);
  list->Dispatch(groups, 1, 1);
  transition(list.Get(), dst.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyResource(readback.Get(), dst.Get());
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  UINT *out;
  CHECK(readback->Map(0, nullptr, (void **)&out));
  unsigned mismatches = 0;
  for (UINT i = 0; i < n; i++) {
    UINT a = input[i], g = i / group, r = i % group % rows;
    UINT threshold = r < rows_in_view ? thresholds[r][0] : 0;
    UINT want[4] = {
        a * 3 + g, std::max(a, threshold), std::bit_cast<UINT>(float(a) * 0.5f),
        input[g * group + (i % group + 1) % group]
    };
    for (int c = 0; c < 4; c++)
      if (out[i * 4 + c] != want[c] && mismatches++ < 8)
        printf("element %u component %d: got %u, want %u\n", i, c, out[i * 4 + c], want[c]);
  }
  printf("%s: %u mismatching components of %u\n", mismatches ? "failed" : "passed", mismatches, n * 4);
  return mismatches != 0;
}
