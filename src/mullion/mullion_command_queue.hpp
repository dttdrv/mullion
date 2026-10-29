/*
 * Copyright 2026 Feifan He for CodeWeavers
 * Copyright 2026 Marc-Aurel Zent for CodeWeavers
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
#include "mullion_allocation.hpp"
#include "mullion_capture.hpp"
#include "mullion_command.hpp"
#include "mullion_command_list.hpp"
#include "mullion_context.hpp"
#include "mullion_diag.hpp"
#include "mullion_counter.hpp"
#include "mullion_query.hpp"
#include "mullion_resource_initializer.hpp"
#include "mullion_ring_bump_allocator.hpp"
#include "mullion_statistics.hpp"
#include "log/log.hpp"
#include "thread.hpp"
#include "util_cpu_fence.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

namespace mullion {

template <typename T> class moveonly_list {
public:
  moveonly_list(T *storage, size_t size) : storage(storage), size_(size) {
    for (unsigned i = 0; i < size_; i++) {
      new (storage + i) T();
    }
  }

  moveonly_list(const moveonly_list &copy) = delete;
  moveonly_list(moveonly_list &&move) {
    storage = move.storage;
    move.storage = nullptr;
    size_ = move.size_;
    move.size_ = 0;
  };

  ~moveonly_list() {
    for (unsigned i = 0; i < size_; i++) {
      storage[i].~T();
    }
  };

  T &
  operator[](int index) {
    return storage[index];
  }

  std::span<T>
  span() const {
    return std::span<T>(storage, size_);
  }

  T *
  data() const {
    return storage;
  }

  size_t
  size() const {
    return size_;
  }

private:
  T *storage;
  size_t size_;
};

constexpr uint32_t kCommandChunkCount = 32;

class CommandQueue;

class CommandChunk {
public:
  CommandChunk(const CommandChunk &) = delete; // delete copy constructor

  void *
  allocate_cpu_heap(size_t size, size_t alignment);

  template <CommandWithContext<ArgumentEncodingContext> F>
  void
  emitcc(F &&func) {
    list_enc.emit(std::forward<F>(func), allocate_cpu_heap(list_enc.calculateCommandSize<F>(), 16));
  }

  void
  encode(WMT::CommandBuffer cmdbuf, ArgumentEncodingContext &enc, const bool &cancelled) {
    enc.$$setEncodingContext(
      chunk_id,
      frame_
    );
    auto& statistics = enc.currentFrameStatistics();
    auto t0 = clock::now();
    list_enc.execute(enc);
    if (cancelled)
      return;
    attached_cmdbuf = cmdbuf;
    auto t1 = clock::now();
    readback = diag::profile ? enc.flushCommands<true>(cmdbuf, chunk_id, chunk_event_id)
                            : enc.flushCommands<false>(cmdbuf, chunk_id, chunk_event_id);
    auto t2 = clock::now();
    statistics.encode_prepare_interval += (t1 - t0);
    statistics.encode_flush_interval += (t2 - t1);
  };

  uint64_t chunk_id;
  uint64_t chunk_event_id;
  uint64_t frame_;
  uint64_t signal_frame_latency_fence_;
  // the device was removed when this chunk was submitted: it is neither encoded nor committed, and completes as the
  // work of a device that is gone does
  bool refused = false;
  std::function<void()> completed;
  QueryReadbacks readback;
  uint64_t resource_initializer_event_id;

private:
  CommandQueue *queue;
  WMT::Reference<WMT::CommandBuffer> attached_cmdbuf;
  
  CommandList<ArgumentEncodingContext> list_enc;
  AllocationRefTracking ref_tracker;

  friend class CommandQueue;

public:
  CommandChunk() {}

  void
  reset() noexcept {
    signal_frame_latency_fence_ = ~0ull;
    refused = false;
    completed = {};
    readback = {};
    list_enc.reset();
    ref_tracker.clear();
    attached_cmdbuf = nullptr;
  }
};

class CommandQueue {

private:
  void CommitChunkInternal(CommandChunk &chunk, uint64_t seq);

  uint32_t EncodingThread();

  uint32_t WaitForFinishThread();

  std::atomic_uint64_t ready_for_encode = 1; // we start from 1, so 0 is always coherent
  std::atomic_uint64_t ready_for_commit = 1;
  std::atomic_uint64_t chunk_ongoing = 0;
  CpuFence cpu_coherent;
  CpuFence frame_latency_fence_;
  std::atomic_bool stopped;
  std::atomic_bool draining_ = false;
  uint64_t drain_last_seq_ = 0;
  bool encoding_cancelled_ = false;
  // the encode thread's, which is the one that refuses a submission
  bool refusal_reported_ = false;

  // MULLION_D3D11_FAULT_DEVICE: the tests' way of a fault, a command buffer that completes as one the GPU failed does,
  // so the removal path runs without a GPU fault. Off unless a run asks for it; its value is the run's completed command
  // buffer that completes as failed (1 is the first, and 4 leaves three of them whole, so a device can be made resources
  // on before its frames are lost)
  const unsigned fault_at_ = [] {
    const std::string asked = env::getEnvVar("MULLION_D3D11_FAULT_DEVICE");
    return asked.empty() ? 0u : (unsigned)std::max(1, atoi(asked.c_str()));
  }();
  // the completed command buffers of the run, whichever device made them: a fault is taken once
  static inline std::atomic_uint fault_completions_{0};
  std::atomic<HRESULT> device_fault_{S_OK};
  mullion::mutex device_fault_mutex_;
  std::function<void(HRESULT)> device_fault_callback_;

  std::array<CommandChunk, kCommandChunkCount> chunks;
  uint64_t encoder_seq = 1;
  uint64_t frame_count = 0;
  // DXGI's default (IDXGIDevice1::SetMaximumFrameLatency)
  static constexpr uint32_t kDefaultLatency = 3;
  uint32_t max_latency_ = kDefaultLatency;

  struct ProfileQueue {
    static inline std::atomic_uint64_t queues{0};
    std::string id = str::format("d3d11:", ++queues);
    std::array<diag::ProfileBuffer, kCommandChunkCount> buffers;
  };
  std::unique_ptr<ProfileQueue> profile_ = diag::profile ? std::make_unique<ProfileQueue>() : nullptr;

  mullion::thread encodeThread;
  mullion::thread finishThread;
  WMT::Device device;
  WMT::Reference<WMT::CommandQueue> commandQueue;

  obj_handle_t shared_event_listener;
  mullion::thread event_listener_thread;

  friend class CommandChunk;
  uint64_t
  GetNextEncoderId() {
    return encoder_seq++;
  }

  RingBumpState<StagingBufferBlockAllocator> staging_allocator;
  RingBumpState<GpuPrivateBufferBlockAllocator> copy_temp_allocator;
  RingBumpState<StagingBufferBlockAllocator, kCommandChunkGPUHeapSize> argbuf_allocator;
  RingBumpState<HostBufferBlockAllocator, kCommandChunkCPUHeapSize, mullion::null_mutex> cpu_command_allocator;
  RingBumpState<HostBufferBlockAllocator, 0x1000 /* 4kB */> reftracker_storage_allocator;
  CaptureState capture_state;

