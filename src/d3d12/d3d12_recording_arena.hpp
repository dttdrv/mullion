#pragma once
#include "thread.hpp"
#include <algorithm>
#include <mutex>
#include <optional>
#include <vector>
#include <windef.h>
#include <winbase.h>

namespace dxmt {

// address space a device's command allocators record into, handed out in runs of blocks: a list has room for whatever
// it records, an allocator holds what its lists recorded and no more, and everything recorded is at an offset into
// one piece, which the words and addresses in it rely on. the pages are the system's until they are written. it is
// the device's, not each allocator's: address space counts as a process's committed memory where Wine reports it,
// and an application that sees itself far over its memory stops loading what it shows
class RecordingArena {
public:
  struct Run {
    size_t offset, length;
  };
  // a sixteenth of the process's address space at most
  static constexpr size_t kSize = std::min<size_t>(size_t(1) << 30, SIZE_MAX / 16 + 1);

  void *base = nullptr;

  ~RecordingArena() {
    if (base)
      VirtualFree(base, 0, MEM_RELEASE);
  }

  // blocks are what the system gives address space in
  bool
  Initialize() {
    if (base)
      return true;
    SYSTEM_INFO system;
    GetSystemInfo(&system);
    block_ = system.dwAllocationGranularity;
    used_.assign(kSize / block_, false);
    return (base = VirtualAlloc(nullptr, kSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  }

  // the first whole blocks in one piece that hold `bytes`
  std::optional<Run>
  Acquire(size_t bytes) {
    size_t count = std::max<size_t>(bytes + block_ - 1, block_) / block_;
    std::lock_guard<dxmt::mutex> lock(mutex_);
    for (size_t i = 0, free = 0; i < used_.size(); i++) {
      free = used_[i] ? 0 : free + 1;
      if (free == count) {
        std::fill_n(used_.begin() + (i + 1 - count), count, true);
        return Run{(i + 1 - count) * block_, count * block_};
      }
    }
    return {};
  }

  void
  Release(const Run &run) {
    std::lock_guard<dxmt::mutex> lock(mutex_);
    std::fill_n(used_.begin() + run.offset / block_, run.length / block_, false);
  }

private:
  size_t block_ = 0;
  std::vector<bool> used_;
  dxmt::mutex mutex_;
};

} // namespace dxmt
