// contract: pipelines are independent objects (ID3D12Device::CreateComputePipelineState), however many are made of
// the same shader, on whichever threads, and whichever of them is released meanwhile. several threads make pipelines
// of one compute shader at once and release every other one as soon as it is made, so a shader's last holder goes
// while others are being made of it; then each pipeline that was kept dispatches with its own slot and must write
// that slot's value from the source buffer, as if it were the only pipeline.
#include "d3d12_test.hpp"
#include <algorithm>
#include <thread>

static const char hlsl[] = R"hlsl(
cbuffer C : register(b0) { uint slot; };
ByteAddressBuffer src : register(t0);
RWByteAddressBuffer dst : register(u0);
[numthreads(1, 1, 1)]
void cs() { dst.Store(slot * 4, src.Load(slot * 4) * 3 + slot); }
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
  const UINT threads = std::max(2u, std::thread::hardware_concurrency()), made = 32, kept = threads * made / 2;

  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER params[3] = {
      {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}, {D3D12_ROOT_PARAMETER_TYPE_SRV}, {D3D12_ROOT_PARAMETER_TYPE_UAV}
  };
  params[0].Constants = {0, 0, 1};
  auto rs = root_signature(device.Get(), {3, params});
  if (!rs) {
    printf("failed: root signature\n");
    return 1;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(cs)};
  std::vector<ComPtr<ID3D12PipelineState>> psos(kept);
  std::vector<HRESULT> results(threads, S_OK);
  std::vector<std::thread> makers;
  for (UINT t = 0; t < threads; t++)
    makers.emplace_back([&, t] {
      for (UINT i = 0; i < made && SUCCEEDED(results[t]); i++) {
        ComPtr<ID3D12PipelineState> pso;
        results[t] = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
        if (i & 1)
          psos[t * made / 2 + i / 2] = pso;
      }
    });
  for (auto &maker : makers)
    maker.join();
  for (UINT t = 0; t < threads; t++)
    CHECK(results[t]);

  auto src = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, kept * 4, D3D12_RESOURCE_STATE_GENERIC_READ);
  UINT *values;
  CHECK(src->Map(0, nullptr, (void **)&values));
  for (UINT i = 0; i < kept; i++)
    values[i] = 1000 + i * 7;
  auto dst = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, kept * 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, kept * 4, D3D12_RESOURCE_STATE_COPY_DEST);
  CHECK(forget(readback.Get()));

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  list->SetComputeRootSignature(rs.Get());
  list->SetComputeRootShaderResourceView(1, src->GetGPUVirtualAddress());
  list->SetComputeRootUnorderedAccessView(2, dst->GetGPUVirtualAddress());
  for (UINT i = 0; i < kept; i++) {
    list->SetPipelineState(psos[i].Get());
    list->SetComputeRoot32BitConstant(0, i, 0);
    list->Dispatch(1, 1, 1);
  }
  transition(list.Get(), dst.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyResource(readback.Get(), dst.Get());
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  UINT *out;
  CHECK(readback->Map(0, nullptr, (void **)&out));
  unsigned mismatches = 0;
  for (UINT i = 0; i < kept; i++)
    if (out[i] != values[i] * 3 + i && ++mismatches <= 8)
      printf("wrong: slot %u got %u, want %u\n", i, out[i], values[i] * 3 + i);
  printf("%s: %u of %u pipelines, made on %u threads, wrote wrong\n", mismatches ? "failed" : "passed", mismatches, kept, threads);
  return mismatches != 0;
}
