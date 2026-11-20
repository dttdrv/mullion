// contract: each queued Present uses that call's settings, and releasing the application's references while it
// is queued must still free the queue. "It takes effect in conjunction with your next call to the
// IDXGISwapChain1::Present1 method." (Microsoft Learn, IDXGISwapChain1::SetBackgroundColor, Remarks:
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgiswapchain1-setbackgroundcolor).
// "When the reference count on an object reaches zero, Release must cause the interface pointer to free itself."
// (IUnknown::Release, Remarks: https://learn.microsoft.com/en-us/windows/win32/api/unknwn/nf-unknwn-iunknown-release).
// source sizes select the buffer's top-left region (IDXGISwapChain2::SetSourceSize); screen rotation uses the
// orientation transforms in "Supporting screen rotation". the gradient identifies which texel reached the window.
// GDI makes the presented pixels readable. two waits keep the worker behind the first gate until both Presents
// and the later settings are submitted; a third gate separates their visible results. destruction callbacks,
// rather than a successful Release alone, prove cleanup. 31, 32, 33 and 65 Presents straddle kCommandQueueSize=32.
#include "d3d12_test.hpp"
#include <cmath>
#include <dxgi1_4.h>
#include <thread>

static void STDMETHODCALLTYPE
destroyed(void *data) {
  SetEvent(static_cast<HANDLE>(data));
}

struct Reentry { ID3D12CommandQueue *queue; ID3D12Fence *fence; HRESULT result = E_FAIL; };

