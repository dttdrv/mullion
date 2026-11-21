// contract: work submitted after the GPU has idled for seconds completes about as fast as work submitted back to
// back. macOS unwires a process's GPU memory a while after its GPU goes idle, and the first submission after waits for
// all of it to be wired again (on an M6, macOS 27.3: 135 ms after 10 s with 8 GiB, instead of 3 ms), which a game
// shows as a stutter when it resumes from a pause, a load or the background: Mullion keeps its memory resident while
// the GPU idles (src/mullion/mullion_heartbeat.hpp; MULLION_RESIDENCY_HEARTBEAT=0 turns that off and fails this test
// on a Mac that unwires). here a dispatch per 256 MB buffer writes a word of each 16 KB page of 6 GiB of buffers; the
// first dispatch after 10 s idle must take at most four times the median of three back to back, plus 20 ms.
#include "d3d12_test.hpp"
#include <algorithm>
#include <vector>

static const char hlsl[] = R"hlsl(
RWStructuredBuffer<uint> pages : register(u0);
[numthreads(64, 1, 1)]
void cs(uint3 tid : SV_DispatchThreadID) {
  pages[tid.x * 4096] += 1;
}
)hlsl";

static double
milliseconds_since(LARGE_INTEGER start) {
  LARGE_INTEGER now, frequency;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&frequency);
  return double(now.QuadPart - start.QuadPart) * 1000.0 / double(frequency.QuadPart);
}

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto code = compiler.compile(hlsl, "cs", "cs");
  if (code.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER param{D3D12_ROOT_PARAMETER_TYPE_UAV};
  auto rs = root_signature(device.Get(), {1, &param});
  if (!rs) {
    printf("failed: root signature\n");
    return 1;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{rs.Get(), bytecode(code)};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)));
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  constexpr UINT64 kBufferBytes = 256ull << 20, kPage = 16384;
  constexpr UINT kBuffers = 24; // 6 GiB
  std::vector<ComPtr<ID3D12Resource>> buffers;
  for (UINT i = 0; i < kBuffers; i++) {
    auto b = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, kBufferBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!b)
      break;
    buffers.push_back(b);
  }
  if (buffers.size() < 8) {
    printf("skipped: only %zu of the 256 MB buffers could be made\n", buffers.size());
    return 77;
  }

  auto frame = [&]() -> double {
    allocator->Reset();
    list->Reset(allocator.Get(), pso.Get());
    list->SetComputeRootSignature(rs.Get());
    for (auto &b : buffers) {
      list->SetComputeRootUnorderedAccessView(0, b->GetGPUVirtualAddress());
      list->Dispatch(UINT(kBufferBytes / kPage / 64), 1, 1);
    }
    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);
    if (FAILED(submit(device.Get(), queue.Get(), list.Get())))
      return -1;
    return milliseconds_since(start);
  };
  for (int i = 0; i < 3; i++)
    frame();
  std::vector<double> steady;
  for (int i = 0; i < 3; i++)
    steady.push_back(frame());
  std::sort(steady.begin(), steady.end());
  Sleep(10000);
  double first = frame();
  double limit = steady[1] * 4 + 20;
  printf("%zu MB resident: back to back %.2f ms, first after 10 s idle %.2f ms (limit %.2f ms)\n",
         buffers.size() * (kBufferBytes >> 20), steady[1], first, limit);
  expect(steady[0] >= 0 && first >= 0, "the dispatches ran");
  expect(first <= limit, "the first submission after the GPU idled waited for its memory");
  return verdict();
}
