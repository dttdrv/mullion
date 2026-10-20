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

static constexpr char shared_mutex_tag[16] = "DXMT_keyedmutex";

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

static void
makeUniqueName(char (&out)[54]) {
  std::random_device rd;
  std::mt19937_64 gen(rd());
  std::uniform_int_distribution<uint64_t> dis;

  uint64_t r1 = dis(gen);
  uint64_t r2 = dis(gen);

  std::snprintf(
      out, 54, "MULLION_shared_resource_%016llx%016llx", static_cast<unsigned long long>(r1),
      static_cast<unsigned long long>(r2)
  );
}

KeyedMutex::~KeyedMutex() {
  if (state_) {
    // "destroying the owner abandons its acquired mutex" (tests/d3d11/d3d11_shared_resource.cpp): an abandoned
    // mutex reports WAIT_ABANDONED to every later acquire and never grants ownership again
    if (InterlockedCompareExchange64(&state_->owner, 0, static_cast<LONG64>(token_)) == static_cast<LONG64>(token_))
      InterlockedExchange(&state_->abandoned, 1);
    UnmapViewOfFile(state_);
    state_ = nullptr;
  }
  if (mapping_) {
    CloseHandle(mapping_);
    mapping_ = nullptr;
  }
};

Rc<KeyedMutex>
KeyedMutex::create(WMT::Device device) {
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
  char name[54];
  makeUniqueName(name);
  if (!WMTBootstrapRegister(name, mach_port)) {
    ERR("KeyedMutex: Failed to register mach port.");
    return {};
  }

  HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SharedMutexState), name);
  if (!mapping) {
    ERR("KeyedMutex: Failed to create shared state: ", GetLastError());
    return {};
  }
  auto state = static_cast<SharedMutexState *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedMutexState)));
  if (!state) {
    ERR("KeyedMutex: Failed to map shared state: ", GetLastError());
    CloseHandle(mapping);
    return {};
  }
  memset(state, 0, sizeof(*state));
  memcpy(state->tag, shared_mutex_tag, sizeof(shared_mutex_tag));

  auto ret = Rc(new KeyedMutex());
  memcpy(ret->name_, name, sizeof(name));
  ret->mapping_ = mapping;
  ret->state_ = state;
  ret->token_ = makeOwnerToken();
  ret->event_ = std::move(event);

  return ret;
}

Rc<KeyedMutex>
KeyedMutex::import(WMT::Device device, const char *name) {
  HANDLE mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
  if (!mapping) {
    ERR("KeyedMutex: Failed to open shared state: ", GetLastError());
    return {};
  }
  auto state = static_cast<SharedMutexState *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedMutexState)));
  if (!state) {
    ERR("KeyedMutex: Failed to map shared state: ", GetLastError());
    CloseHandle(mapping);
    return {};
  }
  if (memcmp(state->tag, shared_mutex_tag, sizeof(shared_mutex_tag))) {
    WARN("KeyedMutex: object is not a shared keyed mutex");
    UnmapViewOfFile(state);
    CloseHandle(mapping);
    return {};
  }

  mach_port_t mach_port = 0;
  if (!WMTBootstrapLookUp(name, &mach_port)) {
    ERR("KeyedMutex: Failed to lookup mach port.");
    UnmapViewOfFile(state);
    CloseHandle(mapping);
    return {};
  }
  auto event = device.newSharedEventWithMachPort(mach_port);
  if (!event) {
    UnmapViewOfFile(state);
    CloseHandle(mapping);
    return {};
  }

  auto ret = Rc(new KeyedMutex());
  snprintf(ret->name_, sizeof(ret->name_), "%s", name);
  ret->mapping_ = mapping;
  ret->state_ = state;
  ret->token_ = makeOwnerToken();
  ret->event_ = std::move(event);
  return ret;
}

HRESULT
KeyedMutex::acquire(KeyedMutexAcquire &&func_acquire, uint64_t key, uint32_t milliseconds) {
  if (!state_)
    return E_FAIL;
  uint64_t deadline = GetTickCount64() + milliseconds;
  for (;;) {
    LONG64 owner = InterlockedCompareExchange64(&state_->owner, 0, 0);
    if (owner == static_cast<LONG64>(token_))
      return DXGI_ERROR_INVALID_CALL;
    // WAIT_ABANDONED is sticky and grants no ownership: "abandoned mutex released without ownership"
    if (InterlockedCompareExchange(&state_->abandoned, 0, 0))
      return WAIT_ABANDONED;
    if (!owner) {
      if (InterlockedCompareExchange64(&state_->owner, static_cast<LONG64>(token_), 0))
        continue;
      // "the initial key is zero and subsequent keys are those released"
      if (state_->current_key == key) {
        // wait for every earlier release's queued work before this instance's queued work runs
        func_acquire(state_->fence_value);
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
  if (InterlockedCompareExchange64(&state_->owner, 0, static_cast<LONG64>(token_)) != static_cast<LONG64>(token_))
    return DXGI_ERROR_INVALID_CALL;
  // "the key may be any UINT64": release publishes the key the next acquire must bring
  state_->current_key = key;
  uint64_t fence = ++state_->fence_value;
  InterlockedExchange64(&state_->owner, 0);
  func_release(fence);
  return S_OK;
}

#else

KeyedMutex::~KeyedMutex() {};

Rc<KeyedMutex>
KeyedMutex::create(WMT::Device device) {
  return {};
}

Rc<KeyedMutex>
KeyedMutex::import(WMT::Device device, const char *name) {
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
