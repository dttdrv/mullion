// contract: explicit removal makes the device unusable and completes monitored fences with the removal sentinel.
// "device removal triggers all fences to be signaled to UINT64_MAX" (Microsoft Learn, RemoveDevice).
// GetCompletedValue: "If the device has been removed, the return value will be UINT64_MAX."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device5-removedevice
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12fence-getcompletedvalue
// vkd3d-proton, test_cpu_signal_fence: "Event is signaled immediately when value <= GetCompletedValue()."
// https://github.com/HansKristian-Work/vkd3d-proton/blob/master/tests/d3d12_sync.c
// Present returns DXGI_ERROR_DEVICE_REMOVED after removal, including DXGI_PRESENT_TEST.
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-present
// Work submitted to a removed device does not run: after a GPU hang the application keeps submitting until it sees the
// removal, and every command buffer that reached the GPU could hang it again.
#include "d3d12_test.hpp"
#include <dxgi1_4.h>
#include <limits>
#include <cstring>
#include <thread>

int main(int, char **) {
  ComPtr<ID3D12Device5> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> presenting;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&presenting)));
  ComPtr<IDXGIFactory4> factory;
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  DXGI_SWAP_CHAIN_DESC1 desc{1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, FALSE, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT, 2,
                            DXGI_SCALING_STRETCH, DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_ALPHA_MODE_UNSPECIFIED, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT};
  HWND window = CreateWindowA("static", "d3d12_device_removal", WS_POPUP, 0, 0, desc.Width, desc.Height,
                              nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
  if (!expect(window != nullptr, "CreateWindow"))
    return verdict();
  ComPtr<IDXGISwapChain1> chain;
  CHECK(factory->CreateSwapChainForHwnd(presenting.Get(), window, &desc, nullptr, nullptr, &chain));
  expect(SUCCEEDED(chain->Present(0, DXGI_PRESENT_TEST)), "usable device rejected Present(TEST)");
  ComPtr<ID3D12Fence> fence;
  const UINT64 removed = std::numeric_limits<UINT64>::max();
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
  HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  if (!expect(event != nullptr, "CreateEvent"))
    return verdict();
  step("register a removal notification before explicit removal");
  CHECK(fence->SetEventOnCompletion(removed, event));
  expect(WaitForSingleObject(event, 0) == WAIT_TIMEOUT, "notification fired before removal");
  ComPtr<ID3D12Fence> other, raced;
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&other)));
  HANDLE other_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  if (!expect(other_event != nullptr, "CreateEvent"))
    return verdict();
  CHECK(other->SetEventOnCompletion(1, other_event));
  {
    ComPtr<ID3D12Fence> discarded;
    CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&discarded)));
  }
  step("record a copy to submit after removal");
  const UINT64 size = 256;
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, size, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, size, D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> copy;
  if (!expect(upload && readback, "copy buffers"))
    return verdict();
  void *bytes;
  CHECK(upload->Map(0, nullptr, &bytes));
  memset(bytes, 0x5a, size);
  upload->Unmap(0, nullptr);
  CHECK(forget(readback.Get()));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&copy)));
  copy->CopyBufferRegion(readback.Get(), 0, upload.Get(), 0, size);
  CHECK(copy->Close());
  ComPtr<IDXGISwapChain2> chain2;
  CHECK(chain.As(&chain2));
  HANDLE waitable = chain2->GetFrameLatencyWaitableObject();
  expect(WaitForSingleObject(waitable, 1000) == WAIT_OBJECT_0, "initial latency token");
  ComPtr<ID3D12Fence> held;
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&held)));
  presenting->Wait(held.Get(), 1);
  expect(SUCCEEDED(chain->Present(0, 0)), "held present");
  HRESULT created = E_FAIL;
  std::thread creation([&] { created = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&raced)); });
  device->RemoveDevice();
  creation.join();
  expect(WaitForSingleObject(waitable, 2000) == WAIT_OBJECT_0, "the held present gives its latency token back");
  step("observe removal after concurrent CreateFence returned %08lx", created);
  expect(created == S_OK ? bool(raced) : created == DXGI_ERROR_DEVICE_REMOVED && !raced,
         "concurrent fence creation: %08lx", created);
  if (raced)
    expect(raced->GetCompletedValue() == removed, "concurrently created fence missed removal");
  HRESULT reason = device->GetDeviceRemovedReason();
  expect(FAILED(reason), "RemoveDevice left the device usable: %08lx", reason);
  expect(fence->GetCompletedValue() == removed, "removal did not complete the fence");
  if (FAILED(reason) && fence->GetCompletedValue() == removed) {
    expect(WaitForSingleObject(event, INFINITE) == WAIT_OBJECT_0, "removal notification failed");
    CHECK(fence->SetEventOnCompletion(removed, nullptr));
    expect(other->GetCompletedValue() == removed, "second fence missed removal");
    expect(WaitForSingleObject(other_event, INFINITE) == WAIT_OBJECT_0, "ordinary completion missed removal");
    other->Signal(0);
    CHECK(other->SetEventOnCompletion(removed, other_event));
    expect(WaitForSingleObject(other_event, 0) == WAIT_OBJECT_0, "late signal rewound removal");
    expect(WaitForSingleObject(other_event, 0) == WAIT_TIMEOUT, "completion was delivered twice");
    if (raced) {
      CHECK(raced->SetEventOnCompletion(removed, other_event));
      expect(WaitForSingleObject(other_event, 0) == WAIT_OBJECT_0,
             "concurrently created fence missed removal notification");
    }
    void *after = fence.Get();
    HRESULT fence_hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), &after);
    expect(fence_hr == DXGI_ERROR_DEVICE_REMOVED && !after, "removed fence creation: %08lx", fence_hr);
    if (fence_hr == S_OK && after)
      static_cast<ID3D12Fence *>(after)->Release();
    for (UINT flags : {0u, UINT(DXGI_PRESENT_TEST)})
      expect(chain->Present(0, flags) == DXGI_ERROR_DEVICE_REMOVED, "removed Present(%u) succeeded", flags);
    step("work submitted after removal does not run");
    ID3D12CommandList *lists[] = {copy.Get()};
    presenting->ExecuteCommandLists(1, lists);
    presenting->Signal(fence.Get(), removed);
    Sleep(200);
    if (expect(SUCCEEDED(readback->Map(0, nullptr, &bytes)), "Map of a readback buffer after removal")) {
      bool untouched = true;
      for (UINT64 i = 0; i < size; i++)
        untouched &= static_cast<uint8_t *>(bytes)[i] == 0xff;
      expect(untouched, "a copy submitted after removal ran");
      readback->Unmap(0, nullptr);
    }
    HRESULT reset = allocator->Reset();
    expect(reset == S_OK || reset == DXGI_ERROR_DEVICE_REMOVED, "Reset of the allocator of dropped work: %08lx", reset);
    step("removed device rejects further queues and keeps its reason");
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    HRESULT hr = device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue));
    expect(hr == DXGI_ERROR_DEVICE_REMOVED && !queue, "removed queue creation: %08lx", hr);
    device->RemoveDevice();
    expect(device->GetDeviceRemovedReason() == reason, "second removal changed its reason");
  }
  if (SUCCEEDED(reason))
    CHECK(fence->Signal(removed));
  copy.Reset();
  allocator.Reset();
  fence.Reset();
  other.Reset();
  raced.Reset();
  CloseHandle(event);
  CloseHandle(other_event);
  chain.Reset();
  presenting.Reset();
  DestroyWindow(window);
  return verdict();
}
