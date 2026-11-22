// contract: a shader model 6.8 compute shader's [WaveSize(min, max[, preferred])] "is compiled for and will only run
// with a wave size in this range" (DirectX-Specs, HLSL_SM_6_8_WaveSizeRange,
// https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_8_WaveSizeRange.html): a pipeline of a range without a size
// of the device's (the powers of 2 in WaveLaneCountMin..Max, D3D12_FEATURE_DATA_D3D12_OPTIONS1) is refused, as one
// [WaveSize(n)] outside them is (HLSL_SM_6_6_WaveSize), and one with a size in it runs at a size in it; the preferred
// size is only a hint. a single [WaveSize(n)] compiled for 6.8 is written as a range too, and is refused or taken the
// same. each accepted pipeline writes WaveGetLaneCount() from each of its 128 threads, which must all be one size in
// the range
#include "d3d12_test.hpp"
#include <algorithm>

static const char hlsl[] = R"hlsl(
RWStructuredBuffer<uint> lanes : register(u0);
[WaveSize(RANGE)] [numthreads(128, 1, 1)]
void cs(uint t : SV_GroupIndex) { lanes[t] = WaveGetLaneCount(); }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler) || !compiler.dxc) {
    printf("skipped: shader model 6.8 needs DXC\n");
    return 77;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1)));
  if (!expect(options1.WaveOps && options1.WaveLaneCountMin && options1.WaveLaneCountMin <= options1.WaveLaneCountMax,
              "wave lanes %u..%u", options1.WaveLaneCountMin, options1.WaveLaneCountMax))
    return verdict();
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  D3D12_ROOT_PARAMETER uav{D3D12_ROOT_PARAMETER_TYPE_UAV};
  auto rs = root_signature(device.Get(), {1, &uav});
  if (!expect(bool(rs), "root signature"))
    return verdict();
  const struct {
    UINT min, max, preferred;
  } ranges[] = {{4, 128, 0}, {16, 64, 32}, {32, 32, 0}, {64, 128, 0}, {4, 16, 0}, {8, 128, 64}, {64, 64, 0}, {4, 4, 0},
                {128, 128, 0}, {32, 64, 64}, {4, 32, 4}};
  for (auto &r : ranges) {
    std::string range = r.min == r.max ? std::to_string(r.min)
                                       : std::to_string(r.min) + "," + std::to_string(r.max) +
                                             (r.preferred ? "," + std::to_string(r.preferred) : "");
    step("a cs_6_8 shader of [WaveSize(%s)]", range.c_str());
    auto cs = compiler.compile(hlsl, "cs", "cs_6_8", {"RANGE=" + range});
    if (!expect(!cs.empty(), "[WaveSize(%s)] compiles", range.c_str()))
      continue;
    bool fits = false;
    for (UINT size = options1.WaveLaneCountMin; size <= options1.WaveLaneCountMax; size *= 2)
      fits |= size >= r.min && size <= r.max;
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(cs)};
    ComPtr<ID3D12PipelineState> pso;
    HRESULT hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
    if (!expect(SUCCEEDED(hr) == fits, "[WaveSize(%s)] on waves of %u..%u: %08lx, want %s", range.c_str(),
                options1.WaveLaneCountMin, options1.WaveLaneCountMax, hr, fits ? "a pipeline" : "a refusal") ||
        FAILED(hr))
      continue;
    auto lanes = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, 128 * 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 128 * 4, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!expect(lanes && readback, "buffers"))
      return verdict();
    CHECK(forget(readback.Get()));
    list->SetComputeRootSignature(rs.Get());
    list->SetPipelineState(pso.Get());
    list->SetComputeRootUnorderedAccessView(0, lanes->GetGPUVirtualAddress());
    list->Dispatch(1, 1, 1);
    transition(list.Get(), lanes.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), 0, lanes.Get(), 0, 128 * 4);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), nullptr));
    const UINT *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    expect(got[0] >= r.min && got[0] <= r.max && std::all_of(got, got + 128, [&](UINT v) { return v == got[0]; }),
           "[WaveSize(%s)] runs on waves of %u", range.c_str(), got[0]);
    readback->Unmap(0, nullptr);
  }
  return verdict();
}
