// the workloads tests/held.py profiles: each frame's bound is known by construction and the program measures it
// without the profile, as a 'fact' line. argv: <front end> <gpu|app|hitch|events> <frames>.
// gpu: one draw of a pixel shader that loops; the frame's GPU time is its timestamp queries' difference
// ("Timestamp frequency ... ticks per second", ID3D12CommandQueue::GetTimestampFrequency). every mode says too how
// long each frame ran outside its submission and its Present, from the last Present's return.
// app: the program sleeps with nothing outstanding on the GPU before it records its frame.
// hitch: the middle frame makes pipelines of new shaders, and draws with one; QueryPerformanceCounter times it.
// events: frame f runs in the queue's event "frame" and draws in the list's event "draw f", whose data is in the
// encoding f picks of those pix_event writes.
#include "d3d12_test.hpp"
#include <dxgi1_4.h>
#include <cstring>
#include <algorithm>

static const char hlsl[] = R"hlsl(
#ifndef SEED
#define SEED 0
#define TURNS 1
#endif
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 pos : SV_Position) : SV_Target {
  uint value = uint(pos.x) * 7919u + uint(pos.y) + SEED;
  [loop] for (uint i = 0; i < TURNS; i++) value = value * 1664525u + 1013904223u;
  return float4(value & 255, 0, 0, 255) / 255;
}
)hlsl";
// BeginEvent's data in each encoding PIX gives it (pix3_win.h): 0 a wide string, 1 an ANSI one, 2 a blob of
// PIXEventsLegacy.h (EncodeBeginEventForContext), WINPIX_EVENT_PIX3BLOB_V2 one of PIXEventsCommon.h, with a color
// word and a wide name, or an index color and an ANSI name; a blob's name follows its string info word
static const UINT encodings[][2] = {{0, 0}, {1, 0}, {2, 0}, {6345127, 0xf1}, {6345127, 0x33}};

static std::vector<char>
pix_event(UINT metadata, UINT flags, const std::string &name) {
  std::wstring wide(name.begin(), name.end());
  bool ansi = metadata == 1 || metadata == 2 || flags & 0x2;
  std::vector<char> text = ansi ? std::vector<char>(name.c_str(), name.c_str() + name.size() + 1)
                                : std::vector<char>((const char *)wide.c_str(), (const char *)(wide.c_str() + wide.size() + 1));
  if (metadata < 2)
    return text;
  std::vector<UINT64> words;
  if (metadata == 2)
    // PIXEvent_BeginEvent_OnContext_NoArgs (0x12) at bits 10-19, then the color
    words = {UINT64(0x12) << 10, 0xff336699};
  else {
    // PIXEvent_BeginEvent (1) at bits 7-11 and the flags at 12-19: HAS_COLOR (0xF0) is a color word, ON_CONTEXT (0x1)
    // the context pointer, STRING_IS_ANSI (0x2)
    words = {UINT64(1) << 7 | UINT64(flags) << 12};
    if ((flags & 0xf0) == 0xf0)
      words.push_back(0xff336699);
    words.push_back(0x123456789a);
  }
  // the string info: copy chunk size 8 at bits 55-59, IsANSI at bit 54
  words.push_back(UINT64(8) << 55 | UINT64(ansi) << 54);
  text.resize((text.size() + 7) / 8 * 8);
  auto at = words.size();
  words.resize(at + text.size() / 8);
  memcpy(&words[at], text.data(), text.size());
  // the event's size in words, at bits 0-6 of V2's header, counts up to the terminating word
  if (metadata != 2)
    words[0] |= words.size();
  words.push_back(0);
  return std::vector<char>((const char *)words.data(), (const char *)(words.data() + words.size()));
}

