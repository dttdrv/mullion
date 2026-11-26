// contract (docs/tracing.md): with MULLION_TRACE_<CATEGORY>=1, mullion.log in the folder MULLION_TRACE_DIR names has
// what is out of the ordinary in each category that is on, and nothing of the others: a queue Wait that holds the
// queue's work back (sync), but not one whose value the fence has reached; a Present 50 ms or more after the one
// before (present), on a D3D12 and on a D3D11 swap chain alike; device removal (gpu). each launch keeps the last one's
// log as mullion.prev.log, and a launch that traces nothing leaves both as they are.
// ID3D12CommandQueue::Wait: "Queues a GPU-side wait, and returns immediately."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-wait
// DXGI_PRESENT_TEST: "Do not present the frame to the output." (Microsoft Learn, DXGI_PRESENT)
#include "d3d12_test.hpp"
#include <d3d11.h>
#include <dxgi1_4.h>
#include <filesystem>
#include <fstream>
#include <iterator>

static const char *const hitch = "ms since the last";

static HWND
window(const char *name) {
  return CreateWindowA("static", name, WS_POPUP, 0, 0, 8, 8, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
}

// three Presents of the swap chain with the test flag: the second 80 ms after the first, the third right after it
static bool
presents(IDXGISwapChain1 *chain) {
  for (DWORD wait : {0, 80, 0}) {
    Sleep(wait);
    if (!expect(SUCCEEDED(chain->Present(0, DXGI_PRESENT_TEST)), "Present(TEST)"))
      return false;
  }
  return true;
}

static int
child() {
  ComPtr<ID3D12Device5> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12Fence> fence, done;
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)));
  step("a Wait the fence has reached, then one that holds the queue until the CPU signals");
  CHECK(queue->Wait(fence.Get(), 0));
  CHECK(queue->Wait(fence.Get(), 1));
  CHECK(fence->Signal(1));
  CHECK(queue->Signal(done.Get(), 1));
  HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  CHECK(done->SetEventOnCompletion(1, event));
  expect(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0, "queue finished");

  step("D3D12 Presents");
  ComPtr<IDXGIFactory4> factory;
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  DXGI_SWAP_CHAIN_DESC1 desc{8, 8, DXGI_FORMAT_R8G8B8A8_UNORM, FALSE, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT, 2,
                             DXGI_SCALING_STRETCH, DXGI_SWAP_EFFECT_FLIP_DISCARD};
  ComPtr<IDXGISwapChain1> chain;
  CHECK(factory->CreateSwapChainForHwnd(queue.Get(), window("d3d12"), &desc, nullptr, nullptr, &chain));
  if (!presents(chain.Get()))
    return verdict();

  step("D3D11 Presents");
  ComPtr<ID3D11Device> device11;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device11,
                          nullptr, nullptr));
  ComPtr<IDXGISwapChain1> chain11;
  CHECK(factory->CreateSwapChainForHwnd(device11.Get(), window("d3d11"), &desc, nullptr, nullptr, &chain11));
  if (!presents(chain11.Get()))
    return verdict();

  step("device removal");
  device->RemoveDevice();
  return verdict();
}

static std::string
contents(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  return file ? std::string(std::istreambuf_iterator<char>(file), {}) : std::string();
}

static unsigned
count(const std::string &text, const std::string &kind, const std::string &what) {
  unsigned found = 0;
  for (size_t at = 0, end; at < text.size(); at = end + 1) {
    end = text.find('\n', at);
    end = end == std::string::npos ? text.size() : end;
    auto line = text.substr(at, end - at);
    found += line.find(kind) != std::string::npos && line.find(what) != std::string::npos;
  }
  return found;
}

int
main(int argc, char **argv) {
  if (argc > 2 && !strcmp(argv[2], "child"))
    return child();
  char exe[MAX_PATH], temporary[MAX_PATH], directory[MAX_PATH];
  auto unix_name =
      (char *(__cdecl *)(const WCHAR *))GetProcAddress(GetModuleHandleA("kernel32.dll"), "wine_get_unix_file_name");
  if (!expect(unix_name && GetModuleFileNameA(nullptr, exe, sizeof(exe)) && GetTempPathA(sizeof(temporary), temporary) &&
                  temporary_directory(temporary, "trc", directory),
              "paths found"))
    return verdict();
  std::filesystem::path folder(directory);
  std::wstring wide = folder.wstring();
  char *unix_folder = unix_name(wide.c_str());
  SetEnvironmentVariableA("MULLION_TRACE_DIR", unix_folder);
  HeapFree(GetProcessHeap(), 0, unix_folder);

  auto run = [&](bool traced) {
    SetEnvironmentVariableA("MULLION_TRACE_SYNC", traced ? "1" : nullptr);
    SetEnvironmentVariableA("MULLION_TRACE_PRESENT", traced ? "1" : nullptr);
    SetEnvironmentVariableA("MULLION_TRACE_GPU", traced ? "1" : nullptr);
    std::string command = std::string("\"") + exe + "\" " + (argc > 1 ? argv[1] : "dxil") + " child";
    STARTUPINFOA startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!expect(CreateProcessA(exe, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process),
                "child started"))
      return;
    auto waited = WaitForSingleObject(process.hProcess, 60000);
    if (waited != WAIT_OBJECT_0)
      TerminateProcess(process.hProcess, 1), WaitForSingleObject(process.hProcess, 60000);
    DWORD status = 1;
    GetExitCodeProcess(process.hProcess, &status);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    expect(waited == WAIT_OBJECT_0 && status == 0, "child exit %lu, wait %lu", status, waited);
  };

  step("a launch that traces sync, present and gpu");
  run(true);
  auto first = contents(folder / "mullion.log");
  expect(first.rfind("Mullion trace ", 0) == 0, "the log starts with its header");
  // the libraries name the categories they read, in trace.cpp's order
  expect(count(first, "[trace] ", ": present, sync, gpu") >= 1, "the log does not name the categories that are on");
  expect(count(first, "[sync] ", "holds the queue's work") == 1, "%u held Waits logged, want 1",
         count(first, "[sync] ", "holds the queue's work"));
  // one hitch a swap chain: its second Present
  expect(count(first, "[present] ", hitch) == 2, "%u hitches logged, want 2", count(first, "[present] ", hitch));
  expect(count(first, "[present] ", ": Present 2,") == 2, "the hitches are not each chain's second Present");
  expect(count(first, "[gpu] ", "is removed: 0x887a0005") == 1, "device removal not logged once");
  expect(count(first, "[shader] ", "") + count(first, "[resource] ", "") + count(first, "[pass] ", "") == 0,
         "categories that are off logged");

  step("the next launch keeps the first one's log");
  run(true);
  expect(contents(folder / "mullion.prev.log") == first, "mullion.prev.log is not the first launch's log");
  auto second = contents(folder / "mullion.log");
  expect(count(second, "[sync] ", "holds the queue's work") == 1, "second launch's log incomplete");

  step("a launch that traces nothing");
  run(false);
  expect(contents(folder / "mullion.log") == second && contents(folder / "mullion.prev.log") == first,
         "a launch without tracing changed the logs");

  std::error_code error;
  std::filesystem::remove_all(folder, error);
  expect(!error, "log folder removed: %s", error.message().c_str());
  return verdict();
}
