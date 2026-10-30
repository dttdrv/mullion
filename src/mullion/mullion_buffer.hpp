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
#include "mullion_deptrack.hpp"
#include "mullion_residency.hpp"
#include "mullion_allocation.hpp"
#include "rc/util_rc_ptr.hpp"
#include "thread.hpp"
#include "mullion_typed_window.hpp"
#include "util_flags.hpp"
#include "util_svector.hpp"
#include <memory>

namespace mullion {

enum class BufferAllocationFlag : uint32_t {
  GpuReadonly = 0,
  NoTracking = 0,
  GpuPrivate = 1,
  CpuInvisible = 1,
  CpuWriteCombined = 2,
  OwnedByCommandList = 3,
  GpuManaged = 4,
  /* will allocate at least one page of memory and try to suballocate from that */
  SuballocateFromOnePage = 5,
  CpuPlaced = 6,
  AllocatedOnHeap = 7,
};

struct BufferViewKey {
  union {
    struct {
      uint32_t index;
      uint32_t valid_bits;
    };
    uint64_t impl_;
  };

  BufferViewKey() {
    impl_ = 0;
  }
  BufferViewKey(uint64_t impl) {
    impl_ = impl;
  }
  BufferViewKey(uint32_t index, std::nullopt_t ensure_correct_constructor) {
    this->index = index;
    valid_bits = ~0u;
  }
  operator uint64_t() const {
    return impl_;
  }
};

struct BufferViewDescriptor {
  WMTPixelFormat format;
  // a written view: Metal reads some formats' texture buffers only
  bool write = false;
  // the byte range of this view inside the allocation; 0 length is the whole allocation, capped at the Metal max
  uint64_t byte_offset = 0;
  uint64_t byte_length = 0;
};

class Buffer;

struct BufferView {
  WMT::Reference<WMT::Texture> texture;
  uint64_t gpu_resource_id;
  uint64_t suballocation_texel;
  MULLION_RESOURCE_RESIDENCY_STATE residency{};

  BufferView(WMT::Reference<WMT::Texture> &&texture, uint64_t gpu_resource_id, uint64_t suballocation_texel) :
      texture(std::move(texture)),
      gpu_resource_id(gpu_resource_id),
      suballocation_texel(suballocation_texel) {}
};

class BufferAllocation final : public Allocation {
  friend class Buffer;
  friend class DynamicBuffer;

public:

  WMT::Buffer
  buffer() const {
    return obj_;
  }

  Flags<BufferAllocationFlag>
  flags() const {
    return flags_;
  }

  void *
  mappedMemory(uint32_t sub) const noexcept {
    return reinterpret_cast<void *>(reinterpret_cast<std::uintptr_t>(mappedMemory_) + sub * suballocation_size_);
  }

  uint64_t
  gpuAddress() const noexcept {
    return gpuAddress_;
  }

  const WMTBufferInfo &info() const noexcept { return info_; }

  void
  useSuballocation(uint32_t suballocation) noexcept {
    current_suballocation_ = suballocation;
  };

  bool
  hasSuballocatoin(uint32_t suballocation) const noexcept {
    return suballocation < suballocation_count_;
  }

  uint64_t
  currentSuballocationOffset() const noexcept {
    return current_suballocation_ * suballocation_size_;
  }

  uint64_t
  currentSuballocationOffset(uint64_t stride) const noexcept {
    return current_suballocation_ * stride;
  }

  uint32_t currentSuballocation() {
    return current_suballocation_;
  }

  void
  updateContents(uint64_t offset, const void *data, uint64_t length, uint32_t suballocation = 0) noexcept {
    if (likely(mappedMemory_ != nullptr && !flags_.test(BufferAllocationFlag::GpuManaged))) {
      memcpy(reinterpret_cast<char *>(mappedMemory_) + suballocation * suballocation_size_ + offset, data, length);
      return;
    }
    obj_.updateContents(suballocation * suballocation_size_ + offset, data, length);
  }

