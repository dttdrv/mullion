#pragma once
#include "Metal.hpp"
#include "mullion_workarounds.hpp"
#include <cstdint>

namespace mullion::memory {

// the memory an application is told it may use and the most Mullion lets it allocate: 95% of the device's
// recommendedMaxWorkingSetSize, leaving the system its share of unified memory (and macOS 26 its kernel: Admit)
uint64_t Limit(WMT::Device device);

// whether the macOS release faults on going past recommendedMaxWorkingSetSize (macOS 26 and earlier)
bool OverLimitPanics();

// the budget an application is told (DXGI's QueryVideoMemoryInfo): Limit while macOS has memory to spare, and less as
// it runs short, as Windows lowers a budget under memory pressure so a game shrinks its pools before the system pages:
// at warning pressure three quarters of Limit, and no more than what the device holds plus what macOS can still give
// without paging; at critical pressure half of Limit. Asked of macOS at most every 250 ms, since a game polls it each
// frame (MULLION_MEMORY_BUDGET=fixed keeps it at Limit)
uint64_t Budget(WMT::Device device);

// events set when the budget changes (DXGI's RegisterVideoMemoryBudgetChangeNotificationEvent), and once when
// registered, as Windows does. A thread watches the budget while any is registered
uint32_t RegisterBudgetEvent(WMT::Device device, void *event);
void UnregisterBudgetEvent(uint32_t cookie);

// whether an application allocation of `size()` bytes may be made. Before macOS 27, one that takes the device's
// allocated memory past Limit is refused, and the creation fails with E_OUTOFMEMORY as Windows fails one past what a
// GPU has: allocating more than recommendedMaxWorkingSetSize and making it resident panics the kernel there
// (workaround::Id::MemoryLimit). `size` is called only when that applies, as it may ask Metal for the size
template <typename Size>
bool
Admit(WMT::Device device, Size &&size) {
  if (!OverLimitPanics() || !workaround::Enabled(workaround::Id::MemoryLimit))
    return true;
  uint64_t bytes = size();
  if (device.currentAllocatedSize() + bytes <= Limit(device))
    return true;
  workaround::Acted(workaround::Id::MemoryLimit, "an allocation past 95% of the GPU's working set was refused");
  return false;
}

} // namespace mullion::memory
