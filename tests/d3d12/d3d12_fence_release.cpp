// contract: the last public fence release unblocks its events and queue waits without public references from
// pending signals. subsequent queue work runs in order, and a drain fence covers that work.
// Microsoft Learn, ID3D12Fence::SetEventOnCompletion: "The fence value when the event is to be signaled."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12fence-seteventoncompletion
// ID3D12CommandQueue::Wait: "Queues a GPU-side wait, and returns immediately."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-wait
// ID3D12CommandQueue::Signal: "Use this method to set a fence value from the GPU side."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-signal
// these pages do not specify last-release behavior or public reference counts. vkd3d-proton's Windows witness,
// tests/d3d12_sync.c, test_fence_wait_robustness_inner: "Validate that signal/wait does not take public ref-counts."
// and "verify that releasing the fence unblocks all waiters". it releases fences with pending signals and checks
// that later work and the drain complete without device removal.
// https://github.com/HansKristian-Work/vkd3d-proton/blob/master/tests/d3d12_sync.c
// a live gate holds signals pending without expensive GPU work. 65 signals cross the queue's 32-command-buffer
// pool (src/d3d12/d3d12_command_queue.cpp, kCommandQueueSize).
#include "d3d12_test.hpp"
#include <thread>

int
main(int argc, char **argv) {
  std::thread([] {
    Sleep(60000);
    expect(false, "a fence call or cleanup did not return within 60 s");
    ExitProcess(verdict());
  }).detach();
  ComPtr<ID3D12Device1> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  const DWORD timeout = 10000;
  const UINT signals = 65;
  const UINT64 targets[] = {0, 1, UINT64_MAX};
  auto wait = [&](HANDLE event, const char *what) {
    DWORD result = WaitForSingleObject(event, timeout);
    if (!expect(result == WAIT_OBJECT_0, "%s: wait returned %lu within %lu ms", what, result, timeout))
      ExitProcess(verdict());
  };
  auto refs = [&](ID3D12Fence *fence, ULONG want, const char *what) {
    fence->AddRef();
    ULONG left = fence->Release();
    expect(left == want, "%s: %lu public references, want %lu", what, left, want);
  };

  for (UINT64 initial : targets) {
    step("events of a fence initially at %llu, with no pending signal", initial);
    ComPtr<ID3D12Fence> fence;
    CHECK(device->CreateFence(initial, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    HANDLE events[std::size(targets)];
    for (UINT i = 0; i < std::size(targets); i++) {
      events[i] = CreateEventA(nullptr, FALSE, FALSE, nullptr);
      if (!expect(events[i] != nullptr, "event %u could not be created", i))
        return verdict();
      CHECK(fence->SetEventOnCompletion(targets[i], events[i]));
      expect(WaitForSingleObject(events[i], 0) == (targets[i] <= initial ? WAIT_OBJECT_0 : WAIT_TIMEOUT),
             "event %u before release, target %llu, initial %llu", i, targets[i], initial);
    }
    fence->AddRef();
    ULONG left = fence->Release();
    expect(left == 1, "non-final release returned %lu, want 1", left);
    for (UINT i = 0; i < std::size(targets); i++)
      expect(WaitForSingleObject(events[i], 0) == WAIT_TIMEOUT, "event %u fired on a non-final release", i);
    left = fence.Detach()->Release();
    expect(left == 0, "final release returned %lu, want 0", left);
    for (UINT i = 0; i < std::size(targets); i++) {
      if (targets[i] > initial)
        wait(events[i], "event on a released fence");
      expect(WaitForSingleObject(events[i], 0) == WAIT_TIMEOUT, "event %u fired more than once", i);
      CloseHandle(events[i]);
    }
  }

  step("pending signals and waits retain no public references");
  ComPtr<ID3D12CommandQueue> queues[2];
  ComPtr<ID3D12Fence> drains[std::size(queues)], gate, signal_fence, wait_fence;
  const D3D12_COMMAND_LIST_TYPE types[] = {D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COMPUTE};
  for (UINT q = 0; q < std::size(queues); q++) {
    D3D12_COMMAND_QUEUE_DESC desc{types[q]};
    CHECK(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queues[q])));
    CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&drains[q])));
  }
  step("a signal on a queue with no wait completes without a public reference");
  HANDLE completed = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  if (!expect(completed != nullptr, "immediate signal event could not be created"))
    ExitProcess(verdict());
  CHECK(queues[0]->Signal(drains[0].Get(), 1));
  refs(drains[0].Get(), 1, "immediate queue Signal");
  CHECK(drains[0]->SetEventOnCompletion(1, completed));
  wait(completed, "immediate queue Signal");
  expect(drains[0]->GetCompletedValue() == 1, "immediate Signal did not reach 1");
  CloseHandle(completed);
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(drains[0].ReleaseAndGetAddressOf())));

  step("signals pending behind the live gate and waits on a second queue");
  for (auto fence : {gate.GetAddressOf(), signal_fence.GetAddressOf(), wait_fence.GetAddressOf()})
    CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence)));
  CHECK(queues[0]->Wait(gate.Get(), 1));
  refs(gate.Get(), 1, "queue Wait");
  for (UINT i = 0; i < signals; i++) {
    CHECK(queues[0]->Signal(signal_fence.Get(), i + 1));
    refs(signal_fence.Get(), 1, "pending queue Signal");
  }
  CHECK(queues[1]->Wait(signal_fence.Get(), UINT64_MAX));
  refs(signal_fence.Get(), 1, "second queue Wait");
  CHECK(queues[1]->Wait(gate.Get(), 1));
  refs(gate.Get(), 1, "second wait on the live gate");
  CHECK(queues[1]->Signal(wait_fence.Get(), 1));
  refs(wait_fence.Get(), 1, "second queue Signal");
  // a pending signal of 1 cannot satisfy this wait: only the last release can
  CHECK(queues[1]->Wait(wait_fence.Get(), 2));
  refs(wait_fence.Get(), 1, "Wait after the pending Signal");
  expect(signal_fence->GetCompletedValue() == 0 && wait_fence->GetCompletedValue() == 0,
         "a pending signal completed through the live gate");
  HANDLE events[2], all = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  if (!expect(all != nullptr, "multiple-fence event could not be created"))
    ExitProcess(verdict());
  ID3D12Fence *fences[] = {signal_fence.Get(), wait_fence.Get()};
  const UINT64 values[] = {UINT64_MAX, UINT64_MAX};
  CHECK(device->SetEventOnMultipleFenceCompletion(fences, values, std::size(fences),
                                                 D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL, all));
  for (UINT i = 0; i < std::size(fences); i++) {
    events[i] = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!expect(events[i] != nullptr, "pending-fence event %u could not be created", i))
      ExitProcess(verdict());
    CHECK(fences[i]->SetEventOnCompletion(UINT64_MAX, events[i]));
    refs(fences[i], 1, "SetEventOnCompletion");
    expect(WaitForSingleObject(events[i], 0) == WAIT_TIMEOUT, "pending-fence event %u fired before release", i);
  }
  step("the last public release fires events while signals remain behind the live gate");
  ULONG left = signal_fence.Detach()->Release();
  expect(left == 0, "signal fence release returned %lu, want 0", left);
  wait(events[0], "signal fence's event after release");
  expect(WaitForSingleObject(all, 0) == WAIT_TIMEOUT, "ALL fired while the second fence was still alive");
  left = wait_fence.Detach()->Release();
  expect(left == 0, "wait fence release returned %lu, want 0", left);
  wait(events[1], "wait fence's event after release");
  wait(all, "multiple-fence event after both releases");
  if (trace::wrong)
    ExitProcess(verdict());
  for (auto event : events)
    CloseHandle(event);
  CloseHandle(all);

  step("a copy and both drains stay behind the live gate after the other fences are released");
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, signals * sizeof(UINT), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto readback =
      buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, signals * sizeof(UINT), D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(upload && readback, "copy buffers could not be created"))
    ExitProcess(verdict());
  UINT *bytes;
  CHECK(upload->Map(0, nullptr, (void **)&bytes));
  for (UINT i = 0; i < signals; i++)
    bytes[i] = i + 1;
  upload->Unmap(0, nullptr);
  CHECK(forget(readback.Get()));
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandAllocator(types[1], IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, types[1], allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  list->CopyBufferRegion(readback.Get(), 0, upload.Get(), 0, signals * sizeof(UINT));
  CHECK(list->Close());
  ID3D12CommandList *copy = list.Get();
  queues[1]->ExecuteCommandLists(1, &copy);
  for (UINT q = 0; q < std::size(queues); q++) {
    CHECK(queues[q]->Signal(drains[q].Get(), 1));
    refs(drains[q].Get(), 1, "drain Signal");
    events[q] = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!expect(events[q] != nullptr, "drain event %u could not be created", q))
      ExitProcess(verdict());
    CHECK(drains[q]->SetEventOnCompletion(1, events[q]));
    expect(WaitForSingleObject(events[q], 100) == WAIT_TIMEOUT && drains[q]->GetCompletedValue() == 0,
           "queue %u drained before the live gate was signaled", q);
  }
  step("the live gate is signaled: both queues finish, with the later copy's bytes");
  CHECK(gate->Signal(1));
  for (UINT q = 0; q < std::size(queues); q++) {
    wait(events[q], q ? "compute queue drain" : "direct queue drain");
    expect(drains[q]->GetCompletedValue() == 1, "queue %u drain is not at 1", q);
    CHECK(drains[q]->SetEventOnCompletion(1, nullptr));
    refs(drains[q].Get(), 1, "completed drain");
    left = drains[q].Detach()->Release();
    expect(left == 0, "drain %u release returned %lu, want 0", q, left);
    CloseHandle(events[q]);
  }
  CHECK(readback->Map(0, nullptr, (void **)&bytes));
  for (UINT i = 0; i < signals; i++)
    expect(bytes[i] == i + 1, "copied word %u: %u, want %u", i, bytes[i], i + 1);
  readback->Unmap(0, nullptr);
  expect(device->GetDeviceRemovedReason() == S_OK, "releasing pending fences removed the device");
  return verdict();
}
