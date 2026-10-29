// contract: a waitable swap chain supplies a closeable readiness handle,
// including before its first frame. "A handle to the waitable object, or NULL
// if the swap chain was not created with
// DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT."
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-getframelatencywaitableobject
// SetMaximumFrameLatency defaults to one and is invalid without the creation
// flag.
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-setmaximumframelatency
// duplicate handles, initial credit, and credits added when increasing latency
// follow Windows observations in Wine dlls/dxgi/tests/dxgi.c,
// test_frame_latency_event. waits below either inspect state immediately or
// wait for GPU work; elapsed time is never a correctness condition.
#include "dxgi_test.hpp"
#include <limits>

int
main() {
  // the latency range is independent of the buffer-count limit (Wine
  // test_maximum_frame_latency).
  constexpr UINT maximum_frame_latency = 16, maximum_sync_interval = 4;
  HWND window = CreateWindowA(
      "static", "dxgi_latency", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 128, 128, nullptr, nullptr,
      GetModuleHandleA(nullptr), nullptr
  );
  if (!expect(window != nullptr, "CreateWindow: %lu", GetLastError()))
    return verdict();
  UpdateWindow(window);
  ComPtr<IDXGIFactory2> factory;
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  for (UINT api : dxgi_apis()) {
    DxgiDevice device;
    CHECK(device.create(api));
    for (bool waitable : {false, true}) {
      step("D3D%u waitable %u: create", api, waitable);
      DXGI_SWAP_CHAIN_DESC1 desc{
          3,
          5,
          DXGI_FORMAT_R8G8B8A8_UNORM,
          FALSE,
          {1, 0},
          DXGI_USAGE_RENDER_TARGET_OUTPUT,
          2,
          DXGI_SCALING_STRETCH,
          DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL
      };
      desc.Flags = waitable ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0;
      ComPtr<IDXGISwapChain1> first;
      CHECK(factory->CreateSwapChainForHwnd(device.input.Get(), window, &desc, nullptr, nullptr, &first));
      ComPtr<IDXGISwapChain2> chain;
      CHECK(first.As(&chain));
      HANDLE handle = chain->GetFrameLatencyWaitableObject();
      expect(bool(handle) == waitable, "waitable handle disagrees with creation flag");
      UINT latency = std::numeric_limits<UINT>::max();
      HRESULT hr = chain->GetMaximumFrameLatency(&latency);
      expect(
          hr == (waitable ? S_OK : DXGI_ERROR_INVALID_CALL) && (!waitable || latency == 1),
          "initial latency: %#lx / %u", hr, latency
      );
      if (!waitable) {
        for (UINT value : {0u, 1u, maximum_frame_latency, maximum_frame_latency + 1})
          expect(chain->SetMaximumFrameLatency(value) == DXGI_ERROR_INVALID_CALL, "latency without creation flag");
        continue;
      }
      if (!expect(handle != nullptr, "missing readiness handle"))
        continue;
      HANDLE duplicate = chain->GetFrameLatencyWaitableObject();
      expect(duplicate && duplicate != handle, "readiness handle is not independently closeable");
      if (duplicate && duplicate != handle)
        expect(CloseHandle(duplicate), "CloseHandle duplicate");
      expect(WaitForSingleObject(handle, 0) == WAIT_OBJECT_0, "first frame is not ready");
      expect(WaitForSingleObject(handle, 0) == WAIT_TIMEOUT, "more than one initial frame credit");
      step("D3D%u TEST and refused present do not release readiness", api);
      expect(chain->Present(0, DXGI_PRESENT_TEST) == S_OK, "TEST present");
      CHECK(device.idle());
      expect(WaitForSingleObject(handle, 0) == WAIT_TIMEOUT, "TEST released readiness");
      expect(chain->Present(maximum_sync_interval + 1, 0) == DXGI_ERROR_INVALID_CALL, "invalid sync interval");
      CHECK(device.idle());
      expect(WaitForSingleObject(handle, 0) == WAIT_TIMEOUT, "refused present released readiness");
      for (UINT frame = 0; frame <= desc.BufferCount * 2; frame++) {
        step("D3D%u readiness after frame %u", api, frame);
        CHECK(chain->Present(0, 0));
        CHECK(device.idle());
        expect(WaitForSingleObject(handle, INFINITE) == WAIT_OBJECT_0, "completed frame did not release readiness");
        expect(WaitForSingleObject(handle, 0) == WAIT_TIMEOUT, "frame released more than one credit");
      }
      if (device.queue) {
        step("D3D12 readiness waits for the queue's GPU dependency");
        ComPtr<ID3D12Fence> gate;
        CHECK(device.d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
        UINT before = 0, after = 0;
        CHECK(chain->GetLastPresentCount(&before));
        CHECK(device.queue->Wait(gate.Get(), 1));
        hr = chain->Present(0, 0);
        HRESULT counted = chain->GetLastPresentCount(&after);
        DWORD ready = WaitForSingleObject(handle, 0);
        HRESULT released = gate->Signal(1);
        expect(hr == S_OK && ready == WAIT_TIMEOUT, "queued present reported readiness before its fence");
        expect(counted == S_OK && after == before + 1, "GPU-pending accepted present was not counted");
        CHECK(released);
        CHECK(device.idle());
        expect(WaitForSingleObject(handle, INFINITE) == WAIT_OBJECT_0, "released queue did not become ready");
        expect(WaitForSingleObject(handle, 0) == WAIT_TIMEOUT, "released queue gave extra readiness");
      }
      for (UINT value = 2; value <= maximum_frame_latency; value++) {
        step("D3D%u increasing latency to %u", api, value);
        expect(chain->SetMaximumFrameLatency(value) == S_OK, "increase latency");
        expect(chain->GetMaximumFrameLatency(&latency) == S_OK && latency == value, "latency getter");
        expect(WaitForSingleObject(handle, 0) == WAIT_OBJECT_0, "increase did not release one credit");
        expect(WaitForSingleObject(handle, 0) == WAIT_TIMEOUT, "increase released extra credits");
      }
      for (UINT value = maximum_frame_latency; value; value--) {
        step("D3D%u decreasing latency to %u", api, value);
        expect(chain->SetMaximumFrameLatency(value) == S_OK, "decrease latency");
        expect(chain->GetMaximumFrameLatency(&latency) == S_OK && latency == value, "decreased latency getter");
        expect(WaitForSingleObject(handle, 0) == WAIT_TIMEOUT, "decrease released readiness");
      }
      for (UINT value : {0u, maximum_frame_latency + 1, std::numeric_limits<UINT>::max()}) {
        expect(chain->SetMaximumFrameLatency(value) == DXGI_ERROR_INVALID_CALL, "invalid latency %u", value);
        expect(chain->GetMaximumFrameLatency(&latency) == S_OK && latency == 1, "refusal changed latency");
      }
      CHECK(device.idle());
      expect(CloseHandle(handle), "CloseHandle readiness");
      HANDLE reopened = chain->GetFrameLatencyWaitableObject();
      expect(reopened != nullptr, "closing the caller's handle closed the swap chain's handle");
      if (reopened)
        expect(CloseHandle(reopened), "CloseHandle reopened readiness");
    }
  }
  DestroyWindow(window);
  return verdict();
}
