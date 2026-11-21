// contract: a program that loads d3d11.dll itself, makes a device, releases it and unloads the DLL, as launchers and
// engines do to probe for support, keeps running. FreeLibrary: "When the reference count reaches zero, the module is
// unloaded from the address space of the calling process"
// https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-freelibrary
// a thread a device started that still runs the DLL's code after that faults. this program imports no Direct3D DLL
// (d3d11_test.hpp would), so the FreeLibrary below is the last reference. three seconds after it outlast the residency
// heartbeat's one second period (src/mullion/mullion_heartbeat.cpp)
#include <windows.h>
#include <d3d11.h>
#include <cstdio>

int
main() {
  HMODULE module = LoadLibraryA("d3d11.dll");
  if (!module) {
    printf("failed: no d3d11.dll\n");
    return 1;
  }
  auto create = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(module, "D3D11CreateDevice");
  ID3D11Device *device = nullptr;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  HRESULT hr = create ? create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                               nullptr, nullptr)
                      : E_NOINTERFACE;
  if (FAILED(hr)) {
    printf("failed: D3D11CreateDevice %#lx\n", hr);
    return 1;
  }
  device->Release();
  if (!FreeLibrary(module)) {
    printf("failed: FreeLibrary %lu\n", GetLastError());
    return 1;
  }
  Sleep(3000);
  printf("passed: d3d11.dll %s after FreeLibrary\n", GetModuleHandleA("d3d11.dll") ? "kept loaded" : "unloaded");
  return 0;
}
