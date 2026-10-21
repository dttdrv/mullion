// a frame-time harness for the libraries' own submit/record/present paths, not a microbenchmark: argv[1] picks
// "d3d11" or "d3d12", argv[2] the frames measured after argv[3] warmup frames, and both APIs draw the same two
// passes a frame from one swap chain with vsync off, updating a constant buffer and changing pipeline state every
// frame, so what the numbers hold is what an application's frame costs here.
// it prints one line a frame ("frame <index> <cpu ms>") and a summary ("result cpu median=... p95=... p99=..."),
// which tools/frametime.py reads for both GPU families; MULLION_DIAG=profile adds the GPU's own frame intervals.
#include <windows.h>
#include <d3d11_1.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

static const char hlsl[] = R"hlsl(
cbuffer Frame : register(b0) { float4 tint; };
struct V { float4 pos : SV_Position; };
V vs(uint id : SV_VertexID) {
  V o;
  o.pos = float4(float2((id << 1) & 2, id & 2) * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}
float4 ps_a(V i) : SV_Target { return tint; }
float4 ps_b(V i) : SV_Target { return float4(tint.rgb * 0.5 + 0.25, 1); }
)hlsl";

static bool
compile(const char *entry, const char *profile, ID3DBlob **code) {
  ComPtr<ID3DBlob> errors;
  if (SUCCEEDED(D3DCompile(hlsl, sizeof(hlsl) - 1, nullptr, nullptr, nullptr, entry, profile, 0, 0, code, &errors)))
    return true;
  if (errors)
    printf("failed: %.*s\n", (int)errors->GetBufferSize(), (const char *)errors->GetBufferPointer());
  return false;
}

// milliseconds since `start`
static double
since(LARGE_INTEGER start, double ticks) {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return 1000.0 * double(now.QuadPart - start.QuadPart) / ticks;
}

static void
report(std::vector<double> &frames) {
  std::sort(frames.begin(), frames.end());
  auto at = [&](double share) {
    if (frames.empty())
      return 0.0;
    size_t index = std::min(frames.size() - 1, size_t(share * frames.size()));
    return frames[index];
  };
  printf("result cpu median=%.4f p95=%.4f p99=%.4f min=%.4f frames=%zu\n", at(0.5), at(0.95), at(0.99), at(0.0),
         frames.size());
}

static const UINT Width = 960, Height = 540, Buffers = 2;

static HWND
window(const char *name) {
  WNDCLASSA wc{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr, name};
  RegisterClassA(&wc);
  RECT rect{0, 0, (LONG)Width, (LONG)Height};
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  return CreateWindowA(name, name, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, rect.right - rect.left,
                       rect.bottom - rect.top, nullptr, nullptr, wc.hInstance, nullptr);
}

struct FrameArgs {
  float tint[4];
};

