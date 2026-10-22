// contract: 64-bit atomics update the addressed word indivisibly, including through overlapping UAVs.
// "This feature introduces overloaded versions of existing Interlocked* functions that take 64-bit integer
// and floating-point parameters, but otherwise function exactly as the originals." (DirectX-Specs, HLSL Shader
// Model 6.6 Atomic Operations, New Atomic Functions). "The value in dst1 memory before addition is returned to dst0."
// (D3D11.3 22.17.24 imm_atomic_iadd). D3D11.3 22.17.15 and 22.17.16 require max and min to be "performed
// atomically." unsigned min/max without a returned value exercise the native-only optimization separately.
// additions carry across both halves: all returned values must be each multiple of the step exactly once.
// the two texture views address the same slice with different relative coordinates, at each mip, with guard
// slices; counts cross SIMD, threadgroup and lock-table boundaries. tests/run.py supplies families 7, 8 and 9.
// Apple7 must use locks: Metal Feature Set Tables, feature availability note 7, provides ulong min/max only
// starting with Apple8 on macOS. the pipeline library preserves the AIR symbols, including the lock release.
// an injected give-up flag must remove the device: GetDeviceRemovedReason "Gets the reason that the device was
// removed, or S_OK if the device isn't removed." (Microsoft Learn, ID3D12Device::GetDeviceRemovedReason).
#include "d3d12_test.hpp"
#include "../../src/airconv/airconv_public.h"
#include "../../src/winemetal/winemetal.h"
#include <algorithm>
#include <bit>
#include <cstdint>

static const char hlsl[] = R"hlsl(
cbuffer Args : register(b0) { uint count; uint words; };
RWByteAddressBuffer raw : register(u0);
#if DIMENSION == 1
RWTexture1DArray<uint64_t> full_view : register(u1);
RWTexture1DArray<uint64_t> sub_view : register(u2);
#define FULL(x) uint2(x, 1)
#define SUB(x) uint2(x, 0)
#elif DIMENSION == 2
RWTexture2DArray<uint64_t> full_view : register(u1);
RWTexture2DArray<uint64_t> sub_view : register(u2);
#define FULL(x) uint3(x, 0, 1)
#define SUB(x) uint3(x, 0, 0)
#else
RWTexture3D<uint64_t> full_view : register(u1);
RWTexture3D<uint64_t> sub_view : register(u2);
#define FULL(x) uint3(x, 0, 1)
#define SUB(x) uint3(x, 0, 0)
#endif
RWStructuredBuffer<uint64_t> returned : register(u3);
RWBuffer<uint64_t> typed_buffer : register(u4);
groupshared uint64_t shared_word;
uint64_t value(uint t) { return (uint64_t(t + 1) << 32) | (count - t); }
[numthreads(GROUP, 1, 1)]
void raw_extremes(uint t : SV_DispatchThreadID) {
  if (t >= count) return;
  raw.InterlockedMax64(0, value(t));
  raw.InterlockedMin64(WORD, value(t));
}
[numthreads(GROUP, 1, 1)]
void typed_extremes(uint t : SV_DispatchThreadID) {
  if (t >= count) return;
  InterlockedMax(full_view[FULL(0)], value(t));
  InterlockedMin(sub_view[SUB(1)], value(t));
}
[numthreads(GROUP, 1, 1)]
void aliased_add(uint t : SV_DispatchThreadID) {
  if (t >= count) return;
  uint64_t old;
  if (t & 1)
    InterlockedAdd(full_view[FULL(t % words)], STEP, old);
  else
    InterlockedAdd(sub_view[SUB(t % words)], STEP, old);
  returned[t] = old;
}
[numthreads(GROUP, 1, 1)]
void shared_add(uint t : SV_DispatchThreadID, uint i : SV_GroupIndex, uint g : SV_GroupID) {
  if (i == 0) shared_word = 0;
  GroupMemoryBarrierWithGroupSync();
  if (t < count) {
    uint64_t old;
    InterlockedAdd(shared_word, STEP, old);
    returned[t] = old;
  }
  GroupMemoryBarrierWithGroupSync();
  if (i == 0) raw.Store<uint64_t>(g * WORD, shared_word);
}
[numthreads(GROUP, 1, 1)]
void buffer_add(uint t : SV_DispatchThreadID) {
  if (t >= count) return;
  uint64_t old;
  if (t & 1)
    raw.InterlockedAdd64((t % words + 1) * WORD, STEP, old);
  else
    InterlockedAdd(typed_buffer[t % words], STEP, old);
  returned[t] = old;
}
)hlsl";

