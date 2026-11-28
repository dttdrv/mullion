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

#include "mullion_counter.hpp"
#include "mullion_buffer.hpp"
#include "util_bit.hpp"

namespace mullion {

CounterSet::CounterSet(WMT::Device device, Rc<BufferAllocation> &&allocation) : allocation_(std::move(allocation)) {
  for (unsigned i = 0; i < counters_.size(); i++) {
    counters_[i].init(this, i);
  }
};

void
CounterSet::release() {
  if (live_refs_.fetch_sub(1u, std::memory_order_acq_rel) == 1u)
    delete this;
}

void
CounterSet::recycle(uint32_t index) {
  auto previous = used_counter_.fetch_and(~(1ull << index), std::memory_order_release);
  (void)previous;
  assert(previous & (1ull << index));
  release();
};

Rc<CounterAllocation>
CounterSet::tryAllocate(uint32_t initial_value) {
  while (true) {
    uint64_t used = used_counter_.load(std::memory_order_relaxed);
    uint64_t free_mask = ~used;
    if (free_mask == 0)
      return {};
    uint32_t index = bit::tzcnt(free_mask);
    uint64_t new_used = used | (1ull << index);
    if (used_counter_.compare_exchange_weak(used, new_used, std::memory_order_acquire, std::memory_order_relaxed)) {
      live_refs_.fetch_add(1u, std::memory_order_relaxed);
      auto counter = &counters_[index];
      allocation_->updateContents(counter->offset(), &initial_value, sizeof(initial_value));
      return counter;
    }
  }
};

void
CounterAllocation::free() {
  set_->recycle(index_);
}

BufferAllocation *
CounterAllocation::buffer() {
  return set_->allocation_.ptr();
}

CounterPool::CounterPool(WMT::Device device) : device_(device) {
  buffer_ = new Buffer(kCounterAlignAs * kCounterSetSize, device);
  counter_sets_.push_back(allocateCounterSet());
}

CounterPool::CounterSetPtr
CounterPool::allocateCounterSet() {
  Flags<BufferAllocationFlag> flags;
  flags.set(BufferAllocationFlag::GpuManaged);
  flags.set(BufferAllocationFlag::NoTracking);
  return CounterSetPtr(new CounterSet(device_, buffer_->allocate(flags)));
}

Rc<CounterAllocation>
CounterPool::allocate(uint32_t initial_value) {
  std::lock_guard<mullion::mutex> lock(mutex_);

  size_t count = counter_sets_.size();
  for (size_t i = 0; i < count; i++) {
    size_t idx = next_set_index_ % count;
    if (auto result = counter_sets_[idx]->tryAllocate(initial_value))
      return result;
    next_set_index_++;
  }
  next_set_index_ = count;
  return counter_sets_.emplace_back(allocateCounterSet())->tryAllocate(initial_value);
}

void
CounterPool::updateCounter(const Rc<Counter> &counter, uint32_t new_value) {
  counter->current_ = allocate(new_value);
}

void
Counter::incRef() {
  refcount_.fetch_add(1u, std::memory_order_acquire);
};

void
Counter::decRef() {
  if (refcount_.fetch_sub(1u, std::memory_order_release) == 1u)
    delete this;
};

} // namespace mullion