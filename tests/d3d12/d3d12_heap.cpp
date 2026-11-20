// contract: SM 6.6 shaders reach resources through the descriptor heaps themselves (ResourceDescriptorHeap,
// SamplerDescriptorHeap) under a root signature that directly indexes them, each resource typed where it is used:
// constant, typed, structured and raw buffers, a depth texture compared through a comparison sampler, and UAVs (a
// typed texture, and a structured buffer with a counter), all found by indices a bound root constant supplies. every
// value read comes back through a UAV from the heap.
// "If the index is not uniform, you must use the NonUniformResourceIndex intrinsic on the index."
// "must both be considered volatile" (DirectX-Specs, HLSL_SM_6_6_DynamicResources, indexing and volatility).
// per-lane indices, heap changes and descriptor rewrites therefore retain the selected bytes and comparison state.
#include "d3d12_test.hpp"
#include <array>
#include <bit>
#include <climits>
#include <cstdint>

static const char hlsl[] = R"hlsl(
cbuffer c : register(b0) { uint first; };
struct Data { uint value; };
[numthreads(1, 1, 1)]
void cs() {
  ConstantBuffer<Data> data = ResourceDescriptorHeap[first + 0];
  Buffer<uint> typed = ResourceDescriptorHeap[first + 1];
  StructuredBuffer<uint> structured = ResourceDescriptorHeap[first + 2];
  Texture2D<float> depth = ResourceDescriptorHeap[first + 3];
  RWStructuredBuffer<uint> result = ResourceDescriptorHeap[first + 4];
  RWByteAddressBuffer raw = ResourceDescriptorHeap[first + 5];
  RWTexture2D<uint> image = ResourceDescriptorHeap[first + 6];
  RWStructuredBuffer<uint> counted = ResourceDescriptorHeap[first + 7];
  SamplerComparisonState less = SamplerDescriptorHeap[0];
  result[0] = data.value;
  result[1] = typed[1];
  result[2] = structured[2];
  result[3] = depth.SampleCmpLevelZero(less, float2(0.5, 0.5), BELOW) > 0.5;
  result[4] = depth.SampleCmpLevelZero(less, float2(0.5, 0.5), ABOVE) > 0.5;
  raw.Store(4, data.value + 1);
  image[uint2(1, 0)] = data.value + 2;
  result[5] = image[uint2(1, 0)];
  result[6] = counted.IncrementCounter();
}

cbuffer varying : register(b1) { uint resource_base, sampler_base, rotation; };
RWStructuredBuffer<uint> observed : register(u0);
struct Pair { uint x, y; };
#ifdef COHERENT
#define MEMORY_COHERENT globallycoherent
#else
#define MEMORY_COHERENT
#endif
[numthreads(GROUP, 1, 1)] void indexed(uint t : SV_GroupIndex) {
  uint choice = (t + rotation) % VARIANTS;
  uint base = resource_base + choice * DESCRIPTORS;
  ConstantBuffer<Data> data = ResourceDescriptorHeap[NonUniformResourceIndex(base)];
  Buffer<uint> typed = ResourceDescriptorHeap[NonUniformResourceIndex(base + 1)];
  StructuredBuffer<Pair> structured = ResourceDescriptorHeap[NonUniformResourceIndex(base + 2)];
  ByteAddressBuffer bytes = ResourceDescriptorHeap[NonUniformResourceIndex(base + 3)];
  Texture2DArray<float> tex = ResourceDescriptorHeap[NonUniformResourceIndex(base + 4)];
  MEMORY_COHERENT RWByteAddressBuffer raw = ResourceDescriptorHeap[NonUniformResourceIndex(base + 5)];
  MEMORY_COHERENT RWBuffer<uint> writable = ResourceDescriptorHeap[NonUniformResourceIndex(base + 6)];
  MEMORY_COHERENT RWStructuredBuffer<Pair> pairs = ResourceDescriptorHeap[NonUniformResourceIndex(base + 7)];
  uint sampler_index = sampler_base + (t / VARIANTS + rotation) % COMPARISONS;
  SamplerComparisonState comp = SamplerDescriptorHeap[NonUniformResourceIndex(sampler_index)];
  const float references[] = {0, 0.25, 0.5, 1};
  uint value = (t + 1) * (rotation + 1);
  raw.Store(t * 4, value);
  writable[t] = value + 1;
  Pair p = {value + 2, ~value};
  pairs[t] = p;
  uint row = t * FIELDS;
  observed[row] = data.value;
  observed[row + 1] = typed[1];
  Pair read_pair = structured[1];
  observed[row + 2] = read_pair.x;
  observed[row + 3] = read_pair.y;
  uint2 read_bytes = bytes.Load2(0);
  observed[row + 4] = read_bytes.x;
  observed[row + 5] = read_bytes.y;
  observed[row + 6] = asuint(tex.Load(int4(0, 0, 0, 0)));
  observed[row + 7] = asuint(tex.SampleCmpLevelZero(comp, float3(0.5, 0.5, 0),
                                  references[t / (VARIANTS * COMPARISONS) % 4]));
  observed[row + 8] = raw.Load(t * 4);
  observed[row + 9] = writable[t];
  Pair written_pair = pairs[t];
  observed[row + 10] = written_pair.x;
  observed[row + 11] = written_pair.y;
}
)hlsl";

