// contract: a thread's reads see its own earlier writes, in program order (D3D11.3 7.14.1, UAV memory accesses of
// one thread are not reordered), and a read after DeviceMemoryBarrierWithGroupSync sees what the group's other
// threads wrote before it (7.14.3). each thread of a group of 64 reads rows of a buffer it never writes; between them
// it writes values to its UAV word and reads them back through an index a row holds (its own, which the compiler
// cannot know, so no write is forwarded to its read), adds to the word atomically and reads it again, and right
// after the barrier reads the word of the thread a row names, in the other SIMD group. that group (threads 32 to 63)
// first spins 65536 steps, whose result its last add takes, so a read moved above the barrier would see its word
// before that add. few enough loads that the window does not bound them. Mullion moves loads of device and constant
// memory ahead of what uses them (airconv's HoistLoadsPass), never past a store, an atomic or a barrier.
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
StructuredBuffer<uint4> rows : register(t0);
RWStructuredBuffer<uint> words : register(u0);
RWStructuredBuffer<uint> results : register(u1);
[numthreads(64, 1, 1)] void cs(uint id : SV_GroupThreadID) {
  uint4 own = rows[id];                     // .z: id, .w: the thread after it
  uint sum = 0;
  words[id] = id * 3 + 1;
  sum += words[own.z];                      // id * 3 + 1
  [unroll] for (uint i = 0; i < 4; i++) {
    uint4 row = rows[(id + i * 5) & 63];
    words[id] = row.x + i;                  // the row's own index + i
    sum += words[own.z] * (i + 1) + row.y;
  }
  uint lcg = id, spin = own.x >= 32 ? 65536 : 0;
  [loop] for (uint k = 0; k < spin; k++) lcg = lcg * 1664525 + 1013904223;
  InterlockedAdd(words[id], 1000 + (lcg >> 31));
  sum += words[own.z];
  DeviceMemoryBarrierWithGroupSync();
  uint neighbour = words[own.w];
  results[id * 2] = sum;
  results[id * 2 + 1] = neighbour;
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler))
    return 77;
  auto code = compiler.compile(hlsl, "cs", "cs");
  if (!expect(!code.empty(), "the shader does not compile"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));

  // the rows through a descriptor table, so that their reads are checked against the view
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0};
  D3D12_ROOT_PARAMETER parameters[3]{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[0].DescriptorTable = {1, &range};
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  parameters[2].Descriptor.ShaderRegister = 1;
  auto rs = root_signature(device.Get(), {3, parameters});
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{rs.Get(), bytecode(code)};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)));

  constexpr UINT threads = 64;
  auto rows = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, threads * 16, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto words = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, threads * 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto results = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, threads * 8, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, threads * 8, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(rs && rows && words && results && readback, "the root signature or buffers are refused"))
    return verdict();
  UINT *mapped;
  CHECK(rows->Map(0, nullptr, (void **)&mapped));
  for (UINT r = 0; r < threads; r++) {
    const UINT row[4] = {r, r * 7 + 2, r, (r + 32) & (threads - 1)};
    memcpy(mapped + r * 4, row, sizeof(row));
  }
  rows->Unmap(0, nullptr);
  ComPtr<ID3D12DescriptorHeap> heap;
  const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1,
                                             D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)));
  D3D12_SHADER_RESOURCE_VIEW_DESC view{DXGI_FORMAT_UNKNOWN, D3D12_SRV_DIMENSION_BUFFER,
                                       D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
  view.Buffer = {0, threads, 16};
  device->CreateShaderResourceView(rows.Get(), &view, heap->GetCPUDescriptorHandleForHeapStart());

  list->SetDescriptorHeaps(1, heap.GetAddressOf());
  list->SetComputeRootSignature(rs.Get());
  list->SetPipelineState(pso.Get());
  list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
  list->SetComputeRootUnorderedAccessView(1, words->GetGPUVirtualAddress());
  list->SetComputeRootUnorderedAccessView(2, results->GetGPUVirtualAddress());
  list->Dispatch(1, 1, 1);
  transition(list.Get(), results.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyResource(readback.Get(), results.Get());
  CHECK(list->Close());
  CHECK(execute(device.Get(), queue.Get(), list.Get()));

  // what each thread's word ends as, and its sum
  auto last = [](UINT id) {
    UINT lcg = id;
    for (UINT k = 0; k < (id >= 32 ? 65536u : 0u); k++)
      lcg = lcg * 1664525 + 1013904223;
    return ((id + 3 * 5) & 63) + 3 + 1000 + (lcg >> 31);
  };
  UINT *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  UINT wrong = 0;
  for (UINT id = 0; id < threads; id++) {
    UINT sum = id * 3 + 1;
    for (UINT i = 0; i < 4; i++) {
      UINT r = (id + i * 5) & 63;
      sum += (r + i) * (i + 1) + r * 7 + 2;
    }
    sum += last(id);
    if (got[id * 2] != sum && !wrong++)
      printf("thread %u summed %u, want %u\n", id, got[id * 2], sum);
    if (got[id * 2 + 1] != last((id + 32) & 63) && !wrong++)
      printf("thread %u read %u from thread %u, want %u\n", id, got[id * 2 + 1], (id + 32) & 63, last((id + 32) & 63));
  }
  readback->Unmap(0, nullptr);
  expect(!wrong, "%u wrong results", wrong);
  return verdict();
}
