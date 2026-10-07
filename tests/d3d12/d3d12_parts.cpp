// contract: Dispatch(X, Y, Z) runs every threadgroup of the X by Y by Z grid once, and a thread's SV_DispatchThreadID
// is its SV_GroupID times the threadgroup's size plus its SV_GroupThreadID ("the sum of SV_GroupID * numthreads and
// GroupThreadID", HLSL SV_DispatchThreadID), in a dispatch whose arguments come from a buffer as well. a translator
// may hand a dispatch to the GPU in parts (this library does for a shader in which a loop reads globally coherent
// memory, whose threadgroups may wait for one another; DXMT_D3D12_THREADS_AT_ONCE gives a part's threads): the
// threads must not see them. every thread marks the cell of its SV_DispatchThreadID in a grid of the dispatch's
// threads, in a loop of as many turns as a word of the buffer says (one); then one group dispatched through
// ExecuteIndirect marks its cells again
#include "d3d12_test.hpp"
#include <cstdint>

static const char hlsl[] = R"hlsl(
globallycoherent RWStructuredBuffer<uint> marks : register(u0);
static const uint3 threads = uint3(TX, TY, TZ), cells = uint3(GX, GY, GZ) * threads;
[numthreads(TX, TY, TZ)]
void cs(uint3 thread : SV_DispatchThreadID, uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID) {
  if (any(thread != group * threads + local) || any(thread >= cells))
    InterlockedAdd(marks[0], 1);
  else
    for (uint turn = 0; turn < marks[1]; turn++)
      InterlockedAdd(marks[2 + thread.x + cells.x * (thread.y + cells.y * thread.z)], 1);
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const UINT groups[3] = {37, 5, 3}, threads[3] = {4, 2, 2};
  // parts of at most 8 groups: the grid is cut along each of its sides
  SetEnvironmentVariableA("DXMT_D3D12_THREADS_AT_ONCE", std::to_string(8 * threads[0] * threads[1] * threads[2]).c_str());
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  const UINT cells[3] = {groups[0] * threads[0], groups[1] * threads[1], groups[2] * threads[2]};
  const UINT count = cells[0] * cells[1] * cells[2];
  std::vector<std::string> defines;
  for (UINT i = 0; i < 3; i++) {
    defines.push_back(std::string("G") + "XYZ"[i] + "=" + std::to_string(groups[i]));
    defines.push_back(std::string("T") + "XYZ"[i] + "=" + std::to_string(threads[i]));
  }
  auto cs = compiler.compile(hlsl, "cs", "cs", defines);
  if (cs.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  D3D12_ROOT_PARAMETER param{D3D12_ROOT_PARAMETER_TYPE_UAV};
  auto rs = root_signature(device.Get(), {1, &param});
  D3D12_COMPUTE_PIPELINE_STATE_DESC cs_desc{rs.Get(), bytecode(cs)};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateComputePipelineState(&cs_desc, IID_PPV_ARGS(&pso)));
  D3D12_INDIRECT_ARGUMENT_DESC argument{D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH};
  D3D12_COMMAND_SIGNATURE_DESC sig_desc{sizeof(D3D12_DISPATCH_ARGUMENTS), 1, &argument};
  ComPtr<ID3D12CommandSignature> sig;
  CHECK(device->CreateCommandSignature(&sig_desc, nullptr, IID_PPV_ARGS(&sig)));

  // the count of threads with a wrong ID (zero), the loop's turns (one), then a mark for every cell (zero)
  const UINT64 bytes = UINT64(2 + count) * sizeof(uint32_t);
  auto marks = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  uint32_t *mapped;
  CHECK(upload->Map(0, nullptr, (void **)&mapped));
  memset(mapped, 0, bytes);
  mapped[1] = 1;
  auto args = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(D3D12_DISPATCH_ARGUMENTS), D3D12_RESOURCE_STATE_GENERIC_READ);
  D3D12_DISPATCH_ARGUMENTS *one;
  CHECK(args->Map(0, nullptr, (void **)&one));
  *one = {1, 1, 1};
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  list->CopyBufferRegion(marks.Get(), 0, upload.Get(), 0, bytes);
  transition(list.Get(), marks.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  list->SetComputeRootSignature(rs.Get());
  list->SetComputeRootUnorderedAccessView(0, marks->GetGPUVirtualAddress());
  list->Dispatch(groups[0], groups[1], groups[2]);
  list->ExecuteIndirect(sig.Get(), 1, args.Get(), 0, nullptr, 0);
  transition(list.Get(), marks.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(), 0, marks.Get(), 0, bytes);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  uint32_t *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));

  unsigned failures = 0;
  if (got[0] && failures++ < 8)
    printf("%u threads' IDs do not add up or lie outside the dispatch\n", got[0]);
  for (UINT z = 0; z < cells[2]; z++)
    for (UINT y = 0; y < cells[1]; y++)
      for (UINT x = 0; x < cells[0]; x++) {
        // the group at the origin ran twice
        UINT want = 1 + (x < threads[0] && y < threads[1] && z < threads[2]);
        UINT mark = got[2 + x + cells[0] * (y + cells[1] * z)];
        if (mark != want && failures++ < 8)
          printf("thread %u, %u, %u ran %u times, want %u\n", x, y, z, mark, want);
      }
  if (failures) {
    printf("failed: %u wrong results\n", failures);
    return 1;
  }
  printf("passed: %u by %u by %u groups of %u by %u by %u threads, each thread once\n", groups[0], groups[1], groups[2],
         threads[0], threads[1], threads[2]);
  return 0;
}