static constexpr UINT comparisons = D3D12_COMPARISON_FUNC_ALWAYS - D3D12_COMPARISON_FUNC_NEVER + 1;
static constexpr UINT variants = comparisons, descriptors = 8, fields = 12;
static constexpr UINT group = D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP;

static int
heap_indexing(ID3D12Device *device, ID3D12CommandQueue *queue, ID3D12CommandAllocator *allocator,
              const std::array<std::string, 2> &shaders) {
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
  if (!expect(options.ResourceBindingTier == D3D12_RESOURCE_BINDING_TIER_3, "SM6.6 heap indexing requires tier3"))
    return verdict();
  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[0].Constants = {1, 0, 3};
  auto rs = root_signature(device, {UINT(std::size(params)), params, 0, nullptr,
                                  D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                                      D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED});
  if (!expect(bool(rs), "nonuniform root signature"))
    return verdict();
  const UINT constant_words = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT / sizeof(UINT);
  auto value_at = [](UINT index, UINT word) { return (index + 1) * (group + 1) + word; };
  auto upload = buffer(device, D3D12_HEAP_TYPE_UPLOAD, 2 * variants * D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,
                       D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(bool(upload), "nonuniform inputs"))
    return verdict();
  UINT *mapped;
  CHECK(upload->Map(0, nullptr, reinterpret_cast<void **>(&mapped)));
  for (UINT i = 0; i < 2 * variants; i++)
    for (UINT word = 0; word < constant_words; word++)
      mapped[i * constant_words + word] = value_at(i, word);
  upload->Unmap(0, nullptr);
  ComPtr<ID3D12Resource> depth;
  D3D12_HEAP_PROPERTIES depth_heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC depth_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 1, 1, UINT16(variants), 1,
      DXGI_FORMAT_R32_TYPELESS, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL};
  CHECK(device->CreateCommittedResource(&depth_heap, D3D12_HEAP_FLAG_NONE, &depth_desc,
      D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&depth)));
  D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, variants};
  ComPtr<ID3D12DescriptorHeap> dsv_heap;
  CHECK(device->CreateDescriptorHeap(&dsv_heap_desc, IID_PPV_ARGS(&dsv_heap)));
  CHECK(allocator->Reset());
  ComPtr<ID3D12GraphicsCommandList> initialize;
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&initialize)));
  auto texel_at = [](UINT index) { return index % 3 == 0 ? 0.25f : index % 3 == 1 ? 0 : 1; };
  for (UINT i = 0; i < variants; i++) {
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{DXGI_FORMAT_D32_FLOAT, D3D12_DSV_DIMENSION_TEXTURE2DARRAY};
    dsv.Texture2DArray = {0, i, 1};
    auto handle = dsv_heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += i * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    device->CreateDepthStencilView(depth.Get(), &dsv, handle);
    initialize->ClearDepthStencilView(handle, D3D12_CLEAR_FLAG_DEPTH, texel_at(i), 0, 0, nullptr);
  }
  transition(initialize.Get(), depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  CHECK(submit(device, queue, initialize.Get()));
  std::array<ComPtr<ID3D12DescriptorHeap>, 2> resources, samplers;
  const UINT resource_origin = UINT8_MAX, sampler_origin = UINT8_MAX - comparisons / 2;
  for (UINT i = 0; i < resources.size(); i++) {
    D3D12_DESCRIPTOR_HEAP_DESC resource_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        resource_origin + UINT(resources.size()) + variants * descriptors, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    D3D12_DESCRIPTOR_HEAP_DESC sampler_desc{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
        sampler_origin + UINT(samplers.size()) + comparisons, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    CHECK(device->CreateDescriptorHeap(&resource_desc, IID_PPV_ARGS(&resources[i])));
    CHECK(device->CreateDescriptorHeap(&sampler_desc, IID_PPV_ARGS(&samplers[i])));
  }
  const UINT resource_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  const UINT sampler_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  const UINT guard = D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT / sizeof(UINT);
  const UINT row_words = group + 2 * guard, output_words = (group + 2) * fields;
  const UINT memory_words[] = {variants * row_words, variants * row_words, variants * row_words * 2};
  std::array<ComPtr<ID3D12Resource>, 3> memory;
  for (UINT i = 0; i < memory.size(); i++)
    memory[i] = buffer(device, D3D12_HEAP_TYPE_DEFAULT, memory_words[i] * sizeof(UINT),
                       D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto output = buffer(device, D3D12_HEAP_TYPE_DEFAULT, output_words * sizeof(UINT), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  const UINT total_words = output_words + memory_words[0] + memory_words[1] + memory_words[2];
  auto initial = buffer(device, D3D12_HEAP_TYPE_UPLOAD, total_words * sizeof(UINT), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto readback = buffer(device, D3D12_HEAP_TYPE_READBACK, total_words * sizeof(UINT), D3D12_RESOURCE_STATE_COPY_DEST);
  auto indirect = buffer(device, D3D12_HEAP_TYPE_UPLOAD, sizeof(D3D12_DISPATCH_ARGUMENTS), D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(output && initial && readback && indirect && memory[0] && memory[1] && memory[2], "nonuniform buffers"))
    return verdict();
  CHECK(initial->Map(0, nullptr, reinterpret_cast<void **>(&mapped)));
  memset(mapped, UINT8_MAX, total_words * sizeof(UINT));
  initial->Unmap(0, nullptr);
  CHECK(indirect->Map(0, nullptr, reinterpret_cast<void **>(&mapped)));
  const D3D12_DISPATCH_ARGUMENTS dispatch{1, 1, 1};
  memcpy(mapped, &dispatch, sizeof(dispatch));
  indirect->Unmap(0, nullptr);
  D3D12_INDIRECT_ARGUMENT_DESC argument{D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH};
  D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(dispatch), 1, &argument};
  ComPtr<ID3D12CommandSignature> signature;
  CHECK(device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signature)));
  for (UINT round = 0; round <= resources.size(); round++) {
    auto &resource_heap = resources[round % resources.size()];
    auto &sampler_heap = samplers[round % samplers.size()];
    const UINT first_resource = resource_origin + round, first_sampler = sampler_origin + round;
    auto slot = [&](UINT i) {
      auto handle = resource_heap->GetCPUDescriptorHandleForHeapStart();
      handle.ptr += (first_resource + i) * resource_step;
      return handle;
    };
    for (UINT i = 0; i < variants; i++) {
      const UINT selected = (i + round) % variants, base = i * descriptors;
      const bool null = selected == variants - 1;
      D3D12_SHADER_RESOURCE_VIEW_DESC view{DXGI_FORMAT_R32_UINT, D3D12_SRV_DIMENSION_BUFFER,
                                          D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
      view.Buffer = {selected * constant_words + 1, 3};
      device->CreateShaderResourceView(null ? nullptr : upload.Get(), &view, slot(base + 1));
      view.Format = DXGI_FORMAT_UNKNOWN;
      view.Buffer = {selected * constant_words / 2, 2, 2 * sizeof(UINT)};
      device->CreateShaderResourceView(null ? nullptr : upload.Get(), &view, slot(base + 2));
      view.Format = DXGI_FORMAT_R32_TYPELESS;
      view.Buffer = {selected * constant_words + guard, 3, 0, D3D12_BUFFER_SRV_FLAG_RAW};
      device->CreateShaderResourceView(null ? nullptr : upload.Get(), &view, slot(base + 3));
      D3D12_SHADER_RESOURCE_VIEW_DESC tex{DXGI_FORMAT_R32_FLOAT, D3D12_SRV_DIMENSION_TEXTURE2DARRAY,
          D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
      tex.Texture2DArray = {0, 1, selected, 1};
      device->CreateShaderResourceView(null ? nullptr : depth.Get(), &tex, slot(base + 4));
      for (UINT kind = 0; kind < memory.size(); kind++) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{kind == 0 ? DXGI_FORMAT_R32_TYPELESS :
            kind == 1 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_UNKNOWN, D3D12_UAV_DIMENSION_BUFFER};
        uav.Buffer = {selected * row_words + guard, group, kind == 2 ? UINT(2 * sizeof(UINT)) : 0, 0,
                      kind == 0 ? D3D12_BUFFER_UAV_FLAG_RAW : D3D12_BUFFER_UAV_FLAG_NONE};
        device->CreateUnorderedAccessView(null ? nullptr : memory[kind].Get(), nullptr, &uav, slot(base + 5 + kind));
      }
    }
    auto rewrite = [&](bool replacement) {
      for (UINT i = 0; i < variants; i++) {
        const UINT selected = (i + round) % variants;
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{selected == variants - 1 ? 0 : upload->GetGPUVirtualAddress() +
            (selected + (replacement ? variants : 0)) * D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,
            D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT};
        device->CreateConstantBufferView(&cbv, slot(i * descriptors));
      }
      for (UINT i = 0; i < comparisons; i++) {
        D3D12_SAMPLER_DESC sampler{D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                  D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP};
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC(D3D12_COMPARISON_FUNC_NEVER + (i + round + replacement) % comparisons);
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        auto handle = sampler_heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += (first_sampler + i) * sampler_step;
        device->CreateSampler(&sampler, handle);
      }
    };
    for (UINT coherent = 0; coherent < shaders.size(); coherent++) {
      D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{rs.Get(), bytecode(shaders[coherent])};
      ComPtr<ID3D12PipelineState> pso;
      CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)));
      for (UINT rotation : {0u, 1u, variants - 1})
        for (bool execute_indirect : {false, true}) {
          step("nonuniform heap round %u, coherent %u, rotation %u, %s", round, coherent, rotation,
               execute_indirect ? "indirect" : "direct");
          rewrite(false);
          CHECK(allocator->Reset());
          ComPtr<ID3D12GraphicsCommandList> list;
          CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, pso.Get(), IID_PPV_ARGS(&list)));
          list->CopyBufferRegion(output.Get(), 0, initial.Get(), 0, output_words * sizeof(UINT));
          transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          for (UINT kind = 0; kind < memory.size(); kind++) {
            list->CopyBufferRegion(memory[kind].Get(), 0, initial.Get(), 0, memory_words[kind] * sizeof(UINT));
            transition(list.Get(), memory[kind].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          }
          ID3D12DescriptorHeap *previous_heaps[] = {resources[(round + 1) % resources.size()].Get(),
                                                    samplers[(round + 1) % samplers.size()].Get()};
          list->SetDescriptorHeaps(UINT(std::size(previous_heaps)), previous_heaps);
          list->SetComputeRootSignature(rs.Get());
          ID3D12DescriptorHeap *heaps[] = {resource_heap.Get(), sampler_heap.Get()};
          list->SetDescriptorHeaps(UINT(std::size(heaps)), heaps);
          list->SetComputeRootSignature(rs.Get());
          const UINT constants[] = {first_resource, first_sampler, rotation};
          list->SetComputeRoot32BitConstants(0, UINT(std::size(constants)), constants, 0);
          list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress() + fields * sizeof(UINT));
          if (execute_indirect)
            list->ExecuteIndirect(signature.Get(), 1, indirect.Get(), 0, nullptr, 0);
          else
            list->Dispatch(1, 1, 1);
          transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
          list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, output_words * sizeof(UINT));
          transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
          UINT cursor = output_words;
          for (UINT kind = 0; kind < memory.size(); kind++) {
            transition(list.Get(), memory[kind].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->CopyBufferRegion(readback.Get(), cursor * sizeof(UINT), memory[kind].Get(), 0, memory_words[kind] * sizeof(UINT));
            transition(list.Get(), memory[kind].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            cursor += memory_words[kind];
          }
          // volatile descriptors are read when the recorded work executes, after these CPU replacements.
          CHECK(list->Close());
          rewrite(true);
          CHECK(execute(device, queue, list.Get()));
          std::vector<UINT> expected(total_words, UINT_MAX);
          const float references[] = {0, 0.25f, 0.5f, 1};
          for (UINT t = 0; t < group; t++) {
            const UINT selected = ((t + rotation) % variants + round) % variants;
            const bool null = selected == variants - 1;
            const float texel = texel_at(selected);
            const float reference = references[t / (variants * comparisons) % std::size(references)];
            const UINT comparison = D3D12_COMPARISON_FUNC_NEVER + ((t / variants + rotation) % comparisons + round + 1) % comparisons;
            bool compared = false;
            switch (comparison) {
            case D3D12_COMPARISON_FUNC_NEVER: break;
            case D3D12_COMPARISON_FUNC_LESS: compared = reference < texel; break;
            case D3D12_COMPARISON_FUNC_EQUAL: compared = reference == texel; break;
            case D3D12_COMPARISON_FUNC_LESS_EQUAL: compared = reference <= texel; break;
            case D3D12_COMPARISON_FUNC_GREATER: compared = reference > texel; break;
            case D3D12_COMPARISON_FUNC_NOT_EQUAL: compared = reference != texel; break;
            case D3D12_COMPARISON_FUNC_GREATER_EQUAL: compared = reference >= texel; break;
            case D3D12_COMPARISON_FUNC_ALWAYS: compared = true; break;
            }
            const UINT value = (t + 1) * (rotation + 1);
            const UINT values[] = {value_at(selected + variants, 0), value_at(selected, 2), value_at(selected, 2),
                value_at(selected, 3), value_at(selected, guard), value_at(selected, guard + 1), std::bit_cast<UINT>(texel),
                std::bit_cast<UINT>(float(compared)), value, value + 1, value + 2, ~value};
            for (UINT f = 0; f < fields; f++)
              expected[(t + 1) * fields + f] = null ? 0 : values[f];
            if (null)
              continue;
            UINT cursor = output_words;
            for (UINT kind = 0; kind < memory.size(); kind++) {
              const UINT words = kind == 2 ? 2 : 1, at = cursor + (selected * row_words + t + guard) * words;
              expected[at] = value + kind;
              if (words == 2)
                expected[at + 1] = ~value;
              cursor += memory_words[kind];
            }
          }
          CHECK(readback->Map(0, nullptr, reinterpret_cast<void **>(&mapped)));
          bool correct = true;
          for (UINT i = 0; i < total_words; i++)
            if (!expect(mapped[i] == expected[i], "heap word %u: %#x, want %#x", i, mapped[i], expected[i])) {
              correct = false;
              break;
            }
          readback->Unmap(0, nullptr);
          if (!correct)
            return verdict();
        }
    }
  }
  return verdict();
}

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  if (!compiler.dxc) {
    printf("skipped: descriptor heap indexing is DXIL only\n");
    return 77;
  }
  // the depth texel, and comparison references on either side of it
  const float depth_value = 0.25f, below = 0.125f, above = 0.5f;
  const UINT first = 3, value = 0x5a5a, elements = 4, count = 7;
  const std::vector<std::string> constants = {"BELOW=" + std::to_string(below), "ABOVE=" + std::to_string(above),
      "GROUP=" + std::to_string(group), "VARIANTS=" + std::to_string(variants),
      "DESCRIPTORS=" + std::to_string(descriptors), "FIELDS=" + std::to_string(fields),
      "COMPARISONS=" + std::to_string(comparisons)};
  auto cs = compiler.compile(hlsl, "cs", "cs_6_6", constants);
  if (cs.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  std::array<std::string, 2> indexed;
  for (UINT coherent = 0; coherent < indexed.size(); coherent++) {
    auto defines = constants;
    if (coherent)
      defines.push_back("COHERENT=1");
    indexed[coherent] = compiler.compile(hlsl, "indexed", "cs_6_6", defines);
    if (indexed[coherent].empty()) {
      printf("failed: nonuniform heap HLSL did not compile\n");
      return 1;
    }
  }
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER param{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
  param.Constants.Num32BitValues = 1;
  auto rs = root_signature(
      device.Get(), {1, &param, 0, nullptr,
                     D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                         D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED}
  );
  if (!rs) {
    printf("failed: root signature\n");
    return 1;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(cs)};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)));

  // one upload buffer holds the constant buffer (256 bytes) and then the element buffer, i at element i
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 512, D3D12_RESOURCE_STATE_GENERIC_READ);
  UINT *mapped;
  CHECK(upload->Map(0, nullptr, (void **)&mapped));
  mapped[0] = value;
  mapped[1] = count;
  for (UINT i = 0; i < elements; i++)
    mapped[64 + i] = i;
  upload->Unmap(0, nullptr);
  auto out = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto raw = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  // the counter starts from the upload buffer's second dword
  auto counted = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto counter = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, 4, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  ComPtr<ID3D12Resource> depth, image;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC depth_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 4, 4, 1, 1, DXGI_FORMAT_R32_TYPELESS, {1, 0},
                                 D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL};
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&depth)
  ));

  D3D12_RESOURCE_DESC image_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 4, 1, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0},
                                 D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &image_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&image)
  ));

  ComPtr<ID3D12DescriptorHeap> resources, samplers, dsvs;
  D3D12_DESCRIPTOR_HEAP_DESC resources_desc{
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, first + 8, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
  };
  D3D12_DESCRIPTOR_HEAP_DESC samplers_desc{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  D3D12_DESCRIPTOR_HEAP_DESC dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
  CHECK(device->CreateDescriptorHeap(&resources_desc, IID_PPV_ARGS(&resources)));
  CHECK(device->CreateDescriptorHeap(&samplers_desc, IID_PPV_ARGS(&samplers)));
  CHECK(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsvs)));
  auto step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  auto slot = [&](UINT i) {
    auto h = resources->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (first + i) * step;
    return h;
  };
  D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{upload->GetGPUVirtualAddress(), 256};
  device->CreateConstantBufferView(&cbv, slot(0));
  D3D12_SHADER_RESOURCE_VIEW_DESC typed{DXGI_FORMAT_R32_UINT, D3D12_SRV_DIMENSION_BUFFER, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
  typed.Buffer = {64, elements};
  device->CreateShaderResourceView(upload.Get(), &typed, slot(1));
  D3D12_SHADER_RESOURCE_VIEW_DESC structured{DXGI_FORMAT_UNKNOWN, D3D12_SRV_DIMENSION_BUFFER, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
  structured.Buffer = {64, elements, 4};
  device->CreateShaderResourceView(upload.Get(), &structured, slot(2));
  D3D12_SHADER_RESOURCE_VIEW_DESC texture{DXGI_FORMAT_R32_FLOAT, D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
  texture.Texture2D.MipLevels = 1;
  device->CreateShaderResourceView(depth.Get(), &texture, slot(3));
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{DXGI_FORMAT_UNKNOWN, D3D12_UAV_DIMENSION_BUFFER};
  uav.Buffer = {0, 64, 4};
  device->CreateUnorderedAccessView(out.Get(), nullptr, &uav, slot(4));
  D3D12_UNORDERED_ACCESS_VIEW_DESC raw_uav{DXGI_FORMAT_R32_TYPELESS, D3D12_UAV_DIMENSION_BUFFER};
  raw_uav.Buffer = {0, 64, 0, 0, D3D12_BUFFER_UAV_FLAG_RAW};
  device->CreateUnorderedAccessView(raw.Get(), nullptr, &raw_uav, slot(5));
  D3D12_UNORDERED_ACCESS_VIEW_DESC image_uav{DXGI_FORMAT_R32_UINT, D3D12_UAV_DIMENSION_TEXTURE2D};
  device->CreateUnorderedAccessView(image.Get(), nullptr, &image_uav, slot(6));
  device->CreateUnorderedAccessView(counted.Get(), counter.Get(), &uav, slot(7));
  D3D12_SAMPLER_DESC sampler{D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                             D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP};
  sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  device->CreateSampler(&sampler, samplers->GetCPUDescriptorHandleForHeapStart());
  D3D12_DEPTH_STENCIL_VIEW_DESC dsv{DXGI_FORMAT_D32_FLOAT, D3D12_DSV_DIMENSION_TEXTURE2D};
  device->CreateDepthStencilView(depth.Get(), &dsv, dsvs->GetCPUDescriptorHandleForHeapStart());

  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 516, D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  list->CopyBufferRegion(counter.Get(), 0, upload.Get(), 4, 4);
  transition(list.Get(), counter.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  list->ClearDepthStencilView(dsvs->GetCPUDescriptorHandleForHeapStart(), D3D12_CLEAR_FLAG_DEPTH, depth_value, 0, 0, nullptr);
  transition(list.Get(), depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  ID3D12DescriptorHeap *heaps[] = {resources.Get(), samplers.Get()};
  list->SetDescriptorHeaps(2, heaps);
  list->SetComputeRootSignature(rs.Get());
  list->SetComputeRoot32BitConstants(0, 1, &first, 0);
  list->Dispatch(1, 1, 1);
  transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(list.Get(), raw.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(), 0, out.Get(), 0, 256);
  list->CopyBufferRegion(readback.Get(), 256, raw.Get(), 0, 256);
  transition(list.Get(), counter.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(), 512, counter.Get(), 0, 4);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  UINT *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  // D3D12 compares the reference against the texel: LESS passes when the reference is below it
  const UINT want[] = {value, 1, 2, below < depth_value, above < depth_value, value + 2, count};
  const char *names[] = {"constant buffer", "typed buffer", "structured buffer", "compare below",
                         "compare above",   "typed UAV",    "UAV counter"};
  unsigned failures = 0;
  for (UINT i = 0; i < std::size(want); i++)
    if (got[i] != want[i] && ++failures)
      printf("%s: %#x, want %#x\n", names[i], got[i], want[i]);
  if (got[64 + 1] != value + 1 && ++failures)
    printf("raw buffer: %#x, want %#x\n", got[64 + 1], value + 1);
  // the increment returned the old count and left the next
  if (got[128] != count + 1 && ++failures)
    printf("UAV counter after: %u, want %u\n", got[128], count + 1);
  printf("%s: %u of %zu reads wrong\n", failures ? "failed" : "passed", failures, std::size(want) + 2);
  expect(failures == 0, "uniform heap indexing: %u wrong results", failures);
  return heap_indexing(device.Get(), queue.Get(), allocator.Get(), indexed);
}
