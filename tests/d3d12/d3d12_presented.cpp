// contract: what a window shows of a presented back buffer. the back buffer's pixel (x, y) holds its place, (x, y)
// over the buffer's size, in red and green, so a pixel of the window says which part of the buffer it shows:
// - all of the buffer over all of the window, and of a source size the part from the buffer's origin
//   (IDXGISwapChain2::SetSourceSize), stretched alike (DXGI_SCALING_STRETCH);
// - the picture turned back from how SetRotation says the buffer holds it: an application draws a place (x, y) of
//   its picture at Rotation(90) * Translation(height, 0), Rotation(180) * Translation(width, height) or
//   Rotation(270) * Translation(0, width) of it ("Supporting screen rotation", the swap chain's orientation
//   transforms), and the window shows the picture;
// - with DXGI_SCALING_NONE the buffer or its source "without any scaling" from the window's top left, and "all target
//   area outside" it in the background color (DXGI_SCALING, IDXGISwapChain1::SetBackgroundColor).
// the window's pixels are read through GDI, which has them when frames get to the window through GDI: the test asks
// for that (dxgi.presentThroughGDI), since nothing reads a Metal view's pixels back.
#include "d3d12_test.hpp"
#include <cmath>
#include <dxgi1_4.h>
#include <functional>

