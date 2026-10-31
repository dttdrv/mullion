#include "d3d11_fence.hpp"
#include "d3d11_device_child.hpp"
#include "d3d11_resource.hpp"
#include "util_win32_compat.h"
#include "mullion_fence.hpp"

namespace mullion {

class MTLD3D11FenceImpl : public MTLD3D11DeviceChild<MTLD3D11Fence> {
public:
  MTLD3D11FenceImpl(MTLD3D11Device *pDevice, WMT::Reference<WMT::SharedEvent> event, D3DKMT_HANDLE handle) :
      MTLD3D11DeviceChild<MTLD3D11Fence>(pDevice) {
    this->event = std::move(event);
    local_kmt = handle;
  };

  ~MTLD3D11FenceImpl() {
    if (local_kmt) {
      D3DKMT_DESTROYALLOCATION destroy = {};
      destroy.hDevice = this->m_parent->GetLocalD3DKMT();
      destroy.hResource = local_kmt;
      D3DKMTDestroyAllocation(&destroy);
    }
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
    if (!pHandle || !local_kmt || Access != GENERIC_ALL)
      return DXGI_ERROR_INVALID_CALL;
    return shareHandle(local_kmt, pAttributes, Access, Name, pHandle);
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
    MTLSharedEvent_setWin32EventAtValue(event.handle, shared_event_listener, Event, Value, true);
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
  auto fence = Com(new MTLD3D11FenceImpl(pDevice, std::move(event), 0));
  if (shared) {
    fence->local_kmt = shareEvent(pDevice->GetLocalD3DKMT(), fence->event);
    if (!fence->local_kmt)
      return E_FAIL;
  }
  if (!ppFence)
    return S_FALSE;
  return fence->QueryInterface(riid, ppFence);
}

HRESULT
OpenSharedFence(MTLD3D11Device *pDevice, HANDLE hResource,
                REFIID riid, void **ppFence) {
  InitReturnPtr(ppFence);

  D3DKMT_HANDLE allocation;
  auto event = openSharedEvent(pDevice->GetMTLDevice(), pDevice->GetLocalD3DKMT(), hResource, allocation);
  auto fence = Com(new MTLD3D11FenceImpl(pDevice, std::move(event), allocation));
  if (!fence->event)
    return E_INVALIDARG;
  if (!ppFence)
    return S_FALSE;
  return fence->QueryInterface(riid, ppFence);
}

} // namespace mullion
