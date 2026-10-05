// contract: wave intrinsics compute over the active lanes of a wave, as D3D12 defines them. one threadgroup of one
// wave (the device's reported lane count) runs; a third of its lanes sit out the operations, so every reduction,
// prefix, ballot and read must see only the rest. each thread records its lane, and the expectations are the same
// operations simulated over those lanes on the CPU, including 64-bit ones, which Metal has no SIMD functions for.
#include "d3d12_test.hpp"
#include <algorithm>
#include <bit>
#include <map>

static const char hlsl[] = R"hlsl(
RWStructuredBuffer<uint> o : register(u0);
[numthreads(LANES, 1, 1)]
void cs(uint t : SV_GroupIndex) {
  uint b = t * FIELDS;
  o[b + 0] = WaveGetLaneIndex();
  o[b + 1] = WaveGetLaneCount();
  if (t % 3 != 1) {
    uint v = t * 7 + 3;
    int s = int(t) - 10;
    float f = float(t) * 0.5;
    uint4 ballot = WaveActiveBallot(t & 1);
    o[b + 2] = ballot.x;
    o[b + 3] = ballot.y;
    o[b + 4] = WaveActiveAnyTrue(t == 5);
    o[b + 5] = WaveActiveAllTrue(t % 3 != 1);
    o[b + 6] = WaveActiveAllEqual(v);
    o[b + 7] = WaveReadLaneFirst(v);
    o[b + 8] = WaveReadLaneAt(v, READ_LANE);
    o[b + 9] = WaveActiveSum(v);
    o[b + 10] = WaveActiveProduct(v);
    o[b + 11] = asuint(WaveActiveMin(s));
    o[b + 12] = WaveActiveMax(v);
    o[b + 13] = asuint(WaveActiveSum(f));
    o[b + 14] = WaveActiveBitAnd(v);
    o[b + 15] = WaveActiveBitOr(v);
    o[b + 16] = WaveActiveBitXor(v);
    o[b + 17] = WavePrefixSum(v);
    o[b + 18] = WavePrefixProduct(v);
    o[b + 19] = WaveActiveCountBits(t & 1);
    o[b + 20] = WavePrefixCountBits(t & 1);
    o[b + 21] = WaveIsFirstLane();
    uint64_t w = (uint64_t(v) << 32) | t;
    uint64_t sum = WaveActiveSum(w), prefix = WavePrefixSum(w), largest = WaveActiveMax(w);
    o[b + 22] = uint(sum);
    o[b + 23] = uint(sum >> 32);
    o[b + 24] = uint(prefix);
    o[b + 25] = uint(prefix >> 32);
    o[b + 26] = uint(largest);
    o[b + 27] = uint(largest >> 32);
  }
  o[b + 28] = QuadReadAcrossX(t);
  o[b + 29] = QuadReadAcrossY(t);
  o[b + 30] = QuadReadAcrossDiagonal(t);
  o[b + 31] = QuadReadLaneAt(t, 2);
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
    printf("skipped: wave operations are DXIL only\n");
    return 77;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS1 options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options, sizeof(options)));
  if (!options.WaveOps || options.WaveLaneCountMin != options.WaveLaneCountMax) {
    printf("failed: wave operations reported %d, lanes %u to %u\n", options.WaveOps, options.WaveLaneCountMin,
           options.WaveLaneCountMax);
    return 1;
  }
  const UINT lanes = options.WaveLaneCountMin, fields = 32, read_lane = 3;
  auto cs = compiler.compile(
      hlsl, "cs", "cs",
      {"LANES=" + std::to_string(lanes), "FIELDS=" + std::to_string(fields), "READ_LANE=" + std::to_string(read_lane)}
  );
  if (cs.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }

  D3D12_ROOT_PARAMETER param{D3D12_ROOT_PARAMETER_TYPE_UAV};
  auto rs = root_signature(device.Get(), {1, &param});
  D3D12_COMPUTE_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(cs)};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)));
  const UINT64 bytes = lanes * fields * 4;
  auto out = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  list->SetComputeRootSignature(rs.Get());
  list->SetComputeRootUnorderedAccessView(0, out->GetGPUVirtualAddress());
  list->Dispatch(1, 1, 1);
  transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyResource(readback.Get(), out.Get());
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  UINT *o;
  CHECK(readback->Map(0, nullptr, (void **)&o));

  // the threads by lane, and the operations over the active ones, in lane order
  std::map<UINT, UINT> thread_of;
  for (UINT t = 0; t < lanes; t++)
    thread_of[o[t * fields]] = t;
  unsigned failures = 0;
  auto expect = [&](UINT t, UINT field, uint64_t want, const char *what) {
    if (o[t * fields + field] != (UINT)want && failures++ < 8)
      printf("thread %u %s: %u, want %u\n", t, what, o[t * fields + field], (UINT)want);
  };
  if (thread_of.size() != lanes)
    printf("failed: %zu distinct lanes of %u\n", thread_of.size(), lanes), failures++;
  auto active = [](UINT t) { return t % 3 != 1; };
  auto v = [](UINT t) { return t * 7 + 3; };
  auto w = [&](UINT t) { return (uint64_t)v(t) << 32 | t; };
  uint64_t ballot = 0, sum64 = 0, max64 = 0;
  UINT sum = 0, product = 1, and_ = ~0u, or_ = 0, xor_ = 0, max = 0, odd = 0, first = ~0u;
  int min = INT32_MAX;
  float fsum = 0;
  for (auto [lane, t] : thread_of)
    if (active(t)) {
      ballot |= (uint64_t)(t & 1) << lane;
      sum += v(t), product *= v(t), and_ &= v(t), or_ |= v(t), xor_ ^= v(t), max = std::max(max, v(t));
      min = std::min(min, (int)t - 10), fsum += t * 0.5f, odd += t & 1;
      sum64 += w(t), max64 = std::max(max64, w(t));
      first = std::min(first, lane);
    }
  for (auto [lane, t] : thread_of) {
    expect(t, 1, lanes, "lane count");
    if (active(t)) {
      UINT prefix_sum = 0, prefix_product = 1, prefix_odd = 0;
      uint64_t prefix64 = 0;
      for (auto [l, u] : thread_of)
        if (l < lane && active(u))
          prefix_sum += v(u), prefix_product *= v(u), prefix_odd += u & 1, prefix64 += w(u);
      expect(t, 2, ballot, "ballot low");
      expect(t, 3, ballot >> 32, "ballot high");
      expect(t, 4, active(5), "any");
      expect(t, 5, 1, "all");
      expect(t, 6, 0, "all equal");
      expect(t, 7, v(thread_of[first]), "read first");
      expect(t, 8, v(thread_of[read_lane]), "read lane");
      expect(t, 9, sum, "sum");
      expect(t, 10, product, "product");
      expect(t, 11, (UINT)min, "min");
      expect(t, 12, max, "max");
      expect(t, 13, std::bit_cast<UINT>(fsum), "float sum");
      expect(t, 14, and_, "and");
      expect(t, 15, or_, "or");
      expect(t, 16, xor_, "xor");
      expect(t, 17, prefix_sum, "prefix sum");
      expect(t, 18, prefix_product, "prefix product");
      expect(t, 19, odd, "count bits");
      expect(t, 20, prefix_odd, "prefix count bits");
      expect(t, 21, lane == first, "first lane");
      expect(t, 22, sum64, "64-bit sum low");
      expect(t, 23, sum64 >> 32, "64-bit sum high");
      expect(t, 24, prefix64, "64-bit prefix low");
      expect(t, 25, prefix64 >> 32, "64-bit prefix high");
      expect(t, 26, max64, "64-bit max low");
      expect(t, 27, max64 >> 32, "64-bit max high");
    }
    // quads are lanes 4n to 4n + 3
    expect(t, 28, thread_of[lane ^ 1], "quad across x");
    expect(t, 29, thread_of[lane ^ 2], "quad across y");
    expect(t, 30, thread_of[lane ^ 3], "quad across diagonal");
    expect(t, 31, thread_of[(lane & ~3u) | 2], "quad lane 2");
  }
  readback->Unmap(0, nullptr);
  printf("%s: %u mismatches over %u lanes\n", failures ? "failed" : "passed", failures, lanes);
  return failures != 0;
}
