// contract: splitting a dispatch preserves its grid and IDs, including under predication and ExecuteIndirect.
// "The invocation process dispatches the specified number of groups in the array." (D3D11.3 18.6.2).
// ThreadGroupID is "Current thread group relative to all thread groups in the Compute Shader Dispatch."
// (23.12). each thread writes both IDs and its atomic's returned value: every grid cell must be written once,
// and the returned values must be the integers below the thread count, once each. a recording of a large grid
// must also Close successfully without allocating a command per part; that grid is never submitted.
// parts share one compute encoder and pipeline binding, ordered by memory barriers (Metal, memoryBarrier(scope:)).
// a dispatch comes in parts where its 64-bit atomics take locks, whose holders wait behind the other threadgroups' SIMD
// groups (Metal promises progress only between those of one threadgroup): an add that returns its old value on every
// GPU, and an unsigned maximum without a result only where the GPU has no native 64-bit min and max (the Metal
// Shading Language's atomic_ulong max and min return void). an add without a result, the shader's only 64-bit atomic,
// on a raw buffer, is a 32-bit atomic on each half and takes no lock. a thread whose atomic has no result writes its
// own index as the returned value.
#include "d3d12_test.hpp"
#include "../../src/airconv/airconv_public.h"
#include "../../src/winemetal/winemetal.h"
#include <algorithm>
#include <array>

static const char hlsl[] = R"hlsl(
cbuffer Grid : register(b0) { uint3 groups; };
RWByteAddressBuffer result : register(u0);
[numthreads(TX, TY, TZ)]
void cs(uint3 tid : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint3 local : SV_GroupThreadID) {
  uint3 cells = groups * uint3(TX, TY, TZ);
  uint t = tid.x + cells.x * (tid.y + cells.y * tid.z);
#if ATOMIC64 == 3
  uint64_t old = t;
  result.InterlockedAdd64(0, 1ull);
#elif ATOMIC64 == 2
  uint64_t old = t;
  result.InterlockedMax64(0, uint64_t(t) + 1);
#elif ATOMIC64
  uint64_t old;
  result.InterlockedAdd64(0, 1ull, old);
#else
  uint old;
  result.InterlockedAdd(0, 1, old);
#endif
  uint at = HEADER + t * RECORD;
  result.Store4(at, uint4(gid, 1));
  result.Store4(at + VECTOR_BYTES, uint4(tid, local.x + TX * (local.y + TY * local.z)));
  result.Store<uint64_t>(at + 2 * VECTOR_BYTES, uint64_t(old));
}
)hlsl";

static decltype(&MTLComputeCommandEncoder_encodeCommands) original_encode;
static decltype(&MTL4CommandEncoder_encodeCommands) original_encode4;
static uint64_t part_groups, largest_part, parts, pipeline_updates, part_pipeline_updates;
static obj_handle_t part_encoder;

static void
observe(obj_handle_t encoder, const wmtcmd_base *cmd) {
  for (auto next = cmd; next; next = static_cast<const wmtcmd_base *>(next->next.ptr)) {
    if (next->type == WMTComputeCommandSetPSO)
      pipeline_updates++;
    if (next->type == WMTComputeCommandDispatchPart) {
      auto part = reinterpret_cast<const wmtcmd_compute_dispatch_part *>(next);
      if (!part->origin.x && !part->origin.y && !part->origin.z) {
        part_encoder = encoder;
        part_pipeline_updates = pipeline_updates;
      }
      expect(encoder == part_encoder && pipeline_updates == part_pipeline_updates,
             "parts share one compute encoder and pipeline binding");
      uint64_t groups = uint64_t(part->size.width) * part->size.height * part->size.depth;
      part_groups += groups;
      largest_part = std::max(largest_part, groups);
      parts++;
    }
  }
}

static void
observe_encode(obj_handle_t encoder, const wmtcmd_base *cmd) {
  observe(encoder, cmd);
  original_encode(encoder, cmd);
}

