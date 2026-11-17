// contract: a pipeline the program has released is held by nothing once the pipeline archive has added it. Metal
// adds a pipeline to an archive (MTLBinaryArchive) on the thread that asks, and what that thread autoreleases stays
// until its pool drains: a thread with no pool drains it only as it ends. so two batches of 256 pipelines of their own
// shaders are made, released, and written by the archive, and the second leaves the process's resident memory
// (WorkingSetSize, which counts mapped files) grown by less than twice the bytes the archive wrote for it more than
// the same batch made without an archive. what the archive keeps of a batch is its files, mapped once written, and
// its own copy of what they hold, each at most those bytes (11-17: 2.2 MB of shards mapped and 1.4 MB of the
// archive's own for 3.3 MB of files), where what released pipelines left behind held more (14.5 MB; 3.4 GB against
// 0.9 GB for 26,000 pipelines, 2.7 GB of archive files). the first batch also takes what the archive takes once, its
// thread and what Metal loads for it. each arm is a process of its own
// (`child <arm> <folder>`) with Metal's cache in a folder of its own, so both compile alike, and is measured with its
// device alive: the device's end ends the archive's thread, and with it that thread's pool
#include "d3d12_test.hpp"
#include "../../src/winemetal/winemetal.h"
#include <filesystem>
#include <fstream>
#include <psapi.h>

static const char hlsl[] = R"hlsl(
float4 vs(uint id : SV_VertexID) : SV_Position { return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0.5, 1); }
float4 ps() : SV_Target { return float4(K / 1024.0, 0, 0, 1); }
)hlsl";

constexpr unsigned kPipelines = 256;

static INT64
resident() {
  PROCESS_MEMORY_COUNTERS counters{sizeof(counters)};
  GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters));
  return (INT64)counters.WorkingSetSize;
}

// the bytes of the archive's files in `cache`, and whether one is being written (its ".new")
static std::pair<INT64, bool>
written(const std::filesystem::path &cache) {
  INT64 bytes = 0;
  bool writing = false;
  std::error_code error;
  for (auto &file : std::filesystem::directory_iterator(cache, error)) {
    auto name = file.path().filename().string();
    if (!name.starts_with("pipelines_"))
      continue;
    if (name.ends_with(".new"))
      writing = true;
    else
      bytes += INT64(file.file_size(error));
  }
  return {bytes, writing};
}

