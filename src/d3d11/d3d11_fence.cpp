#include "d3d11_fence.hpp"
#include "d3d11_device_child.hpp"
#include "d3d11_resource.hpp"
#include "util_win32_compat.h"
#include "mullion_fence.hpp"

namespace mullion {

class MTLD3D11FenceImpl : public MTLD3D11DeviceChild<MTLD3D11Fence> {
public:
  MTLD3D11FenceImpl(MTLD3D11Device *pDevice, WMT::Reference<WMT::SharedEvent> event, bool shared,
                    const char (&name)[54]) :
      MTLD3D11DeviceChild<MTLD3D11Fence>(pDevice) {
    this->event = std::move(event);
    this->shared = shared;
    memcpy(this->mach_port_name, name, sizeof(this->mach_port_name));
  };

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) final {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) ||
        riid == __uuidof(ID3D11Fence)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D11Query), riid)) {
      WARN("D3D11Fence: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  };

  HRESULT STDMETHODCALLTYPE
  CreateSharedHandle(const SECURITY_ATTRIBUTES *pAttributes, DWORD Access,
                     const WCHAR *Name, HANDLE *pHandle) final {
    InitReturnPtr(pHandle);
    // "CreateSharedHandle requires SHARED and GENERIC_ALL; invalid parameters return DXGI_ERROR_INVALID_CALL"
    // (tests/d3d11/d3d11_shared_fence.cpp)
    if (!pHandle || !shared || Access != GENERIC_ALL)
      return DXGI_ERROR_INVALID_CALL;
    return shareHandle(mach_port_name, pAttributes, Name, pHandle);
  };

  UINT64 STDMETHODCALLTYPE GetCompletedValue() final {
    return event.signaledValue();
  };

  HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64 Value,
                                                 HANDLE Event) final {
    // a null event blocks until the value, as ID3D12Fence's does
    if (!Event)
      return event.waitUntilSignaledValue(Value, ~0ull) ? S_OK : E_FAIL;
    auto shared_event_listener = this->m_parent->GetMullionDevice().queue().GetSharedEventListener();
    MTLSharedEvent_setWin32EventAtValue(event.handle, shared_event_listener, Event, Value);
    return S_OK;
  };
};

HRESULT
CreateFence(MTLD3D11Device *pDevice, UINT64 InitialValue, D3D11_FENCE_FLAG Flags, REFIID riid, void **ppFence) {
  InitReturnPtr(ppFence);
  bool shared = !!(Flags & (D3D11_FENCE_FLAG_SHARED | D3D11_FENCE_FLAG_SHARED_CROSS_ADAPTER));
  auto event = pDevice->GetMTLDevice().newSharedEvent();
  if (!event)
    return E_OUTOFMEMORY;
  event.signalValue(InitialValue);
  char mach_port_name[54] = {};
  if (shared && !shareEvent(event, mach_port_name))
    return E_FAIL;
  auto fence = Com(new MTLD3D11FenceImpl(pDevice, std::move(event), shared, mach_port_name));
  if (!ppFence)
    return S_FALSE;
  return fence->QueryInterface(riid, ppFence);
}

HRESULT
OpenSharedFence(MTLD3D11Device *pDevice, HANDLE hResource, REFIID riid, void **ppFence) {
  InitReturnPtr(ppFence);

  char mach_port_name[54];
  auto event = openSharedEvent(pDevice->GetMTLDevice(), hResource, mach_port_name);
  if (!event)
    return E_INVALIDARG;
  auto fence = Com(new MTLD3D11FenceImpl(pDevice, std::move(event), true, mach_port_name));
  if (!ppFence)
    return S_FALSE;
  return fence->QueryInterface(riid, ppFence);
}

} // namespace mullion
