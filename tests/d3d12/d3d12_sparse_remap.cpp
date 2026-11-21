// contract: a remapped buffer reads its new heap tiles, and a NULL mapping reads zero, including after earlier reads.
// "Reads from non-mapped tiles return 0 in all non-missing components of the format" (D3D11.3 5.9.7.2).
// "The updates are applied from first region to last, so if regions overlap in a single call, the updates later
// in the list overwrite the areas overlapping with previous updates." (D3D11.3 5.9.3.1).
// D3D12 UpdateTileMappings permits tiles from multiple heaps; CopyTileMappings copies mappings, not contents.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-updatetilemappings
// CopyTileMappings: "The source and destination regions can overlap", "as if the source was saved to a temporary
// location" (Microsoft Learn).
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-copytilemappings
// "Mappings start off all NULL when a resource is initially created." (D3D12 UpdateTileMappings).
// "returns FALSE if any values were taken from an unmapped tile." (Microsoft Learn, CheckAccessFullyMapped).
// https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/checkaccessfullymapped
// "ClearUnorderedAccessViewUint(...) clears a UAV with bit-precise values" (D3D11.3 5.2.3.2); an R32_UINT
// clear must survive a buffer copy from the same mapped tiles, independently of shader loads.
// "copying the lower ni bits from each array element i to the corresponding channel, where ni is the number of
// bits in the ith channel of the resource Format" (D3D11.3 5.2.3.2) defines the packed Uint expectations below.
// "Writes to non-mapped tiles are stopped from going to memory" (D3D11.3 5.9.7.2): a NULL clear must leave the
// neighbouring heap tile unchanged. "It only works on FLOAT, UNORM, and SNORM UAVs, with format conversion from
// FLOAT to *NORM where appropriate" (5.2.3.2). Float cases use 0 and 1: UNORM's "all 1's means 1.0f", SNORM's
// "maximum value means 1.0f" (3.2.3.1); FLOAT encodings follow 3.1.3, 3.1.5 and 3.1.6. Formats and widths come
// from the SDK through copy_formats.hpp. Rectangles are "rectangles in the resource view to clear" (Microsoft
// Learn, ID3D12GraphicsCommandList::ClearUnorderedAccessViewUint, pRects).
// physical word i, numbering heap B after heap A, holds round * heap_words + i + 1, except each tile's middle word
// is zero. the reference keeps the heap tile each virtual tile names. reads sample both ends and the middle,
// with residency independent of content.
#include "d3d12_test.hpp"
#include "../copy_formats.hpp"
#include <algorithm>
#include <climits>
#include <random>

static const char hlsl[] = R"hlsl(
ByteAddressBuffer bytes : register(t0);
StructuredBuffer<uint> words : register(t1);
struct Result { uint4 value; uint mapped; };
RWStructuredBuffer<Result> result : register(u0);
cbuffer Limits : register(b0) { uint visible_tiles; };
uint address(uint i, uint width) {
  uint probe = i % PROBES;
  uint last = i / PROBES + 1 == visible_tiles ? width : 1;
  return (i / PROBES) * TILE_WORDS + (probe == 0 ? 0 : probe == 1 ? TILE_WORDS / 2 : TILE_WORDS - last);
}
[numthreads(1, 1, 1)] void raw(uint3 position : SV_DispatchThreadID) {
  uint status;
  Result r;
  r.value = uint4(bytes.Load(address(position.x, 1) * WORD_BYTES, status), 0, 0, 0);
  r.mapped = CheckAccessFullyMapped(status);
  result[position.x] = r;
}
[numthreads(1, 1, 1)] void structured(uint3 position : SV_DispatchThreadID) {
  uint status;
  Result r;
  r.value = uint4(words.Load(address(position.x, 1), status), 0, 0, 0);
  r.mapped = CheckAccessFullyMapped(status);
  result[position.x] = r;
}
[numthreads(1, 1, 1)] void raw4(uint3 position : SV_DispatchThreadID) {
  uint status;
  Result r;
  r.value = bytes.Load4(address(position.x, 4) * WORD_BYTES, status);
  r.mapped = CheckAccessFullyMapped(status);
  result[position.x] = r;
}
[numthreads(1, 1, 1)] void status_only(uint3 position : SV_DispatchThreadID) {
  uint status;
  bytes.Load4(address(position.x, 4) * WORD_BYTES, status);
  Result r;
  r.value = 0;
  r.mapped = CheckAccessFullyMapped(status);
  result[position.x] = r;
}
)hlsl";

