// contract: dispatch, copy and draw results preserve submission order while Metal 4 combines compute-class work.
// "all UAV accesses must complete before any future UAV accesses can begin" (Microsoft, D3D12_RESOURCE_UAV_BARRIER,
// https://learn.microsoft.com/windows/win32/api/d3d12/ns-d3d12-d3d12_resource_uav_barrier).
// timestamps sample "at the instant that the GPU is finished with all the preceding workload" (D3D11.3 functional
// specification, 20.4.3). "Encodes a compute pass and other memory operations into a command buffer." (Apple,
// MTL4ComputeCommandEncoder.h). a timestamp is sampled where its encoder ends, and one between draws where the open
// render pass ends: the draws on both sides of it share the pass.
#include "d3d12_test.hpp"
#include "../../src/winemetal/winemetal.h"
#include <algorithm>
#include <filesystem>
#include <fstream>

static constexpr UINT group = 64;
// below, at and above the shader's threadgroup boundary, and of so many groups that work begun beside the dispatch
// would read what it has not written yet
static constexpr UINT sizes[] = {group - 1, group, group + 1, group *group + 1, group *group *group};
static const char hlsl[] = R"hlsl(
RWStructuredBuffer<uint> source : register(u0);
RWStructuredBuffer<uint> destination : register(u1);
cbuffer Parameters : register(b0) { uint count; uint phase; };
[numthreads(GROUP, 1, 1)] void cs(uint3 tid : SV_DispatchThreadID) {
  if (tid.x < count)
    destination[tid.x] = phase == 0 ? tid.x : source[count - 1 - tid.x] * 3 + phase;
}
float4 vs(uint id : SV_VertexID) : SV_Position {
  return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0, 1);
}
float4 ps() : SV_Target { return float4(1, 0, 0, 1); }
)hlsl";

