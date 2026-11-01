// contract: ID3D11Fence::SetEventOnCompletion for a value the fence already has raises the event at once, whether the
// value is the fence's initial value, one a context's Signal gave it, or one another fence object of the same shared
// fence was given.
// ID3D11Fence::SetEventOnCompletion: "Specifies an event that should be fired when the fence reaches a certain value."
// "This member function is equivalent to the Direct3D 12 ID3D12Fence::SetEventOnCompletion member function".
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/nf-d3d11_3-id3d11fence-seteventoncompletion
// ID3D11Device5::CreateFence: "InitialValue: The initial value for the fence."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11_4/nf-d3d11_4-id3d11device5-createfence
// ID3D11DeviceContext4::Signal: "Updates a fence to a specified value after all previous work has completed."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/nf-d3d11_3-id3d11devicecontext4-signal
// ID3D11Device5::OpenSharedFence opens the fence ID3D11Fence::CreateSharedHandle shared, with its value.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11_4/nf-d3d11_4-id3d11device5-opensharedfence
// the pages do not say when the event is raised for a value the fence already has. vkd3d's test_cpu_signal_fence
// (tests/d3d12.c), which runs on Windows, sets an event for every value up to GetCompletedValue of the equivalent
// ID3D12Fence and expects each one raised at a wait of 0 ms, and raised once; this test expects the same of
// ID3D11Fence, and that the event of the value after the fence's is not raised. initial values are 5 and 2^32 + 5,
// across UINT's width. the fences are unshared but one, so a run leaves a single shared fence name behind. there are
// no shaders.
#include "d3d11_test.hpp"
#include <d3d11_4.h>
#include <thread>

int
main() {
  std::thread([] {
    Sleep(60000);
    expect(false, "a fence call did not return within 60 s");
    ExitProcess(verdict());
  }).detach();
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Device5> device5;
  ComPtr<ID3D11DeviceContext4> context4;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                          &context));
  CHECK(device.As(&device5));
  CHECK(context.As(&context4));
  HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  if (!expect(event != nullptr, "CreateEvent: %lu", GetLastError()))
    return verdict();

  // the event is raised for each value from `first` to `reached` at once and once, and not for the value after it,
  // whose event stays set for the next Signal
  auto reached_at_once = [&](ID3D11Fence *fence, UINT64 first, UINT64 reached, const char *what) {
    expect(fence->GetCompletedValue() == reached, "%s: GetCompletedValue is %llu, want %llu", what,
           fence->GetCompletedValue(), reached);
    for (UINT64 value = first; value <= reached; value++) {
      CHECK(fence->SetEventOnCompletion(value, event));
      DWORD result = WaitForSingleObject(event, 0);
      // one raised late is let through, so that it is not taken for the next value's
      if (!expect(result == WAIT_OBJECT_0, "%s: the event for %llu, which the fence has, was not raised at once (%lu)",
                  what, value, result))
        WaitForSingleObject(event, 5000);
      // a second, late notification would come within this
      Sleep(20);
      expect(WaitForSingleObject(event, 0) == WAIT_TIMEOUT, "%s: the event for %llu was raised twice", what, value);
    }
    UINT64 next = reached + 1;
    CHECK(fence->SetEventOnCompletion(next, event));
    expect(WaitForSingleObject(event, 0) == WAIT_TIMEOUT, "%s: the event for %llu was raised early", what, next);
    return 0;
  };

  const UINT64 wide = (UINT64(1) << 32) + 5;
  for (UINT64 initial : {UINT64(5), wide}) {
    step("an unshared fence created at %llu", initial);
    ComPtr<ID3D11Fence> fence;
    CHECK(device5->CreateFence(initial, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    if (reached_at_once(fence.Get(), initial - 5, initial, "created"))
      return 1;

    step("the unshared fence after a context's Signal of %llu", initial + 10);
    // the event of initial + 1, set above, is raised by this Signal
    CHECK(context4->Signal(fence.Get(), initial + 10));
    context->Flush();
    DWORD result = WaitForSingleObject(event, 10000);
    if (!expect(result == WAIT_OBJECT_0, "the context's Signal did not complete (%lu)", result))
      return verdict();
    if (reached_at_once(fence.Get(), initial + 8, initial + 10, "signaled"))
      return 1;
    CHECK(context4->Signal(fence.Get(), initial + 11));
    context->Flush();
    result = WaitForSingleObject(event, 10000);
    expect(result == WAIT_OBJECT_0, "the event pending for %llu was not raised by its Signal (%lu)", initial + 11,
           result);
  }

  step("a second object of a shared fence created at %llu, after the first's Signal", wide);
  ComPtr<ID3D11Fence> fence, opened;
  CHECK(device5->CreateFence(wide, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)));
  HANDLE handle = nullptr;
  CHECK(fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle));
  CHECK(device5->OpenSharedFence(handle, IID_PPV_ARGS(&opened)));
  CloseHandle(handle);
  if (reached_at_once(opened.Get(), wide - 2, wide, "opened"))
    return 1;
  CHECK(context4->Signal(fence.Get(), wide + 10));
  context->Flush();
  DWORD result = WaitForSingleObject(event, 10000);
  if (!expect(result == WAIT_OBJECT_0, "the first object's Signal did not complete (%lu)", result))
    return verdict();
  if (reached_at_once(opened.Get(), wide + 8, wide + 10, "opened, after the first's Signal"))
    return 1;
  CHECK(context4->Signal(fence.Get(), wide + 11));
  context->Flush();
  result = WaitForSingleObject(event, 10000);
  expect(result == WAIT_OBJECT_0, "the opened object's pending event was not raised by the first's Signal (%lu)",
         result);
  CloseHandle(event);
  return verdict();
}
