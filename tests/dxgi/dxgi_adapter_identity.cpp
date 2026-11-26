// contract: the PCI identity an adapter reports, in each of its descriptions
// (IDXGIAdapter::GetDesc, GetDesc1 and IDXGIAdapter4::GetDesc3: "VendorId",
// "DeviceId", "SubSysId", "Revision", DXGI_ADAPTER_DESC). applications look the
// vendor and device up in tables of PC GPUs, where a device ID of 0 is in none:
// a vendor's adapter has a device of that vendor's, NVIDIA's RTX 4080
// (10de:2704, the GPU src/nvngx/signing/README.md names), AMD's RX 7900 XTX
// (1002:744c) and Intel's Arc A770 (8086:56a0), and Apple's own vendor none.
// the configuration names another device (dxgi.customDeviceId), a subsystem
// (dxgi.customSubSysId, up to eight hex digits) and a revision
// (dxgi.customRevision, up to two), each 0 when not named or not a number of
// its digits, and the name (dxgi.customDeviceDesc). NVEXT
// (MULLION_ENABLE_NVEXT=1) is NVIDIA's vendor. each case runs in a child of its
// own, as the configuration is read once a process.
#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "../trace.hpp"

using Microsoft::WRL::ComPtr;

static const struct Case {
  const char *config, *nvext;
  UINT vendor, device, subsys, revision;
  const wchar_t *name; // null for the GPU's own, which is Apple's
} cases[] = {
    {"", "", 0x1002, 0x744c, 0, 0},
    {"", "1", 0x10de, 0x2704, 0, 0},
    {"dxgi.customVendorId=10de", "", 0x10de, 0x2704, 0, 0},
    {"dxgi.customVendorId=8086", "", 0x8086, 0x56a0, 0, 0},
    {"dxgi.customVendorId=106b", "", 0x106b, 0, 0, 0},
    {"dxgi.customVendorId=10de; dxgi.customDeviceId=2684", "", 0x10de, 0x2684,
     0, 0},
    {"dxgi.customVendorId=10de; dxgi.customDeviceId=2704; "
     "dxgi.customSubSysId=889e1043; dxgi.customRevision=a1; "
     "dxgi.customDeviceDesc=NVIDIA",
     "", 0x10de, 0x2704, 0x889e1043, 0xa1, L"NVIDIA"},
    {"dxgi.customSubSysId=7; dxgi.customRevision=1", "", 0x1002, 0x744c, 7, 1},
    {"dxgi.customSubSysId=123456789; dxgi.customRevision=1a2", "", 0x1002,
     0x744c, 0, 0},
    {"dxgi.customSubSysId=zz; dxgi.customRevision=g", "", 0x1002, 0x744c, 0, 0},
};

// in the child: what the first adapter reports, against case `index`
static int report(unsigned index) {
  auto &wanted = cases[index];
  step("adapter identity of \"%s\", NVEXT \"%s\"", wanted.config, wanted.nvext);
  ComPtr<IDXGIFactory1> factory;
  ComPtr<IDXGIAdapter1> first;
  ComPtr<IDXGIAdapter4> adapter;
  if (!expect(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
                  SUCCEEDED(factory->EnumAdapters1(0, &first)) &&
                  SUCCEEDED(first.As(&adapter)),
              "no adapter"))
    return 1;
  DXGI_ADAPTER_DESC desc{};
  DXGI_ADAPTER_DESC1 desc1{};
  DXGI_ADAPTER_DESC3 desc3{};
  if (!expect(SUCCEEDED(adapter->GetDesc(&desc)) &&
                  SUCCEEDED(adapter->GetDesc1(&desc1)) &&
                  SUCCEEDED(adapter->GetDesc3(&desc3)),
              "no description"))
    return 1;
  auto check = [&](const char *which, UINT vendor, UINT device, UINT subsys,
                   UINT revision, const WCHAR *name) {
    expect(vendor == wanted.vendor && device == wanted.device &&
               subsys == wanted.subsys && revision == wanted.revision,
           "%s: %04x:%04x subsystem %08x revision %02x, where %04x:%04x %08x "
           "%02x is wanted",
           which, vendor, device, subsys, revision, wanted.vendor,
           wanted.device, wanted.subsys, wanted.revision);
    expect(wanted.name ? !wcscmp(name, wanted.name)
                       : wcsstr(name, L"Apple") != nullptr,
           "%s: the name %ls", which, name);
  };
  check("GetDesc", desc.VendorId, desc.DeviceId, desc.SubSysId, desc.Revision,
        desc.Description);
  check("GetDesc1", desc1.VendorId, desc1.DeviceId, desc1.SubSysId,
        desc1.Revision, desc1.Description);
  check("GetDesc3", desc3.VendorId, desc3.DeviceId, desc3.SubSysId,
        desc3.Revision, desc3.Description);
  return trace::wrong != 0;
}

static bool child(unsigned index) {
  char exe[MAX_PATH];
  if (!GetModuleFileNameA(nullptr, exe, sizeof(exe)))
    return false;
  SetEnvironmentVariableA("MULLION_CONFIG",
                          *cases[index].config ? cases[index].config : nullptr);
  SetEnvironmentVariableA("MULLION_ENABLE_NVEXT",
                          *cases[index].nvext ? cases[index].nvext : nullptr);
  std::string command = std::string("\"") + exe + "\" " + std::to_string(index);
  STARTUPINFOA startup{sizeof(startup)};
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = startup.hStdError = GetStdHandle(STD_OUTPUT_HANDLE);
  SetHandleInformation(startup.hStdOutput, HANDLE_FLAG_INHERIT,
                       HANDLE_FLAG_INHERIT);
  PROCESS_INFORMATION process{};
  bool ran = CreateProcessA(exe, command.data(), nullptr, nullptr, TRUE, 0,
                            nullptr, nullptr, &startup, &process);
  SetEnvironmentVariableA("MULLION_CONFIG", nullptr);
  SetEnvironmentVariableA("MULLION_ENABLE_NVEXT", nullptr);
  if (!ran)
    return false;
  DWORD status = 1;
  bool ended = WaitForSingleObject(process.hProcess, 120000) == WAIT_OBJECT_0;
  GetExitCodeProcess(process.hProcess, &status);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return ended && !status;
}

int main(int argc, char **argv) {
  if (argc > 1)
    return report(strtoul(argv[1], nullptr, 10));
  // the test's own environment does not choose for the children
  SetEnvironmentVariableA("MULLION_CONFIG_FILE", "nul");
  for (unsigned i = 0; i < std::size(cases); i++) {
    step("case %u: \"%s\", NVEXT \"%s\"", i, cases[i].config, cases[i].nvext);
    expect(child(i), "the adapter's identity is not the one wanted");
  }
  return verdict();
}
