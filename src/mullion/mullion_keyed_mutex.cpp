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

#include "mullion_keyed_mutex.hpp"
#include "log/log.hpp"
#include <random>

#ifdef _WIN32
#include <windows.h>
#else
#include "util_win32_compat.h"
#endif

namespace mullion {

static constexpr char shared_mutex_tag[20] = "MULLION_keyedmutex";

void
KeyedMutex::incRef() {
  refcount_.fetch_add(1u, std::memory_order_acquire);
};

void
KeyedMutex::decRef() {
  if (refcount_.fetch_sub(1u, std::memory_order_release) == 1u)
    delete this;
};

#ifdef _WIN32

// one token per instance: a process part and a counter part, so only the very instance that acquired the mutex is
// its owner (recursion is rejected, and only the owner can release), and a destroyed owner is recognized as dead
static uint64_t
makeOwnerToken() {
  static std::atomic<uint32_t> next = {0u};
  return (uint64_t(GetCurrentProcessId()) << 32) | next.fetch_add(1u, std::memory_order_relaxed) + 1u;
}

// whether the instance a token names still exists. an owner that dies without unwinding anything (ExitProcess, a
// crash, a kill) leaves its token in the state and runs no destructor, so only its process can say whether the
// token still means an owner. an answer of "not alive" must never be given lightly: abandonment is sticky and
// grants no ownership again, so a process that cannot be opened for any reason but "there is no such process"
// counts as alive
static bool
ownerProcessAlive(uint64_t token) {
  DWORD pid = DWORD(token >> 32);
  if (!pid)
    return false;
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process)
    return GetLastError() != ERROR_INVALID_PARAMETER;
  DWORD code = 0;
  bool alive = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
  CloseHandle(process);
  return alive;
}

// external: mullion_fence.cpp's shareEvent names its own mach port with it too, as it did before
void
makeUniqueName(char (&out)[54]) {
  std::random_device rd;
  std::mt19937_64 gen(rd());
  std::uniform_int_distribution<uint64_t> dis;

  uint64_t r1 = dis(gen);
  uint64_t r2 = dis(gen);

  std::snprintf(
      out, sizeof(out), "MULLION_shared_%016llx%016llx", static_cast<unsigned long long>(r1),
      static_cast<unsigned long long>(r2)
  );
}

KeyedMutex::~KeyedMutex() {
  if (state_) {
    // "destroying the owner abandons its acquired mutex" (tests/d3d11/d3d11_shared_resource.cpp): an abandoned
    // mutex reports WAIT_ABANDONED to every later acquire and never grants ownership again
    if (InterlockedCompareExchange64(&state_->owner, 0, static_cast<LONG64>(token_)) == static_cast<LONG64>(token_))
      InterlockedExchange(&state_->abandoned, 1);
    state_ = nullptr;
  }
  if (carrier_view_) {
    UnmapViewOfFile(carrier_view_);
    carrier_view_ = nullptr;
  }
  if (mapping_) {
    CloseHandle(mapping_);
    mapping_ = nullptr;
  }
};

// the state of a mutex is the share's own bytes: this maps the carrier out from `state_offset`, under a handle of
// its own, so a mutex never holds the state open through a handle it does not own
Rc<KeyedMutex>
KeyedMutex::mapCarrierState(HANDLE carrier, size_t state_offset) {
  HANDLE mapping = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), carrier, GetCurrentProcess(), &mapping, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
    ERR("KeyedMutex: Failed to keep the carrier open: ", GetLastError());
    return {};
  }
  void *view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, state_offset + sizeof(SharedMutexState));
  if (!view) {
    ERR("KeyedMutex: Failed to map the carrier's state: ", GetLastError());
    CloseHandle(mapping);
    return {};
  }
  auto ret = Rc(new KeyedMutex());
  ret->mapping_ = mapping;
  ret->carrier_view_ = view;
  ret->state_ = reinterpret_cast<SharedMutexState *>(reinterpret_cast<char *>(view) + state_offset);
  return ret;
}

Rc<KeyedMutex>
KeyedMutex::create(WMT::Device device, HANDLE carrier, size_t state_offset) {
  auto event = device.newSharedEvent();
  if (!event) {
    ERR("KeyedMutex: Failed to create shared event.");
    return {};
  }

  mach_port_t mach_port = event.createMachPort();
  if (!mach_port) {
    ERR("KeyedMutex: Failed to create mach port.");
    return {};
  }
  // the state's own name is the carrier's, so the event is what this name is for: every instance of the mutex
  // looks the mach port it orders on up by it
  char name[54];
  makeUniqueName(name);
  if (!WMTBootstrapRegister(name, event.handle, mach_port)) {
    ERR("KeyedMutex: Failed to register mach port.");
    return {};
  }

  auto ret = mapCarrierState(carrier, state_offset);
  if (!ret)
    return {};
  // the creator owns the state of a share: it is zeroed and tagged before any handle to the share exists, as the
  // carrier is written before its publishing handle is handed out
  memset(ret->state_, 0, sizeof(*ret->state_));
  memcpy(ret->state_->tag, shared_mutex_tag, sizeof(shared_mutex_tag));

  memcpy(ret->name_, name, sizeof(name));
  ret->token_ = makeOwnerToken();
  ret->event_ = std::move(event);
  return ret;
}

