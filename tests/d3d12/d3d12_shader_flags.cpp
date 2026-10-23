// contract: MULLION_SHADER_FLAGS accepts a complete unsigned flag value; an invalid override is diagnosed and
// leaves device corrections in place. a pipeline still returns and writes its value: "Random-access write of
// 1-4 32bit components into typeless memory." (D3D11.3 22.4.11 store_raw).
// the input cases distinguish no digits, trailing text, negative values, uint32 overflow,
// strtoul overflow, and valid decimal, hexadecimal and octal values, including zero.
#include "d3d12_test.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>

static const struct {
  const char *text;
  bool valid;
} inputs[] = {{"", true},
              {"0", true},
              {"1", true},
              {"2", true},
              {"4", true},
              {"7", true},
              {"8", true},
              {"0x8", true},
              {"15", true},
              {"0x4", true},
              {"0x20", true},
              {"04", true},
              {"+4", true},
              {" 4", true},
              {"garbage", false},
              {"4garbage", false},
              {"-1", false},
              {" -0", false},
              {"4294967296", false},
              {"0x100000000", false},
              {"18446744073709551616", false},
              {" ", false},
              {"0x", false},
              {"08", false},
              {"16", false},
              {"0x34", false}};
static const char hlsl[] = R"hlsl(
RWByteAddressBuffer output : register(u0);
[numthreads(1, 1, 1)] void cs() { output.Store(0, VALUE); }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: DXC not found\n");
    return 77;
  }
  std::string shaders[std::size(inputs)];
  for (UINT i = 0; i < std::size(inputs); i++) {
    shaders[i] = compiler.compile(hlsl, "cs", "cs", {"VALUE=" + std::to_string(i + 1)});
    if (!expect(!shaders[i].empty(), "shader %u compiled", i))
      return verdict();
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  if (argc < 3) {
    char executable[MAX_PATH], temporary[MAX_PATH], directory[MAX_PATH];
    modulePath(executable, sizeof(executable));
    GetTempPathA(sizeof(temporary), temporary);
    if (!expect(GetTempFileNameA(temporary, "flg", 0, directory), "temporary log name"))
      return verdict();
    DeleteFileA(directory);
    if (!expect(CreateDirectoryA(directory, nullptr), "log directory created"))
      return verdict();
    SetEnvironmentVariableA("MULLION_LOG_PATH", directory);
    SetEnvironmentVariableA("MULLION_LOG_LEVEL", "info");
    std::string command = std::string("\"") + executable + "\" " + (argc > 1 ? argv[1] : "dxil") + " child";
    STARTUPINFOA startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!expect(CreateProcessA(executable, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                               &process),
                "child started"))
      return verdict();
    const auto wait = WaitForSingleObject(process.hProcess, 120000);
    if (wait != WAIT_OBJECT_0) {
      TerminateProcess(process.hProcess, 1);
      WaitForSingleObject(process.hProcess, 60000);
    }
    DWORD status = 1;
    GetExitCodeProcess(process.hProcess, &status);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    expect(wait == WAIT_OBJECT_0 && (status == 0 || status == 77), "child exit %lu, wait %lu", status, wait);
    std::string log;
    for (const auto &entry : std::filesystem::directory_iterator(directory)) {
      std::ifstream file(entry.path());
      log.append(std::istreambuf_iterator<char>(file), {});
    }
    if (status != 77)
      for (auto input : inputs) {
        step("MULLION_SHADER_FLAGS='%s'", input.text);
        const std::string message = std::string("Invalid MULLION_SHADER_FLAGS '") + input.text + "'";
        expect((log.find(message) == std::string::npos) == input.valid, "override diagnostic disagrees with validity");
      }
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    expect(!error, "temporary logs removed: %s", error.message().c_str());
    return status == 77 && !trace::wrong ? 77 : verdict();
  }
  D3D12_ROOT_PARAMETER parameter{D3D12_ROOT_PARAMETER_TYPE_UAV};
  auto root = root_signature(device.Get(), {1, &parameter});
  auto output = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, sizeof(UINT), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, sizeof(UINT), D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(root && output && readback, "resources created"))
    return verdict();
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  for (UINT i = 0; i < std::size(inputs); i++) {
    step("MULLION_SHADER_FLAGS='%s'", inputs[i].text);
    SetEnvironmentVariableA("MULLION_SHADER_FLAGS", inputs[i].text);
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{root.Get(), bytecode(shaders[i])};
    ComPtr<ID3D12PipelineState> pipeline;
    CHECK(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline)));
    CHECK(forget(readback.Get()));
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), pipeline.Get()));
    list->SetComputeRootSignature(root.Get());
    list->SetComputeRootUnorderedAccessView(0, output->GetGPUVirtualAddress());
    list->Dispatch(1, 1, 1);
    transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, sizeof(UINT));
    transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    UINT *data;
    CHECK(readback->Map(0, nullptr, reinterpret_cast<void **>(&data)));
    expect(*data == i + 1, "pipeline wrote %u, want %u", *data, i + 1);
    readback->Unmap(0, nullptr);
  }
  return verdict();
}
