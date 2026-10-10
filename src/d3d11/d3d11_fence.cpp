#include "d3d11_fence.hpp"
#include "d3d11_device_child.hpp"
#include "d3d11_resource.hpp"
#include "util_win32_compat.h"

namespace dxmt {

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

    OBJECT_ATTRIBUTES attr = {};
    attr.Length = sizeof(attr);
    attr.SecurityDescriptor = pAttributes ? pAttributes->lpSecurityDescriptor : nullptr;

    std::wstring buffer;
    UNICODE_STRING name_str;
    if (Name) {
      if (wcslen(Name) > MAX_PATH)
        return DXGI_ERROR_INVALID_CALL;
      DWORD session;

      ProcessIdToSessionId(GetCurrentProcessId(), &session);
      buffer = L"\\Sessions\\" + std::to_wstring(session) + L"\\BaseNamedObjects\\" + Name;
      name_str.MaximumLength = name_str.Length = buffer.size() * sizeof(WCHAR);
      name_str.MaximumLength += sizeof(WCHAR);
      name_str.Buffer = buffer.data();

      attr.ObjectName = &name_str;
      attr.Attributes = OBJ_CASE_INSENSITIVE;
    }

    if (D3DKMTShareObjects(1, &local_kmt, &attr, Access, pHandle)) {
      ERR("D3D11Fence: Failed to create shared handle");
      return E_FAIL;
    }

    return S_OK;
  };

  UINT64 STDMETHODCALLTYPE GetCompletedValue() final {
    return event.signaledValue();
  };

  HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64 Value,
                                                 HANDLE Event) final {
    auto shared_event_listener = this->m_parent->GetDXMTDevice().queue().GetSharedEventListener();
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
  auto fence = Com(new MTLD3D11FenceImpl(pDevice, std::move(event), 0));
  if (shared) {
    if (!(pDevice->GetLocalD3DKMT() & 0xc0000000)) {
      ERR("D3D11Fence: Invalid device handle");
      return E_FAIL;
    }
    mach_port_t mach_port = fence->event.createMachPort();
    if (!mach_port) {
      ERR("D3D11Fence: Failed to create mach port for shared fence");
      return E_FAIL;
    }
    char mach_port_name[54];
    MakeUniqueSharedName(mach_port_name);
    if (!WMTBootstrapRegister(mach_port_name, mach_port)) {
      ERR("D3D11Fence: Failed to register mach port for shared fence");
      return E_FAIL;
    }
    // Wine's resource objects retain runtime data with the NT handle; synchronization objects do not
    D3DKMT_CREATEALLOCATION create = {};
    create.hDevice = pDevice->GetLocalD3DKMT();
    create.pPrivateRuntimeData = mach_port_name;
    create.PrivateRuntimeDataSize = sizeof(mach_port_name);
    create.Flags.StandardAllocation = create.Flags.ExistingSysMem = 1;
    create.Flags.CreateResource = create.Flags.CreateShared = create.Flags.NtSecuritySharing = 1;
    D3DDDI_ALLOCATIONINFO2 allocation = {};
    allocation.pSystemMem = mach_port_name;
    create.pAllocationInfo2 = &allocation;
    create.NumAllocations = 1;
    D3DKMT_CREATESTANDARDALLOCATION standard = {};
    standard.Type = D3DKMT_STANDARDALLOCATIONTYPE_EXISTINGHEAP;
    create.pStandardAllocation = &standard;
    if (D3DKMTCreateAllocation2(&create)) {
      ERR("D3D11Fence: Failed to create D3DKMT handle");
      return E_FAIL;
    }
    fence->local_kmt = create.hResource;
  }
  if (!ppFence)
    return S_FALSE;
  return fence->QueryInterface(riid, ppFence);
}

HRESULT
OpenSharedFence(MTLD3D11Device *pDevice, HANDLE hResource,
                REFIID riid, void **ppFence) {
  InitReturnPtr(ppFence);

  if (reinterpret_cast<uintptr_t>(hResource) & 0xc0000000) {
    WARN("OpenSharedFence: Invalid shared handle type");
    return E_INVALIDARG;
  }

  char mach_port_name[54] = {};

  D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE query = {};
  query.hDevice = pDevice->GetLocalD3DKMT();
  query.hNtHandle = hResource;
  query.pPrivateRuntimeData = mach_port_name;
  query.PrivateRuntimeDataSize = sizeof(mach_port_name);

  if (D3DKMTQueryResourceInfoFromNtHandle(&query)) {
    WARN(str::format("OpenSharedFence: Failed to query resource: ", hResource));
    return E_INVALIDARG;
  }

  if (query.PrivateRuntimeDataSize != sizeof(mach_port_name)) {
    WARN(str::format("OpenSharedFence: Unexpected size: ", query.PrivateRuntimeDataSize));
    return E_INVALIDARG;
  }

  D3DKMT_OPENRESOURCEFROMNTHANDLE open = {};
  D3DDDI_OPENALLOCATIONINFO2 allocation = {};
  char driver_data;
  open.hDevice = pDevice->GetLocalD3DKMT();
  open.hNtHandle = hResource;
  open.NumAllocations = 1;
  open.pOpenAllocationInfo2 = &allocation;
  open.pPrivateRuntimeData = mach_port_name;
  open.PrivateRuntimeDataSize = sizeof(mach_port_name);
  open.pTotalPrivateDriverDataBuffer = &driver_data;

  if (D3DKMTOpenResourceFromNtHandle(&open)) {
    WARN(str::format("OpenSharedFence: Failed to open resource: ", hResource));
    return E_INVALIDARG;
  }

  auto fence = Com(new MTLD3D11FenceImpl(pDevice, {}, open.hResource));
  if (open.PrivateRuntimeDataSize != sizeof(mach_port_name) || mach_port_name[sizeof(mach_port_name) - 1])
    return E_INVALIDARG;
  mach_port_t mach_port;
  if (!WMTBootstrapLookUp(mach_port_name, &mach_port)) {
    ERR("OpenSharedFence: Failed to look up mach port");
    return E_INVALIDARG;
  }

  fence->event = pDevice->GetMTLDevice().newSharedEventWithMachPort(mach_port);
  if (!fence->event)
    return E_INVALIDARG;
  if (!ppFence)
    return S_FALSE;
  return fence->QueryInterface(riid, ppFence);
}

} // namespace dxmt
