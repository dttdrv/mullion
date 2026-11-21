// contract: ALL waits for every required value; ANY waits for at least one, across the full UINT64 value range.
// "wait for all fences before signaling the event"; ANY fires after "any one of the fence values has been reached"
// (Microsoft Learn, D3D12_MULTIPLE_FENCE_WAIT_FLAGS). independent registrations must all be delivered exactly once.
// zero-time event observations test a state; positive completion waits have no correctness deadline.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_multiple_fence_wait_flags
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device1-seteventonmultiplefencecompletion
#include "d3d12_test.hpp"
#include <array>

int main(int, char **) {
  ComPtr<ID3D12Device1> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  for (auto type : {D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COMPUTE, D3D12_COMMAND_LIST_TYPE_COPY}) {
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC q{type};
    CHECK(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
    for (auto flags : {D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL, D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY})
      for (UINT count : {1u, 2u, 3u, 31u, 32u, 33u, 65u}) {
        step("queue %u, %s, %u fences", type, flags == D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY ? "ANY" : "ALL", count);
        const UINT64 initial = UINT64(1) << (sizeof(UINT) * 8);
        std::vector<ComPtr<ID3D12Fence>> fences(count);
        std::vector<ID3D12Fence *> pointers(count);
        std::vector<UINT64> values(count);
        for (UINT i = 0; i < count; i++) {
          CHECK(device->CreateFence(initial, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fences[i])));
          pointers[i] = fences[i].Get();
          values[i] = initial + i + 2;
        }
        std::array<HANDLE, 2> events;
        for (auto &event : events) {
          event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
          if (!expect(event != nullptr, "event creation failed"))
            return verdict();
          CHECK(device->SetEventOnMultipleFenceCompletion(pointers.data(), values.data(), count, flags, event));
        }
        auto negative = [&] {
          bool quiet = true;
          for (auto event : events)
            quiet &= expect(WaitForSingleObject(event, 0) == WAIT_TIMEOUT, "event signaled before its condition");
          return quiet;
        };
        if (!negative())
          return verdict();
        for (UINT i = 0; i < count; i++)
          CHECK(fences[i]->Signal(values[i] - 1));
        if (!negative())
          return verdict();
        bool delivered = false;
        for (UINT done = 0; done < count; done++) {
          const UINT i = count - done - 1;
          if (done + 1 == count) {
            CHECK(queue->Signal(fences[i].Get(), values[i]));
          } else {
            CHECK(fences[i]->Signal(values[i]));
          }
          const bool satisfied = flags == D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY || done + 1 == count;
          if (satisfied && !delivered) {
            for (auto event : events)
              expect(WaitForSingleObject(event, INFINITE) == WAIT_OBJECT_0, "completion wait failed");
            delivered = true;
          } else if (!negative())
            return verdict();
        }
        // ANY may have completed before the queue's final signal; this separate notification drains that signal.
        CHECK(fences[0]->SetEventOnCompletion(values[0], events[0]));
        expect(WaitForSingleObject(events[0], INFINITE) == WAIT_OBJECT_0, "final queue signal did not complete");
        for (UINT i = 0; i < count; i++) {
          expect(fences[i]->GetCompletedValue() == values[i], "fence %u lost its high bits", i);
          CHECK(fences[i]->SetEventOnCompletion(values[i], nullptr));
        }
        CHECK(device->SetEventOnMultipleFenceCompletion(pointers.data(), values.data(), count, flags, nullptr));
        for (auto event : events) {
          expect(WaitForSingleObject(event, 0) == WAIT_TIMEOUT, "registration was delivered more than once");
          CloseHandle(event);
        }
      }
  }
  return verdict();
}
