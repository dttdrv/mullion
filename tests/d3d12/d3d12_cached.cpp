// contract: a pipeline state gives a cached blob, and a pipeline created from the same description with that blob
// is created (D3D12_CACHED_PIPELINE_STATE: "intended to be filled with the data retrieved from
// ID3D12PipelineState::GetCachedBlob"); a blob that is not one of this driver's is refused with
// D3D12_ERROR_DRIVER_VERSION_MISMATCH, which tells the application to do without its cache; the device says it
// supports this (D3D12_SHADER_CACHE_SUPPORT_SINGLE_PSO: "This is always supported"). a pipeline created from a blob
// works: its dispatch writes what the shader computes. the same holds for a graphics pipeline's blob.
// AtomicCopyBufferUINT and AtomicCopyBufferUINT64 copy their one element, in order with the list's other copies.
#include "d3d12_test.hpp"
#include <iterator>

static const char hlsl[] = R"hlsl(
RWStructuredBuffer<uint> o : register(u0);
[numthreads(1, 1, 1)] void cs() { o[0] = VALUE; }
float4 vs(uint id : SV_VertexID) : SV_Position { return float4(float2(id & 1, id >> 1) * 4 - 1, 0, 1); }
float4 ps() : SV_Target { return 1; }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const UINT value = 0xcac4ed, first = 0x11111111, second = 0x22222222;
  const UINT64 wide = 0x3333333344444444ull;
  std::vector<std::string> defines = {"VALUE=" + std::to_string(value)};
  auto cs = compiler.compile(hlsl, "cs", "cs", defines), vs = compiler.compile(hlsl, "vs", "vs", defines),
       ps = compiler.compile(hlsl, "ps", "ps", defines);
  if (cs.empty() || vs.empty() || ps.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  unsigned failures = 0;
  auto expect = [&](bool ok, const char *what, long long got, long long want) {
    if (!ok && failures++ < 12)
      printf("%s: %#llx, want %#llx\n", what, got, want);
  };
  D3D12_FEATURE_DATA_SHADER_CACHE cache{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_CACHE, &cache, sizeof(cache)));
  expect(cache.SupportFlags & D3D12_SHADER_CACHE_SUPPORT_SINGLE_PSO, "shader cache support", cache.SupportFlags,
         D3D12_SHADER_CACHE_SUPPORT_SINGLE_PSO);

  D3D12_ROOT_PARAMETER param{D3D12_ROOT_PARAMETER_TYPE_UAV};
  auto rs = root_signature(device.Get(), {1, &param});
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute{rs.Get(), bytecode(cs)};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{rs.Get(), bytecode(vs), bytecode(ps)};
  graphics.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  graphics.SampleMask = ~0u;
  graphics.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  graphics.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  graphics.NumRenderTargets = 1;
  graphics.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  graphics.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> made[2], again[2], refused;
  CHECK(device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&made[0])));
  CHECK(device->CreateGraphicsPipelineState(&graphics, IID_PPV_ARGS(&made[1])));
  ComPtr<ID3DBlob> blobs[2];
  for (int kind = 0; kind < 2; kind++) {
    const char *name = kind ? "a graphics pipeline" : "a compute pipeline";
    HRESULT hr = made[kind]->GetCachedBlob(&blobs[kind]);
    if (FAILED(hr) || !blobs[kind] || !blobs[kind]->GetBufferSize()) {
      printf("failed: GetCachedBlob of %s: %#lx\n", name, (unsigned long)hr);
      return 1;
    }
    // what is not this driver's blob: its bytes with one changed, and nothing like it
    std::string changed((const char *)blobs[kind]->GetBufferPointer(), blobs[kind]->GetBufferSize()), other = "PSO";
    changed[0] ^= 1;
    auto create = [&](const void *bytes, SIZE_T size, ComPtr<ID3D12PipelineState> &out) {
      compute.CachedPSO = graphics.CachedPSO = {bytes, size};
      return kind ? device->CreateGraphicsPipelineState(&graphics, IID_PPV_ARGS(&out))
                  : device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&out));
    };
    hr = create(blobs[kind]->GetBufferPointer(), blobs[kind]->GetBufferSize(), again[kind]);
    expect(hr == S_OK && again[kind], name, hr, S_OK);
    for (auto &foreign : {changed, other}) {
      hr = create(foreign.data(), foreign.size(), refused);
      expect(hr == D3D12_ERROR_DRIVER_VERSION_MISMATCH && !refused, "another driver's blob", hr, D3D12_ERROR_DRIVER_VERSION_MISMATCH);
    }
  }
  if (!again[0]) {
    printf("failed: no pipeline from a cached blob\n");
    return 1;
  }

  // the dispatch of the pipeline from the blob, then the copies: a UINT, a UINT64, and a UINT over the first
  const UINT64 out_size = 32, values_size = 16;
  auto out = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, out_size, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto values = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, values_size, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, out_size, D3D12_RESOURCE_STATE_COPY_DEST);
  uint8_t *mapped;
  CHECK(values->Map(0, nullptr, (void **)&mapped));
  memcpy(mapped, &first, 4);
  memcpy(mapped + 4, &second, 4);
  memcpy(mapped + 8, &wide, 8);
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12GraphicsCommandList1> list1;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), again[0].Get(), IID_PPV_ARGS(&list)));
  CHECK(list.As(&list1));
  list->SetComputeRootSignature(rs.Get());
  list->SetComputeRootUnorderedAccessView(0, out->GetGPUVirtualAddress());
  list->Dispatch(1, 1, 1);
  transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
  // elements 2 and 4..5 of 32-bit words; element 3 is written twice, the second copy last
  list1->AtomicCopyBufferUINT(out.Get(), 8, values.Get(), 0, 0, nullptr, nullptr);
  list1->AtomicCopyBufferUINT64(out.Get(), 16, values.Get(), 8, 0, nullptr, nullptr);
  list1->AtomicCopyBufferUINT(out.Get(), 12, values.Get(), 0, 0, nullptr, nullptr);
  list1->AtomicCopyBufferUINT(out.Get(), 12, values.Get(), 4, 0, nullptr, nullptr);
  transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyResource(readback.Get(), out.Get());
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  const UINT *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  UINT64 got_wide;
  memcpy(&got_wide, got + 4, 8);
  expect(got[0] == value, "the dispatch of a pipeline from a cached blob", got[0], value);
  expect(got[1] == 0, "the word after it", got[1], 0);
  expect(got[2] == first, "AtomicCopyBufferUINT", got[2], first);
  expect(got[3] == second, "the later of two copies to one element", got[3], second);
  expect(got_wide == wide, "AtomicCopyBufferUINT64", got_wide, wide);
  expect(got[6] == 0 && got[7] == 0, "the words after the copies", got[6] | got[7], 0);
  printf("%s: %u wrong answers\n", failures ? "failed" : "passed", failures);
  return failures != 0;
}