static int
d3d11(HWND hwnd, UINT frames, UINT warmup, UINT draws) {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                               &device, nullptr, &context))) {
    printf("failed: D3D11CreateDevice\n");
    return 1;
  }
  ComPtr<IDXGIDevice> dxgi_device;
  ComPtr<IDXGIAdapter> adapter;
  ComPtr<IDXGIFactory2> factory;
  if (FAILED(device.As(&dxgi_device)) || FAILED(dxgi_device->GetAdapter(&adapter)) ||
      FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
    printf("failed: DXGI factory\n");
    return 1;
  }
  DXGI_SWAP_CHAIN_DESC1 desc{Width, Height, DXGI_FORMAT_R8G8B8A8_UNORM, FALSE, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT,
                             Buffers};
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  ComPtr<IDXGISwapChain1> swapchain;
  if (FAILED(factory->CreateSwapChainForHwnd(device.Get(), hwnd, &desc, nullptr, nullptr, &swapchain))) {
    printf("failed: CreateSwapChainForHwnd\n");
    return 1;
  }
  ComPtr<ID3D11RenderTargetView> target;
  {
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&back))) ||
        FAILED(device->CreateRenderTargetView(back.Get(), nullptr, &target))) {
      printf("failed: render target\n");
      return 1;
    }
  }
  // SM 5.1 needs a 11_1 device, which D3D11CreateDevice without a level list does not ask for: 4_0 like the tests
  ComPtr<ID3DBlob> vs_code, a_code, b_code;
  if (!compile("vs", "vs_4_0", &vs_code) || !compile("ps_a", "ps_4_0", &a_code) ||
      !compile("ps_b", "ps_4_0", &b_code)) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  // the vertex shader takes no input but SV_VertexID: no input layout, no vertex buffers
  auto pipeline = [&](ID3DBlob *ps, ComPtr<ID3D11PixelShader> &shader) {
    return SUCCEEDED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &shader));
  };
  ComPtr<ID3D11VertexShader> vertex;
  ComPtr<ID3D11PixelShader> first, second;
  if (FAILED(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vertex)) ||
      !pipeline(a_code.Get(), first) || !pipeline(b_code.Get(), second)) {
    printf("failed: pipeline\n");
    return 1;
  }
  D3D11_BUFFER_DESC buffer_desc{sizeof(FrameArgs), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE};
  ComPtr<ID3D11Buffer> constants;
  if (FAILED(device->CreateBuffer(&buffer_desc, nullptr, &constants))) {
    printf("failed: constant buffer\n");
    return 1;
  }
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->VSSetShader(vertex.Get(), nullptr, 0);
  context->IASetInputLayout(nullptr);
  context->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
  const float clear[4] = {0.05f, 0.05f, 0.08f, 1};
  context->OMSetRenderTargets(1, target.GetAddressOf(), nullptr);
  const D3D11_VIEWPORT viewport{0, 0, float(Width), float(Height), 0, 1};
  context->RSSetViewports(1, &viewport);

  LARGE_INTEGER ticks;
  QueryPerformanceFrequency(&ticks);
  const double frequency = double(ticks.QuadPart);
  std::vector<double> measured;
  for (UINT frame = 0; frame < frames + warmup; frame++) {
    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(context->Map(constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
      printf("failed: Map\n");
      return 1;
    }
    FrameArgs args{{float(frame % 256) / 255.0f, 0.25f, 0.5f, 1}};
    memcpy(mapped.pData, &args, sizeof(args));
    context->Unmap(constants.Get(), 0);
    context->ClearRenderTargetView(target.Get(), clear);
    // an application's frame: draws that change pipeline state, and constants that change under them
    for (UINT draw = 0; draw < draws; draw++) {
      context->PSSetShader(draw & 1 ? second.Get() : first.Get(), nullptr, 0);
      if (!(draw & 7)) {
        D3D11_MAPPED_SUBRESOURCE again;
        if (FAILED(context->Map(constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &again)))
          return 1;
        FrameArgs next{{float(draw) / 255.0f, 0.25f, 0.5f, 1}};
        memcpy(again.pData, &next, sizeof(next));
        context->Unmap(constants.Get(), 0);
      }
      context->Draw(3, 0);
    }
    HRESULT hr = swapchain->Present(0, 0);
    double milliseconds = since(start, frequency);
    if (FAILED(hr)) {
      printf("failed: Present %08lx at frame %u\n", hr, frame);
      return 1;
    }
    if (frame >= warmup) {
      measured.push_back(milliseconds);
      printf("frame %u %.4f\n", frame - warmup, milliseconds);
    }
  }
  report(measured);
  return 0;
}

