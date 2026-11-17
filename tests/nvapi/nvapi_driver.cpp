// contract: NVAPI reports the driver Windows' device list holds where that is NVIDIA's (the last five digits of its
// DriverVersion: x.x.16.1714 is 617.14), else 999.99, the largest those digits hold, which every minimum a game asks
// for passes. both functions that report it agree, and their branch names its major and ends with its NUL. with
// NVEXT off nvapi64.dll does not load, as on another vendor's GPU. each case runs in a child of its own, as NVAPI
// reads the device list and decides NVEXT once a process
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#define __NVAPI_EMPTY_SAL
#include "nvapi.h"
#include "nvapi_interface.h"
#undef __NVAPI_EMPTY_SAL
#include "../trace.hpp"

static const wchar_t adapter[] =
    L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}\\0000";

// a function by its name in NVIDIA's table
template <typename F>
static F
function(FARPROC query, const char *name) {
  for (auto &entry : nvapi_interface_table)
    if (!strcmp(entry.func, name))
      return (F)((void *(__cdecl *)(NvU32))query)(entry.id);
  return nullptr;
}

// in the child: what NVAPI reports, against the version wanted (0: nvapi64.dll does not load)
static int
report(unsigned wanted) {
  step("NVAPI's driver, %u wanted", wanted);
  auto nvapi = LoadLibraryA("nvapi64.dll");
  DWORD error = nvapi ? 0 : GetLastError();
  if (!wanted)
    return !expect(error == ERROR_DLL_INIT_FAILED, "nvapi64.dll with NVEXT off: loaded %d, error %lu", !!nvapi, error);
  auto query = nvapi ? GetProcAddress(nvapi, "nvapi_QueryInterface") : nullptr;
  if (!expect(query, "nvapi64.dll does not load: %lu", error))
    return 1;
  auto initialize = function<NvAPI_Status(__cdecl *)()>(query, "NvAPI_Initialize");
  auto sys = function<NvAPI_Status(__cdecl *)(NvU32 *, NvAPI_ShortString)>(query, "NvAPI_SYS_GetDriverAndBranchVersion");
  auto display = function<NvAPI_Status(__cdecl *)(NvDisplayHandle, NV_DISPLAY_DRIVER_VERSION *)>(
      query, "NvAPI_GetDisplayDriverVersion"
  );
  if (!expect(initialize && sys && display, "NVAPI lacks a function"))
    return 1;
  NvAPI_Status status = initialize();
  if (!expect(status == NVAPI_OK, "NvAPI_Initialize returns %d", status))
    return 1;

  NvU32 version = 0;
  NvAPI_ShortString branch;
  memset(branch, 'x', sizeof(branch));
  status = sys(&version, branch);
  expect(status == NVAPI_OK && version == wanted, "NvAPI_SYS_GetDriverAndBranchVersion returns %d, %u", status, version);
  NV_DISPLAY_DRIVER_VERSION shown;
  memset(&shown, 'x', sizeof(shown));
  shown.version = NV_DISPLAY_DRIVER_VERSION_VER;
  status = display(NVAPI_DEFAULT_HANDLE, &shown);
  expect(status == NVAPI_OK && shown.drvVersion == wanted, "NvAPI_GetDisplayDriverVersion returns %d, %u", status,
         shown.drvVersion);
  auto major = "r" + std::to_string(wanted / 100) + "_";
  for (auto *text : {branch, shown.szBuildBranchString})
    if (expect(memchr(text, 0, NVAPI_SHORT_STRING_MAX), "a branch has no NUL"))
      expect(!strncmp(text, major.c_str(), major.size()), "the branch %s is not of %u", text, wanted / 100);
  return trace::wrong != 0;
}

static bool
child(unsigned wanted, const char *nvext) {
  char exe[MAX_PATH];
  if (!GetModuleFileNameA(nullptr, exe, sizeof(exe)))
    return false;
  SetEnvironmentVariableA("MULLION_ENABLE_NVEXT", nvext);
  std::string command = std::string("\"") + exe + "\" " + std::to_string(wanted);
  STARTUPINFOA startup{sizeof(startup)};
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = startup.hStdError = GetStdHandle(STD_OUTPUT_HANDLE);
  SetHandleInformation(startup.hStdOutput, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
  PROCESS_INFORMATION process{};
  bool ran = CreateProcessA(exe, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process);
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

// the device list's DriverVersion: a value, or none
static bool
set(const wchar_t *value) {
  if (!value) {
    LSTATUS status = RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, adapter, L"DriverVersion");
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
  }
  return !RegSetKeyValueW(HKEY_LOCAL_MACHINE, adapter, L"DriverVersion", REG_SZ, value,
                          DWORD((wcslen(value) + 1) * sizeof(wchar_t)));
}

int
main(int argc, char **argv) {
  if (argc > 1)
    return report(strtoul(argv[1], nullptr, 10));

  wchar_t held[64];
  DWORD size = sizeof(held);
  bool had = !RegGetValueW(HKEY_LOCAL_MACHINE, adapter, L"DriverVersion", RRF_RT_REG_SZ, nullptr, held, &size);
  const struct {
    const wchar_t *value;
    const char *nvext;
    unsigned wanted;
  } cases[] = {{L"35.0.10.1000", "1", 99999}, // Wine's own
               {L"32.0.16.1714", "1", 61714}, // NVIDIA's 617.14
               {L"31.0.15.5222", "1", 55222}, // NVIDIA's 552.22
               {nullptr, "1", 99999},
               {L"32.0.16.1714", "0", 0}};
  for (auto &c : cases) {
    step("the device list's DriverVersion %ls, NVEXT %s", c.value ? c.value : L"none", c.nvext);
    if (expect(set(c.value), "the device list cannot be written: %lu", GetLastError()))
      expect(child(c.wanted, c.nvext), "the child failed");
  }
  step("the device list as it was");
  expect(set(had ? held : nullptr), "the device list cannot be put back");
  return verdict();
}
