// contract: a swap chain back buffer drawn by a vertex and pixel shader pair holds, at every pixel, the color the
// pixel shader computes from that pixel's center (the interpolated uv), and Present of it succeeds for `frames`
// frames. argv[2] sets how many frames to present, so a runner can capture the window while it stays up.
// the swap chain's settings that leave its buffers alone hold what they are given and refuse what the documentation
// says they refuse: a background color with a component outside 0 to 1 (IDXGISwapChain1::SetBackgroundColor), a
// source size of zero or beyond the back buffer (IDXGISwapChain2::SetSourceSize: "must be greater than zero, and
// must be less than or equal to the overall" size); a flip model swap chain takes a rotation
// (IDXGISwapChain1::SetRotation), and one restricted to no output says so. the last frame is presented from a
// quarter of the back buffer, and new buffers are presented whole again.
#include "d3d12_test.hpp"
#include <cmath>
#include <dxgi1_4.h>

static const char hlsl[] = R"hlsl(
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
  V o; o.uv = float2((id << 1) & 2, id & 2);
  o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o;
}
float4 ps(V i) : SV_Target { return float4(i.uv, 0.5, 1); }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const UINT frames = argc > 2 ? atoi(argv[2]) : 3, size = 256, buffers = 2;
  auto vs = compiler.compile(hlsl, "vs", "vs"), ps = compiler.compile(hlsl, "ps", "ps");
  if (vs.empty() || ps.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }

  WNDCLASSA wc{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr, "d3d12_present"};
  RegisterClassA(&wc);
  RECT rect{0, 0, (LONG)size, (LONG)size};
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  auto hwnd = CreateWindowA(
      "d3d12_present", "d3d12_present", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, rect.right - rect.left,
      rect.bottom - rect.top, nullptr, nullptr, wc.hInstance, nullptr
  );

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
  DXGI_SWAP_CHAIN_DESC1 sc_desc{size,   size,   DXGI_FORMAT_R8G8B8A8_UNORM,
                                FALSE,  {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT,
                                buffers};
  sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  ComPtr<IDXGISwapChain1> sc1;
  CHECK(factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &sc_desc, nullptr, nullptr, &sc1));
  ComPtr<IDXGISwapChain3> swapchain;
  CHECK(sc1.As(&swapchain));
  unsigned mismatches = 0;
  auto expect = [&](bool ok, const char *what, long long got, long long want) {
    if (!ok && mismatches++ < 12)
      printf("%s: %#llx, want %#llx\n", what, got, want);
  };
  ComPtr<IDXGIOutput> restricted;
  HRESULT hr = swapchain->GetRestrictToOutput(&restricted);
  expect(hr == S_OK && !restricted, "the output of a swap chain restricted to none", hr, S_OK);
  const DXGI_RGBA color{0.25f, 0.5f, 0.75f, 1}, beyond{0.25f, 1.5f, 0.75f, 1};
  DXGI_RGBA got_color{};
  expect(swapchain->SetBackgroundColor(&color) == S_OK && swapchain->GetBackgroundColor(&got_color) == S_OK &&
             !memcmp(&got_color, &color, sizeof(color)),
         "the background color", got_color.g * 4, color.g * 4);
  expect((hr = swapchain->SetBackgroundColor(&beyond)) == E_INVALIDARG, "a color beyond 1", hr, E_INVALIDARG);
  expect((hr = swapchain->SetBackgroundColor(nullptr)) == E_INVALIDARG, "no color", hr, E_INVALIDARG);
  expect(swapchain->GetBackgroundColor(&got_color) == S_OK && !memcmp(&got_color, &color, sizeof(color)),
         "the background color after refused ones", got_color.g * 4, color.g * 4);
  DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_UNSPECIFIED;
  expect(swapchain->GetRotation(&rotation) == S_OK && rotation == DXGI_MODE_ROTATION_IDENTITY, "the first rotation",
         rotation, DXGI_MODE_ROTATION_IDENTITY);
  expect((hr = swapchain->SetRotation(DXGI_MODE_ROTATION_ROTATE90)) == S_OK &&
             swapchain->GetRotation(&rotation) == S_OK && rotation == DXGI_MODE_ROTATION_ROTATE90,
         "a rotation", rotation, DXGI_MODE_ROTATION_ROTATE90);
  CHECK(swapchain->SetRotation(DXGI_MODE_ROTATION_IDENTITY));
  UINT source[2]{};
  auto source_is = [&](UINT width, UINT height, const char *what) {
    expect(swapchain->GetSourceSize(&source[0], &source[1]) == S_OK && source[0] == width && source[1] == height, what,
           source[0] << 16 | source[1], width << 16 | height);
  };
  source_is(size, size, "the first source size");
  expect((hr = swapchain->SetSourceSize(0, size)) == E_INVALIDARG, "a source of no width", hr, E_INVALIDARG);
  expect((hr = swapchain->SetSourceSize(size, size + 1)) == E_INVALIDARG, "a source beyond the buffer", hr, E_INVALIDARG);
  source_is(size, size, "the source size after refused ones");

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
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
  UINT64 bytes;
  auto bb_desc = back[0]->GetDesc();
  device->GetCopyableFootprints(&bb_desc, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);

  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  for (UINT f = 0; f < frames; f++) {
    // the last frame is presented from the quarter at the buffer's origin
    if (f == frames - 1) {
      expect((hr = swapchain->SetSourceSize(size / 2, size / 2)) == S_OK, "a source size", hr, S_OK);
      source_is(size / 2, size / 2, "the source size");
    }
    auto i = swapchain->GetCurrentBackBufferIndex();
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), pso.Get()));
    transition(list.Get(), back[i].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{rtv_heap->GetCPUDescriptorHandleForHeapStart().ptr + i * rtv_step};
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->SetGraphicsRootSignature(rs.Get());
    D3D12_VIEWPORT viewport{0, 0, (float)size, (float)size, 0, 1};
    D3D12_RECT scissor{0, 0, (LONG)size, (LONG)size};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    transition(list.Get(), back[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{back[i].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
        dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = fp}};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(list.Get(), back[i].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));

    unsigned char *pixels;
    CHECK(readback->Map(0, nullptr, (void **)&pixels));
    for (UINT y = 0; y < size; y++)
      for (UINT x = 0; x < size; x++) {
        // uv at the pixel center; unorm results may round either way (1 lsb)
        float want[4] = {(x + 0.5f) / size * 255, (y + 0.5f) / size * 255, 0.5f * 255, 255};
        auto got = pixels + y * fp.Footprint.RowPitch + x * 4;
        for (int c = 0; c < 4; c++)
          if (std::fabs(got[c] - want[c]) > 1.0f && mismatches++ < 4)
            printf("frame %u pixel %u,%u channel %d: got %u, want %.2f\n", f, x, y, c, got[c], want[c]);
      }
    readback->Unmap(0, nullptr);
    CHECK(swapchain->Present(1, 0));
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
      DispatchMessageA(&msg);
  }
  // new buffers are presented whole
  for (auto &b : back)
    b.Reset();
  const UINT resized = size * 3 / 4;
  CHECK(swapchain->ResizeBuffers(buffers, resized, resized, DXGI_FORMAT_UNKNOWN, 0));
  source_is(resized, resized, "the source size of new buffers");
  expect((hr = swapchain->SetSourceSize(size, size)) == E_INVALIDARG, "a source beyond the new buffers", hr, E_INVALIDARG);
  printf("%s: %u mismatching channels over %u frames\n", mismatches ? "failed" : "passed", mismatches, frames);
  return mismatches != 0;
}
