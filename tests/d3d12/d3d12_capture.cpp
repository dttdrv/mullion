// contract: a GPU capture of a frame (MTL_CAPTURE_ENABLED=1 with MULLION_CAPTURE_EXECUTABLE and MULLION_CAPTURE_FRAME,
// src/mullion/mullion_capture.cpp) ends, and leaves its trace where the logs go (MULLION_LOG_PATH). frame n is the work
// after the nth Present, so the capture of frame 1 starts at the first Present and stops at the second. in frame 1 a
// compute queue waits for a fence the direct queue signals after that second Present, and its work is encoded ahead
// and committed once the fence is there: Metal's stop of a capture waits for every command buffer of the capture,
// so a stop on the thread that would go on to that Signal never returns. frame 1's two passes are in events of
// their own, which the trace holds as debug groups around them, with each pass encoded on a thread of its own
// (MULLION_D3D12_PARALLEL_COMMANDS=1)
#include "d3d12_test.hpp"
#include "d3d12_capture.hpp"
#include <dxgi1_4.h>
#include <string>
#include <thread>

int
main() {
  char temporary[MAX_PATH], directory[MAX_PATH];
  CHECK(GetTempPathA(sizeof(temporary), temporary) ? S_OK : E_FAIL);
  CHECK(temporary_directory(temporary, "cap", directory) ? S_OK : E_FAIL);
  SetEnvironmentVariableA("MULLION_CAPTURE_EXECUTABLE", "d3d12_capture");
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
  WNDCLASSA wc{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr, "d3d12_capture"};
  RegisterClassA(&wc);
  auto window = CreateWindowA(wc.lpszClassName, wc.lpszClassName, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 160, 160,
                              nullptr, nullptr, wc.hInstance, nullptr);
  CHECK(window ? S_OK : E_FAIL);
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  ComPtr<ID3D12CommandQueue> queue;
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  const UINT buffers = 2, frames = 4;
  DXGI_SWAP_CHAIN_DESC1 desc{64, 64, DXGI_FORMAT_R8G8B8A8_UNORM, FALSE, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT,
                             buffers, DXGI_SCALING_STRETCH, DXGI_SWAP_EFFECT_FLIP_DISCARD};
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
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  D3D12_COMMAND_QUEUE_DESC compute_desc{D3D12_COMMAND_LIST_TYPE_COMPUTE};
  ComPtr<ID3D12CommandQueue> compute;
  CHECK(device->CreateCommandQueue(&compute_desc, IID_PPV_ARGS(&compute)));
  ComPtr<ID3D12CommandAllocator> compute_allocator;
  ComPtr<ID3D12GraphicsCommandList> compute_list;
  CHECK(device->CreateCommandAllocator(compute_desc.Type, IID_PPV_ARGS(&compute_allocator)));
  CHECK(device->CreateCommandList(0, compute_desc.Type, compute_allocator.Get(), nullptr, IID_PPV_ARGS(&compute_list)));
  const UINT64 size = 1 << 16;
  auto from = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, size, D3D12_RESOURCE_STATE_COMMON);
  auto to = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, size, D3D12_RESOURCE_STATE_COMMON);
  auto copy = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, size, D3D12_RESOURCE_STATE_COMMON);
  const char *events[] = {"mullion capture clear", "mullion capture copy"};
  compute_list->CopyBufferRegion(to.Get(), 0, from.Get(), 0, size);
  CHECK(compute_list->Close());
  ComPtr<ID3D12Fence> held, idle;
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&held)));
  for (UINT frame = 0; frame < frames; frame++) {
    step("frame %u, and the Present that ends it", frame);
    ComPtr<ID3D12Resource> back;
    CHECK(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back)));
    device->CreateRenderTargetView(back.Get(), nullptr, rtv);
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), nullptr));
    transition(list.Get(), back.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float color[] = {frame / float(frames), 0.5f, 0.25f, 1};
    list->BeginEvent(1, events[0], UINT(strlen(events[0]) + 1));
    list->ClearRenderTargetView(rtv, color, 0, nullptr);
    list->EndEvent();
    list->BeginEvent(1, events[1], UINT(strlen(events[1]) + 1));
    list->CopyBufferRegion(copy.Get(), 0, from.Get(), 0, size);
    list->EndEvent();
    transition(list.Get(), back.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    CHECK(list->Close());
    if (frame == 1) {
      CHECK(compute->Wait(held.Get(), 1));
      ID3D12CommandList *computes[] = {compute_list.Get()};
      compute->ExecuteCommandLists(1, computes);
    }
    ID3D12CommandList *lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    CHECK(swapchain->Present(0, 0));
    if (frame == 1)
      CHECK(queue->Signal(held.Get(), 1));
    // the list is recorded again for the next frame only once this one has run
    CHECK(idle ? S_OK : device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&idle)));
    CHECK(queue->Signal(idle.Get(), frame + 1));
    CHECK(idle->SetEventOnCompletion(frame + 1, nullptr));
  }
  // the queue's destruction waits for its capture to stop
  HANDLE gone = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  CHECK(gone && on_destruction(queue.Get(), gone) ? S_OK : E_FAIL);
  sc1.Reset();
  swapchain.Reset();
  queue.Reset();
  expect(WaitForSingleObject(gone, 60000) == WAIT_OBJECT_0, "the queue was not destroyed");
  step("the trace of frame 1");
  WIN32_FIND_DATAA found;
  HANDLE trace = FindFirstFileA((std::string(directory) + "\\*_F.1_*.gputrace").c_str(), &found);
  expect(trace != INVALID_HANDLE_VALUE, "no trace of frame 1 in %s", directory);
  if (trace != INVALID_HANDLE_VALUE) {
    FindClose(trace);
    for (auto event : events)
      expect(trace_holds(std::string(directory) + "\\" + found.cFileName, event), "the trace has no group %s", event);
  }
  expect(remove_tree(directory), "%s was not removed", directory);
  return verdict();
}