static int
typed_clears(ID3D12Device *device, ID3D12CommandQueue *queue, ID3D12CommandAllocator *allocator,
    ID3D12GraphicsCommandList *list, ID3D12DescriptorHeap *cpu, ID3D12DescriptorHeap *gpu, UINT seed) {
  const UINT tile = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES, tiles = 4, bytes = tiles * tile;
  D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_BUFFER, 0, bytes, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
      D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
  D3D12_HEAP_DESC heap_desc{bytes, {D3D12_HEAP_TYPE_DEFAULT}, 0, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS};
  ComPtr<ID3D12Heap> heap;
  ComPtr<ID3D12Resource> sparse, placed;
  CHECK(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap)));
  CHECK(device->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&sparse)));
  CHECK(device->CreatePlacedResource(heap.Get(), 0, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
      nullptr, IID_PPV_ARGS(&placed)));
  auto upload = buffer(device, D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto readback = buffer(device, D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(upload && readback, "typed clear copy buffers"))
    return 1;
  std::mt19937 random(seed);
  std::vector<uint8_t> initial(bytes), expected(bytes);
  for (auto &byte : initial)
    byte = uint8_t(random());
  void *data;
  CHECK(upload->Map(0, nullptr, &data));
  memcpy(data, initial.data(), bytes);
  upload->Unmap(0, nullptr);
  UINT widths = 0;
  bool rgb10 = false, rg11 = false;
  for (auto &format : copy_formats) {
    if (format.depth || format.unit != 1 || strstr(format.name, "TYPELESS"))
      continue;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format.id};
    CHECK(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)));
    if (!(support.Support1 & D3D12_FORMAT_SUPPORT1_BUFFER) ||
        !(support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW))
      continue;
    widths |= 1u << format.bytes;
    rgb10 |= format.id == DXGI_FORMAT_R10G10B10A2_UINT;
    rg11 |= format.id == DXGI_FORMAT_R11G11B10_FLOAT;
    random.seed(seed ^ UINT(format.id));
    const UINT elements = tile / format.bytes;
    const UINT ranges[][2] = {{1, elements - 2}, {0, elements}, {elements - 1, elements + 2},
        {elements, elements}, {0, bytes / format.bytes}};
    for (UINT mode = 0; mode < 3; mode++) {
      if (mode && !strstr(format.name, "FLOAT") && !strstr(format.name, "UNORM") && !strstr(format.name, "SNORM"))
        continue;
      const UINT color[] = {UINT(random()), UINT(random()), UINT(random()), UINT(random())};
      const float floats[] = {float(mode == 1), float(mode == 2), float(mode == 1), float(mode == 2)};
      std::vector<uint8_t> texel(format.bytes);
      UINT bit = 0;
      for (const char *field = format.family; *field;) {
        char component = *field++;
        char *next;
        UINT width = strtoul(field, &next, 10);
        field = next;
        UINT channel = component == 'R' ? 0 : component == 'G' ? 1 : component == 'B' ? 2 : 3;
        UINT value = color[channel];
        if (mode) {
          value = 0;
          if (floats[channel]) {
            if (strstr(format.name, "UNORM"))
              value = UINT((UINT64(1) << width) - 1);
            else if (strstr(format.name, "SNORM"))
              value = UINT((UINT64(1) << (width - 1)) - 1);
            else if (width == sizeof(float) * CHAR_BIT)
              memcpy(&value, &floats[channel], sizeof(value));
            else {
              // D3D11.3 3.1.5/3.1.6: five exponent bits, bias 15, a sign bit only in FLOAT16
              const UINT exponent_bits = 5, exponent_bias = (1u << (exponent_bits - 1)) - 1;
              value = exponent_bias << (width - exponent_bits - UINT(width == sizeof(uint16_t) * CHAR_BIT));
            }
          }
        }
        for (UINT b = 0; b < width; b++, bit++)
          texel[bit / CHAR_BIT] |= uint8_t(((value >> b) & 1) << (bit % CHAR_BIT));
      }
      for (UINT mapping = 0; mapping < 2; mapping++) {
        for (UINT i = 0; i < tiles; i++) {
          D3D12_TILED_RESOURCE_COORDINATE coordinate{i};
          D3D12_TILE_RANGE_FLAGS flag = mapping && i == 1 ? D3D12_TILE_RANGE_FLAG_NULL : D3D12_TILE_RANGE_FLAG_NONE;
          queue->UpdateTileMappings(sparse.Get(), 1, &coordinate, nullptr, heap.Get(), 1, &flag, &i, nullptr,
              D3D12_TILE_MAPPING_FLAG_NONE);
        }
        for (UINT range = 0; range < std::size(ranges); range++)
          for (UINT rectangles = 0; rectangles < 3; rectangles++) {
            const UINT first = ranges[range][0], count = ranges[range][1];
            step("seed %u format %s clear %u mapping %u view %u rects %u: typed clear bytes", seed, format.name,
                mode, mapping, range, rectangles);
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{format.id, D3D12_UAV_DIMENSION_BUFFER};
            uav.Buffer.FirstElement = first;
            uav.Buffer.NumElements = count;
            device->CreateUnorderedAccessView(sparse.Get(), nullptr, &uav, cpu->GetCPUDescriptorHandleForHeapStart());
            device->CreateUnorderedAccessView(sparse.Get(), nullptr, &uav, gpu->GetCPUDescriptorHandleForHeapStart());
            const D3D12_RECT edges[] = {{0, 0, 1, 1}, {LONG(count - 1), 0, LONG(count), 1}, {1, 0, 1, 1}};
            const D3D12_RECT clipped[] = {{-2, -1, LONG(count + 2), 2}, {0, 1, LONG(count), 2},
                {0, -2, LONG(count), 0}, {-3, 0, -1, 1}, {LONG(count + 1), 0, LONG(count + 2), 1}};
            const auto rects = rectangles == 1 ? edges : clipped;
            const UINT rect_count = !rectangles ? 0 :
                rectangles == 1 ? UINT(std::size(edges)) : UINT(std::size(clipped));
            expected = initial;
            for (UINT i = first; i < first + count; i++)
              if ((!mapping || i * format.bytes / tile != 1) &&
                  (rectangles != 1 || i == first || i + 1 == first + count))
                memcpy(expected.data() + i * format.bytes, texel.data(), texel.size());
            CHECK(forget(readback.Get()));
            CHECK(allocator->Reset());
            CHECK(list->Reset(allocator, nullptr));
            D3D12_RESOURCE_BARRIER barrier{D3D12_RESOURCE_BARRIER_TYPE_ALIASING};
            list->ResourceBarrier(1, &barrier);
            list->CopyBufferRegion(placed.Get(), 0, upload.Get(), 0, bytes);
            list->ResourceBarrier(1, &barrier);
            ID3D12DescriptorHeap *heaps[] = {gpu};
            list->SetDescriptorHeaps(UINT(std::size(heaps)), heaps);
            if (mode)
              list->ClearUnorderedAccessViewFloat(gpu->GetGPUDescriptorHandleForHeapStart(),
                  cpu->GetCPUDescriptorHandleForHeapStart(), sparse.Get(), floats,
                  rect_count, rectangles ? rects : nullptr);
            else
              list->ClearUnorderedAccessViewUint(gpu->GetGPUDescriptorHandleForHeapStart(),
                  cpu->GetCPUDescriptorHandleForHeapStart(), sparse.Get(), color,
                  rect_count, rectangles ? rects : nullptr);
            list->ResourceBarrier(1, &barrier);
            transition(list, placed.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->CopyBufferRegion(readback.Get(), 0, placed.Get(), 0, bytes);
            transition(list, placed.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            CHECK(submit(device, queue, list));
            CHECK(readback->Map(0, nullptr, &data));
            auto got = static_cast<uint8_t *>(data);
            auto mismatch = std::mismatch(expected.begin(), expected.end(), got);
            const auto at = mismatch.first - expected.begin();
            expect(at == bytes, "typed clear byte %zu: %#x, expected %#x", size_t(at),
                at == bytes ? 0 : got[at], at == bytes ? 0 : expected[at]);
            readback->Unmap(0, nullptr);
            if (range + 1 == std::size(ranges) && !rectangles) {
              step("seed %u format %s clear %u mapping %u: replay after remapping the NULL tile", seed, format.name,
                  mode, mapping);
              const D3D12_TILED_RESOURCE_COORDINATE coordinate{1};
              const UINT physical = coordinate.X;
              D3D12_TILE_RANGE_FLAGS flag = mapping ? D3D12_TILE_RANGE_FLAG_NONE : D3D12_TILE_RANGE_FLAG_NULL;
              queue->UpdateTileMappings(sparse.Get(), 1, &coordinate, nullptr, heap.Get(), 1, &flag, &physical, nullptr,
                  D3D12_TILE_MAPPING_FLAG_NONE);
              for (UINT i = elements; i < 2 * elements; i++)
                memcpy(expected.data() + i * format.bytes,
                    mapping ? texel.data() : initial.data() + i * format.bytes, format.bytes);
              CHECK(forget(readback.Get()));
              CHECK(execute(device, queue, list));
              CHECK(readback->Map(0, nullptr, &data));
              expect(!memcmp(data, expected.data(), bytes), "replayed clear bytes");
              readback->Unmap(0, nullptr);
              flag = mapping ? D3D12_TILE_RANGE_FLAG_NULL : D3D12_TILE_RANGE_FLAG_NONE;
              queue->UpdateTileMappings(sparse.Get(), 1, &coordinate, nullptr, heap.Get(), 1, &flag, &physical, nullptr,
                  D3D12_TILE_MAPPING_FLAG_NONE);
            }
          }
      }
    }
  }
  for (UINT width = 1; width <= sizeof(UINT[4]); width *= 2)
    expect(widths & (1u << width), "typed UAV format of %u bytes was exercised", width);
  expect(rgb10 && rg11, "both packed typed UAV formats were exercised");
  return 0;
}

