// contract: with the GPU error diagnostic set, each successful pipeline creation or deferred variant that ends
// after the device's first non-test Present reaches a queue reports its shader name, kind and elapsed milliseconds
// immediately; each queue release reports the device's count, sum and maximum. no Present means zero; unset means
// no reports.
// "The only place in the Pipeline where adjacency information is visible to the application is in the Geometry
// Shader." (D3D11.3 functional specification, 8.15). without a geometry shader, adjacent vertices do not form the
// rasterized triangle. the even vertices below cover the target; the odd ones lie outside it. this exercises
// Mullion's first-draw adjacency variants, twice each to distinguish compilation from reuse.
// "3 vertices for the triangle, and 3 for the adjacency." (8.15): each adjacency draw has twice a triangle's vertices.
// DXGI_PRESENT_TEST: "Do not present the frame to the output." (Microsoft Learn, DXGI_PRESENT).
// https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-present
#include "d3d12_test.hpp"
#include <dxgi1_4.h>
#include <wincrypt.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <set>
#include <sstream>
#include <thread>

static const char hlsl[] = R"hlsl(
float4 position(uint id) { return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0, 1); }
float4 vs(uint id : SV_VertexID) : SV_Position { return position(id); }
float4 adjacency_vs(uint id : SV_VertexID) : SV_Position {
  return id & 1 ? float4(10, 10, 0, 1) : position(id / 2);
}
uint ps() : SV_Target { return VALUE; }
RWStructuredBuffer<uint> stored : register(u0);
[numthreads(1, 1, 1)] void cs() { stored[0] = VALUE; }
)hlsl";

static const char ray_hlsl[] = R"hlsl(
RWStructuredBuffer<uint> stored : register(u0);
[shader("raygeneration")] void raygen() { stored[0] = VALUE; }
)hlsl";

static const std::string made_prefix = "D3D12 pipeline made: ", totals_prefix = "D3D12 pipeline totals: ";

static std::string
logs(const std::filesystem::path &directory) {
  std::string text;
  std::error_code error;
  for (std::filesystem::directory_iterator entry(directory, error), end; !error && entry != end; entry.increment(error))
    if (entry->path().extension() == ".log") {
      std::ifstream file(entry->path());
      expect(bool(file), "log opened: %s", entry->path().string().c_str());
      text.append(std::istreambuf_iterator<char>(file), {});
    }
  expect(!error, "log directory read: %s", error.message().c_str());
  return text;
}

static std::string
shader_name(const std::string &code) {
  auto dll = LoadLibraryA("crypt32.dll");
  auto hash = dll ? reinterpret_cast<decltype(&CryptHashCertificate)>(GetProcAddress(dll, "CryptHashCertificate"))
                  : nullptr;
  DWORD size = 0;
  std::string name;
  if (expect(hash && hash(0, CALG_SHA1, 0, reinterpret_cast<const BYTE *>(code.data()), code.size(), nullptr, &size),
          "SHA-1 size queried")) {
    std::vector<BYTE> digest(size);
    if (expect(hash(0, CALG_SHA1, 0, reinterpret_cast<const BYTE *>(code.data()), code.size(), digest.data(), &size),
            "shader SHA-1 computed")) {
      std::ostringstream out;
      for (auto byte : digest)
        out << std::hex << std::setfill('0') << std::setw(2) << unsigned(byte);
      name = out.str() + " ";
    }
  }
  if (dll)
    FreeLibrary(dll);
  return name;
}

