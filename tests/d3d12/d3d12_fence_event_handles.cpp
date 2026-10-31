// contract: an event given to SetEventOnCompletion or SetEventOnMultipleFenceCompletion is the event object the handle
// named when the call was made. the caller may close its handle before the fence reaches the value: the object is
// still raised, and an object that takes the closed handle's value later is not.
// ID3D12Fence::SetEventOnCompletion: "hEvent: A handle to the event object."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12fence-seteventoncompletion
// ID3D12Device1::SetEventOnMultipleFenceCompletion: "hEvent: A handle to the event object."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device1-seteventonmultiplefencecompletion
// a handle is a process's name for an object, which lives while anything references it (Handles and Objects,
// https://learn.microsoft.com/en-us/windows/win32/sysinfo/handles-and-objects); the fence raises the object, as
// Windows' kernel does by referencing it when the event is set. Wine gives a new handle the lowest free value, so the
// next event made takes the value of the one closed; the test says so when it does not, and checks the rest.
// the fences are an unshared one, reached by a CPU and a queue Signal, and a shared one, reached through another
// object of the same fence.
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
  auto event = [](BOOL manual) {
    HANDLE handle = CreateEventA(nullptr, manual, FALSE, nullptr);
    if (!expect(handle != nullptr, "CreateEvent: %lu", GetLastError()))
      ExitProcess(verdict());
    return handle;
  };

  enum Reach { cpu, queued, other };
  for (Reach reach : {cpu, queued, other})
    for (bool multiple : {false, true}) {
      const char *how = reach == cpu ? "a CPU Signal" : reach == queued ? "a queue Signal" : "another fence object";
      step("%s, reached by %s, with the caller's handle closed", multiple ? "SetEventOnMultipleFenceCompletion" :
           "SetEventOnCompletion", how);
      ComPtr<ID3D12Fence> fence, waited;
      CHECK(device->CreateFence(0, reach == other ? D3D12_FENCE_FLAG_SHARED : D3D12_FENCE_FLAG_NONE,
                                IID_PPV_ARGS(&fence)));
      waited = fence;
      if (reach == other) {
        HANDLE shared = nullptr;
        CHECK(device->CreateSharedHandle(fence.Get(), nullptr, GENERIC_ALL, nullptr, &shared));
        CHECK(device->OpenSharedHandle(shared, IID_PPV_ARGS(&waited)));
        CloseHandle(shared);
      }
      // `given` is the handle passed and closed; `kept` names the same object and stays open
      HANDLE given = event(FALSE), kept;
      if (!expect(DuplicateHandle(GetCurrentProcess(), given, GetCurrentProcess(), &kept, 0, FALSE,
                                  DUPLICATE_SAME_ACCESS), "DuplicateHandle: %lu", GetLastError()))
        return verdict();
      const UINT64 value = 1;
      ID3D12Fence *fences[] = {waited.Get()};
      CHECK(multiple ? device->SetEventOnMultipleFenceCompletion(fences, &value, 1, D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL,
                                                                 given)
                     : waited->SetEventOnCompletion(value, given));
      HANDLE closed = given;
      CloseHandle(given);
      // a manual-reset event stays raised, so a late raise is seen however late it comes
      HANDLE unrelated = event(TRUE);
      if (unrelated != closed)
        printf("note: the new event is %p, not the closed handle %p\n", unrelated, closed);
      CHECK(reach == queued ? queue->Signal(fence.Get(), value) : fence->Signal(value));
      DWORD result = WaitForSingleObject(kept, 10000);
      expect(result == WAIT_OBJECT_0, "the event object whose handle was closed was not raised (%lu)", result);
      // what raised the object has nothing left to raise
      Sleep(100);
      result = WaitForSingleObject(unrelated, 0);
      expect(result == WAIT_TIMEOUT, "the event that took the closed handle's value %p was raised (%lu)", closed,
             result);
      CloseHandle(unrelated);
      CloseHandle(kept);
    }
  return verdict();
}
