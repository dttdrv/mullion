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

// a shared fence travels as an ordinary handle on any Wine: a file mapping whose contents name its event's mach port,
// which winemetal's bootstrap resolves in any process of the session, so duplicated and inherited handles reopen it.
// its tag refuses every other handle, as OpenSharedFence "only operates on fences" (ID3D11Device5::OpenSharedFence)
struct SharedFenceData {
  char tag[16];
  char mach_port_name[54];
};

static constexpr char shared_fence_tag[16] = "MULLION_fence";

bool
shareEvent(WMT::SharedEvent event, char (&name)[54]) {
  mach_port_t mach_port = event.createMachPort();
  if (!mach_port) {
    ERR("shareEvent: Failed to create mach port for shared fence");
    return false;
  }
  makeUniqueName(name);
  if (!WMTBootstrapRegister(name, mach_port)) {
    ERR("shareEvent: Failed to register mach port for shared fence");
    return false;
  }
  return true;
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
shareHandle(const char (&event_name)[54], const SECURITY_ATTRIBUTES *attributes, const WCHAR *name, HANDLE *handle) {
  if (name && wcslen(name) > MAX_PATH)
    return DXGI_ERROR_INVALID_CALL;
  HANDLE mapping = CreateFileMappingW(
      INVALID_HANDLE_VALUE, const_cast<SECURITY_ATTRIBUTES *>(attributes), PAGE_READWRITE, 0, sizeof(SharedFenceData),
      name
  );
  if (!mapping) {
    ERR("shareHandle: Failed to create shared handle: ", GetLastError());
    return E_FAIL;
  }
  // "If Name matches the name of an existing resource, CreateSharedHandle fails with DXGI_ERROR_NAME_ALREADY_EXISTS"
  // (ID3D12Device::CreateSharedHandle)
  if (name && GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(mapping);
    return DXGI_ERROR_NAME_ALREADY_EXISTS;
  }
  auto data = static_cast<SharedFenceData *>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(SharedFenceData)));
  if (!data) {
    ERR("shareHandle: Failed to write shared handle: ", GetLastError());
    CloseHandle(mapping);
    return E_FAIL;
  }
  memcpy(data->tag, shared_fence_tag, sizeof(shared_fence_tag));
  memcpy(data->mach_port_name, event_name, sizeof(event_name));
  UnmapViewOfFile(data);
  *handle = mapping;
  return S_OK;
}

WMT::Reference<WMT::SharedEvent>
openSharedEvent(WMT::Device mtl, HANDLE handle, char (&event_name)[54]) {
  auto data = handle ? static_cast<const SharedFenceData *>(
                           MapViewOfFile(handle, FILE_MAP_READ, 0, 0, sizeof(SharedFenceData))
                       )
                     : nullptr;
  if (!data) {
    WARN("openSharedEvent: handle is not a shared fence");
    return {};
  }
  bool fence = !memcmp(data->tag, shared_fence_tag, sizeof(shared_fence_tag));
  memcpy(event_name, data->mach_port_name, sizeof(event_name));
  UnmapViewOfFile(data);
  if (!fence || event_name[sizeof(event_name) - 1]) {
    WARN("openSharedEvent: handle is not a shared fence");
    return {};
  }
  mach_port_t mach_port;
  if (!WMTBootstrapLookUp(event_name, &mach_port)) {
    ERR("openSharedEvent: Failed to look up mach port");
    return {};
  }
  return mtl.newSharedEventWithMachPort(mach_port);
}

HRESULT
openHandleByName(const WCHAR *name, DWORD access, HANDLE *handle) {
  // shared names are found in any case: Wine's win32u d3dkmt test, which runs on Windows, opens a resource by its
  // lower-case name. OpenFileMappingW would compare them as they are
  static auto open_section = reinterpret_cast<NTSTATUS(WINAPI *)(HANDLE *, ACCESS_MASK, const OBJECT_ATTRIBUTES *)>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtOpenSection")
  );
  OBJECT_ATTRIBUTES attr = {};
  attr.Length = sizeof(attr);
  attr.Attributes = OBJ_CASE_INSENSITIVE;
  std::wstring buffer;
  UNICODE_STRING name_str;
  sessionName(name, buffer, name_str, attr);
  if (open_section(handle, access, &attr)) {
    WARN(str::format("openHandleByName: Failed to open shared handle: ", name));
    return E_INVALIDARG;
  }
  return S_OK;
}

#endif

}; // namespace mullion