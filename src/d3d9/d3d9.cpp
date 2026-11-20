// Direct3D 9's exports: the create calls, the PIX event markers games import
// from d3d9.dll (Alien Swarm's D3DPERF_SetOptions, CS2's BeginEvent/EndEvent/
// SetMarker), the shader validator Wine accepts, and the On12 / maximized-
// windowed shims modern Windows tables carry. ordinals match the NT 10.0 table
// (and DXVK's d3d9.def): Create9On12 @20/@21, validator @24, PERF @27-33,
// Debug @34-35, maximized shim @36, Create9 @37, Create9Ex @38, and the
// nameless ForceHybridEnumeration @16.
#include "d3d9_direct3d.hpp"
#include "../mullion/mullion_router.hpp"

using namespace mullion;

Logger Logger::s_instance("d3d9.log");

namespace {

// D3D9ON12_ARGS is not in this tree's d3d9.h: match Windows SDK / Wine's
// d3d9on12.h layout so Direct3DCreate9On12 can take the pointer games pass.
struct D3D9ON12_ARGS {
  BOOL Enable9On12;
  IUnknown *pD3D12Device;
  IUnknown *pOverrideQueues[2];
  UINT NumQueues;
  UINT NodeMask;
};

// a validator that accepts: Wine's Direct3DShaderValidatorCreate9 hands back an
// object whose Begin/Instruction/End succeed for a well-formed shader, and
// games that call it only need that the create does not return null. IUnknown
// is the first base so the pointer this hands back is the IUnknown, which is
// the one a caller releases.
class ShaderValidator9 final : public IUnknown, private d3d9::Object9 {
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (riid == IID_IUnknown) {
      *object = static_cast<IUnknown *>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return d3d9::Object9::AddRef(); }
  ULONG STDMETHODCALLTYPE Release() override { return d3d9::Object9::Release(); }

