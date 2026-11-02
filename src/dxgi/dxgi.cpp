#include "dxgi_interfaces.h"
#include "dxgi_ngx_certificate.h"
#include "log/log.hpp"
#include "util_env.hpp"
#include "config/config.hpp"
#include <algorithm>
#include "winemetal.h"
#include <mutex>
#include <utility>

namespace mullion {
Logger Logger::s_instance("dxgi.log");

VendorExtension g_extension_enabled = VendorExtension::None;

#ifdef _WIN32
std::once_flag nvext_init;

// trusts the certificate Mullion's nvngx.dll is signed with, in the prefix's Root store (NGX loads only a core whose
// signature chains to a trusted root). crypt32 is loaded here only, for the processes that ask for NVEXT
static void TrustNGXCertificate() {
  typedef void *(WINAPI * open_t)(LPCSTR, DWORD, ULONG_PTR, DWORD, const void *);
  typedef BOOL(WINAPI * add_t)(void *, DWORD, const BYTE *, DWORD, DWORD, void *);
  typedef BOOL(WINAPI * close_t)(void *, DWORD);
  HMODULE crypt32 = LoadLibraryW(L"crypt32.dll");
  if (!crypt32)
    return;
  auto open = (open_t)GetProcAddress(crypt32, "CertOpenStore");
  auto add = (add_t)GetProcAddress(crypt32, "CertAddEncodedCertificateToStore");
  auto close = (close_t)GetProcAddress(crypt32, "CertCloseStore");
  // CERT_STORE_PROV_SYSTEM_W, CERT_SYSTEM_STORE_LOCAL_MACHINE; X509_ASN_ENCODING, CERT_STORE_ADD_USE_EXISTING
  if (open && add && close) {
    if (void *store = open((LPCSTR)10, 0, 0, 2 << 16, L"Root")) {
      if (!add(store, 1, mullion_ngx_certificate, sizeof(mullion_ngx_certificate), 2, nullptr))
        Logger::warn("NVEXT: failed to trust the NGX core's certificate");
      close(store, 0);
    }
  }
  FreeLibrary(crypt32);
}

// NVEXT is on with MULLION_ENABLE_NVEXT=1, or where the configuration shows the program an NVIDIA GPU
// (dxgi.customVendorId = 10de), unless MULLION_ENABLE_NVEXT=0: a launcher that rewrites the environment it starts a
// game with leaves the configuration's per-program section alone
static bool NvidiaExtensionRequested() {
  auto env = env::getEnvVar("MULLION_ENABLE_NVEXT");
  if (env == "1" || env == "0")
    return env == "1";
  auto vendor = Config::getInstance().getOption<std::string>("dxgi.customVendorId");
  std::transform(vendor.begin(), vendor.end(), vendor.begin(), [](unsigned char c) { return std::tolower(c); });
  return vendor == "10de" || vendor == "0x10de";
}

// NVEXT: an NVIDIA GPU's driver registry values, where NGX looks for its core: NVIDIA
// Corporation\Global\NGXCore FullPath and nvlddmkm\NGXCore NGXPath, both the folder this DXGI was loaded from, which
// has Mullion's nvngx.dll. A process without it leaves them as they are: they are the prefix's, and another process's
// (Steam's, which loads this DXGI too) removal raced the game's
static void InitializeVendorExtensionNV() {
#ifdef __i386__
  return;
#endif
  if (!NvidiaExtensionRequested())
    return;
  HKEY key1 = nullptr, key2 = nullptr, key3 = nullptr, key4 = nullptr;
  auto name1 = L"{41FCC608-8496-4DEF-B43E-7D9BD675A6FF}";
  WCHAR folder[MAX_PATH] = L"C:\\Windows\\System32";
  HMODULE self = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         (LPCWSTR)&InitializeVendorExtensionNV, &self)) {
    WCHAR path[MAX_PATH];
    DWORD length = GetModuleFileNameW(self, path, MAX_PATH);
    WCHAR *slash = length && length < MAX_PATH ? wcsrchr(path, L'\\') : nullptr;
    if (slash) {
      *slash = 0;
      wcscpy(folder, path);
    }
  }
  if (RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\NVIDIA Corporation\\Global", 0, nullptr, 0,
                      KEY_ALL_ACCESS, nullptr, &key1, nullptr) ||
      RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                      L"SYSTEM\\ControlSet001\\Services\\nvlddmkm", 0, nullptr,
                      0, KEY_ALL_ACCESS, nullptr, &key2, nullptr) ||
      RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", 0,
                      nullptr, 0, KEY_ALL_ACCESS, nullptr, &key3, nullptr) ||
      RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                      L"SYSTEM\\ControlSet001\\Services\\nvlddmkm\\NGXCore", 0,
                      nullptr, 0, KEY_ALL_ACCESS, nullptr, &key4, nullptr)) {
    goto cleanup;
  }
  {
    DWORD _1 = 1, size = DWORD((wcslen(folder) + 1) * sizeof(WCHAR));
    RegSetValueExW(key1, name1, 0, REG_DWORD, (const BYTE *)&_1, sizeof(_1));
    RegSetValueExW(key2, name1, 0, REG_DWORD, (const BYTE *)&_1, sizeof(_1));
    RegSetValueExW(key3, L"FullPath", 0, REG_SZ, (const BYTE *)folder, size);
    RegSetValueExW(key4, L"NGXPath", 0, REG_SZ, (const BYTE *)folder, size);
    TrustNGXCertificate();
    Logger::info("Vendor extension enabled: NVEXT");
    g_extension_enabled = VendorExtension::Nvidia;
  }
  cleanup:
  if (key1) RegCloseKey(key1);
  if (key2) RegCloseKey(key2);
  if (key3) RegCloseKey(key3);
  if (key4) RegCloseKey(key4);
};

static void InitializeMetalCachePath() {
  if (env::getEnvVar("MULLION_USE_DEFAULT_METAL_CACHE") == "1")
    return;
  // This is the framework cache (mainly for PSOs), not managed by Mullion
  auto metal_cache_path = str::format("mullion/", env::getExeName(), "/com.apple.metal");
  if (!WMTSetMetalShaderCachePath(metal_cache_path.c_str())) {
    Logger::info("Failed to set Metal cache path, fallback to system default");
  }
}

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason,
                               LPVOID reserved) {
  if (reason != DLL_PROCESS_ATTACH)
    return TRUE;

  DisableThreadLibraryCalls(instance);

  InitializeMetalCachePath();
  std::call_once(nvext_init, InitializeVendorExtensionNV);

  return TRUE;
}

#endif

extern "C" HRESULT __stdcall DXGIGetDebugInterface1(UINT Flags, REFIID riid,
                                                    void **ppDebug) {
#ifdef _WIN32
  // it's a Mullion implementation detail
  if (riid == MULLION_NVEXT_GUID) {
    std::call_once(nvext_init, InitializeVendorExtensionNV);
    return g_extension_enabled == VendorExtension::Nvidia ? S_OK : E_NOINTERFACE;
  }
#endif

  return E_NOINTERFACE;
}

extern "C" HRESULT __stdcall DXGIDeclareAdapterRemovalSupport() {
  static bool enabled = false;

  if (std::exchange(enabled, true))
    return FAIL(DXGI_ERROR_ALREADY_EXISTS);

  Logger::warn("DXGIDeclareAdapterRemovalSupport: stub");
  return S_OK;
}

} // namespace mullion