#include "mullion_memory.hpp"
#include "log/log.hpp"
#include "log/trace.hpp"
#include "thread.hpp"
#include "util_env.hpp"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>

namespace mullion::memory {

uint64_t
Limit(WMT::Device device) {
  return device.recommendedMaxWorkingSetSize() / 20 * 19;
}

bool
OverLimitPanics() {
  static const bool panics = [] {
    uint64_t major = 0;
    WMTGetOSVersion(&major, nullptr, nullptr);
    return major < 27;
  }();
  return panics;
}

namespace {

struct Watched {
  uint64_t budget = 0, pressure = 1;
  std::chrono::steady_clock::time_point asked{};
};

mullion::mutex state_mutex;
Watched watched;

uint64_t
Compute(WMT::Device device, uint64_t &pressure_out) {
  const uint64_t limit = Limit(device);
  static const bool fixed = env::getEnvVar("MULLION_MEMORY_BUDGET") == "fixed";
  uint64_t pressure = 1, available = 0;
  if (!fixed)
    WMTGetMemoryState(&pressure, &available);
  pressure_out = pressure;
  if (pressure >= 4)
    return limit / 2;
  if (pressure >= 2)
    return std::max(limit / 2, std::min(limit / 4 * 3, device.currentAllocatedSize() + available));
  return limit;
}

} // namespace

uint64_t
Budget(WMT::Device device) {
  auto now = std::chrono::steady_clock::now();
  std::lock_guard lock(state_mutex);
  if (!watched.budget || now - watched.asked >= std::chrono::milliseconds(250)) {
    uint64_t pressure;
    uint64_t budget = Compute(device, pressure);
    if (watched.budget && budget != watched.budget)
      TRACE_IN(Memory, "budget ", budget >> 20, " MB at memory pressure ", pressure, " (was ", watched.budget >> 20,
               " MB)");
    watched = {budget, pressure, now};
  }
  return watched.budget;
}

namespace {

struct Events {
  mullion::mutex mutex;
  mullion::condition_variable changed;
  // each event, and whether it is Mullion's own copy (closed when unregistered) or the application's handle
  std::map<uint32_t, std::pair<HANDLE, bool>> events;
  uint32_t next = 0;
  bool running = false;
  WMT::Reference<WMT::Device> device;
  mullion::thread watcher;
};

Events &
events() {
  static Events *instance = new Events; // left for the process's end: the watcher may still run then
  return *instance;
}

void
Watch() {
  env::setThreadName("mullion-memory-budget");
  auto &e = events();
  uint64_t last = 0;
  std::unique_lock lock(e.mutex);
  while (!e.events.empty()) {
    auto device = e.device;
    lock.unlock();
    uint64_t budget = Budget(device);
    lock.lock();
    if (last && budget != last)
      for (auto &[cookie, event] : e.events)
        SetEvent(event.first);
    last = budget;
    e.changed.wait_for(lock, std::chrono::milliseconds(500), [&] { return e.events.empty(); });
  }
  e.running = false;
}

} // namespace

uint32_t
RegisterBudgetEvent(WMT::Device device, void *event) {
  auto &e = events();
  std::lock_guard lock(e.mutex);
  HANDLE copy = nullptr;
  bool own = DuplicateHandle(GetCurrentProcess(), (HANDLE)event, GetCurrentProcess(), &copy, 0, FALSE,
                             DUPLICATE_SAME_ACCESS);
  if (!own)
    copy = (HANDLE)event;
  uint32_t cookie = ++e.next;
  e.events[cookie] = {copy, own};
  SetEvent(copy);
  if (!e.device)
    e.device = device;
  if (!e.running) {
    e.running = true;
    if (e.watcher.joinable())
      e.watcher.join();
    e.watcher = mullion::thread(Watch);
  }
  return cookie;
}

void
UnregisterBudgetEvent(uint32_t cookie) {
  auto &e = events();
  std::lock_guard lock(e.mutex);
  if (auto found = e.events.find(cookie); found != e.events.end()) {
    if (found->second.second)
      CloseHandle(found->second.first);
    e.events.erase(found);
  }
  e.changed.notify_all();
}

bool
Lossy(WMT::Device device, const WMTTextureInfo &info) {
  static const bool on = env::getEnvVar("MULLION_LOSSY_TEXTURES") == "1";
  if (!on)
    return false;
  static const bool family = device.supportsFamily(WMTGPUFamilyApple8);
  if (!family || (info.options & 0xf0) != WMTResourceStorageModePrivate /* the storage mode bits */ ||
      (info.usage & (WMTTextureUsageShaderWrite | WMTTextureUsagePixelFormatView | WMTTextureUsageShaderAtomic)))
    return false;
  // the half and float formats: an integer or normalized texture (IDs, masks, packed data) keeps its values exact
  switch (WMTPixelFormat(ORIGINAL_FORMAT(info.pixel_format))) {
  case WMTPixelFormatR16Float:
  case WMTPixelFormatRG16Float:
  case WMTPixelFormatRGBA16Float:
  case WMTPixelFormatR32Float:
  case WMTPixelFormatRG32Float:
  case WMTPixelFormatRGBA32Float:
    return true;
  default:
    return false;
  }
}

} // namespace mullion::memory
