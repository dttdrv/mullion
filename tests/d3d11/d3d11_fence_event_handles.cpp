// contract: an event given to ID3D11Fence::SetEventOnCompletion or ID3D11DeviceContext3::Flush1 is the event object
// the handle named when the call was made. the caller may close its handle before the GPU gets there: the object is
// still raised, and an object that takes the closed handle's value later is not.
// ID3D11Fence::SetEventOnCompletion: "hEvent: A handle to the event object."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/nf-d3d11_3-id3d11fence-seteventoncompletion
// ID3D11DeviceContext3::Flush1: "hEvent: An optional event handle. When specified, this method will set hEvent
// when the commands submitted by this flush are completed."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/nf-d3d11_3-id3d11devicecontext3-flush1
// a handle is a process's name for an object, which lives while anything references it (Handles and Objects,
// https://learn.microsoft.com/en-us/windows/win32/sysinfo/handles-and-objects); the runtime raises the object, as
// Windows' kernel does by referencing it when the event is set. Wine gives a new handle the lowest free value, so the
// next event made takes the value of the one closed; the test says so when it does not, and checks the rest.
// the fence's value comes from ID3D11DeviceContext4::Signal after a Wait for a fence the CPU holds back, so the GPU
// cannot get there before the handle is closed. a wait for a value the fence never gets ends with the device that
// waited: once it is released, nothing of the wait stays, which the process's handles show
// (NtQuerySystemInformation(SystemExtendedHandleInformation), before and after; 4 of 32 waits' worth may come and go
// with threads). there are no shaders.
#include "d3d11_test.hpp"
#include <d3d11_4.h>
#include <algorithm>
#include <vector>

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
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Device5> device5;
  ComPtr<ID3D11DeviceContext4> context4;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                          &context));
  CHECK(device.As(&device5));
  CHECK(context.As(&context4));
  auto event = [](BOOL manual) {
    HANDLE handle = CreateEventA(nullptr, manual, FALSE, nullptr);
    if (!expect(handle != nullptr, "CreateEvent: %lu", GetLastError()))
      ExitProcess(verdict());
    return handle;
  };

  for (bool flush : {false, true}) {
    step("%s, with the caller's handle closed", flush ? "Flush1" : "SetEventOnCompletion");
    ComPtr<ID3D11Fence> fence, gate;
    CHECK(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    CHECK(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&gate)));
    // `given` is the handle passed and closed; `kept` names the same object and stays open
    HANDLE given = event(FALSE), kept, done = event(FALSE);
    if (!expect(DuplicateHandle(GetCurrentProcess(), given, GetCurrentProcess(), &kept, 0, FALSE,
                                DUPLICATE_SAME_ACCESS), "DuplicateHandle: %lu", GetLastError()))
      return verdict();
    CHECK(context4->Wait(gate.Get(), 1));
    CHECK(context4->Signal(fence.Get(), 1));
    if (flush) {
      context4->Flush1(D3D11_CONTEXT_TYPE_ALL, given);
    } else {
      CHECK(fence->SetEventOnCompletion(1, given));
      context->Flush();
    }
    HANDLE closed = given;
    CloseHandle(given);
    // a manual-reset event stays raised, so a late raise is seen however late it comes
    HANDLE unrelated = event(TRUE);
    if (unrelated != closed)
      printf("note: the new event is %p, not the closed handle %p\n", unrelated, closed);
    // the work is held back until the CPU gives the gate its value
    expect(WaitForSingleObject(kept, 100) == WAIT_TIMEOUT && fence->GetCompletedValue() == 0,
           "the work passed the Wait before the gate was signaled");
    CHECK(fence->SetEventOnCompletion(1, done));
    // a second device signals the gate, as the CPU cannot signal a D3D11 fence
    ComPtr<ID3D11Device> other;
    ComPtr<ID3D11DeviceContext> other_context;
    ComPtr<ID3D11DeviceContext4> other4;
    CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &other,
                            nullptr, &other_context));
    CHECK(other_context.As(&other4));
    ComPtr<ID3D11Device5> other5;
    CHECK(other.As(&other5));
    ComPtr<ID3D11Fence> opened;
    HANDLE shared = nullptr;
    CHECK(gate->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &shared));
    CHECK(other5->OpenSharedFence(shared, IID_PPV_ARGS(&opened)));
    CloseHandle(shared);
    CHECK(other4->Signal(opened.Get(), 1));
    other_context->Flush();
    DWORD result = WaitForSingleObject(done, 10000);
    if (!expect(result == WAIT_OBJECT_0, "the fence did not reach 1 (%lu)", result))
      return verdict();
    result = WaitForSingleObject(kept, 10000);
    expect(result == WAIT_OBJECT_0, "the event object whose handle was closed was not raised (%lu)", result);
    // what raised the object has nothing left to raise
    Sleep(100);
    result = WaitForSingleObject(unrelated, 0);
    expect(result == WAIT_TIMEOUT, "the event that took the closed handle's value %p was raised (%lu)", closed,
           result);
    for (HANDLE handle : {unrelated, kept, done})
      CloseHandle(handle);
  }

  // a device with a fence that waits for nothing, made and released, so the count starts from what a device leaves
  auto waits = [&](UINT count) {
    ComPtr<ID3D11Device> waiting;
    ComPtr<ID3D11Device5> waiting5;
    CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &waiting,
                            nullptr, nullptr));
    CHECK(waiting.As(&waiting5));
    ComPtr<ID3D11Fence> fence;
    CHECK(waiting5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    for (UINT i = 0; i < count; i++) {
      HANDLE given = event(FALSE);
      CHECK(fence->SetEventOnCompletion(1000 + i, given));
      CloseHandle(given);
    }
    fence = nullptr;
    waiting5 = nullptr;
    ULONG left = waiting.Detach()->Release();
    expect(left == 0, "the waiting device has %lu references after its last release", left);
    return 0;
  };
  step("a released device's waits for values its fence never got");
  if (waits(0))
    return 1;
  Sleep(100);
  LONG before = handles();
  const UINT count = 32;
  if (waits(count))
    return 1;
  Sleep(100);
  LONG after = handles();
  printf("%u waits on a released device: %ld handles before, %ld after\n", count, before, after);
  expect(before > 0 && after - before <= 4, "%u waits on a released device left %ld handles behind", count,
         after - before);
  return verdict();
}
