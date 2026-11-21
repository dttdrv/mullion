// contract: SM6.6 atomic overloads "function exactly as the originals", storing the operation's result and
// returning the original value (DirectX-Specs, HLSL SM6.6 Atomic Operations, New Atomic Functions,
// https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_6_Int64_and_Float_Atomics.html).
// typed int64/uint64 resources use R32G32_UINT. advertised typed, groupshared and descriptor-heap capabilities
// require these operations. each thread owns one word; CPU states check both the returned and stored bits at
// every step, including successful and failed compare exchanges/stores. surrounding words remain untouched.
// ResourceDescriptorHeap forms use the same contract under AtomicInt64OnDescriptorHeapResourceSupported;
// the heap is bound before its root signature (DirectX-Specs, HLSL Dynamic Resources, Root Signature Changes).
#include "d3d12_test.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <limits>

enum Kind { raw, structured, typed, tex1d, tex1da, tex2d, tex2da, tex3d, shared, kinds };
enum Operation {
  add,
  bit_and,
  bit_or,
  bit_xor,
  minimum,
  maximum,
  exchange,
  compare_equal,
  compare_unequal,
  store_equal,
  store_unequal,
  operations
};
static const char hlsl[] = R"hlsl(
StructuredBuffer<uint64_t2> inputs : register(t0);
RWStructuredBuffer<uint64_t> output : register(u1);
#if HEAP
cbuffer Binding : register(b0) { uint memory_descriptor; }
// a global takes an initializer only when it is static
#define MEMORY_BIND = ResourceDescriptorHeap[memory_descriptor]
#define MEMORY_STORAGE static
#else
#define MEMORY_BIND : register(u0)
#define MEMORY_STORAGE
#endif
#if KIND == RAW
MEMORY_STORAGE RWByteAddressBuffer memory MEMORY_BIND;
#define ATOM(OP,V,O) memory.Interlocked##OP##64((t + 1) * WORD, T(V), O)
#define CAS(C,V,O) memory.InterlockedCompareExchange64((t + 1) * WORD, T(C), T(V), O)
#define STORE(C,V) memory.InterlockedCompareStore64((t + 1) * WORD, T(C), T(V))
#else
#if KIND == SHARED
groupshared T memory[GROUP + 2];
#define DEST memory[t % GROUP + 1]
#elif KIND == STRUCTURED
MEMORY_STORAGE RWStructuredBuffer<T> memory MEMORY_BIND;
#define DEST memory[t + 1]
#elif KIND == TYPED
MEMORY_STORAGE RWBuffer<T> memory MEMORY_BIND;
#define DEST memory[t + 1]
#elif KIND == TEX1D
MEMORY_STORAGE RWTexture1D<T> memory MEMORY_BIND;
#define DEST memory[t + 1]
#elif KIND == TEX1DA
MEMORY_STORAGE RWTexture1DArray<T> memory MEMORY_BIND;
#define DEST memory[uint2(t % SIDE + 1, t / SIDE + 1)]
#elif KIND == TEX2D
MEMORY_STORAGE RWTexture2D<T> memory MEMORY_BIND;
#define DEST memory[uint2(t % SIDE + 1, t / SIDE + 1)]
#elif KIND == TEX2DA
MEMORY_STORAGE RWTexture2DArray<T> memory MEMORY_BIND;
#define DEST memory[uint3(t % SIDE + 1, t / SIDE % ROWS + 1, t / (SIDE * ROWS) + 1)]
#else
MEMORY_STORAGE RWTexture3D<T> memory MEMORY_BIND;
#define DEST memory[uint3(t % SIDE + 1, t / SIDE % ROWS + 1, t / (SIDE * ROWS) + 1)]
#endif
#define ATOM(OP,V,O) Interlocked##OP(DEST, T(V), O)
#define CAS(C,V,O) InterlockedCompareExchange(DEST, T(C), T(V), O)
#define STORE(C,V) InterlockedCompareStore(DEST, T(C), T(V))
#endif
void record(uint t, uint op, T old) {
  T current;
  ATOM(Or, 0, current);
  output[(t * OPS + op) * 2] = uint64_t(old);
  output[(t * OPS + op) * 2 + 1] = uint64_t(current);
}
[numthreads(GROUP, 1, 1)] void cs(uint t : SV_DispatchThreadID, uint local : SV_GroupIndex, uint group : SV_GroupID) {
#if KIND == SHARED
  memory[local + 1] = T(inputs[t].x);
  if (local == 0) memory[0] = memory[GROUP + 1] = T(GUARD);
  GroupMemoryBarrierWithGroupSync();
#endif
  if (t < COUNT) {
    T a = T(inputs[t].x), b = T(inputs[t].y), old;
    ATOM(Add, b, old); record(t, ADD, old);
    ATOM(And, b, old); record(t, BIT_AND, old);
    ATOM(Or, a, old); record(t, BIT_OR, old);
    ATOM(Xor, b, old); record(t, BIT_XOR, old);
    ATOM(Min, b, old); record(t, MINIMUM, old);
    ATOM(Max, a, old); record(t, MAXIMUM, old);
    ATOM(Exchange, b, old); record(t, EXCHANGE, old);
    CAS(b, ~b, old); record(t, COMPARE_EQUAL, old);
    CAS(b, a, old); record(t, COMPARE_UNEQUAL, old);
    ATOM(Or, 0, old); STORE(~b, a); record(t, STORE_EQUAL, old);
    ATOM(Or, 0, old); STORE(~b, b); record(t, STORE_UNEQUAL, old);
  }
#if KIND == SHARED
  GroupMemoryBarrierWithGroupSync();
  if (local == 0) {
    output[COUNT * OPS * 2 + group * 2] = uint64_t(memory[0]);
    output[COUNT * OPS * 2 + group * 2 + 1] = uint64_t(memory[GROUP + 1]);
  }
#endif
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler) || !compiler.dxc) {
    printf("skipped: 64-bit atomic forms require DXC\n");
    return 77;
  }
  const UINT group = D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP, count = group + 1,
             groups = (count + group - 1) / group;
  const UINT side = sizeof(UINT) * CHAR_BIT + 1, rows = 4;
  const uint64_t guard = UINT64_MAX - 1;
  const char *names[] = {"raw", "structured", "typed", "tex1d", "tex1da", "tex2d", "tex2da", "tex3d", "shared"};
  const char *kind_names[] = {"RAW", "STRUCTURED", "TYPED", "TEX1D", "TEX1DA", "TEX2D", "TEX2DA", "TEX3D", "SHARED"};
  const char *op_names[] = {"ADD",      "BIT_AND",       "BIT_OR",          "BIT_XOR",     "MINIMUM",      "MAXIMUM",
                            "EXCHANGE", "COMPARE_EQUAL", "COMPARE_UNEQUAL", "STORE_EQUAL", "STORE_UNEQUAL"};
  std::array<std::array<std::array<std::string, kinds>, 2>, 2> shaders;
  for (UINT sign = 0; sign < shaders.size(); sign++)
    for (UINT heap = 0; heap < shaders[sign].size(); heap++)
      for (UINT kind = 0; kind < kinds; kind++) {
        if (heap && kind == shared)
          continue;
        std::vector<std::string> defines = {
            std::string("T=") + (sign ? "int64_t" : "uint64_t"),
            "KIND=" + std::to_string(kind),
            "HEAP=" + std::to_string(heap),
            "GROUP=" + std::to_string(group),
            "COUNT=" + std::to_string(count),
            "SIDE=" + std::to_string(side),
            "ROWS=" + std::to_string(rows),
            "OPS=" + std::to_string(operations),
            "WORD=" + std::to_string(sizeof(uint64_t)),
            "GUARD=" + std::to_string(guard) + "ull"
        };
        for (UINT k = 0; k < kinds; k++)
          defines.push_back(std::string(kind_names[k]) + "=" + std::to_string(k));
        for (UINT k = 0; k < operations; k++)
          defines.push_back(std::string(op_names[k]) + "=" + std::to_string(k));
        step("compile %s %s atomic operations, heap=%u", sign ? "signed" : "unsigned", names[kind], heap);
        shaders[sign][heap][kind] = compiler.compile(hlsl, "cs", "cs_6_6", defines);
        if (!expect(!shaders[sign][heap][kind].empty(), "%s atomic HLSL", names[kind]))
          return verdict();
      }
  using Pair = std::array<uint64_t, 2>;
  const uint64_t edges[] = {
      0,
      1,
      UINT64_MAX,
      uint64_t(INT64_MIN),
      uint64_t(INT64_MAX),
      UINT32_MAX,
      uint64_t(1) << 32,
      (uint64_t(1) << 32) + 1,
      0x5555555555555555ull,
      0xaaaaaaaaaaaaaaaaull
  };
  std::vector<Pair> input(groups * group);
  for (UINT i = 0; i < input.size(); i++)
    input[i] = {edges[i % std::size(edges)], edges[(i / std::size(edges) + i + 1) % std::size(edges)]};
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
  D3D12_FEATURE_DATA_D3D12_OPTIONS9 options9{};
  D3D12_FEATURE_DATA_D3D12_OPTIONS11 options11{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1)));
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9, &options9, sizeof(options9)));
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS11, &options11, sizeof(options11)));
  if (!expect(
          options1.Int64ShaderOps && options9.AtomicInt64OnTypedResourceSupported &&
              options9.AtomicInt64OnGroupSharedSupported && options11.AtomicInt64OnDescriptorHeapResourceSupported,
          "advertised atomic capabilities"
      ))
    return verdict();
  auto inputs =
      buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, input.size() * sizeof(Pair), D3D12_RESOURCE_STATE_GENERIC_READ);
  const UINT64 result_bytes = (UINT64(count) * operations * 2 + groups * 2) * sizeof(uint64_t);
  auto initial = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, result_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto output = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, result_bytes, D3D12_RESOURCE_STATE_COPY_DEST,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, result_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(inputs && initial && output && readback, "atomic buffers"))
    return verdict();
  void *mapped;
  CHECK(inputs->Map(0, nullptr, &mapped));
  memcpy(mapped, input.data(), input.size() * sizeof(Pair));
  inputs->Unmap(0, nullptr);
  CHECK(initial->Map(0, nullptr, &mapped));
  memset(mapped, 0xcd, result_bytes);
  initial->Unmap(0, nullptr);
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  D3D12_INDIRECT_ARGUMENT_DESC argument{D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH};
  D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(D3D12_DISPATCH_ARGUMENTS), 1, &argument};
  ComPtr<ID3D12CommandSignature> signature;
  CHECK(device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signature)));
  auto indirect =
      buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(D3D12_DISPATCH_ARGUMENTS), D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(bool(indirect), "indirect atomic dispatch buffer"))
    return verdict();
  CHECK(indirect->Map(0, nullptr, &mapped));
  const D3D12_DISPATCH_ARGUMENTS dispatch{groups, 1, 1};
  memcpy(mapped, &dispatch, sizeof(dispatch));
  indirect->Unmap(0, nullptr);
  for (UINT kind = 0; kind < kinds; kind++) {
    UINT width = kind < tex1da || kind == shared ? count + 2 : side + 2;
    UINT height = kind == tex2d ? (count + side - 1) / side + 2 : kind >= tex2da && kind <= tex3d ? rows + 2 : 1;
    UINT depth = kind == tex1da                    ? (count + side - 1) / side + 2
                 : kind >= tex2da && kind <= tex3d ? (count + side * rows - 1) / (side * rows) + 2
                                                   : 1;
    auto index = [&](UINT t) {
      return kind < tex1da || kind == shared ? t + 1
             : kind == tex1da                ? (t / side + 1) * width + t % side + 1
             : kind == tex2d ? (t / side + 1) * width + t % side + 1
                             : ((t / (side * rows) + 1) * height + t / side % rows + 1) * width + t % side + 1;
    };
    std::vector<uint64_t> start(width * height * depth, guard);
    for (UINT t = 0; t < count; t++)
      start[index(t)] = input[t][0];
    ComPtr<ID3D12Resource> memory, upload, memory_readback;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
    UINT64 memory_bytes = start.size() * sizeof(uint64_t);
    D3D12_RESOURCE_DESC resource_desc{};
    if (kind < tex1d || kind == shared) {
      memory = buffer(
          device.Get(), D3D12_HEAP_TYPE_DEFAULT, memory_bytes, D3D12_RESOURCE_STATE_COPY_DEST,
          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
      );
      if (!expect(bool(memory), "atomic memory buffer"))
        return verdict();
      resource_desc = memory->GetDesc();
    } else {
      resource_desc = {
          kind <= tex1da  ? D3D12_RESOURCE_DIMENSION_TEXTURE1D
          : kind == tex3d ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                          : D3D12_RESOURCE_DIMENSION_TEXTURE2D,
          0,
          width,
          height,
          UINT16(depth),
          1,
          DXGI_FORMAT_R32G32_UINT,
          {1, 0},
          D3D12_TEXTURE_LAYOUT_UNKNOWN,
          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
      };
      D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
      CHECK(device->CreateCommittedResource(
          &heap, D3D12_HEAP_FLAG_NONE, &resource_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&memory)
      ));
      footprints.resize(kind == tex3d ? 1 : depth);
      device->GetCopyableFootprints(
          &resource_desc, 0, footprints.size(), 0, footprints.data(), nullptr, nullptr, &memory_bytes
      );
    }
    upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, memory_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
    memory_readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, memory_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!expect(upload && memory_readback, "atomic memory copies"))
      return verdict();
    char *bytes;
    CHECK(upload->Map(0, nullptr, (void **)&bytes));
    if (footprints.empty())
      memcpy(bytes, start.data(), memory_bytes);
    else
      for (UINT z = 0; z < depth; z++) {
        auto fp = footprints[kind == tex3d ? 0 : z];
        for (UINT y = 0; y < height; y++)
          memcpy(
              bytes + fp.Offset + (kind == tex3d ? z * height : 0) * fp.Footprint.RowPitch + y * fp.Footprint.RowPitch,
              start.data() + (z * height + y) * width, width * sizeof(uint64_t)
          );
      }
    upload->Unmap(0, nullptr);
    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
    };
    CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{DXGI_FORMAT_R32G32_UINT, D3D12_UAV_DIMENSION_BUFFER};
    if (kind <= typed || kind == shared) {
      uav.Buffer.NumElements = start.size();
      if (kind == raw)
        uav.Format = DXGI_FORMAT_R32_TYPELESS, uav.Buffer.NumElements *= 2,
        uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
      if (kind == structured || kind == shared)
        uav.Format = DXGI_FORMAT_UNKNOWN, uav.Buffer.StructureByteStride = sizeof(uint64_t);
    } else if (kind == tex1d)
      uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
    else if (kind == tex1da)
      uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY, uav.Texture1DArray.ArraySize = depth;
    else if (kind == tex2d)
      uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    else if (kind == tex2da)
      uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY, uav.Texture2DArray.ArraySize = depth;
    else
      uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D, uav.Texture3D.WSize = depth;
    const UINT memory_descriptor = 1;
    auto memory_cpu = heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateUnorderedAccessView(nullptr, nullptr, &uav, memory_cpu);
    memory_cpu.ptr += memory_descriptor * device->GetDescriptorHandleIncrementSize(heap_desc.Type);
    device->CreateUnorderedAccessView(memory.Get(), nullptr, &uav, memory_cpu);
    auto memory_gpu = heap->GetGPUDescriptorHandleForHeapStart();
    memory_gpu.ptr += memory_descriptor * device->GetDescriptorHandleIncrementSize(heap_desc.Type);
    for (UINT sign = 0; sign < shaders.size(); sign++)
      for (UINT binding = 0; binding < (kind <= structured ? 3u : kind == shared ? 1u : 2u); binding++)
        for (bool indirect_path : {false, true}) {
          bool dynamic = kind <= structured ? binding == 2 : kind != shared && binding == 1;
          bool descriptor = binding || kind >= typed;
          D3D12_ROOT_PARAMETER params[] = {
              {dynamic      ? D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS
               : descriptor ? D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE
                            : D3D12_ROOT_PARAMETER_TYPE_UAV},
              {D3D12_ROOT_PARAMETER_TYPE_SRV},
              {D3D12_ROOT_PARAMETER_TYPE_UAV}
          };
          D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1};
          if (dynamic)
            params[0].Constants.Num32BitValues = 1;
          else if (descriptor)
            params[0].DescriptorTable = {1, &range};
          params[2].Descriptor.ShaderRegister = 1;
          auto rs = root_signature(
              device.Get(),
              {std::size(params), params, 0, nullptr,
               dynamic ? D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED : D3D12_ROOT_SIGNATURE_FLAG_NONE}
          );
          D3D12_COMPUTE_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(shaders[sign][dynamic][kind])};
          ComPtr<ID3D12PipelineState> pso;
          CHECK(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)));
          step(
              "%s %s atomics, binding=%u, indirect=%d", sign ? "signed" : "unsigned", names[kind], binding,
              indirect_path
          );
          CHECK(forget(readback.Get()));
          CHECK(allocator->Reset());
          CHECK(list->Reset(allocator.Get(), pso.Get()));
          if (footprints.empty())
            list->CopyResource(memory.Get(), upload.Get());
          else
            for (UINT sub = 0; sub < footprints.size(); sub++) {
              D3D12_TEXTURE_COPY_LOCATION from{
                  upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprints[sub]}
              },
                  to{memory.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = sub}};
              list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            }
          list->CopyResource(output.Get(), initial.Get());
          transition(list.Get(), memory.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          if (descriptor) {
            ID3D12DescriptorHeap *heaps[] = {heap.Get()};
            list->SetDescriptorHeaps(1, heaps);
          }
          list->SetComputeRootSignature(rs.Get());
          if (dynamic)
            list->SetComputeRoot32BitConstant(0, memory_descriptor, 0);
          else if (descriptor)
            list->SetComputeRootDescriptorTable(0, memory_gpu);
          else
            list->SetComputeRootUnorderedAccessView(0, memory->GetGPUVirtualAddress());
          list->SetComputeRootShaderResourceView(1, inputs->GetGPUVirtualAddress());
          list->SetComputeRootUnorderedAccessView(2, output->GetGPUVirtualAddress());
          if (indirect_path)
            list->ExecuteIndirect(signature.Get(), 1, indirect.Get(), 0, nullptr, 0);
          else
            list->Dispatch(groups, 1, 1);
          transition(list.Get(), memory.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
          transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
          list->CopyResource(readback.Get(), output.Get());
          if (footprints.empty())
            list->CopyResource(memory_readback.Get(), memory.Get());
          else
            for (UINT sub = 0; sub < footprints.size(); sub++) {
              D3D12_TEXTURE_COPY_LOCATION from{
                  memory.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = sub}
              },
                  to{memory_readback.Get(),
                     D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                     {.PlacedFootprint = footprints[sub]}};
              list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            }
          transition(list.Get(), memory.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
          transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
          CHECK(submit(device.Get(), queue.Get(), list.Get()));
          uint64_t *got;
          CHECK(readback->Map(0, nullptr, (void **)&got));
          auto expected_memory = start;
          for (UINT t = 0; t < count; t++) {
            auto [a, b] = input[t];
            uint64_t value = a;
            auto less = [&](uint64_t x, uint64_t y) {
              return (x ^ (sign ? uint64_t(1) << 63 : 0)) < (y ^ (sign ? uint64_t(1) << 63 : 0));
            };
            for (UINT op = 0; op < operations; op++) {
              uint64_t old = value;
              switch (op) {
              case add:
                value += b;
                break;
              case bit_and:
                value &= b;
                break;
              case bit_or:
                value |= a;
                break;
              case bit_xor:
                value ^= b;
                break;
              case minimum:
                value = less(b, value) ? b : value;
                break;
              case maximum:
                value = less(value, a) ? a : value;
                break;
              case exchange:
                value = b;
                break;
              case compare_equal:
              case compare_unequal:
                if (value == b)
                  value = op == compare_equal ? ~b : a;
                break;
              case store_equal:
              case store_unequal:
                if (value == ~b)
                  value = op == store_equal ? a : b;
                break;
              }
              UINT at = (t * operations + op) * 2;
              expect(
                  got[at] == old && got[at + 1] == value, "thread %u op %u old/new %#llx/%#llx, want %#llx/%#llx", t,
                  op, got[at], got[at + 1], old, value
              );
            }
            if (kind != shared)
              expected_memory[index(t)] = value;
          }
          if (kind == shared)
            for (UINT g = 0; g < groups * 2; g++)
              expect(got[count * operations * 2 + g] == guard, "groupshared guard %u", g);
          readback->Unmap(0, nullptr);
          CHECK(memory_readback->Map(0, nullptr, (void **)&bytes));
          for (UINT z = 0; z < depth; z++)
            for (UINT y = 0; y < height; y++)
              for (UINT x = 0; x < width; x++) {
                UINT64 offset = ((z * height + y) * width + x) * sizeof(uint64_t);
                if (!footprints.empty()) {
                  auto fp = footprints[kind == tex3d ? 0 : z];
                  offset =
                      fp.Offset + ((kind == tex3d ? z * height : 0) + y) * fp.Footprint.RowPitch + x * sizeof(uint64_t);
                }
                uint64_t value;
                memcpy(&value, bytes + offset, sizeof(value));
                expect(
                    value == expected_memory[(z * height + y) * width + x], "memory (%u,%u,%u): %#llx", x, y, z, value
                );
              }
          memory_readback->Unmap(0, nullptr);
        }
  }
  return verdict();
}
