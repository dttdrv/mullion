// contract: a program's pipeline archive holds the pipelines of the converter version that runs and of no other. no
// version finds a pipeline another one archived, and a device reads every file of the archive when it starts and
// keeps what Metal loads of it: so its files name the version that wrote them (AIRCONV_VERSION), and a device removes
// the files of other versions, those named before versions had their own among them, before it reads any. it then
// opens only its own version's files, and finds its pipeline in them. the files of the other versions here are
// copies of this version's own, which Metal would read as well as the originals.
#include "d3d12_test.hpp"
#include "../../src/airconv/airconv_public.h"
#include "../../src/winemetal/winemetal.h"
#include <algorithm>
#include <map>

static const char hlsl[] = R"hlsl(
float4 vs(uint id : SV_VertexID) : SV_Position { return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0.5, 1); }
float4 ps() : SV_Target { return 1; }
)hlsl";

// the names of the archive's files in `directory`
static std::vector<std::string>
archive_files(const std::string &directory) {
  std::vector<std::string> names;
  WIN32_FIND_DATAA found;
  auto first = FindFirstFileA((directory + "\\pipelines_*").c_str(), &found);
  for (bool more = first != INVALID_HANDLE_VALUE; more; more = FindNextFileA(first, &found))
    names.push_back(found.cFileName);
  FindClose(first);
  return names;
}

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler))
    return printf("skipped: dxcompiler.dll not found\n"), 77;
  auto vs = compiler.compile(hlsl, "vs", "vs"), ps = compiler.compile(hlsl, "ps", "ps");
  char temporary[MAX_PATH], directory[MAX_PATH];
  WCHAR wide[MAX_PATH];
  GetTempPathA(sizeof(temporary), temporary);
  auto unix_name = (char *(__cdecl *)(const WCHAR *))GetProcAddress(GetModuleHandleA("kernel32"), "wine_get_unix_file_name");
  if (!temporary_directory(temporary, "av", directory) || !MultiByteToWideChar(CP_ACP, 0, directory, -1, wide, MAX_PATH))
    return printf("failed: no temporary directory\n"), 1;
  SetEnvironmentVariableA("MULLION_SHADER_CACHE", "1");
  SetEnvironmentVariableA("MULLION_SHADER_CACHE_PATH", unix_name(wide));
  // the calls counted are a creation's own: its Metal pipeline is made at the create call here
  SetEnvironmentVariableA("MULLION_D3D12_DEFER_PIPELINES", "0");
  auto winemetal = LoadLibraryA("winemetal.dll");
  auto time_calls = (decltype(&WMTDiagTimeCalls))GetProcAddress(winemetal, "WMTDiagTimeCalls");
  auto totals = (decltype(&WMTDiagCallTotals))GetProcAddress(winemetal, "WMTDiagCallTotals");
  CHECK(time_calls && totals ? S_OK : E_FAIL);
  time_calls();

  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12RootSignature> rs;
  ComPtr<ID3D12PipelineState> pipeline;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{.VS = bytecode(vs), .PS = bytecode(ps)};
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = UINT_MAX;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  // the calls a device and its first pipeline make, by name, and those of the pipeline's creation alone: the device
  // may open the archive for pipelines of its own
  struct Calls {
    std::map<std::string, uint64_t> device, pipeline;
  };
  auto first_pipeline = [&] {
    static WMTDiagCall calls[WMTUnixCallCount];
    Calls named;
    auto add = [&](std::map<std::string, uint64_t> &to) {
      totals(calls);
      for (auto &[count, ticks, name] : calls)
        if (name)
          to[name] += count;
    };
    totals(calls);
    if (!expect(SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))), "device created"))
      return named;
    desc.pRootSignature = (rs = root_signature(device.Get(), {})).Get();
    add(named.device);
    expect(SUCCEEDED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline))), "pipeline created");
    add(named.pipeline);
    for (auto &[name, count] : named.pipeline)
      named.device[name] += count;
    // the device's end writes its archive
    pipeline.Reset(), rs.Reset(), device.Reset();
    return named;
  };

  step("a device's archive");
  first_pipeline();
  auto own = archive_files(directory);
  if (!expect(!own.empty(), "no archive file written"))
    return verdict();
  // what the files of other versions are called, after those of this one
  std::vector<std::string> others;
  auto named = [](unsigned metal, const char *version, unsigned shard) {
    char name[64];
    snprintf(name, sizeof(name), "pipelines_%u_%s%u.metallib", metal, version, shard);
    return std::string(name);
  };
  for (auto &name : own) {
    unsigned metal, version, shard;
    char end;
    if (!expect(sscanf(name.c_str(), "pipelines_%u_v%u_%u.metalli%c", &metal, &version, &shard, &end) == 4 && end == 'b',
                "%s names no converter version", name.c_str()) ||
        !expect(version == AIRCONV_VERSION, "%s is of version %u, want %u", name.c_str(), version, AIRCONV_VERSION))
      continue;
    for (auto other : {named(metal, "", shard), named(metal, ("v" + std::to_string(version + 1) + "_").c_str(), shard),
                       named(metal, ("v" + std::to_string(version - 1) + "_").c_str(), shard)})
      if (CopyFileA((std::string(directory) + "\\" + name).c_str(), (std::string(directory) + "\\" + other).c_str(), TRUE))
        others.push_back(other);
  }
  if (!expect(!others.empty(), "no file of another version made"))
    return verdict();

  step("a device after files of other versions were written");
  auto calls = first_pipeline();
  // one archive to add to, this version's files, and the read past them that finds none
  expect(calls.device["MTLDevice_newBinaryArchive"] == own.size() + 2, "%llu archives opened, want %zu",
         (unsigned long long)calls.device["MTLDevice_newBinaryArchive"], own.size() + 2);
  expect(calls.pipeline["MTLDevice_newRenderPipelineState"] == 1, "%llu Metal pipeline creations, want the archive's one",
         (unsigned long long)calls.pipeline["MTLDevice_newRenderPipelineState"]);
  auto now = archive_files(directory);
  for (auto &other : others)
    expect(std::find(now.begin(), now.end(), other) == now.end(), "%s is still there", other.c_str());
  for (auto &name : own)
    expect(std::find(now.begin(), now.end(), name) != now.end(), "this version's %s was removed", name.c_str());
  return verdict();
}