// a Metal 4 queue encodes compute work through the encoder of every stream
static void
observe_encode4(obj_handle_t encoder, enum WMTMetal4Stream kind, const wmtcmd_base *cmd) {
  if (kind == WMTMetal4Compute)
    observe(encoder, cmd);
  original_encode4(encoder, kind, cmd);
}

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler) || !compiler.dxc) {
    printf("skipped: resource 64-bit atomics require DXC\n");
    return 77;
  }
  constexpr UINT vector_bytes = sizeof(std::array<UINT, 4>), header = sizeof(uint64_t),
                 record = 2 * vector_bytes + sizeof(uint64_t);
  const std::array<UINT, 3> threads[] = {{1, 1, 1}, {4, 2, 2}, {D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP, 1, 1}};
  const UINT budget = threads[1][0] * threads[1][1] * threads[1][2];
  // a 32-bit add, a 64-bit add, a 64-bit maximum without a result, a 64-bit add without a result
  enum { add32, add64, max64, split64, atomics };
  std::string shaders[std::size(threads)][atomics];
  for (UINT shape = 0; shape < std::size(threads); shape++)
    for (UINT atomic = 0; atomic < atomics; atomic++) {
      shaders[shape][atomic] = compiler.compile(
          hlsl, "cs", "cs_6_6",
          {"TX=" + std::to_string(threads[shape][0]), "TY=" + std::to_string(threads[shape][1]),
           "TZ=" + std::to_string(threads[shape][2]), "ATOMIC64=" + std::to_string(atomic),
           "HEADER=" + std::to_string(header), "RECORD=" + std::to_string(record),
           "VECTOR_BYTES=" + std::to_string(vector_bytes)});
      if (!expect(!shaders[shape][atomic].empty(), "shader %u/%u compiled", shape, atomic))
        return verdict();
    }
  SetEnvironmentVariableA("MULLION_D3D12_THREADS_AT_ONCE", std::to_string(budget).c_str());
  SetEnvironmentVariableA("MULLION_SHADER_FLAGS", std::to_string(SM50_SHADER_FLAG_NO_ATOMIC_MINMAX64).c_str());
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  auto module = reinterpret_cast<BYTE *>(GetModuleHandleA("d3d12core.dll"));
  if (!expect(module != nullptr, "d3d12core loaded"))
    return verdict();
  auto dos = reinterpret_cast<IMAGE_DOS_HEADER *>(module);
  auto nt = reinterpret_cast<IMAGE_NT_HEADERS *>(module + dos->e_lfanew);
  auto imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(
      module + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
  ULONG_PTR *import = nullptr, *import4 = nullptr;
  for (; imports->Name; imports++) {
    auto names = reinterpret_cast<IMAGE_THUNK_DATA *>(module + imports->OriginalFirstThunk);
    auto functions = reinterpret_cast<IMAGE_THUNK_DATA *>(module + imports->FirstThunk);
    for (; names->u1.AddressOfData; names++, functions++) {
      if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
        continue;
      auto name = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(module + names->u1.AddressOfData);
      if (!strcmp(reinterpret_cast<const char *>(name->Name), "MTLComputeCommandEncoder_encodeCommands"))
        import = &functions->u1.Function;
      if (!strcmp(reinterpret_cast<const char *>(name->Name), "MTL4CommandEncoder_encodeCommands"))
        import4 = &functions->u1.Function;
    }
  }
  DWORD protection;
  if (!expect(import && import4 && VirtualProtect(import, sizeof(*import), PAGE_READWRITE, &protection) &&
                  VirtualProtect(import4, sizeof(*import4), PAGE_READWRITE, &protection),
              "probe compute imports"))
    return verdict();
  original_encode = reinterpret_cast<decltype(original_encode)>(*import);
  *import = reinterpret_cast<ULONG_PTR>(observe_encode);
  original_encode4 = reinterpret_cast<decltype(original_encode4)>(*import4);
  *import4 = reinterpret_cast<ULONG_PTR>(observe_encode4);

  auto metal = GetModuleHandleA("winemetal.dll");
  auto all = reinterpret_cast<decltype(&WMTCopyAllDevices)>(GetProcAddress(metal, "WMTCopyAllDevices"));
  auto count_devices = reinterpret_cast<decltype(&NSArray_count)>(GetProcAddress(metal, "NSArray_count"));
  auto at = reinterpret_cast<decltype(&NSArray_object)>(GetProcAddress(metal, "NSArray_object"));
  auto supports =
      reinterpret_cast<decltype(&MTLDevice_supportsFamily)>(GetProcAddress(metal, "MTLDevice_supportsFamily"));
  auto release = reinterpret_cast<decltype(&NSObject_release)>(GetProcAddress(metal, "NSObject_release"));
  if (!expect(all && count_devices && at && supports && release, "Metal capability exports"))
    return verdict();
  auto devices = all();
  if (!expect(devices && count_devices(devices), "Metal devices"))
    return verdict();
  const bool native_minmax = supports(at(devices, 0), WMTGPUFamilyApple8);
  release(devices);

  enum {
    Direct, EqualSkip, EqualRun, NotEqualRun, NotEqualSkip, Indirect, Counted, CountZero, CountPastMaximum,
    IndirectEqualSkip, IndirectEqualRun, IndirectNotEqualSkip, IndirectNotEqualRun, NoCommands, TwoCommands, ModeCount
  };
  for (bool emulated : {false, true}) {
    if (!emulated && !native_minmax)
      continue;
    SetEnvironmentVariableA("MULLION_SHADER_FLAGS",
                            std::to_string(emulated ? SM50_SHADER_FLAG_NO_ATOMIC_MINMAX64 : 0).c_str());
    // the flags are read once per device
    device.Reset();
    CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    D3D12_ROOT_PARAMETER parameters[] = {{D3D12_ROOT_PARAMETER_TYPE_UAV}, {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}};
    parameters[1].Constants = {0, 0, 3};
    auto root = root_signature(device.Get(), {UINT(std::size(parameters)), parameters});
    D3D12_INDIRECT_ARGUMENT_DESC arguments[] = {{D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT},
                                               {D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH}};
    arguments[0].Constant = {1, 0, 3};
    struct Command {
      UINT grid[3];
      D3D12_DISPATCH_ARGUMENTS dispatch;
    };
    D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(Command), UINT(std::size(arguments)), arguments};
    ComPtr<ID3D12CommandSignature> signature, plain_signature;
    CHECK(device->CreateCommandSignature(&signature_desc, root.Get(), IID_PPV_ARGS(&signature)));
    D3D12_COMMAND_SIGNATURE_DESC plain_desc{sizeof(D3D12_DISPATCH_ARGUMENTS), 1, &arguments[1]};
    CHECK(device->CreateCommandSignature(&plain_desc, nullptr, IID_PPV_ARGS(&plain_signature)));
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
    CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
    CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
    CHECK(list->Close());
    for (UINT shape = 0; shape < std::size(threads); shape++)
      for (UINT atomic = 0; atomic < atomics; atomic++) {
        const bool locks = atomic == add64 || (atomic == max64 && emulated);
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{root.Get(), bytecode(shaders[shape][atomic])};
        ComPtr<ID3D12PipelineState> pipeline;
        CHECK(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline)));
        const UINT per_group = threads[shape][0] * threads[shape][1] * threads[shape][2];
        const UINT boundary = std::max(budget / per_group, 1u);
        const std::array<UINT, 3> grids[] = {{boundary - 1, 1, 1}, {boundary, 1, 1}, {boundary + 1, 1, 1},
                                           {9, 5, 3}, {3, 9, 5}, {5, 3, 9}, {0, 5, 3}, {9, 0, 3}, {9, 5, 0}};
        for (auto grid : grids)
          for (UINT mode = Direct; mode < ModeCount; mode++) {
            if (mode >= NoCommands && !locks)
              continue;
            step("emulated %u, shape %u, atomic64 %u, %u by %u by %u groups, mode %u", emulated, shape, atomic,
                 grid[0], grid[1], grid[2], mode);
            const UINT groups = grid[0] * grid[1] * grid[2], count = groups * per_group;
            const UINT64 bytes = header + UINT64(std::max(count, 1u)) * record;
            auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes + 2 * sizeof(Command) + sizeof(UINT64),
                                 D3D12_RESOURCE_STATE_GENERIC_READ);
            auto output = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COPY_DEST,
                                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            auto indirect = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, 2 * sizeof(Command) + sizeof(UINT64),
                                   D3D12_RESOURCE_STATE_COPY_DEST);
            auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
            if (!expect(upload && output && indirect && readback, "buffers created"))
              return verdict();
            char *mapped;
            CHECK(upload->Map(0, nullptr, reinterpret_cast<void **>(&mapped)));
            memset(mapped, 0, header);
            memset(mapped + header, 0xff, bytes - header);
            Command command[] = {{{grid[0], grid[1], grid[2]}, {grid[0], grid[1], grid[2]}},
                                 {{grid[1], grid[0], grid[2]}, {grid[1], grid[0], grid[2]}}};
            memcpy(mapped + bytes, &command, sizeof(command));
            const UINT maximum = mode == NoCommands ? 0 : mode == TwoCommands ? UINT(std::size(command)) : 1;
            const UINT64 predicate = mode == CountPastMaximum ? maximum + 1 : mode == TwoCommands ? maximum :
                                     mode == EqualRun || mode == NotEqualSkip || mode == Counted ||
                                     mode == IndirectEqualRun || mode == IndirectNotEqualSkip ? 1 : 0;
            memcpy(mapped + bytes + sizeof(command), &predicate, sizeof(predicate));
            CHECK(allocator->Reset());
            CHECK(list->Reset(allocator.Get(), pipeline.Get()));
            list->CopyBufferRegion(output.Get(), 0, upload.Get(), 0, bytes);
            list->CopyBufferRegion(indirect.Get(), 0, upload.Get(), bytes, sizeof(command) + sizeof(UINT64));
            transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            transition(list.Get(), indirect.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
            list->SetComputeRootSignature(root.Get());
            list->SetComputeRootUnorderedAccessView(0, output->GetGPUVirtualAddress());
            const UINT zero[3]{};
            list->SetComputeRoot32BitConstants(1, 3, mode >= Counted ? zero : grid.data(), 0);
            if ((mode >= EqualSkip && mode <= NotEqualSkip) ||
                (mode >= IndirectEqualSkip && mode <= IndirectNotEqualRun))
              list->SetPredication(indirect.Get(), sizeof(command),
                                   mode <= EqualRun || mode == IndirectEqualSkip || mode == IndirectEqualRun
                                       ? D3D12_PREDICATION_OP_EQUAL_ZERO : D3D12_PREDICATION_OP_NOT_EQUAL_ZERO);
            if (mode >= Indirect)
              list->ExecuteIndirect(mode == Indirect ? plain_signature.Get() : signature.Get(),
                                    maximum, indirect.Get(),
                                    mode == Indirect ? offsetof(Command, dispatch) : 0,
                                    (mode >= Counted && mode <= CountPastMaximum) || mode == TwoCommands
                                        ? indirect.Get() : nullptr,
                                    sizeof(command));
            else
              list->Dispatch(grid[0], grid[1], grid[2]);
            list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
            transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->CopyResource(readback.Get(), output.Get());
            part_groups = largest_part = parts = 0;
            CHECK(submit(device.Get(), queue.Get(), list.Get()));
            const bool runs = mode != EqualSkip && mode != NotEqualSkip && mode != CountZero &&
                              mode != IndirectEqualSkip && mode != IndirectNotEqualSkip && mode != NoCommands;
            const UINT draws = mode == TwoCommands ? maximum : 1, executed = runs ? draws * count : 0;
            if (locks) {
              expect(part_groups == (runs ? draws * groups : 0), "%llu groups in parts, want %u", part_groups,
                     runs ? draws * groups : 0);
              expect(largest_part <= boundary, "part of %llu groups exceeds %u", largest_part, boundary);
            } else {
              expect(parts == 0, "ordinary shader was split");
            }
            const char *got;
            CHECK(readback->Map(0, nullptr, (void **)&got));
            uint64_t total;
            memcpy(&total, got, sizeof(total));
            const UINT stored = atomic == max64 ? (runs ? count : 0) : executed;
            expect(total == stored, "atomic result %llu, want %u", total, stored);
            std::vector<uint64_t> returned;
            const UINT cells[] = {grid[mode == TwoCommands ? 1 : 0] * threads[shape][0],
                                  grid[mode == TwoCommands ? 0 : 1] * threads[shape][1], grid[2] * threads[shape][2]};
            for (UINT t = 0; t < count; t++) {
              UINT ids[8];
              memcpy(ids, got + header + UINT64(t) * record, sizeof(ids));
              if (!runs) {
                for (auto id : ids)
                  expect(id == UINT_MAX, "skipped dispatch wrote thread %u", t);
                continue;
              }
              const UINT position[] = {t % cells[0], t / cells[0] % cells[1], t / (cells[0] * cells[1])};
              for (UINT axis = 0; axis < 3; axis++) {
                expect(ids[axis] == position[axis] / threads[shape][axis], "thread %u GroupID axis %u", t, axis);
                expect(ids[4 + axis] == position[axis], "thread %u DispatchThreadID axis %u", t, axis);
              }
              expect(ids[3] == 1, "thread %u was not written", t);
              expect(ids[7] == position[0] % threads[shape][0] + threads[shape][0] *
                                      (position[1] % threads[shape][1] +
                                       threads[shape][1] * (position[2] % threads[shape][2])),
                     "thread %u local index", t);
              uint64_t old;
              memcpy(&old, got + header + UINT64(t) * record + sizeof(ids), sizeof(old));
              returned.push_back(old);
            }
            std::sort(returned.begin(), returned.end());
            for (UINT t = 0; t < returned.size(); t++)
              expect(returned[t] == (atomic >= max64 ? 0 : (draws - 1) * count) + t, "returned value %u: %llu", t,
                     returned[t]);
            readback->Unmap(0, nullptr);
            if (trace::wrong)
              return verdict();
          }
        if (locks && per_group == D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP) {
          step("record a large grid without submitting it");
          CHECK(allocator->Reset());
          CHECK(list->Reset(allocator.Get(), pipeline.Get()));
          list->SetComputeRootSignature(root.Get());
          list->Dispatch(32768, 8192, 1);
          expect(list->Close() == S_OK, "large dispatch exhausted recording storage");
        }
      }
  }
  *import = reinterpret_cast<ULONG_PTR>(original_encode);
  expect(VirtualProtect(import, sizeof(*import), protection, &protection), "restore compute import protection");
  return verdict();
}
