// contract: Metal takes one GPU capture at a time in a process (MTLCaptureManager), so Mullion keeps one: a queue
// released during the frame it captures ends that capture, whose trace is whole once the queue is gone, and a queue
// made later starts no other (MULLION_CAPTURE_FRAME names one frame of the process). queue A presents once,
// which starts the capture of frame 1, puts frame 1's work in an event and is released before a second Present;
// queue B then presents frames of its own. the bridge's call counts see each start
#include "d3d12_test.hpp"
#include "d3d12_capture.hpp"
#include "../../src/winemetal/winemetal.h"
#include <dxgi1_4.h>
#include <thread>

static const char event[] = "mullion capture release";

// `presents` frames on a queue of its own, and the work of frame `presents` after them; frame 1's in `event`. `gone`
// is set once the queue is destroyed. nonzero when a call failed
static int
frames(ID3D12Device *device, IDXGIFactory4 *factory, HWND window, UINT presents, HANDLE gone) {
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  ComPtr<ID3D12CommandQueue> queue;
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(on_destruction(queue.Get(), gone) ? S_OK : E_FAIL);
  DXGI_SWAP_CHAIN_DESC1 desc{64, 64, DXGI_FORMAT_R8G8B8A8_UNORM, FALSE, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT,
                             2, DXGI_SCALING_STRETCH, DXGI_SWAP_EFFECT_FLIP_DISCARD};
  ComPtr<IDXGISwapChain1> sc1;
  CHECK(factory->CreateSwapChainForHwnd(queue.Get(), window, &desc, nullptr, nullptr, &sc1));
  ComPtr<IDXGISwapChain3> swapchain;
  CHECK(sc1.As(&swapchain));
  ComPtr<ID3D12DescriptorHeap> views;
  D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&views)));
  auto rtv = views->GetCPUDescriptorHandleForHeapStart();
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> idle;
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&idle)));
  for (UINT frame = 0; frame <= presents; frame++) {
    ComPtr<ID3D12Resource> back;
    CHECK(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back)));
    device->CreateRenderTargetView(back.Get(), nullptr, rtv);
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), nullptr));
    transition(list.Get(), back.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float color[] = {0.5f, 0.25f, frame / 4.0f, 1};
    const char *name = frame == 1 ? event : "";
    list->BeginEvent(1, name, UINT(strlen(name) + 1));
    list->ClearRenderTargetView(rtv, color, 0, nullptr);
    list->EndEvent();
    transition(list.Get(), back.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    CHECK(list->Close());
    ID3D12CommandList *lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    if (frame < presents)
      CHECK(swapchain->Present(0, 0));
    CHECK(queue->Signal(idle.Get(), frame + 1));
    CHECK(idle->SetEventOnCompletion(frame + 1, nullptr));
  }
  return 0;
}

int
main() {
  char temporary[MAX_PATH], directory[MAX_PATH];
  CHECK(GetTempPathA(sizeof(temporary), temporary) ? S_OK : E_FAIL);
  CHECK(temporary_directory(temporary, "cap", directory) ? S_OK : E_FAIL);
  SetEnvironmentVariableA("MULLION_CAPTURE_EXECUTABLE", "d3d12_capture_release");
  SetEnvironmentVariableA("MULLION_CAPTURE_FRAME", "1");
  SetEnvironmentVariableA("MULLION_LOG_PATH", directory);
  std::thread([] {
    Sleep(120000);
    printf("wrong: the frames did not end within 120 s (while: %s)\nfailed: 1 wrong results\n", trace::doing);
    ExitProcess(1);
  }).detach();
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<IDXGIFactory4> factory;
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  WNDCLASSA wc{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr, "capture"};
  RegisterClassA(&wc);
  auto window = CreateWindowA(wc.lpszClassName, wc.lpszClassName, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 160, 160,
                              nullptr, nullptr, wc.hInstance, nullptr);
  CHECK(window ? S_OK : E_FAIL);
  auto winemetal = GetModuleHandleA("winemetal.dll");
  auto totals = (decltype(&WMTDiagCallTotals))GetProcAddress(winemetal, "WMTDiagCallTotals");
  ((decltype(&WMTDiagTimeCalls))GetProcAddress(winemetal, "WMTDiagTimeCalls"))();
  static WMTDiagCall calls[WMTUnixCallCount];
  totals(calls);

  step("queue A, released during frame 1");
  HANDLE gone = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  if (!gone || frames(device.Get(), factory.Get(), window, 1, gone))
    return 1;
  expect(WaitForSingleObject(gone, 60000) == WAIT_OBJECT_0, "queue A was not destroyed");
  WIN32_FIND_DATAA found;
  HANDLE trace = FindFirstFileA((std::string(directory) + "\\*_F.1_*.gputrace").c_str(), &found);
  if (expect(trace != INVALID_HANDLE_VALUE, "no trace of frame 1 in %s", directory)) {
    FindClose(trace);
    expect(holds(std::string(directory) + "\\" + found.cFileName, event), "the trace has no group %s", event);
  }
  step("queue B, presenting after it");
  ResetEvent(gone);
  if (frames(device.Get(), factory.Get(), window, 3, gone))
    return 1;
  totals(calls);
  uint64_t starts = 0;
  for (auto &[count, ticks, name] : calls)
    if (name && !strcmp(name, "MTLCaptureManager_startCapture"))
      starts += count;
  expect(starts == 1, "%llu captures started, want one", (unsigned long long)starts);
  expect(remove_tree(directory), "%s was not removed", directory);
  return verdict();
}
