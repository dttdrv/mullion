// contract: a device that waited on a shared fence can be released before the fence gets the value it waited for;
// the fence's other users then signal it as before, and the process goes on unharmed.
// ID3D12Fence::SetEventOnCompletion: "Specifies an event that's raised when the fence reaches a certain value."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12fence-seteventoncompletion
// ID3D11Fence::SetEventOnCompletion: "Specifies an event that should be fired when the fence reaches a certain value."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/nf-d3d11_3-id3d11fence-seteventoncompletion
// IUnknown::Release: "When the reference count reaches zero, the object is freed from memory."
// https://learn.microsoft.com/en-us/windows/win32/api/unknwn/nf-unknwn-iunknown-release
// a D3D11 device makes the fence and signals it. a D3D12 device and a second D3D11 device open it, set events for a
// value it does not have yet, and are released. the D3D11 device then signals that value, as a game does across a
// device lost to removal. what waited on the released devices is gone with them, so a fault or a thread left spinning
// on it is the failure: the test watches the process's CPU time while it idles after the signal. each round leaves
// another released device behind.
#include "d3d12_test.hpp"
#include <d3d11_4.h>
#include <thread>

int
main(int argc, char **argv) {
  std::thread([] {
    Sleep(60000);
    expect(false, "a call did not return within 60 s");
    ExitProcess(verdict());
  }).detach();
  ComPtr<ID3D11Device> device11;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Device5> device5;
  ComPtr<ID3D11DeviceContext4> context4;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device11,
                          nullptr, &context));
  CHECK(device11.As(&device5));
  CHECK(context.As(&context4));
  ComPtr<ID3D11Fence> fence;
  CHECK(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)));
  HANDLE shared = nullptr;
  CHECK(fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &shared));
  HANDLE signaled = CreateEventA(nullptr, FALSE, FALSE, nullptr), waited = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  if (!expect(signaled && waited, "CreateEvent: %lu", GetLastError()))
    return verdict();
  // the CPU time of the process's threads, in 100 ns units
  auto busy = [] {
    FILETIME created, exited, kernel, user;
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    return (ULARGE_INTEGER{{kernel.dwLowDateTime, kernel.dwHighDateTime}}.QuadPart +
            ULARGE_INTEGER{{user.dwLowDateTime, user.dwHighDateTime}}.QuadPart);
  };

  const UINT rounds = 4;
  for (UINT round = 1; round <= rounds; round++) {
    step("round %u: a D3D12 device and a D3D11 device wait for %u and are released", round, round);
    {
      ComPtr<ID3D12Device> device;
      CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
      ComPtr<ID3D12Fence> opened;
      CHECK(device->OpenSharedHandle(shared, IID_PPV_ARGS(&opened)));
      CHECK(opened->SetEventOnCompletion(round, waited));
      ComPtr<ID3D11Device> other;
      ComPtr<ID3D11Device5> other5;
      CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &other,
                              nullptr, nullptr));
      CHECK(other.As(&other5));
      ComPtr<ID3D11Fence> opened11;
      CHECK(other5->OpenSharedFence(shared, IID_PPV_ARGS(&opened11)));
      CHECK(opened11->SetEventOnCompletion(round, waited));
      opened = nullptr;
      opened11 = nullptr;
      other5 = nullptr;
      ULONG left = device.Detach()->Release();
      expect(left == 0, "the D3D12 device has %lu references after its last release", left);
      left = other.Detach()->Release();
      expect(left == 0, "the second D3D11 device has %lu references after its last release", left);
    }
    // the released devices' threads are gone before the signal
    Sleep(100);

    step("round %u: the creating device signals %u", round, round);
    CHECK(fence->SetEventOnCompletion(round, signaled));
    CHECK(context4->Signal(fence.Get(), round));
    context->Flush();
    DWORD result = WaitForSingleObject(signaled, 10000);
    if (!expect(result == WAIT_OBJECT_0, "the creating device's event for %u was not raised (%lu)", round, result))
      return verdict();
    expect(fence->GetCompletedValue() == round, "GetCompletedValue: %llu, want %u", fence->GetCompletedValue(), round);

    step("round %u: the process idles after the signal", round);
    ULONGLONG before = busy();
    Sleep(500);
    ULONGLONG spent = busy() - before;
    // an idle process spends a few ms in 500; a thread spinning on what a released device left spends all of it
    expect(spent < 2000000, "the process spent %llu ms of CPU time in 500 ms of idling", spent / 10000);
  }
  CloseHandle(shared);
  CloseHandle(signaled);
  CloseHandle(waited);
  return verdict();
}