  MULLION_RESOURCE_RESIDENCY_STATE residencyState;
  uint64_t sparse_tile_bytes = 0;
  uint64_t sparse_tile_count = 0;
  small_vector<GenericAccessTracker, 1> fenceTrackers;

private:
  BufferAllocation(
      WMT::Device device, const WMTBufferInfo &info, Flags<BufferAllocationFlag> flags, uint32_t names,
      std::shared_ptr<void> memory
  );
  BufferAllocation(
      WMT::Device device, const WMTBufferInfo &info, Flags<BufferAllocationFlag> flags, WMT::Heap heap,
      uint64_t heap_offset
  );
  BufferAllocation(
      WMT::Device device, const WMTBufferInfo &info, Flags<BufferAllocationFlag> flags, WMTSparsePageSize page_size
  );
  BufferAllocation(
      WMT::Reference<WMT::Buffer> &&buffer, const WMTBufferInfo &info, Flags<BufferAllocationFlag> flags
  );
  void free();

  BufferAllocation(const BufferAllocation &) = delete;
  BufferAllocation(BufferAllocation &&) = delete;

  std::shared_ptr<void> memory_;
  WMT::Reference<WMT::Buffer> obj_;
  WMTBufferInfo info_;
  uint32_t version_ = 0;
  Flags<BufferAllocationFlag> flags_;
  small_vector<std::unique_ptr<BufferView>, 1> cached_view_;
  void *mappedMemory_;
  uint64_t gpuAddress_;
  uint32_t current_suballocation_ = 0;
  uint32_t suballocation_size_;
  uint32_t suballocation_count_ = 1;

  void * placed_buffer = nullptr;
};

class Buffer {
public:
  void incRef();
  void decRef();

  BufferViewKey createView(BufferViewDescriptor const &);

  constexpr BufferAllocation *
  current() {
    return current_.ptr();
  }

  // names: how many the allocation holds, for a buffer that suballocates; none asks for a page's worth.
  // memory: the application's own pages to wrap, kept for the allocation's life
  Rc<BufferAllocation> allocate(Flags<BufferAllocationFlag> flags, uint32_t names = 0, std::shared_ptr<void> memory = {});
  Rc<BufferAllocation> allocate(Flags<BufferAllocationFlag> flags, WMT::Heap heap, uint64_t heap_offset);
  // a placement sparse buffer, whose tiles are mapped to heaps later
  Rc<BufferAllocation> allocate(Flags<BufferAllocationFlag> flags, WMTSparsePageSize page_size);
  // an allocation of a Metal buffer another API made, at its GPU address and with its CPU mapping or none
  // (D3D11On12's wrapped resources)
  Rc<BufferAllocation>
  wrap(WMT::Reference<WMT::Buffer> &&buffer, const WMTBufferInfo &info, Flags<BufferAllocationFlag> flags);

  Rc<BufferAllocation> rename(Rc<BufferAllocation> &&newAllocation);

  Buffer(uint64_t length, WMT::Device device) : length_(length), device_(device) {}

  WMT::Texture view(BufferViewKey key);
  WMT::Texture view(BufferViewKey key, BufferAllocation *allocation);

  BufferView const &view_(BufferViewKey key); 
  BufferView const &view_(BufferViewKey key, BufferAllocation *allocation);

  MULLION_RESOURCE_RESIDENCY_STATE &residency(BufferViewKey key);
  MULLION_RESOURCE_RESIDENCY_STATE &residency(BufferViewKey key, BufferAllocation *allocation);

  constexpr uint64_t
  length() {
    return length_;
  };

  WMTPixelFormat pixelFormat(BufferViewKey view) const {
    return viewDescriptors_[view.index].format;
  }

private:
  void prepareAllocationViews(BufferAllocation *allocation);

  uint64_t length_;

  Rc<BufferAllocation> current_;
  uint32_t version_ = 0;
  std::atomic<uint32_t> refcount_ = {0u};

  small_vector<BufferViewDescriptor, 1> viewDescriptors_;
  mullion::mutex mutex_;
  WMT::Device device_;
};

struct BufferSlice {
  uint32_t byteOffset = 0;
  uint32_t byteLength = 0;
  uint32_t firstElement = 0;
  uint32_t elementCount = 0;
};

} // namespace mullion