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

#include "mullion_buffer.hpp"
#include "mullion_diag.hpp"
#include "mullion_format.hpp"
#include "log/log.hpp"
#include "thread.hpp"
#include "util_likely.hpp"
#include "util_math.hpp"
#include "wsi_platform.hpp"
#include <cassert>
#include <mutex>

namespace mullion {

std::atomic_uint64_t global_buffer_seq = {0};

BufferAllocation::BufferAllocation(
    WMT::Device device, const WMTBufferInfo &info, Flags<BufferAllocationFlag> flags, uint32_t names,
    std::shared_ptr<void> memory
) :
    memory_(std::move(memory)),
    info_(info),
    flags_(flags) {
  // (sub)allocate a minimum of 256B buffer so that texture can be created
  info_.length = std::max(info_.length, 256ull);
  suballocation_size_ = info_.length;
  if (flags_.test(BufferAllocationFlag::SuballocateFromOnePage) && (names || suballocation_size_ <= MULLION_PAGE_SIZE)) {
    suballocation_size_ = align(info_.length, 16);
    info_.length = std::max<uint64_t>(MULLION_PAGE_SIZE, uint64_t(names) * suballocation_size_);
    suballocation_count_ = info_.length / suballocation_size_;
  }
  fenceTrackers.resize(suballocation_count_);
  if (flags_.test(BufferAllocationFlag::CpuPlaced)) {
    placed_buffer = wsi::aligned_malloc(info_.length, MULLION_PAGE_SIZE);
    info_.memory.set(placed_buffer);
  }
  obj_ = device.newBuffer(info_);
  if (diag::profile && obj_)
    diag::Write(diag::Line("buffer", diag::Now(), obj_.handle, info_.length));
  gpuAddress_ = info_.gpu_address;
  mappedMemory_ = info_.memory.get_accessible_or_null();
};

BufferAllocation::BufferAllocation(
    WMT::Device device, const WMTBufferInfo &info, Flags<BufferAllocationFlag> flags, WMT::Heap heap,
    uint64_t heap_offset
) :
    info_(info),
    flags_(flags) {
  info_.length = info_.length;
  suballocation_size_ = info_.length;
  if (flags_.test(BufferAllocationFlag::SuballocateFromOnePage)) {
    assert(0 && "heap-backed buffer doesn't support suballocation");
  }
  fenceTrackers.resize(suballocation_count_);
  if (flags_.test(BufferAllocationFlag::CpuPlaced)) {
    assert(0 && "heap doesn't support placed buffer");
  }
  obj_ = heap.newBuffer(info_, heap_offset);
  if (diag::profile && obj_)
    diag::Write(diag::Line("buffer", diag::Now(), obj_.handle, info_.length));
  gpuAddress_ = info_.gpu_address;
  mappedMemory_ = info_.memory.get_accessible_or_null();
}

BufferAllocation::BufferAllocation(
    WMT::Device device, const WMTBufferInfo &info, Flags<BufferAllocationFlag> flags, WMTSparsePageSize page_size
) :
    info_(info),
    flags_(flags) {
  // whole tiles, which map one by one
  sparse_tile_bytes = device.sparseTileSizeInBytes(page_size);
  info_.length = align(info_.length, sparse_tile_bytes);
  sparse_tile_count = info_.length / sparse_tile_bytes;
  suballocation_size_ = info_.length;
  fenceTrackers.resize(suballocation_count_);
  // and a tile more, which stays mapped (MTLD3D12CommandQueue::Settle)
  obj_ = device.newPlacementSparseBuffer(info_.length + sparse_tile_bytes, info_.options, page_size, gpuAddress_);
  if (diag::profile && obj_)
    diag::Write(diag::Line("buffer", diag::Now(), obj_.handle, info_.length));
  mappedMemory_ = nullptr;
}

BufferAllocation::BufferAllocation(
    WMT::Reference<WMT::Buffer> &&buffer, const WMTBufferInfo &info, Flags<BufferAllocationFlag> flags
) :
    obj_(std::move(buffer)),
    info_(info),
    flags_(flags) {
  suballocation_size_ = info_.length;
  fenceTrackers.resize(suballocation_count_);
  gpuAddress_ = info_.gpu_address;
  mappedMemory_ = info_.memory.get_accessible_or_null();
}

void
BufferAllocation::free() {
  if (placed_buffer) {
    wsi::aligned_free(placed_buffer);
    placed_buffer = nullptr;
  }
  delete this;
}

WMT::Texture
Buffer::view(BufferViewKey key) {
  return view(key, current_.ptr());
};

WMT::Texture
Buffer::view(BufferViewKey key, BufferAllocation *allocation) {
  return view_(key, allocation).texture;
};

BufferView const &
Buffer::view_(BufferViewKey key) {
  return view_(key, current_.ptr());
};

BufferView const &
Buffer::view_(BufferViewKey key, BufferAllocation *allocation) {
  if (unlikely(allocation->version_ != version_)) {
    prepareAllocationViews(allocation);
  }
  return *allocation->cached_view_[key.index];
};

MULLION_RESOURCE_RESIDENCY_STATE &
Buffer::residency(BufferViewKey key) {
  return residency(key, current_.ptr());
}

MULLION_RESOURCE_RESIDENCY_STATE &
Buffer::residency(BufferViewKey key, BufferAllocation *allocation) {
  if (unlikely(allocation->version_ != version_)) {
    prepareAllocationViews(allocation);
  }
  return allocation->cached_view_[key.index]->residency;
}

void
Buffer::prepareAllocationViews(BufferAllocation *allocation) {
  std::unique_lock<mullion::mutex> lock(mutex_);
  for (unsigned version = allocation->version_; version < version_; version++) {
    auto format = viewDescriptors_[version].format;
    auto texel_size = MTLGetTexelSize(format);
    assert(texel_size);
    assert(!(allocation->suballocation_size_ & (texel_size - 1)));
    auto total_length = allocation->suballocation_size_ * allocation->suballocation_count_;
    auto window = typedBufferWindow(
        viewDescriptors_[version].byte_offset,
        viewDescriptors_[version].byte_length ? viewDescriptors_[version].byte_length : total_length, texel_size
    );
    if (window.capped)
      ERR("typed buffer view is past Metal textureBuffer's 2^28: the window is the first 2^28");
    if (window.byte_offset + window.byte_length > total_length)
      window.byte_length = total_length > window.byte_offset ? total_length - window.byte_offset : 0;
    WMTTextureInfo info{};
    info.type = WMTTextureTypeTextureBuffer;
    info.width = window.byte_length / (uint64_t)texel_size;
    if (!info.width)
      info.width = 1;
    info.height = 1;
    info.depth = 1;
    info.array_length = 1;
    info.mipmap_level_count = 1;
    info.sample_count = 1;
    info.pixel_format = format;
    info.options = allocation->info_.options;
    auto usage = WMTTextureUsageShaderRead;
    if (viewDescriptors_[version].write && !allocation->flags().test(BufferAllocationFlag::GpuReadonly)) {
      usage |= WMTTextureUsageShaderWrite;
      if (format == WMTPixelFormatR32Uint || format == WMTPixelFormatR32Sint ||
          (format == WMTPixelFormatRG32Uint && device_.supportsFamily(WMTGPUFamilyApple8))) {
        usage |= WMTTextureUsageShaderAtomic;
      }
    }
    info.usage = usage;

    auto view = allocation->obj_.newTexture(info, window.byte_offset, window.byte_length);

    allocation->cached_view_.push_back(std::make_unique<BufferView>(
        std::move(view), info.gpu_resource_id, allocation->suballocation_size_ / texel_size
    ));
  }
  allocation->version_ = version_;
};

BufferViewKey
Buffer::createView(BufferViewDescriptor const &descriptor) {
  std::unique_lock<mullion::mutex> lock(mutex_);
  unsigned i = 0;
  for (; i < version_; i++) {
    if (viewDescriptors_[i].format == descriptor.format && viewDescriptors_[i].write == descriptor.write &&
        viewDescriptors_[i].byte_offset == descriptor.byte_offset &&
        viewDescriptors_[i].byte_length == descriptor.byte_length) {
      return BufferViewKey(i, std::nullopt);
    }
  }
  viewDescriptors_.push_back(descriptor);
  version_ = version_ + 1;
  return BufferViewKey(i, std::nullopt);;
}

Rc<BufferAllocation>
Buffer::allocate(Flags<BufferAllocationFlag> flags, uint32_t names, std::shared_ptr<void> memory) {
  WMTResourceOptions options = WMTResourceHazardTrackingModeUntracked;
  if (flags.test(BufferAllocationFlag::CpuWriteCombined)) {
    options |= WMTResourceOptionCPUCacheModeWriteCombined;
  }
  if (flags.test(BufferAllocationFlag::CpuInvisible)) {
    options |= WMTResourceStorageModePrivate;
  }
  if (flags.test(BufferAllocationFlag::GpuManaged)) {
    options |= WMTResourceStorageModeManaged;
  }
  WMTBufferInfo info;
  info.memory.set(memory.get());
  info.length = length_;
  info.options = options;
  return new BufferAllocation(device_, info, flags, names, std::move(memory));
};

Rc<BufferAllocation>
Buffer::allocate(Flags<BufferAllocationFlag> flags, WMT::Heap heap, uint64_t heap_offset) {
  WMTResourceOptions options = WMTResourceHazardTrackingModeUntracked;
  if (flags.test(BufferAllocationFlag::CpuWriteCombined)) {
    options |= WMTResourceOptionCPUCacheModeWriteCombined;
  }
  if (flags.test(BufferAllocationFlag::CpuInvisible)) {
    options |= WMTResourceStorageModePrivate;
  }
  if (flags.test(BufferAllocationFlag::GpuManaged)) {
    options |= WMTResourceStorageModeManaged;
  }
  WMTBufferInfo info;
  info.memory.set(0);
  info.length = length_;
  info.options = options;
  flags.set(BufferAllocationFlag::AllocatedOnHeap);
  return new BufferAllocation(device_, info, flags, heap, heap_offset);
};

Rc<BufferAllocation>
Buffer::allocate(Flags<BufferAllocationFlag> flags, WMTSparsePageSize page_size) {
  WMTBufferInfo info;
  info.memory.set(0);
  info.length = length_;
  info.options = WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModePrivate;
  flags.set(BufferAllocationFlag::CpuInvisible);
  return new BufferAllocation(device_, info, flags, page_size);
};

Rc<BufferAllocation>
Buffer::wrap(WMT::Reference<WMT::Buffer> &&buffer, const WMTBufferInfo &info, Flags<BufferAllocationFlag> flags) {
  return buffer ? new BufferAllocation(std::move(buffer), info, flags) : nullptr;
}

Rc<BufferAllocation>
Buffer::rename(Rc<BufferAllocation> &&newAllocation) {
  Rc<BufferAllocation> old = std::move(current_);
  current_ = std::move(newAllocation);
  return old;
}

void
Buffer::incRef() {
  refcount_.fetch_add(1u, std::memory_order_acquire);
};

void
Buffer::decRef() {
  if (refcount_.fetch_sub(1u, std::memory_order_release) == 1u)
    delete this;
};

} // namespace mullion