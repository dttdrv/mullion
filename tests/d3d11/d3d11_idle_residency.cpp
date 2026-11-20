// contract: work submitted after the GPU has idled for seconds completes about as fast as work submitted back to
// back, as tests/d3d12/d3d12_idle_residency.cpp asks of D3D12. D3D11 has no residency set of its own; its buffers and
// textures of a MiB or more stay wired while the GPU idles in the residency heartbeat's (src/mullion/mullion_heartbeat.hpp;
// MULLION_RESIDENCY_HEARTBEAT=0 turns that off and fails this test on a Mac that unwires). here a dispatch per 256 MB
// buffer writes a word of each 16 KB page of 6 GiB of buffers; the first dispatch after 10 s idle must take at most
// four times the median of three back to back, plus 20 ms.
#include "d3d11_test.hpp"
#include <algorithm>

static const char hlsl[] = R"hlsl(
RWByteAddressBuffer pages : register(u0);
[numthreads(64, 1, 1)]
void cs(uint3 tid : SV_DispatchThreadID) {
  pages.Store(tid.x * 16384, pages.Load(tid.x * 16384) + 1);
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
main() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                          &context));
  auto code = compile(hlsl, "cs", "cs");
  if (!code) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D11ComputeShader> cs;
  CHECK(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs));

  constexpr UINT kBufferBytes = 256u << 20, kPage = 16384, kBuffers = 24; // 6 GiB
  std::vector<ComPtr<ID3D11UnorderedAccessView>> views;
  std::vector<ComPtr<ID3D11Buffer>> buffers;
  for (UINT i = 0; i < kBuffers; i++) {
    D3D11_BUFFER_DESC desc{kBufferBytes, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                           D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS};
    ComPtr<ID3D11Buffer> b;
    if (FAILED(device->CreateBuffer(&desc, nullptr, &b)))
      break;
    D3D11_UNORDERED_ACCESS_VIEW_DESC view{DXGI_FORMAT_R32_TYPELESS, D3D11_UAV_DIMENSION_BUFFER};
    view.Buffer = {0, kBufferBytes / 4, D3D11_BUFFER_UAV_FLAG_RAW};
    ComPtr<ID3D11UnorderedAccessView> uav;
    CHECK(device->CreateUnorderedAccessView(b.Get(), &view, &uav));
    buffers.push_back(b);
    views.push_back(uav);
  }
  if (buffers.size() < 8) {
    printf("skipped: only %zu of the 256 MB buffers could be made\n", buffers.size());
    return 77;
  }
  D3D11_QUERY_DESC query_desc{D3D11_QUERY_EVENT};
  ComPtr<ID3D11Query> done;
  CHECK(device->CreateQuery(&query_desc, &done));

  auto frame = [&]() -> double {
    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);
    context->CSSetShader(cs.Get(), nullptr, 0);
    for (auto &uav : views) {
      context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
      context->Dispatch(kBufferBytes / kPage / 64, 1, 1);
    }
    context->End(done.Get());
    context->Flush();
    BOOL finished = FALSE;
    while (context->GetData(done.Get(), &finished, sizeof(finished), 0) != S_OK)
      ;
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
  if (first > limit) {
    printf("failed: the first submission after the GPU idled waited for its memory\n");
    return 1;
  }
  printf("passed\n");
  return 0;
}
