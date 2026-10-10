// contract: profiling records each pass's stages and pipelines, each present and each submission without changing
// results. "When the GPU starts or finishes a stage, it samples the counters" (Apple, Sampling GPU data into counter
// sample buffers, Sample counters at stage boundaries). queries retain "The value should be sampled at the instant
// that the GPU is finished with all the preceding workload." (D3D11.3 20.4.3). the record follows the profile contract.
#include "d3d12_test.hpp"
#include <dxgi1_4.h>
#include <wincrypt.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <climits>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

static constexpr UINT frames = 4, concurrent_frames = 12, width = 8, height = 4, turns = 4096, threads = 32;
static const char hlsl[] = R"hlsl(
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 red() : SV_Target { return float4(1, 0, 0, 1); }
float4 green() : SV_Target { return float4(0, 1, 0, 1); }
RWStructuredBuffer<uint> result : register(u0);
cbuffer Args : register(b0) { uint seed; uint slot; }
[numthreads(THREADS, 1, 1)] void cs(uint id : SV_DispatchThreadID) {
  uint value = seed + id;
  [loop] for (uint i = 0; i < TURNS; i++) value = value * 1664525u + 1013904223u;
  result[slot * THREADS + id] = value;
}
)hlsl";

static std::string
sha1(const std::string &code) {
  auto dll = LoadLibraryA("advapi32.dll");
  auto acquire = (decltype(&CryptAcquireContextA))GetProcAddress(dll, "CryptAcquireContextA");
  auto create = (decltype(&CryptCreateHash))GetProcAddress(dll, "CryptCreateHash");
  auto hash = (decltype(&CryptHashData))GetProcAddress(dll, "CryptHashData");
  auto get = (decltype(&CryptGetHashParam))GetProcAddress(dll, "CryptGetHashParam");
  auto destroy = (decltype(&CryptDestroyHash))GetProcAddress(dll, "CryptDestroyHash");
  auto release = (decltype(&CryptReleaseContext))GetProcAddress(dll, "CryptReleaseContext");
  HCRYPTPROV provider = 0;
  HCRYPTHASH state = 0;
  BYTE bytes[160 / CHAR_BIT]; // FIPS 180-4, SHA-1: a 160-bit digest
  DWORD size = sizeof(bytes);
  std::string name;
  if (expect(acquire && create && hash && get && destroy && release &&
                 acquire(&provider, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT) &&
                 create(provider, CALG_SHA1, 0, 0, &state) && hash(state, (const BYTE *)code.data(), code.size(), 0) &&
                 get(state, HP_HASHVAL, bytes, &size, 0),
             "SHA-1 failed")) {
    for (auto byte : bytes) {
      char hex[3];
      snprintf(hex, sizeof(hex), "%02x", byte);
      name += hex;
    }
    name += ' ';
  }
  if (state)
    destroy(state);
  if (provider)
    release(provider, 0);
  FreeLibrary(dll);
  return name;
}

static UINT64
now() {
  LARGE_INTEGER ticks, frequency;
  QueryPerformanceCounter(&ticks);
  QueryPerformanceFrequency(&frequency);
  return ticks.QuadPart / frequency.QuadPart * 1'000'000'000 +
         ticks.QuadPart % frequency.QuadPart * 1'000'000'000 / frequency.QuadPart;
}

