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
#include "rc/util_rc_ptr.hpp"
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <functional>

namespace mullion {

using KeyedMutexAcquire = std::function<void(uint64_t)>;
using KeyedMutexRelease = std::function<void(uint64_t)>;

// the keyed mutex's shared state: it lives in the carrier of the share the mutex belongs to, past the share's own
// identity, so one object carries both and every handle to the share is a handle to the state - a handle kept
// after every resource of the share is released still names the key, its owner and its abandonment, which is what
// a later open of that handle comes back with (tests/d3d11/d3d11_shared_texture_crossprocess.cpp). the GPU
// ordering travels separately, on the shared event whose mach port is registered under the mutex's own name (the
// carrier pattern of d3d11_fence.cpp). the state is the contract of IDXGIKeyedMutex: "the initial key is zero and
// subsequent keys are those released" (Microsoft Learn, IDXGIKeyedMutex::AcquireSync), and "a keyed mutex does not
// support recursive calls to the AcquireSync method"
struct SharedMutexState {
  char tag[20];
  volatile LONG64 owner;     // the owning instance's token, 0 when the mutex is free
  volatile LONG abandoned;   // set when the owner is destroyed while owning, and never cleared
  uint64_t current_key;      // the key the last release released with
  uint64_t fence_value;      // the value the last release signaled on the shared event
};

class KeyedMutex {
public:
  void incRef();
  void decRef();

  ~KeyedMutex();

  // `carrier` is the mapping that holds the state, `state_offset` bytes into it. each mutex keeps a handle to it
  // and its own view of it, so the state is the share's own bytes and this mutex holds them open under its own
  // handle, for as long as this mutex lives and no longer
  static Rc<KeyedMutex> create(WMT::Device device, HANDLE carrier, size_t state_offset);
  static Rc<KeyedMutex> import(WMT::Device device, HANDLE carrier, size_t state_offset, const char *event_name);

  HRESULT acquire(KeyedMutexAcquire &&func_acquire, uint64_t key, uint32_t milliseconds);
  HRESULT release(KeyedMutexRelease &&func_release, uint64_t key);

  const char *
  name() const {
    return name_;
  }

  WMT::Reference<WMT::SharedEvent>
  sharedEvent() const {
    return event_;
  }

private:
  KeyedMutex() = default;

  // maps the state out of the carrier, under a handle of this mutex's own
  static Rc<KeyedMutex> mapCarrierState(HANDLE carrier, size_t state_offset);

  char name_[54] = {};         // the shared event's bootstrap name, which every instance looks its mach port up by
  HANDLE mapping_ = nullptr;   // the carrier, kept open so the state outlives the mutex that mapped it
  void *carrier_view_ = nullptr;
  SharedMutexState *state_ = nullptr;
  uint64_t token_ = 0;
  // whether the event this mutex orders on is the share's own. an instance whose creator's event is gone makes an
  // event of its own, which counts only its own releases: it waits for what that event has, never for a value
  // nothing would signal
  bool orders_shared_ = true;
  uint64_t local_fence_ = 0;
  std::atomic<uint32_t> refcount_ = {0u};
  WMT::Reference<WMT::SharedEvent> event_;
};

} // namespace mullion
