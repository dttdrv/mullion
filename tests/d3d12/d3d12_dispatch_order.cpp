// contract: "all UAV accesses (reads or writes) to a particular resource must complete before any future UAV
// accesses (read or write) can begin" across a UAV barrier (D3D12_RESOURCE_UAV_BARRIER), and a transition gives the
// state after it the contents written in the state before. a chain of dispatches in one list, each reading what the
// one before wrote, ends in the value the same chain gives on the CPU, for a UAV barrier, a transition between
// unordered access and shader resource, and an enhanced barrier between the steps. the steps differ, and a thread
// reads the element the thread opposite it wrote, so that a step whose first threads ran while the last threads of
// the step before still had to write gives another value.
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
StructuredBuffer<uint> read_only : register(t0);
RWStructuredBuffer<uint> read_write : register(u0);
RWStructuredBuffer<uint> dst : register(u1);
cbuffer C : register(b0) { uint step; uint last; };
[numthreads(64, 1, 1)]
void cs(uint3 tid : SV_DispatchThreadID) {
  dst[tid.x] = (AS_RESOURCE ? read_only[last - tid.x] : read_write[last - tid.x]) * 3 + step;
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  // many steps, of one group (the GPU has room for all of them at once) and of more than it starts together
  const UINT group = 64, sizes[] = {1, 64}, most = group * sizes[1], steps = 33;
  std::string code[2];
  for (UINT as_resource = 0; as_resource < 2; as_resource++)
    if ((code[as_resource] = compiler.compile(hlsl, "cs", "cs", {"AS_RESOURCE=" + std::to_string(as_resource)})).empty()) {
      printf("failed: HLSL did not compile\n");
      return 1;
    }

  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER params[4] = {
      {D3D12_ROOT_PARAMETER_TYPE_SRV}, {D3D12_ROOT_PARAMETER_TYPE_UAV}, {D3D12_ROOT_PARAMETER_TYPE_UAV},
      {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}
  };
  params[2].Descriptor.ShaderRegister = 1;
  params[3].Constants.Num32BitValues = 2;
  auto rs = root_signature(device.Get(), {4, params});
  if (!rs) {
    printf("failed: root signature\n");
    return 1;
  }
  ComPtr<ID3D12PipelineState> pso[2];
  for (UINT as_resource = 0; as_resource < 2; as_resource++) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(code[as_resource])};
    CHECK(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso[as_resource])));
  }

  const UINT64 bytes = UINT64(most) * sizeof(UINT);
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12Resource> chain[2];
  for (auto &link : chain)
    link = buffer(
        device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
    );
  UINT *input;
  CHECK(upload->Map(0, nullptr, (void **)&input));
  for (UINT i = 0; i < most; i++)
    input[i] = i * 2654435761u + 7;

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12GraphicsCommandList7> list7;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  list.As(&list7);

  enum { uav_barrier, transitions, enhanced, orders };
  const char *names[orders] = {"UAV barrier", "transitions", "enhanced barrier"};
  unsigned failed = 0;
  for (int run = 0; run < orders * int(std::size(sizes)); run++) {
    const int order = run % orders;
    const UINT groups = sizes[run / orders], n = group * groups;
    if (order == enhanced && !list7) {
      printf("enhanced barrier: no ID3D12GraphicsCommandList7\n");
      failed++;
      continue;
    }
    bool as_resource = order == transitions;
    const auto reads = as_resource ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    CHECK(forget(readback.Get()));
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), pso[as_resource].Get()));
    list->CopyResource(chain[0].Get(), upload.Get());
    transition(list.Get(), chain[0].Get(), D3D12_RESOURCE_STATE_COPY_DEST, reads);
    transition(list.Get(), chain[1].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetComputeRootSignature(rs.Get());
    for (UINT step = 0; step < steps; step++) {
      auto src = chain[step % 2].Get(), dst = chain[1 - step % 2].Get();
      if (step && as_resource) {
        transition(list.Get(), src, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, reads);
        transition(list.Get(), dst, reads, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      } else if (step && order == enhanced) {
        D3D12_BUFFER_BARRIER barriers[2];
        for (int i = 0; i < 2; i++)
          barriers[i] = {D3D12_BARRIER_SYNC_COMPUTE_SHADING,    D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                         D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                         chain[i].Get(),                        0,
                         UINT64_MAX};
        D3D12_BARRIER_GROUP barrier_group{D3D12_BARRIER_TYPE_BUFFER, 2};
        barrier_group.pBufferBarriers = barriers;
        list7->Barrier(1, &barrier_group);
      } else if (step) {
        // a null resource is every UAV access
        D3D12_RESOURCE_BARRIER barrier{D3D12_RESOURCE_BARRIER_TYPE_UAV};
        list->ResourceBarrier(1, &barrier);
      }
      if (as_resource)
        list->SetComputeRootShaderResourceView(0, src->GetGPUVirtualAddress());
      else
        list->SetComputeRootUnorderedAccessView(1, src->GetGPUVirtualAddress());
      list->SetComputeRootUnorderedAccessView(2, dst->GetGPUVirtualAddress());
      list->SetComputeRoot32BitConstant(3, step, 0);
      list->SetComputeRoot32BitConstant(3, n - 1, 1);
      list->Dispatch(groups, 1, 1);
    }
    auto last = chain[steps % 2].Get(), other = chain[1 - steps % 2].Get();
    transition(list.Get(), last, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(readback.Get(), last);
    transition(list.Get(), last, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    transition(list.Get(), other, reads, D3D12_RESOURCE_STATE_COPY_DEST);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));

    UINT *out;
    CHECK(readback->Map(0, nullptr, (void **)&out));
    unsigned mismatches = 0;
    for (UINT i = 0; i < n; i++) {
      // each step mirrors the chain, so an odd count of them ends opposite where it began
      UINT want = input[steps % 2 ? n - 1 - i : i];
      for (UINT step = 0; step < steps; step++)
        want = want * 3 + step;
      if (out[i] != want && mismatches++ < 4)
        printf("%s, %u groups a step: element %u: got %u, want %u\n", names[order], groups, i, out[i], want);
    }
    readback->Unmap(0, nullptr);
    printf("%s, %u groups a step: %u of %u elements differ\n", names[order], groups, mismatches, n);
    failed += mismatches != 0;
  }
  printf("%s: %u of %d chains gave another value\n", failed ? "failed" : "passed", failed, int(orders * std::size(sizes)));
  return failed != 0;
}
