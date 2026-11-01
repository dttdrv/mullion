// contract: a 64-bit InterlockedAdd, InterlockedOr, InterlockedAnd or InterlockedXor whose old value is unused is one
// indivisible step, as every 64-bit atomic is (D3D12 SM 6.6 "Int64 atomics"), in a shader whose 64-bit atomics are all
// that op, where airconv splits them into 32-bit atomics without locks. for each op a shader of only that op runs four
// times as many threads as the GPU runs at once: each adds or xors a value that carries between the halves (or sets or
// clears its own bit) on one word all threads share, on one of 64 words, and on its threadgroup's groupshared word,
// which the group's first thread then stores. the words must end as the CPU's sum (or bits) of every thread's step.
#include "d3d12_test.hpp"
#include <algorithm>
#include <bit>
#include <cstdint>

static const char hlsl[] = R"hlsl(
RWByteAddressBuffer m : register(u0);
groupshared uint64_t shared_word;
uint64_t step(uint t) {
#if OP == 0 || OP == 3
  return uint64_t(t + 1) * MULTIPLIER;
#elif OP == 2
  return ~(1ull << (t % 64));
#else
  return 1ull << (t % 64);
#endif
}
void apply_buffer(uint at, uint64_t v) {
#if OP == 0
  m.InterlockedAdd64(at, v);
#elif OP == 1
  m.InterlockedOr64(at, v);
#elif OP == 2
  m.InterlockedAnd64(at, v);
#else
  m.InterlockedXor64(at, v);
#endif
}
[numthreads(GROUP, 1, 1)]
void cs(uint t : SV_DispatchThreadID, uint i : SV_GroupIndex, uint g : SV_GroupID) {
  if (i == 0)
    shared_word = START;
  GroupMemoryBarrierWithGroupSync();
  uint64_t v = step(t);
#if OP == 0
  InterlockedAdd(shared_word, v);
#elif OP == 1
  InterlockedOr(shared_word, v);
#elif OP == 2
  InterlockedAnd(shared_word, v);
#else
  InterlockedXor(shared_word, v);
#endif
  apply_buffer(0, v);
  apply_buffer(8 * (1 + t % 64), v);
  GroupMemoryBarrierWithGroupSync();
  if (i == 0)
    m.Store<uint64_t>(8 * (65 + g), shared_word);
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
    printf("skipped: 64-bit atomics are DXIL only\n");
    return 77;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1)));
  const UINT group = 256, threads = std::max(std::bit_ceil(options1.TotalLaneCount) * 4, 4096u),
             groups = threads / group, words = 65 + groups;
  const uint64_t multiplier = 0x9e3779b97f4a7c15;
  const char *names[4] = {"add", "or", "and", "xor"};

  D3D12_ROOT_PARAMETER param{D3D12_ROOT_PARAMETER_TYPE_UAV};
  auto rs = root_signature(device.Get(), {1, &param});
  const UINT64 bytes = UINT64(words) * 8;
  auto memory = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));

  unsigned failures = 0;
  for (int op = 0; op < 4; op++) {
    // and clears bits from all ones; add, or and xor start from zero
    const uint64_t start = op == 2 ? ~uint64_t(0) : 0;
    auto cs = compiler.compile(
        hlsl, "cs", "cs_6_6",
        {"OP=" + std::to_string(op), "GROUP=" + std::to_string(group),
         "MULTIPLIER=" + std::to_string(multiplier) + "ull", "START=" + std::to_string(start) + "ull"}
    );
    if (cs.empty()) {
      printf("failed: HLSL for %s did not compile\n", names[op]);
      return 1;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC cs_desc{rs.Get(), bytecode(cs)};
    ComPtr<ID3D12PipelineState> pso;
    CHECK(device->CreateComputePipelineState(&cs_desc, IID_PPV_ARGS(&pso)));

    // what each word must end as: the steps of every thread that touches it, in any order
    auto step = [&](uint64_t t) -> uint64_t {
      return op == 0 || op == 3 ? (t + 1) * multiplier : op == 2 ? ~(uint64_t(1) << (t % 64)) : uint64_t(1) << (t % 64);
    };
    auto combine = [&](uint64_t a, uint64_t b) { return op == 0 ? a + b : op == 1 ? a | b : op == 2 ? a & b : a ^ b; };
    std::vector<uint64_t> want(words, start);
    for (UINT t = 0; t < threads; t++) {
      want[0] = combine(want[0], step(t));
      want[1 + t % 64] = combine(want[1 + t % 64], step(t));
      want[65 + t / group] = combine(want[65 + t / group], step(t));
    }

    CHECK(allocator->Reset());
    ComPtr<ID3D12GraphicsCommandList> list;
    CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
    void *mapped;
    CHECK(upload->Map(0, nullptr, &mapped));
    std::fill_n((uint64_t *)mapped, words, start);
    upload->Unmap(0, nullptr);
    list->CopyBufferRegion(memory.Get(), 0, upload.Get(), 0, bytes);
    transition(list.Get(), memory.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetComputeRootSignature(rs.Get());
    list->SetComputeRootUnorderedAccessView(0, memory->GetGPUVirtualAddress());
    list->Dispatch(groups, 1, 1);
    transition(list.Get(), memory.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), 0, memory.Get(), 0, bytes);
    transition(list.Get(), memory.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    uint64_t *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    for (UINT w = 0; w < words; w++)
      if (got[w] != want[w] && failures++ < 12)
        printf("%s word %u (%s): %#llx, want %#llx\n", names[op], w,
               w == 0 ? "shared by all" : w <= 64 ? "one of 64" : "groupshared", (unsigned long long)got[w],
               (unsigned long long)want[w]);
    readback->Unmap(0, nullptr);
  }
  if (failures) {
    printf("failed: %u wrong results\n", failures);
    return 1;
  }
  printf("passed: 0 wrong results\n");
  return 0;
}
