// contract: what a title sees when the router answers it, and that a refusal of one generation refuses no other.
//
// The rules and their precedence are tested without a GPU by tests/host/router.cpp; this cell is about the thing only
// a running process can show: the error a generation gives for "there is no device here" is the API's own and not one
// this build invented -- D3D11CreateDevice answers E_NOINTERFACE (docs/ARCHITECTURE.md section 1, mullion.conf's
// router section) -- and a refusal is about the title's request, never about the other generations.
//
// Every case runs in a child of its own, as the settings are per process and the environment is read once a
// generation's library is loaded (the same reason tests/dxgi/dxgi_ngx_trust.cpp forks).
//
// The Direct3D 9 cases -- Direct3DCreate9 answering null, Direct3DCreate9Ex its documented D3DERR_NOTAVAILABLE, and a
// Direct3D 9 device still made over a refused Direct3D 11 (the case that shows a refusal does not reach this build's
// own layers) -- need the Direct3D 9 layer and live with it, in the pull request that adds src/d3d9.
#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

unsigned wrong = 0, checks = 0;

void
expect(bool ok, const char *what) {
  checks++;
  if (!ok)
    printf("wrong: %s\n", what), wrong++;
}

// a child's verdict: 0 is the answer the case asked for, 1 is a call that answered otherwise, 2 is a failure the
// child did not expect at all (a create that returned an error where one of the two was due), and anything else is
// a crash or a timeout. Only 0 is the case passing: accepting 1 here would make every refusal cell vacuous, which is
// exactly what a control with the rule deliberately broken showed.
bool
child(const char *mode, const char *config) {
  char exe[MAX_PATH];
  if (!GetModuleFileNameA(nullptr, exe, sizeof(exe)))
    return false;
  SetEnvironmentVariableA("MULLION_CONFIG", config);
  std::string command = std::string("\"") + exe + "\" " + mode;
  STARTUPINFOA startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  const bool ran = CreateProcessA(exe, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                                  &process);
  SetEnvironmentVariableA("MULLION_CONFIG", nullptr);
  if (!ran)
    return false;
  DWORD status = 3;
  const bool ended = WaitForSingleObject(process.hProcess, 120000) == WAIT_OBJECT_0;
  GetExitCodeProcess(process.hProcess, &status);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return ended && status == 0;
}

// ---- the children: each asks for one generation and reports what it got ---------------------------------------

int
child_d3d11(bool expect_device) {
  ID3D11Device *device = nullptr;
  ID3D11DeviceContext *context = nullptr;
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL(0);
  const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                       nullptr, 0, D3D11_SDK_VERSION, &device, &level, &context);
  if (!expect_device) {
    if (device)
      device->Release();
    if (context)
      context->Release();
    return hr == E_NOINTERFACE ? 0 : 1;
  }
  if (FAILED(hr) || !device || !context)
    return 2;
  context->Release();
  device->Release();
  return 0;
}

} // namespace

int
main(int argc, char **argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc > 1) {
    const std::string mode = argv[1];
    if (mode == "d3d11-yes")
      return child_d3d11(true);
    if (mode == "d3d11-no")
      return child_d3d11(false);
    printf("failed: unknown mode %s\n", mode.c_str());
    return 2;
  }

  // the generation answers when nothing refuses it, in its own process
  expect(child("d3d11-yes", "router.d3d11=Auto"), "D3D11 answered nothing when the router was left alone");

  // and gives its own documented error when it is refused
  expect(child("d3d11-no", "router.d3d11=false"), "a refused D3D11CreateDevice did not answer E_NOINTERFACE");

  // refusing one generation for the title refuses no other
  expect(child("d3d11-yes", "router.d3d12=false"), "refusing another generation refused Direct3D 11 too");

  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