static const char hlsl[] = R"hlsl(
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
  V o; o.uv = float2((id << 1) & 2, id & 2);
  o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o;
}
float4 ps(V i) : SV_Target { return float4(i.uv, BLUE, 1); }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  SetEnvironmentVariableA("DXMT_CONFIG", "dxgi.presentThroughGDI=True");
  // the window's client area, and the smaller buffer of the swap chain that is not stretched
  const UINT client = 256, small = client / 2, buffers = 2;
  const float blue = 0.5f;
  const DXGI_RGBA background{0.25f, 0.75f, 1, 1};
  auto vs = compiler.compile(hlsl, "vs", "vs", {"BLUE=" + std::to_string(blue)}),
       ps = compiler.compile(hlsl, "ps", "ps", {"BLUE=" + std::to_string(blue)});
  if (vs.empty() || ps.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  auto rs = root_signature(device.Get(), {});
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = rs.Get();
  desc.VS = bytecode(vs);
  desc.PS = bytecode(ps);
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<IDXGIFactory4> factory;
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  WNDCLASSA wc{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr, "d3d12_presented"};
  RegisterClassA(&wc);

  // a window with a swap chain of buffers of `size`
  struct Shown {
    HWND window;
    UINT size;
    ComPtr<IDXGISwapChain3> swapchain;
    ComPtr<ID3D12DescriptorHeap> views;
    ComPtr<ID3D12Resource> back[buffers];
  };
  auto shown = [&](UINT size, DXGI_SCALING scaling, Shown &out) -> HRESULT {
    RECT rect{0, 0, (LONG)client, (LONG)client};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    out.window = CreateWindowA(
        "d3d12_presented", "d3d12_presented", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, rect.right - rect.left,
        rect.bottom - rect.top, nullptr, nullptr, wc.hInstance, nullptr
    );
    out.size = size;
    DXGI_SWAP_CHAIN_DESC1 sc_desc{size, size, DXGI_FORMAT_R8G8B8A8_UNORM, FALSE, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT, buffers, scaling,
                                  DXGI_SWAP_EFFECT_FLIP_DISCARD};
    ComPtr<IDXGISwapChain1> sc1;
    HRESULT hr = factory->CreateSwapChainForHwnd(queue.Get(), out.window, &sc_desc, nullptr, nullptr, &sc1);
    if (FAILED(hr) || FAILED(hr = sc1.As(&out.swapchain)))
      return hr;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, buffers};
    if (FAILED(hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&out.views))))
      return hr;
    for (UINT i = 0; i < buffers; i++) {
      if (FAILED(hr = out.swapchain->GetBuffer(i, IID_PPV_ARGS(&out.back[i]))))
        return hr;
      device->CreateRenderTargetView(
          out.back[i].Get(), nullptr,
          {out.views->GetCPUDescriptorHandleForHeapStart().ptr + i * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)}
      );
    }
    return out.swapchain->SetBackgroundColor(&background);
  };
  Shown stretched, kept;
  CHECK(shown(client, DXGI_SCALING_STRETCH, stretched));
  CHECK(shown(small, DXGI_SCALING_NONE, kept));

  unsigned failures = 0, cases = 0;
  // draws and presents a frame, and compares the window with what each of its pixels must show: the place in the
  // buffer, in parts of it, or none for the background. frames get to the window some time after Present
  using Place = std::function<bool(float u, float v, float place[2])>;
  auto presents = [&](const char *what, Shown &to, const Place &place) -> HRESULT {
    cases++;
    auto i = to.swapchain->GetCurrentBackBufferIndex();
    HRESULT hr;
    if (FAILED(hr = allocator->Reset()) || FAILED(hr = list->Reset(allocator.Get(), pso.Get())))
      return hr;
    transition(list.Get(), to.back[i].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{
        to.views->GetCPUDescriptorHandleForHeapStart().ptr + i * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)
    };
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->SetGraphicsRootSignature(rs.Get());
    D3D12_VIEWPORT viewport{0, 0, (float)to.size, (float)to.size, 0, 1};
    D3D12_RECT scissor{0, 0, (LONG)to.size, (LONG)to.size};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    transition(list.Get(), to.back[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    if (FAILED(hr = submit(device.Get(), queue.Get(), list.Get())) || FAILED(hr = to.swapchain->Present(1, 0)))
      return hr;
    // pixels across the window and along its diagonal, where a picture's last pixel and the first one past it are.
    // a channel is within two steps of its value: one for the buffer's rounding, one for the window's
    RECT area;
    GetClientRect(to.window, &area);
    const UINT size = area.right, across = 16, step = size / across;
    unsigned wrong = 0;
    for (UINT attempt = 0; attempt < 250; attempt++) {
      MSG msg;
      while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
        DispatchMessageA(&msg);
      Sleep(20);
      wrong = 0;
      HDC dc = GetDC(to.window);
      auto shows = [&](UINT x, UINT y) {
        float at[2];
        bool inside = place((x + 0.5f) / size, (y + 0.5f) / size, at);
        float want[3] = {inside ? at[0] : background.r, inside ? at[1] : background.g, inside ? blue : background.b};
        auto got = GetPixel(dc, x, y);
        const int channels[3] = {GetRValue(got), GetGValue(got), GetBValue(got)};
        bool same = true;
        for (int c = 0; c < 3; c++)
          same &= std::fabs(channels[c] - want[c] * 255) <= 2;
        if (!same && wrong++ < 2 && attempt == 249)
          printf("%s: the window's pixel %u,%u is %d %d %d, want %.1f %.1f %.1f\n", what, x, y, channels[0],
                 channels[1], channels[2], want[0] * 255, want[1] * 255, want[2] * 255);
      };
      for (UINT y = step / 2; y < size; y += step)
        for (UINT x = step / 2; x < size; x += step)
          shows(x, y);
      for (UINT d = 0; d < size; d++)
        shows(d, d);
      ReleaseDC(to.window, dc);
      if (!wrong)
        break;
    }
    failures += wrong;
    return S_OK;
  };
  auto whole = [](float u, float v, float place[2]) {
    place[0] = u, place[1] = v;
    return true;
  };
  CHECK(presents("the whole buffer", stretched, whole));
  CHECK(stretched.swapchain->SetSourceSize(client / 2, client / 4));
  CHECK(presents("a source size", stretched, [](float u, float v, float place[2]) {
    place[0] = u / 2, place[1] = v / 4;
    return true;
  }));
  CHECK(stretched.swapchain->SetSourceSize(client, client));
  // the picture's place (u, v) is in the buffer where the orientation transform puts it
  const struct {
    DXGI_MODE_ROTATION rotation;
    const char *what;
    Place place;
  } turned[] = {
      {DXGI_MODE_ROTATION_ROTATE90, "a buffer rotated by 90", [](float u, float v, float place[2]) { place[0] = 1 - v, place[1] = u; return true; }},
      {DXGI_MODE_ROTATION_ROTATE180, "a buffer rotated by 180", [](float u, float v, float place[2]) { place[0] = 1 - u, place[1] = 1 - v; return true; }},
      {DXGI_MODE_ROTATION_ROTATE270, "a buffer rotated by 270", [](float u, float v, float place[2]) { place[0] = v, place[1] = 1 - u; return true; }},
      {DXGI_MODE_ROTATION_IDENTITY, "a buffer not rotated again", whole},
  };
  for (auto &turn : turned) {
    CHECK(stretched.swapchain->SetRotation(turn.rotation));
    CHECK(presents(turn.what, stretched, turn.place));
  }
  // the picture at its own size in a larger window, then a part of it, then in the window made larger still: the
  // picture stays as large, to the pixel
  auto within = [&](UINT picture, UINT window) {
    return [=](float u, float v, float place[2]) {
      float scale = (float)window / small;
      place[0] = u * scale, place[1] = v * scale;
      return u * window < picture && v * window < picture;
    };
  };
  CHECK(presents("a buffer that is not stretched", kept, within(small, client)));
  CHECK(kept.swapchain->SetSourceSize(small / 2, small / 2));
  CHECK(presents("a source size that is not stretched", kept, within(small / 2, client)));
  const UINT larger = client * 3 / 2;
  RECT rect{0, 0, (LONG)larger, (LONG)larger};
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  SetWindowPos(kept.window, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top, SWP_NOMOVE | SWP_NOZORDER);
  CHECK(presents("a picture that is not stretched in a window made larger", kept, within(small / 2, larger)));
  printf("%s: %u wrong pixels of the window in %u cases\n", failures ? "failed" : "passed", failures, cases);
  return failures != 0;
}
