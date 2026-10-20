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

// the keyed mutex's shared state: one named file mapping per mutex, openable by name from any device or process of
// the session that holds a texture made from it. the GPU ordering travels separately, on the shared event whose
// mach port is registered under the same name (the carrier pattern of d3d11_fence.cpp). the state is the contract
// of IDXGIKeyedMutex: "the initial key is zero and subsequent keys are those released" (Microsoft Learn,
// IDXGIKeyedMutex::AcquireSync), and "a keyed mutex does not support recursive calls to the AcquireSync method"
struct SharedMutexState {
  char tag[16];
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

  static Rc<KeyedMutex> create(WMT::Device device);
  static Rc<KeyedMutex> import(WMT::Device device, const char *name);

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

  char name_[54] = {};
  HANDLE mapping_ = nullptr;
  SharedMutexState *state_ = nullptr;
  uint64_t token_ = 0;
  std::atomic<uint32_t> refcount_ = {0u};
  WMT::Reference<WMT::SharedEvent> event_;
};

} // namespace mullion
