// contract: ExecuteIndirect sets root constants from its argument buffer, and afterwards the constants it set are 0
// (D3D12 indirect drawing: "the root constant value is set to 0"), so a later direct dispatch that does not rebind
// them reads 0. the shader marks the slot its constant names; any other outcome leaves a different mark pattern.
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
cbuffer C : register(b0) { uint c; };
RWByteAddressBuffer dst : register(u0);
[numthreads(1, 1, 1)]
void cs() { dst.Store(c * 4, c + 1); }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto cs = compiler.compile(hlsl, "cs", "cs");
  if (cs.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  // the direct constant, the indirect one, and room for every slot either could mark
  const UINT direct = 7, indirect = 5, slots = 8;

  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[0].Constants = {0, 0, 1};
  auto rs = root_signature(device.Get(), {2, params});
  if (!rs) {
    printf("failed: root signature\n");
    return 1;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{rs.Get(), bytecode(cs)};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)));

  struct Arguments {
    UINT constant;
    D3D12_DISPATCH_ARGUMENTS dispatch;
  } arguments{indirect, {1, 1, 1}};
  D3D12_INDIRECT_ARGUMENT_DESC descs[2] = {{D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT}, {D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH}};
  descs[0].Constant = {0, 0, 1};
  D3D12_COMMAND_SIGNATURE_DESC sig_desc{sizeof(Arguments), 2, descs};
  ComPtr<ID3D12CommandSignature> sig;
  CHECK(device->CreateCommandSignature(&sig_desc, rs.Get(), IID_PPV_ARGS(&sig)));

  auto args = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(arguments), D3D12_RESOURCE_STATE_GENERIC_READ);
  void *mapped;
  CHECK(args->Map(0, nullptr, &mapped));
  memcpy(mapped, &arguments, sizeof(arguments));
  auto dst = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, slots * 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, slots * 4, D3D12_RESOURCE_STATE_COPY_DEST);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  list->SetComputeRootSignature(rs.Get());
  list->SetComputeRootUnorderedAccessView(1, dst->GetGPUVirtualAddress());
  list->SetComputeRoot32BitConstant(0, direct, 0);
  list->ExecuteIndirect(sig.Get(), 1, args.Get(), 0, nullptr, 0);
  D3D12_RESOURCE_BARRIER uav{D3D12_RESOURCE_BARRIER_TYPE_UAV};
  uav.UAV.pResource = dst.Get();
  list->ResourceBarrier(1, &uav);
  list->Dispatch(1, 1, 1);
  transition(list.Get(), dst.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyResource(readback.Get(), dst.Get());
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  UINT *out;
  CHECK(readback->Map(0, nullptr, (void **)&out));
  // the indirect dispatch marks its own slot, the direct one slot 0
  unsigned mismatches = 0;
  for (UINT i = 0; i < slots; i++) {
    UINT want = i == indirect || i == 0 ? i + 1 : 0;
    if (out[i] != want && ++mismatches)
      printf("slot %u: got %u, want %u\n", i, out[i], want);
  }
  printf("%s: %u mismatching slots of %u\n", mismatches ? "failed" : "passed", mismatches, slots);
  return mismatches != 0;
}
