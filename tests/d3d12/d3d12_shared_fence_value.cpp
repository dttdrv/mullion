// contract: a shared fence's events and waits for a value it already has are over at once, whether the value is the
// fence's initial value or one another fence object of the same shared fence gave it.
// ID3D12Device::CreateFence: "InitialValue: The initial value for the fence."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createfence
// ID3D12Fence::SetEventOnCompletion: "Specifies an event that's raised when the fence reaches a certain value."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12fence-seteventoncompletion
// ID3D12Device::OpenSharedHandle opens the fence that ID3D12Device::CreateSharedHandle shared, with its value.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-opensharedhandle
// the pages do not say when the event is raised for a value the fence already has. vkd3d's test_cpu_signal_fence
// (tests/d3d12.c), which runs on Windows, sets an event for every value up to GetCompletedValue and expects each one
// raised at a wait of 0 ms, and raised once. this test expects the same of a shared fence, for its initial value, for
// a value given by another object of the same fence (a CPU Signal and a queue Signal), and of
// SetEventOnMultipleFenceCompletion. a queue Wait for a reached value must let a later Signal through.
#include "d3d12_test.hpp"
#include <thread>

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
  HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  if (!expect(event != nullptr, "CreateEvent: %lu", GetLastError()))
    return verdict();

  // the event is raised for each value up to `reached` at once and once, and not for the value after it
  auto reached_at_once = [&](ID3D12Fence *fence, UINT64 first, UINT64 reached, const char *what) {
    for (UINT64 value = first; value <= reached; value++) {
      CHECK(fence->SetEventOnCompletion(value, event));
      DWORD result = WaitForSingleObject(event, 0);
      expect(result == WAIT_OBJECT_0, "%s: the event for %llu, which the fence has, was not raised at once (%lu)", what,
             value, result);
      expect(WaitForSingleObject(event, 0) == WAIT_TIMEOUT, "%s: the event for %llu was raised twice", what, value);
    }
    ID3D12Fence *fences[] = {fence};
    for (auto flags : {D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL, D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY}) {
      CHECK(device->SetEventOnMultipleFenceCompletion(fences, &reached, 1, flags, event));
      DWORD result = WaitForSingleObject(event, 0);
      expect(result == WAIT_OBJECT_0, "%s: the multiple-fence event (%s) for %llu was not raised at once (%lu)", what,
             flags == D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL ? "all" : "any", reached, result);
    }
    UINT64 next = reached + 1;
    CHECK(fence->SetEventOnCompletion(next, event));
    expect(WaitForSingleObject(event, 0) == WAIT_TIMEOUT, "%s: the event for %llu was raised early", what, next);
    return 0;
  };

  const UINT64 wide = (UINT64(1) << 32) + 5;
  for (UINT64 initial : {UINT64(5), wide}) {
    step("a shared fence created at %llu", initial);
    ComPtr<ID3D12Fence> fence, opened;
    CHECK(device->CreateFence(initial, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)));
    expect(fence->GetCompletedValue() == initial, "GetCompletedValue: %llu, want %llu", fence->GetCompletedValue(),
           initial);
    if (reached_at_once(fence.Get(), initial - 5, initial, "created"))
      return 1;

    step("a second object of the fence, opened at %llu, after the first's CPU Signal", initial);
    HANDLE handle = nullptr;
    CHECK(device->CreateSharedHandle(fence.Get(), nullptr, GENERIC_ALL, nullptr, &handle));
    CHECK(device->OpenSharedHandle(handle, IID_PPV_ARGS(&opened)));
    CloseHandle(handle);
    // the event of the value after the initial one, set on the first object above, is raised by this Signal
    CHECK(fence->Signal(initial + 10));
    expect(WaitForSingleObject(event, 0) == WAIT_OBJECT_0, "the first object's event was not raised by its Signal");
    expect(opened->GetCompletedValue() == initial + 10, "opened GetCompletedValue: %llu, want %llu",
           opened->GetCompletedValue(), initial + 10);
    if (reached_at_once(opened.Get(), initial + 8, initial + 10, "opened, after a CPU Signal"))
      return 1;
    // the event of initial + 11 is pending on the opened object: the first object's Signal raises it too
    CHECK(fence->Signal(initial + 11));
    DWORD result = WaitForSingleObject(event, 10000);
    expect(result == WAIT_OBJECT_0, "the opened object's event was not raised by the first's Signal (%lu)", result);

    step("the opened object, after the first's queue Signal of %llu", initial + 20);
    CHECK(queue->Signal(fence.Get(), initial + 20));
    CHECK(fence->SetEventOnCompletion(initial + 20, event));
    result = WaitForSingleObject(event, 10000);
    if (!expect(result == WAIT_OBJECT_0, "the queue Signal did not complete (%lu)", result))
      return verdict();
    if (reached_at_once(opened.Get(), initial + 19, initial + 20, "opened, after a queue Signal"))
      return 1;
    CHECK(fence->Signal(initial + 21));
    result = WaitForSingleObject(event, 10000);
    expect(result == WAIT_OBJECT_0, "the opened object's event was not raised by the first's Signal (%lu)", result);

    step("a queue Wait on the opened object for a value it has");
    ComPtr<ID3D12Fence> drain;
    CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&drain)));
    CHECK(queue->Wait(opened.Get(), initial + 21));
    CHECK(queue->Signal(drain.Get(), 1));
    CHECK(drain->SetEventOnCompletion(1, event));
    result = WaitForSingleObject(event, 10000);
    expect(result == WAIT_OBJECT_0, "a Signal after the Wait did not complete (%lu)", result);
  }
  CloseHandle(event);
  return verdict();
}