static int
run(const std::string &cs, const std::string &vs, const std::string &ps, UINT queries, UINT heaps) {
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER parameters[3] = {
      {D3D12_ROOT_PARAMETER_TYPE_UAV}, {D3D12_ROOT_PARAMETER_TYPE_UAV}, {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}
  };
  parameters[1].Descriptor.ShaderRegister = 1;
  parameters[2].Constants.Num32BitValues = 2;
  auto roots = root_signature(device.Get(), {UINT(std::size(parameters)), parameters});
  auto graphics_roots = root_signature(device.Get(), {});
  CHECK(roots && graphics_roots ? S_OK : E_FAIL);
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc{roots.Get(), bytecode(cs)};
  ComPtr<ID3D12PipelineState> compute, graphics;
  CHECK(device->CreateComputePipelineState(&compute_desc, IID_PPV_ARGS(&compute)));
  D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics_desc{graphics_roots.Get(), bytecode(vs), bytecode(ps)};
  graphics_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  graphics_desc.SampleMask = ~0u;
  graphics_desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  graphics_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  graphics_desc.NumRenderTargets = 1;
  graphics_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  graphics_desc.SampleDesc = {1, 0};
  CHECK(device->CreateGraphicsPipelineState(&graphics_desc, IID_PPV_ARGS(&graphics)));
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(
      device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), compute.Get(), IID_PPV_ARGS(&list))
  );

  const UINT count = sizes[std::size(sizes) - 1];
  const UINT64 bytes = UINT64(count) * sizeof(UINT);
  ComPtr<ID3D12Resource> work[4];
  for (auto &resource : work) {
    resource = buffer(
        device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
    );
    CHECK(resource ? S_OK : E_FAIL);
  }
  auto readback =
      buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes * std::size(sizes) * 3, D3D12_RESOURCE_STATE_COPY_DEST);
  CHECK(readback ? S_OK : E_FAIL);
  CHECK(forget(readback.Get()));
  const UINT width = 8, height = 4;
  D3D12_RESOURCE_DESC texture_desc{
      D3D12_RESOURCE_DIMENSION_TEXTURE2D,
      0,
      width,
      height,
      1,
      1,
      DXGI_FORMAT_R8G8B8A8_UNORM,
      {1, 0},
      D3D12_TEXTURE_LAYOUT_UNKNOWN,
      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
  };
  D3D12_HEAP_PROPERTIES properties{D3D12_HEAP_TYPE_DEFAULT};
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(
      &properties, D3D12_HEAP_FLAG_NONE, &texture_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
      IID_PPV_ARGS(&target)
  ));
  ComPtr<ID3D12DescriptorHeap> views;
  D3D12_DESCRIPTOR_HEAP_DESC view_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&view_desc, IID_PPV_ARGS(&views)));
  auto view = views->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, view);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 pixel_bytes;
  device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint, nullptr, nullptr, &pixel_bytes);
  auto pixels = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, pixel_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  CHECK(pixels ? S_OK : E_FAIL);
  // more heaps than Metal makes counter heaps, made and released one after another, then held together
  D3D12_QUERY_HEAP_DESC heap_desc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, queries}, single_desc{heap_desc.Type, 1};
  for (UINT i = 0; i <= heaps; i++) {
    ComPtr<ID3D12QueryHeap> released;
    CHECK(device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&released)));
  }
  ComPtr<ID3D12QueryHeap> heap;
  CHECK(device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&heap)));
  std::vector<ComPtr<ID3D12QueryHeap>> singles(heaps + 1);
  for (auto &single : singles)
    CHECK(device->CreateQueryHeap(&single_desc, IID_PPV_ARGS(&single)));
  const UINT stamps = queries + UINT(singles.size());
  auto clocks =
      buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, UINT64(stamps) * sizeof(UINT64), D3D12_RESOURCE_STATE_COPY_DEST);
  CHECK(clocks ? S_OK : E_FAIL);

  list->SetComputeRootSignature(roots.Get());
  for (UINT size = 0; size < std::size(sizes); size++) {
    step("dispatch/copy/UAV chain of %u elements", sizes[size]);
    auto dispatch = [&](UINT phase, UINT src, UINT dst) {
      list->SetComputeRootUnorderedAccessView(0, work[src]->GetGPUVirtualAddress());
      list->SetComputeRootUnorderedAccessView(1, work[dst]->GetGPUVirtualAddress());
      const UINT constants[] = {sizes[size], phase};
      list->SetComputeRoot32BitConstants(2, UINT(std::size(constants)), constants, 0);
      list->Dispatch((sizes[size] + group - 1) / group, 1, 1);
    };
    auto copy = [&](UINT src, UINT dst) {
      transition(list.Get(), work[src].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      transition(list.Get(), work[dst].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
      list->CopyBufferRegion(work[dst].Get(), 0, work[src].Get(), 0, UINT64(sizes[size]) * sizeof(UINT));
      transition(list.Get(), work[src].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      transition(list.Get(), work[dst].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    };
    dispatch(0, 1, 0);
    copy(0, 1);
    dispatch(1, 1, 2);
    copy(2, 3);
    D3D12_RESOURCE_BARRIER barrier{D3D12_RESOURCE_BARRIER_TYPE_UAV};
    list->ResourceBarrier(1, &barrier);
    dispatch(2, 3, 0);
    const UINT results[] = {1, 3, 0};
    for (UINT phase = 0; phase < std::size(results); phase++) {
      auto result = work[results[phase]].Get();
      transition(list.Get(), result, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyBufferRegion(
          readback.Get(), bytes * (size * std::size(results) + phase), result, 0, UINT64(sizes[size]) * sizeof(UINT)
      );
      transition(list.Get(), result, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
  }
  list->SetPipelineState(graphics.Get());
  list->SetGraphicsRootSignature(graphics_roots.Get());
  list->OMSetRenderTargets(1, &view, FALSE, nullptr);
  D3D12_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
  list->RSSetViewports(1, &viewport);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  D3D12_RECT left{0, 0, width / 2, height}, right{width / 2, 0, width, height};
  list->RSSetScissorRects(1, &left);
  list->DrawInstanced(3, 1, 0, 0);
  for (UINT i = 0; i < queries; i++)
    list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i);
  for (auto &single : singles)
    list->EndQuery(single.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
  list->RSSetScissorRects(1, &right);
  list->DrawInstanced(3, 1, 0, 0);
  list->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, queries, clocks.Get(), 0);
  for (UINT i = 0; i < singles.size(); i++)
    list->ResolveQueryData(
        singles[i].Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 1, clocks.Get(), UINT64(queries + i) * sizeof(UINT64)
    );
  transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  D3D12_TEXTURE_COPY_LOCATION dst{pixels.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  dst.PlacedFootprint = footprint;
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  UINT64 before, after, cpu;
  CHECK(queue->GetClockCalibration(&before, &cpu));
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  CHECK(queue->GetClockCalibration(&after, &cpu));
  step("read all dispatch bytes, pixels and timestamps");
  UINT *words;
  CHECK(readback->Map(0, nullptr, (void **)&words));
  for (UINT size = 0; size < std::size(sizes); size++)
    for (UINT phase = 0; phase < 3; phase++)
      for (UINT i = 0; i < count; i++) {
        UINT want = ~UINT(0);
        if (i < sizes[size])
          want = phase == 0 ? i : phase == 1 ? (sizes[size] - 1 - i) * 3 + 1 : (i * 3 + 1) * 3 + 2;
        UINT got = words[count * (size * 3 + phase) + i];
        if (!expect(got == want, "size %u phase %u element %u: %u, want %u", sizes[size], phase, i, got, want))
          break;
      }
  readback->Unmap(0, nullptr);
  BYTE *image;
  CHECK(pixels->Map(0, nullptr, (void **)&image));
  const BYTE red[] = {255, 0, 0, 255};
  for (UINT y = 0; y < height; y++)
    for (UINT x = 0; x < width; x++)
      expect(
          !memcmp(image + footprint.Offset + y * footprint.Footprint.RowPitch + x * sizeof(red), red, sizeof(red)),
          "pixel %u,%u", x, y
      );
  pixels->Unmap(0, nullptr);
  UINT64 *timestamps, frequency;
  CHECK(queue->GetTimestampFrequency(&frequency));
  expect(frequency != 0, "timestamp frequency is zero");
  CHECK(clocks->Map(0, nullptr, (void **)&timestamps));
  expect(timestamps[0] >= before && timestamps[stamps - 1] <= after, "timestamps %llu to %llu outside %llu to %llu",
         timestamps[0], timestamps[stamps - 1], before, after);
  for (UINT i = 1; i < stamps; i++)
    expect(timestamps[i] >= timestamps[i - 1], "timestamp %u decreased: %llu after %llu", i, timestamps[i],
           timestamps[i - 1]);
  clocks->Unmap(0, nullptr);
  return verdict();
}

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler))
    return 77;
  const std::vector<std::string> defines = {"GROUP=" + std::to_string(group)};
  auto cs = compiler.compile(hlsl, "cs", "cs", defines), vs = compiler.compile(hlsl, "vs", "vs", defines),
       ps = compiler.compile(hlsl, "ps", "ps", defines);
  if (!expect(!cs.empty() && !vs.empty() && !ps.empty(), "HLSL did not compile"))
    return verdict();
  if (argc > 2 && !strcmp(argv[2], "shaders"))
    return verdict();
  auto bridge = LoadLibraryA("winemetal.dll");
  auto all_devices = reinterpret_cast<decltype(&WMTCopyAllDevices)>(GetProcAddress(bridge, "WMTCopyAllDevices"));
  auto object = reinterpret_cast<decltype(&NSArray_object)>(GetProcAddress(bridge, "NSArray_object"));
  auto make = reinterpret_cast<decltype(&MTLCounterSampleBuffer_newTimestampBuffer)>(
      GetProcAddress(bridge, "MTLCounterSampleBuffer_newTimestampBuffer")
  );
  // typed here: a bridge without Metal 4 declares no MTL4CounterHeap_new
  auto make4 =
      reinterpret_cast<obj_handle_t (*)(obj_handle_t, uint64_t)>(GetProcAddress(bridge, "MTL4CounterHeap_new"));
  auto release = reinterpret_cast<decltype(&NSObject_release)>(GetProcAddress(bridge, "NSObject_release"));
  auto family =
      reinterpret_cast<decltype(&MTLDevice_supportsFamily)>(GetProcAddress(bridge, "MTLDevice_supportsFamily"));
  if (!expect(all_devices && object && make && make4 && release && family, "Metal bridge unavailable"))
    return verdict();
  auto devices = all_devices(), metal = object(devices, 0);
  const bool supported = family(metal, WMTGPUFamilyMetal4);
  UINT capacity = 1;
  for (;;) {
    auto heap = make(metal, capacity * 2, true);
    if (!heap)
      break;
    release(heap);
    capacity *= 2;
  }
  // past the entries of a Metal 4 counter heap too, and how many of those Metal makes (none before macOS 26)
  for (UINT size = capacity * 2;; size *= 2) {
    auto heap = make4(metal, size);
    if (!heap)
      break;
    release(heap);
    capacity = size;
  }
  std::vector<obj_handle_t> made;
  for (obj_handle_t heap; (heap = make4(metal, 1));)
    made.push_back(heap);
  for (auto heap : made)
    release(heap);
  release(devices);
  if (argc > 2)
    return run(cs, vs, ps, capacity + 1, UINT(made.size()));
  char temporary[MAX_PATH], directory[MAX_PATH], exe[MAX_PATH];
  CHECK(GetTempPathA(sizeof(temporary), temporary) ? S_OK : E_FAIL);
  CHECK(GetModuleFileNameA(nullptr, exe, sizeof(exe)) ? S_OK : E_FAIL);
  for (bool metal4 : {false, true}) {
    if (metal4 && !supported) {
      printf("skipped: device does not support MTLGPUFamilyMetal4\n");
      return trace::wrong ? verdict() : 77;
    }
    step("Metal %u passes", metal4 ? 4 : 3);
    CHECK(GetTempFileNameA(temporary, "mtl", 0, directory) ? S_OK : E_FAIL);
    DeleteFileA(directory);
    CHECK(CreateDirectoryA(directory, nullptr) ? S_OK : E_FAIL);
    SetEnvironmentVariableA("MULLION_LOG_PATH", directory);
    SetEnvironmentVariableA("MULLION_LOG_LEVEL", "info");
    SetEnvironmentVariableA("MULLION_TRACE_ENCODERS", "1");
    SetEnvironmentVariableA("MULLION_DIAG", nullptr);
    SetEnvironmentVariableA("MULLION_D3D12_METAL4", metal4 ? "1" : "0");
    auto command = std::string("\"") + exe + "\" " + (argc > 1 ? argv[1] : "dxil") + " child";
    STARTUPINFOA startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    CHECK(
        CreateProcessA(exe, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process) ? S_OK
                                                                                                              : E_FAIL
    );
    DWORD waited = WaitForSingleObject(process.hProcess, 120000), result = 1;
    if (waited != WAIT_OBJECT_0)
      TerminateProcess(process.hProcess, 1);
    GetExitCodeProcess(process.hProcess, &result);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    expect(waited == WAIT_OBJECT_0 && result == 0, "child result %lu, wait %lu", result, waited);
    unsigned render = 0, compute = 0, blit = 0, traced = 0;
    for (auto &entry : std::filesystem::directory_iterator(directory)) {
      std::ifstream log(entry.path());
      for (std::string line; std::getline(log, line);) {
        unsigned r, c, b, api;
        if (sscanf(line.c_str(), "info:  D3D12 encoders Metal%u: render=%u compute=%u blit=%u", &api, &r, &c, &b) ==
            4) {
          expect(api == (metal4 ? 4u : 3u), "requested Metal%u, got Metal%u", metal4 ? 4 : 3, api);
          render += r;
          compute += c;
          blit += b;
          traced++;
        }
      }
    }
    expect(traced != 0, "no D3D12 encoder trace");
    expect(render == 1, "render passes: %u", render);
    // Metal 3 begins an encoder where the kind of work changes: a size's dispatches are three, its copies three (one,
    // one, and the readbacks together), and the resolves and the texture's copy after the draws one more
    expect(
        metal4 ? compute == 2 && blit == 0 : compute == 3 * std::size(sizes) && blit == 3 * std::size(sizes) + 1,
        "compute=%u blit=%u", compute, blit
    );
    std::filesystem::remove_all(directory);
  }
  return verdict();
}
