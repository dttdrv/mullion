// contract: waiting on a shared fence for a value it never gets costs nothing once the waits are over: releasing the
// fence ends its events and queue Waits, and leaves no handle of the process behind, however many waits there were.
// ID3D12Fence::SetEventOnCompletion: "Specifies an event that's raised when the fence reaches a certain value."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12fence-seteventoncompletion
// ID3D12CommandQueue::Wait: "Queues a GPU-side wait, and returns immediately."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-wait
// IUnknown::Release: "When the reference count reaches zero, the object is freed from memory."
// https://learn.microsoft.com/en-us/windows/win32/api/unknwn/nf-unknwn-iunknown-release
// the pages do not say what the last release does to pending events; vkd3d-proton's Windows witness
// (tests/d3d12_sync.c, test_fence_wait_robustness_inner) has it end them, and d3d12_fence_release checks that here.
// the process's handles are counted with NtQuerySystemInformation(SystemExtendedHandleInformation), before and after
// the waits; the first round makes whatever a process keeps for its first shared fence. a few handles may come and
// go with threads, so the test allows 8 of 128 waits' worth. what a released fence leaves in memory is the bytes of
// the blocks in use in the process's heaps (HeapWalk), after each of eight rounds of 8 fences with one wait each,
// beside 256 unshared fences that took the heaps' bookkeeping to its size: from the fourth round to the eighth, fences
// that kept the storage of their completions (about 176 bytes each) would add 5.6 KB, and the test allows 2 KB. every
// shared fence made stays registered with launchd after the process (shareEvent), so the test makes few: about 90.
#include "d3d12_test.hpp"
#include <algorithm>
#include <thread>

// the bytes of the blocks in use in the process's heaps
static SIZE_T
heap_bytes() {
  std::vector<HANDLE> heaps(GetProcessHeaps(0, nullptr) + 16);
  heaps.resize(GetProcessHeaps(heaps.size(), heaps.data()));
  SIZE_T bytes = 0;
  for (HANDLE heap : heaps) {
    HeapLock(heap);
    PROCESS_HEAP_ENTRY entry{};
    while (HeapWalk(heap, &entry))
      if (entry.wFlags & PROCESS_HEAP_ENTRY_BUSY)
        bytes += entry.cbData;
    HeapUnlock(heap);
  }
  return bytes;
}

struct HandleEntry {
  void *Object;
  ULONG_PTR UniqueProcessId, HandleValue;
  ULONG GrantedAccess;
  USHORT CreatorBackTraceIndex, ObjectTypeIndex;
  ULONG HandleAttributes, Reserved;
};

struct HandleTable {
  ULONG_PTR NumberOfHandles, Reserved;
  HandleEntry Handles[1];
};

// the handles the process has open
static LONG
handles() {
  using Query = LONG(WINAPI *)(ULONG, void *, ULONG, ULONG *);
  static auto query = (Query)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQuerySystemInformation");
  std::vector<char> table(1 << 20);
  LONG status;
  ULONG size;
  while ((status = query(64 /* SystemExtendedHandleInformation */, table.data(), table.size(), &size)) ==
         LONG(0xC0000004) /* STATUS_INFO_LENGTH_MISMATCH */)
    table.resize(std::max<size_t>(table.size() * 2, size));
  if (status < 0)
    return -1;
  auto info = (HandleTable *)table.data();
  LONG count = 0;
  for (ULONG_PTR i = 0; i < info->NumberOfHandles; i++)
    count += info->Handles[i].UniqueProcessId == GetCurrentProcessId();
  return count;
}

int
main(int argc, char **argv) {
  std::thread([] {
    Sleep(60000);
    expect(false, "a fence call did not return within 60 s");
    ExitProcess(verdict());
  }).detach();
  ComPtr<ID3D12Device1> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12Fence> drain;
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&drain)));
  HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr), drained = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  if (!expect(event && drained, "CreateEvent: %lu", GetLastError()))
    return verdict();
  const UINT waits = 128;
  UINT64 drains = 0;
  // the queue gets past every Wait it was given: the fences' releases end them
  auto drain_queue = [&](const char *what) {
    CHECK(queue->Signal(drain.Get(), ++drains));
    CHECK(drain->SetEventOnCompletion(drains, drained));
    DWORD result = WaitForSingleObject(drained, 10000);
    expect(result == WAIT_OBJECT_0, "%s: the queue did not get past its Waits (%lu)", what, result);
    return 0;
  };

  // the waits of a round. `fences` shared fences get waits spread over them: events, queue Waits and multiple-fence
  // events for values each one never gets. every fence is then released
  auto round = [&](UINT fences, const char *what) {
    std::vector<ComPtr<ID3D12Fence>> made(fences);
    for (auto &fence : made)
      CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)));
    for (UINT i = 0; i < waits; i++) {
      ID3D12Fence *fence = made[i % fences].Get();
      UINT64 value = 1000 + (i / fences) % 3;
      ID3D12Fence *list[] = {fence};
      CHECK(i % 4 == 3   ? queue->Wait(fence, value)
            : i % 4 == 2 ? device->SetEventOnMultipleFenceCompletion(list, &value, 1, D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY,
                                                                     event)
                         : fence->SetEventOnCompletion(value, event));
    }
    expect(WaitForSingleObject(event, 0) == WAIT_TIMEOUT, "%s: an event was raised for a value no fence has", what);
    made.clear();
    expect(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0, "%s: releasing the fences raised no event", what);
    return drain_queue(what);
  };

  step("a first round, which makes what the process keeps for shared fences");
  if (round(1, "first round"))
    return 1;
  Sleep(200);
  LONG before = handles();
  if (!expect(before > 0, "NtQuerySystemInformation gave no handles of the process"))
    return verdict();
  for (UINT fences : {1u, 8u, 16u}) {
    step("%u waits over %u shared fences, which are released", waits, fences);
    if (round(fences, "waits"))
      return 1;
    Sleep(200);
    LONG after = handles();
    printf("%u waits over %u fences: %ld handles before, %ld after\n", waits, fences, before, after);
    expect(after - before <= 8, "%u waits over %u fences left %ld handles behind (%ld before, %ld after)", waits,
           fences, after - before, before, after);
  }

  // fences of their own, one wait each, released at once
  const UINT many = 8;
  auto singles = [&] {
    for (UINT i = 0; i < many; i++) {
      ComPtr<ID3D12Fence> fence;
      CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)));
      CHECK(fence->SetEventOnCompletion(1000, event));
    }
    return 0;
  };
  // the heaps' own bookkeeping grows with how many blocks of a size were ever in use: unshared fences, which nothing
  // registers, take it to that first, and stay, so a block a released fence keeps takes new room
  std::vector<ComPtr<ID3D12Fence>> unshared(256);
  for (auto &fence : unshared) {
    CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    CHECK(fence->SetEventOnCompletion(1000, event));
  }
  step("8 rounds of %u shared fences with a wait each, which are released", many);
  SIZE_T bytes[9];
  bytes[0] = heap_bytes();
  for (UINT i = 1; i <= 8; i++) {
    if (singles())
      return 1;
    bytes[i] = heap_bytes();
  }
  printf("heap bytes in use after each round of %u fences:", many);
  for (UINT i = 0; i <= 8; i++)
    printf(" %zu", bytes[i]);
  printf("\n");
  expect(bytes[8] < bytes[4] + 2 * 1024, "4 rounds of %u released fences left %zd bytes in the heaps", many,
         SSIZE_T(bytes[8] - bytes[4]));
  unshared.clear();
  CloseHandle(event);
  CloseHandle(drained);
  return verdict();
}
