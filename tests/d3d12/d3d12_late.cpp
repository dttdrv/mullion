// contract: delayed commits preserve queue order, copied bytes and signals, and CPU waits still complete.
// Apple, MTLCommandBuffer.enqueue: "Reserves the next available place for the command buffer in its command queue."
// https://developer.apple.com/documentation/metal/mtlcommandbuffer/enqueue()
// MULLION_TEST_LATE specifies a minimum commit delay; only that switch's timing is tested against a clock.
#include "d3d12_test.hpp"
#include "../../src/winemetal/winemetal.h"
#include <algorithm>
#include <iterator>
#include <random>

int
main(int argc, char **argv) {
  const auto delay_ms = 50ull;
  const DWORD timeout_ms = 60000;
  auto finished = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  if (!expect(finished != nullptr, "watchdog event was created"))
    return verdict();
  auto watchdog = CreateThread(
      nullptr, 0,
      [](void *event) -> DWORD {
        if (WaitForSingleObject(event, timeout_ms) != WAIT_OBJECT_0) {
          printf("failed: commit, wait or teardown did not finish within %lu ms\n", timeout_ms);
          ExitProcess(1);
        }
        return 0;
      },
      finished, 0, nullptr
  );
  if (!expect(watchdog != nullptr, "watchdog was created"))
    return verdict();

  if (argc < 3 || strcmp(argv[2], "child")) {
    step("child with MULLION_TEST_LATE=%llu", delay_ms);
    auto spawn = reinterpret_cast<LONG(WINAPI *)(char *const *, int)>(
        GetProcAddress(GetModuleHandleA("ntdll.dll"), "__wine_unix_spawnvp")
    );
    auto unix_name = reinterpret_cast<char *(__cdecl *)(const WCHAR *)>(
        GetProcAddress(GetModuleHandleA("kernel32.dll"), "wine_get_unix_file_name")
    );
    std::wstring loader(GetEnvironmentVariableW(L"WINELOADER", nullptr, 0), L'\0');
    if (!expect(spawn && unix_name && !loader.empty() &&
                    GetEnvironmentVariableW(L"WINELOADER", loader.data(), loader.size()),
                "Wine's loader or Unix process launch is unavailable"))
      return verdict();
    if (loader.starts_with(L"\\??\\"))
      loader.replace(0, std::size(L"\\??\\") - 1, L"\\\\?\\");
    auto loader_path = unix_name(loader.c_str());
    if (!expect(loader_path != nullptr, "the loader's Unix path is unavailable"))
      return verdict();
    // a test run from a Unix path outside every drive reports its module as unix\... (the
    // \\?\unix device without its prefix), which the loader cannot open and the conversion
    // below does not recognize; a Z:\ module is already whole
    WCHAR module[MAX_PATH], exe_path[MAX_PATH + 4];
    const auto length = GetModuleFileNameW(nullptr, module, std::size(module));
    if (!expect(length && length < std::size(module), "the executable's path is unavailable")) {
      HeapFree(GetProcessHeap(), 0, loader_path);
      return verdict();
    }
    if (module[0] == L'\\' || module[1] == L':') {
      lstrcpynW(exe_path, module, std::size(exe_path));
    } else {
      lstrcpyW(exe_path, L"\\\\?\\");
      lstrcpynW(exe_path + 4, module, std::size(exe_path) - 4);
    }
    auto exe = unix_name(exe_path);
    if (!expect(exe != nullptr, "the executable's Unix path is unavailable")) {
      HeapFree(GetProcessHeap(), 0, loader_path);
      return verdict();
    }
    const auto setting = "MULLION_TEST_LATE=" + std::to_string(delay_ms);
    const char *args[] = {"env", setting.c_str(), loader_path, exe, argc > 1 ? argv[1] : "dxil", "child", nullptr};
    const auto status = spawn(const_cast<char *const *>(args), TRUE);
    HeapFree(GetProcessHeap(), 0, exe);
    HeapFree(GetProcessHeap(), 0, loader_path);
    expect(status == 0, "late child ended with %ld", status);
    SetEvent(finished);
    expect(WaitForSingleObject(watchdog, timeout_ms) == WAIT_OBJECT_0, "watchdog ended");
    CloseHandle(watchdog);
    CloseHandle(finished);
    return verdict();
  }

  const UINT seed = 1;
  std::mt19937 next(seed);
  LARGE_INTEGER frequency;
  QueryPerformanceFrequency(&frequency);
  auto ticks = [] {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
  };
  auto milliseconds = [&](LONGLONG since) { return 1000.0 * (ticks() - since) / frequency.QuadPart; };

  auto bridge = LoadLibraryA("winemetal.dll");
  if (!expect(bridge != nullptr, "winemetal.dll was loaded"))
    return verdict();
#define METAL(name)                                                                                                    \
  auto name = reinterpret_cast<decltype(&::name)>(GetProcAddress(bridge, #name));                                      \
  if (!expect(name != nullptr, "%s was exported", #name))                                                              \
    return verdict();
  METAL(NSAutoreleasePool_alloc_init);
  METAL(NSObject_retain);
  METAL(NSObject_release);
  METAL(WMTCopyAllDevices);
  METAL(NSArray_count);
  METAL(NSArray_object);
  METAL(MTLDevice_newCommandQueue);
  METAL(MTLCommandQueue_commandBuffer);
  METAL(MTLCommandBuffer_commit);
  METAL(MTLCommandBuffer_status);
  METAL(MTLCommandBuffer_waitUntilCompleted);
  METAL(MTLDevice_newBuffer);
  METAL(MTLCommandBuffer_blitCommandEncoder);
  METAL(MTLBlitCommandEncoder_encodeCommands);
  METAL(MTLCommandEncoder_endEncoding);
  METAL(MTLDevice_newSharedEvent);
  METAL(MTLCommandBuffer_encodeWaitForEvent);
  METAL(MTLCommandBuffer_encodeSignalEvent);
  METAL(MTLSharedEvent_signaledValue);
  METAL(MTLSharedEvent_waitUntilSignaledValue);
#undef METAL
  auto pool = NSAutoreleasePool_alloc_init();
  auto devices = WMTCopyAllDevices();
  if (!expect(devices && NSArray_count(devices), "a Metal device exists"))
    return verdict();
  auto device = NSArray_object(devices, 0);
  SYSTEM_INFO system;
  GetSystemInfo(&system);
  const UINT capacity = 2, count = 2 * capacity + 1;
  const UINT word = sizeof(UINT), page = system.dwPageSize;
  const UINT sizes[] = {word, page - word, page, page + word};
  bool returned_early = false;
  for (bool event_wait : {false, true}) {
    auto queue = MTLDevice_newCommandQueue(device, capacity);
    if (!expect(queue != 0, "command queue was created"))
      return verdict();
    for (auto size : sizes) {
      step("seed=%u event_wait=%u bytes=%u buffers=%u", seed, event_wait, size, count);
      auto event = MTLDevice_newSharedEvent(device);
      WMTBufferInfo source_info{size, WMTResourceStorageModeShared},
          result_info{count * size, WMTResourceStorageModeShared};
      auto source = MTLDevice_newBuffer(device, &source_info), result = MTLDevice_newBuffer(device, &result_info);
      if (!expect(event && source && result && result_info.memory.ptr, "shared buffers and event were created"))
        return verdict();
      memset(result_info.memory.ptr, 0, result_info.length);
      std::vector<BYTE> expected(result_info.length);
      obj_handle_t last = 0;
      LONGLONG submitted = 0;
      for (UINT i = 0; i < count; i++) {
        auto local = NSAutoreleasePool_alloc_init();
        auto commands = MTLCommandQueue_commandBuffer(queue);
        if (!expect(commands != 0, "command buffer %u was created", i))
          return verdict();
        BYTE byte = BYTE(next());
        std::fill_n(expected.begin() + i * size, size, byte);
        auto encoder = MTLCommandBuffer_blitCommandEncoder(commands);
        if (!expect(encoder != 0, "blit encoder %u was created", i))
          return verdict();
        wmtcmd_blit_copy_from_buffer_to_buffer copy{};
        copy.type = WMTBlitCommandCopyFromBufferToBuffer;
        copy.src = source;
        copy.dst = result;
        copy.dst_offset = i * size;
        copy.copy_length = size;
        wmtcmd_blit_fillbuffer fill{};
        fill.type = WMTBlitCommandFillBuffer;
        fill.next.ptr = &copy;
        fill.buffer = source;
        fill.length = size;
        fill.value = byte;
        MTLBlitCommandEncoder_encodeCommands(encoder, reinterpret_cast<wmtcmd_base *>(&fill));
        MTLCommandEncoder_endEncoding(encoder);
        MTLCommandBuffer_encodeSignalEvent(commands, event, i + 1);
        submitted = ticks();
        MTLCommandBuffer_commit(commands);
        returned_early |= milliseconds(submitted) < delay_ms;
        expect(MTLCommandBuffer_status(commands) >= WMTCommandBufferStatusCommitted,
               "buffer %u appears uncommitted after commit", i);
        const auto signaled = MTLSharedEvent_signaledValue(event);
        if (milliseconds(submitted) < delay_ms)
          expect(signaled < i + 1, "buffer %u signaled before its delay", i);
        if (i + 1 == count) {
          last = commands;
          NSObject_retain(last);
        }
        NSObject_release(local);
      }
      NSObject_release(source);
      if (event_wait && !expect(MTLSharedEvent_waitUntilSignaledValue(event, count, timeout_ms),
                                "shared event wait completed"))
        ExitProcess(verdict());
      MTLCommandBuffer_waitUntilCompleted(last);
      const auto elapsed = milliseconds(submitted);
      expect(elapsed >= delay_ms, "completion took %.3f ms, wanted at least %llu ms", elapsed, delay_ms);
      expect(MTLCommandBuffer_status(last) == WMTCommandBufferStatusCompleted, "last command buffer completed");
      expect(MTLSharedEvent_signaledValue(event) == count, "signal did not follow all copies");
      const auto bytes = static_cast<const BYTE *>(result_info.memory.ptr);
      auto mismatch = std::mismatch(expected.begin(), expected.end(), bytes).first;
      expect(mismatch == expected.end(), "copy differs at byte %zu of %zu", size_t(mismatch - expected.begin()),
             expected.size());
      for (auto object : {last, result, event})
        NSObject_release(object);
    }
    NSObject_release(queue);
  }
  expect(returned_early, "every commit blocked for the requested delay of %llu ms", delay_ms);

  step("a CPU wait cannot hold up the commit that signals another queue's GPU wait");
  auto event = MTLDevice_newSharedEvent(device);
  const obj_handle_t queues[] = {MTLDevice_newCommandQueue(device, capacity),
                                 MTLDevice_newCommandQueue(device, capacity)};
  if (!expect(event && queues[0] && queues[1], "cross-queue event and queues were created"))
    return verdict();
  auto consumer = MTLCommandQueue_commandBuffer(queues[0]), producer = MTLCommandQueue_commandBuffer(queues[1]);
  if (!expect(consumer && producer, "cross-queue command buffers were created"))
    return verdict();
  MTLCommandBuffer_encodeWaitForEvent(consumer, event, 1);
  MTLCommandBuffer_encodeSignalEvent(producer, event, 1);
  MTLCommandBuffer_commit(consumer);
  MTLCommandBuffer_commit(producer);
  MTLCommandBuffer_waitUntilCompleted(consumer);
  MTLCommandBuffer_waitUntilCompleted(producer);
  expect(MTLCommandBuffer_status(consumer) == WMTCommandBufferStatusCompleted &&
             MTLCommandBuffer_status(producer) == WMTCommandBufferStatusCompleted &&
             MTLSharedEvent_signaledValue(event) == 1,
         "cross-queue wait and signal completed");
  for (auto object : {queues[0], queues[1], event})
    NSObject_release(object);
  NSObject_release(devices);
  NSObject_release(pool);

  step("seed=%u D3D12 queue fence covers copied bytes and allocator reuse", seed);
  {
    ComPtr<ID3D12Device> d3d;
    CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d)));
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    CHECK(d3d->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    CHECK(d3d->CreateCommandAllocator(desc.Type, IID_PPV_ARGS(&allocator)));
    CHECK(d3d->CreateCommandList(0, desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
    CHECK(list->Close());
    CHECK(d3d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    const auto size = sizes[std::size(sizes) - 1];
    auto upload = buffer(d3d.Get(), D3D12_HEAP_TYPE_UPLOAD, size, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto readback = buffer(d3d.Get(), D3D12_HEAP_TYPE_READBACK, size, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!expect(upload && readback, "D3D12 buffers were created"))
      return verdict();
    auto event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!expect(event != nullptr, "fence event was created"))
      return verdict();
    for (UINT round = 1; round <= count; round++) {
      step("seed=%u D3D12 copy round=%u", seed, round);
      std::vector<BYTE> expected(upload->GetDesc().Width);
      for (auto &byte : expected)
        byte = BYTE(next());
      void *mapped;
      CHECK(upload->Map(0, nullptr, &mapped));
      memcpy(mapped, expected.data(), expected.size());
      upload->Unmap(0, nullptr);
      CHECK(forget(readback.Get()));
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), nullptr));
      list->CopyBufferRegion(readback.Get(), 0, upload.Get(), 0, expected.size());
      CHECK(list->Close());
      ID3D12CommandList *lists[] = {list.Get()};
      queue->ExecuteCommandLists(std::size(lists), lists);
      CHECK(queue->Signal(fence.Get(), round));
      CHECK(fence->SetEventOnCompletion(round, event));
      if (!expect(WaitForSingleObject(event, timeout_ms) == WAIT_OBJECT_0, "fence wait completed"))
        ExitProcess(verdict());
      expect(fence->GetCompletedValue() == round, "fence reached the copy's value");
      CHECK(readback->Map(0, nullptr, &mapped));
      expect(!memcmp(mapped, expected.data(), expected.size()), "fence preceded copied bytes");
      readback->Unmap(0, nullptr);
    }
    CHECK(d3d->GetDeviceRemovedReason());
    CloseHandle(event);
  }
  FreeLibrary(bridge);
  SetEvent(finished);
  expect(WaitForSingleObject(watchdog, timeout_ms) == WAIT_OBJECT_0, "watchdog ended");
  CloseHandle(watchdog);
  CloseHandle(finished);
  return verdict();
}
