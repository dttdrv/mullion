// contract: the working folders Metal keeps for a program's pipeline archives do not stay after its runs. Metal makes
// one ("gpuarchiver-" and six digits) in its cache folder for every archive a pipeline is added to and never removes
// it; Mullion removes those that are there when a process starts and no other process of that cache folder lives
// (WMTSetMetalShaderCachePath). every step is a process of its own (`child <step> <folder>`):
// - `make` creates a pipeline through the archive and ends: its folder is left;
// - `start` starts after it and does nothing else: the folders that were there go;
// - `hold` creates a pipeline and lives on, and a process that starts `beside` it leaves its folder alone.
#include "d3d12_test.hpp"
#include "../../src/winemetal/winemetal.h"

static const char hlsl[] = R"hlsl(
float4 vs(uint id : SV_VertexID) : SV_Position { return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0.5, 1); }
float4 ps() : SV_Target { return 1; }
)hlsl";

static unsigned
folders(const std::string &metal) {
  WIN32_FIND_DATAA found;
  auto first = FindFirstFileA((metal + "\\gpuarchiver-*").c_str(), &found);
  unsigned count = first != INVALID_HANDLE_VALUE;
  while (first != INVALID_HANDLE_VALUE && FindNextFileA(first, &found))
    count++;
  FindClose(first);
  return count;
}

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler))
    return printf("skipped: dxcompiler.dll not found\n"), 77;
  auto unix_name = (char *(__cdecl *)(const WCHAR *))GetProcAddress(GetModuleHandleA("kernel32"), "wine_get_unix_file_name");
  auto unix_path = [&](const std::string &path) {
    WCHAR wide[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, wide, MAX_PATH);
    return std::string(unix_name(wide));
  };
  char temporary[MAX_PATH], directory[MAX_PATH], exe[MAX_PATH];
  GetModuleFileNameA(nullptr, exe, sizeof(exe));

  if (argc > 4 && !strcmp(argv[2], "child")) {
    std::string step = argv[3], metal = std::string(argv[4]) + "\\metal";
    unsigned before = folders(metal);
    auto set = (decltype(&WMTSetMetalShaderCachePath))GetProcAddress(GetModuleHandleA("winemetal.dll"), "WMTSetMetalShaderCachePath");
    if (!set(unix_path(metal).c_str()))
      return printf("failed: the Metal cache folder was not taken\n"), 100;
    // the folders that were there are removed off the starting thread: `start` waits for them to go, `beside` for
    // long enough that they would have
    if (step == "start" || step == "beside") {
      for (unsigned waited = 0; before && folders(metal) && waited < (step == "start" ? 10000 : 1000); waited += 50)
        Sleep(50);
      return folders(metal);
    }
    SetEnvironmentVariableA("MULLION_SHADER_CACHE", "1");
    SetEnvironmentVariableA("MULLION_SHADER_CACHE_PATH", unix_path(std::string(argv[4]) + "\\cache").c_str());
    SetEnvironmentVariableA("MULLION_D3D12_DEFER_PIPELINES", "0");
    ComPtr<ID3D12Device> device;
    CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    auto rs = root_signature(device.Get(), {});
    auto vs = compiler.compile(hlsl, "vs", "vs"), ps = compiler.compile(hlsl, "ps", "ps");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{.pRootSignature = rs.Get(), .VS = bytecode(vs), .PS = bytecode(ps)};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    // a target format of the step's own: a pipeline no archive file of the folder has
    desc.RTVFormats[0] = step == "hold" ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pipeline;
    CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)));
    if (step == "hold") {
      SetEvent(OpenEventA(EVENT_MODIFY_STATE, FALSE, "mullion_archive_folders_made"));
      WaitForSingleObject(OpenEventA(SYNCHRONIZE, FALSE, "mullion_archive_folders_done"), 60000);
    }
    return 0;
  }

  GetTempPathA(sizeof(temporary), temporary);
  if (!temporary_directory(temporary, "af", directory))
    return printf("failed: no temporary directory\n"), 1;
  // the processes take the test's folder as Metal's cache folder themselves, where a program's own is taken as its
  // libraries load
  SetEnvironmentVariableA("MULLION_USE_DEFAULT_METAL_CACHE", "1");
  std::string metal = std::string(directory) + "\\metal";
  auto start = [&](const char *step, PROCESS_INFORMATION &process) {
    std::string command = std::string("\"") + exe + "\" " + (argc > 1 ? argv[1] : "dxil") + " child " + step + " \"" + directory + "\"";
    STARTUPINFOA startup{sizeof(startup)};
    return CreateProcessA(exe, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process);
  };
  auto ended = [&](PROCESS_INFORMATION &process) -> DWORD {
    DWORD status = ~0u;
    if (WaitForSingleObject(process.hProcess, 120000) != WAIT_OBJECT_0)
      TerminateProcess(process.hProcess, ~0u), WaitForSingleObject(process.hProcess, 60000);
    GetExitCodeProcess(process.hProcess, &status);
    CloseHandle(process.hThread), CloseHandle(process.hProcess);
    return status;
  };
  auto run = [&](const char *step) -> DWORD {
    PROCESS_INFORMATION process{};
    return start(step, process) ? ended(process) : ~0u;
  };

  step("a process that made a pipeline through the archive, after it ended");
  if (!expect(!run("make"), "the process made its pipeline"))
    return verdict();
  // without a folder left there is nothing to remove, and nothing to show that it is
  if (!folders(metal))
    return printf("skipped: Metal left no working folder\n"), 77;

  step("a process that starts after it");
  auto left = run("start");
  expect(!left, "%lu working folders left", left);

  step("a process that starts while another lives");
  auto made = CreateEventA(nullptr, TRUE, FALSE, "mullion_archive_folders_made"),
       done = CreateEventA(nullptr, TRUE, FALSE, "mullion_archive_folders_done");
  PROCESS_INFORMATION holder{};
  if (expect(start("hold", holder), "the living process started")) {
    expect(WaitForSingleObject(made, 120000) == WAIT_OBJECT_0, "the living process made its pipeline");
    // Metal makes the folder as the archive is written, on a thread of its own
    unsigned its = 0;
    for (unsigned waited = 0; !(its = folders(metal)) && waited < 10000; waited += 50)
      Sleep(50);
    expect(its > 0, "the living process has a working folder");
    left = run("beside");
    expect(left == its, "%lu working folders beside the living process, %u before", left, its);
    SetEvent(done);
    expect(!ended(holder), "the living process ended");
  }
  return verdict();
}
