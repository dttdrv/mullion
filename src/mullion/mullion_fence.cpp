/*
 * Copyright 2026 Feifan He for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "mullion_fence.hpp"
#include "log/log.hpp"
#include <string>

namespace mullion {

void
Fence::incRef() {
  refcount_.fetch_add(1u, std::memory_order_acquire);
};

void
Fence::decRef() {
  if (refcount_.fetch_sub(1u, std::memory_order_release) == 1u)
    delete this;
};

Fence::Fence(WMT::Device device) : device_(device) {
  reset();
}

void
Fence::reset() {
  this->event_ = device_.newSharedEvent();
}

EventListener::EventListener() :
    shared_event_listener_(SharedEventListener_create()),
    event_listener_thread_([this]() { SharedEventListener_start(this->shared_event_listener_); }) {};

EventListener::~EventListener() {
  SharedEventListener_destroy(shared_event_listener_);
  event_listener_thread_.join();
}

void
EventListener::setEventOnValue(Fence const *fence, HANDLE event, uint64_t value) {
  setEventOnValue(fence->sharedEvent(), event, value);
}

void
EventListener::setEventOnValue(WMT::SharedEvent shared, HANDLE event, uint64_t value) {
  MTLSharedEvent_setWin32EventAtValue(shared.handle, shared_event_listener_, event, value);
}

#ifdef _WIN32

void makeUniqueName(char (&out)[54]);

D3DKMT_HANDLE
shareEvent(D3DKMT_HANDLE device, WMT::SharedEvent event) {
  if (!(device & 0xc0000000)) {
    ERR("shareEvent: Invalid device handle");
    return 0;
  }
  mach_port_t mach_port = event.createMachPort();
  if (!mach_port) {
    ERR("shareEvent: Failed to create mach port for shared fence");
    return 0;
  }
  char mach_port_name[54];
  makeUniqueName(mach_port_name);
  if (!WMTBootstrapRegister(mach_port_name, event.handle, mach_port)) {
    ERR("shareEvent: Failed to register mach port for shared fence");
    return 0;
  }
  // Wine's resource objects retain runtime data with the NT handle; synchronization objects do not
  D3DKMT_CREATEALLOCATION create = {};
  create.hDevice = device;
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
    ERR("shareEvent: Failed to create D3DKMT handle");
    return 0;
  }
  return create.hResource;
}

// `name` among the session's objects (BaseNamedObjects), which `buffer` holds
static void
sessionName(const WCHAR *name, std::wstring &buffer, UNICODE_STRING &string, OBJECT_ATTRIBUTES &attributes) {
  DWORD session;
  ProcessIdToSessionId(GetCurrentProcessId(), &session);
  buffer = L"\\Sessions\\" + std::to_wstring(session) + L"\\BaseNamedObjects\\" + name;
  string.MaximumLength = string.Length = buffer.size() * sizeof(WCHAR);
  string.MaximumLength += sizeof(WCHAR);
  string.Buffer = buffer.data();
  attributes.ObjectName = &string;
}

HRESULT
shareHandle(D3DKMT_HANDLE allocation, const SECURITY_ATTRIBUTES *attributes, DWORD access, const WCHAR *name,
            HANDLE *handle) {
  OBJECT_ATTRIBUTES attr = {};
  attr.Length = sizeof(attr);
  attr.SecurityDescriptor = attributes ? attributes->lpSecurityDescriptor : nullptr;
  attr.Attributes = attributes && attributes->bInheritHandle ? OBJ_INHERIT : 0;
  std::wstring buffer;
  UNICODE_STRING name_str;
  if (name) {
    if (wcslen(name) > MAX_PATH)
      return DXGI_ERROR_INVALID_CALL;
    sessionName(name, buffer, name_str, attr);
  }
  if (D3DKMTShareObjects(1, &allocation, &attr, access, handle)) {
    ERR("shareHandle: Failed to create shared handle");
    return E_FAIL;
  }
  return S_OK;
}

WMT::Reference<WMT::SharedEvent>
openSharedEvent(WMT::Device mtl, D3DKMT_HANDLE device, HANDLE handle, D3DKMT_HANDLE &allocation) {
  allocation = 0;
  if (reinterpret_cast<uintptr_t>(handle) & 0xc0000000) {
    WARN("openSharedEvent: Invalid shared handle type");
    return {};
  }
  char mach_port_name[54] = {};
  D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE query = {};
  query.hDevice = device;
  query.hNtHandle = handle;
  query.pPrivateRuntimeData = mach_port_name;
  query.PrivateRuntimeDataSize = sizeof(mach_port_name);
  if (D3DKMTQueryResourceInfoFromNtHandle(&query)) {
    WARN(str::format("openSharedEvent: Failed to query resource: ", handle));
    return {};
  }
  if (query.PrivateRuntimeDataSize != sizeof(mach_port_name)) {
    WARN(str::format("openSharedEvent: Unexpected size: ", query.PrivateRuntimeDataSize));
    return {};
  }
  D3DKMT_OPENRESOURCEFROMNTHANDLE open = {};
  D3DDDI_OPENALLOCATIONINFO2 info = {};
  char driver_data;
  open.hDevice = device;
  open.hNtHandle = handle;
  open.NumAllocations = 1;
  open.pOpenAllocationInfo2 = &info;
  open.pPrivateRuntimeData = mach_port_name;
  open.PrivateRuntimeDataSize = sizeof(mach_port_name);
  open.pTotalPrivateDriverDataBuffer = &driver_data;
  if (D3DKMTOpenResourceFromNtHandle(&open)) {
    WARN(str::format("openSharedEvent: Failed to open resource: ", handle));
    return {};
  }
  allocation = open.hResource;
  mach_port_t mach_port;
  if (open.PrivateRuntimeDataSize != sizeof(mach_port_name) || mach_port_name[sizeof(mach_port_name) - 1])
    return {};
  if (!WMTBootstrapLookUp(mach_port_name, &mach_port)) {
    ERR("openSharedEvent: Failed to look up mach port");
    return {};
  }
  WMT::SharedNameHold hold(mach_port_name);
  auto event = mtl.newSharedEventWithMachPort(mach_port);
  hold.keep(event.handle);
  return event;
}

HRESULT
openHandleByName(const WCHAR *name, DWORD access, HANDLE *handle) {
  D3DKMT_OPENNTHANDLEFROMNAME open = {};
  open.dwDesiredAccess = access;
  OBJECT_ATTRIBUTES attr = {};
  attr.Length = sizeof(attr);
  std::wstring buffer;
  UNICODE_STRING name_str;
  sessionName(name, buffer, name_str, attr);
  open.pObjAttrib = &attr;
  if (D3DKMTOpenNtHandleFromName(&open)) {
    WARN(str::format("openHandleByName: Failed to open NT handle from name: ", name));
    return E_INVALIDARG;
  }
  *handle = open.hNtHandle;
  return S_OK;
}

#endif

}; // namespace mullion