public:
  InternalCommandLibrary cmd_library;
  ArgumentEncodingContext argument_encoding_ctx;
  WMT::Reference<WMT::SharedEvent> event;
  std::uint64_t current_event_seq_id = 0;
  FrameStatisticsContainer statistics;
  ResourceInitializer initializer;
  CounterPool counter_pool;
  diag::TimestampSamples timestamp_samples;

  CommandQueue(WMT::Device device);

  ~CommandQueue();

  void Shutdown();
  void Drain();

  // A command buffer the GPU failed loses the device: the reason is the first one, and every later completion leaves it
  // as it was (ID3D12Device::GetDeviceRemovedReason, and D3D11's the same). The API's device tells its application
  // through the callback, set once, which the device clears before it goes away.
  void SetDeviceFaultCallback(std::function<void(HRESULT)> &&callback);
  void ReportDeviceFault(HRESULT reason);
  HRESULT DeviceFaultReason() const { return device_fault_.load(std::memory_order_acquire); }
  bool HasDeviceFault() const { return FAILED(DeviceFaultReason()); }

  void WaitSharedEvent(WMT::SharedEvent event, uint64_t value) {
    while (event.signaledValue() < value) {
      if (stopped.load(std::memory_order_acquire) || draining_.load(std::memory_order_acquire)) {
        encoding_cancelled_ = true;
        return;
      }
      event.waitUntilSignaledValue(value, 100);
    }
  }

  void OnCompletion(uint64_t seq, std::function<void()> &&action) {
    auto &completed = chunks[seq % kCommandChunkCount].completed;
    completed = [previous = std::move(completed), action = std::move(action)] {
      if (previous)
        previous();
      action();
    };
  }


  CommandChunk *
  CurrentChunk() {
    auto id = ready_for_encode.load(std::memory_order_relaxed);
    return &chunks[id % kCommandChunkCount];
  };

  uint64_t
  CoherentSeqId() {
    return cpu_coherent.signaledValue();
  };

  uint64_t
  CurrentSeqId() {
    return ready_for_encode.load(std::memory_order_relaxed);
  };

  uint64_t
  GetNextEventSeqId() {
    return ++current_event_seq_id;
  };

  uint64_t
  GetCurrentEventSeqId() {
    return current_event_seq_id;
  };


  uint64_t
  SignaledEventSeqId() {
    return event.signaledValue();
  };

  obj_handle_t GetSharedEventListener() {
    return shared_event_listener;
  }

  /**
  This is not thread-safe!
  CurrentChunk & CommitCurrentChunk should be called on the same thread

  */
  void CommitCurrentChunk();

  WMTSampleBufferAttachmentInfo ProfilePass(EncoderData *pass, diag::ProfileBuffer *&buffer);

  uint64_t CurrentFrameSeq() {
    return frame_count + 1;
  }

  FrameStatistics& CurrentFrameStatistics() {
    return statistics.at(frame_count);
  }

  void
  PresentBoundary() {
    statistics.compute(frame_count);
    frame_count++;
    statistics.at(frame_count).reset();
    // After present N-th frame (N starts from 1), wait for (N - max_latency)-th frame to finish rendering 
    if (likely(frame_count > max_latency_)) {
      auto t0 = clock::now();
      auto began = diag::profile && frame_latency_fence_.signaledValue() < frame_count - max_latency_ ? diag::Now() : 0;
      frame_latency_fence_.wait(frame_count - max_latency_);
      if (began)
        diag::Waited("latency", began);
      auto t1 = clock::now();
      statistics.at(frame_count).present_latency_interval += (t1 - t0);
    }
    statistics.at(frame_count).latency = max_latency_;
  }

  uint32_t GetMaxLatency() { return max_latency_; }

  // 0 is the default again
  void SetMaxLatency(uint32_t value) { max_latency_ = value ? value : kDefaultLatency; };

  void
  WaitCPUFence(uint64_t seq) {
    auto began = diag::profile && cpu_coherent.signaledValue() < seq ? diag::Now() : 0;
    cpu_coherent.wait(seq);
    if (began)
      diag::Waited("cpu", began);
  };

  std::tuple<WMT::Buffer, uint64_t>
  AllocateStagingBuffer(size_t size, size_t alignment) {
    auto [block, offset] = staging_allocator.allocate(ready_for_encode, cpu_coherent.signaledValue(), size, alignment);
    return {block.buffer, offset};
  }

  std::pair<WMT::Buffer, uint64_t>
  AllocateTempBuffer(uint64_t seq, size_t size, size_t alignment) {
    auto [block, offset] = copy_temp_allocator.allocate(seq, cpu_coherent.signaledValue(), size, alignment);
    return {block.buffer, offset};
  }

  AllocatedTempBufferSlice
  AllocateTempBuffer1(uint64_t seq, size_t size, size_t alignment) {
    auto [block, offset] = copy_temp_allocator.allocate(seq, cpu_coherent.signaledValue(), size, alignment);
    return {block.buffer, offset, block.gpu_address};
  }

  std::tuple<void *, WMT::Buffer, uint64_t>
  AllocateArgumentBuffer(uint64_t seq, size_t size) {
    auto [block, offset] = argbuf_allocator.allocate(seq, cpu_coherent.signaledValue(), size, 64);
    return {ptr_add(block.mapped_address, offset), block.buffer, offset};
  }

  // an argument buffer the GPU reaches by address too: its mapping, buffer, offset and GPU address
  std::tuple<void *, WMT::Buffer, uint64_t, uint64_t>
  AllocateAddressedArgumentBuffer(uint64_t seq, size_t size) {
    auto [block, offset] = argbuf_allocator.allocate(seq, cpu_coherent.signaledValue(), size, 64);
    return {ptr_add(block.mapped_address, offset), block.buffer, offset, block.gpu_address + offset};
  }

  void *
  AllocateCommandData(size_t size, size_t alignment) {
    auto [block, offset] =
        cpu_command_allocator.allocate(ready_for_encode, cpu_coherent.signaledValue(), size, alignment);
    return ptr_add(block.ptr, offset);
  }

  void Retain(uint64_t seq, Allocation *allocation);
};

} // namespace mullion
