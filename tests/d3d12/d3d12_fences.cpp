// contract: SetEventOnMultipleFenceCompletion sets its event once all fences reach their values (or, with
// D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY, once one does), not before; without an event it returns only then. one fence is
// signaled by the queue, the other by the CPU, and the event is sampled between the two.
#include "d3d12_test.hpp"

int
main(int argc, char **argv) {
  ComPtr<ID3D12Device1> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));

  unsigned failures = 0;
  auto expect = [&](const char *what, bool got, bool want) {
    if (got != want) {
      printf("%s: %s, want %s\n", what, got ? "set" : "not set", want ? "set" : "not set");
      failures++;
    }
  };
  for (auto flags : {D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL, D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY}) {
    bool any = flags == D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY;
    ComPtr<ID3D12Fence> fences[2];
    for (auto &f : fences)
      CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f)));
    ID3D12Fence *list[] = {fences[0].Get(), fences[1].Get()};
    const UINT64 values[] = {1, 2};
    auto event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    CHECK(device->SetEventOnMultipleFenceCompletion(list, values, 2, flags, event));
    // a wait long enough for a wrongly set event to show
    expect(any ? "any, before" : "all, before", WaitForSingleObject(event, 100) == WAIT_OBJECT_0, false);
    CHECK(queue->Signal(fences[0].Get(), 1));
    expect(any ? "any, after one" : "all, after one", WaitForSingleObject(event, 1000) == WAIT_OBJECT_0, any);
    CHECK(fences[1]->Signal(2));
    if (!any)
      expect("all, after both", WaitForSingleObject(event, 1000) == WAIT_OBJECT_0, true);
    CloseHandle(event);
    // without an event the call returns once the fences are done, which they are
    CHECK(device->SetEventOnMultipleFenceCompletion(list, values, 2, flags, nullptr));
  }
  printf("%s: %u wrong event states\n", failures ? "failed" : "passed", failures);
  return failures != 0;
}