static void STDMETHODCALLTYPE
buffer_destroyed(void *data) {
  auto &reentry = *static_cast<Reentry *>(data);
  reentry.result = reentry.queue->Signal(reentry.fence, 2);
}

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const char hlsl[] = R"hlsl(
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
  V v; v.uv = float2((id << 1) & 2, id & 2);
  v.pos = float4(v.uv * float2(2, -2) + float2(-1, 1), 0, 1); return v;
}
float4 ps(V v) : SV_Target { return float4(v.uv, 0.5, 1); }
)hlsl";
  auto vs = compiler.compile(hlsl, "vs", "vs"), ps = compiler.compile(hlsl, "ps", "ps");
  if (vs.empty() || ps.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  SetEnvironmentVariableA("MULLION_CONFIG", "dxgi.presentThroughGDI=True");
  std::thread([] {
    Sleep(60000);
    printf("failed: queued Present or cleanup did not finish within 60 s\n");
    ExitProcess(1);
  }).detach();
  const UINT client = 128, buffers = 2;
  const DWORD timeout = 10000;
  auto wait = [&](HANDLE event, const char *what) {
    auto result = WaitForSingleObject(event, timeout);
    if (!expect(result == WAIT_OBJECT_0, "%s: wait returned %lu", what, result))
      ExitProcess(verdict());
  };
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  auto rs = root_signature(device.Get(), {});
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
  pipeline.pRootSignature = rs.Get();
  pipeline.VS = bytecode(vs);
  pipeline.PS = bytecode(ps);
  pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pipeline.SampleMask = ~0u;
  pipeline.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pipeline.NumRenderTargets = 1;
  pipeline.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  pipeline.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&pso)));
  ComPtr<IDXGIFactory4> factory;
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  WNDCLASSA wc{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr,
               "d3d12_queued_present"};
  RegisterClassA(&wc);
  RECT rect{0, 0, client, client};
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  auto window = CreateWindowA(wc.lpszClassName, wc.lpszClassName, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0,
                              rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, wc.hInstance, nullptr);
  if (!expect(window != nullptr, "could not create window"))
    return verdict();
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  DXGI_PRESENT_PARAMETERS parameters{};
  for (auto scaling : {DXGI_SCALING_STRETCH, DXGI_SCALING_NONE}) {
    const UINT size = scaling == DXGI_SCALING_NONE ? client / 2 : client;
    ComPtr<ID3D12CommandQueue> queue;
    CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
    DXGI_SWAP_CHAIN_DESC1 desc{size, size, DXGI_FORMAT_R8G8B8A8_UNORM, FALSE, {1, 0},
                               DXGI_USAGE_RENDER_TARGET_OUTPUT, buffers, scaling, DXGI_SWAP_EFFECT_FLIP_DISCARD};
    ComPtr<IDXGISwapChain1> sc1;
    CHECK(factory->CreateSwapChainForHwnd(queue.Get(), window, &desc, nullptr, nullptr, &sc1));
    ComPtr<IDXGISwapChain3> swapchain;
    CHECK(sc1.As(&swapchain));
    sc1.Reset();
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
    CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
    ComPtr<ID3D12DescriptorHeap> views;
    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, buffers};
    CHECK(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&views)));
    for (UINT i = 0; i < buffers; i++) {
      ComPtr<ID3D12Resource> back;
      CHECK(swapchain->GetBuffer(i, IID_PPV_ARGS(&back)));
      D3D12_CPU_DESCRIPTOR_HANDLE rtv{views->GetCPUDescriptorHandleForHeapStart().ptr +
                                     i * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)};
      device->CreateRenderTargetView(back.Get(), nullptr, rtv);
      transition(list.Get(), back.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
      list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
      list->SetGraphicsRootSignature(rs.Get());
      D3D12_VIEWPORT viewport{0, 0, float(size), float(size), 0, 1};
      D3D12_RECT scissor{0, 0, LONG(size), LONG(size)};
      list->RSSetViewports(1, &viewport);
      list->RSSetScissorRects(1, &scissor);
      list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      list->DrawInstanced(3, 1, 0, 0);
      transition(list.Get(), back.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    }
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    struct Settings { UINT width, height, quarters; DXGI_RGBA background; };
    const Settings settings[] = {{size, size, 0, {0, 0, 0, 1}},
                                 {size / 2, size / 4, 0, {1, 0, 0, 1}},
                                 {size / 2, size, 1, {0, 1, 0, 1}},
                                 {size, size / 2, 2, {0, 0, 1, 1}},
                                 {size / 4, size / 2, 3, {1, 1, 1, 1}}};
    auto set = [&](const Settings &s) -> HRESULT {
      HRESULT hr;
      if (FAILED(hr = swapchain->SetSourceSize(s.width, s.height)) ||
          FAILED(hr = swapchain->SetBackgroundColor(&s.background)))
        return hr;
      return swapchain->SetRotation(DXGI_MODE_ROTATION(DXGI_MODE_ROTATION_IDENTITY + s.quarters));
    };
    auto shows = [&](const Settings &s) {
      unsigned wrong = 0;
      for (UINT attempt = 0; attempt < 250; attempt++) {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
          DispatchMessageA(&msg);
        wrong = 0;
        HDC dc = GetDC(window);
        for (UINT y = 0; y < client; y += client / 16)
          for (UINT x = 0; x < client; x += client / 16) {
            UINT width = s.quarters % 2 ? s.height : s.width, height = s.quarters % 2 ? s.width : s.height;
            bool inside = scaling == DXGI_SCALING_STRETCH || (x < width && y < height);
            float u = (x + 0.5f) / (scaling == DXGI_SCALING_STRETCH ? client : width);
            float v = (y + 0.5f) / (scaling == DXGI_SCALING_STRETCH ? client : height);
            float bu = u, bv = v;
            if (s.quarters == 1) { bu = 1 - v; bv = u; }
            if (s.quarters == 2) { bu = 1 - u; bv = 1 - v; }
            if (s.quarters == 3) { bu = v; bv = 1 - u; }
            float want[] = {inside ? bu * s.width / size : s.background.r,
                            inside ? bv * s.height / size : s.background.g, inside ? 0.5f : s.background.b};
            auto pixel = GetPixel(dc, x, y);
            const int got[] = {GetRValue(pixel), GetGValue(pixel), GetBValue(pixel)};
            for (UINT c = 0; c < std::size(got); c++)
              wrong += std::fabs(got[c] - want[c] * 255) > 2;
          }
        ReleaseDC(window, dc);
        if (!wrong)
          break;
        Sleep(20);
      }
      if (!expect(!wrong, "scaling=%u source=%u,%u rotation=%u: %u wrong channels", scaling, s.width, s.height,
                  s.quarters, wrong))
        ExitProcess(verdict());
    };
    CHECK(swapchain->Present1(0, 0, &parameters));
    shows(settings[0]);
    for (UINT i = 0; i < std::size(settings); i++) {
      step("scaling=%u queued settings %u and %u", scaling, i, (i + 1) % UINT(std::size(settings)));
      ComPtr<ID3D12Fence> gate, drain;
      CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
      CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&drain)));
      HANDLE done = CreateEventA(nullptr, FALSE, FALSE, nullptr);
      if (!expect(done != nullptr, "could not create drain event"))
        return verdict();
      CHECK(queue->Wait(gate.Get(), 1));
      CHECK(queue->Wait(gate.Get(), 2));
      CHECK(set(settings[i]));
      CHECK(swapchain->Present1(0, 0, &parameters));
      CHECK(queue->Signal(drain.Get(), 1));
      CHECK(queue->Wait(gate.Get(), 3));
      const auto &next = settings[(i + 1) % std::size(settings)];
      CHECK(set(next));
      CHECK(swapchain->Present(0, 0));
      CHECK(queue->Signal(drain.Get(), 2));
      CHECK(gate->Signal(2));
      CHECK(drain->SetEventOnCompletion(1, done));
      wait(done, "first queued Present");
      shows(settings[i]);
      CHECK(gate->Signal(3));
      CHECK(drain->SetEventOnCompletion(2, done));
      wait(done, "second queued Present");
      shows(next);
      CloseHandle(done);
    }
  }
  pso.Reset();
  rs.Reset();
  device.Reset();
  for (UINT count : {0u, 1u, 31u, 32u, 33u, 65u}) {
    step("release everything with %u Presents queued", count);
    CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    ComPtr<ID3D12CommandQueue> queue;
    CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12Fence> gate, drain;
    CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
    CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&drain)));
    HANDLE gone = CreateEventA(nullptr, FALSE, FALSE, nullptr), done = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!expect(gone && done, "could not create lifetime events"))
      return verdict();
    ComPtr<ID3DDestructionNotifier> notifier;
    CHECK(queue.As(&notifier));
    CHECK(notifier->RegisterDestructionCallback(destroyed, gone, nullptr));
    notifier.Reset();
    DXGI_SWAP_CHAIN_DESC1 desc{client, client, DXGI_FORMAT_R8G8B8A8_UNORM, FALSE, {1, 0},
                               DXGI_USAGE_RENDER_TARGET_OUTPUT, buffers, DXGI_SCALING_STRETCH,
                               DXGI_SWAP_EFFECT_FLIP_DISCARD};
    ComPtr<IDXGISwapChain1> swapchain;
    CHECK(factory->CreateSwapChainForHwnd(queue.Get(), window, &desc, nullptr, nullptr, &swapchain));
    Reentry reentry{queue.Get(), drain.Get()};
    if (count) {
      ComPtr<ID3D12Resource> back;
      CHECK(swapchain->GetBuffer(0, IID_PPV_ARGS(&back)));
      CHECK(back.As(&notifier));
      CHECK(notifier->RegisterDestructionCallback(buffer_destroyed, &reentry, nullptr));
      notifier.Reset();
    }
    CHECK(queue->Wait(gate.Get(), 1));
    CHECK(queue->Wait(gate.Get(), 2));
    for (UINT i = 0; i < count; i++)
      CHECK(swapchain->Present1(0, 0, &parameters));
    CHECK(queue->Signal(drain.Get(), 1));
    CHECK(drain->SetEventOnCompletion(1, done));
    queue.Reset();
    swapchain.Reset();
    device.Reset();
    CHECK(gate->Signal(2));
    if (count)
      wait(done, "work after the last queued Present");
    wait(gone, "queue destruction");
    if (count)
      expect(reentry.result == S_OK && drain->GetCompletedValue() == 2,
             "the buffer's destruction callback could not submit queue work");
    gate.Reset();
    drain.Reset();
    CloseHandle(done);
    CloseHandle(gone);
  }
  DestroyWindow(window);
  return verdict();
}