Rc<KeyedMutex>
KeyedMutex::import(WMT::Device device, HANDLE carrier, size_t state_offset, const char *event_name) {
  auto ret = mapCarrierState(carrier, state_offset);
  if (!ret)
    return {};
  if (memcmp(ret->state_->tag, shared_mutex_tag, sizeof(shared_mutex_tag))) {
    WARN("KeyedMutex: object is not a shared keyed mutex");
    return {};
  }

  snprintf(ret->name_, sizeof(ret->name_), "%s", event_name);
  mach_port_t mach_port = 0;
  if (WMTBootstrapLookUp(event_name, &mach_port)) {
    WMT::SharedNameHold hold(event_name);
    ret->event_ = device.newSharedEventWithMachPort(mach_port);
    hold.keep(ret->event_.handle);
  }
  if (!ret->event_) {
    // the creator's device is gone and its event with it: this instance has no earlier work of the share's to order
    // against, so it orders on an event of its own. the state - the key, its owner and its abandonment - is still
    // the share's, which is what a later open of the share comes back with
    WARN("KeyedMutex: no shared event to order against, this instance orders against its own");
    ret->event_ = device.newSharedEvent();
    ret->orders_shared_ = false;
  }
  if (!ret->event_) {
    ERR("KeyedMutex: Failed to create shared event.");
    return {};
  }
  ret->token_ = makeOwnerToken();
  return ret;
}

HRESULT
KeyedMutex::acquire(KeyedMutexAcquire &&func_acquire, uint64_t key, uint32_t milliseconds) {
  if (!state_)
    return E_FAIL;
  uint64_t deadline = GetTickCount64() + milliseconds;
  // one liveness question per owner a wait sees, so a spinner does not ask it once a turn; the same
  // owner is re-probed while waiting so a death mid-wait is not missed
  uint64_t checked_owner = 0;
  uint64_t last_probe = 0;
  for (;;) {
    LONG64 owner = InterlockedCompareExchange64(&state_->owner, 0, 0);
    if (owner == static_cast<LONG64>(token_))
      return DXGI_ERROR_INVALID_CALL;
    // WAIT_ABANDONED is sticky and grants no ownership: "abandoned mutex released without ownership"
    if (InterlockedCompareExchange(&state_->abandoned, 0, 0))
      return WAIT_ABANDONED;
    // an owner whose process is gone left the token behind and will never release it: the abandonment its
    // destructor would have marked is marked here instead, whether the owner exited or was killed
    // re-probe the same owner while waiting: an owner that dies mid-wait must abandon rather than
    // leave a waiter spinning on a token that will never release (GitHub keyed-mutex residual)
    uint64_t now = GetTickCount64();
    if (owner && (uint64_t(owner) != checked_owner || now - last_probe >= 10)) {
      checked_owner = uint64_t(owner);
      last_probe = now;
      if (!ownerProcessAlive(checked_owner)) {
        InterlockedExchange(&state_->abandoned, 1);
        return WAIT_ABANDONED;
      }
    }
    if (!owner) {
      if (InterlockedCompareExchange64(&state_->owner, static_cast<LONG64>(token_), 0))
        continue;
      // "the initial key is zero and subsequent keys are those released"
      if (state_->current_key == key) {
        // wait for every earlier release's queued work before this instance's queued work runs. an instance whose
        // creator's event is gone has no shared history to wait for, and its own event counts only its own
        // releases, so it waits for what that event has reached and never for a value nothing would signal
        func_acquire(orders_shared_ ? state_->fence_value : local_fence_);
        return S_OK;
      }
      // a key nobody released yet waits for the release that publishes it: "If the mutex is not released within
      // the time-out interval, the method returns", so an acquirer that arrives before its turn finds the key
      // when the other side releases it, and a wrong key with no timeout still fails at once
      InterlockedExchange64(&state_->owner, 0);
    }
    if (!milliseconds || GetTickCount64() >= deadline)
      return WAIT_TIMEOUT;
    Sleep(1);
  }
}

HRESULT
KeyedMutex::release(KeyedMutexRelease &&func_release, uint64_t key) {
  if (!state_)
    return E_FAIL;
  // the release starts by reading the owner without exchanging it: exchanging it here frees the mutex while the key
  // this release is publishing is still the old one, so another instance takes the old key in that window and the
  // exchange below then clears that instance's ownership. the owner is kept until the key is published, and the
  // exchange that ends it only ever clears this instance's own token
  if (InterlockedCompareExchange64(&state_->owner, 0, 0) != static_cast<LONG64>(token_))
    return DXGI_ERROR_INVALID_CALL;
  // "the key may be any UINT64": release publishes the key the next acquire must bring
  state_->current_key = key;
  uint64_t fence = ++state_->fence_value;
  if (InterlockedCompareExchange64(&state_->owner, 0, static_cast<LONG64>(token_)) != static_cast<LONG64>(token_))
    return DXGI_ERROR_INVALID_CALL;
  func_release(fence);
  if (!orders_shared_)
    local_fence_ = fence;
  return S_OK;
}

#else

KeyedMutex::~KeyedMutex() {};

Rc<KeyedMutex>
KeyedMutex::create(WMT::Device device, HANDLE carrier, size_t state_offset) {
  return {};
}

Rc<KeyedMutex>
KeyedMutex::import(WMT::Device device, HANDLE carrier, size_t state_offset, const char *event_name) {
  return {};
}

HRESULT
KeyedMutex::acquire(KeyedMutexAcquire &&func_acquire, uint64_t key, uint32_t milliseconds) {
  return DXGI_ERROR_INVALID_CALL;
}

HRESULT
KeyedMutex::release(KeyedMutexRelease &&func_release, uint64_t key) {
  return DXGI_ERROR_INVALID_CALL;
}

#endif

}; // namespace mullion