static int
child(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler))
    return 77;
  const D3D12_PRIMITIVE_TOPOLOGY topologies[] = {
      D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ
  };
  const UINT workers = 2, variants = workers + 1, size = workers + std::size(topologies), triangle_vertices = 3;
  auto vs = compiler.compile(hlsl, "vs", "vs", {"VALUE=1"});
  auto adjacency_vs = compiler.compile(hlsl, "adjacency_vs", "vs", {"VALUE=1"});
  std::string ps[variants], cs[variants], libraries[variants];
  for (UINT i = 0; i < variants; i++) {
    auto defines = std::vector<std::string>{"VALUE=" + std::to_string(i + 1)};
    ps[i] = compiler.compile(hlsl, "ps", "ps", defines);
    cs[i] = compiler.compile(hlsl, "cs", "cs", defines);
    if (compiler.dxc)
      libraries[i] = compiler.compile(ray_hlsl, "", "lib_6_3", defines);
    if (vs.empty() || adjacency_vs.empty() || ps[i].empty() || cs[i].empty() ||
        (compiler.dxc && libraries[i].empty())) {
      printf("failed: HLSL did not compile\n");
      return 1;
    }
  }
  char directory[MAX_PATH], setting[MAX_PATH];
  GetEnvironmentVariableA("DXMT_LOG_PATH", directory, sizeof(directory));
  const bool enabled = GetEnvironmentVariableA("DXMT_D3D12_GPU_ERRORS", setting, sizeof(setting)) != 0;
  const bool presents = strcmp(argv[3], "never") != 0;
  std::multiset<std::string> expected;
  auto checkpoint = [&] {
    auto text = logs(directory);
    size_t count = 0;
    for (size_t at = 0; (at = text.find(made_prefix, at)) != std::string::npos; at += made_prefix.size())
      count++;
    expect(count == (enabled ? expected.size() : 0), "%zu immediate reports, want %zu", count,
        enabled ? expected.size() : 0);
  };
  ComPtr<ID3D12Device5> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER parameter{D3D12_ROOT_PARAMETER_TYPE_UAV};
  auto rs = root_signature(device.Get(), {1, &parameter});
  if (!expect(bool(rs), "root signature created"))
    return verdict();
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  ComPtr<ID3D12CommandQueue> queues[2];
  ComPtr<IDXGISwapChain1> chains[2];
  HWND windows[2];
  ComPtr<IDXGIFactory4> factory;
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  DXGI_SWAP_CHAIN_DESC1 chain_desc{size, size, DXGI_FORMAT_R8G8B8A8_UNORM, FALSE, {1, 0},
      DXGI_USAGE_RENDER_TARGET_OUTPUT, 2};
  chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  for (UINT i = 0; i < std::size(queues); i++) {
    CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queues[i])));
    windows[i] = CreateWindowA("STATIC", "pipeline times", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 64, 64,
        nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
    if (!expect(windows[i] != nullptr, "window %u created", i))
      return verdict();
    CHECK(factory->CreateSwapChainForHwnd(queues[i].Get(), windows[i], &chain_desc, nullptr, nullptr, &chains[i]));
  }
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(adjacency_vs), bytecode(ps[0])};
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> adjacency, graphics[workers], compute[variants];
  step("pipelines before any Present");
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&adjacency)));
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc{rs.Get(), bytecode(cs[0])};
  CHECK(device->CreateComputePipelineState(&compute_desc, IID_PPV_ARGS(&compute[0])));
  D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options)));
  const bool rays = compiler.dxc && options.RaytracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
  ComPtr<ID3D12StateObject> states[variants];
  auto state = [&](UINT i, D3D12_STATE_OBJECT_TYPE type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE,
                   ID3D12StateObject *collection = nullptr, ID3D12StateObject *parent = nullptr) {
    D3D12_EXPORT_DESC renamed{L"added_raygen", L"raygen"};
    D3D12_DXIL_LIBRARY_DESC library{bytecode(libraries[i]), parent ? 1u : 0u, parent ? &renamed : nullptr};
    D3D12_EXISTING_COLLECTION_DESC existing{collection};
    D3D12_GLOBAL_ROOT_SIGNATURE global{rs.Get()};
    D3D12_RAYTRACING_SHADER_CONFIG shader_config{};
    D3D12_RAYTRACING_PIPELINE_CONFIG pipeline_config{1};
    D3D12_STATE_OBJECT_CONFIG additions{D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS};
    const D3D12_STATE_SUBOBJECT subobjects[] = {
        {D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &additions},
        collection ? D3D12_STATE_SUBOBJECT{D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &existing}
                   : D3D12_STATE_SUBOBJECT{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library},
        {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global},
        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader_config},
        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipeline_config}
    };
    D3D12_STATE_OBJECT_DESC state_desc{type, UINT(std::size(subobjects)), subobjects};
    if (parent) {
      ComPtr<ID3D12Device7> device7;
      HRESULT hr = device.As(&device7);
      return FAILED(hr) ? hr : device7->AddToStateObject(&state_desc, parent, IID_PPV_ARGS(&states[i]));
    }
    return device->CreateStateObject(&state_desc, IID_PPV_ARGS(&states[i]));
  };
  if (rays) {
    CHECK(state(0));
  } else {
    printf("not checked: ray timing needs DXIL and ray tracing support\n");
  }
  checkpoint();
  step("test Presents leave the device before its first Present");
  for (auto &chain : chains)
    CHECK(chain->Present(0, DXGI_PRESENT_TEST));
  ComPtr<ID3D12PipelineState> tested;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&tested)));
  checkpoint();
  const UINT first = !strcmp(argv[3], "second") ? 1 : 0;
  if (presents)
    CHECK(chains[first]->Present(0, 0));

  step("two threads create graphics and compute pipelines after the boundary");
  HRESULT results[workers][2]{};
  std::thread threads[workers];
  for (UINT t = 0; t < workers; t++)
    threads[t] = std::thread([&, t] {
      auto graphics_desc = desc;
      graphics_desc.VS = bytecode(vs);
      graphics_desc.PS = bytecode(ps[t + 1]);
      D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc{rs.Get(), bytecode(cs[t + 1])};
      results[t][0] = device->CreateGraphicsPipelineState(&graphics_desc, IID_PPV_ARGS(&graphics[t]));
      results[t][1] = device->CreateComputePipelineState(&compute_desc, IID_PPV_ARGS(&compute[t + 1]));
    });
  for (UINT t = 0; t < workers; t++) {
    threads[t].join();
    expect(SUCCEEDED(results[t][0]) && SUCCEEDED(results[t][1]), "thread %u pipeline creations: %08lx, %08lx", t,
        results[t][0], results[t][1]);
    if (presents) {
      expected.insert("graphics; " + shader_name(vs) + shader_name(ps[t + 1]));
      expected.insert("compute; " + shader_name(cs[t + 1]));
    }
  }
  if (trace::wrong)
    return verdict();
  checkpoint();
  if (presents)
    CHECK(chains[1 - first]->Present(0, 0));
  auto invalid = desc;
  invalid.VS = {};
  expect(device->CreateGraphicsPipelineState(&invalid, IID_PPV_ARGS(&tested)) == E_INVALIDARG,
      "a failed creation returns E_INVALIDARG");
  checkpoint();

  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList4> list;
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1, desc.RTVFormats[0],
      {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
      nullptr, IID_PPV_ARGS(&target)));
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 bytes;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes + variants * sizeof(UINT),
      D3D12_RESOURCE_STATE_COPY_DEST);
  auto stored = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, variants * sizeof(UINT),
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  if (!expect(readback && stored, "readback and output buffers created"))
    return verdict();
  D3D12_VIEWPORT viewport{0, 0, float(size), float(size), 0, 1};
  list->SetGraphicsRootSignature(rs.Get());
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  list->RSSetViewports(1, &viewport);
  step("a pre-Present state's adjacency variants compile at first draw, then are reused");
  UINT column = 0;
  for (auto topology : topologies) {
    D3D12_RECT stripe{LONG(column), 0, LONG(column + 1), LONG(size)};
    list->RSSetScissorRects(1, &stripe);
    column++;
    list->SetPipelineState(adjacency.Get());
    list->IASetPrimitiveTopology(topology);
    for (UINT reuse = 0; reuse < 2; reuse++) {
      list->DrawInstanced(2 * triangle_vertices, 1, 0, 0);
      if (!reuse && presents)
        expected.insert("graphics; " + shader_name(adjacency_vs) + shader_name(ps[0]));
      checkpoint();
    }
  }
  for (UINT i = 0; i < variants; i++) {
    if (i) {
      D3D12_RECT stripe{LONG(column), 0, LONG(column + 1), LONG(size)};
      list->RSSetScissorRects(1, &stripe);
      column++;
      list->SetPipelineState(graphics[i - 1].Get());
      list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      list->DrawInstanced(triangle_vertices, 1, 0, 0);
    }
    list->SetPipelineState(compute[i].Get());
    list->SetComputeRootSignature(rs.Get());
    list->SetComputeRootUnorderedAccessView(0, stored->GetGPUVirtualAddress() + i * sizeof(UINT));
    list->Dispatch(1, 1, 1);
  }
  transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(list.Get(), stored.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION from{target.Get()}, to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  to.PlacedFootprint = footprint;
  list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  list->CopyBufferRegion(readback.Get(), bytes, stored.Get(), 0, variants * sizeof(UINT));
  CHECK(submit(device.Get(), queues[1 - first].Get(), list.Get()));
  const char *data;
  CHECK(readback->Map(0, nullptr, (void **)&data));
  for (UINT y = 0; y < size; y++)
    for (UINT x = 0; x < size; x++)
      expect(reinterpret_cast<const UINT *>(data + footprint.Offset + y * footprint.Footprint.RowPitch)[x] ==
              (x < std::size(topologies) ? 1 : x - std::size(topologies) + 2),
          "pixel %u,%u differs", x, y);
  for (UINT i = 0; i < variants; i++)
    expect(reinterpret_cast<const UINT *>(data + bytes)[i] == i + 1, "compute output %u differs", i);
  readback->Unmap(0, nullptr);

  if (rays) {
    step("ray functions compile at state creation, stack variants at first dispatch, and reuse does not count");
    for (UINT i = 0; i < variants; i++) {
      auto name = shader_name(libraries[i]);
      if (i) {
        if (i == 1) {
          CHECK(state(i, D3D12_STATE_OBJECT_TYPE_COLLECTION));
          checkpoint();
          auto collection = states[i];
          CHECK(state(i, D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, collection.Get()));
        } else {
          CHECK(state(i, D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, nullptr, states[i - 1].Get()));
          name = shader_name(libraries[i - 1]) + name;
        }
        if (presents)
          expected.insert("ray; " + name);
        checkpoint();
      }
      ComPtr<ID3D12StateObjectProperties> properties;
      CHECK(states[i].As(&properties));
      auto table = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT,
          D3D12_RESOURCE_STATE_GENERIC_READ);
      void *mapped;
      if (!expect(bool(table), "ray table created"))
        return verdict();
      CHECK(table->Map(0, nullptr, &mapped));
      auto identifier = properties->GetShaderIdentifier(i == variants - 1 ? L"added_raygen" : L"raygen");
      if (!expect(identifier != nullptr, "raygen identifier found"))
        return verdict();
      memcpy(mapped, identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
      table->Unmap(0, nullptr);
      D3D12_DISPATCH_RAYS_DESC dispatch{};
      dispatch.RayGenerationShaderRecord = {table->GetGPUVirtualAddress(), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES};
      dispatch.Width = dispatch.Height = dispatch.Depth = 1;
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), nullptr));
      transition(list.Get(), stored.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      list->SetComputeRootSignature(rs.Get());
      list->SetComputeRootUnorderedAccessView(0, stored->GetGPUVirtualAddress());
      list->SetPipelineState1(states[i].Get());
      for (UINT variant = 0; variant < 2; variant++) {
        properties->SetPipelineStackSize(properties->GetPipelineStackSize() + variant);
        for (UINT reuse = 0; reuse < 2; reuse++) {
          list->DispatchRays(&dispatch);
          if (!reuse && presents)
            expected.insert("ray; " + name);
          checkpoint();
          D3D12_RESOURCE_BARRIER barrier{D3D12_RESOURCE_BARRIER_TYPE_UAV};
          barrier.UAV.pResource = stored.Get();
          list->ResourceBarrier(1, &barrier);
        }
      }
      transition(list.Get(), stored.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyBufferRegion(readback.Get(), bytes, stored.Get(), 0, sizeof(UINT));
      CHECK(submit(device.Get(), queues[1 - first].Get(), list.Get()));
      CHECK(readback->Map(0, nullptr, (void **)&data));
      expect(*reinterpret_cast<const UINT *>(data + bytes) == i + 1, "ray output %u differs", i);
      readback->Unmap(0, nullptr);
    }
  }
  std::ofstream manifest(std::filesystem::path(directory) / "expected.txt");
  for (auto &name : expected)
    manifest << name << '\n';
  manifest.close();
  for (auto &chain : chains)
    chain.Reset();
  for (auto &queue : queues)
    queue.Reset();
  for (auto window : windows)
    DestroyWindow(window);
  return verdict();
}

