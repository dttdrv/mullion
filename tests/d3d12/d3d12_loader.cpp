// contract: Direct3D 12 is two modules (Getting Started with the Agility SDK, DirectX Developer Blog): d3d12.dll,
// "now just a thin loader", and D3D12Core.dll, which it loads and which is the runtime. an application built with
// the Agility SDK exports the version and path of a D3D12Core.dll of its own (D3D12SDKVersion, D3D12SDKPath), and the
// system's is loaded in its place when that is the more recent: here always, so such an application gets its device.
// applications also ask for the module by name to see that Direct3D 12 is this kind.
// d3d12.dll's exports are its own functions, as Windows' are: some applications read an export's address from the
// export address table themselves and call it, and an entry whose address is within the export section is a
// forwarder RVA, the address of a "DLL.name" string (PE Format, Export Address Table).
#include "d3d12_test.hpp"

// an Agility SDK far from any there is, and where its files would be
extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = D3D12_SDK_VERSION + 1000;
__declspec(dllexport) extern const char *D3D12SDKPath = ".\\D3D12\\";
}

int
main(int, char **) {
  unsigned wrong = 0;
  // linked to d3d12.dll, the process has the core before any call
  HMODULE core = GetModuleHandleA("D3D12Core.dll");
  if (!core && ++wrong)
    printf("no module named D3D12Core.dll in a process that links to d3d12.dll\n");
  ComPtr<ID3D12Device> device;
  HRESULT hr = make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
  if (FAILED(hr) && ++wrong)
    printf("no device for an application with an Agility SDK of its own: %#lx\n", hr);
  // the device is the core's: its functions are in that module
  MEMORY_BASIC_INFORMATION at{};
  if (device && core && (!VirtualQuery(*(void ***)device.Get(), &at, sizeof(at)) || at.AllocationBase != core) && ++wrong)
    printf("the device's functions are not in D3D12Core.dll\n");
  void *create = nullptr;
  if (auto loader = (BYTE *)GetModuleHandleA("d3d12.dll")) {
    auto nt = (IMAGE_NT_HEADERS *)(loader + ((IMAGE_DOS_HEADER *)loader)->e_lfanew);
    auto section = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    auto exports = (IMAGE_EXPORT_DIRECTORY *)(loader + section.VirtualAddress);
    auto names = (DWORD *)(loader + exports->AddressOfNames);
    auto ordinals = (WORD *)(loader + exports->AddressOfNameOrdinals);
    auto addresses = (DWORD *)(loader + exports->AddressOfFunctions);
    for (DWORD i = 0; i < exports->NumberOfNames; i++) {
      auto name = (const char *)(loader + names[i]);
      DWORD rva = addresses[ordinals[i]];
      if (rva - section.VirtualAddress < section.Size && ++wrong)
        printf("d3d12.dll forwards %s to %s\n", name, (const char *)(loader + rva));
      else if (!strcmp(name, "D3D12CreateDevice"))
        create = loader + rva;
    }
  } else if (++wrong)
    printf("no module named d3d12.dll in a process that links to it\n");
  // called as those applications call it: through the address the table gives
  ComPtr<ID3D12Device> again;
  if (create && FAILED(hr = ((decltype(&D3D12CreateDevice))create)(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&again))) &&
      ++wrong)
    printf("no device from D3D12CreateDevice at its address in the export table: %#lx\n", hr);
  printf("%s: %u wrong\n", wrong ? "failed" : "passed", wrong);
  return wrong != 0;
}
