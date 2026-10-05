// contract: every 64-bit atomic on a buffer, under contention across and within SIMD groups, is one indivisible step
// and returns what memory held. THREADS threads add a step that carries between the halves to one word, so the values
// they get back are each multiple of the step once; they exchange distinct values into one word, so what comes back and
// what stays are the values put in; they compare-exchange one word from zero, so one wins and the rest see its value;
// min and max, signed and unsigned, end at the extreme and return only values put in. every 64 threads set, clear or
// flip one bit each of a word of their own, so each sees its own bit untouched and a different count of the others.
// in groupshared memory, each threadgroup, of many SIMD groups, adds the step to its own word and compare-exchanges
// another from zero; and every 64 threads add it to a texel of their own of an R32G32_UINT texture, through a heap.
// alone, one thread raises and lowers words with min and max, signed and unsigned, and gets back what they held.
#include "d3d12_test.hpp"
#include <algorithm>
#include <bit>
#include <cstdint>

static const char hlsl[] = R"hlsl(
RWByteAddressBuffer m : register(u0);
RWStructuredBuffer<uint64_t> r : register(u1);
RWTexture2D<uint64_t> texels : register(u2);
groupshared uint64_t shared_words[2];
uint64_t value(uint t) { return uint64_t(t) * MULTIPLIER; }
[numthreads(GROUP, 1, 1)]
void cs(uint t : SV_DispatchThreadID, uint i : SV_GroupIndex, uint g : SV_GroupID) {
  uint64_t v = value(t), bit = 1ull << (t % 64), o[OPS];
  if (i == 0)
    shared_words[0] = shared_words[1] = 0;
  GroupMemoryBarrierWithGroupSync();
  InterlockedAdd(shared_words[0], STEP, o[10]);
  InterlockedCompareExchange(shared_words[1], 0, v + 1, o[11]);
  InterlockedAdd(texels[uint2(t / 64, 0)], STEP, o[12]);
  uint alone = 8 * (SINGLES + 3 * WORDS + 2 * GROUPS);
  o[13] = o[14] = o[15] = o[16] = 0;
  if (t == 0) {
    int64_t signed_old[2];
    m.InterlockedMax64(alone, 9ull, o[13]);
    m.InterlockedMin64(alone + 8, 5ull, o[14]);
    m.InterlockedMax64(alone + 16, int64_t(3), signed_old[0]);
    m.InterlockedMin64(alone + 24, int64_t(-1), signed_old[1]);
    o[15] = signed_old[0];
    o[16] = signed_old[1];
  }
  GroupMemoryBarrierWithGroupSync();
  if (i == 0) {
    m.Store<uint64_t>(8 * (SINGLES + 3 * WORDS + 2 * g), shared_words[0]);
    m.Store<uint64_t>(8 * (SINGLES + 3 * WORDS + 2 * g + 1), shared_words[1]);
  }
  int64_t s[2];
  uint bits = 8 * (SINGLES + t / 64);
  m.InterlockedAdd64(0, STEP, o[0]);
  m.InterlockedMax64(8, v, o[1]);
  m.InterlockedMin64(16, v, o[2]);
  m.InterlockedMax64(24, int64_t(v), s[0]);
  m.InterlockedMin64(32, int64_t(v), s[1]);
  m.InterlockedExchange64(40, v, o[5]);
  m.InterlockedCompareExchange64(48, 0, v + 1, o[6]);
  m.InterlockedOr64(bits, bit, o[7]);
  m.InterlockedAnd64(bits + 8 * WORDS, ~bit, o[8]);
  m.InterlockedXor64(bits + 16 * WORDS, bit, o[9]);
  o[3] = s[0];
  o[4] = s[1];
  for (uint k = 0; k < OPS; k++)
    r[t * OPS + k] = o[k];
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
  if (!options1.Int64ShaderOps) {
    printf("failed: Int64ShaderOps is off\n");
    return 1;
  }
  // the largest threadgroups, and four times as many threads as the GPU runs at once, so SIMD groups contend
  const UINT group = D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP,
             threads = std::max(std::bit_ceil(options1.TotalLaneCount) * 4, group), ops = 17,
             singles = 7, words = threads / 64;
  const uint64_t multiplier = 0x9e3779b97f4a7c15, step = (uint64_t(1) << 32) + 1;
  auto value = [&](uint64_t t) { return t * multiplier; };
  auto cs = compiler.compile(
      hlsl, "cs", "cs_6_6",
      {"GROUP=" + std::to_string(group), "OPS=" + std::to_string(ops), "SINGLES=" + std::to_string(singles),
       "WORDS=" + std::to_string(words), "GROUPS=" + std::to_string(threads / group), "MULTIPLIER=" + std::to_string(multiplier) + "ull",
       "STEP=" + std::to_string(step) + "ull"}
  );
  if (cs.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  D3D12_DESCRIPTOR_RANGE texel_range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 2};
  D3D12_ROOT_PARAMETER params[3] = {{D3D12_ROOT_PARAMETER_TYPE_UAV}, {D3D12_ROOT_PARAMETER_TYPE_UAV},
                                    {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}};
  params[1].Descriptor.ShaderRegister = 1;
  params[2].DescriptorTable = {1, &texel_range};
  auto rs = root_signature(device.Get(), {3, params});
  D3D12_COMPUTE_PIPELINE_STATE_DESC cs_desc{rs.Get(), bytecode(cs)};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateComputePipelineState(&cs_desc, IID_PPV_ARGS(&pso)));

  // the words' starting values: unsigned and signed extremes for min and max, one value no thread puts in for the
  // exchange, zero for the compare exchange, then the bit words (or and xor from zero, and from all ones)
  const UINT groups = threads / group;
  // the words one thread moves alone, and where they end: max 5 to 9, min 9 to 5, signed max -1 to 3, min 3 to -1
  const uint64_t alone_start[4] = {5, 9, uint64_t(-1), 3}, alone_end[4] = {9, 5, 3, uint64_t(-1)};
  std::vector<uint64_t> start(singles + 3 * words + 2 * groups + 4);
  std::copy(std::begin(alone_start), std::end(alone_start), start.end() - 4);
  start[2] = ~uint64_t(0);
  start[3] = uint64_t(INT64_MIN);
  start[4] = uint64_t(INT64_MAX);
  start[5] = value(threads);
  std::fill_n(start.begin() + singles + words, words, ~uint64_t(0));
  const UINT64 memory_bytes = start.size() * 8, returned_bytes = UINT64(threads) * ops * 8;
  auto memory = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, memory_bytes, D3D12_RESOURCE_STATE_COPY_DEST,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto returned = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, returned_bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, memory_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  // the texture, a row of a texel per 64 threads, starting at zero from a zeroed upload, read back after the rest
  const UINT64 pitch = (words * 8 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~UINT64(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1),
               texels_at = (memory_bytes + returned_bytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
                           ~UINT64(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
  ComPtr<ID3D12Resource> texture;
  D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC texture_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, words, 1, 1, 1, DXGI_FORMAT_R32G32_UINT, {1, 0},
                                   D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
  CHECK(device->CreateCommittedResource(
      &default_heap, D3D12_HEAP_FLAG_NONE, &texture_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)
  ));
  auto zeros = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, pitch, D3D12_RESOURCE_STATE_GENERIC_READ);
  ComPtr<ID3D12DescriptorHeap> heap;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)));
  D3D12_UNORDERED_ACCESS_VIEW_DESC texel_view{DXGI_FORMAT_R32G32_UINT, D3D12_UAV_DIMENSION_TEXTURE2D};
  device->CreateUnorderedAccessView(texture.Get(), nullptr, &texel_view, heap->GetCPUDescriptorHandleForHeapStart());
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, texels_at + pitch, D3D12_RESOURCE_STATE_COPY_DEST);
  void *mapped;
  CHECK(upload->Map(0, nullptr, &mapped));
  memcpy(mapped, start.data(), memory_bytes);
  CHECK(zeros->Map(0, nullptr, &mapped));
  memset(mapped, 0, pitch);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  list->CopyBufferRegion(memory.Get(), 0, upload.Get(), 0, memory_bytes);
  transition(list.Get(), memory.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  D3D12_TEXTURE_COPY_LOCATION row{zeros.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
      texel_rows{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  row.PlacedFootprint = {0, {DXGI_FORMAT_R32G32_UINT, words, 1, 1, (UINT)pitch}};
  list->CopyTextureRegion(&texel_rows, 0, 0, 0, &row, nullptr);
  transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  list->SetDescriptorHeaps(1, heap.GetAddressOf());
  list->SetComputeRootSignature(rs.Get());
  list->SetComputeRootUnorderedAccessView(0, memory->GetGPUVirtualAddress());
  list->SetComputeRootUnorderedAccessView(1, returned->GetGPUVirtualAddress());
  list->SetComputeRootDescriptorTable(2, heap->GetGPUDescriptorHandleForHeapStart());
  list->Dispatch(groups, 1, 1);
  transition(list.Get(), memory.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(list.Get(), returned.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(), 0, memory.Get(), 0, memory_bytes);
  list->CopyBufferRegion(readback.Get(), memory_bytes, returned.Get(), 0, returned_bytes);
  transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  row = {readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  row.PlacedFootprint = {texels_at, {DXGI_FORMAT_R32G32_UINT, words, 1, 1, (UINT)pitch}};
  list->CopyTextureRegion(&row, 0, 0, 0, &texel_rows, nullptr);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  uint64_t *m;
  CHECK(readback->Map(0, nullptr, (void **)&m));
  const uint64_t *r = m + start.size();

  unsigned failures = 0;
  auto fail = [&](const char *what, uint64_t got, uint64_t want) {
    if (failures++ < 12)
      printf("%s: %#llx, want %#llx\n", what, (unsigned long long)got, (unsigned long long)want);
  };
  auto returns = [&](UINT op) {
    std::vector<uint64_t> v(threads);
    for (UINT t = 0; t < threads; t++)
      v[t] = r[t * ops + op];
    return v;
  };
  std::vector<uint64_t> values(threads);
  for (UINT t = 0; t < threads; t++)
    values[t] = value(t);

  // add: memory at threads steps; returned, sorted, each multiple of the step
  auto added = returns(0);
  std::sort(added.begin(), added.end());
  if (m[0] != threads * step)
    fail("add memory", m[0], threads * step);
  for (UINT k = 0; k < threads; k++)
    if (added[k] != k * step)
      fail("add returned (sorted)", added[k], k * step);
  // min and max: memory at the extreme; returned, the start or a value put in
  auto extreme = [&](UINT op, auto better, const char *what) {
    auto want = start[op];
    for (auto v : values)
      want = better(v, want) ? v : want;
    if (m[op] != want)
      fail(what, m[op], want);
    for (auto got : returns(op))
      if (got != start[op] && !std::binary_search(values.begin(), values.end(), got))
        fail(what, got, want);
  };
  std::sort(values.begin(), values.end());
  extreme(1, [](uint64_t a, uint64_t b) { return a > b; }, "unsigned max");
  extreme(2, [](uint64_t a, uint64_t b) { return a < b; }, "unsigned min");
  extreme(3, [](uint64_t a, uint64_t b) { return int64_t(a) > int64_t(b); }, "signed max");
  extreme(4, [](uint64_t a, uint64_t b) { return int64_t(a) < int64_t(b); }, "signed min");
  // exchange: what came back and what stays are what went in
  auto swapped = returns(5), in = values;
  swapped.push_back(m[5]);
  in.push_back(start[5]);
  std::sort(swapped.begin(), swapped.end());
  std::sort(in.begin(), in.end());
  for (UINT k = 0; k <= threads; k++)
    if (swapped[k] != in[k])
      fail("exchange (sorted)", swapped[k], in[k]);
  // compare exchange from zero: one thread sees zero, and memory and every other thread hold its value
  auto compared = returns(6);
  auto winners = std::count(compared.begin(), compared.end(), 0);
  auto winner = std::find(compared.begin(), compared.end(), 0) - compared.begin();
  if (winners != 1)
    fail("compare exchange winners", winners, 1);
  for (UINT t = 0; t < threads; t++)
    if (t != winner && compared[t] != m[6])
      fail("compare exchange returned", compared[t], m[6]);
  if (m[6] != value(winner) + 1)
    fail("compare exchange memory", m[6], value(winner) + 1);
  // bits: each thread's own bit as it started, and each thread of a word a different count of the rest
  const uint64_t ends[3] = {~uint64_t(0), 0, ~uint64_t(0)};
  const char *names[3] = {"or", "and", "xor"};
  for (UINT op = 0; op < 3; op++)
    for (UINT w = 0; w < words; w++) {
      auto word = singles + op * words + w;
      if (m[word] != ends[op])
        fail(names[op], m[word], ends[op]);
      uint64_t counts = 0;
      for (UINT t = w * 64; t < w * 64 + 64; t++) {
        auto got = r[t * ops + 7 + op], bit = uint64_t(1) << (t % 64);
        if ((got & bit) != (start[word] & bit))
          fail(names[op], got, start[word] ^ bit);
        counts |= uint64_t(1) << std::popcount(start[word] ? ~got : got);
      }
      if (counts != ~uint64_t(0))
        fail(names[op], counts, ~uint64_t(0));
    }
  // groupshared: per threadgroup, the adds as above, and one compare exchange winner whose value the rest see
  for (UINT g = 0; g < groups; g++) {
    auto word = m + singles + 3 * words + 2 * g;
    std::vector<uint64_t> olds;
    UINT winners = 0, winner = 0;
    for (UINT t = g * group; t < (g + 1) * group; t++) {
      olds.push_back(r[t * ops + 10]);
      if (!r[t * ops + 11])
        winners++, winner = t;
    }
    std::sort(olds.begin(), olds.end());
    if (word[0] != group * step)
      fail("groupshared add memory", word[0], group * step);
    for (UINT k = 0; k < group; k++)
      if (olds[k] != k * step)
        fail("groupshared add returned (sorted)", olds[k], k * step);
    if (winners != 1 || word[1] != value(winner) + 1)
      fail("groupshared compare exchange", word[1], value(winner) + 1);
    for (UINT t = g * group; t < (g + 1) * group; t++)
      if (t != winner && r[t * ops + 11] != word[1])
        fail("groupshared compare exchange returned", r[t * ops + 11], word[1]);
  }
  // texels: 64 adds each, and the values returned each multiple of the step once
  auto texels = m + texels_at / 8;
  for (UINT w = 0; w < words; w++) {
    std::vector<uint64_t> olds;
    for (UINT t = w * 64; t < w * 64 + 64; t++)
      olds.push_back(r[t * ops + 12]);
    std::sort(olds.begin(), olds.end());
    if (texels[w] != 64 * step)
      fail("texel add memory", texels[w], 64 * step);
    for (UINT k = 0; k < 64; k++)
      if (olds[k] != k * step)
        fail("texel add returned (sorted)", olds[k], k * step);
  }
  // alone: each returns what the word held, and leaves what it moved it to
  for (UINT k = 0; k < 4; k++) {
    if (r[13 + k] != alone_start[k])
      fail("alone min/max returned", r[13 + k], alone_start[k]);
    if (m[start.size() - 4 + k] != alone_end[k])
      fail("alone min/max memory", m[start.size() - 4 + k], alone_end[k]);
  }
  if (failures) {
    printf("failed: %u wrong results\n", failures);
    return 1;
  }
  printf("passed: %u threads\n", threads);
  return 0;
}