static int
d3d12(HWND hwnd, UINT frames, UINT warmup, UINT draws) {
  ComPtr<ID3D12Device> device;
  if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
    printf("failed: D3D12CreateDevice\n");
    return 1;
  }
  D3D12_ROOT_PARAMETER parameter{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
  parameter.Constants = {0, 0, sizeof(FrameArgs) / sizeof(UINT)};
  D3D12_ROOT_SIGNATURE_DESC signature{1, &parameter};
  ComPtr<ID3DBlob> blob, errors;
  ComPtr<ID3D12RootSignature> root;
  if (FAILED(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
      FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)))) {
    printf("failed: root signature\n");
    return 1;
  }
  // SM 5.1 needs a 11_1 device, which D3D11CreateDevice without a level list does not ask for: 4_0 like the tests
  ComPtr<ID3DBlob> vs_code, a_code, b_code;
  if (!compile("vs", "vs_4_0", &vs_code) || !compile("ps_a", "ps_4_0", &a_code) ||
      !compile("ps_b", "ps_4_0", &b_code)) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  auto pipeline = [&](ID3DBlob *ps, ComPtr<ID3D12PipelineState> &state) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root.Get();
    desc.VS = {vs_code->GetBufferPointer(), vs_code->GetBufferSize()};
    desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = ~0u;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc = {1, 0};
    return SUCCEEDED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&state)));
  };
  ComPtr<ID3D12PipelineState> first, second;
  if (!pipeline(a_code.Get(), first) || !pipeline(b_code.Get(), second)) {
    printf("failed: pipeline\n");
    return 1;
  }
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  if (FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)))) {
    printf("failed: command queue\n");
    return 1;
  }
  ComPtr<IDXGIFactory4> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    printf("failed: DXGI factory\n");
    return 1;
  }
  DXGI_SWAP_CHAIN_DESC1 desc{Width, Height, DXGI_FORMAT_R8G8B8A8_UNORM, FALSE, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT,
                             Buffers};
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  ComPtr<IDXGISwapChain1> swapchain1;
  ComPtr<IDXGISwapChain3> swapchain;
  if (FAILED(factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &desc, nullptr, nullptr, &swapchain1)) ||
      FAILED(swapchain1.As(&swapchain))) {
    printf("failed: CreateSwapChainForHwnd\n");
    return 1;
  }
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, Buffers};
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap)))) {
    printf("failed: descriptor heap\n");
    return 1;
  }
  auto step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  ComPtr<ID3D12Resource> back[Buffers];
  for (UINT i = 0; i < Buffers; i++)
    if (FAILED(swapchain->GetBuffer(i, IID_PPV_ARGS(&back[i]))))
      return 1;
  for (UINT i = 0; i < Buffers; i++)
    device->CreateRenderTargetView(back[i].Get(), nullptr, {rtv_heap->GetCPUDescriptorHandleForHeapStart().ptr + i * step});

  D3D12_HEAP_PROPERTIES upload_props{D3D12_HEAP_TYPE_UPLOAD};
  D3D12_RESOURCE_DESC upload_desc{D3D12_RESOURCE_DIMENSION_BUFFER, 0, sizeof(FrameArgs) * Buffers, 1, 1, 1,
                                  DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                  D3D12_RESOURCE_FLAG_NONE};
  ComPtr<ID3D12Resource> constants;
  if (FAILED(device->CreateCommittedResource(&upload_props, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&constants)))) {
    printf("failed: constant buffer\n");
    return 1;
  }
  FrameArgs *mapped = nullptr;
  if (FAILED(constants->Map(0, nullptr, (void **)&mapped))) {
    printf("failed: Map\n");
    return 1;
  }
  ComPtr<ID3D12CommandAllocator> allocator[Buffers];
  ComPtr<ID3D12GraphicsCommandList> lists[Buffers];
  ComPtr<ID3D12Fence> fence;
  UINT64 value = 0, signalled = 0;
  HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  if (!event || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
    printf("failed: fence\n");
    return 1;
  }
  for (UINT i = 0; i < Buffers; i++) {
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator[i]))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator[i].Get(), first.Get(),
                                         IID_PPV_ARGS(&lists[i])))) {
      printf("failed: command list\n");
      return 1;
    }
  }
  const float clear[4] = {0.05f, 0.05f, 0.08f, 1};
  LARGE_INTEGER ticks;
  QueryPerformanceFrequency(&ticks);
  const double frequency = double(ticks.QuadPart);
  std::vector<double> measured;
  for (UINT frame = 0; frame < frames + warmup; frame++) {
    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);
    UINT slot = frame % Buffers;
    if (frame >= Buffers) {
      if (signalled < frame - Buffers + 1) {
        if (FAILED(fence->SetEventOnCompletion(frame - Buffers + 1, event))) {
          printf("failed: SetEventOnCompletion\n");
          return 1;
        }
        WaitForSingleObject(event, INFINITE);
        signalled = frame - Buffers + 1;
      }
      allocator[slot]->Reset();
      lists[slot]->Reset(allocator[slot].Get(), first.Get());
    }
    FrameArgs args{{float(frame % 256) / 255.0f, 0.25f, 0.5f, 1}};
    mapped[slot] = args;
    ID3D12GraphicsCommandList *list = lists[slot].Get();
    list->SetGraphicsRootSignature(root.Get());
    list->SetGraphicsRoot32BitConstants(0, sizeof(FrameArgs) / sizeof(UINT), &args, 0);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_RESOURCE_BARRIER barrier{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
    barrier.Transition = {back[slot].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET};
    list->ResourceBarrier(1, &barrier);
    const D3D12_CPU_DESCRIPTOR_HANDLE target{rtv_heap->GetCPUDescriptorHandleForHeapStart().ptr + slot * step};
    list->OMSetRenderTargets(1, &target, FALSE, nullptr);
    list->ClearRenderTargetView(target, clear, 0, nullptr);
    // an application's frame: draws that change pipeline state, and constants that change under them
    for (UINT draw = 0; draw < draws; draw++) {
      if (!(draw & 7)) {
        FrameArgs next{{float(draw) / 255.0f, 0.25f, 0.5f, 1}};
        mapped[slot] = next;
        list->SetGraphicsRoot32BitConstants(0, sizeof(FrameArgs) / sizeof(UINT), &next, 0);
      }
      list->SetPipelineState(draw & 1 ? second.Get() : first.Get());
      list->DrawInstanced(3, 1, 0, 0);
    }
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    list->ResourceBarrier(1, &barrier);
    if (FAILED(list->Close())) {
      printf("failed: Close\n");
      return 1;
    }
    ID3D12CommandList *commands[] = {list};
    queue->ExecuteCommandLists(1, commands);
    queue->Signal(fence.Get(), ++value);
    HRESULT hr = swapchain->Present(0, 0);
    double milliseconds = since(start, frequency);
    if (FAILED(hr)) {
      printf("failed: Present %08lx at frame %u\n", hr, frame);
      return 1;
    }
    if (frame >= warmup) {
      measured.push_back(milliseconds);
      printf("frame %u %.4f\n", frame - warmup, milliseconds);
    }
  }
  if (FAILED(fence->SetEventOnCompletion(value, event))) {
    printf("failed: SetEventOnCompletion\n");
    return 1;
  }
  WaitForSingleObject(event, INFINITE);
  report(measured);
  return 0;
}

int
main(int argc, char **argv) {
  const char *api = argc > 1 ? argv[1] : "d3d12";
  const UINT frames = argc > 2 ? UINT(atoi(argv[2])) : 600, warmup = argc > 3 ? UINT(atoi(argv[3])) : 60,
             draws = argc > 4 ? UINT(atoi(argv[4])) : 128;
  HWND hwnd = window("frametime");
  if (!hwnd) {
    printf("failed: window\n");
    return 1;
  }
  MSG message;
  while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&message);
    DispatchMessageA(&message);
  }
  if (!strcmp(api, "d3d11"))
    return d3d11(hwnd, frames, warmup, draws);
  if (!strcmp(api, "d3d12"))
    return d3d12(hwnd, frames, warmup, draws);
  printf("failed: unknown api %s\n", api);
  return 1;
}