  // The undocumented IDirect3DShaderValidator9 entry points, after IUnknown's three: a caller reaches them by vtable
  // index, so they are virtual here and their signatures are the interface's (Wine's d3d9_private.h declares the same
  // begin/instruction/end shape). They accept a well-formed shader, which is all a caller of the validator asks of it.
  virtual HRESULT STDMETHODCALLTYPE Begin(void *, void *, DWORD) { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Instruction(const char *, UINT, const DWORD *, DWORD) { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE End() { return S_OK; }
};

} // namespace

extern "C" IDirect3D9 *WINAPI Direct3DCreate9(UINT sdk_version) {
  if (!mullion::router::offered(mullion::router::Request{mullion::router::Api::D3D9}))
    return nullptr;
  if (sdk_version != D3D_SDK_VERSION) {
    ERR("D3D9: Direct3DCreate9 asked for SDK version ", sdk_version, ", this build is ", D3D_SDK_VERSION, ".");
    return nullptr;
  }
  return d3d9::create_direct3d(false);
}

extern "C" HRESULT WINAPI Direct3DCreate9Ex(UINT sdk_version, IDirect3D9Ex **out) {
  if (!out)
    return D3DERR_INVALIDCALL;
  *out = nullptr;
  if (!mullion::router::offered(mullion::router::Request{mullion::router::Api::D3D9, true}))
    return D3DERR_NOTAVAILABLE;
  if (sdk_version != D3D_SDK_VERSION) {
    ERR("D3D9: Direct3DCreate9Ex asked for SDK version ", sdk_version, ", this build is ", D3D_SDK_VERSION, ".");
    return D3DERR_NOTAVAILABLE;
  }
  IDirect3D9 *created = d3d9::create_direct3d(true);
  if (!created)
    return E_OUTOFMEMORY;
  HRESULT hr = created->QueryInterface(__uuidof(IDirect3D9Ex), reinterpret_cast<void **>(out));
  created->Release();
  return hr;
}

namespace {

// Whether the caller asked for a device this build's 9On12 does not make: Enable9On12 with a Direct3D 12 device, its
// own queues or a node. An Enable9On12 of FALSE asks for the ordinary Direct3D 9 object, which is what Wine and DXVK
// return for it.
bool
on12_overrides(const D3D9ON12_ARGS *args, UINT count) {
  return args && count && args->Enable9On12 &&
         (args->pD3D12Device || args->NumQueues || args->pOverrideQueues[0] || args->pOverrideQueues[1] ||
          args->NodeMask);
}

} // namespace

extern "C" IDirect3D9 *WINAPI Direct3DCreate9On12(UINT sdk_version, D3D9ON12_ARGS *args, UINT count) {
  if (!mullion::router::offered(mullion::router::Request{mullion::router::Api::D3D9, false, true}))
    return nullptr;
  if (sdk_version != D3D_SDK_VERSION) {
    ERR("D3D9: Direct3DCreate9On12 asked for SDK version ", sdk_version, ", this build is ", D3D_SDK_VERSION, ".");
    return nullptr;
  }
  // 9On12 is not a second Metal path here: the D3D9 device already sits on this build's Direct3D 11 device. A caller
  // that hands over a Direct3D 12 device or its own queues asks for the 9On12 device to run on those, which this build
  // cannot do; ignoring them would draw on a device the caller did not name, so the request is refused and logged
  // (Microsoft Learn, D3D9ON12_ARGS and Direct3DCreate9On12)
  if (on12_overrides(args, count)) {
    ERR_ONCE("D3D9: Direct3DCreate9On12 was given a Direct3D 12 device or queues, which this build's 9On12 does not "
             "run on; no device is made rather than one on another device.");
    return nullptr;
  }
  return d3d9::create_direct3d(false);
}

extern "C" HRESULT WINAPI Direct3DCreate9On12Ex(UINT sdk_version, D3D9ON12_ARGS *args, UINT count,
                                                IDirect3D9Ex **out) {
  if (!out)
    return D3DERR_INVALIDCALL;
  *out = nullptr;
  if (!mullion::router::offered(mullion::router::Request{mullion::router::Api::D3D9, true, true}))
    return D3DERR_NOTAVAILABLE;
  if (on12_overrides(args, count)) {
    ERR_ONCE("D3D9: Direct3DCreate9On12Ex was given a Direct3D 12 device or queues, which this build's 9On12 does not "
             "run on; no device is made rather than one on another device.");
    return D3DERR_NOTAVAILABLE;
  }
  return Direct3DCreate9Ex(sdk_version, out);
}

extern "C" int WINAPI D3DPERF_BeginEvent(D3DCOLOR, const WCHAR *) {
  return 0;
}

extern "C" int WINAPI D3DPERF_EndEvent(void) {
  return 0;
}

extern "C" DWORD WINAPI D3DPERF_GetStatus(void) {
  return 0;
}

extern "C" WINBOOL WINAPI D3DPERF_QueryRepeatFrame(void) {
  return FALSE;
}

extern "C" void WINAPI D3DPERF_SetMarker(D3DCOLOR, const WCHAR *) {}

extern "C" void WINAPI D3DPERF_SetOptions(DWORD) {}

extern "C" void WINAPI D3DPERF_SetRegion(D3DCOLOR, const WCHAR *) {}

extern "C" int WINAPI DebugSetLevel(void) {
  return 0;
}

extern "C" void WINAPI DebugSetMute(void) {}

extern "C" BOOL WINAPI Direct3D9EnableMaximizedWindowedModeShim(UINT) {
  return TRUE;
}

// ordinal 16, exported without a name as in Windows: hybrid-GPU enumeration has one GPU to enumerate here
extern "C" void WINAPI Direct3D9ForceHybridEnumeration(UINT) {}

extern "C" void *WINAPI Direct3DShaderValidatorCreate9(void) {
  return static_cast<IUnknown *>(new ShaderValidator9());
}

// processor-specific geometry pipeline stubs (Windows XP era); never called on a HAL device
class D3DFE_PROCESSVERTICES;
using PSGPERRORID = UINT;

extern "C" void WINAPI PSGPError(D3DFE_PROCESSVERTICES *, PSGPERRORID, UINT) {}

extern "C" void WINAPI PSGPSampleTexture(D3DFE_PROCESSVERTICES *, UINT, float (*const)[4], UINT, float (*const)[4]) {}