int
main(int argc, char **argv) {
  if (argc > 3 && !strcmp(argv[2], "child"))
    return child(argc, argv);
  char exe[MAX_PATH], temporary[MAX_PATH], directory[MAX_PATH];
  if (!expect(
          GetModuleFileNameA(nullptr, exe, sizeof(exe)) && GetTempPathA(sizeof(temporary), temporary), "paths found"
      ))
    return verdict();
  for (const char *mode : {"never", "first", "second"})
    for (const char *setting : {static_cast<const char *>(nullptr), "0", "2"}) {
      step("%s child, DXMT_D3D12_GPU_ERRORS=%s", mode, setting ? setting : "unset");
      if (!expect(GetTempFileNameA(temporary, "pso", 0, directory), "temporary name found"))
        return verdict();
      DeleteFileA(directory);
      if (!expect(CreateDirectoryA(directory, nullptr), "log directory created"))
        return verdict();
      SetEnvironmentVariableA("DXMT_LOG_PATH", directory);
      SetEnvironmentVariableA("DXMT_LOG_LEVEL", "info");
      SetEnvironmentVariableA("DXMT_D3D12_GPU_ERRORS", setting);
      SetEnvironmentVariableA("DXMT_D3D12_ISOLATE", nullptr);
      std::string command = std::string("\"") + exe + "\" " + (argc > 1 ? argv[1] : "dxil") + " child " + mode;
      STARTUPINFOA startup{sizeof(startup)};
      PROCESS_INFORMATION process{};
      if (!expect(CreateProcessA(exe, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process),
              "child started"))
        return verdict();
      auto waited = WaitForSingleObject(process.hProcess, 60000);
      if (waited != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 60000);
      }
      DWORD status = 1;
      GetExitCodeProcess(process.hProcess, &status);
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
      std::error_code error;
      if (status == 77) {
        std::filesystem::remove_all(directory, error);
        if (!expect(!error, "log directory removed: %s", error.message().c_str()))
          return verdict();
        return 77;
      }
      expect(waited == WAIT_OBJECT_0 && status == 0, "child exit %lu, wait %lu", status, waited);
      std::multiset<std::string> expected, reported;
      std::ifstream manifest(std::filesystem::path(directory) / "expected.txt");
      expect(bool(manifest), "child wrote expected shader names");
      std::string line;
      while (std::getline(manifest, line))
        if (setting)
          expected.insert(line);
      manifest.close();
      auto text = logs(directory);
      std::istringstream lines(text);
      double sum = 0, longest = 0;
      UINT totals = 0;
      while (std::getline(lines, line)) {
        if (auto at = line.find(made_prefix); at != std::string::npos) {
          auto end = line.rfind("; ");
          std::istringstream fields(line.substr(end + 2));
          double ms = -1;
          std::string unit, extra;
          fields >> ms >> unit;
          expect(fields && std::isfinite(ms) && ms >= 0 && unit == "ms" && !(fields >> extra),
              "bad pipeline time: %s", line.c_str());
          reported.insert(line.substr(at + made_prefix.size(), end - at - made_prefix.size()));
          sum += ms;
          longest = std::max(longest, ms);
        }
        if (auto at = line.find(totals_prefix); at != std::string::npos) {
          std::istringstream fields(line.substr(at + totals_prefix.size()));
          UINT64 count = 0;
          double total = -1, most = -1;
          std::string after, first, present, ms, total_word, max_ms, longest_word, extra;
          fields >> count >> after >> first >> present >> total >> ms >> total_word >> most >> max_ms >> longest_word;
          expect(fields && after == "after" && first == "first" && present == "Present;" && ms == "ms" &&
                  total_word == "total;" && max_ms == "ms" && longest_word == "longest" && !(fields >> extra) &&
                  count == expected.size() && count == reported.size() && total == sum && most == longest,
              "bad device totals: %s", line.c_str());
          totals++;
        }
      }
      expect(reported == expected, "reported shader names and kinds differ: %zu reports, want %zu", reported.size(),
          expected.size());
      expect(totals == (setting ? 2u : 0u), "%u totals lines, want %u", totals, setting ? 2 : 0);
      std::filesystem::remove_all(directory, error);
      expect(!error, "log directory removed: %s", error.message().c_str());
    }
  return verdict();
}
