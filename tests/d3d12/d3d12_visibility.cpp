// contract: InterlockedMax and InterlockedMin of 64 bits that ask for no old value, from every thread of a dispatch
// at once, leave each word the extreme of what was put in ("the max of the destination and the value", HLSL
// InterlockedMax). this is how Unreal's Nanite writes its visibility buffer: each thread of its rasterizer walks the
// pixels of its triangles and raises each one's texel of an R32G32_UINT texture, a 64-bit depth and triangle, so
// millions of writes meet on a million texels; a translator that locks each word stalls there for seconds and loses
// writes. here every thread raises PIXELS texels and lowers as many words of a buffer, at places and with values
// that are hashes of its number and the step, whose halves order differently, so a maximum of the halves apart is
// wrong; the reference does the same on the CPU
#include "d3d12_test.hpp"
#include <algorithm>
#include <cstdint>

static const char hlsl[] = R"hlsl(
RWStructuredBuffer<uint64_t> words : register(u0);
RWTexture2D<uint64_t> texels : register(u1);
[numthreads(GROUP, 1, 1)]
void cs(uint t : SV_DispatchThreadID) {
  for (uint i = 0; i < PIXELS; i++) {
    uint64_t v = uint64_t(t * PIXELS + i + 1) * MULTIPLIER;
    uint at = uint(v >> 40) % (SIDE * SIDE);
    InterlockedMax(texels[uint2(at % SIDE, at / SIDE)], v);
    InterlockedMin(words[at], v);
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
    printf("skipped: 64-bit atomics are DXIL only\n");
    return 77;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
  D3D12_FEATURE_DATA_D3D12_OPTIONS9 options9{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1)));
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9, &options9, sizeof(options9)));
  if (!options1.Int64ShaderOps || !options9.AtomicInt64OnTypedResourceSupported) {
    printf("skipped: no 64-bit atomics on typed resources\n");
    return 77;
  }
  // a target of SIDE by SIDE, and sixteen writes for each of its pixels
  const UINT group = D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP, side = 1024, pixels = 64, threads = side * side * 16 / pixels;
  const uint64_t multiplier = 0x9e3779b97f4a7c15;
  auto cs = compiler.compile(
      hlsl, "cs", "cs_6_6",
      {"GROUP=" + std::to_string(group), "SIDE=" + std::to_string(side), "PIXELS=" + std::to_string(pixels),
       "MULTIPLIER=" + std::to_string(multiplier) + "ull"}
  );
  if (cs.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  D3D12_DESCRIPTOR_RANGE texel_range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1};
  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_UAV}, {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}};
  params[1].DescriptorTable = {1, &texel_range};
  auto rs = root_signature(device.Get(), {2, params});
  D3D12_COMPUTE_PIPELINE_STATE_DESC cs_desc{rs.Get(), bytecode(cs)};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateComputePipelineState(&cs_desc, IID_PPV_ARGS(&pso)));

  // the texels start at the least value and the words at the greatest
  const UINT words = side * side;
  const UINT64 bytes = UINT64(words) * 8, pitch = side * 8;
  static_assert(1024 * 8 % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT == 0 && 1024 * 1024 * 8 % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT == 0);
  auto memory = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 2 * bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12Resource> texture;
  D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC texture_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, side, side, 1, 1, DXGI_FORMAT_R32G32_UINT, {1, 0},
                                   D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
  CHECK(device->CreateCommittedResource(
      &default_heap, D3D12_HEAP_FLAG_NONE, &texture_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)
  ));
  ComPtr<ID3D12DescriptorHeap> heap;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)));
  D3D12_UNORDERED_ACCESS_VIEW_DESC texel_view{DXGI_FORMAT_R32G32_UINT, D3D12_UAV_DIMENSION_TEXTURE2D};
  device->CreateUnorderedAccessView(texture.Get(), nullptr, &texel_view, heap->GetCPUDescriptorHandleForHeapStart());
  void *mapped;
  CHECK(upload->Map(0, nullptr, &mapped));
  memset(mapped, 0xff, bytes);
  auto zeros = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  CHECK(zeros->Map(0, nullptr, &mapped));
  memset(mapped, 0, bytes);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  D3D12_TEXTURE_COPY_LOCATION rows, texel_rows{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  list->CopyBufferRegion(memory.Get(), 0, upload.Get(), 0, bytes);
  transition(list.Get(), memory.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  rows = {zeros.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  rows.PlacedFootprint = {0, {DXGI_FORMAT_R32G32_UINT, side, side, 1, (UINT)pitch}};
  list->CopyTextureRegion(&texel_rows, 0, 0, 0, &rows, nullptr);
  transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  list->SetDescriptorHeaps(1, heap.GetAddressOf());
  list->SetComputeRootSignature(rs.Get());
  list->SetComputeRootUnorderedAccessView(0, memory->GetGPUVirtualAddress());
  list->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
  list->Dispatch(threads / group, 1, 1);
  transition(list.Get(), memory.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(), 0, memory.Get(), 0, bytes);
  rows = {readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  rows.PlacedFootprint = {bytes, {DXGI_FORMAT_R32G32_UINT, side, side, 1, (UINT)pitch}};
  list->CopyTextureRegion(&rows, 0, 0, 0, &texel_rows, nullptr);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  uint64_t *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));

  // each place's extreme, of the values the threads put there
  std::vector<uint64_t> lowest(words, ~uint64_t(0)), highest(words, 0);
  for (uint64_t n = 1; n <= uint64_t(threads) * pixels; n++) {
    // HLSL's uint(t * PIXELS + i + 1): the count wraps at 32 bits, which it stays under
    uint64_t v = n * multiplier, at = uint32_t(v >> 40) % words;
    highest[at] = std::max(highest[at], v);
    lowest[at] = std::min(lowest[at], v);
  }
  unsigned failures = 0;
  for (UINT i = 0; i < words; i++) {
    if (got[i] != lowest[i] && failures++ < 12)
      printf("buffer min of word %u: %#llx, want %#llx\n", i, (unsigned long long)got[i], (unsigned long long)lowest[i]);
    if (got[words + i] != highest[i] && failures++ < 12)
      printf("texel max at %u: %#llx, want %#llx\n", i, (unsigned long long)got[words + i], (unsigned long long)highest[i]);
  }
  if (failures) {
    printf("failed: %u wrong results\n", failures);
    return 1;
  }
  printf("passed: %u threads, %u writes each, on %u texels and as many words\n", threads, pixels, words);
  return 0;
}
