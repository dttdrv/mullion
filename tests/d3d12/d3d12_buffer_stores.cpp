// contract: "No format conversion." and "any part that is in bounds is written correctly" (D3D11.3
// functional specification, 22.4.11); an invalid structure index writes nothing (22.4.13). DXIL.rst,
// Memory accesses: "Out-of-bounds behavior and various restrictions are preserved."
// native rawBufferStore overloads extend these moves to 16/64 bits (DXC DXIL.rst, RawBufferStore;
// https://github.com/microsoft/DirectXShaderCompiler/blob/v1.7.2308/docs/DXIL.rst).
// null descriptors discard writes (ID3D12Device::CreateUnorderedAccessView, Microsoft Learn).
// snapshots after each store prevent a later overlapping write from hiding a wrong result.
#include "d3d12_test.hpp"
#include <algorithm>
#include <array>
#include <climits>

static const char hlsl[] = R"hlsl(
StructuredBuffer<uint2> addresses : register(t0);
StructuredBuffer<uint2> values : register(t1);
cbuffer Case : register(b0) { uint case_index; }
struct Record { VALUE value;
#if PAD
  T padding[PAD];
#endif
};
RWByteAddressBuffer raw_output : register(u0);
RWStructuredBuffer<Record> structured_output : register(u1);
globallycoherent RWByteAddressBuffer coherent_raw : register(u2);
globallycoherent RWStructuredBuffer<Record> coherent_structured : register(u3);
T value(uint2 bits) {
#if HALF
  return asfloat16(uint16_t(bits.x));
#elif BITS == 16
  return T(bits.x);
#else
  return T(uint64_t(bits.x) | (uint64_t(bits.y) << 32));
#endif
}
[numthreads(1,1,1)] void cs() {
  VALUE v;
#if WIDTH == 1
  v = value(values[case_index * WIDTH]);
#else
  for (uint i = 0; i < WIDTH; i++) v[i] = value(values[case_index * WIDTH + i]);
#endif
  uint2 address = addresses[case_index];
  raw_output.Store<VALUE>(address.x, v);
  structured_output[address.y].value = v;
  coherent_raw.Store<VALUE>(address.x, v);
  coherent_structured[address.y].value = v;
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler) || !compiler.dxc) {
    printf("skipped: native buffer stores require DXC\n");
    return 77;
  }
  struct Type {
    const char *name;
    UINT bits;
    bool half;
  };
  const Type types[] = {{"uint16_t", 16}, {"int16_t", 16}, {"float16_t", 16, true}, {"uint64_t", 64}, {"int64_t", 64}};
  constexpr UINT components = 4, kinds = 4;
  struct Variant {
    Type type;
    UINT width, padding;
    std::string shader;
  };
  std::vector<Variant> variants;
  for (const auto &type : types)
    for (UINT width = 1; width <= components; width++)
      for (UINT padding : {0u, components}) {
        std::string value = type.name;
        if (width != 1)
          value += std::to_string(width);
        step("compile %s width %u padding %u stores", type.name, width, padding);
        auto code = compiler.compile(
            hlsl, "cs", "cs_6_6",
            {std::string("T=") + type.name, "VALUE=" + value, "BITS=" + std::to_string(type.bits),
             "WIDTH=" + std::to_string(width), "PAD=" + std::to_string(padding), "HALF=" + std::to_string(type.half)},
            type.bits == 16 ? std::vector<std::wstring>{L"-enable-16bit-types"} : std::vector<std::wstring>{}
        );
        if (!expect(!code.empty(), "buffer store HLSL"))
          return verdict();
        variants.push_back({type, width, padding, std::move(code)});
      }
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS1 int64{};
  D3D12_FEATURE_DATA_D3D12_OPTIONS4 native16{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &int64, sizeof(int64)));
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &native16, sizeof(native16)));
  if (!expect(int64.Int64ShaderOps && native16.Native16BitShaderOpsSupported, "native scalar capabilities"))
    return verdict();
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kinds};
  D3D12_ROOT_PARAMETER params[] = {
      {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
      {D3D12_ROOT_PARAMETER_TYPE_SRV},
      {D3D12_ROOT_PARAMETER_TYPE_SRV},
      {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}
  };
  params[0].DescriptorTable = {1, &range};
  params[1].Descriptor.ShaderRegister = 0;
  params[2].Descriptor.ShaderRegister = 1;
  params[3].Constants = {0, 0, 1};
  auto root = root_signature(device.Get(), {UINT(std::size(params)), params});
  if (!expect(bool(root), "store root signature"))
    return verdict();
  ComPtr<ID3D12DescriptorHeap> heap;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kinds, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
  };
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)));
  const UINT descriptor_size = device->GetDescriptorHandleIncrementSize(heap_desc.Type);
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  D3D12_INDIRECT_ARGUMENT_DESC argument{D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH};
  D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(D3D12_DISPATCH_ARGUMENTS), 1, &argument};
  ComPtr<ID3D12CommandSignature> signature;
  CHECK(device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signature)));
  auto indirect =
      buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(D3D12_DISPATCH_ARGUMENTS), D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(bool(indirect), "indirect store dispatch"))
    return verdict();
  void *mapped;
  CHECK(indirect->Map(0, nullptr, &mapped));
  const D3D12_DISPATCH_ARGUMENTS dispatch{1, 1, 1};
  memcpy(mapped, &dispatch, sizeof(dispatch));
  indirect->Unmap(0, nullptr);
  for (const auto &test : variants) {
    const UINT word = test.type.bits / CHAR_BIT, stride = (test.width + test.padding) * word;
    const UINT count = components + 1, first = components / 2;
    const UINT raw_origin = D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT;
    const UINT raw_bytes = count * stride / sizeof(UINT) * sizeof(UINT) - (word > sizeof(UINT) ? sizeof(UINT) : 0);
    const UINT needed = std::max((first + count + components) * stride, raw_origin + raw_bytes + components * word);
    const UINT bytes = (needed + sizeof(UINT) - 1) / sizeof(UINT) * sizeof(UINT);
    const UINT alignment = std::min(word, UINT(sizeof(UINT)));
    std::vector<UINT> offsets{0, alignment, word, raw_bytes, raw_bytes + word};
    for (UINT c = 1; c <= test.width; c++)
      offsets.push_back(raw_bytes > c * word ? raw_bytes - c * word : 0);
    if (word > sizeof(UINT))
      for (UINT c = 0; c < test.width; c++)
        offsets.push_back(raw_bytes - c * word - sizeof(UINT));
    offsets.push_back((UINT_MAX - components * word) / alignment * alignment);
    const UINT indices[] = {0, 1, count - 1, count, count + 1, UINT_MAX / stride, UINT_MAX / stride + 1, UINT_MAX};
    std::vector<uint64_t> edges{0, 1, UINT64_MAX, uint64_t(1) << (test.type.bits - 1)};
    for (UINT bit = 0; bit < test.type.bits; bit++)
      edges.push_back(uint64_t(1) << bit);
    if (test.type.half)
      edges.insert(
          edges.end(),
          {0x03ff, 0x0400, 0x7bff, 0x7c00, 0x7c01, 0x7e00, 0x8000, 0x83ff, 0x8400, 0xfbff, 0xfc00, 0xfc01, 0xfe00}
      );
    using Pair = std::array<UINT, 2>;
    std::vector<Pair> addresses, values;
    const UINT addresses_per_value = UINT(std::max(offsets.size(), std::size(indices)));
    const UINT cases = UINT(edges.size()) * addresses_per_value;
    for (UINT c = 0; c < cases; c++) {
      addresses.push_back({offsets[c % offsets.size()], indices[c % std::size(indices)]});
      for (UINT component = 0; component < test.width; component++) {
        uint64_t bits = edges[(c / addresses_per_value + component) % edges.size()];
        values.push_back({UINT(bits), UINT(bits >> (sizeof(UINT) * CHAR_BIT))});
      }
    }
    std::vector<uint8_t> start(bytes);
    for (UINT b = 0; b < bytes; b++)
      start[b] = uint8_t(b ^ (b >> CHAR_BIT) ^ UINT8_MAX);
    auto initial = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto address_input = buffer(
        device.Get(), D3D12_HEAP_TYPE_UPLOAD, addresses.size() * sizeof(Pair), D3D12_RESOURCE_STATE_GENERIC_READ
    );
    auto value_input =
        buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, values.size() * sizeof(Pair), D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!expect(initial && address_input && value_input, "store inputs"))
      return verdict();
    CHECK(initial->Map(0, nullptr, &mapped));
    memcpy(mapped, start.data(), bytes);
    initial->Unmap(0, nullptr);
    CHECK(address_input->Map(0, nullptr, &mapped));
    memcpy(mapped, addresses.data(), addresses.size() * sizeof(Pair));
    address_input->Unmap(0, nullptr);
    CHECK(value_input->Map(0, nullptr, &mapped));
    memcpy(mapped, values.data(), values.size() * sizeof(Pair));
    value_input->Unmap(0, nullptr);
    ComPtr<ID3D12PipelineState> pipeline;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{root.Get(), bytecode(test.shader)};
    CHECK(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipeline)));
    for (bool null_view : {false, true}) {
      std::array<ComPtr<ID3D12Resource>, kinds> memory;
      auto readback =
          buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, UINT64(cases) * kinds * bytes, D3D12_RESOURCE_STATE_COPY_DEST);
      if (!expect(bool(readback), "store snapshots"))
        return verdict();
      for (UINT kind = 0; kind < kinds; kind++) {
        memory[kind] = buffer(
            device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
        );
        if (!expect(bool(memory[kind]), "store destination"))
          return verdict();
        const bool structured = kind % 2;
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{
            structured ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_TYPELESS, D3D12_UAV_DIMENSION_BUFFER
        };
        view.Buffer = {
            structured ? first : raw_origin / sizeof(UINT), structured ? count : raw_bytes / UINT(sizeof(UINT)),
            structured ? stride : 0, 0, structured ? D3D12_BUFFER_UAV_FLAG_NONE : D3D12_BUFFER_UAV_FLAG_RAW
        };
        auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += kind * descriptor_size;
        device->CreateUnorderedAccessView(null_view ? nullptr : memory[kind].Get(), nullptr, &view, cpu);
      }
      for (UINT path = 0; path < 2; path++) {
        step(
            "%s width %u padding %u null %u %s", test.type.name, test.width, test.padding, unsigned(null_view),
            path ? "indirect" : "direct"
        );
        CHECK(forget(readback.Get()));
        CHECK(allocator->Reset());
        CHECK(list->Reset(allocator.Get(), pipeline.Get()));
        for (auto &resource : memory) {
          list->CopyBufferRegion(resource.Get(), 0, initial.Get(), 0, bytes);
          transition(list.Get(), resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        ID3D12DescriptorHeap *heaps[] = {heap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(root.Get());
        list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
        list->SetComputeRootShaderResourceView(1, address_input->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(2, value_input->GetGPUVirtualAddress());
        for (UINT c = 0; c < cases; c++) {
          list->SetComputeRoot32BitConstant(3, c, 0);
          if (path)
            list->ExecuteIndirect(signature.Get(), 1, indirect.Get(), 0, nullptr, 0);
          else
            list->Dispatch(1, 1, 1);
          for (UINT kind = 0; kind < kinds; kind++) {
            transition(
                list.Get(), memory[kind].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE
            );
            list->CopyBufferRegion(readback.Get(), (UINT64(c) * kinds + kind) * bytes, memory[kind].Get(), 0, bytes);
            transition(
                list.Get(), memory[kind].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS
            );
          }
        }
        for (auto &resource : memory)
          transition(list.Get(), resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        CHECK(list->Close());
        CHECK(execute(device.Get(), queue.Get(), list.Get()));
        CHECK(readback->Map(0, nullptr, &mapped));
        std::array<std::vector<uint8_t>, kinds> expected;
        expected.fill(start);
        bool correct = true;
        for (UINT c = 0; c < cases && correct; c++)
          for (UINT kind = 0; kind < kinds && correct; kind++) {
            const bool structured = kind % 2;
            for (UINT component = 0; component < test.width && !null_view; component++) {
              const auto &v = values[c * test.width + component];
              const uint64_t bits = uint64_t(v[0]) | (uint64_t(v[1]) << (sizeof(UINT) * CHAR_BIT));
              for (UINT b = 0; b < word; b++) {
                UINT64 offset = UINT64(addresses[c][0]) + component * word + b;
                if (structured ? addresses[c][1] >= count : offset >= raw_bytes)
                  continue;
                UINT64 target =
                    structured ? UINT64(first + addresses[c][1]) * stride + component * word + b : raw_origin + offset;
                expected[kind][target] = uint8_t(bits >> (b * CHAR_BIT));
              }
            }
            const auto *actual = static_cast<const uint8_t *>(mapped) + (UINT64(c) * kinds + kind) * bytes;
            for (UINT b = 0; b < bytes; b++)
              if (actual[b] != expected[kind][b]) {
                correct = expect(
                    false, "case %u kind %u byte %u: %02x expected %02x (raw %u index %u)", c, kind, b, actual[b],
                    expected[kind][b], addresses[c][0], addresses[c][1]
                );
                break;
              }
          }
        readback->Unmap(0, nullptr);
        if (!correct)
          return verdict();
      }
    }
  }
  return verdict();
}
