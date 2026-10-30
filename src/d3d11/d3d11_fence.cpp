#include "d3d11_fence.hpp"
#include "d3d11_device_child.hpp"
#include "d3d11_resource.hpp"
#include "util_win32_compat.h"
#include "mullion_fence.hpp"

namespace mullion {

// a shared fence travels as an ordinary inheritable handle: a file mapping whose first bytes carry the bootstrap
// name of the fence's mach port, which winemetal's bootstrap resolves in any process of the session. the mapping is
// only a carrier: its contents say which shared object the handle belongs to, so OpenSharedFence "only operates on
// fences" (Microsoft Learn, ID3D11Device5::OpenSharedFence) and refuses every other handle by that content.
struct SharedFenceData {
  char tag[16];
  char mach_port_name[54];
};

static constexpr char shared_fence_tag[16] = "MULLION_fence";

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
    // (tests/d3d11/d3d11_shared_fence.cpp). "The name is limited to MAX_PATH characters." (Microsoft Learn,
    // ID3D11Fence::CreateSharedHandle)
    if (!pHandle || !shared || Access != GENERIC_ALL)
      return DXGI_ERROR_INVALID_CALL;
    if (Name && wcslen(Name) > MAX_PATH)
      return DXGI_ERROR_INVALID_CALL;

    // Name is the mapping's name: OpenFileMappingW finds it, as D3D12 shareHandle already names its object
    // (Microsoft Learn, ID3D11Fence::CreateSharedHandle)
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, const_cast<SECURITY_ATTRIBUTES *>(pAttributes),
                                        PAGE_READWRITE, 0, sizeof(SharedFenceData), Name);
    if (!mapping) {
      ERR("D3D11Fence: Failed to create shared handle: ", GetLastError());
      return E_FAIL;
    }
    if (Name && GetLastError() == ERROR_ALREADY_EXISTS) {
      CloseHandle(mapping);
      return DXGI_ERROR_NAME_ALREADY_EXISTS;
    }
    void *view = MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(SharedFenceData));
    if (!view) {
      ERR("D3D11Fence: Failed to write shared handle: ", GetLastError());
      CloseHandle(mapping);
      return E_FAIL;
    }
    SharedFenceData data = {};
    memcpy(data.tag, shared_fence_tag, sizeof(shared_fence_tag));
    memcpy(data.mach_port_name, mach_port_name, sizeof(data.mach_port_name));
    memcpy(view, &data, sizeof(data));
    UnmapViewOfFile(view);

    *pHandle = mapping;
    return S_OK;
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
  if (shared) {
    mach_port_t mach_port = event.createMachPort();
    if (!mach_port) {
      ERR("D3D11Fence: Failed to create mach port for shared fence");
      return E_FAIL;
    }
    MakeUniqueSharedName(mach_port_name);
    if (!WMTBootstrapRegister(mach_port_name, mach_port)) {
      ERR("D3D11Fence: Failed to register mach port for shared fence");
      return E_FAIL;
    }
  }
  auto fence = Com(new MTLD3D11FenceImpl(pDevice, std::move(event), shared, mach_port_name));
  if (!ppFence)
    return S_FALSE;
  return fence->QueryInterface(riid, ppFence);
}

HRESULT
OpenSharedFence(MTLD3D11Device *pDevice, HANDLE hResource, REFIID riid, void **ppFence) {
  InitReturnPtr(ppFence);

  SharedFenceData data = {};
  void *view = hResource ? MapViewOfFile(hResource, FILE_MAP_READ, 0, 0, sizeof(SharedFenceData)) : nullptr;
  if (!view) {
    WARN("OpenSharedFence: handle is not a shared fence");
    return E_INVALIDARG;
  }
  memcpy(&data, view, sizeof(data));
  UnmapViewOfFile(view);
  if (memcmp(data.tag, shared_fence_tag, sizeof(shared_fence_tag)) ||
      data.mach_port_name[sizeof(data.mach_port_name) - 1]) {
    WARN("OpenSharedFence: handle is not a shared fence");
    return E_INVALIDARG;
  }

  mach_port_t mach_port = 0;
  if (!WMTBootstrapLookUp(data.mach_port_name, &mach_port)) {
    ERR("OpenSharedFence: Failed to look up mach port");
    return E_INVALIDARG;
  }

  auto event = pDevice->GetMTLDevice().newSharedEventWithMachPort(mach_port);
  if (!event)
    return E_INVALIDARG;
  auto fence = Com(new MTLD3D11FenceImpl(pDevice, std::move(event), true, data.mach_port_name));
  if (!ppFence)
    return S_FALSE;
  return fence->QueryInterface(riid, ppFence);
}

} // namespace mullion