static int
child(char **argv, const ComPtr<ID3D12Device> &device, const std::string &vs, const std::string (&ps)[2],
      const std::string (&cs)[2]) {
  std::ofstream facts(std::string(argv[3]) + "\\facts");
  for (auto &code : ps)
    facts << sha1(vs) + sha1(code) << '\n';
  for (auto &code : cs)
    facts << sha1(code) << '\n';
  auto bridge = LoadLibraryA("winemetal.dll");
  auto write = (UINT (*)(const char *, const char *, UINT64))GetProcAddress(bridge, "WMTDiagWrite");
  const UINT pid = write ? write(nullptr, nullptr, 0) : 0;
  char setting[32]{};
  GetEnvironmentVariableA("DXMT_DIAG", setting, sizeof(setting));
  const bool profiling = !strcmp(setting, "profile");
  if (profiling && pid)
    std::ofstream(std::string(argv[3]) + "\\mullion-" + std::to_string(pid) + ".diag") << "stale\n";
  facts << pid << '\n' << now() << '\n';

  auto devices = (UINT64 (*)())GetProcAddress(bridge, "WMTCopyAllDevices");
  auto object = (UINT64 (*)(UINT64, UINT64))GetProcAddress(bridge, "NSArray_object");
  auto make = (UINT64 (*)(UINT64, UINT, bool))GetProcAddress(bridge, "MTLCounterSampleBuffer_newTimestampBuffer");
  auto release = (void (*)(UINT64))GetProcAddress(bridge, "NSObject_release");
  if (!expect(devices && object && make && release, "counter sample buffer bridge unavailable"))
    return verdict();
  auto all = devices(), metal = object(all, 0);
  UINT capacity = 1;
  for (;;) {
    auto sample_buffer = make(metal, capacity * 2, true);
    if (!sample_buffer)
      break;
    release(sample_buffer);
    capacity *= 2;
  }
  release(all);
  const UINT burst = capacity + 1;
  facts << capacity << '\n';
  D3D12_ROOT_PARAMETER params[2]{{D3D12_ROOT_PARAMETER_TYPE_UAV}, {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}};
  params[1].Constants = {0, 0, 2};
  auto rs = root_signature(device.Get(), {UINT(std::size(params)), params});
  if (!expect(rs != nullptr, "root signature failed"))
    return verdict();
  ComPtr<ID3D12PipelineState> graphics[2], compute[2];
  for (UINT i = 0; i < std::size(graphics); i++) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs), bytecode(ps[i])};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = ~0u;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc = {1, 0};
    CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&graphics[i])));
    D3D12_COMPUTE_PIPELINE_STATE_DESC of{rs.Get(), bytecode(cs[i])};
    CHECK(device->CreateComputePipelineState(&of, IID_PPV_ARGS(&compute[i])));
  }
  ComPtr<ID3D12CommandQueue> queues[2];
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  for (auto &queue : queues)
    CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocators[2];
  ComPtr<ID3D12GraphicsCommandList> lists[2];
  for (UINT i = 0; i < std::size(lists); i++) {
    CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[i])));
    CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[i].Get(), nullptr,
                                    IID_PPV_ARGS(&lists[i])));
  }
  ComPtr<IDXGIFactory4> factory;
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  WNDCLASSA wc{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr, "d3d12_profile"};
  RegisterClassA(&wc);
  HWND windows[2];
  ComPtr<IDXGISwapChain3> chains[2];
  ComPtr<ID3D12Resource> backs[2];
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, UINT(std::size(backs))};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  for (UINT i = 0; i < std::size(chains); i++) {
    windows[i] = CreateWindowA(wc.lpszClassName, wc.lpszClassName, WS_OVERLAPPEDWINDOW, 0, 0, 160, 120, nullptr,
                               nullptr, wc.hInstance, nullptr);
    if (!expect(windows[i] != nullptr, "window creation failed"))
      return verdict();
    DXGI_SWAP_CHAIN_DESC1 desc{
        width, height, DXGI_FORMAT_R8G8B8A8_UNORM, FALSE, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT, 2};
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> chain;
    CHECK(factory->CreateSwapChainForHwnd(queues[i].Get(), windows[i], &desc, nullptr, nullptr, &chain));
    CHECK(chain.As(&chains[i]));
    CHECK(chains[i]->GetBuffer(0, IID_PPV_ARGS(&backs[i])));
    D3D12_CPU_DESCRIPTOR_HANDLE handle{rtv.ptr + i * device->GetDescriptorHandleIncrementSize(rtv_desc.Type)};
    device->CreateRenderTargetView(backs[i].Get(), nullptr, handle);
  }
  auto target_desc = backs[0]->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 size;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &size);
  auto pixels = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, size, D3D12_RESOURCE_STATE_COPY_DEST);
  auto output = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, sizeof(UINT) * threads * 2,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto bytes =
      buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, sizeof(UINT) * threads * 2, D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_QUERY_HEAP_DESC query_desc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2};
  auto queries = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, std::size(compute) * query_desc.Count * sizeof(UINT64),
                        D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(pixels && output && bytes && queries, "readback allocation failed"))
    return verdict();
  ComPtr<ID3D12QueryHeap> timestamps;
  CHECK(device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&timestamps)));
  auto list = lists[0].Get();
  transition(list, backs[0].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
  const float black[4] = {0, 0, 0, 1};
  list->ClearRenderTargetView(rtv, black, 0, nullptr);
  list->SetGraphicsRootSignature(rs.Get());
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  D3D12_VIEWPORT viewport{0, 0, width, height, 0, 1};
  list->RSSetViewports(1, &viewport);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  for (UINT i = 0; i < std::size(graphics); i++) {
    D3D12_RECT scissor{LONG(i * width / 2), 0, LONG((i + 1) * width / 2), height};
    list->RSSetScissorRects(1, &scissor);
    list->SetPipelineState(graphics[i].Get());
    list->DrawInstanced(3, 1, 0, 0);
  }
  list->SetComputeRootSignature(rs.Get());
  list->SetComputeRootUnorderedAccessView(0, output->GetGPUVirtualAddress());
  for (UINT i = 0; i < std::size(compute); i++) {
    list->EndQuery(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    list->SetPipelineState(compute[i].Get());
    UINT args[] = {7, i};
    list->SetComputeRoot32BitConstants(1, std::size(args), args, 0);
    list->Dispatch(1, 1, 1);
    list->EndQuery(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
    list->ResolveQueryData(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, query_desc.Count, queries.Get(),
                           i * query_desc.Count * sizeof(UINT64));
  }
  transition(list, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyResource(bytes.Get(), output.Get());
  transition(list, output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  transition(list, backs[0].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION source{backs[0].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
      dest{pixels.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
  list->CopyTextureRegion(&dest, 0, 0, 0, &source, nullptr);
  transition(list, backs[0].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
  CHECK(list->Close());
  CHECK(lists[1]->Close());
  std::ofstream result(std::string(argv[3]) + "\\pixels", std::ios::binary);
  ComPtr<ID3D12Fence> held;
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&held)));
  for (UINT frame = 0; frame < frames; frame++) {
    step("child frame %u: replay two pipelines, two kernels and timestamps", frame);
    ID3D12CommandList *submitted[] = {lists[0].Get(), lists[1].Get()};
    if (!frame)
      CHECK(queues[0]->Wait(held.Get(), 1));
    queues[0]->ExecuteCommandLists(std::size(submitted), submitted);
    if (!frame)
      CHECK(held->Signal(1));
    ComPtr<ID3D12Fence> fence;
    CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    auto event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    CHECK(queues[0]->Signal(fence.Get(), 1));
    CHECK(fence->SetEventOnCompletion(1, event));
    expect(WaitForSingleObject(event, 60000) == WAIT_OBJECT_0, "GPU timed out");
    CloseHandle(event);
    CHECK(device->GetDeviceRemovedReason());
    UINT *words;
    CHECK(bytes->Map(0, nullptr, (void **)&words));
    for (UINT slot = 0; slot < 2; slot++)
      for (UINT id = 0; id < threads; id++) {
        UINT want = 7 + id;
        for (UINT i = 0; i < (slot ? turns : 1); i++)
          want = want * 1664525u + 1013904223u;
        expect(words[slot * threads + id] == want, "kernel output wrong at %u/%u", slot, id);
      }
    result.write((const char *)words, bytes->GetDesc().Width);
    bytes->Unmap(0, nullptr);
    UINT8 *rgba;
    CHECK(pixels->Map(0, nullptr, (void **)&rgba));
    for (UINT y = 0; y < height; y++)
      for (UINT x = 0; x < width; x++) {
        const UINT8 want[4] = {UINT8(x < width / 2 ? 255 : 0), UINT8(x < width / 2 ? 0 : 255), 0, 255};
        const auto at = rgba + y * footprint.Footprint.RowPitch + x * sizeof(want);
        expect(!memcmp(at, want, sizeof(want)), "pixel wrong at %u/%u", x, y);
        result.write((const char *)at, sizeof(want));
      }
    pixels->Unmap(0, nullptr);
    UINT64 *clock;
    CHECK(queries->Map(0, nullptr, (void **)&clock));
    for (UINT i = 0; i < std::size(compute); i++)
      expect(clock[i * query_desc.Count + 1] > clock[i * query_desc.Count],
             "timestamp query did not bracket kernel %u", i);
    auto long_query = clock + (std::size(compute) - 1) * query_desc.Count;
    facts << long_query[0] << ' ' << long_query[1] << '\n';
    queries->Unmap(0, nullptr);
    CHECK(chains[0]->Present(0, 0));
  }
  step("child: more passes than one counter buffer holds, with timestamp queries in the same command buffer");
  CHECK(allocators[1]->Reset());
  CHECK(lists[1]->Reset(allocators[1].Get(), nullptr));
  auto many = lists[1].Get();
  many->EndQuery(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
  auto other_rtv = rtv;
  other_rtv.ptr += device->GetDescriptorHandleIncrementSize(rtv_desc.Type);
  transition(many, backs[1].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
  for (UINT i = 0; i < burst; i++)
    many->ClearRenderTargetView(other_rtv, black, 0, nullptr);
  many->EndQuery(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
  many->ResolveQueryData(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, queries.Get(), 0);
  transition(many, backs[1].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
  CHECK(submit(device.Get(), queues[1].Get(), many));
  UINT64 *clock;
  CHECK(queries->Map(0, nullptr, (void **)&clock));
  expect(clock[1] > clock[0], "overflow lost application timestamps");
  facts << clock[0] << '\n' << clock[1] << '\n';
  queries->Unmap(0, nullptr);
  auto go = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  struct Present {
    IDXGISwapChain3 *chain;
    HANDLE go;
  } present{chains[1].Get(), go};
  auto thread = CreateThread(
      nullptr, 0,
      [](void *argument) -> DWORD {
        auto &present = *(Present *)argument;
        if (WaitForSingleObject(present.go, 60000) != WAIT_OBJECT_0)
          return 1;
        for (UINT i = 0; i < concurrent_frames; i++)
          if (FAILED(present.chain->Present(0, 0)))
            return 1;
        return 0;
      },
      &present, 0, nullptr);
  if (!expect(go && thread, "present thread creation failed"))
    return verdict();
  SetEvent(go);
  for (UINT i = 0; i < concurrent_frames; i++)
    expect(SUCCEEDED(chains[0]->Present(0, 0)), "concurrent present failed");
  if (!expect(WaitForSingleObject(thread, 60000) == WAIT_OBJECT_0, "present thread timed out"))
    ExitProcess(verdict());
  DWORD status = 1;
  GetExitCodeThread(thread, &status);
  expect(status == 0, "second queue present failed");
  CloseHandle(thread);
  CloseHandle(go);
  for (auto &chain : chains)
    chain.Reset();
  for (auto &queue : queues)
    queue.Reset();
  for (auto window : windows)
    DestroyWindow(window);
  facts << now() << '\n';
  FreeLibrary(bridge);
  return verdict();
}

static std::vector<std::string>
fields(const std::string &line, char separator = '\t') {
  std::vector<std::string> result;
  std::istringstream stream(line);
  std::string field;
  while (std::getline(stream, field, separator))
    result.push_back(field);
  return result;
}

static UINT64
number(const std::string &field) {
  UINT64 value = 0;
  auto parsed = std::from_chars(field.data(), field.data() + field.size(), value);
  expect(parsed.ec == std::errc{} && parsed.ptr == field.data() + field.size(), "bad number: %s", field.c_str());
  return value;
}

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto vs = compiler.compile(hlsl, "vs", "vs", {"TURNS=1", "THREADS=" + std::to_string(threads)});
  std::string ps[] = {compiler.compile(hlsl, "red", "ps", {"TURNS=1", "THREADS=" + std::to_string(threads)}),
                      compiler.compile(hlsl, "green", "ps", {"TURNS=1", "THREADS=" + std::to_string(threads)})};
  std::string cs[] = {
      compiler.compile(hlsl, "cs", "cs", {"TURNS=1", "THREADS=" + std::to_string(threads)}),
      compiler.compile(hlsl, "cs", "cs", {"TURNS=" + std::to_string(turns), "THREADS=" + std::to_string(threads)})};
  if (!expect(!vs.empty() && !ps[0].empty() && !ps[1].empty() && !cs[0].empty() && !cs[1].empty(),
              "HLSL did not compile"))
    return verdict();
  if (argc > 4 && !strcmp(argv[4], "shaders"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  if (argc > 3 && !strcmp(argv[2], "child"))
    return child(argv, device, vs, ps, cs);
  char temporary[MAX_PATH], directory[MAX_PATH], exe[MAX_PATH];
  GetTempPathA(sizeof(temporary), temporary);
  GetModuleFileNameA(nullptr, exe, sizeof(exe));
  auto unix_name =
      (char *(__cdecl *)(const WCHAR *))GetProcAddress(GetModuleHandleA("kernel32.dll"), "wine_get_unix_file_name");
  if (!expect(unix_name != nullptr, "Wine path conversion unavailable"))
    return verdict();
  std::string baseline;
  const char *settings[] = {nullptr, nullptr, "profiles", "profile", "profile"};
  for (UINT run = 0; run < std::size(settings); run++) {
    auto setting = settings[run];
    const bool profiling = setting && !strcmp(setting, "profile");
    step("child DXMT_DIAG=%s, run %u", setting ? setting : "unset", run);
    if (!expect(GetTempFileNameA(temporary, "prf", 0, directory), "no temporary name"))
      return verdict();
    DeleteFileA(directory);
    if (!expect(CreateDirectoryA(directory, nullptr), "no temporary directory"))
      return verdict();
    WCHAR wide[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, directory, -1, wide, std::size(wide));
    char *path = unix_name(wide);
    if (!expect(path != nullptr, "Wine path conversion failed"))
      return verdict();
    SetEnvironmentVariableA("DXMT_DIAG_PATH", path);
    HeapFree(GetProcessHeap(), 0, path);
    SetEnvironmentVariableA("DXMT_DIAG", setting);
    SetEnvironmentVariableA("DXMT_LOG_PATH", "none");
    SetEnvironmentVariableA("DXMT_D3D12_GPU_ERRORS", run == std::size(settings) - 1 ? "1" : nullptr);
    SetEnvironmentVariableA("DXMT_D3D12_ISOLATE", nullptr);
    std::string command =
        std::string("\"") + exe + "\" " + (argc > 1 ? argv[1] : "dxil") + " child \"" + directory + "\"";
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!expect(CreateProcessA(exe, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process),
                "child did not start"))
      return verdict();
    auto waited = WaitForSingleObject(process.hProcess, 120000);
    if (waited != WAIT_OBJECT_0) {
      TerminateProcess(process.hProcess, 1);
      WaitForSingleObject(process.hProcess, 60000);
    }
    DWORD status = 1;
    GetExitCodeProcess(process.hProcess, &status);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    expect(waited == WAIT_OBJECT_0 && status == 0, "child ended with %lu after wait %lu", status, waited);
    if (status == 77)
      return 77;
    std::ifstream pixels(std::string(directory) + "\\pixels", std::ios::binary);
    std::string output((std::istreambuf_iterator<char>(pixels)), {});
    if (!run)
      baseline = output;
    expect(!output.empty() && output == baseline, "profile changed pixels or buffers");
    pixels.close();
    std::ifstream facts(std::string(directory) + "\\facts");
    std::string names[4];
    for (auto &name : names)
      std::getline(facts, name);
    UINT pid = 0, capacity = 0;
    UINT64 began = 0, ended = 0, query_start = 0, query_end = 0;
    facts >> pid >> began >> capacity;
    std::array<UINT64, 2> sampled_queries[frames]{};
    for (UINT frame = 0; frame < frames; frame++) {
      auto &query = sampled_queries[frame];
      facts >> query[0] >> query[1];
      expect(query[1] > query[0], "empty query interval: %llu..%llu", query[0], query[1]);
    }
    facts >> query_start >> query_end >> ended;
    expect(facts.good(), "child facts incomplete");
    facts.close();
    using Key = std::pair<std::string, UINT64>;
    std::map<Key, std::vector<std::string>> buffers;
    std::vector<std::vector<std::string>> passes, presents;
    std::map<std::string, UINT> frame_counts;
    UINT files = 0, clocks = 0, spans = 0;
    std::map<UINT64, UINT> list_counts;
    UINT64 last_frame = 0;
    for (auto &entry : std::filesystem::directory_iterator(directory)) {
      if (entry.path().extension() != ".diag")
        continue;
      files++;
      expect(entry.path().filename() == "mullion-" + std::to_string(pid) + ".diag", "wrong unix process id");
      std::ifstream file(entry.path());
      std::string line;
      while (std::getline(file, line)) {
        auto row = fields(line);
        if (!expect(!row.empty(), "empty record line"))
          continue;
        if (row[0] == "clock" && expect(row.size() == 3, "malformed clock")) {
          clocks++;
          expect(began <= number(row[1]) && number(row[1]) <= ended && number(row[2]), "clock outside run");
        } else if (row[0] == "cmdbuf" && expect(row.size() == 7, "malformed command buffer")) {
          expect(buffers.emplace(Key{row[1], number(row[2])}, row).second, "duplicate command buffer");
          expect(began <= number(row[3]) && number(row[3]) <= number(row[4]) && number(row[4]) <= ended,
                 "command buffer CPU clocks outside run");
          expect(number(row[5]) <= number(row[6]), "command buffer GPU times reversed");
        } else if (row[0] == "frame" && expect(row.size() == 4, "malformed frame")) {
          expect(number(row[3]) >= last_frame, "concurrent frames out of time order");
          expect(began <= number(row[3]) && number(row[3]) <= ended, "frame clock outside run");
          last_frame = number(row[3]);
          frame_counts[row[1]]++;
          presents.push_back(row);
        } else if (row[0] == "pass" && expect(row.size() == 9 || row.size() == 11, "malformed pass")) {
          passes.push_back(row);
        } else if (row[0] == "span" && expect(row.size() == 6, "malformed span")) {
          expect(row[1] == "execute", "unexpected span kind");
          spans++;
          list_counts[number(row[2])]++;
          expect(number(row[3]) && number(row[5]) && began <= number(row[4]) &&
                     number(row[4]) + number(row[5]) <= ended,
                 "execute span outside run");
        } else {
          expect(false, "unexpected record kind %s (stale record was not truncated)", row[0].c_str());
        }
      }
    }
    expect(files == UINT(profiling), "got %u records, want %u", files, UINT(profiling));
    if (profiling) {
      expect(clocks == 2, "one clock pair per queue");
      expect(spans == frames + 1 && list_counts[2] == frames && list_counts[1] == 1, "execute spans lost list counts");
      expect(presents.size() == frames + 2 * concurrent_frames && frame_counts.size() == 2, "presents missing");
      if (!presents.empty()) {
        const auto queue = presents.front()[1];
        expect(frame_counts[queue] == frames + concurrent_frames, "main queue presents misattributed");
        for (auto &[other, count] : frame_counts)
          if (other != queue)
            expect(count == concurrent_frames, "second queue presents misattributed");
      }
      std::set<Key> presented;
      for (auto &row : presents) {
        Key key{row[1], number(row[2])};
        expect(buffers.count(key) && presented.insert(key).second, "present has no unique command buffer");
        if (buffers.count(key))
          expect(number(row[3]) <= number(buffers[key][3]), "frame follows its commit");
      }
      std::map<std::string, UINT> kinds;
      std::map<Key, std::vector<const std::vector<std::string> *>> by_buffer;
      std::map<std::string, std::vector<UINT64>> kernel_times;
      std::map<std::string, UINT> run_counts;
      std::set<std::tuple<Key, std::string, UINT64>> seen;
      UINT unsampled = 0;
      for (auto &row : passes) {
        Key key{row[1], number(row[2])};
        kinds[row[4]]++;
        by_buffer[key].push_back(&row);
        expect(seen.emplace(key, row[4], number(row[3])).second, "duplicate pass");
        if (!expect(buffers.count(key), "pass has no command buffer"))
          continue;
        expect(!presented.count(key), "presenter's own pass was sampled");
        expect(row.size() == (row[4] == "Render" ? 11u : 9u), "wrong stage count");
        expect(number(row[5]) > 0, "empty CPU encoding time");
        UINT64 gpu = 0;
        auto &cmdbuf = buffers[key];
        const auto start = number(cmdbuf[5]), finish = number(cmdbuf[6]);
        const UINT64 rounding =
            UINT64(std::ceil(std::nextafter(double(finish) / 1e9, std::numeric_limits<double>::infinity()) * 1e9 -
                             finish)) +
            1;
        for (size_t i = 7; i + 1 < row.size(); i += 2) {
          auto a = number(row[i]), b = number(row[i + 1]);
          if (a == ~UINT64(0) || b == ~UINT64(0)) {
            expect(a == ~UINT64(0) && b == ~UINT64(0), "partial stage sample");
            unsampled++;
          } else {
            expect(a <= b && a + rounding >= start && b <= finish + rounding, "stage outside command buffer");
            gpu += b - a;
          }
        }
        if (row[4] == "Render")
          expect(std::none_of(row.begin() + 7, row.end(), [](auto &time) { return number(time) == ~UINT64(0); }),
                 "render stages were not sampled");
        auto runs = fields(row[6], '|');
        if (row[4] == "Render")
          expect(runs.size() == 2 && runs[0] == names[0] + "*0*0*0" && runs[1] == names[1] + "*0*0*0",
                 "render pipelines missing or out of order");
        else if (row[4] == "Compute") {
          expect(runs.size() == 1 && (runs[0] == names[2] + "*0*0*0" || runs[0] == names[3] + "*0*0*0"),
                 "compute pipeline wrong");
          if (runs.size() == 1) {
            auto &times = kernel_times[runs[0]];
            if (runs[0] == names[3] + "*0*0*0" && times.size() < frames) {
              auto &query = sampled_queries[times.size()];
              expect(query[0] <= number(row[7]) + rounding && number(row[8]) <= query[1] + rounding,
                     "application query does not bracket the sampled kernel");
              auto at = size_t(&row - passes.data());
              if (expect(at > 0 && at + 1 < passes.size(), "kernel has no surrounding query passes")) {
                auto &before = passes[at - 1], &after = passes[at + 1];
                expect(before[1] == row[1] && before[2] == row[2] && after[1] == row[1] && after[2] == row[2] &&
                           before[4] == "Blit" && after[4] == "Blit" && query[0] != ~UINT64(0) &&
                           query[1] != ~UINT64(0) && query[0] == number(before[7]) && query[1] == number(after[7]),
                       "application timestamps shifted from the kernel's surrounding query passes");
              }
            }
            times.push_back(gpu);
          }
        } else
          expect(row[6].empty(), "non-application pipeline recorded");
        for (auto &run_name : runs)
          run_counts[run_name]++;
      }
      expect(kinds["Render"] == frames && kinds["Compute"] == 2 * frames && kinds["Clear"] == frames + capacity + 1 &&
                 kinds["Blit"] == 5 * frames + 2 && kinds.size() == 4,
             "pass kinds or counts differ from recording");
      expect(unsampled > 0, "overflow did not use unsampled sentinel");
      for (auto &name : names)
        expect(run_counts[name + "*0*0*0"] == frames, "pipeline lost on list replay");
      for (UINT frame = 0; frame < frames; frame++) {
        auto &fast = kernel_times[names[2] + "*0*0*0"], &slow = kernel_times[names[3] + "*0*0*0"];
        if (expect(fast.size() == frames && slow.size() == frames, "kernel samples missing"))
          expect(slow[frame] > fast[frame] && fast[frame] > 0, "long kernel not slower in frame %u", frame);
      }
      UINT overflow_buffers = 0;
      for (auto &[key, group] : by_buffer)
        if (group.size() > capacity) {
          overflow_buffers++;
          expect(group.size() == capacity + 3, "overflow command buffer split or lost passes");
          auto &cmdbuf = buffers[key];
          const auto finish = number(cmdbuf[6]);
          const UINT64 rounding =
              UINT64(std::ceil(std::nextafter(double(finish) / 1e9, std::numeric_limits<double>::infinity()) * 1e9 -
                               finish)) +
              1;
          expect(query_start + rounding >= number(cmdbuf[5]) && query_end <= finish + rounding,
                 "application queries do not share overflow command buffer");
        }
      expect(overflow_buffers == 1, "no intact overflow command buffer");
    }
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    expect(!error, "could not remove test directory");
  }
  return verdict();
}