int
run(int argc, char **argv) {
  const UINT tile = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES, tile_words = tile / sizeof(UINT);
  const UINT tiles = 64, probes = 3, samples = tiles * probes, seed = 1, rounds = 100;
  const UINT heap_words = tiles * tile_words;
  struct Result {
    UINT value[4], mapped;
  };
  const UINT result_bytes = samples * sizeof(Result);
  const std::vector<std::string> defines = {"PROBES=" + std::to_string(probes),
      "TILE_WORDS=" + std::to_string(tile_words), "WORD_BYTES=" + std::to_string(sizeof(UINT))};
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto raw = compiler.compile(hlsl, "raw", "cs", defines);
  auto structured = compiler.compile(hlsl, "structured", "cs", defines);
  auto raw4 = compiler.compile(hlsl, "raw4", "cs", defines);
  auto status_only = compiler.compile(hlsl, "status_only", "cs", defines);
  if (raw.empty() || structured.empty() || raw4.empty() || status_only.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
  if (!expect(options.TiledResourcesTier >= D3D12_TILED_RESOURCES_TIER_2,
      "tiled resources tier %u", options.TiledResourcesTier))
    return verdict();

  D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_BUFFER, 0, UINT64(tiles) * tile, 1, 1, 1,
      DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
  ComPtr<ID3D12Resource> sparse, alias;
  CHECK(device->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
      nullptr, IID_PPV_ARGS(&sparse)));
  CHECK(device->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
      nullptr, IID_PPV_ARGS(&alias)));
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  ComPtr<ID3D12DescriptorHeap> cpu, gpu;
  D3D12_DESCRIPTOR_HEAP_DESC views{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1};
  CHECK(device->CreateDescriptorHeap(&views, IID_PPV_ARGS(&cpu)));
  views.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  CHECK(device->CreateDescriptorHeap(&views, IID_PPV_ARGS(&gpu)));
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{DXGI_FORMAT_R32_UINT, D3D12_UAV_DIMENSION_BUFFER};
  uav.Buffer.NumElements = heap_words;
  device->CreateUnorderedAccessView(sparse.Get(), nullptr, &uav, cpu->GetCPUDescriptorHandleForHeapStart());
  device->CreateUnorderedAccessView(sparse.Get(), nullptr, &uav, gpu->GetCPUDescriptorHandleForHeapStart());
  D3D12_ROOT_PARAMETER params[4] = {{D3D12_ROOT_PARAMETER_TYPE_SRV}, {D3D12_ROOT_PARAMETER_TYPE_SRV},
      {D3D12_ROOT_PARAMETER_TYPE_UAV}, {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}};
  params[1].Descriptor.ShaderRegister = 1;
  params[3].Constants.Num32BitValues = 1;
  auto signature = root_signature(device.Get(), {UINT(std::size(params)), params});
  if (!expect(bool(signature), "root signature"))
    return verdict();
  ComPtr<ID3D12PipelineState> pipelines[4];
  D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{signature.Get(), bytecode(raw)};
  CHECK(device->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&pipelines[0])));
  pipeline_desc.CS = bytecode(structured);
  CHECK(device->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&pipelines[1])));
  pipeline_desc.CS = bytecode(raw4);
  CHECK(device->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&pipelines[2])));
  pipeline_desc.CS = bytecode(status_only);
  CHECK(device->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&pipelines[3])));
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, desc.Width, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto output = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, result_bytes,
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, result_bytes,
      D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(upload && output && readback, "copy and result buffers"))
    return verdict();
  CHECK(list->Close());
  if (typed_clears(device.Get(), queue.Get(), allocator.Get(), list.Get(), cpu.Get(), gpu.Get(), seed))
    return 1;
  device->CreateUnorderedAccessView(sparse.Get(), nullptr, &uav, cpu->GetCPUDescriptorHandleForHeapStart());
  device->CreateUnorderedAccessView(sparse.Get(), nullptr, &uav, gpu->GetCPUDescriptorHandleForHeapStart());

  std::mt19937 random(seed);
  std::vector<UINT> mapping(tiles, ~0u);
  D3D12_TILED_RESOURCE_COORDINATE origin{};
  D3D12_TILE_REGION_SIZE whole{tiles};
  for (UINT round = 0; round < rounds + 2; round++) {
    step("seed %u round %u: replace the heap and initialize every physical word", seed, round);
    D3D12_HEAP_DESC heap_desc{desc.Width, {D3D12_HEAP_TYPE_DEFAULT}, 0, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS};
    ComPtr<ID3D12Heap> heap;
    ComPtr<ID3D12Resource> placed;
    CHECK(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap)));
    CHECK(device->CreatePlacedResource(heap.Get(), 0, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&placed)));
    UINT *data;
    CHECK(upload->Map(0, nullptr, (void **)&data));
    for (UINT i = 0; i < heap_words; i++)
      data[i] = i % tile_words == tile_words / 2 ? 0 : round * heap_words + i + 1;
    upload->Unmap(0, nullptr);
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), nullptr));
    list->CopyBufferRegion(placed.Get(), 0, upload.Get(), 0, desc.Width);
    transition(list.Get(), placed.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));

    if (round == 2) {
      step("seed %u round %u: typed UAV clear and copy across mapped tile boundaries", seed, round);
      queue->UpdateTileMappings(sparse.Get(), 1, nullptr, nullptr, heap.Get(), 1, nullptr, nullptr, nullptr,
          D3D12_TILE_MAPPING_FLAG_NONE);
      CHECK(forget(readback.Get()));
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), nullptr));
      D3D12_RESOURCE_BARRIER barrier{D3D12_RESOURCE_BARRIER_TYPE_ALIASING};
      list->ResourceBarrier(1, &barrier);
      transition(list.Get(), sparse.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      ID3D12DescriptorHeap *heaps[] = {gpu.Get()};
      list->SetDescriptorHeaps(UINT(std::size(heaps)), heaps);
      const UINT clear[] = {seed, 0, 0, 0};
      list->ClearUnorderedAccessViewUint(gpu->GetGPUDescriptorHandleForHeapStart(),
          cpu->GetCPUDescriptorHandleForHeapStart(), sparse.Get(), clear, 0, nullptr);
      transition(list.Get(), sparse.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      for (UINT i = 0; i < samples; i++) {
        UINT offset = (i / probes) * tile_words +
            (i % probes == 0 ? 0 : i % probes == 1 ? tile_words / 2 : tile_words - 1);
        list->CopyBufferRegion(readback.Get(), i * sizeof(UINT), sparse.Get(), offset * sizeof(UINT), sizeof(UINT));
      }
      transition(list.Get(), sparse.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      CHECK(readback->Map(0, nullptr, (void **)&data));
      for (UINT i = 0; i < samples; i++)
        expect(data[i] == clear[0], "typed clear tile %u probe %u: %#x, expected %#x",
            i / probes, i % probes, data[i], clear[0]);
      readback->Unmap(0, nullptr);
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), nullptr));
      list->ResourceBarrier(1, &barrier);
      transition(list.Get(), placed.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
          D3D12_RESOURCE_STATE_COPY_DEST);
      list->CopyBufferRegion(placed.Get(), 0, upload.Get(), 0, desc.Width);
      transition(list.Get(), placed.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
    }

    step("seed %u round %u: NULL, sequential, shared, skipped and overlapping mappings", seed, round);
    const D3D12_TILE_RANGE_FLAGS null = D3D12_TILE_RANGE_FLAG_NULL;
    if (round)
      queue->UpdateTileMappings(sparse.Get(), 1, nullptr, nullptr, nullptr, 1, &null, nullptr, nullptr,
          D3D12_TILE_MAPPING_FLAG_NONE);
    std::fill(mapping.begin(), mapping.end(), ~0u);
    for (UINT change = 0; round >= 2 && change < tiles; change++) {
      UINT start = random() % tiles, count = 1 + random() % (tiles - start);
      UINT physical = random() % (tiles - count + 1);
      D3D12_TILED_RESOURCE_COORDINATE coordinate{start};
      D3D12_TILE_REGION_SIZE region{count, BOOL(change & 1), count, 1, 1};
      D3D12_TILE_RANGE_FLAGS flag = D3D12_TILE_RANGE_FLAG_NONE;
      if (change % 4 == 1)
        flag = D3D12_TILE_RANGE_FLAG_NULL;
      else if (change % 4 == 2)
        flag = D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE;
      else if (change % 4 == 3)
        flag = D3D12_TILE_RANGE_FLAG_SKIP;
      queue->UpdateTileMappings(sparse.Get(), 1, &coordinate, &region, heap.Get(), 1, &flag, &physical, &count,
          D3D12_TILE_MAPPING_FLAG_NONE);
      for (UINT i = start; i < start + count; i++)
        if (flag != D3D12_TILE_RANGE_FLAG_SKIP)
          mapping[i] = flag == D3D12_TILE_RANGE_FLAG_NULL ? ~0u :
              flag == D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE ? physical : physical + i - start;
    }
    step("seed %u round %u: two overlapping regions in one call, last region wins", seed, round);
    const D3D12_TILED_RESOURCE_COORDINATE overlap[] = {{0}, {0}, {1}};
    const UINT first = random() % tiles;
    const UINT distinct[] = {first, (first + 1 + UINT(random() % (tiles - 1))) % tiles, 0};
    const D3D12_TILE_RANGE_FLAGS flags[] = {D3D12_TILE_RANGE_FLAG_NONE, D3D12_TILE_RANGE_FLAG_NONE,
        D3D12_TILE_RANGE_FLAG_NULL};
    if (round >= 2) {
      queue->UpdateTileMappings(sparse.Get(), UINT(std::size(overlap)), overlap, nullptr, heap.Get(),
          UINT(std::size(distinct)), flags, distinct, nullptr, D3D12_TILE_MAPPING_FLAG_NONE);
      mapping[0] = distinct[1];
      mapping[1] = ~0u;
      step("seed %u round %u: copy overlapping mappings in both directions", seed, round);
      const D3D12_TILED_RESOURCE_COORDINATE next{1};
      const D3D12_TILE_REGION_SIZE shifted{tiles - 1};
      queue->CopyTileMappings(sparse.Get(), &next, sparse.Get(), &origin, &shifted, D3D12_TILE_MAPPING_FLAG_NONE);
      memmove(mapping.data() + 1, mapping.data(), (tiles - 1) * sizeof(mapping[0]));
      queue->CopyTileMappings(sparse.Get(), &origin, sparse.Get(), &next, &shifted, D3D12_TILE_MAPPING_FLAG_NONE);
      memmove(mapping.data(), mapping.data() + 1, (tiles - 1) * sizeof(mapping[0]));
    }

    ComPtr<ID3D12Heap> other_heap;
    ComPtr<ID3D12Resource> other_placed;
    for (UINT phase = 0; phase < (round + 1 == rounds + 2 ? 3u : 1u); phase++) {
      if (phase == 1) {
        step("seed %u round %u: map heap B without clearing heap A", seed, round);
        auto other_desc = desc;
        other_desc.Width = tile;
        heap_desc.SizeInBytes = other_desc.Width;
        CHECK(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&other_heap)));
        CHECK(device->CreatePlacedResource(other_heap.Get(), 0, &other_desc, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&other_placed)));
        CHECK(upload->Map(0, nullptr, (void **)&data));
        for (UINT i = 0; i < tile_words; i++)
          data[i] = i == tile_words / 2 ? 0 : round * heap_words + heap_words + i + 1;
        upload->Unmap(0, nullptr);
        CHECK(allocator->Reset());
        CHECK(list->Reset(allocator.Get(), nullptr));
        list->CopyBufferRegion(other_placed.Get(), 0, upload.Get(), 0, other_desc.Width);
        CHECK(submit(device.Get(), queue.Get(), list.Get()));
        const D3D12_TILED_RESOURCE_COORDINATE second{1};
        queue->UpdateTileMappings(sparse.Get(), 1, &second, nullptr, other_heap.Get(), 1, nullptr,
            nullptr, nullptr, D3D12_TILE_MAPPING_FLAG_NONE);
        mapping[second.X] = tiles;
      } else if (phase == 2) {
        step("seed %u round %u: release heap A, read only heap B", seed, round);
        placed.Reset();
        heap.Reset();
      }
      if (phase != 2)
        queue->CopyTileMappings(alias.Get(), &origin, sparse.Get(), &origin, &whole, D3D12_TILE_MAPPING_FLAG_NONE);
      const UINT read_samples = phase == 2 ? probes : samples;
      CHECK(forget(upload.Get()));
      for (UINT resource = 0; resource < (phase == 2 ? 2u : 3u); resource++)
        for (UINT variant = 0; variant < std::size(pipelines); variant++) {
          step("seed %u round %u phase %u resource %u variant %u: read tile boundaries and residency",
              seed, round, phase, resource, variant);
          CHECK(forget(readback.Get()));
          CHECK(allocator->Reset());
          CHECK(list->Reset(allocator.Get(), pipelines[variant].Get()));
          transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
          list->CopyBufferRegion(output.Get(), 0, upload.Get(), 0, result_bytes);
          transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          D3D12_RESOURCE_BARRIER barrier{D3D12_RESOURCE_BARRIER_TYPE_ALIASING};
          list->ResourceBarrier(1, &barrier);
          list->SetComputeRootSignature(signature.Get());
          list->SetComputeRoot32BitConstant(3, phase == 2 ? 1 : tiles, 0);
          auto address = (resource == 2 ? placed : resource ? alias : sparse)->GetGPUVirtualAddress() +
              (phase == 2 ? tile : 0);
          list->SetComputeRootShaderResourceView(0, address);
          list->SetComputeRootShaderResourceView(1, address);
          list->SetComputeRootUnorderedAccessView(2, output->GetGPUVirtualAddress());
          list->Dispatch(read_samples, 1, 1);
          transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
          list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, result_bytes);
          transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          CHECK(submit(device.Get(), queue.Get(), list.Get()));
          Result *got;
          CHECK(readback->Map(0, nullptr, (void **)&got));
          for (UINT i = 0; i < read_samples; i++) {
            UINT width = variant >= 2 ? UINT(std::size(got[i].value)) : 1;
            UINT last = i / probes + 1 == (phase == 2 ? 1 : tiles) ? width : 1;
            UINT offset = i % probes == 0 ? 0 : i % probes == 1 ? tile_words / 2 : tile_words - last;
            UINT virtual_tile = phase == 2 ? 1 : i / probes;
            bool mapped = true;
            for (UINT c = 0; c < std::size(got[i].value); c++) {
              UINT word = virtual_tile * tile_words + offset + c;
              UINT physical = c < width ? (resource == 2 ? word / tile_words : mapping[word / tile_words]) : ~0u;
              UINT want = c >= width || variant == 3 || physical == ~0u || word % tile_words == tile_words / 2 ? 0 :
                  round * heap_words + physical * tile_words + word % tile_words + 1;
              expect(got[i].value[c] == want, "tile %u word %u component %u: %#x, expected %#x",
                  virtual_tile, offset, c, got[i].value[c], want);
              if (c < width)
                mapped &= physical != ~0u;
            }
            expect(got[i].mapped == UINT(mapped), "tile %u word %u: residency %u, expected %u",
                virtual_tile, offset, got[i].mapped, UINT(mapped));
          }
          readback->Unmap(0, nullptr);
        }
    }
  }
  return verdict();
}

int
main(int argc, char **argv) {
  auto done = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  if (!expect(done != nullptr, "watchdog event"))
    return verdict();
  auto watchdog = CreateThread(nullptr, 0, [](void *event) WINAPI -> DWORD {
    if (WaitForSingleObject(event, 120000) != WAIT_OBJECT_0) {
      fprintf(stderr, "failed: sparse remap timed out; last step is in the trace\n");
      TerminateProcess(GetCurrentProcess(), 1);
    }
    return 0;
  }, done, 0, nullptr);
  if (!expect(watchdog != nullptr, "watchdog thread"))
    return verdict();
  int result = run(argc, argv);
  SetEvent(done);
  expect(WaitForSingleObject(watchdog, 120000) == WAIT_OBJECT_0, "watchdog ended");
  CloseHandle(watchdog);
  CloseHandle(done);
  return result;
}
