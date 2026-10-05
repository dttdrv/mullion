// d3d12.dll: Direct3D 12's loader, whose functions are D3D12Core.dll's (d3d12.def). the two are apart because they
// are apart in Windows, where applications built with the Agility SDK look for the core by its name
#include <windows.h>

extern "C" __declspec(dllimport) HRESULT WINAPI D3D12GetInterface(REFCLSID, REFIID, void **);

extern "C" BOOL WINAPI
DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  // one use of the core, for the system to load it with the loader and not at the first call
  volatile auto core = &D3D12GetInterface;
  return core != nullptr;
}
