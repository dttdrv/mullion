// contract: a swap chain presents into its window's client area, wherever the window is: a child window that moves
// in its parent, or whose parent is resized around it, takes what is presented along. the child is cleared to one
// color and its parent painted another; after the first frame the child moves to x = argv[3] and the parent's
// client area becomes argv[4] wide. Present succeeds for argv[2] frames, during which a runner captures the window
// and finds the child's color in the child's rectangle and nowhere else (the window's pixels are the witness).
#include "d3d12_test.hpp"
#include <dxgi1_4.h>

int
main(int argc, char **argv) {
  const UINT frames = argc > 2 ? atoi(argv[2]) : 3, buffers = 2;
  // the parent's client area and the child in it; CHILD_* are the runner's too
  const LONG parent = 320, child = 128, child_x = 64, child_y = 96;
  const LONG moved_x = argc > 3 ? atoi(argv[3]) : child_x, resized = argc > 4 ? atoi(argv[4]) : parent;
  const float color[4] = {1, 0, 0, 1};

  WNDCLASSA wc{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, (HBRUSH)(COLOR_WINDOW + 1),
               nullptr, "d3d12_child"};
  RegisterClassA(&wc);
  auto outer = [](LONG width, LONG height) {
    RECT rect{0, 0, width, height};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    return SIZE{rect.right - rect.left, rect.bottom - rect.top};
  };
  auto size = outer(parent, parent);
  auto top = CreateWindowA(
      "d3d12_child", "d3d12_child", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, size.cx, size.cy, nullptr, nullptr,
      wc.hInstance, nullptr
  );
  auto hwnd = CreateWindowA(
      "d3d12_child", "", WS_CHILD | WS_VISIBLE, child_x, child_y, child, child, top, nullptr, wc.hInstance, nullptr
  );

  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<IDXGIFactory4> factory;
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  DXGI_SWAP_CHAIN_DESC1 sc_desc{(UINT)child, (UINT)child, DXGI_FORMAT_R8G8B8A8_UNORM, FALSE, {1, 0},
                                DXGI_USAGE_RENDER_TARGET_OUTPUT, buffers};
  sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  ComPtr<IDXGISwapChain1> sc1;
  CHECK(factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &sc_desc, nullptr, nullptr, &sc1));
  ComPtr<IDXGISwapChain3> swapchain;
  CHECK(sc1.As(&swapchain));

  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, buffers};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap)));
  auto rtv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  ComPtr<ID3D12Resource> back[buffers];
  for (UINT i = 0; i < buffers; i++) {
    CHECK(swapchain->GetBuffer(i, IID_PPV_ARGS(&back[i])));
    device->CreateRenderTargetView(
        back[i].Get(), nullptr, {rtv_heap->GetCPUDescriptorHandleForHeapStart().ptr + i * rtv_step}
    );
  }
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  for (UINT f = 0; f < frames; f++) {
    auto i = swapchain->GetCurrentBackBufferIndex();
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), nullptr));
    transition(list.Get(), back[i].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ClearRenderTargetView({rtv_heap->GetCPUDescriptorHandleForHeapStart().ptr + i * rtv_step}, color, 0, nullptr);
    transition(list.Get(), back[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    CHECK(swapchain->Present(1, 0));
    if (!f) {
      size = outer(resized, parent);
      SetWindowPos(top, nullptr, 0, 0, size.cx, size.cy, SWP_NOMOVE | SWP_NOZORDER);
      MoveWindow(hwnd, moved_x, child_y, child, child, TRUE);
    }
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
      DispatchMessageA(&msg);
  }
  printf("passed: %u frames presented\n", frames);
  return 0;
}
