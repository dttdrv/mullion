#include "mullion_heartbeat.hpp"
#include "log/log.hpp"
#include "log/trace.hpp"
#include "thread.hpp"
#include "util_env.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <utility>

namespace mullion::heartbeat {

namespace {

using clock = std::chrono::steady_clock;

constexpr auto kIdle = std::chrono::seconds(1);       // how long without a submission counts as idle
constexpr auto kPeriod = std::chrono::seconds(1);     // how often an idle device's memory is asked to stay
constexpr auto kGiveUp = std::chrono::minutes(3);     // how long an idle game keeps its memory wired

std::atomic<int64_t> last_submission{0};

struct State {
  mullion::mutex mutex;
  mullion::condition_variable changed;
  std::map<uint64_t, std::function<void()>> watched;
  uint64_t next = 0;
  bool running = false;
  mullion::thread thread;
};

// the set Keep fills, under its own lock: allocations are made and freed while a D3D12 device's lock is held, and
// that lock is taken under the state's
struct Kept {
  mullion::mutex mutex;
  WMT::Reference<WMT::ResidencySet> set;
  bool dirty = false;
  uint64_t count = 0;
};

Kept &
kept_set() {
  static Kept *instance = new Kept;
  return *instance;
}

// commits the kept set's changes, so allocations taken out of it are freed within a period
void
Settle(bool idle) {
  auto &k = kept_set();
  std::lock_guard lock(k.mutex);
  if (!k.set)
    return;
  if (std::exchange(k.dirty, false))
    k.set.commit();
  if (idle && k.count)
    k.set.requestResidency();
}

State &
state() {
  static State *instance = new State; // left for the process's end, as its thread may still run then
  return *instance;
}

bool
enabled() {
  static const bool on = env::getEnvVar("MULLION_RESIDENCY_HEARTBEAT") != "0";
  return on;
}

void
Beat() {
  env::setThreadName("mullion-residency-heartbeat");
  auto &s = state();
  std::unique_lock lock(s.mutex);
  bool kept = false;
  while (!s.watched.empty()) {
    s.changed.wait_for(lock, kPeriod, [&] { return s.watched.empty(); });
    if (s.watched.empty())
      break;
    auto idle = clock::now() - clock::time_point(clock::duration(last_submission.load(std::memory_order_relaxed)));
    Settle(idle >= kIdle && idle <= kGiveUp);
    if (idle < kIdle || idle > kGiveUp) {
      if (kept && idle > kGiveUp)
        TRACE_IN(Memory, "residency heartbeat: idle for over 3 minutes, memory left to macOS");
      kept = false;
      continue;
    }
    if (!kept)
      TRACE_IN(Memory, "residency heartbeat: GPU idle, keeping ", s.watched.size(), " residency set(s) wired");
    kept = true;
    // under the state's lock, so Forget waits for a set's owner to be done with it
    for (auto &[id, keep] : s.watched)
      keep();
  }
  s.running = false;
}

} // namespace

uint64_t
Watch(std::function<void()> keep_resident) {
  if (!enabled())
    return 0;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (!last_submission.load(std::memory_order_relaxed))
    Submitted();
  uint64_t id = ++s.next;
  s.watched.emplace(id, std::move(keep_resident));
  if (!s.running) {
    s.running = true;
    if (s.thread.joinable())
      s.thread.join();
    s.thread = mullion::thread(Beat);
  }
  return id;
}

void
Forget(uint64_t id) {
  if (!id)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  s.watched.erase(id);
  s.changed.notify_all();
}

void
KeepAllocations(WMT::Device device) {
  if (!enabled())
    return;
  auto &k = kept_set();
  {
    std::lock_guard lock(k.mutex);
    if (k.set)
      return;
    WMT::Reference<WMT::Error> error;
    k.set = device.newResidencySet(0, error);
    if (!k.set) {
      WARN("residency heartbeat: no residency set for D3D11 allocations");
      return;
    }
  }
  // the thread runs while a set is watched; this one's work is done in Settle
  Watch([] {});
}

bool
Keep(WMT::Allocation allocation, uint64_t bytes) {
  if (bytes < (1u << 20) || !allocation)
    return false;
  auto &k = kept_set();
  std::lock_guard lock(k.mutex);
  if (!k.set)
    return false;
  k.set.addAllocations(&allocation, 1);
  k.dirty = true;
  k.count++;
  return true;
}

void
Release(WMT::Allocation allocation) {
  auto &k = kept_set();
  std::lock_guard lock(k.mutex);
  if (!k.set)
    return;
  k.set.removeAllocations(&allocation, 1);
  k.dirty = true;
  k.count--;
}

void
Submitted() {
  last_submission.store(clock::now().time_since_epoch().count(), std::memory_order_relaxed);
}

} // namespace mullion::heartbeat
