// contract: a call that fails says why in the log MULLION_LOG_PATH names ("Writes a log file for each DLL", README),
// as the debug layer of Direct3D 12 does on Windows: issue #32's start-up failed with E_INVALIDARG and logged nothing.
// the start-up is Starfield's on an M1 Max (#32): a factory, the first adapter, devices probed from 12_2 down,
// the DRED interfaces asked for, a direct queue, a swap chain for a window. then a refused call of each DLL:
// "E_INVALIDARG ... if a size mismatch is detected for the FeatureSupportDataSize parameter" (Microsoft Learn,
// ID3D12Device::CheckFeatureSupport), and a swap chain with no OutputWindow is DXGI_ERROR_INVALID_CALL (Wine's
// dlls/dxgi/tests/dxgi.c, test_create_swapchain). each must leave a line with the function that refused and its code
// in that DLL's log, which is written to a path spelt as the issue's is, with backslashes.
#include "d3d12_test.hpp"
#include <dxgi1_6.h>
#include <fstream>
#include <iterator>

int
main() {
  char temporary[MAX_PATH], directory[MAX_PATH], exe[MAX_PATH];
  auto length = GetTempPathA(sizeof(temporary), temporary);
  CHECK(length && length < sizeof(temporary) ? S_OK : E_FAIL);
  CHECK(temporary_directory(temporary, "sup", directory) ? S_OK : E_FAIL);
  CHECK(SetEnvironmentVariableA("MULLION_LOG_PATH", directory) ? S_OK : E_FAIL);
  CHECK(GetModuleFileNameA(nullptr, exe, sizeof(exe)) ? S_OK : E_FAIL);

  step("start up as Starfield does");
  ComPtr<IDXGIFactory4> factory;
  CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
  ComPtr<IDXGIAdapter1> adapter;
  CHECK(factory->EnumAdapters1(0, &adapter));
  ComPtr<ID3D12Device> device;
  for (auto level : {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0})
    if (!device && SUCCEEDED(D3D12CreateDevice(adapter.Get(), level, __uuidof(ID3D12Device), nullptr)))
      CHECK(D3D12CreateDevice(adapter.Get(), level, IID_PPV_ARGS(&device)));
  if (!expect(device != nullptr, "no device at 12_0 or above"))
    return verdict();
  ComPtr<ID3D12DeviceRemovedExtendedData1> dred1;
  ComPtr<ID3D12DeviceRemovedExtendedData> dred;
  device.As(&dred1);
  device.As(&dred);
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  ComPtr<ID3D12CommandQueue> queue;
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  WNDCLASSA wc{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr, "d3d12_startup"};
  HWND window = RegisterClassA(&wc) ? CreateWindowA(wc.lpszClassName, "", WS_OVERLAPPEDWINDOW, 0, 0, 640, 360, nullptr,
                                                    nullptr, wc.hInstance, nullptr)
                                    : nullptr;
  if (!expect(window != nullptr, "could not create the window"))
    return verdict();
  DXGI_SWAP_CHAIN_DESC1 desc{0, 0, DXGI_FORMAT_R10G10B10A2_UNORM, FALSE, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT, 2};
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  ComPtr<IDXGISwapChain1> swapchain;
  CHECK(factory->CreateSwapChainForHwnd(queue.Get(), window, &desc, nullptr, nullptr, &swapchain));

  step("refuse a call of each DLL");
  D3D12_FEATURE_DATA_D3D12_OPTIONS options;
  expect(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options) - 1) == E_INVALIDARG,
         "a short CheckFeatureSupport is not E_INVALIDARG");
  DXGI_SWAP_CHAIN_DESC windowless{{0, 0, {}, desc.Format}, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT, 2, nullptr, TRUE,
                                  DXGI_SWAP_EFFECT_FLIP_DISCARD};
  ComPtr<IDXGISwapChain> refused;
  expect(factory->CreateSwapChain(queue.Get(), &windowless, &refused) == DXGI_ERROR_INVALID_CALL,
         "a swap chain with no window is not DXGI_ERROR_INVALID_CALL");

  step("read the logs");
  const std::string name = std::string(exe).substr(std::string(exe).find_last_of("\\/") + 1);
  const std::string base = std::string(directory) + "\\" + name.substr(0, name.find_last_of('.')) + "_";
  for (auto [log, line] : {std::pair{"d3d12.log", "CheckFeatureSupport: E_INVALIDARG"},
                           std::pair{"dxgi.log", "CreateSwapChain: DXGI_ERROR_INVALID_CALL"}}) {
    std::ifstream file(base + log);
    const std::string lines{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    expect(file.is_open(), "%s%s was not written", base.c_str(), log);
    expect(lines.find(line) != std::string::npos, "%s has no \"%s\"", log, line);
    file.close();
    DeleteFileA((base + log).c_str());
  }
  swapchain = nullptr;
  queue = nullptr;
  device = nullptr;
  DestroyWindow(window);
  RemoveDirectoryA(directory);
  return verdict();
}