// the most pipelines the hitch frame may make: it makes them until it lasts three of the frames before it, whose
// length the display can set (a frame waits for a drawable), so a hitch is one under any display; and how long the
// app frame sleeps
static constexpr UINT made = 64, size = 64, buffers = 2;
static constexpr DWORD sleep_ms = 20;

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  std::string mode = argc > 2 ? argv[2] : "gpu";
  const UINT frames = argc > 3 ? atoi(argv[3]) : 48, hitch = frames / 2;
  // the GPU frame loops long enough to outlast anything the CPU does in it, on the slowest GPU too
  auto turns = std::to_string(mode == "gpu" ? 1 << 20 : 1);
  auto vs = compiler.compile(hlsl, "vs", "vs");
  std::vector<std::string> ps;
  // the run's own shaders, which no cache of an earlier run has
  const UINT run = GetCurrentProcessId() ^ GetTickCount();
  for (UINT seed = 0; seed <= (mode == "hitch" ? made : 0); seed++)
    ps.push_back(compiler.compile(hlsl, "ps", "ps", {"TURNS=" + turns, "SEED=" + std::to_string(run + seed)}));
  for (auto &code : ps)
    if (vs.empty() || code.empty()) {
      printf("failed: HLSL did not compile\n");
      return 1;
    }

  WNDCLASSA wc{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr, "held"};
  RegisterClassA(&wc);
  auto hwnd = CreateWindowA("held", "held", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, size, size, nullptr, nullptr,
                            wc.hInstance, nullptr);
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  auto rs = root_signature(device.Get(), {});
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = rs.Get();
  desc.VS = bytecode(vs);
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc = {1, 0};
  auto pipeline = [&](const std::string &code, ComPtr<ID3D12PipelineState> &pso) {
    desc.PS = bytecode(code);
    return device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
  };
  ComPtr<ID3D12PipelineState> pso;
  CHECK(pipeline(ps[0], pso));

  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<IDXGIFactory4> factory;
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  DXGI_SWAP_CHAIN_DESC1 sc_desc{size, size, DXGI_FORMAT_R8G8B8A8_UNORM, FALSE, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT,
                                buffers};
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
    device->CreateRenderTargetView(back[i].Get(), nullptr, {rtv_heap->GetCPUDescriptorHandleForHeapStart().ptr + i * rtv_step});
  }
  ComPtr<ID3D12QueryHeap> timestamps;
  D3D12_QUERY_HEAP_DESC query_desc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2};
  CHECK(device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&timestamps)));
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 2 * sizeof(UINT64), D3D12_RESOURCE_STATE_COPY_DEST);
  UINT64 ticks;
  CHECK(queue->GetTimestampFrequency(&ticks));
  LARGE_INTEGER qpc;
  QueryPerformanceFrequency(&qpc);

  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  auto now = [&] {
    LARGE_INTEGER time;
    QueryPerformanceCounter(&time);
    return time.QuadPart / qpc.QuadPart * 1000000000 + time.QuadPart % qpc.QuadPart * 1000000000 / qpc.QuadPart;
  };
  auto presented = now();
  std::vector<long long> lengths;
  for (UINT f = 0; f < frames; f++) {
    if (mode == "app")
      Sleep(sleep_ms);
    if (mode == "hitch" && f == hitch) {
      std::nth_element(lengths.begin(), lengths.begin() + lengths.size() / 2, lengths.end());
      auto began = now(), median = lengths[lengths.size() / 2];
      for (UINT seed = 1; seed <= made && now() - began < 3 * median; seed++)
        CHECK(pipeline(ps[seed], pso));
      if (now() - began < 3 * median) {
        printf("failed: %u pipelines took less than three frames of %lld ns\n", made, median);
        return 1;
      }
      printf("fact hitch %u %lld\n", f, now() - began);
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
    list->EndQuery(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    auto [metadata, flags] = encodings[f % std::size(encodings)];
    if (mode == "events") {
      auto data = pix_event(metadata, flags, "draw " + std::to_string(f));
      list->BeginEvent(metadata, data.data(), data.size());
      printf("fact event %u %u\n", f, f % UINT(std::size(encodings)));
    }
    list->DrawInstanced(3, 1, 0, 0);
    if (mode == "events")
      list->EndEvent();
    list->EndQuery(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
    list->ResolveQueryData(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, readback.Get(), 0);
    transition(list.Get(), back[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    auto submitted = now();
    if (mode == "events")
      queue->BeginEvent(1, "frame", sizeof "frame");
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    if (mode == "events")
      queue->EndEvent();
    auto waited = now() - submitted;
    UINT64 *clock;
    CHECK(readback->Map(0, nullptr, (void **)&clock));
    printf("fact gpu %u %lld\n", f, (long long)((clock[1] - clock[0]) * 1000000000 / ticks));
    readback->Unmap(0, nullptr);
    printf("fact app %u %lld\n", f, now() - presented - waited);
    CHECK(swapchain->Present(0, 0));
    lengths.push_back(now() - presented);
    printf("fact frame %u %lld\n", f, lengths.back());
    presented = now();
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
      DispatchMessageA(&msg);
  }
  if (mode == "app")
    printf("fact sleep %lu\n", sleep_ms * 1000000ul);
  printf("passed: %u frames\n", frames);
  return 0;
}
