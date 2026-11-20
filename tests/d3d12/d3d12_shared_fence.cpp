// contract: both APIs open the same fence, including its CPU signals, GPU signals and waits.
// "In DirectX 12, you can open the shared fence handle with the ID3D12Device::OpenSharedHandle method."
// (ID3D11Fence::CreateSharedHandle, Remarks).
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/nf-d3d11_3-id3d11fence-createsharedhandle
// "a shared fence is opened in DirectX 11 with the ID3D11Device5::OpenSharedFence method"
// (ID3D12Device::CreateSharedHandle, Remarks). Access accepts only GENERIC_ALL; Name is optional.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createsharedhandle
// "If hEvent is a null handle, then this API will not return until the specified fence value(s) have been reached."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12fence-seteventoncompletion
// copies need no shaders. the D3D11 readback is relayed through the CPU into a D3D12 upload before the D3D11 signal,
// so this tests fence sharing without requiring shared resources or heaps. events prove each consumer was held.
#include "d3d12_test.hpp"
#include <d3d11_4.h>
#include <limits>
#include <thread>

int
main(int argc, char **argv) {
  const DWORD observation_ms = 100, completion_ms = 10000;
  auto wait = [&](HANDLE event, const char *what) {
    if (!expect(WaitForSingleObject(event, completion_ms) == WAIT_OBJECT_0, "%s did not complete", what))
      ExitProcess(1);
  };
  auto event = [&] {
    HANDLE handle = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!expect(handle != nullptr, "CreateEvent: %lu", GetLastError()))
      ExitProcess(1);
    return handle;
  };
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D11Device> device11;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Device5> device5;
  ComPtr<ID3D11DeviceContext4> context4;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                          &device11, nullptr, &context));
  CHECK(device11.As(&device5));
  CHECK(context.As(&context4));

  step("unshared fences, invalid handles, access and required outputs");
  ComPtr<ID3D12Fence> ordinary, invalid;
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&ordinary)));
  HANDLE handle = nullptr;
  expect(device->CreateSharedHandle(ordinary.Get(), nullptr, GENERIC_ALL, nullptr, &handle) == E_INVALIDARG && !handle,
         "an unshared fence was exported");
  expect(FAILED(device->CreateSharedHandle(nullptr, nullptr, GENERIC_ALL, nullptr, &handle)) && !handle,
         "a null object was exported");
  expect(FAILED(device->OpenSharedHandle(nullptr, IID_PPV_ARGS(&invalid))) && !invalid, "a null handle was opened");
  HANDLE not_fence = event();
  expect(FAILED(device->OpenSharedHandle(not_fence, IID_PPV_ARGS(&invalid))) && !invalid,
         "an event handle was opened as a fence");
  CloseHandle(not_fence);
  expect(FAILED(device->OpenSharedHandleByName(nullptr, GENERIC_ALL, &handle)) && !handle,
         "a null name was opened");

  const UINT64 wide = UINT64(std::numeric_limits<UINT>::max()) + 1;
  for (bool from11 : {false, true}) {
    for (UINT64 initial : {UINT64(0), wide}) {
      step("D3D%u-created fence initially at %llu", from11 ? 11 : 12, initial);
      ComPtr<ID3D12Fence> fence, twice;
      ComPtr<ID3D11Fence> fence11;
      WCHAR name[MAX_PATH];
      swprintf(name, ARRAYSIZE(name), L"mullion_fence_%lu_%u_%llu", GetCurrentProcessId(), from11, initial);
      if (from11) {
        CHECK(device5->CreateFence(initial, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence11)));
        CHECK(fence11->CreateSharedHandle(nullptr, GENERIC_ALL, name, &handle));
        CHECK(device->OpenSharedHandle(handle, IID_PPV_ARGS(&fence)));
      } else {
        CHECK(device->CreateFence(initial, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)));
        CHECK(device->CreateSharedHandle(fence.Get(), nullptr, GENERIC_ALL, name, &handle));
        CHECK(device5->OpenSharedFence(handle, IID_PPV_ARGS(&fence11)));
      }
      CHECK(device->OpenSharedHandle(handle, IID_PPV_ARGS(&twice)));
      CHECK(device->OpenSharedHandle(handle, __uuidof(ID3D12Fence), nullptr));
      ComPtr<ID3D12Fence1> fence1;
      CHECK(device->OpenSharedHandle(handle, IID_PPV_ARGS(&fence1)));
      expect(!!(fence1->GetCreationFlags() & D3D12_FENCE_FLAG_SHARED), "opened Fence1 lost its shared flag");
      expect(fence->GetCompletedValue() == initial && fence11->GetCompletedValue() == initial &&
                 twice->GetCompletedValue() == initial, "opened fences lost their initial value");
      HANDLE named = nullptr;
      CHECK(device->OpenSharedHandleByName(name, GENERIC_ALL, &named));
      // D3DKMT names are found in any case: Wine's win32u d3dkmt test, which runs on Windows, opens a resource by
      // its lower-case name
      WCHAR other_case[MAX_PATH];
      wcscpy(other_case, name);
      for (WCHAR *c = other_case; *c; c++)
        *c = towupper(*c);
      HANDLE upper = nullptr;
      ComPtr<ID3D12Fence> by_upper;
      expect(SUCCEEDED(device->OpenSharedHandleByName(other_case, GENERIC_ALL, &upper)) && upper &&
                 SUCCEEDED(device->OpenSharedHandle(upper, IID_PPV_ARGS(&by_upper))) &&
                 by_upper->GetCompletedValue() == initial,
             "a name in another case did not open the fence");
      if (upper)
        CloseHandle(upper);
      ComPtr<ID3D12Fence> by_name;
      CHECK(device->OpenSharedHandle(named, IID_PPV_ARGS(&by_name)));
      CloseHandle(named);
      CloseHandle(handle);
      handle = nullptr;
      expect(FAILED(device->CreateSharedHandle(fence.Get(), nullptr, GENERIC_READ, nullptr, &handle)) && !handle,
             "wrong export access was accepted");
      expect(FAILED(device->CreateSharedHandle(fence.Get(), nullptr, GENERIC_ALL, nullptr, nullptr)),
             "a null handle output was accepted");
      expect(FAILED(device->OpenSharedHandleByName(name, GENERIC_READ, &handle)) && !handle,
             "wrong named access was accepted");
      expect(FAILED(device->OpenSharedHandleByName(name, GENERIC_ALL, nullptr)),
             "a null named handle output was accepted");
      expect(FAILED(device->OpenSharedHandle(nullptr, __uuidof(ID3D12Fence), nullptr)),
             "an invalid handle was accepted with an optional null output");

      const UINT count = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT / sizeof(UINT) + 1;
      std::vector<UINT> wanted(count), zero(count);
      for (UINT i = 0; i < count; i++)
        wanted[i] = i + 1;
      const UINT bytes = count * sizeof(UINT);
      D3D11_BUFFER_DESC desc{bytes, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE};
      D3D11_SUBRESOURCE_DATA data{zero.data()};
      ComPtr<ID3D11Buffer> source11, staging11;
      CHECK(device11->CreateBuffer(&desc, &data, &source11));
      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      CHECK(device11->CreateBuffer(&desc, nullptr, &staging11));

      step("CPU Signal releases a held D3D11 copy, at a 64-bit value");
      const UINT64 target = wide + 1;
      HANDLE done = event();
      HANDLE seen11 = event();
      CHECK(fence11->SetEventOnCompletion(target, seen11));
      ComPtr<ID3D11Fence> completed11;
      CHECK(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&completed11)));
      CHECK(completed11->SetEventOnCompletion(1, done));
      HANDLE submitted = event();
      DWORD queued = WAIT_FAILED, early = WAIT_FAILED;
      DWORD seen_early = WAIT_FAILED;
      HRESULT written = E_FAIL, released = E_FAIL;
      std::thread release([&] {
        queued = WaitForSingleObject(submitted, completion_ms);
        if (queued == WAIT_OBJECT_0) {
          early = WaitForSingleObject(done, observation_ms);
          seen_early = WaitForSingleObject(seen11, 0);
          D3D11_MAPPED_SUBRESOURCE mapped{};
          written = context->Map(source11.Get(), 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &mapped);
          if (SUCCEEDED(written)) {
            memcpy(mapped.pData, wanted.data(), bytes);
            context->Unmap(source11.Get(), 0);
          }
        }
        released = twice->Signal(target);
      });
      HRESULT waited = context4->Wait(fence11.Get(), target);
      HRESULT signaled = E_FAIL;
      if (SUCCEEDED(waited)) {
        context->CopyResource(staging11.Get(), source11.Get());
        signaled = context4->Signal(completed11.Get(), 1);
        context->Flush();
      }
      BOOL notified = SetEvent(submitted);
      release.join();
      CHECK(waited);
      CHECK(signaled);
      CHECK(written);
      CHECK(released);
      expect(notified && queued == WAIT_OBJECT_0, "D3D11 Wait blocked submission: %lu", queued);
      expect(early == WAIT_TIMEOUT, "D3D11 consumer passed the pending wait: %lu", early);
      expect(seen_early == WAIT_TIMEOUT, "the shared D3D11 event fired before the CPU signal");
      CloseHandle(submitted);
      D3D11_MAPPED_SUBRESOURCE mapped11{};
      wait(done, "D3D11 copy after CPU Signal");
      wait(seen11, "shared D3D11 event after CPU Signal");
      CloseHandle(seen11);
      CHECK(context->Map(staging11.Get(), 0, D3D11_MAP_READ, 0, &mapped11));
      expect(!memcmp(mapped11.pData, wanted.data(), bytes), "D3D11 copied before the CPU producer wrote its data");
      context->Unmap(staging11.Get(), 0);
      expect(fence->GetCompletedValue() == target && fence11->GetCompletedValue() == target &&
                 by_name->GetCompletedValue() == target, "CPU Signal is not visible to every wrapper");
      CloseHandle(done);

      step("D3D11 GPU Signal fires D3D12 events and releases a held D3D12 copy");
      auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
      auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
      if (!expect(upload && readback, "D3D12 copy buffers were not created"))
        return verdict();
      void *mapped;
      CHECK(upload->Map(0, nullptr, &mapped));
      memset(mapped, 0, bytes);
      CHECK(forget(readback.Get()));
      ComPtr<ID3D12CommandAllocator> allocator;
      ComPtr<ID3D12GraphicsCommandList> list;
      CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
      CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
      list->CopyBufferRegion(readback.Get(), 0, upload.Get(), 0, bytes);
      CHECK(list->Close());
      ComPtr<ID3D12Fence> drain;
      CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&drain)));
      HANDLE reached = event(), copied = event(), blocked = event(), entered = event();
      HANDLE blocked11 = event(), entered11 = event();
      CHECK(fence->SetEventOnCompletion(target + 1, reached));
      CHECK(drain->SetEventOnCompletion(1, copied));
      HRESULT blocking_hr = E_FAIL;
      std::thread blocking([&] {
        SetEvent(entered);
        blocking_hr = twice->SetEventOnCompletion(target + 1, nullptr);
        SetEvent(blocked);
      });
      HRESULT blocking11_hr = E_FAIL;
      std::thread blocking11([&] {
        SetEvent(entered11);
        blocking11_hr = fence11->SetEventOnCompletion(target + 1, nullptr);
        SetEvent(blocked11);
      });
      wait(entered, "blocking waiter entry");
      wait(entered11, "blocking D3D11 waiter entry");
      submitted = event();
      queued = early = WAIT_FAILED;
      DWORD event_early = WAIT_FAILED, block_early = WAIT_FAILED;
      DWORD block11_early = WAIT_FAILED;
      written = released = E_FAIL;
      release = std::thread([&] {
        queued = WaitForSingleObject(submitted, completion_ms);
        if (queued == WAIT_OBJECT_0) {
          early = WaitForSingleObject(copied, observation_ms);
          event_early = WaitForSingleObject(reached, 0);
          block_early = WaitForSingleObject(blocked, 0);
          block11_early = WaitForSingleObject(blocked11, 0);
        }
        // the relay precedes the shared GPU signal, with no shared resource
        context->CopyResource(staging11.Get(), source11.Get());
        D3D11_MAPPED_SUBRESOURCE result{};
        written = context->Map(staging11.Get(), 0, D3D11_MAP_READ, 0, &result);
        if (SUCCEEDED(written)) {
          memcpy(mapped, result.pData, bytes);
          context->Unmap(staging11.Get(), 0);
        }
        released = context4->Signal(fence11.Get(), target + 1);
        context->Flush();
      });
      waited = queue->Wait(fence.Get(), target + 1);
      signaled = E_FAIL;
      if (SUCCEEDED(waited)) {
        ID3D12CommandList *copy = list.Get();
        queue->ExecuteCommandLists(1, &copy);
        signaled = queue->Signal(drain.Get(), 1);
      }
      notified = SetEvent(submitted);
      release.join();
      CHECK(waited);
      CHECK(signaled);
      CHECK(written);
      CHECK(released);
      expect(notified && queued == WAIT_OBJECT_0, "D3D12 Wait blocked submission: %lu", queued);
      expect(early == WAIT_TIMEOUT, "D3D12 consumer passed the pending wait: %lu", early);
      expect(event_early == WAIT_TIMEOUT && block_early == WAIT_TIMEOUT && block11_early == WAIT_TIMEOUT,
             "an event or blocking wait completed before the D3D11 signal");
      CloseHandle(submitted);
      wait(reached, "D3D12 event after D3D11 GPU Signal");
      wait(blocked, "blocking D3D12 wait after D3D11 GPU Signal");
      wait(blocked11, "blocking D3D11 wait after D3D11 GPU Signal");
      blocking.join();
      blocking11.join();
      CHECK(blocking_hr);
      CHECK(blocking11_hr);
      wait(copied, "D3D12 copy after D3D11 GPU Signal");
      upload->Unmap(0, nullptr);
      CHECK(readback->Map(0, nullptr, &mapped));
      expect(!memcmp(mapped, wanted.data(), bytes), "D3D12 did not read the D3D11 producer's data");
      readback->Unmap(0, nullptr);
      expect(fence->GetCompletedValue() == target + 1 && fence11->GetCompletedValue() == target + 1 &&
                 twice->GetCompletedValue() == target + 1, "D3D11 GPU Signal is not visible to every wrapper");
      for (HANDLE e : {reached, copied, blocked, entered, blocked11, entered11})
        CloseHandle(e);
    }
  }
  return verdict();
}
