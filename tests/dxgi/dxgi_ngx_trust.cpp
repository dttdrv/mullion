// contract: with NVEXT on, the prefix trusts the certificate Mullion's nvngx.dll is signed with, and NGX finds its
// core: NGX loads a core only if its Authenticode signature chains to a root the machine trusts for code signing, and
// looks for it in the folder its driver's registry values name. the trust is given once a process makes a DXGI
// factory, which a game does before the device NGX needs, never by loading DXGI alone (DllMain: opening Wine's Root
// store syncs it with the host's under a semaphore other processes share), and never with NVEXT off. each case runs
// in a child of its own, as DXGI decides NVEXT once a process
#include <windows.h>
#include <wincrypt.h>
#include <dxgi1_6.h>
#include <cstdio>
#include <cstring>
#include <string>
#include "../trace.hpp"
#include "../../src/dxgi/dxgi_ngx_certificate.h"

static PCCERT_CONTEXT
trusted(HCERTSTORE root, PCCERT_CONTEXT mullion) {
  return CertFindCertificateInStore(root, X509_ASN_ENCODING, 0, CERT_FIND_EXISTING, mullion, nullptr);
}

static bool
child(const char *mode, const char *nvext) {
  char exe[MAX_PATH];
  if (!GetModuleFileNameA(nullptr, exe, sizeof(exe)))
    return false;
  SetEnvironmentVariableA("MULLION_ENABLE_NVEXT", nvext);
  std::string command = std::string("\"") + exe + "\" " + mode;
  STARTUPINFOA startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  bool ran = CreateProcessA(exe, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process);
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

int
main(int argc, char **argv) {
  const bool core_built = argc > 1 && !strcmp(argv[1], "ngx");
  if (argc > 1 && !core_built) {
    // a child: DXGI is loaded (it is imported) and, for "factory", a factory made
    IDXGIFactory1 *factory = nullptr;
    if (!strcmp(argv[1], "factory") && FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
      return 1;
    if (factory)
      factory->Release();
    return 0;
  }

  auto mullion = CertCreateCertificateContext(X509_ASN_ENCODING, mullion_ngx_certificate,
                                              sizeof(mullion_ngx_certificate));
  auto root = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE, L"Root");
  if (!mullion || !root) {
    printf("failed: the certificate or the Root store cannot be opened: %lu\n", GetLastError());
    return 1;
  }
  step("the prefix without the certificate");
  if (auto held = trusted(root, mullion))
    expect(CertDeleteCertificateFromStore(held), "the certificate could not be taken out: %lu", GetLastError());

  const struct {
    const char *mode, *nvext;
    bool trusts;
  } cases[] = {{"factory", "0", false}, {"load", "1", false}, {"factory", "1", true}};
  for (auto &c : cases) {
    step("a process with NVEXT %s that %s", c.nvext, !strcmp(c.mode, "load") ? "only loads DXGI" : "makes a factory");
    if (!expect(child(c.mode, c.nvext), "the child failed"))
      continue;
    // the store as it is now, not as this process cached it
    auto now = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE, L"Root");
    auto held = now ? trusted(now, mullion) : nullptr;
    expect(!!held == c.trusts, "the certificate is %strusted", held ? "" : "not ");
    if (held)
      CertFreeCertificateContext(held);
    if (now)
      CertCloseStore(now, 0);
  }

  step("the certificate's chain, for code signing, under Authenticode's policy");
  LPSTR usage[] = {const_cast<LPSTR>(szOID_PKIX_KP_CODE_SIGNING)};
  CERT_CHAIN_PARA para{sizeof(para)};
  para.RequestedUsage = {USAGE_MATCH_TYPE_AND, {1, usage}};
  PCCERT_CHAIN_CONTEXT chain = nullptr;
  if (expect(CertGetCertificateChain(nullptr, mullion, nullptr, nullptr, &para, 0, nullptr, &chain),
             "no chain: %lu", GetLastError())) {
    expect(!chain->TrustStatus.dwErrorStatus, "the chain has errors %#lx", chain->TrustStatus.dwErrorStatus);
    CERT_CHAIN_POLICY_PARA policy_para{sizeof(policy_para)};
    CERT_CHAIN_POLICY_STATUS status{sizeof(status)};
    expect(CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_AUTHENTICODE, chain, &policy_para, &status) &&
               !status.dwError,
           "Authenticode's policy refuses it: %#lx", status.dwError);
    CertFreeCertificateChain(chain);
  }

  step("where NGX looks for its core: the folder DXGI is in, where nvngx.dll is when the build has it");
  wchar_t dxgi[MAX_PATH], path[MAX_PATH];
  DWORD size = sizeof(path);
  if (expect(GetModuleFileNameW(GetModuleHandleW(L"dxgi.dll"), dxgi, MAX_PATH), "DXGI's path is unknown") &&
      expect(!RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"FullPath",
                           RRF_RT_REG_SZ, nullptr, path, &size),
             "NGXCore has no FullPath")) {
    *wcsrchr(dxgi, L'\\') = 0;
    expect(!_wcsicmp(dxgi, path), "NGXCore names %ls, DXGI is in %ls", path, dxgi);
    std::wstring core = std::wstring(path) + L"\\nvngx.dll";
    expect(!core_built || GetFileAttributesW(core.c_str()) != INVALID_FILE_ATTRIBUTES, "%ls is not there", core.c_str());
  }
  CertCloseStore(root, 0);
  CertFreeCertificateContext(mullion);
  return verdict();
}
