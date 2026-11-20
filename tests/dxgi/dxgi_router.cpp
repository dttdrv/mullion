// contract: what a title sees when the router answers it, and what does not change when the router refuses one
// generation for that title.
//
// The rules and their precedence are tested without a GPU by tests/host/router.cpp; this cell is about the two things
// only a running process can show:
//
// - the error each generation gives for "there is no device here", which is the API's own and not one this build
//   invented: Direct3DCreate9 answers null, Direct3DCreate9Ex D3DERR_NOTAVAILABLE, D3D11CreateDevice and
//   D3D10CoreCreateDevice and D3D12CreateDevice E_NOINTERFACE (docs/ARCHITECTURE.md section 1, mullion.conf's
//   router section).
// - that a refusal is about the title's request and not about this build's own layers. Direct3D 10 is implemented
//   over the Direct3D 11 device and the Direct3D 9 device makes one to draw over, so refusing router.d3d11 for a
//   process must still leave Direct3D 9 able to make a device -- the alternative is a refusal that reads as a broken
//   D3D9 layer (observed before this rule: D3D9's own CreateDevice answered 0x80004002).
//
// Every case runs in a child of its own, as the settings are per process and the environment is read once a
// generation's library is loaded (the same reason tests/dxgi/dxgi_ngx_trust.cpp forks).
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi1_2.h>
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

// D3D9: the classic create answers null, the Ex create its documented error
int
child_d3d9(bool expect_device) {
  IDirect3D9 *d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
  if (!expect_device) {
    if (d3d9)
      d3d9->Release();
    return d3d9 ? 1 : 0;
  }
  if (!d3d9)
    return 2;
  D3DADAPTER_IDENTIFIER9 identifier{};
  const HRESULT caps = d3d9->GetAdapterIdentifier(0, 0, &identifier);
  d3d9->Release();
  return SUCCEEDED(caps) ? 0 : 2;
}

int
child_d3d9ex(bool expect_device) {
  IDirect3D9Ex *ex = nullptr;
  const HRESULT hr = Direct3DCreate9Ex(D3D_SDK_VERSION, &ex);
  if (!expect_device) {
    if (ex)
      ex->Release();
    return hr == D3DERR_NOTAVAILABLE ? 0 : 1;
  }
  if (FAILED(hr) || !ex)
    return 2;
  ex->Release();
  return 0;
}

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

// Direct3D 9 is the one that shows what a refusal of another generation does not touch: its device is made over a
// Direct3D 11 one, and it still has to work when the title is refused that generation
int
child_d3d9_over_refused_d3d11() {
  IDirect3D9 *d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d9)
    return 2;
  D3DCAPS9 caps{};
  HRESULT hr = d3d9->GetDeviceCaps(0, D3DDEVTYPE_HAL, &caps);
  WNDCLASSA wc{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr, "dxgi_router"};
  RegisterClassA(&wc);
  HWND window = CreateWindowA("dxgi_router", "dxgi_router", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
                              wc.hInstance, nullptr);
  D3DPRESENT_PARAMETERS present{};
  present.BackBufferWidth = 64;
  present.BackBufferHeight = 64;
  present.BackBufferFormat = D3DFMT_X8R8G8B8;
  present.SwapEffect = D3DSWAPEFFECT_DISCARD;
  present.hDeviceWindow = window;
  present.Windowed = TRUE;
  present.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  IDirect3DDevice9 *device = nullptr;
  if (SUCCEEDED(hr))
    hr = d3d9->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &present, &device);
  const bool made = SUCCEEDED(hr) && device;
  if (device)
    device->Release();
  d3d9->Release();
  DestroyWindow(window);
  return made ? 0 : 1;
}

} // namespace

int
main(int argc, char **argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc > 1) {
    const std::string mode = argv[1];
    if (mode == "d3d9-yes")
      return child_d3d9(true);
    if (mode == "d3d9-no")
      return child_d3d9(false);
    if (mode == "d3d9ex-yes")
      return child_d3d9ex(true);
    if (mode == "d3d9ex-no")
      return child_d3d9ex(false);
    if (mode == "d3d11-yes")
      return child_d3d11(true);
    if (mode == "d3d11-no")
      return child_d3d11(false);
      if (mode == "d3d9-over-refused-d3d11")
      return child_d3d9_over_refused_d3d11();
    printf("failed: unknown mode %s\n", mode.c_str());
    return 2;
  }

  // every generation answers when nothing refuses it, in its own process
  expect(child("d3d9-yes", "router.d3d9=Auto"), "D3D9 answered nothing when the router was left alone");
  expect(child("d3d9ex-yes", "router.d3d9=Auto"), "D3D9Ex answered nothing when the router was left alone");
  expect(child("d3d11-yes", "router.d3d11=Auto"), "D3D11 answered nothing when the router was left alone");

  // and each gives its own documented error when it is refused
  expect(child("d3d9-no", "router.d3d9=false"), "a refused Direct3DCreate9 did not answer null");
  expect(child("d3d9ex-no", "router.d3d9=false"), "a refused Direct3DCreate9Ex did not answer D3DERR_NOTAVAILABLE");
  expect(child("d3d11-no", "router.d3d11=false"), "a refused D3D11CreateDevice did not answer E_NOINTERFACE");

  // refusing a generation for the title does not refuse it to this build's own layer: Direct3D 9 sits on Direct3D 11,
  // and its device has to come with the refusal in place (d3d11.cpp's MULLION_INTERNAL_DEVICE)
  expect(child("d3d9-over-refused-d3d11", "router.d3d11=false"),
         "refusing router.d3d11 left the Direct3D 9 device with no Direct3D 11 device to draw over");

  // refusing the generation Direct3D 9 sits on does not refuse Direct3D 9 itself, and the other way round
  expect(child("d3d9-yes", "router.d3d12=false"), "refusing another generation refused Direct3D 9 too");
  expect(child("d3d11-yes", "router.d3d9=false"), "refusing another generation refused Direct3D 11 too");

  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
