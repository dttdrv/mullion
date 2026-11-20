// contract: d3d9.dll carries every export a modern Windows d3d9.dll (NT 10.0, as Wine and DXVK mirror it) has, and each
// one can be called safely: the PIX event markers (CS2 imports D3DPERF_BeginEvent/EndEvent/SetMarker, Alien Swarm calls
// D3DPERF_SetOptions before Direct3DCreate9), the shader validator, the processor-specific-geometry-pipeline stubs, the
// debug stubs, the maximized-windowed shim, and the create calls including the 9On12 pair, which Wine and DXVK answer
// with an ordinary Direct3D 9 object when no D3D12 device is handed in.
// (D3DPERF_* "Direct3D Perf" functions:
// https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dperf-beginevent and its siblings;
// Direct3DCreate9On12: https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-direct3dcreate9on12)
//
// every export is looked up with GetProcAddress, not linked, so a name missing from src/d3d9/d3d9.def is a failure of its
// own here and not a link error that hides the rest.
//
// the other DLLs' tables are tests/d3d10/d3d10core_exports.cpp, tests/d3d11/d3d11_exports.cpp,
// tests/d3d12/d3d12_exports.cpp and tests/dxgi/dxgi_exports.cpp. ordinal 16 is Direct3D9ForceHybridEnumeration,
// exported without a name, as Windows does.
#include <windows.h>
#include <d3d9.h>
#include <cstdio>

// not in this tree's d3d9.h: the Windows SDK's d3d9on12.h layout (src/d3d9/d3d9.cpp declares the same)
struct D3D9ON12_ARGS {
  BOOL Enable9On12;
  IUnknown *pD3D12Device;
  IUnknown *pOverrideQueues[2];
  UINT NumQueues;
  UINT NodeMask;
};

using PFN_BeginEvent = int(WINAPI *)(D3DCOLOR, const WCHAR *);
using PFN_EndEvent = int(WINAPI *)(void);
using PFN_GetStatus = DWORD(WINAPI *)(void);
using PFN_QueryRepeatFrame = BOOL(WINAPI *)(void);
using PFN_Marker = void(WINAPI *)(D3DCOLOR, const WCHAR *);
using PFN_SetOptions = void(WINAPI *)(DWORD);
using PFN_DebugSetLevel = int(WINAPI *)(void);
using PFN_DebugSetMute = void(WINAPI *)(void);
using PFN_MaximizedShim = BOOL(WINAPI *)(UINT);
using PFN_ValidatorCreate = void *(WINAPI *)(void);
using PFN_PSGPError = void(WINAPI *)(void *, UINT, UINT);
using PFN_PSGPSampleTexture = void(WINAPI *)(void *, UINT, float (*const)[4], UINT, float (*const)[4]);
using PFN_Create9 = IDirect3D9 *(WINAPI *)(UINT);
using PFN_Create9Ex = HRESULT(WINAPI *)(UINT, IDirect3D9Ex **);
using PFN_Create9On12 = IDirect3D9 *(WINAPI *)(UINT, D3D9ON12_ARGS *, UINT);
using PFN_Create9On12Ex = HRESULT(WINAPI *)(UINT, D3D9ON12_ARGS *, UINT, IDirect3D9Ex **);