static decltype(&MTLDevice_newBuffer) original_buffer;
static UINT64 lock_bytes;
static LONG *given_up;

static obj_handle_t
observe_buffer(obj_handle_t device, WMTBufferInfo *info) {
  auto result = original_buffer(device, info);
  if (result && info->length == lock_bytes && info->options == WMTResourceStorageModeShared) {
    if (!expect(!given_up && info->memory.get(), "one accessible lock table"))
      return result;
    given_up = reinterpret_cast<LONG *>(static_cast<char *>(info->memory.get()) + info->length) - 1;
  }
  return result;
}

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler) || !compiler.dxc) {
    printf("skipped: 64-bit atomics require DXC\n");
    return 77;
  }
  constexpr UINT group = D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP;
  constexpr uint64_t increment = UINT32_MAX;
  const char *entries[] = {"raw_extremes", "typed_extremes", "aliased_add", "shared_add", "buffer_add"};
  std::string shaders[3][std::size(entries)];
  for (UINT dimension = 1; dimension <= std::size(shaders); dimension++)
    for (UINT i = 0; i < std::size(entries); i++) {
      shaders[dimension - 1][i] =
          compiler.compile(hlsl, entries[i], "cs_6_6",
                           {"GROUP=" + std::to_string(group), "WORD=" + std::to_string(sizeof(uint64_t)),
                            "DIMENSION=" + std::to_string(dimension), "STEP=" + std::to_string(increment) + "ull"});
      if (!expect(!shaders[dimension - 1][i].empty(), "%s dimension %u did not compile", entries[i], dimension))
        return verdict();
    }
  SetEnvironmentVariableA("MULLION_SHADER_FLAGS", nullptr);
  ComPtr<ID3D12Device1> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS1 options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options, sizeof(options)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS9 optional{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9, &optional, sizeof(optional)));
  if (!expect(options.Int64ShaderOps && optional.AtomicInt64OnTypedResourceSupported &&
                  optional.AtomicInt64OnGroupSharedSupported && options.WaveLaneCountMin && options.TotalLaneCount,
              "64-bit atomic capabilities are missing"))
    return verdict();
  const UINT many = std::max(group, std::bit_ceil(options.TotalLaneCount)) * 4;
  D3D12_DESCRIPTOR_RANGE ranges[] = {{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 1},
                                     {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 4, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND}};
  D3D12_ROOT_PARAMETER params[] = {{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS},
                                   {D3D12_ROOT_PARAMETER_TYPE_UAV},
                                   {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
                                   {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[0].Constants = {0, 0, 2};
  params[2].DescriptorTable = {UINT(std::size(ranges)), ranges};
  params[3].Descriptor.ShaderRegister = 3;
  auto root = root_signature(device.Get(), {UINT(std::size(params)), params});
  if (!expect(bool(root), "root signature created"))
    return verdict();
  for (UINT dimension = 1; dimension <= std::size(shaders); dimension++) {
    const UINT width = std::min(
        UINT((dimension == 3 ? D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION : D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION) / 2),
        std::bit_ceil(options.TotalLaneCount) + 1);
    ComPtr<ID3D12PipelineState> pipelines[std::size(entries)];
    for (UINT i = 0; i < std::size(entries); i++) {
      D3D12_COMPUTE_PIPELINE_STATE_DESC desc{root.Get(), bytecode(shaders[dimension - 1][i])};
      CHECK(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipelines[i])));
      if (i < 2) {
        step("%s dimension %u converted atomics", entries[i], dimension);
        ComPtr<ID3D12PipelineLibrary> library;
        CHECK(device->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&library)));
        CHECK(library->StorePipeline(L"atomics", pipelines[i].Get()));
        std::string converted(library->GetSerializedSize(), '\0');
        CHECK(library->Serialize(converted.data(), converted.size()));
        const bool minimum = converted.find(i ? "air.atomic_min_explicit_" : "air.atomic.global.min.u.i64") !=
                             std::string::npos;
        const bool maximum = converted.find(i ? "air.atomic_max_explicit_" : "air.atomic.global.max.u.i64") !=
                             std::string::npos;
        const bool locks = converted.find("air.atomic.global.xchg.i32") != std::string::npos;
        expect((minimum && maximum && !locks) || (!minimum && !maximum && locks),
               "AIR must contain both native min/max or the lock release: min=%u max=%u locks=%u", minimum, maximum,
               locks);
        char family[16]{};
        GetEnvironmentVariableA("MULLION_GPU_FAMILY", family, sizeof(family));
        if (!strcmp(family, "7"))
          expect(locks && !minimum && !maximum, "Apple7 AIR must use locks and contain no native 64-bit min/max");
      }
    }
    const UINT64 result_bytes = UINT64(many) * sizeof(uint64_t);
    auto raw = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, result_bytes, D3D12_RESOURCE_STATE_COPY_DEST,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto returned = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, result_bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ComPtr<ID3D12Resource> texture;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION(D3D12_RESOURCE_DIMENSION_TEXTURE1D + dimension - 1),
                             0,
                             UINT64(width) * 2,
                             dimension == 1 ? 1u : 2u,
                             UINT16(dimension == 3 ? 4 : 3),
                             2,
                             DXGI_FORMAT_R32G32_UINT,
                             {1, 0},
                             D3D12_TEXTURE_LAYOUT_UNKNOWN,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(&texture)));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layouts[6];
    UINT rows[std::size(layouts)];
    UINT64 row_bytes[std::size(layouts)], texture_bytes;
    const UINT subresources = desc.MipLevels * (dimension == 3 ? 1 : desc.DepthOrArraySize);
    device->GetCopyableFootprints(&desc, 0, subresources, 0, layouts, rows, row_bytes, &texture_bytes);
    auto upload =
        buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, result_bytes + texture_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, result_bytes * 2 + texture_bytes,
                           D3D12_RESOURCE_STATE_COPY_DEST);
    if (!expect(raw && returned && upload && readback, "test buffers created"))
      return verdict();
    ComPtr<ID3D12DescriptorHeap> views;
    D3D12_DESCRIPTOR_HEAP_DESC views_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3,
                                          D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    CHECK(device->CreateDescriptorHeap(&views_desc, IID_PPV_ARGS(&views)));
    const UINT view_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
    CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
    CHECK(list->Close());
    for (UINT op = 0; op < std::size(entries); op++)
      for (UINT mip = 0; mip < (op == 1 || op == 2 ? desc.MipLevels : 1u); mip++)
        for (UINT words : (op == 2 || op == 4 ? std::vector<UINT>{1, width} : std::vector<UINT>{1}))
          for (UINT count : {1u, options.WaveLaneCountMin - 1, options.WaveLaneCountMin, options.WaveLaneCountMin + 1,
                             group - 1, group, group + 1, many}) {
            step("%s dimension %u, mip %u, %u words, %u threads", entries[op], dimension, mip, words, count);
            const UINT groups = (count + group - 1) / group;
            D3D12_UNORDERED_ACCESS_VIEW_DESC view{DXGI_FORMAT_R32G32_UINT};
            auto handle = views->GetCPUDescriptorHandleForHeapStart();
            if (dimension == 1) {
              view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
              view.Texture1DArray = {mip, 0, desc.DepthOrArraySize};
            } else if (dimension == 2) {
              view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
              view.Texture2DArray = {mip, 0, desc.DepthOrArraySize, 0};
            } else {
              view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
              view.Texture3D = {mip, 0, UINT(desc.DepthOrArraySize >> mip)};
            }
            device->CreateUnorderedAccessView(texture.Get(), nullptr, &view, handle);
            handle.ptr += view_step;
            if (dimension == 1) {
              view.Texture1DArray = {mip, 1, 1};
            } else if (dimension == 2) {
              view.Texture2DArray = {mip, 1, 1, 0};
            } else {
              view.Texture3D = {mip, 1, 1};
            }
            device->CreateUnorderedAccessView(texture.Get(), nullptr, &view, handle);
            handle.ptr += view_step;
            D3D12_UNORDERED_ACCESS_VIEW_DESC buffer_view{DXGI_FORMAT_R32G32_UINT, D3D12_UAV_DIMENSION_BUFFER};
            buffer_view.Buffer.FirstElement = 1;
            buffer_view.Buffer.NumElements = words;
            device->CreateUnorderedAccessView(raw.Get(), nullptr, &buffer_view, handle);
            void *mapped;
            CHECK(upload->Map(0, nullptr, &mapped));
            memset(mapped, 0, result_bytes + texture_bytes);
            if (op == 0)
              static_cast<uint64_t *>(mapped)[1] = ~uint64_t(0);
            if (op == 1) {
              auto at = result_bytes + (dimension == 3
                                            ? layouts[mip].Offset + UINT64(layouts[mip].Footprint.RowPitch) * rows[mip]
                                            : layouts[desc.MipLevels + mip].Offset);
              reinterpret_cast<uint64_t *>(static_cast<char *>(mapped) + at)[1] = ~uint64_t(0);
            }
            upload->Unmap(0, nullptr);
            CHECK(forget(readback.Get()));
            CHECK(allocator->Reset());
            CHECK(list->Reset(allocator.Get(), pipelines[op].Get()));
            list->CopyBufferRegion(raw.Get(), 0, upload.Get(), 0, result_bytes);
            for (UINT s = 0; s < subresources; s++) {
              D3D12_TEXTURE_COPY_LOCATION from{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
                  to{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
              from.PlacedFootprint = layouts[s];
              from.PlacedFootprint.Offset += result_bytes;
              to.SubresourceIndex = s;
              list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            }
            transition(list.Get(), raw.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list->SetDescriptorHeaps(1, views.GetAddressOf());
            list->SetComputeRootSignature(root.Get());
            const UINT args[] = {count, words};
            list->SetComputeRoot32BitConstants(0, std::size(args), args, 0);
            list->SetComputeRootUnorderedAccessView(1, raw->GetGPUVirtualAddress());
            list->SetComputeRootDescriptorTable(2, views->GetGPUDescriptorHandleForHeapStart());
            list->SetComputeRootUnorderedAccessView(3, returned->GetGPUVirtualAddress());
            list->Dispatch(groups, 1, 1);
            transition(list.Get(), raw.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(list.Get(), returned.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->CopyBufferRegion(readback.Get(), 0, raw.Get(), 0, result_bytes);
            list->CopyBufferRegion(readback.Get(), result_bytes, returned.Get(), 0, result_bytes);
            for (UINT s = 0; s < subresources; s++) {
              D3D12_TEXTURE_COPY_LOCATION from{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
                  to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
              from.SubresourceIndex = s;
              to.PlacedFootprint = layouts[s];
              to.PlacedFootprint.Offset += 2 * result_bytes;
              list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            }
            transition(list.Get(), raw.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            transition(list.Get(), returned.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            CHECK(submit(device.Get(), queue.Get(), list.Get()));
            uint64_t *data;
            CHECK(readback->Map(0, nullptr, reinterpret_cast<void **>(&data)));
            auto value = [&](UINT t) { return (uint64_t(t + 1) << 32) | (count - t); };
            for (UINT s = 0; s < subresources; s++)
              for (UINT z = 0; z < layouts[s].Footprint.Depth; z++)
                for (UINT y = 0; y < rows[s]; y++)
                  for (UINT x = 0; x < layouts[s].Footprint.Width; x++) {
                    auto row = reinterpret_cast<const uint64_t *>(
                        reinterpret_cast<const char *>(data) + 2 * result_bytes + layouts[s].Offset +
                        (UINT64(z) * rows[s] + y) * layouts[s].Footprint.RowPitch);
                    uint64_t want = 0;
                    if ((dimension == 3 ? s == mip && z == 1 : s == desc.MipLevels + mip) && y == 0) {
                      if (op == 1 && x < 2)
                        want = value(x ? 0 : count - 1);
                      if (op == 2 && x < words)
                        want = ((count + words - 1 - x) / words) * increment;
                    }
                    expect(row[x] == want, "subresource %u (%u,%u,%u): %llx, want %llx", s, x, y, z,
                           (unsigned long long)row[x], (unsigned long long)want);
                  }
            for (UINT x = 0; x < many; x++) {
              uint64_t want = 0;
              if (op == 0 && x < 2)
                want = value(x ? 0 : count - 1);
              if (op == 3 && x < groups)
                want = std::min(group, count - x * group) * increment;
              if (op == 4 && x && x <= words)
                want = ((count + words - x) / words) * increment;
              expect(data[x] == want, "buffer word %u: %llx, want %llx", x, (unsigned long long)data[x],
                     (unsigned long long)want);
            }
            if (op >= 2) {
              const auto old = data + result_bytes / sizeof(*data);
              const UINT buckets = op == 3 ? groups : words;
              std::vector<std::vector<uint64_t>> sequences(buckets);
              for (UINT t = 0; t < count; t++)
                sequences[op == 3 ? t / group : t % words].push_back(old[t]);
              for (UINT b = 0; b < buckets; b++) {
                auto &sequence = sequences[b];
                std::sort(sequence.begin(), sequence.end());
                for (UINT i = 0; i < sequence.size(); i++)
                  expect(sequence[i] == i * increment, "word %u returned[%u]: %llx", b, i,
                         (unsigned long long)sequence[i]);
              }
            }
            readback->Unmap(0, nullptr);
            if (trace::wrong)
              return verdict();
          }
  }
  step("inject the lock table's give-up flag without holding a GPU lock");
  // the table is a mask word, then a ticket and the ticket a lock serves for each of bit_ceil(lanes) locks (a FIFO,
  // so a waiter is served after those that arrived before it), then the give-up flag: the last word of the buffer
  lock_bytes = SM50_NULL_ACCELERATION_STRUCTURE_HEADER_SIZE +
               sizeof(uint32_t) * (2 * UINT64(std::bit_ceil(options.TotalLaneCount)) + 2);
  auto module = reinterpret_cast<BYTE *>(GetModuleHandleA("d3d12core.dll"));
  if (!expect(module != nullptr, "d3d12core loaded"))
    return verdict();
  auto dos = reinterpret_cast<IMAGE_DOS_HEADER *>(module);
  auto nt = reinterpret_cast<IMAGE_NT_HEADERS *>(module + dos->e_lfanew);
  auto imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(
      module + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
  ULONG_PTR *import = nullptr;
  for (; imports->Name; imports++) {
    auto names = reinterpret_cast<IMAGE_THUNK_DATA *>(module + imports->OriginalFirstThunk);
    auto functions = reinterpret_cast<IMAGE_THUNK_DATA *>(module + imports->FirstThunk);
    for (; names->u1.AddressOfData; names++, functions++) {
      if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
        continue;
      auto name = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(module + names->u1.AddressOfData);
      if (!strcmp(reinterpret_cast<const char *>(name->Name), "MTLDevice_newBuffer"))
        import = &functions->u1.Function;
    }
  }
  DWORD protection;
  if (!expect(import && VirtualProtect(import, sizeof(*import), PAGE_READWRITE, &protection), "probe buffer import"))
    return verdict();
  original_buffer = reinterpret_cast<decltype(original_buffer)>(*import);
  *import = reinterpret_cast<ULONG_PTR>(observe_buffer);
  ComPtr<ID3D12Device> failed;
  const auto created = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&failed));
  *import = reinterpret_cast<ULONG_PTR>(original_buffer);
  expect(VirtualProtect(import, sizeof(*import), protection, &protection), "restore buffer import protection");
  if (!expect(SUCCEEDED(created) && given_up, "device and its lock table captured: %08lx", created))
    return verdict();
  expect(*given_up == 0 && failed->GetDeviceRemovedReason() == S_OK, "fresh device has no give-up");
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(failed->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(failed->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  CHECK(failed->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  InterlockedExchange(given_up, 1);
  expect(submit(failed.Get(), queue.Get(), list.Get()) == DXGI_ERROR_DEVICE_HUNG, "give-up submission removed device");
  expect(failed->GetDeviceRemovedReason() == DXGI_ERROR_DEVICE_HUNG, "give-up reports DXGI_ERROR_DEVICE_HUNG");
  expect(failed->GetDeviceRemovedReason() == DXGI_ERROR_DEVICE_HUNG && device->GetDeviceRemovedReason() == S_OK,
         "removal persists on the failed device alone");
  return verdict();
}