static int
child(Compiler &compiler, const std::string &arm, const std::filesystem::path &folder) {
  auto unix_name =
      (char *(__cdecl *)(const WCHAR *))GetProcAddress(GetModuleHandleA("kernel32"), "wine_get_unix_file_name");
  auto unix_path = [&](const std::filesystem::path &path) { return std::string(unix_name(path.wstring().c_str())); };
  auto metal = folder / ("metal-" + arm), cache = folder / ("cache-" + arm);
  std::filesystem::create_directories(metal), std::filesystem::create_directories(cache);
  auto set = (decltype(&WMTSetMetalShaderCachePath))GetProcAddress(
      GetModuleHandleA("winemetal.dll"), "WMTSetMetalShaderCachePath"
  );
  if (!set(unix_path(metal).c_str()))
    return printf("failed: the Metal cache folder was not taken\n"), 100;
  bool archive = arm == "archive";
  SetEnvironmentVariableA("MULLION_SHADER_CACHE", archive ? "1" : "0");
  SetEnvironmentVariableA("MULLION_SHADER_CACHE_PATH", unix_path(cache).c_str());
  SetEnvironmentVariableA("MULLION_D3D12_DEFER_PIPELINES", "0");
  // one more shader than measured: the first pipeline opens the archive and Metal's compiler, which stay
  std::vector<std::string> shaders;
  for (unsigned i = 0; i <= 2 * kPipelines; i++)
    shaders.push_back(compiler.compile(hlsl, "ps", "ps", {"K=" + std::to_string(i)}));
  auto vs = compiler.compile(hlsl, "vs", "vs", {"K=0"});
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  auto rs = root_signature(device.Get(), {});
  auto make = [&](const std::string &ps) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{.pRootSignature = rs.Get(), .VS = bytecode(vs), .PS = bytecode(ps)};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pipeline;
    return SUCCEEDED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)));
  };
  if (!make(shaders[2 * kPipelines]))
    return printf("failed: the first pipeline\n"), 1;
  INT64 grown[2], bytes[2];
  for (unsigned batch = 0; batch < 2; batch++) {
    INT64 before = resident();
    for (unsigned i = batch * kPipelines; i < (batch + 1) * kPipelines; i++)
      if (!make(shaders[i]))
        return printf("failed: pipeline %u\n", i), 1;
    // the archive is written once additions stop for as long as its last write took: done once its files stay as
    // they are for 3 s, none being written
    auto last = written(cache);
    for (unsigned still = 0, waited = 0; archive && still < 3000 && waited < 120000; waited += 100) {
      Sleep(100);
      auto now = written(cache);
      still = now == last && !now.second ? still + 100 : 0;
      last = now;
    }
    grown[batch] = resident() - before;
    bytes[batch] = last.first;
  }
  printf("fact %s: resident grew %lld, then %lld bytes; archive files %lld, then %lld bytes\n", arm.c_str(), grown[0],
         grown[1], bytes[0], bytes[1]);
  std::ofstream(folder / (arm + ".txt")) << grown[1] << ' ' << bytes[1] - bytes[0] << '\n';
  return 0;
}

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler))
    return printf("skipped: dxcompiler.dll not found\n"), 77;
  if (argc > 4 && !strcmp(argv[2], "child"))
    return child(compiler, argv[3], argv[4]);

  char temporary[MAX_PATH], directory[MAX_PATH], exe[MAX_PATH];
  GetModuleFileNameA(nullptr, exe, sizeof(exe));
  GetTempPathA(sizeof(temporary), temporary);
  if (!temporary_directory(temporary, "am", directory))
    return printf("failed: no temporary directory\n"), 1;
  // the processes take their own folder as Metal's cache folder, where a program's own is taken as its libraries load
  SetEnvironmentVariableA("MULLION_USE_DEFAULT_METAL_CACHE", "1");
  auto run = [&](const char *arm) {
    std::string command =
        std::string("\"") + exe + "\" " + (argc > 1 ? argv[1] : "dxil") + " child " + arm + " \"" + directory + "\"";
    STARTUPINFOA startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    DWORD status = ~0u;
    if (!CreateProcessA(exe, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process))
      return status;
    if (WaitForSingleObject(process.hProcess, 300000) != WAIT_OBJECT_0)
      TerminateProcess(process.hProcess, ~0u), WaitForSingleObject(process.hProcess, 60000);
    GetExitCodeProcess(process.hProcess, &status);
    CloseHandle(process.hThread), CloseHandle(process.hProcess);
    return status;
  };
  INT64 grown[2] = {}, bytes[2] = {};
  const char *arms[2] = {"none", "archive"};
  for (unsigned i = 0; i < 2; i++) {
    step("two batches of %u pipelines made and released, %s", kPipelines,
         i ? "through the archive" : "without an archive");
    auto status = run(arms[i]);
    std::ifstream measured(std::filesystem::path(directory) / (std::string(arms[i]) + ".txt"));
    expect(!status && measured >> grown[i] >> bytes[i], "the process measured its memory (exit %lu)", status);
  }
  printf("fact: the second batch grew with the archive %lld bytes (its files %lld), without %lld\n", grown[1],
         bytes[1], grown[0]);
  step("what the archive's process holds of the second batch beside the other");
  if (expect(bytes[1] > 0, "the archive wrote no file for the second batch"))
    expect(
        grown[1] - grown[0] < 2 * bytes[1],
        "the second batch grew the process with the archive %lld bytes, the one without %lld: %lld more, past twice "
        "the %lld bytes of its archive files",
        grown[1], grown[0], grown[1] - grown[0], bytes[1]
    );
  std::error_code error;
  std::filesystem::remove_all(directory, error);
  expect(!error, "test folder removed: %s", error.message().c_str());
  return verdict();
}