int
main() {
  unsigned wrong = 0, checks = 0;
  auto expect = [&](bool ok, const char *what) {
    checks++;
    if (!ok && wrong++ < 16)
      printf("wrong: %s\n", what);
  };

  HMODULE d3d9 = LoadLibraryA("d3d9.dll");
  if (!d3d9) {
    printf("failed: LoadLibrary d3d9.dll (error %lu)\n", GetLastError());
    return 1;
  }

  // every modern Windows d3d9.dll export, resolved by name
  static const char *const kExports[] = {"Direct3DShaderValidatorCreate9",
                                         "PSGPError",
                                         "PSGPSampleTexture",
                                         "D3DPERF_BeginEvent",
                                         "D3DPERF_EndEvent",
                                         "D3DPERF_GetStatus",
                                         "D3DPERF_QueryRepeatFrame",
                                         "D3DPERF_SetMarker",
                                         "D3DPERF_SetOptions",
                                         "D3DPERF_SetRegion",
                                         "DebugSetLevel",
                                         "DebugSetMute",
                                         "Direct3D9EnableMaximizedWindowedModeShim",
                                         "Direct3DCreate9",
                                         "Direct3DCreate9Ex",
                                         "Direct3DCreate9On12",
                                         "Direct3DCreate9On12Ex"};
  bool all_resolved = true;
  for (const char *name : kExports) {
    if (!GetProcAddress(d3d9, name)) {
      printf("missing export: %s\n", name);
      all_resolved = false;
    }
  }
  expect(all_resolved, "every d3d9.dll export resolves");
  {
    auto hybrid = reinterpret_cast<void(WINAPI *)(UINT)>(
        reinterpret_cast<void *>(GetProcAddress(d3d9, MAKEINTRESOURCEA(16))));
    expect(hybrid != nullptr && GetProcAddress(d3d9, "Direct3D9ForceHybridEnumeration") == nullptr,
           "ordinal 16 is Direct3D9ForceHybridEnumeration, with no name");
    if (hybrid)
      hybrid(0);
  }
  if (!all_resolved) {
    printf("%u of %u checks wrong\n", wrong, checks);
    return 1;
  }

#define PROC(type, name) auto name = reinterpret_cast<type>(reinterpret_cast<void *>(GetProcAddress(d3d9, #name)))
  PROC(PFN_BeginEvent, D3DPERF_BeginEvent);
  PROC(PFN_EndEvent, D3DPERF_EndEvent);
  PROC(PFN_GetStatus, D3DPERF_GetStatus);
  PROC(PFN_QueryRepeatFrame, D3DPERF_QueryRepeatFrame);
  PROC(PFN_Marker, D3DPERF_SetMarker);
  PROC(PFN_Marker, D3DPERF_SetRegion);
  PROC(PFN_SetOptions, D3DPERF_SetOptions);
  PROC(PFN_DebugSetLevel, DebugSetLevel);
  PROC(PFN_DebugSetMute, DebugSetMute);
  PROC(PFN_MaximizedShim, Direct3D9EnableMaximizedWindowedModeShim);
  PROC(PFN_ValidatorCreate, Direct3DShaderValidatorCreate9);
  PROC(PFN_PSGPError, PSGPError);
  PROC(PFN_PSGPSampleTexture, PSGPSampleTexture);
  PROC(PFN_Create9, Direct3DCreate9);
  PROC(PFN_Create9Ex, Direct3DCreate9Ex);
  PROC(PFN_Create9On12, Direct3DCreate9On12);
  PROC(PFN_Create9On12Ex, Direct3DCreate9On12Ex);
#undef PROC

  // the PIX markers: safe to call in any order, and with the no-op results a build with no PIX attached answers
  D3DPERF_SetOptions(0);
  D3DPERF_BeginEvent(0, L"t");
  D3DPERF_SetMarker(0, L"m");
  D3DPERF_SetRegion(0, L"r");
  int end_level = D3DPERF_EndEvent();
  (void)end_level;
  DWORD status = D3DPERF_GetStatus();
  (void)status;
  BOOL repeat = D3DPERF_QueryRepeatFrame();
  expect(repeat == FALSE, "D3DPERF_QueryRepeatFrame is FALSE (no PIX asking to repeat a frame)");

  // the debug stubs
  DebugSetLevel();
  DebugSetMute();

  expect(Direct3D9EnableMaximizedWindowedModeShim(0) != FALSE, "Direct3D9EnableMaximizedWindowedModeShim(0) is TRUE");

  // the validator: a non-null object whose lifetime is IUnknown's
  {
    void *validator = Direct3DShaderValidatorCreate9();
    expect(validator != nullptr, "Direct3DShaderValidatorCreate9 is not null");
    if (validator)
      static_cast<IUnknown *>(validator)->Release();
  }

  // the PSGP stubs take nulls
  PSGPError(nullptr, 0, 0);
  PSGPSampleTexture(nullptr, 0, nullptr, 0, nullptr);

  // the create calls
  {
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    expect(d3d != nullptr, "Direct3DCreate9(D3D_SDK_VERSION) is not null");
    if (d3d)
      d3d->Release();
  }
  {
    IDirect3D9Ex *ex = nullptr;
    HRESULT hr = Direct3DCreate9Ex(D3D_SDK_VERSION, &ex);
    expect(hr == S_OK && ex != nullptr, "Direct3DCreate9Ex(D3D_SDK_VERSION) is S_OK with an object");
    if (ex)
      ex->Release();
  }
  {
    IDirect3D9 *on12 = Direct3DCreate9On12(D3D_SDK_VERSION, nullptr, 0);
    expect(on12 != nullptr, "Direct3DCreate9On12(D3D_SDK_VERSION, nullptr, 0) is not null (falls back to Direct3D 9)");
    if (on12)
      on12->Release();
  }
  {
    IDirect3D9Ex *ex = nullptr;
    HRESULT hr = Direct3DCreate9On12Ex(D3D_SDK_VERSION, nullptr, 0, &ex);
    expect(hr == S_OK && ex != nullptr, "Direct3DCreate9On12Ex(D3D_SDK_VERSION, nullptr, 0) is S_OK with an object");
    if (ex)
      ex->Release();
  }

  // CS2's import set: the three PIX calls and Direct3DCreate9 resolve under their own names, and the markers make it
  // through a begin / marker / end frame the way the engine brackets one
  {
    expect(GetProcAddress(d3d9, "D3DPERF_BeginEvent") != nullptr, "CS2 import D3DPERF_BeginEvent resolves");
    expect(GetProcAddress(d3d9, "D3DPERF_EndEvent") != nullptr, "CS2 import D3DPERF_EndEvent resolves");
    expect(GetProcAddress(d3d9, "D3DPERF_SetMarker") != nullptr, "CS2 import D3DPERF_SetMarker resolves");
    expect(GetProcAddress(d3d9, "Direct3DCreate9") != nullptr, "CS2 import Direct3DCreate9 resolves");
    D3DPERF_BeginEvent(D3DCOLOR_ARGB(255, 255, 0, 0), L"cs2 frame");
    D3DPERF_SetMarker(D3DCOLOR_ARGB(255, 0, 255, 0), L"cs2 marker");
    D3DPERF_EndEvent();
  }

  // Alien Swarm's path: D3DPERF_SetOptions(1) (hide this process from PIX) before Direct3DCreate9 reaches a live object
  {
    D3DPERF_SetOptions(1);
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    expect(d3d != nullptr, "Alien Swarm: Direct3DCreate9 after D3DPERF_SetOptions is not null");
    if (d3d) {
      expect(d3d->GetAdapterCount() > 0, "Alien Swarm: the object has an adapter");
      D3DCAPS9 caps{};
      expect(SUCCEEDED(d3d->GetDeviceCaps(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps)),
             "Alien Swarm: the object answers GetDeviceCaps");
      d3d->Release();
    }
  }

  FreeLibrary(d3d9);

  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong ? 1 : 0;
}
