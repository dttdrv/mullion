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

#pragma once

#include "Metal.hpp"
#include "thread.hpp"
#include "util_d3dkmt.h"
#include <atomic>
#include <cstdint>

namespace mullion {

class Fence {
public:
  void incRef();
  void decRef();

  WMT::SharedEvent
  sharedEvent() const {
    return event_;
  }

  uint64_t
  completedValue() {
    return event_.signaledValue();
  }

  void
  signal(uint64_t value) {
    if (value < last_signaled_)
      reset();
    else if (value == last_signaled_)
      return;
    event_.signalValue(value);
    last_signaled_ = value;
  };

  void
  signal(WMT::CommandBuffer cmdbuf, uint64_t value) {
    if (value < last_signaled_)
      reset();
    else if (value == last_signaled_)
      return;
    cmdbuf.encodeSignalEvent(event_, value);
    last_signaled_ = value;
  }

  void
  wait(uint64_t value, uint64_t timeout = ~0ULL) {
    event_.waitUntilSignaledValue(value, timeout);
  }

  void
  wait(WMT::CommandBuffer cmdbuf, uint64_t value) {
    cmdbuf.encodeWaitForEvent(event_, value);
  }

  void reset();

  Fence(WMT::Device device);

private:
  WMT::Device device_;
  WMT::Reference<WMT::SharedEvent> event_;
  uint64_t last_signaled_ = 0;
  std::atomic<uint32_t> refcount_ = {0u};
};

class EventListener {
public:
  EventListener();
  ~EventListener();

  // `event` is one of Mullion's own, which stays open until the notification has come
  void setEventOnValue(Fence const *fence, HANDLE event, uint64_t value);
  void setEventOnValue(WMT::SharedEvent shared, HANDLE event, uint64_t value);

private:
  obj_handle_t shared_event_listener_;
  mullion::thread event_listener_thread_;
};

#ifdef _WIN32
// sharing of fences (ID3D11Fence and ID3D12Fence, CreateSharedHandle): a D3DKMT allocation of `device` whose private
// runtime data is the registered name of the event's mach port, 0 if one cannot be made
D3DKMT_HANDLE shareEvent(D3DKMT_HANDLE device, WMT::SharedEvent event);
// an NT handle of `allocation`, named `name` among the session's objects when it has a name
HRESULT shareHandle(D3DKMT_HANDLE allocation, const SECURITY_ATTRIBUTES *attributes, DWORD access, const WCHAR *name,
                    HANDLE *handle);
// the event of a handle shareEvent's allocation was shared by, and that allocation's handle in `device`; none if the
// handle shares something else
WMT::Reference<WMT::SharedEvent> openSharedEvent(WMT::Device mtl, D3DKMT_HANDLE device, HANDLE handle,
                                                 D3DKMT_HANDLE &allocation);
// a new NT handle to what is shared under `name` among the session's objects
HRESULT openHandleByName(const WCHAR *name, DWORD access, HANDLE *handle);
#endif

}; // namespace mullion
