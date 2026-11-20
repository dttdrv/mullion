// contract: float sum, product, min and max reduce "across all active non-helper lanes"; prefixes include "up to
// but not including that lane" (Microsoft DXC, Wave Intrinsics, Wave Reduction and Wave Scan/Prefix Intrinsics,
// https://github.com/microsoft/DirectXShaderCompiler/wiki/Wave-Intrinsics). empty prefixes are zero and one.
// finite inputs are signed powers of two, so every allowed evaluation order has the same result. separate cases
// use each infinity alone and with finite values, so neither a sum nor a product produces a NaN.
#include "d3d12_test.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <map>

static const char hlsl[] = R"hlsl(
StructuredBuffer<float> values : register(t0);
StructuredBuffer<uint2> cases : register(t1);
RWStructuredBuffer<uint> result : register(u0);
[numthreads(GROUP, 1, 1)]
void cs(uint t : SV_GroupIndex, uint3 group : SV_GroupID) {
  uint2 c = cases[group.x];
  if (t >= c.x) return;
  uint at = group.x * GROUP + t, b = at * FIELDS;
  uint lane = WaveGetLaneIndex(), width = WaveGetLaneCount();
  result[b] = WaveReadLaneFirst(t);
  result[b + 1] = lane;
  result[b + 2] = width;
  if (c.y == 0 || (c.y == 1 ? lane % 3 != 0 : lane + 1 == width)) {
    float v = values[at];
    float reduced[] = {WaveActiveSum(v), WaveActiveProduct(v), WaveActiveMin(v), WaveActiveMax(v),
                       WavePrefixSum(v), WavePrefixProduct(v)};
    for (uint k = 0; k < OPS; k++)
      result[b + META + k] = asuint(reduced[k]);
  }
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  if (!compiler.dxc) {
    printf("skipped: wave intrinsics are DXIL only\n");
    return 77;
  }
  const char *names[] = {"sum", "product", "min", "max", "prefix sum", "prefix product"};
  const UINT group = D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP, metadata = 3, fields = metadata + std::size(names);
  step("compile float wave reductions and prefixes");
  auto cs = compiler.compile(
      hlsl, "cs", "cs",
      {"GROUP=" + std::to_string(group), "META=" + std::to_string(metadata), "FIELDS=" + std::to_string(fields),
       "OPS=" + std::to_string(std::size(names))}
  );
  if (!expect(!cs.empty(), "float wave HLSL did not compile"))
    return verdict();

  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS1 options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options, sizeof(options)));
  if (!expect(
          options.WaveOps && options.WaveLaneCountMin > 1 && options.WaveLaneCountMax < group,
          "wave operations %d, lanes %u to %u", options.WaveOps, options.WaveLaneCountMin, options.WaveLaneCountMax
      ))
    return verdict();

  const float infinity = std::numeric_limits<float>::infinity();
  const char *patterns[] = {"negative", "positive", "+infinity", "-infinity", "mixed +infinity", "mixed -infinity"};
  std::vector<std::array<UINT, 2>> cases;
  std::vector<float> values;
  for (UINT count :
       {1u, options.WaveLaneCountMin - 1, options.WaveLaneCountMin, options.WaveLaneCountMin + 1,
        options.WaveLaneCountMax - 1, options.WaveLaneCountMax, options.WaveLaneCountMax + 1, group - 1, group})
    for (UINT mask = 0; mask < 3; mask++)
      for (UINT pattern = 0; pattern < std::size(patterns); pattern++) {
        cases.push_back({count, mask});
        for (UINT t = 0; t < group; t++) {
          float v = t & 1 ? -1.0f : -2.0f;
          switch (pattern) {
          case 1: v = -v; break;
          case 2: v = infinity; break;
          case 3: v = -infinity; break;
          case 4: v = t % 3 ? v : infinity; break;
          case 5: v = t % 3 ? v : -infinity; break;
          }
          values.push_back(v);
        }
      }
  auto input = buffer(
      device.Get(), D3D12_HEAP_TYPE_UPLOAD, values.size() * sizeof(float), D3D12_RESOURCE_STATE_GENERIC_READ
  );
  auto sizes = buffer(
      device.Get(), D3D12_HEAP_TYPE_UPLOAD, cases.size() * sizeof(cases[0]), D3D12_RESOURCE_STATE_GENERIC_READ
  );
  void *mapped;
  CHECK(input->Map(0, nullptr, &mapped));
  memcpy(mapped, values.data(), values.size() * sizeof(float));
  input->Unmap(0, nullptr);
  CHECK(sizes->Map(0, nullptr, &mapped));
  memcpy(mapped, cases.data(), cases.size() * sizeof(cases[0]));
  sizes->Unmap(0, nullptr);
  const UINT64 bytes = values.size() * fields * sizeof(UINT);
  auto output = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_ROOT_PARAMETER params[3] = {{D3D12_ROOT_PARAMETER_TYPE_SRV}, {D3D12_ROOT_PARAMETER_TYPE_SRV},
                                   {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[1].Descriptor.ShaderRegister = 1;
  auto rs = root_signature(device.Get(), {std::size(params), params});
  D3D12_COMPUTE_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(cs)};
  ComPtr<ID3D12PipelineState> pso;
  step("create the float wave pipeline");
  CHECK(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)));
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  list->SetComputeRootSignature(rs.Get());
  list->SetComputeRootShaderResourceView(0, input->GetGPUVirtualAddress());
  list->SetComputeRootShaderResourceView(1, sizes->GetGPUVirtualAddress());
  list->SetComputeRootUnorderedAccessView(2, output->GetGPUVirtualAddress());
  step("dispatch float wave reductions and prefixes");
  list->Dispatch(cases.size(), 1, 1);
  transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyResource(readback.Get(), output.Get());
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  UINT *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  for (UINT c = 0; c < cases.size(); c++) {
    auto [count, mask] = cases[c];
    step("%s, %u threads, mask %u", patterns[c % std::size(patterns)], count, mask);
    std::map<UINT, std::map<UINT, UINT>> waves;
    auto row = [&](UINT t) { return got + (c * group + t) * fields; };
    auto active = [&](UINT lane, UINT width) { return mask == 0 || (mask == 1 ? lane % 3 != 0 : lane + 1 == width); };
    for (UINT t = 0; t < count; t++) {
      auto r = row(t);
      if (!expect(r[0] < count && r[2] >= options.WaveLaneCountMin && r[2] <= options.WaveLaneCountMax && r[1] < r[2],
                  "thread %u: wave leader %u, lane %u, width %u", t, r[0], r[1], r[2]))
        continue;
      expect(waves[r[0]].emplace(r[1], t).second, "thread %u repeats lane %u in wave %u", t, r[1], r[0]);
    }
    for (auto &[leader, lanes] : waves) {
      float sum = 0, product = 1, minimum = infinity, maximum = -infinity;
      for (auto [lane, t] : lanes)
        if (active(lane, row(t)[2])) {
          float v = values[c * group + t];
          sum += v;
          product *= v;
          minimum = std::min(minimum, v);
          maximum = std::max(maximum, v);
        }
      float prefix_sum = 0, prefix_product = 1;
      for (auto [lane, t] : lanes)
        if (active(lane, row(t)[2])) {
          float want[] = {sum, product, minimum, maximum, prefix_sum, prefix_product};
          for (UINT k = 0; k < std::size(want); k++) {
            float value = std::bit_cast<float>(row(t)[metadata + k]);
            if (!expect(
                    value == want[k], "wave %u lane %u float %s: %a, want %a", leader, lane, names[k], value, want[k]
                )) {
              readback->Unmap(0, nullptr);
              return verdict();
            }
          }
          prefix_sum += values[c * group + t];
          prefix_product *= values[c * group + t];
        }
    }
  }
  readback->Unmap(0, nullptr);
  return verdict();
}
