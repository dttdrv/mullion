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

#include <deque>
#include <functional>
#include <algorithm>
#include "com/com_guid.hpp"
#include "com/com_pointer.hpp"
#include "d3d12_acceleration_structure.hpp"
#include "d3d12_command_allocator.hpp"
#include "d3d12_device.hpp"
#include "d3d12_pageable.hpp"
#include "dxgi_interfaces.h"
#include "log/log.hpp"
#include "util_env.hpp"
#include "mullion_capture.hpp"
#include "mullion_diag.hpp"
#include "mullion_command_constants.hpp"
#include <atomic>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <limits>
#include <span>
#include <type_traits>
#include <sstream>
#include "d3d10_1.h"
#include "d3d11_4.h"

namespace mullion {

constexpr auto kCommandQueueSize = 32u;
const std::string dump_frames = env::getEnvVar("MULLION_D3D12_DUMP_FRAMES"); // as d3d12_device.hpp declares it

// a pass whose every dispatch waits for the one before (a UAV barrier between each two) runs serially, where Metal
// orders the dispatches itself and a barrier costs nothing: a concurrent encoder's barriers take longer than that.
// a pass with two dispatches that may overlap, or an indirect command buffer's, stays concurrent
static bool
DispatchesOverlap(const wmtcmd_base *cmd) {
  bool unordered = false; // a dispatch since the last barrier
  for (; cmd; cmd = static_cast<const wmtcmd_base *>(cmd->next.ptr)) {
    switch (cmd->type) {
    case WMTComputeCommandDispatch:
    case WMTComputeCommandDispatchIndirect:
    case WMTComputeCommandDispatchThreads:
      if (unordered)
        return true;
      unordered = true;
      break;
    case WMTComputeCommandMemoryBarrier:
      unordered = false;
      break;
    case WMTComputeCommandExecuteCommandsInBuffer:
    case WMTComputeCommandExecuteCommandsInBufferIndirect:
    case WMTComputeCommandDispatchPart:
      return true;
    default:
      break;
    }
  }
  return false;
}

const GUID kD3D12CommandQueueDownlevelUUID = {
    0x38a8c5ef, 0x7ccb, 0x4e81, {0x91, 0x4f, 0xa6, 0xe9, 0xd0, 0x72, 0xc4, 0x94}
};

template <bool Metal4>
class MTLD3D12CommandQueueImpl : public MTLD3D12Pageable<MTLD3D12CommandQueue, IMTLSwapChainFactory> {

  D3D12_COMMAND_QUEUE_DESC desc_;

  using MetalCommandBuffer = std::conditional_t<Metal4, WMT::Metal4CommandBuffer, WMT::CommandBuffer>;
  using MetalCommandQueue = std::conditional_t<Metal4, WMT::MTL4CommandQueue, WMT::CommandQueue>;
  WMT::Reference<MetalCommandQueue> queue_;
  WMT::Reference<WMT::SharedEvent> completion_event_;
  // Metal 3: an aliasing barrier's command buffer signals it, and the next waits for it (CommittingScope::OrderAliases)
  WMT::Reference<WMT::SharedEvent> alias_event_;
  uint64_t alias_value_ = 0;
  // Metal 4: the next command buffer's first encoder orders aliased memory too, for a barrier its command buffer ended
  // before an encoder could
  bool order_aliases_ = false;
  // MULLION_D3D12_ALIASES_INSIDE (on unless 0), Metal 4: an aliasing barrier between work of the buffer's one compute
  // encoder, which began with the last, is a barrier inside it (MTL4CommandBuffer_orderAliases), and is encoded on any
  // thread (Parallel). otherwise the next encoder begins with a barrier against all the queue's work before it
  static bool
  AliasesInside() {
    static const bool inside = env::getEnvVar("MULLION_D3D12_ALIASES_INSIDE") != "0";
    return Metal4 && inside;
  }
  WMT::Reference<WMT::Fence> fence_;
  // the clock of the timestamps the queue's work writes: Metal 3 samples nanoseconds, a Metal 4 counter heap ticks
  // (MTLDevice.h queryTimestampFrequency: "the frequency of the GPU timestamp in ticks per second")
  uint64_t timestamp_frequency_ = kGPUTimestampFrequency;
  static uint64_t
  Rescale(uint64_t time, uint64_t to, uint64_t from) {
    return uint64_t((unsigned __int128)time * to / from);
  }

  // MULLION_D3D12_PASS_OVERLAP (on unless 0): a pass waits for the passes before the last ordering point before it,
  // not for those since (EncoderData::overlap). passes from one ordering point to the next are a group, each with a
  // fence of its own from one half of group_fences_, the next group's from the other: a group's passes wait for the
  // fences of the group before, and those waited for theirs. fence_ stands for all work before the last encoder that
  // updated it, which waited for every pass (WaitAll). a group has at most kGroupPasses passes
  static constexpr unsigned kGroupPasses = 8;
  std::array<WMT::Reference<WMT::Fence>, 2 * kGroupPasses> group_fences_;
  // the half of group_fences_ the current group's passes update, how many have, and how many of the group before did
  unsigned group_half_ = 0, group_count_ = 0, previous_count_ = 0;
  // a pass that orders itself after all before it encoded nothing (a list's first, which is no pass of Metal's): the
  // next that does is ordered in its place
  bool order_next_ = false;

  static bool
  PassesOverlap() {
    static const bool overlap = env::getEnvVar("MULLION_D3D12_PASS_OVERLAP") != "0";
    return overlap;
  }

  // what a pass waits for and updates: `waits` before it begins (a render pass's before its vertices), `pixels`
  // before its fragments
  struct PassSync {
    std::array<WMT::Fence, kGroupPasses + 1> waits;
    unsigned wait_count = 0;
    std::array<WMT::Fence, kGroupPasses> pixels;
    unsigned pixel_count = 0;
    WMT::Fence update;
  };

  // whether a pass's encoder surely does something: a fence update in one that Metal may leave out would leave the
  // fence's last update the older one, which a later pass would then wait for instead
  static bool
  Encodes(const EncoderData *pass) {
    switch (pass->type) {
    case EncoderType::Clear:
    case EncoderType::Render:
    case EncoderType::Resolve:
      return true;
    case EncoderType::Blit:
      return pass->stamps || static_cast<const BlitEncoderData *>(pass)->cmd_head.next.ptr;
    case EncoderType::Compute:
    case EncoderType::ComputeParts:
      return pass->stamps || static_cast<const ComputeEncoderData *>(pass)->cmd_head.next.ptr;
    default:
      return false;
    }
  }

  // the next pass's waits and update, in the order the passes are submitted. a pass that may do nothing (and a pass
  // of no encoder) neither waits nor updates: it has no work to order
  PassSync
  NextSync(const EncoderData *pass) {
    PassSync sync;
    // a Metal 4 pass of compute or blit work that waits for every pass before it, as WaitAll's encoders do, is a
    // segment of the buffer's one compute encoder, which barriers order and whose end updates fence_ (closeCompute).
    // one that joins a group is an encoder of its own, as on Metal 3
    bool segment = Metal4 && pass->type != EncoderType::Render && pass->type != EncoderType::Clear &&
                   pass->type != EncoderType::Resolve;
    if (!PassesOverlap()) {
      sync.waits[sync.wait_count++] = fence_;
      if (!segment)
        sync.update = fence_;
      return sync;
    }
    if (!Encodes(pass)) {
      order_next_ |= pass->overlap == PassOverlap::Ordered;
      return sync;
    }
    auto overlap = std::exchange(order_next_, false) ? PassOverlap::Ordered : pass->overlap;
    if (segment && (overlap == PassOverlap::Ordered || !group_count_ || group_count_ == kGroupPasses)) {
      WaitAll([&](WMT::Fence fence) { sync.waits[sync.wait_count++] = fence; });
      return sync;
    }
    sync.waits[sync.wait_count++] = fence_;
    if (overlap == PassOverlap::Ordered || !group_count_ || group_count_ == kGroupPasses) {
      overlap = PassOverlap::Ordered;
      previous_count_ = group_count_;
      group_half_ ^= 1;
      group_count_ = 0;
    }
    for (unsigned i = 0; i < previous_count_; i++)
      sync.waits[sync.wait_count++] = group_fences_[(group_half_ ^ 1) * kGroupPasses + i];
    if (overlap == PassOverlap::Shared)
      for (unsigned i = 0; i < group_count_; i++)
        sync.pixels[sync.pixel_count++] = group_fences_[group_half_ * kGroupPasses + i];
    sync.update = group_fences_[group_half_ * kGroupPasses + group_count_++];
    return sync;
  }

  // an encoder that waits for all work before it: fence_ and the current group's passes, which waited for the group
  // before. it updates fence_, which then stands for them all
  template <typename Wait>
  void
  WaitAll(Wait &&wait) {
    wait(WMT::Fence(fence_));
    for (unsigned i = 0; i < group_count_; i++)
      wait(WMT::Fence(group_fences_[group_half_ * kGroupPasses + i]));
    group_count_ = previous_count_ = 0;
  }
  CaptureState capture_;
  // a Signal and a timestamp resolve wait here for a command buffer the completion thread is done with
  Reached retired_{};

  // Metal fails a command buffer that waits for an event for more than a few seconds, and ignores the queue's
  // later ones, so no command buffer waits: the CPU does. after Wait for a value the fence has yet to reach, the
  // work the application gives the queue is held, and a thread commits it in order once the fence is there
  mullion::mutex held_mutex_;
  mullion::condition_variable held_changed_;
  std::deque<std::function<void()>> held_;
  bool holding_ = false, held_stop_ = false;
  // a Wait's fence has the queue's later command buffers until it is there: they are encoded ahead and committed
  // in order by the signal that brings it. one gate is closed at a time, and is open only once its buffers are
  // committed, so the order is the queue's. a queue that ends behind its Wait cancels: its buffers never run
  struct Gate {
    mullion::mutex mutex;
    std::atomic<bool> closed;
    bool cancelled = false;
    std::vector<std::pair<WMT::Reference<MetalCommandBuffer>, uint64_t *>> buffers;
    // the device, whose residency set changes are committed before each command buffer
    MTLD3D12Device *device = nullptr;

    void
    Commit(MetalCommandBuffer buffer, uint64_t *committed) {
      std::lock_guard<mullion::mutex> lock(mutex);
      closed ? (void)buffers.emplace_back(buffer, committed) : Run(buffer, committed);
    }

    void
    Open(bool cancel = false) {
      std::lock_guard<mullion::mutex> lock(mutex);
      cancelled |= cancel && closed;
      for (auto &[buffer, committed] : std::exchange(buffers, {}))
        Run(buffer, committed);
      closed = false;
      closed.notify_all();
    }

  private:
    void
    Run(MetalCommandBuffer buffer, uint64_t *committed) {
      if (cancelled)
        return;
      // a removed device runs nothing more (D3D12: work submitted to one is dropped): after a GPU hang or fault the
      // application goes on submitting until it sees the removal, and each command buffer can hang the GPU again,
      // which stalls the whole system's graphics. one never committed is retired as a cancelled one is
      if (device && FAILED(device->RemovedReason())) {
        FLIGHT("command buffer %#llx is dropped: the device is removed", buffer.handle);
        return;
      }
      if constexpr (Metal4)
        if (!buffer)
          return;
      if (committed)
        *committed = diag::Now();
      if (device)
        device->CommitResidency();
      FLIGHT_HEAVY(Gpu, "command buffer %#llx commits", buffer.handle);
      buffer.commit();
    }
  };
  std::shared_ptr<Gate> gate_ = std::make_shared<Gate>();
  mullion::thread held_thread_;
  std::atomic<DWORD> held_thread_id_{0};

  // a submission's runs of passes that need nothing but a command buffer (EncodePass) are encoded by several threads
  // at once, the queue's own among them, each into a command buffer of its own, and committed in the queue's order:
  // Metal's encoding of draws is what a frame of many waits for. MULLION_D3D12_ENCODE_THREADS=1 encodes on one.
  static constexpr unsigned kMaxEncodeThreads = 8;
  // commands: a run of fewer is encoded on one thread, and a thread is given no fewer
  // (MULLION_D3D12_PARALLEL_COMMANDS, for tests: both are that number)
  static inline const uint64_t kParallelCommands = [] {
    auto set = env::getEnvVar("MULLION_D3D12_PARALLEL_COMMANDS");
    uint64_t count = 2048;
    std::from_chars(set.data(), set.data() + set.size(), count);
    return count;
  }();
  static inline const uint64_t kChunkCommands = std::min<uint64_t>(kParallelCommands, 512);
  mullion::mutex encode_mutex_;
  mullion::condition_variable encode_changed_, encoded_;
  std::deque<std::function<void()>> encode_jobs_;
  bool encode_stop_ = false;
  std::vector<mullion::thread> encode_threads_;

  static unsigned
  EncodeThreads() {
    static const unsigned threads = [] {
      auto set = env::getEnvVar("MULLION_D3D12_ENCODE_THREADS");
      unsigned count = 0;
      if (std::from_chars(set.data(), set.data() + set.size(), count).ec != std::errc{} || !count)
        count = std::clamp(mullion::thread::hardware_concurrency() / 2, 1u, 4u);
      return std::min(count, kMaxEncodeThreads);
    }();
    return threads;
  }

  void
  EncodeWorker() {
    env::setThreadName("mullion-encode-thread");
    std::unique_lock<mullion::mutex> lock(encode_mutex_);
    for (;;) {
      encode_changed_.wait(lock, [&] { return encode_stop_ || !encode_jobs_.empty(); });
      if (encode_stop_)
        return;
      auto job = std::move(encode_jobs_.front());
      encode_jobs_.pop_front();
      lock.unlock();
      job();
      job = {};
      lock.lock();
    }
  }

  // a pass EncodePass encodes with nothing of the queue's: no timestamps, no resolver words read back, no wait
  static bool
  Parallel(const EncoderData *pass) {
    if (pass->stamps)
      return false;
    switch (pass->type) {
    case EncoderType::Null:
    case EncoderType::Clear:
    case EncoderType::Blit:
    case EncoderType::Compute:
    case EncoderType::Resolve:
      return true;
    case EncoderType::Render:
      return !static_cast<const RenderEncoderData *>(pass)->omitted;
    case EncoderType::ComputeParts:
      return !static_cast<const ComputePartsEncoderData *>(pass)->indirect;
    // a flag of the command buffer's, which EncodeParallel carries to the next where it ends one
    case EncoderType::Settle:
      return AliasesInside();
    default:
      return false;
    }
  }

  // how many commands a pass has: what encoding it costs
  static uint64_t
  Commands(const EncoderData *pass) {
    auto count = [](const void *head) {
      uint64_t n = 0;
      for (auto cmd = static_cast<const wmtcmd_base *>(head); cmd; cmd = static_cast<const wmtcmd_base *>(cmd->next.ptr))
        n++;
      return n;
    };
    switch (pass->type) {
    case EncoderType::Clear:
    case EncoderType::Render: {
      auto data = static_cast<const RenderEncoderData *>(pass);
      return count(&data->cmd_head) + count(&data->before_head);
    }
    case EncoderType::Blit:
      return count(&static_cast<const BlitEncoderData *>(pass)->cmd_head);
    case EncoderType::Compute:
    case EncoderType::ComputeParts:
      return count(&static_cast<const ComputeEncoderData *>(pass)->cmd_head);
    default:
      return 1;
    }
  }

  // what the queue does and where it waits, with MULLION_LOG_LEVEL=trace: the last lines of a hang name it
  template <typename... Args>
  void
  Trace(const Args &...args) {
    if (Logger::logLevel() == LogLevel::Trace)
      TRACE(GetTickCount64(), " ms, queue ", this, " thread ", GetCurrentThreadId(), ": ", args...);
  }

  // true when the queue holds its work back: `work` is then done after what it holds, and `begin` starts holding
  template <typename Work>
  bool
  Hold(Work &&work, bool begin = false) {
    std::unique_lock<mullion::mutex> lock(held_mutex_);
    if (!holding_ && !begin && !gate_->closed)
      return false;
    if (!held_thread_.joinable())
      held_thread_ = mullion::thread([this] { CommitHeld(); });
    holding_ = true;
    held_.emplace_back(std::forward<Work>(work));
    held_changed_.notify_all();
    return true;
  }

  bool
  Holding() {
    std::unique_lock<mullion::mutex> lock(held_mutex_);
    return holding_ || gate_->closed;
  }

  void
  CommitHeld() {
    env::setThreadName("mullion-held-work-thread");
    held_thread_id_ = GetCurrentThreadId();
    std::unique_lock<mullion::mutex> lock(held_mutex_);
    for (;;) {
      held_changed_.wait(lock, [&] { return held_stop_ || !held_.empty(); });
      if (held_stop_)
        return;
      // the deque is the lock's: the work is taken out of it before it runs, and its place stays until it is done
      auto work = std::move(held_.front());
      lock.unlock();
      Trace("held work starts");
      work();
      work = {};
      lock.lock();
      held_.pop_front();
      Trace("held work done, ", held_.size(), " left");
      if (held_.empty()) {
        holding_ = false;
        held_changed_.notify_all();
      }
    }
  }

  // the list the queue records on (LaterEncoderData), and the memory of the last build it recorded there
  Com<ID3D12CommandAllocator> own_allocator_;
  Com<ID3D12GraphicsCommandList4> own_list_;
  std::vector<Com<ID3D12Resource>> own_buffers_;
  // top-level structures deserialized before every structure their instances name was there, which a serialized
  // structure's pointers need not be until it is used (DXR, "..._POSTBUILD_INFO_SERIALIZATION_DESC"): each is built
  // again when they are, unless something else has filled its memory by then
  struct Unresolved {
    D3D12_GPU_VIRTUAL_ADDRESS destination;
    std::shared_ptr<AccelerationStructure> structure;
    std::shared_ptr<const AccelerationStructureInputs> inputs;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS described;
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances;
  };
  std::vector<Unresolved> unresolved_;

  // the process's queues, for MULLION_TRACE_RESOURCE: what each has on the GPU when a tile mapping is made, and for
  // GetDeviceRemovedReason, which reads their command buffers' feedback (SettleCommandQueueFeedback)
  static inline mullion::mutex queues_mutex_;
  static inline std::vector<MTLD3D12CommandQueueImpl *> queues_;

  // Metal 4: command buffers whose work the GPU has finished, and whose fences are signalled, with Metal's feedback
  // still to come (the status, an error and GPU times, which a commit's feedback handler gives some milliseconds
  // after the queue's event): its sequence number with each. an error read in one removes the device, as one read
  // before the fences would have
  mullion::mutex feedback_mutex_;
  std::vector<std::pair<WMT::Reference<MetalCommandBuffer>, uint64_t>> unsettled_;

  // reads what feedback has come, all of it when `wait`
  void
  SettleFeedback(bool wait) {
    std::lock_guard lock(feedback_mutex_);
    std::erase_if(unsettled_, [&](auto &unsettled) {
      auto &[cmdbuf, seq] = unsettled;
      if (wait)
        cmdbuf.waitUntilCompleted();
      auto status = cmdbuf.status();
      if (status == WMTCommandBufferStatusCommitted)
        return false;
      if (status == WMTCommandBufferStatusError) {
        bool first = SUCCEEDED(device_->RemovedReason());
        auto line = str::format(first ? "Device error: " : "", "queue ", this, ", command buffer ", seq,
                                first ? ": " : " fails too: ", cmdbuf.error().description().getUTF8String());
        if (first)
          ERR(line);
        else
          TRACE_IN(Gpu, line);
        device_->LoseDevice();
      }
      return true;
    });
  }

public:
  static void
  SettleAll(MTLD3D12Device *device) {
    std::lock_guard<mullion::mutex> lock(queues_mutex_);
    for (auto queue : queues_)
      if (queue->device_ == device)
        queue->SettleFeedback(true);
  }

private:

  std::string
  QueuesInFlight() {
    std::lock_guard<mullion::mutex> lock(queues_mutex_);
    std::string text;
    for (auto queue : queues_)
      if (queue->device_ == device_)
        text += str::format(text.empty() ? "" : ", ", queue == this ? "this " : "", queue, " ",
                            queue->inflight_cmdbuf_count_.load(), " in flight");
    return text;
  }

  // tile mappings: a Metal 4 queue makes them, once the work before has run and before the work after is committed
  // (Metal 4's mappings hold for the classic queue's work ordered after them)
  WMT::Reference<WMT::MTL4CommandQueue> mapping_queue_;
  Rc<Fence> mapped_;
  uint64_t mapping_value_ = 0;

  // mapping work, in order with the queue's other work: held with it, so the caller goes on to what a held wait
  // waits for. `update` owns what it names
  template <typename Update>
  void
  Map(Update &&update) {
    auto work = [this, update = std::forward<Update>(update)] {
      auto scope = StartCommitting();
      if (!mapping_queue_) {
        mapping_queue_ = device_->GetMTLDevice().newMTL4CommandQueue();
        mapped_ = new Fence(device_->GetMTLDevice());
      }
      Trace("tile mappings: waits for the work before");
      if (!scope.Settle())
        return;
      // nothing more reaches the GPU of a removed device (Gate::Run)
      if (FAILED(device_->RemovedReason()))
        return;
      // the heaps it maps from must be resident by now, as for a command buffer
      device_->CommitResidency();
      TRACE_IN(Resource, "queue ", this, ": tile mapping ", mapping_value_ + 1, " made; queues: ", QueuesInFlight());
      update(mapping_queue_);
      mapping_queue_.signalEvent(mapped_->sharedEvent(), ++mapping_value_);
      Trace("tile mappings: waits for mapping ", mapping_value_);
      mapped_->wait(mapping_value_);
      // a GPU wait orders mapping memory across queues (Metal, Resource synchronization: events)
      scope.Start();
      if constexpr (Metal4)
        if (!scope.inflight->cmdbuf)
          return;
      scope.inflight->cmdbuf.encodeWaitForEvent(mapped_->sharedEvent(), mapping_value_);
      Trace("tile mappings: done");
    };
    if (!Hold(work))
      work();
  }

  // Metal lets go of the memory a resource's unmapped tiles had when the resource next has a tile mapped: until then
  // they read it, and take writes to it (seen of buffers, and of textures that shaders can write). a buffer has a tile
  // past its end for this (BufferAllocation), mapped to a tile of the queue's own. a texture's mapped tile, copied
  // onto itself, is such a mapping and changes none; with none mapped, its first tile is mapped to the queue's tile
  // and unmapped again, which leaves that one tile reading the queue's
  WMT::Reference<WMT::Heap> spare_tile_;
  WMT::Heap
  SpareTile() {
    if (!spare_tile_) {
      WMTHeapInfo info{};
      info.sparse_page_size = device_->GetSparsePageSize();
      info.size = device_->GetMTLDevice().sparseTileSizeInBytes(info.sparse_page_size);
      info.type = WMTHeapTypePlacement;
      spare_tile_ = device_->GetMTLDevice().newHeap(info);
      device_->RegisterResidency(spare_tile_);
    }
    return spare_tile_;
  }
  std::function<void(WMT::MTL4CommandQueue)>
  Settle(const Tiling &tiling, MTLD3D12Resource *resource) {
    if (!tiling.texture)
      return [this, res = Com(resource)](WMT::MTL4CommandQueue queue) {
        auto buffer = res->buffer->current();
        queue.updateMappings(buffer->buffer(), false, SpareTile(), std::vector<WMTSparseBufferMapping>{
            {WMTSparseMappingModeMap, buffer->sparse_tile_count, 1, 0}});
      };
    auto &mapped = resource->mapped_tiles;
    auto bound = std::find(mapped.begin(), mapped.end(), true);
    auto at = tiling.place(bound == mapped.end() ? 0 : bound - mapped.begin());
    if (bound != mapped.end())
      return [res = Com(resource), at](WMT::MTL4CommandQueue queue) {
        auto texture = res->texture->current()->texture();
        queue.copyMappings(texture, texture, true, std::vector<WMTSparseTextureMappingCopy>{
            {at.origin, at.size, at.level, at.slice, at.origin, at.level, at.slice}});
      };
    return [this, res = Com(resource), at](WMT::MTL4CommandQueue queue) mutable {
      for (auto mode : {WMTSparseMappingModeMap, WMTSparseMappingModeUnmap}) {
        at.mode = mode;
        queue.updateMappings(res->texture->current()->texture(), true, SpareTile(), std::vector{at});
      }
    };
  }

  std::atomic_uint64_t inflight_cmdbuf_seq_ = 1;
  std::atomic_uint64_t inflight_cmdbuf_count_ = 0;
  std::atomic_uint64_t inflight_cmdbuf_stop_ = 0;

  std::shared_ptr<GPURecordTotals> record_totals_;
  std::shared_ptr<GPURecordChecks> own_checks_;

  struct InflightCommandBuffer {
    WMT::Reference<MetalCommandBuffer> cmdbuf{};
    WMT::Reference<WMT::Buffer> sparse_mappings{};
    std::shared_ptr<Gate> gate{};
    std::shared_ptr<GPURecordChecks> records{};
    HANDLE semaphore{};
    std::function<void()> completed{};
    std::vector<std::tuple<WMT::Reference<WMT::Buffer>, std::span<const uint32_t>, bool>> omitted{};
    // the timestamp queries the command buffer samples: the device's counter sample buffer it holds until it
    // completes, and where each sample taken is with the queries it is for
    WMT::Reference<WMT::CounterSampleBuffer> samples{};
    std::vector<std::pair<uint64_t, std::vector<std::pair<Com<MTLD3D12QueryHeap>, uint32_t>>>> sampled{};
    // the resolves of timestamps into readback buffers, which the CPU does when the command buffer completes
    struct Resolve {
      Com<MTLD3D12QueryHeap> heap;
      uint32_t first, count;
      Com<ID3D12Resource> readback;
      void *to;
      // how many of the command buffer's samples were taken before it: it reads its queries as they were then
      size_t after;
    };
    std::vector<Resolve> resolves{};
    // the allocators of the lists the command buffer is the last of
    std::vector<Com<MTLD3D12CommandAllocator>> allocators{};
    // MULLION_D3D12_GPU_ERRORS: the passes with indirect commands and where their resolvers keep their largest numbers
    std::vector<std::pair<std::string, const RenderEncoderData::Most *>> most{};
    // how many encoders of each kind it has, and whether it presents: what a report of one that does not complete says
    std::array<uint16_t, size_t(EncoderType::AccelerationStructure) + 1> encoders{};
    bool presents = false;
    diag::DebugGroups groups{};
    std::array<uint32_t, WMTMetal4AccelerationStructure + 1> physical{};
  };

  struct ProfileBuffer {
    uint32_t samples = 0;
    uint64_t committed = 0, cpu_begin = 0, gpu_begin = 0;
    struct Pass {
      const EncoderData *pass;
      uint64_t id;
      EncoderType type;
      uint32_t first, count, last;
      uint64_t encoding;
      std::string event;
    };
    std::vector<Pass> passes;
  };
  struct ProfileQueue {
    static inline std::atomic_uint64_t queues{0};
    uint64_t id = ++queues;
    std::array<ProfileBuffer, kCommandQueueSize> buffers;
  };
  std::unique_ptr<ProfileQueue> profile_ = diag::profile ? std::make_unique<ProfileQueue>() : nullptr;
  // the path of the queue's open events, which only its held work changes and reads (BeginEvent)
  std::string queue_events_;

  std::array<InflightCommandBuffer, kCommandQueueSize> inflight_cmdbuf_pool_;
  mullion::thread inflight_cmdbuf_wait_thread_;

  // a command buffer that does not complete stops the queue with no error from Metal: a thread reports the one the
  // completion thread has waited on for StuckTime(), with what Metal says of it and what is in it. a warning: one
  // list of very many draws takes as long
  static std::chrono::milliseconds
  StuckTime() {
    // MULLION_TRACE_GPU reports a command buffer sooner: after MULLION_TRACE_GPU_HANG_MS, 2 s unless it says
    static const auto time = [] {
      if (!trace::On(trace::Gpu))
        return std::chrono::milliseconds(10'000);
      auto given = env::getEnvVar("MULLION_TRACE_GPU_HANG_MS");
      return std::chrono::milliseconds(given.empty() ? 2000 : std::max(1, std::atoi(given.c_str())));
    }();
    return time;
  }
  mullion::mutex watch_mutex_;
  mullion::condition_variable watch_changed_;
  uint64_t watched_seq_ = 0;
  bool watch_stop_ = false;
  mullion::thread watch_thread_;

  void
  Watch() {
    env::setThreadName("mullion-cmdbuf-watch-thread");
    std::unique_lock<mullion::mutex> lock(watch_mutex_);
    while (!watch_stop_) {
      auto seq = watched_seq_;
      // the same one still, after the time: it is reported once, and the thread sleeps until another is waited on
      if (watch_changed_.wait_for(lock, StuckTime(), [&] { return watch_stop_ || watched_seq_ != seq; }) || !seq)
        continue;
      auto &inflight = inflight_cmdbuf_pool_[seq % kCommandQueueSize];
      std::string encoders;
      for (auto count : inflight.encoders)
        encoders += std::to_string(count) + " ";
      WARN("command buffer ", seq, " of queue ", this, " has not completed for ", StuckTime().count(),
          " ms; Metal's status of it is ", uint64_t(inflight.cmdbuf.status()), " (2 committed, 3 scheduled); encoders by kind: ",
          encoders, inflight.presents ? "and a present" : "");
      FLIGHT_IN(Gpu, "queue %p: command buffer %llu has not completed in %lld ms, status %llu", this, seq,
             int64_t(StuckTime().count()), uint64_t(inflight.cmdbuf.status()));
      WMTFlightDump("a stuck command buffer");
      watch_changed_.wait(lock, [&] { return watch_stop_ || watched_seq_ != seq; });
    }
  }

  // the command buffer the completion thread waits on, 0 for none
  void
  Watched(uint64_t seq) {
    std::lock_guard<mullion::mutex> lock(watch_mutex_);
    watched_seq_ = seq;
    watch_changed_.notify_all();
  }

  mullion::mutex mutex_commit_;

  void
  CommandBufferWaitingThread() {
    env::setThreadName("mullion-cmdbuf-waiting-thread");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    uint64_t internal_seq = 1;
    for (;;) {
      // what feedback has come, without waiting for more: the next command buffer's completion is not held behind it.
      // what is still to come is read at the next one, or by GetDeviceRemovedReason, which waits for it
      if constexpr (Metal4)
        SettleFeedback(false);
      inflight_cmdbuf_seq_.wait(internal_seq, std::memory_order_acquire);
      if (inflight_cmdbuf_stop_.load() == internal_seq)
        break;
      auto &inflight = inflight_cmdbuf_pool_[internal_seq % kCommandQueueSize];

      inflight.gate->closed.wait(true);
      // a cancelled gate's: it never runs, and nothing of the queue's follows it
      if (inflight.cmdbuf.status() < WMTCommandBufferStatusCommitted) {
        for (auto &allocator : inflight.allocators)
          if (allocator)
            allocator->pending--;
        if (inflight.semaphore)
          ReleaseSemaphore(inflight.semaphore, 1, nullptr);
        inflight = {};
        retired_.Reach(internal_seq);
        inflight_cmdbuf_count_.fetch_sub(1, std::memory_order_release);
        inflight_cmdbuf_count_.notify_one();
        internal_seq++;
        continue;
      }
      Trace("waits for command buffer ", internal_seq);
      Watched(internal_seq);
      // Metal 4: the queue's event says the work is done; its feedback may come later, and only the profile, which
      // reads its GPU times, waits for it here
      if constexpr (Metal4)
        profile_ ? inflight.cmdbuf.waitUntilCompleted() : inflight.cmdbuf.waitForGPU();
      else if (profile_ || inflight.cmdbuf.status() <= WMTCommandBufferStatusScheduled)
        inflight.cmdbuf.waitUntilCompleted();
      auto completed = profile_ ? diag::Now() : 0;
      Watched(0);
      Trace("command buffer ", internal_seq, " completed");
      FLIGHT_HEAVY(Gpu, "queue %p: command buffer %llu completes, status %llu", this, internal_seq,
             uint64_t(inflight.cmdbuf.status()));
      device_->CheckAtomicLocks();
      if (record_totals_)
        CompareGPURecords(inflight.records, record_totals_);
      if (inflight.cmdbuf.status() == WMTCommandBufferStatusError) {
        // the first says why; Metal fails what follows a fault of the process's too. MULLION_TRACE_GPU has them all:
        // one Metal calls an innocent victim of a GPU recovery leaves the one at fault to another command buffer
        bool first = SUCCEEDED(device_->RemovedReason());
        if (first || trace::On(trace::Gpu)) {
          auto report = [&](const std::string &line) {
            if (first)
              ERR(line);
            else
              TRACE_IN(Gpu, line);
          };
          report(str::format(first ? "Device error: " : "", "queue ", this, ", command buffer ", internal_seq,
                             first ? ": " : " fails too: ", inflight.cmdbuf.error().description().getUTF8String()));
          for (auto &[label, most] : inflight.most)
            report(str::format("indirect commands of ", label, ": ", most->words[5], " commands of at most ",
                               most->call->max_count, most->call->max_count_buffer ? " with a count buffer" : "",
                               ", arguments at ", (void *)most->call->argument_buffer, "; the most instances ",
                               most->words[0], ", vertices or indices ", most->words[1], ", start ", most->words[2],
                               ", base vertex ", most->words[3], ", start instance ", most->words[4]));
        }
        device_->LoseDevice();
      }

      std::vector<uint64_t> clock(profile_ && profile_->buffers[internal_seq % kCommandQueueSize].samples
                                      ? device_->timestamp_samples.limit
                                      : inflight.sampled.size(),
                                  profile_ ? ~uint64_t(0) : 0);
      if (inflight.samples) {
        inflight.samples.resolveCounterRange(0, clock.size(), clock.data(), clock.size() * sizeof(uint64_t));
        device_->timestamp_samples.Return(std::move(inflight.samples));
      }
      if (profile_) {
        auto &buffer = profile_->buffers[internal_seq % kCommandQueueSize];
        uint64_t cpu_end, gpu_end;
        device_->GetMTLDevice().sampleTimestamps(cpu_end, gpu_end);
        auto lines = diag::Line("cmdbuf", profile_->id, internal_seq, buffer.committed, completed,
                                inflight.cmdbuf.gpuStartTime(), inflight.cmdbuf.gpuEndTime());
        lines += diag::Line("calibration", profile_->id, internal_seq, buffer.cpu_begin, buffer.gpu_begin, cpu_end, gpu_end);
        for (auto &pass : buffer.passes) {
          const char *kind = pass.type == EncoderType::Clear     ? "Clear"
                             : pass.type == EncoderType::Render  ? "Render"
                             : pass.type == EncoderType::Blit    ? "Blit"
                             : pass.type == EncoderType::Compute || pass.type == EncoderType::ComputeParts ? "Compute"
                             : pass.type == EncoderType::Resolve ? "Resolve"
                                                                 : "AccelerationStructure";
          std::string runs;
          auto name = [&](uint64_t pipeline) {
            auto label = device_->PassName(pass.pass, false, pipeline);
            return label.empty() ? str::format(kind, ':', pipeline) : label;
          };
          if (pass.type == EncoderType::Render)
            runs = diag::PipelineRuns(static_cast<const RenderEncoderData *>(pass.pass)->cmd_head, name);
          else if (pass.type == EncoderType::Compute || pass.type == EncoderType::ComputeParts)
            runs = diag::PipelineRuns(static_cast<const ComputeEncoderData *>(pass.pass)->cmd_head, name);
          auto line = diag::Line("pass", profile_->id, internal_seq, pass.id, kind, pass.encoding, runs);
          line.pop_back();
          // the profile's clock is nanoseconds, the queries' the queue's frequency
          for (uint32_t i = 0; i < pass.count; i++) {
            auto time = pass.first == ~0u ? ~uint64_t(0) : clock[i + 1 == pass.count ? pass.last : pass.first + i];
            line += str::format('\t', time == ~uint64_t(0) ? time : Rescale(time, kGPUTimestampFrequency, timestamp_frequency_));
          }
          lines += line + '\n';
          if (!pass.event.empty())
            lines += diag::Line("event", profile_->id, internal_seq, pass.id, pass.event);
          // the attachments of a render or clear pass, as D3D11's profile has them, and how each is loaded and stored
          if (pass.type == EncoderType::Render || pass.type == EncoderType::Clear) {
            auto data = static_cast<const RenderEncoderData *>(pass.pass);
            auto target = [&](const char *binding, unsigned slot, WMT::Texture texture, unsigned level, unsigned slice,
                              WMTLoadAction load, WMTStoreAction store) {
              if (texture)
                lines += diag::Line("target", profile_->id, internal_seq, pass.id, binding, slot, texture.handle,
                                    texture.width(), texture.height(), uint64_t(texture.pixelFormat()), level, slice,
                                    texture.arrayLength(), uint32_t(load), uint32_t(store));
            };
            for (unsigned i = 0; i < data->colors.size(); i++) {
              auto &color = data->colors[i];
              target("color", i, color.attachment.texture(), color.level, color.slice, color.load_action,
                     color.store_action);
            }
            target("depth", 0, data->depth.attachment.texture(), data->depth.level, data->depth.slice,
                   data->depth.load_action, data->depth.store_action);
            target("stencil", 0, data->stencil.attachment.texture(), data->stencil.level, data->stencil.slice,
                   data->stencil.load_action, data->stencil.store_action);
          }
        }
        diag::Write(lines);
        buffer = {};
      }
      // samples and resolves in the order the lists had them: a query sampled again after a resolve is another value
      size_t read = 0;
      auto sample = [&](size_t until) {
        for (; read < until; read++)
          for (auto &[heap, query] : inflight.sampled[read].second)
            heap->timestamps[query] = clock[inflight.sampled[read].first];
      };
      for (auto &resolve : inflight.resolves) {
        sample(resolve.after);
        memcpy(resolve.to, &resolve.heap->timestamps[resolve.first], resolve.count * sizeof(uint64_t));
      }
      sample(inflight.sampled.size());
      uint32_t omitted[2] = {};
      if (inflight.cmdbuf.status() != WMTCommandBufferStatusError)
        for (auto &[buffer, words, scratch] : inflight.omitted)
          for (auto word : words)
            omitted[scratch] = std::max(omitted[scratch], word);
      if (omitted[0])
        WARN("Omitted indirect mesh draw (GS) because of too many object threadgroups (", omitted[0], ")");
      if (omitted[1]) {
        for (auto budget = device_->so_scratch_budget.load();
             omitted[1] > budget && !device_->so_scratch_budget.compare_exchange_weak(budget, omitted[1]);) {
        }
        ERR("Stream output from an indirect command needed ", omitted[1], " bytes of scratch, more than it had: the "
            "command was not run, and lists recorded from now on have that much");
      }
      if constexpr (Metal4)
        if (inflight.cmdbuf.status() == WMTCommandBufferStatusCommitted) {
          std::lock_guard lock(feedback_mutex_);
          unsettled_.emplace_back(inflight.cmdbuf, internal_seq);
        }
      for (auto &allocator : inflight.allocators)
        if (allocator)
          allocator->pending--;
      if (inflight.completed)
        inflight.completed();
      if (inflight.semaphore) {
        if (diag::profile)
          diag::LatencyReleased(inflight.semaphore);
        ReleaseSemaphore(inflight.semaphore, 1, nullptr);
      }

      inflight = {};
      retired_.Reach(internal_seq);

      inflight_cmdbuf_count_.fetch_sub(1, std::memory_order_release);
      inflight_cmdbuf_count_.notify_one();

      internal_seq++;
    }
  };

  struct CommittingScope {
    MTLD3D12CommandQueueImpl *queue;
    std::lock_guard<mullion::mutex> lock;
    InflightCommandBuffer *inflight;
    WMT::Reference<WMT::Object> pool;

    CommittingScope(MTLD3D12CommandQueueImpl *queue) :
        queue(queue), lock(queue->mutex_commit_) {
      Start();
    };

    // a slot is the scope's from here to its retirement: it is waited for and counted under the lock, so two
    // submitters never pass one free slot, and counted before the completion thread hears of it. `encoded` is a
    // command buffer of the queue's encoded already (Metal 3), which the slot then has
    void
    Start(WMT::Reference<MetalCommandBuffer> encoded = {}) {
      queue->inflight_cmdbuf_count_.wait(kCommandQueueSize, std::memory_order_acquire);
      queue->inflight_cmdbuf_count_.fetch_add(1, std::memory_order_relaxed);
      auto seq = queue->inflight_cmdbuf_seq_.load(std::memory_order_relaxed);
      inflight = &queue->inflight_cmdbuf_pool_[seq % kCommandQueueSize];
      pool = WMT::MakeAutoreleasePool();
      if constexpr (Metal4) {
        inflight->cmdbuf = encoded ? std::move(encoded) : queue->NewCommandBuffer(seq);
        if (!inflight->cmdbuf)
          queue->device_->LoseDevice();
        else if (!encoded && queue->order_aliases_)
          inflight->cmdbuf.orderAliases();
      } else {
        inflight->cmdbuf = encoded ? std::move(encoded) : queue->queue_.commandBuffer();
      }
      if (queue->profile_) {
        auto &buffer = queue->profile_->buffers[seq % kCommandQueueSize];
        queue->device_->GetMTLDevice().sampleTimestamps(buffer.cpu_begin, buffer.gpu_begin);
      }
    };

    WMT::Reference<MetalCommandBuffer>
    End() {
      auto ended = inflight->cmdbuf;
      if (diag::events)
        inflight->groups.Enter(ended, {});
      if (diag::trace_encoders) {
        auto &counts = inflight->physical;
        uint32_t render = counts[WMTMetal4Render], compute = counts[WMTMetal4Compute], blit = counts[WMTMetal4Blit];
        if constexpr (Metal4) {
          auto feedback = ended.feedback();
          render = feedback.render_passes;
          compute = feedback.compute_passes;
          blit = 0;
        }
        Logger::info(
            str::format(
                "D3D12 encoders Metal", Metal4 ? 4 : 3, ": render=", render, " compute=", compute, " blit=", blit
            )
        );
      }
      (inflight->gate = queue->gate_)->Commit(
          ended, queue->profile_
                     ? &queue->profile_->buffers[queue->inflight_cmdbuf_seq_.load() % kCommandQueueSize].committed
                     : nullptr);
      FLIGHT_HEAVY(Gpu, "queue %p: command buffer %llu is %#llx", queue, queue->inflight_cmdbuf_seq_.load(), ended.handle);
      inflight = nullptr;
      queue->inflight_cmdbuf_seq_.fetch_add(1, std::memory_order_release);
      queue->inflight_cmdbuf_seq_.notify_one();
      pool = nullptr;
      return ended;
    }

    // what follows sees memory the work so far reached through aliased addresses (a reserved buffer's tiles and a
    // resource placed over the same heap memory): Metal 4 begins the next encoder with a barrier of resource-alias
    // visibility, Metal 3 ends the command buffer and starts the next behind a GPU wait on an MTLSharedEvent it
    // signals; neither waits on the CPU (d3d12_sparse_remap, d3d12_alias_submissions)
    void
    OrderAliases() {
      if constexpr (Metal4) {
        inflight->cmdbuf.orderAliases(AliasesInside());
        // and again at each command buffer after it, until a pass has taken it: one that ends first, or that is only a
        // queue's wait, has no encoder to begin with the barrier
        queue->order_aliases_ = true;
      } else {
        auto value = ++queue->alias_value_;
        inflight->cmdbuf.encodeSignalEvent(queue->alias_event_, value);
        End();
        Start();
        inflight->cmdbuf.encodeWaitForEvent(queue->alias_event_, value);
      }
    }

    // ends the command buffer and waits for it, past the fence a Wait before it names: what the work so far leaves
    // in memory is then there. false when the queue ended behind that Wait
    bool
    Settle() {
      auto ended = End();
      queue->gate_->closed.wait(true);
      if (ended.status() < WMTCommandBufferStatusCommitted)
        return false;
      ended.waitUntilCompleted();
      return true;
    }

    ~CommittingScope() {
      if (inflight)
        End();
    }
  };

  CommittingScope
  StartCommitting() {
    return CommittingScope(this);
  }

  // a Metal 4 command buffer for the slot of sequence number `seq`, whose commit signals that number
  WMT::Reference<MetalCommandBuffer>
  NewCommandBuffer(uint64_t seq) {
    return WMT::Reference<MetalCommandBuffer>(MTL4CommandBuffer_new(
        queue_, fence_, completion_event_, seq, device_->GetGlobalResidencySet(),
        {kCustomBufferArgumentIndex0 + 1, D3D12_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT,
         D3D12_COMMONSHADER_SAMPLER_SLOT_COUNT}
    ));
  }

public:
  ULONG STDMETHODCALLTYPE
  Release() override {
    auto refs = --m_refCount;
    if (!refs) {
      // a queued Present can release the queue's last reference on the thread its destructor joins
      if (held_thread_id_ == GetCurrentThreadId())
        mullion::thread([this] { ReleasePrivate(); }).detach();
      else
        ReleasePrivate();
    }
    return refs;
  }

  MTLD3D12CommandQueueImpl(MTLD3D12Device *pDevice) :
      MTLD3D12Pageable<MTLD3D12CommandQueue, IMTLSwapChainFactory>(pDevice),
      inflight_cmdbuf_wait_thread_([this]() { this->CommandBufferWaitingThread(); }),
      watch_thread_([this] { Watch(); }) {
    std::lock_guard<mullion::mutex> lock(queues_mutex_);
    queues_.push_back(this);
  }

  ~MTLD3D12CommandQueueImpl() {
    {
      std::lock_guard<mullion::mutex> lock(queues_mutex_);
      std::erase(queues_, this);
    }
    if (held_thread_.joinable()) {
      {
        std::unique_lock<mullion::mutex> lock(held_mutex_);
        // the lists given before are committed, unless the queue waits for a fence
        held_changed_.wait(lock, [&] { return held_.empty() || gate_->closed; });
        held_stop_ = true;
        held_changed_.notify_all();
        // a wait the queue will never see the end of
        gate_->Open(true);
      }
      held_thread_.join();
    }
    // the held work's encoding has ended with it
    {
      std::lock_guard<mullion::mutex> lock(encode_mutex_);
      encode_stop_ = true;
      encode_changed_.notify_all();
    }
    for (auto &thread : encode_threads_)
      thread.join();
    std::lock_guard<mullion::mutex> lock(mutex_commit_);
    // the marker first: the completion thread reads it as soon as the sequence moves
    inflight_cmdbuf_stop_.store(inflight_cmdbuf_seq_.load());
    inflight_cmdbuf_seq_.fetch_add(1);
    inflight_cmdbuf_seq_.notify_one();
    inflight_cmdbuf_wait_thread_.join();
    {
      std::lock_guard<mullion::mutex> watch(watch_mutex_);
      watch_stop_ = true;
      watch_changed_.notify_all();
    }
    watch_thread_.join();
    if (record_totals_) {
      ReportGPURecords(record_totals_);
      device_->ReportPipelineTotals();
    }
  }

  HRESULT
  Initialize(const D3D12_COMMAND_QUEUE_DESC *pDesc) {
    if (diag::errors)
      record_totals_ = CreateGPURecordTotals();
    // TODO: validate and normalize
    desc_ = *pDesc;
    desc_.NodeMask = 1; // typically 1 GPU only

    auto metal_device = device_->GetMTLDevice();
    if constexpr (Metal4) {
      queue_ = metal_device.newMTL4CommandQueue();
      completion_event_ = metal_device.newSharedEvent();
      if (!completion_event_)
        return E_OUTOFMEMORY;
      timestamp_frequency_ = MTLDevice_queryTimestampFrequency(metal_device);
    } else {
      // the slots' command buffers, and those encoded ahead of them on the encoding threads
      queue_ = metal_device.newCommandQueue(kCommandQueueSize + kMaxEncodeThreads);
      alias_event_ = metal_device.newSharedEvent();
      if (!alias_event_)
        return E_OUTOFMEMORY;
    }
    if (!queue_)
      return FAIL(E_FAIL);
    if constexpr (!Metal4)
      queue_.addResidencySet(device_->GetGlobalResidencySet());
    gate_->device = device_;

    fence_ = metal_device.newFence();
    for (auto &fence : group_fences_)
      if (!(fence = metal_device.newFence()))
        return E_FAIL;
    // the work before the first present is frame 0, as Present numbers the work after it
    capture_.frame(metal_device, 0);
    if (profile_) {
      UINT64 cpu, gpu;
      GetClockCalibration(&gpu, &cpu);
      diag::Write(diag::Line("clock", diag::Nanoseconds(cpu), Rescale(gpu, kGPUTimestampFrequency, timestamp_frequency_)));
    }

    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return FAIL(E_POINTER);

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12CommandQueue)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (riid == __uuidof(IMTLSwapChainFactory)) {
      *ppvObject = ref_and_cast<IMTLSwapChainFactory>(this);
      return S_OK;
    }

    if (riid == __uuidof(ID3D10Device) || riid == __uuidof(ID3D10Device1))
      return E_NOINTERFACE;

    if (riid == __uuidof(ID3D11Device) || riid == __uuidof(ID3D11Device1) || riid == __uuidof(ID3D11Device2) ||
        riid == __uuidof(ID3D11Device3) || riid == __uuidof(ID3D11Device4) || riid == __uuidof(ID3D11Device5))
      return E_NOINTERFACE;

    if (riid == kD3D12CommandQueueDownlevelUUID)
      return E_NOINTERFACE;

    if (logQueryInterfaceError(__uuidof(ID3D12CommandQueue), riid)) {
      WARN("D3D12CommandQueue: Unknown interface query ", str::format(riid));
    }

    return MTLD3D12Pageable::QueryInterface(riid, ppvObject);
  }

  void STDMETHODCALLTYPE UpdateTileMappings(
      ID3D12Resource *resource, UINT region_count, const D3D12_TILED_RESOURCE_COORDINATE *region_start_coordinates,
      const D3D12_TILE_REGION_SIZE *region_sizes, ID3D12Heap *heap, UINT range_count,
      const D3D12_TILE_RANGE_FLAGS *range_flags, const UINT *heap_range_offsets, const UINT *range_tile_counts,
      D3D12_TILE_MAPPING_FLAGS flags
  ) {
    if (trace::On(trace::Resource)) {
      // where the regions start, and their sizes in tiles: up to 8 of them, every one with heavy
      std::string regions;
      for (UINT region = 0; region < region_count && (region < 8 || trace::Heavy(trace::Resource)); region++)
        regions += str::format(region ? ", " : " (", region_start_coordinates ? region_start_coordinates[region].X : 0u,
                               ",", region_start_coordinates ? region_start_coordinates[region].Y : 0u, ",",
                               region_start_coordinates ? region_start_coordinates[region].Z : 0u, " subresource ",
                               region_start_coordinates ? region_start_coordinates[region].Subresource : 0u, ": ",
                               region_sizes ? region_sizes[region].NumTiles : 0u, " tiles");
      std::string ranges;
      for (UINT range = 0; range < range_count && range < 8; range++)
        ranges += str::format(range ? ", " : " (", "flags ", range_flags ? uint32_t(range_flags[range]) : 0u,
                              " heap tile ", heap_range_offsets ? heap_range_offsets[range] : 0u, ": ",
                              range_tile_counts ? range_tile_counts[range] : 0u, " tiles");
      TRACE_IN(Resource, "queue ", this, ": UpdateTileMappings of ", resource, ", ", region_count, " regions",
               regions.empty() ? "" : regions + (region_count > 8 && !trace::Heavy(trace::Resource) ? ", ...)" : ")"),
               ", ", range_count, " ranges", ranges.empty() ? "" : ranges + (range_count > 8 ? ", ...)" : ")"),
               ", heap ", heap, ", flags ", uint32_t(flags));
    }
    if (trace::Heavy(trace::Resource))
      for (UINT range = 0; range < range_count; range++)
        TRACE_IN(Resource, "  range ", range, ": flags ", range_flags ? uint32_t(range_flags[range]) : 0u, ", heap tile ",
                 heap_range_offsets ? heap_range_offsets[range] : 0u, ", tiles ",
                 range_tile_counts ? range_tile_counts[range] : 0u);
    // D3D12's regions and heap ranges, walked tile by tile together, become Metal operations over runs of tiles along
    // x with consecutive heap tiles. Metal applies them in order, so a tile named twice keeps the last mapping
    Tiling tiling(resource);
    D3D12_TILED_RESOURCE_COORDINATE coordinate{};
    D3D12_TILE_REGION_SIZE size{region_start_coordinates ? 1 : tiling.total};
    D3D12_TILE_RANGE_FLAGS range_flag = D3D12_TILE_RANGE_FLAG_NONE;
    UINT range_size = ~0u, range_offset = 0;
    std::vector<WMTSparseTextureMapping> operations;
    auto &mapped = static_cast<MTLD3D12Resource *>(resource)->mapped_tiles;
    mapped.resize(tiling.texture ? tiling.total : 0);
    for (UINT region = 0, region_tile = 0, range = 0, range_tile = 0; region < region_count && range < range_count;) {
      if (!range_tile) {
        range_flag = range_flags ? range_flags[range] : D3D12_TILE_RANGE_FLAG_NONE;
        // no counts: one range covers the regions, and several are a tile each
        range_size = range_tile_counts ? range_tile_counts[range] : range_count == 1 ? ~0u : 1;
        range_offset = heap_range_offsets ? heap_range_offsets[range] : 0;
      }
      if (!region_tile) {
        coordinate = region_start_coordinates ? region_start_coordinates[region] : coordinate;
        size = region_sizes ? region_sizes[region] : size;
      }
      if (range_flag != D3D12_TILE_RANGE_FLAG_SKIP) {
        auto tile = tiling.index(coordinate, size, region_tile);
        auto op = tiling.place(tile);
        op.mode = range_flag == D3D12_TILE_RANGE_FLAG_NULL ? WMTSparseMappingModeUnmap : WMTSparseMappingModeMap;
        if (tiling.texture)
          mapped[tile] = op.mode == WMTSparseMappingModeMap;
        op.heap_tile = range_flag == D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE ? range_offset : range_offset + range_tile;
        auto *last = operations.empty() ? nullptr : &operations.back();
        if (last && last->mode == op.mode && last->level == op.level && last->slice == op.slice &&
            last->origin.y == op.origin.y && last->origin.z == op.origin.z &&
            last->origin.x + last->size.width == op.origin.x &&
            (op.mode == WMTSparseMappingModeUnmap || last->heap_tile + last->size.width == op.heap_tile))
          last->size.width++;
        else
          operations.push_back(op);
      }
      if (++range_tile == range_size)
        range++, range_tile = 0;
      if (++region_tile == size.NumTiles)
        region++, region_tile = 0;
    }
    if (operations.empty())
      return;
    std::function<void(WMT::MTL4CommandQueue)> settle = [](auto) {};
    if (operations.back().mode == WMTSparseMappingModeUnmap)
      settle = Settle(tiling, static_cast<MTLD3D12Resource *>(resource));
    Map([this, texture = tiling.texture, res = Com(static_cast<MTLD3D12Resource *>(resource)), settle = std::move(settle),
         heap = Com(static_cast<MTLD3D12Heap *>(heap)), operations = std::move(operations)](WMT::MTL4CommandQueue queue) {
      WMT::Object heap_handle = heap != nullptr ? heap->heap : WMT::Heap{};
      if (texture) {
        queue.updateMappings(res->texture->current()->texture(), true, heap_handle, operations);
        settle(queue);
        return;
      }
      // a buffer's tiles are its x
      std::vector<WMTSparseBufferMapping> ranges;
      for (auto &op : operations)
        ranges.push_back({op.mode, op.origin.x, op.size.width, op.heap_tile});
      queue.updateMappings(res->buffer->current()->buffer(), false, heap_handle, ranges);
      settle(queue);
      device_->UpdateSparseBufferMappings(res->buffer->current(), ranges);
    });
  };

  void STDMETHODCALLTYPE CopyTileMappings(
      ID3D12Resource *dst_resource, const D3D12_TILED_RESOURCE_COORDINATE *dst_region_start_coordinate,
      ID3D12Resource *src_resource, const D3D12_TILED_RESOURCE_COORDINATE *src_region_start_coordinate,
      const D3D12_TILE_REGION_SIZE *region_size, D3D12_TILE_MAPPING_FLAGS flags
  ) {
    TRACE_IN(Resource, "queue ", this, ": CopyTileMappings from ", src_resource, " to ", dst_resource, ", ",
             region_size ? region_size->NumTiles : 0u, " tiles, flags ", uint32_t(flags));
    Tiling source(src_resource), destination(dst_resource);
    if (source.texture != destination.texture) {
      ERR("CopyTileMappings: Metal copies mappings between buffers or between textures only");
      return;
    }
    std::vector<WMTSparseTextureMappingCopy> operations;
    auto src = static_cast<MTLD3D12Resource *>(src_resource), dst = static_cast<MTLD3D12Resource *>(dst_resource);
    src->mapped_tiles.resize(source.texture ? source.total : 0);
    dst->mapped_tiles.resize(source.texture ? destination.total : 0);
    const auto mapped = src->mapped_tiles;
    bool unmaps = false;
    for (UINT n = 0; n < region_size->NumTiles; n++) {
      auto from_tile = source.index(*src_region_start_coordinate, *region_size, n);
      auto to_tile = destination.index(*dst_region_start_coordinate, *region_size, n);
      auto from = source.place(from_tile), to = destination.place(to_tile);
      operations.push_back({from.origin, from.size, from.level, from.slice, to.origin, to.level, to.slice});
      if (source.texture)
        unmaps |= !(dst->mapped_tiles[to_tile] = mapped[from_tile]);
    }
    // regions of one resource may overlap, with the result of a copy through a temporary one. Metal copies the tiles
    // in order, so a copy to later tiles goes from its last tile back, as memmove does
    if (src_resource == dst_resource && region_size->NumTiles &&
        destination.index(*dst_region_start_coordinate, *region_size, 0) >
            source.index(*src_region_start_coordinate, *region_size, 0))
      std::reverse(operations.begin(), operations.end());
    // a copy of a tile that is not mapped unmaps; which of a buffer's are is not kept here
    std::function<void(WMT::MTL4CommandQueue)> settle = [](auto) {};
    if (unmaps || !source.texture)
      settle = Settle(destination, dst);
    Map([this, texture = source.texture, src = Com(src), dst = Com(dst), settle = std::move(settle),
         operations = std::move(operations)](WMT::MTL4CommandQueue queue) {
      if (texture) {
        queue.copyMappings(src->texture->current()->texture(), dst->texture->current()->texture(), true, operations);
        settle(queue);
        return;
      }
      std::vector<WMTSparseBufferMappingCopy> ranges;
      for (auto &op : operations)
        ranges.push_back({op.origin.x, 1, op.destination_origin.x});
      queue.copyMappings(src->buffer->current()->buffer(), dst->buffer->current()->buffer(), false, ranges);
      settle(queue);
      device_->CopySparseBufferMappings(src->buffer->current(), dst->buffer->current(), ranges);
    });
  };

  void STDMETHODCALLTYPE
  ExecuteCommandLists(UINT Count, ID3D12CommandList *const *ppCommandLists) {
    for (UINT i = 0; i < Count; i++)
      if (static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[i])->encoder_count ==
          std::numeric_limits<size_t>::max()) {
        device_->LoseDevice(DXGI_ERROR_INVALID_CALL);
        return;
      }
    // what the lists recorded, which is theirs no longer: the application may record them again at once
    std::vector<EncoderData *> recorded(Count);
    std::vector<Com<MTLD3D12CommandAllocator>> allocators(Count);
    for (UINT i = 0; i < Count; i++) {
      auto list = static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[i]);
      recorded[i] = list->entry;
      if ((allocators[i] = list->recorded_on))
        allocators[i]->pending++;
    }
    Trace("ExecuteCommandLists of ", Count);
    FLIGHT_HEAVY(Sync, "queue %p: ExecuteCommandLists of %llu", this, uint64_t(Count));
    auto work = [this, recorded = std::move(recorded), allocators = std::move(allocators)]() mutable {
      auto began = profile_ ? diag::Now() : 0;
      Commit(recorded, std::move(allocators));
      if (profile_)
        diag::Write(diag::Line("span", "execute", recorded.size(), GetCurrentThreadId(), began, diag::Now() - began));
    };
    // the queue's thread encodes the lists, so the application's thread goes on recording
    Hold(work, true);
    Trace("ExecuteCommandLists returns");
  }

  void
  Commit(const std::vector<EncoderData *> &recorded, std::vector<Com<MTLD3D12CommandAllocator>> &&allocators = {}) {
    auto scope = StartCommitting();
    // the lists' allocators stay, and count the lists as pending, until the last command buffer of theirs completes
    struct Keeps {
      CommittingScope &scope;
      std::vector<Com<MTLD3D12CommandAllocator>> allocators;
      ~Keeps() { scope.inflight->allocators = std::move(allocators); }
    } keeps{scope, std::move(allocators)};
    if constexpr (Metal4)
      if (!scope.inflight->cmdbuf)
        return;
    WMT::Reference<WMT::Buffer> sparse_mappings;
    auto mappings = device_->SparseBufferMappings();
    // another queue's mappings change the table, and a Wait may be for them: it is read past the Wait's fence
    if (!mappings.empty() && gate_->closed) {
      gate_->closed.wait(true);
      mappings = device_->SparseBufferMappings();
    }
    if (!mappings.empty()) {
      WMTBufferInfo info{mappings.size() * sizeof(mappings[0]), WMTResourceStorageModeShared};
      sparse_mappings = device_->GetMTLDevice().newBuffer(info);
      if (!sparse_mappings) {
        device_->LoseDevice();
        return;
      }
      sparse_mappings.updateContents(0, mappings.data(), info.length);
    }
    static const bool isolate = !env::getEnvVar("MULLION_D3D12_ISOLATE").empty();
    MetalCommandBuffer cmdbuf = scope.inflight->cmdbuf;
    uint64_t queries = 0;
    if (profile_)
      for (auto list : recorded)
        for (auto pass = list; pass; pass = pass->next)
          queries += !!pass->stamps;
    // ends the command buffer and waits for it: what the work so far leaves in memory is then there
    auto settle = [&] {
      std::shared_ptr<GPURecordChecks> checks;
      if (record_totals_)
        checks = scope.inflight->records;
      scope.Settle();
      if (record_totals_)
        CompareGPURecords(checks, record_totals_);
      scope.Start();
      cmdbuf = scope.inflight->cmdbuf;
    };
    // whether the queue has run a build or a copy into the structure at the address
    auto built = [&](D3D12_GPU_VIRTUAL_ADDRESS address) {
      auto structure = device_->LookupAccelerationStructure(address);
      return structure && structure->built;
    };
    auto resolved = [&](const std::vector<D3D12_RAYTRACING_INSTANCE_DESC> &instances) {
      return std::ranges::all_of(instances, [&](auto &instance) {
        return !instance.AccelerationStructure || built(instance.AccelerationStructure);
      });
    };
    // how deep in the queue's own list the passes are
    unsigned own = 0;
    // a list's passes, and those of the list the queue records on
    auto encode = [&](auto &encode, EncoderData *current, MTLD3D12CommandAllocator *allocator) -> void {
      auto records = record_totals_
                         ? static_cast<MTLD3D12CommandAllocatorImpl *>(allocator)->GPURecordsFor(current) : nullptr;
      // records commands on the queue's list, which is free once what it recorded before has run, as are that
      // recording's buffers, and encodes them here
      auto record = [&](auto &&commands) {
        if (own_list_) {
          // the queue's waits completed its last recording; the completion thread may not have compared it
          if (record_totals_)
            CompareGPURecords(own_checks_, record_totals_);
          if (HRESULT hr = own_allocator_->Reset(); FAILED(hr)) {
            ERR("CommandQueue: private allocator Reset failed: ", hr);
            return;
          }
          if (FAILED(own_list_->Reset(own_allocator_.ptr(), nullptr)))
            own_list_ = nullptr;
        }
        if (!own_allocator_ &&
            FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&own_allocator_))))
          return;
        if (!own_list_ && FAILED(device_->CreateCommandList(
                             0, D3D12_COMMAND_LIST_TYPE_DIRECT, own_allocator_.ptr(), nullptr, IID_PPV_ARGS(&own_list_)
                         ))) {
          return;
        }
        auto list = static_cast<MTLD3D12GraphicsCommandList *>(own_list_.ptr());
        list->leaves_for_later = false;
        own_buffers_.clear();
        bool recorded = commands(list);
        if (HRESULT hr = list->Close(); FAILED(hr)) {
          ERR("CommandQueue: private list Close failed: ", hr);
          return;
        }
        if (!recorded)
          return;
        own++;
        encode(encode, list->entry, static_cast<MTLD3D12CommandAllocator *>(own_allocator_.ptr()));
        if (record_totals_)
          own_checks_ = scope.inflight->records;
        own--;
      };
      const uint32_t *decided = nullptr;
      while (current) {
        // with MULLION_D3D12_ISOLATE set every pass is a command buffer that is waited for, and the trace names each:
        // the last one named is the one the GPU does not finish
        if (isolate) {
          settle();
          Trace("pass ", current->id, " of kind ", uint32_t(current->type), " starts");
        }
        // indirect dimensions and bindings are read only after their resolver has finished
        if (current->type == EncoderType::ComputeParts && static_cast<ComputePartsEncoderData *>(current)->indirect)
          settle();
        if (record_totals_ && current->type != EncoderType::Render && current->type != EncoderType::Compute)
          SnapshotGPURecords(records, current, "unlabelled", scope.inflight->records);
        scope.inflight->encoders[size_t(current->type)]++;
        WMTSampleBufferAttachmentInfo attachment{};
        typename ProfileBuffer::Pass *profiled = nullptr;
        auto &pool = device_->timestamp_samples;
        // a command buffer has one buffer of samples: when it is full the next begins
        if (current->stamps && scope.inflight->samples && scope.inflight->sampled.size() == pool.limit) {
          scope.End();
          scope.Start();
          cmdbuf = scope.inflight->cmdbuf;
        }
        std::string event;
        if (diag::events)
          scope.inflight->groups.Enter(cmdbuf, event = queue_events_ + (current->event_path ? current->event_path : ""));
        if (profile_ &&
            (current->type == EncoderType::Clear || current->type == EncoderType::Render ||
             current->type == EncoderType::Blit || current->type == EncoderType::Compute ||
             current->type == EncoderType::ComputeParts ||
             current->type == EncoderType::Resolve || current->type == EncoderType::AccelerationStructure)) {
          auto &buffer = profile_->buffers[inflight_cmdbuf_seq_.load() % kCommandQueueSize];
          if (!scope.inflight->samples && buffer.passes.empty())
            scope.inflight->samples = pool.Take(device_->GetMTLDevice(), false, Metal4);
          auto count = current->type == EncoderType::Render ? WMTRenderTimestampSamples : WMTTimestampSamplesPerStage;
          uint32_t first = ~0u;
          // queries keep their slots; extra profile passes stay unsampled instead of splitting a command buffer
          if (scope.inflight->samples &&
              count <= pool.limit - buffer.samples -
                           std::min<uint64_t>(scope.inflight->sampled.size() + queries, pool.limit)) {
            buffer.samples += count;
            first = pool.limit - buffer.samples;
            attachment = {scope.inflight->samples, first, first + count - 1};
          }
          buffer.passes.push_back({current, current->id, current->type, first, uint32_t(count),
                                   uint32_t(attachment.end_of_encoder_sample_index), diag::Now(), event});
          profiled = &buffer.passes.back();
        }
        if (current->stamps) {
          if (profile_)
            queries--;
          if (!scope.inflight->samples && !(scope.inflight->samples = pool.Take(device_->GetMTLDevice(), true, Metal4))) {
            ERR("EndQuery: Metal has no counter sample buffer for a timestamp");
          } else {
            // the profile's sample of the pass's end is the queries' too
            if (!attachment.sample_buffer)
              attachment = {scope.inflight->samples, ~0ull /* MTLCounterDontSample */, scope.inflight->sampled.size()};
            auto &[slot, taken] = scope.inflight->sampled.emplace_back();
            slot = attachment.end_of_encoder_sample_index;
            for (auto stamp = current->stamps; stamp; stamp = stamp->next)
              taken.push_back({stamp->heap, stamp->query});
          }
        }
        // what a pass of Metal's waits for and updates, in the order the passes are submitted
        PassSync sync;
        switch (current->type) {
        case EncoderType::Clear:
        case EncoderType::Render:
        case EncoderType::Blit:
        case EncoderType::Compute:
        case EncoderType::ComputeParts:
        case EncoderType::Resolve:
          sync = NextSync(current);
          break;
        default:
          break;
        }
        switch (current->type) {
        case EncoderType::Null:
          break;
        case EncoderType::Clear:
        case EncoderType::Render:
        case EncoderType::Blit:
        case EncoderType::Compute:
        case EncoderType::ComputeParts:
        case EncoderType::Resolve:
          if (!EncodePass(cmdbuf, current, attachment, sparse_mappings, *scope.inflight, records, sync))
            return;
          // its encoder began with an aliasing barrier the queue carried (OrderAliases)
          order_aliases_ = false;
          break;
        case EncoderType::AccelerationStructure: {
          // which structures are built is the device's, and another queue's builds change it
          gate_->closed.wait(true);
          auto data = static_cast<AccelerationStructureEncoderData *>(current);
          for (auto filled = data->filled; filled; filled = filled->next) {
            auto &structure = *filled->structure;
            structure.built = true;
            if (filled->kept)
              structure.inputs = filled->kept->shared_from_this();
            else if (filled->from)
              structure.inputs = filled->from->inputs;
            else
              structure.inputs = nullptr;
          }
          if (diag::trace_encoders)
            scope.inflight->physical[WMTMetal4AccelerationStructure]++;
          auto encoder = attachment.sample_buffer ? cmdbuf.accelerationStructureCommandEncoder(attachment)
                                                  : cmdbuf.accelerationStructureCommandEncoder();
          PassSync all;
          WaitAll([&](WMT::Fence fence) { all.waits[all.wait_count++] = fence; });
          encoder.waitForFences(all.waits, all.wait_count);
          encoder.encodeCommands(&data->cmd_head);
          if constexpr (!Metal4)
            encoder.updateFence(fence_);
          if constexpr (Metal4)
            encoder.endSegment();
          else
            encoder.endEncoding();
          break;
        }
        case EncoderType::ResolveTimestamps: {
          auto data = static_cast<ResolveTimestampsData *>(current);
          if (data->readback) {
            scope.inflight->resolves.push_back({data->heap, data->start, data->count, data->readback,
                                                static_cast<char *>(data->memory) + data->dst_offset,
                                                scope.inflight->sampled.size()});
            break;
          }
          // custom heaps may be read by later GPU work: the CPU writes them only after earlier work has completed
          Trace("waits for the timestamps of command buffer ", inflight_cmdbuf_seq_.load());
          if (scope.Settle())
            retired_.Expect(inflight_cmdbuf_seq_ - 1)->wait(false);
          WMTBufferInfo info{data->count * sizeof(uint64_t), WMTResourceStorageModeShared};
          auto staging = data->memory ? WMT::Reference<WMT::Buffer>{} : device_->GetMTLDevice().newBuffer(info);
          memcpy(data->memory ? static_cast<char *>(data->memory) + data->dst_offset : info.memory.ptr,
                 &data->heap->timestamps[data->start], data->count * sizeof(uint64_t));
          scope.Start();
          cmdbuf = scope.inflight->cmdbuf;
          if (data->memory)
            break;
          if (diag::trace_encoders)
            scope.inflight->physical[WMTMetal4Blit]++;
          auto encoder = cmdbuf.blitCommandEncoder();
          WaitAll([&](WMT::Fence fence) { encoder.waitForFence(fence); });
          encoder.copyFromBuffer(staging, 0, data->dst, data->dst_offset, data->count * sizeof(uint64_t));
          if constexpr (!Metal4)
            encoder.updateFence(fence_);
          if constexpr (Metal4)
            encoder.endSegment();
          else
            encoder.endEncoding();
          break;
        }
        case EncoderType::Settle:
          // an aliasing barrier once a reserved buffer exists
          scope.OrderAliases();
          cmdbuf = scope.inflight->cmdbuf;
          break;
        case EncoderType::Predicate: {
          // what its region's kernel decided is in memory once the command buffer with the kernel has completed
          auto data = static_cast<PredicateEncoderData *>(current);
          if (std::exchange(decided, data->skips) != data->skips)
            settle();
          if (*data->skips)
            current = data->end;
          break;
        }
        case EncoderType::Later: {
          auto data = static_cast<LaterEncoderData *>(current);
          // GPU memory, read once the work so far has run
          auto read = [&](D3D12_GPU_VIRTUAL_ADDRESS address, uint64_t length) {
            uint64_t offset;
            auto allocation = device_->LookupBufferByVA(address, &offset);
            WMTBufferInfo info{length, WMTResourceStorageModeShared};
            auto staging = device_->GetMTLDevice().newBuffer(info);
            if (diag::trace_encoders)
              scope.inflight->physical[WMTMetal4Blit]++;
            auto encoder = cmdbuf.blitCommandEncoder();
            WaitAll([&](WMT::Fence fence) { encoder.waitForFence(fence); });
            if (allocation)
              encoder.copyFromBuffer(allocation->buffer(), offset, staging, 0, length);
            if constexpr (!Metal4)
              encoder.updateFence(fence_);
            if constexpr (Metal4)
              encoder.endSegment();
            else
              encoder.endEncoding();
            settle();
            auto bytes = static_cast<const char *>(info.memory.get());
            return std::vector<char>(bytes, bytes + length);
          };
          bool deserialize = data->command == LaterEncoderData::Copy &&
                             data->mode == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE;
          D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc = data->build;
          std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries;
          std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances;
          if (deserialize && !Deserialized(data->source, data->destination, read, desc, geometries, instances))
            break;
          // reading the serialized structure has waited already
          if (!deserialize)
            settle();
          // instances of structures that are not there yet are of none until they are
          bool waits = deserialize && !resolved(instances);
          auto placed = instances;
          for (auto &instance : placed)
            if (!built(instance.AccelerationStructure))
              instance.AccelerationStructure = 0;
          record([&](MTLD3D12GraphicsCommandList *list) {
            if (deserialize && !Place(desc, placed))
              return false;
            if (deserialize || data->command == LaterEncoderData::Build)
              list->BuildRaytracingAccelerationStructure(&desc, data->information_count, data->information);
            else if (data->command == LaterEncoderData::Copy)
              list->CopyRaytracingAccelerationStructure(data->destination, data->source, data->mode);
            else
              list->EmitRaytracingAccelerationStructurePostbuildInfo(data->information, data->source_count, data->sources);
            return true;
          });
          if (auto structure = waits ? device_->LookupAccelerationStructure(data->destination) : nullptr)
            unresolved_.push_back({data->destination, structure, structure->inputs, desc.Inputs, std::move(instances)});
          break;
        }
        }
        if (profiled) {
          profiled->encoding = diag::Now() - profiled->encoding;
          // Metal 4 may start the pass with the sample the pass before it ended with
          if (profiled->first != ~0u)
            profiled->first = uint32_t(attachment.start_of_encoder_sample_index);
        }
        current = current->next;
        // what an application's pass has filled may be what a deserialized top-level structure waited for
        for (size_t i = 0; !own && i < unresolved_.size();) {
          auto &waiting = unresolved_[i];
          bool there = device_->LookupAccelerationStructure(waiting.destination) == waiting.structure &&
                       waiting.structure->inputs == waiting.inputs;
          if (there && !resolved(waiting.instances)) {
            i++;
            continue;
          }
          auto ready = std::move(waiting);
          unresolved_.erase(unresolved_.begin() + i);
          if (!there)
            continue;
          settle();
          record([&](MTLD3D12GraphicsCommandList *list) {
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{ready.destination, ready.described};
            if (!Place(desc, ready.instances))
              return false;
            list->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
            return true;
          });
        }
      }
    };
    // lists whose every pass encodes on any thread, as many as follow each other: their passes, and what they cost.
    // not with what keeps state of a slot's own (profiles, GPU records, debug groups, encoder counts)
    std::vector<std::pair<EncoderData *, uint64_t>> passes;
    bool parallel = EncodeThreads() > 1 && !profile_ && !record_totals_ && !isolate && !diag::events &&
                    !diag::trace_encoders;
    for (size_t i = 0; i < recorded.size();) {
      size_t j = i;
      uint64_t commands = 0;
      passes.clear();
      for (; parallel && unresolved_.empty() && j < recorded.size(); j++) {
        auto first = passes.size();
        auto pass = recorded[j];
        for (; pass && Parallel(pass); pass = pass->next)
          commands += passes.emplace_back(pass, Commands(pass)).second;
        if (pass) {
          for (auto k = first; k < passes.size(); k++)
            commands -= passes[k].second;
          passes.resize(first);
          break;
        }
      }
      if (commands >= kParallelCommands && passes.size() > 1) {
        if (!EncodeParallel(scope, passes, commands, sparse_mappings))
          return;
        cmdbuf = scope.inflight->cmdbuf;
        i = j;
        continue;
      }
      for (j = std::max(j, i + 1); i < j; i++)
        encode(encode, recorded[i], keeps.allocators[i].ptr());
    }
  };

  // encodes a pass of draws, copies, dispatches or a resolve into `cmdbuf`: it needs nothing of the queue's but its
  // fences (`sync`), so the encoding threads run it too (EncodeParallel). `attachment` is the caller's: a Metal 4 pass
  // may start with the sample the pass before it ended with, which the bridge writes into it. false when the device
  // is lost
  bool
  EncodePass(
      MetalCommandBuffer cmdbuf, EncoderData *current, WMTSampleBufferAttachmentInfo &attachment,
      WMT::Buffer sparse_mappings, InflightCommandBuffer &inflight, const GPUListRecords *records, const PassSync &sync
  ) {
    auto wait = [&](auto &encoder) { encoder.waitForFences(sync.waits, sync.wait_count); };
    // a Metal 4 pass of compute or blit work with no fence of its own is a segment of the buffer's one compute
    // encoder (NextSync), and so is a render pass's work before it
    auto end = [&](auto &encoder) {
      if constexpr (Metal4)
        if (!sync.update.handle || current->type == EncoderType::Render || current->type == EncoderType::Clear)
          return encoder.endSegment();
      if (sync.update.handle)
        encoder.updateFence(sync.update);
      encoder.endEncoding();
    };
    switch (current->type) {
    case EncoderType::Clear:
    case EncoderType::Render: {
      auto data = static_cast<RenderEncoderData *>(current);
      WMTRenderPassInfo render_pass_info;
      WMT::InitializeRenderPassInfo(render_pass_info);
      {
        for (unsigned i = 0; i < std::size(render_pass_info.colors); i++) {
          auto &color_data = data->colors[i];
          if (!color_data.attachment)
            continue;
          auto &color_info = render_pass_info.colors[i];
          color_info.texture = color_data.attachment.texture();
          color_info.load_action = color_data.load_action;
          color_info.store_action = color_data.store_action;
          color_info.level = color_data.level;
          color_info.slice = color_data.slice;
          color_info.depth_plane = color_data.depth_plane;
          color_info.clear_color = color_data.clear_color;
          color_info.resolve_texture = color_data.resolve_attachment.texture();
          color_info.resolve_level = color_data.resolve_level;
          color_info.resolve_slice = color_data.resolve_slice;
          color_info.resolve_depth_plane = color_data.resolve_depth_plane;
        }
        if (data->depth.attachment) {
          auto &depth_info = render_pass_info.depth;
          auto &depth_data = data->depth;
          depth_info.texture = depth_data.attachment.texture();
          depth_info.load_action = depth_data.load_action;
          depth_info.store_action = depth_data.store_action;
          depth_info.level = depth_data.level;
          depth_info.slice = depth_data.slice;
          depth_info.depth_plane = depth_data.depth_plane;
          depth_info.clear_depth = depth_data.clear_depth;
        }
        if (data->stencil.attachment) {
          auto &stencil_info = render_pass_info.stencil;
          auto &stencil_data = data->stencil;
          stencil_info.texture = stencil_data.attachment.texture();
          stencil_info.load_action = stencil_data.load_action;
          stencil_info.store_action = stencil_data.store_action;
          stencil_info.level = stencil_data.level;
          stencil_info.slice = stencil_data.slice;
          stencil_info.depth_plane = stencil_data.depth_plane;
          stencil_info.clear_stencil = stencil_data.clear_stencil;
        }
        render_pass_info.default_raster_sample_count = data->default_raster_sample_count;
        render_pass_info.programmed_samples = data->programmed_samples;
        std::ranges::copy(data->sample_positions, render_pass_info.sample_positions);
        render_pass_info.render_target_array_length = data->render_target_array_length;
        render_pass_info.render_target_width = data->render_target_width;
        render_pass_info.render_target_height = data->render_target_height;
        render_pass_info.visibility_buffer = data->visibility_buffer;
      }
      if (data->before_head.next.get()) {
        if (diag::trace_encoders)
          inflight.physical[WMTMetal4Compute]++;
        auto before = cmdbuf.computeCommandEncoder(true);
        wait(before);
        before.encodeCommands(&data->before_head);
        end(before);
        if (data->omitted) {
          size_t count = 0;
          for (auto omitted = data->omitted; omitted; omitted = omitted->next)
            count += omitted->words;
          WMTBufferInfo info{count * sizeof(uint32_t), WMTResourceStorageModeShared};
          auto buffer = device_->GetMTLDevice().newBuffer(info);
          if (!buffer) {
            device_->LoseDevice();
            return false;
          }
          // a replay may overwrite the resolver's words before the completion thread runs
          if (diag::trace_encoders)
            inflight.physical[WMTMetal4Blit]++;
          auto copy = cmdbuf.blitCommandEncoder();
          if constexpr (!Metal4)
            if (sync.update.handle)
              copy.waitForFence(sync.update);
          size_t offset = 0;
          for (auto omitted = data->omitted; omitted; omitted = omitted->next) {
            copy.copyFromBuffer(omitted->buffer, omitted->offset, buffer, offset * sizeof(uint32_t), omitted->words * sizeof(uint32_t));
            inflight.omitted.emplace_back(
                buffer, std::span(static_cast<const uint32_t *>(info.memory.get()) + offset, omitted->words),
                omitted->scratch
            );
            offset += omitted->words;
          }
          end(copy);
        }
      }
      if (diag::trace_encoders)
        inflight.physical[WMTMetal4Render]++;
      auto encoder = attachment.sample_buffer ? cmdbuf.renderCommandEncoder(render_pass_info, attachment)
                                              : cmdbuf.renderCommandEncoder(render_pass_info);
      if ((record_totals_ || trace::Heavy(trace::Pass)) && current->type == EncoderType::Render) {
        auto label = "render " + device_->PassName(current, !profile_);
        if (trace::Heavy(trace::Pass))
          TRACE_IN(Pass, "queue ", this, ": ", label);
        if (record_totals_) {
          SnapshotGPURecords(records, current, label, inflight.records);
          for (auto most = data->most; most; most = most->next)
            inflight.most.emplace_back(label, most);
        }
        encoder.setLabel(WMT::String::string(label.c_str(), WMTUTF8StringEncoding));
      }
      if (current->type == EncoderType::Clear)
        encoder.setLabel(WMT::String::string("ClearPass", WMTUTF8StringEncoding));
      // the object and mesh stages read what earlier passes wrote as the vertex stage does. a pass that draws to what
      // one of its group drew to waits for that one's pixels before its own. the waits go to Metal in one call
      std::array<wmtcmd_render_fence_op, std::tuple_size_v<decltype(sync.waits)> + 1 +
                                             std::tuple_size_v<decltype(sync.pixels)>>
          waits;
      unsigned waited = 0;
      auto wait_for = [&](WMT::Fence fence, WMTRenderStages stages) {
        auto &op = waits[waited] = {WMTRenderCommandWaitForFence, {}, {}, fence.handle, stages};
        if (waited++)
          waits[waited - 2].next.set(&op);
      };
      for (unsigned i = 0; i < sync.wait_count; i++)
        wait_for(sync.waits[i], WMTRenderStagePreRaster);
      if (data->before_head.next.get() && sync.update.handle)
        wait_for(sync.update, WMTRenderStagePreRaster);
      for (unsigned i = 0; i < sync.pixel_count; i++)
        wait_for(sync.pixels[i], WMTRenderStageFragment);
      if (waited)
        encoder.encodeCommands(reinterpret_cast<const wmtcmd_render_nop *>(waits.data()));
      if (sparse_mappings) {
        inflight.sparse_mappings = sparse_mappings;
        encoder.setVertexBuffer(sparse_mappings, 0, kSparseBufferArgumentIndex);
        encoder.setFragmentBuffer(sparse_mappings, 0, kSparseBufferArgumentIndex);
        encoder.setObjectBuffer(sparse_mappings, 0, kSparseBufferArgumentIndex);
        encoder.setMeshBuffer(sparse_mappings, 0, kSparseBufferArgumentIndex);
      }
      encoder.encodeCommands(&data->cmd_head);
      if (sync.update.handle)
        encoder.updateFence(sync.update, WMTRenderStageFragment);
      encoder.endEncoding();
      break;
    }
    case EncoderType::Blit: {
      auto data = static_cast<BlitEncoderData *>(current);
      if (diag::trace_encoders)
        inflight.physical[WMTMetal4Blit]++;
      auto encoder = attachment.sample_buffer ? cmdbuf.blitCommandEncoderWithSampleBuffers(&attachment, 1)
                                              : cmdbuf.blitCommandEncoder();
      wait(encoder);
      encoder.encodeCommands(&data->cmd_head);
      end(encoder);
      break;
    }
    case EncoderType::Compute: {
      auto data = static_cast<ComputeEncoderData *>(current);
      if (diag::trace_encoders)
        inflight.physical[WMTMetal4Compute]++;
      bool concurrent = DispatchesOverlap(reinterpret_cast<const wmtcmd_base *>(&data->cmd_head));
      auto encoder = attachment.sample_buffer ? cmdbuf.computeCommandEncoder(attachment, concurrent)
                                              : cmdbuf.computeCommandEncoder(concurrent);
      if (record_totals_ || trace::Heavy(trace::Pass)) {
        auto label = "compute " + device_->PassName(current, !profile_);
        if (trace::Heavy(trace::Pass))
          TRACE_IN(Pass, "queue ", this, ": ", label);
        if (record_totals_)
          SnapshotGPURecords(records, current, label, inflight.records);
        encoder.setLabel(WMT::String::string(label.c_str(), WMTUTF8StringEncoding));
      }
      wait(encoder);
      if (sparse_mappings || data->rays) {
        inflight.sparse_mappings = sparse_mappings;
        wmtcmd_compute_setbuffer binding{};
        binding.type = WMTComputeCommandSetBuffer;
        binding.buffer = sparse_mappings ? sparse_mappings.handle : device_->GetNullBufferHandle();
        binding.index = kSparseBufferArgumentIndex;
        encoder.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&binding));
      }
      encoder.encodeCommands(&data->cmd_head);
      end(encoder);
      break;
    }
    case EncoderType::ComputeParts: {
      auto data = static_cast<ComputePartsEncoderData *>(current);
      auto size = data->indirect ? WMTSize{data->indirect->ThreadGroupCountX, data->indirect->ThreadGroupCountY,
                                         data->indirect->ThreadGroupCountZ} : data->size;
      const std::array<uint64_t, 3> extent{size.width, size.height, size.depth};
      auto pipeline = reinterpret_cast<const wmtcmd_compute_setpso *>(data->cmd_head.next.ptr);
      assert(pipeline->type == WMTComputeCommandSetPSO);
      if (diag::trace_encoders)
        inflight.physical[WMTMetal4Compute]++;
      auto encoder = attachment.sample_buffer ? cmdbuf.computeCommandEncoder(attachment, true)
                                              : cmdbuf.computeCommandEncoder(true);
      wait(encoder);
      if (sparse_mappings) {
        inflight.sparse_mappings = sparse_mappings;
        wmtcmd_compute_setbuffer binding{};
        binding.type = WMTComputeCommandSetBuffer;
        binding.buffer = sparse_mappings;
        binding.index = kSparseBufferArgumentIndex;
        encoder.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&binding));
      }
      encoder.encodeCommands(&data->cmd_head);
      auto parts = [&](auto &parts, std::array<uint64_t, 3> origin, std::array<uint64_t, 3> size) -> void {
        if (size[0] * size[1] * size[2] > data->at_once) {
          auto axis = std::max_element(size.begin(), size.end()) - size.begin();
          auto half = size;
          half[axis] /= 2;
          parts(parts, origin, half);
          origin[axis] += half[axis];
          size[axis] -= half[axis];
          return parts(parts, origin, size);
        }
        wmtcmd_compute_dispatch_part part{};
        part.type = WMTComputeCommandDispatchPart;
        part.origin = {origin[0], origin[1], origin[2]};
        part.size = {size[0], size[1], size[2]};
        part.threadgroup_size = pipeline->threadgroup_size;
        encoder.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&part));
        wmtcmd_compute_memory_barrier barrier{};
        barrier.type = WMTComputeCommandMemoryBarrier;
        barrier.scope = WMTBarrierScopeBuffers | WMTBarrierScopeTextures;
        encoder.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&barrier));
      };
      parts(parts, {}, extent);
      end(encoder);
      break;
    }
    case EncoderType::Resolve: {
      auto data = static_cast<ResolveEncoderData *>(current);

      WMTRenderPassInfo info;
      WMT::InitializeRenderPassInfo(info);
      info.colors[0].texture = data->src.texture();
      info.colors[0].load_action = WMTLoadActionLoad;
      info.colors[0].store_action = WMTStoreActionStoreAndMultisampleResolve;
      info.colors[0].resolve_texture = data->dst.texture();

      if (diag::trace_encoders)
        inflight.physical[WMTMetal4Render]++;
      auto encoder = attachment.sample_buffer ? cmdbuf.renderCommandEncoder(info, attachment)
                                              : cmdbuf.renderCommandEncoder(info);
      for (unsigned i = 0; i < sync.wait_count; i++)
        encoder.waitForFence(sync.waits[i], WMTRenderStageFragment);
      encoder.setLabel(WMT::String::string("ResolvePass", WMTUTF8StringEncoding));
      if (sync.update.handle)
        encoder.updateFence(sync.update, WMTRenderStageFragment);
      encoder.endEncoding();

      break;
    }
    case EncoderType::Settle:
      // the encoding threads' aliasing barriers (Parallel); the queue's are the scope's (OrderAliases)
      if constexpr (Metal4)
        cmdbuf.orderAliases(true);
      break;
    default:
      break;
    }
    return true;
  }

  // encodes passes on the encoding threads, in parts of about the same cost: the first part follows what the scope's
  // command buffer has, and the scope ends with the last part's. false when the device is lost
  bool
  EncodeParallel(
      CommittingScope &scope, const std::vector<std::pair<EncoderData *, uint64_t>> &passes, uint64_t commands,
      WMT::Buffer sparse_mappings
  ) {
    auto parts = unsigned(std::min<uint64_t>({EncodeThreads(), commands / std::max<uint64_t>(kChunkCommands, 1), passes.size()}));
    if (encode_threads_.size() + 1 < parts) {
      std::lock_guard<mullion::mutex> lock(encode_mutex_);
      while (encode_threads_.size() + 1 < parts)
        encode_threads_.emplace_back([this] { EncodeWorker(); });
    }
    // where each part begins, and the end
    std::vector<size_t> bounds{0};
    uint64_t sum = 0;
    for (size_t i = 0; i + 1 < passes.size() && bounds.size() < parts; i++)
      if ((sum += passes[i].second) * parts >= commands * bounds.size())
        bounds.push_back(i + 1);
    bounds.push_back(passes.size());
    parts = bounds.size() - 1;
    // the passes' waits and updates are the queue's, in their order
    std::vector<PassSync> syncs;
    syncs.reserve(passes.size());
    for (auto &[pass, _] : passes)
      syncs.push_back(NextSync(pass));
    std::vector<WMT::Reference<MetalCommandBuffer>> buffers(parts);
    buffers[0] = scope.inflight->cmdbuf;
    // whether each part begins after an aliasing barrier that no encoder has taken yet: one a part ends with is the
    // next part's, as OrderAliases carries it to the next command buffer. the last's is the queue's after them
    std::vector<bool> carried(parts + 1);
    carried[0] = order_aliases_;
    for (unsigned k = 0; k < parts; k++) {
      carried[k + 1] = carried[k];
      for (auto i = bounds[k]; i < bounds[k + 1]; i++)
        if (passes[i].first->type == EncoderType::Settle)
          carried[k + 1] = true;
        else if (passes[i].first->type != EncoderType::Null)
          carried[k + 1] = false;
    }
    // a part's command buffer is the slot's after the one before, which commits it next
    for (unsigned k = 1; k < parts; k++) {
      if constexpr (Metal4) {
        buffers[k] = NewCommandBuffer(inflight_cmdbuf_seq_.load(std::memory_order_relaxed) + k);
        // each part's encoders run apart from the first part's, so each orders aliased memory itself
        if ((order_aliases_ || carried[k]) && buffers[k])
          buffers[k].orderAliases();
      } else {
        buffers[k] = queue_.commandBuffer();
      }
      if (!buffers[k]) {
        device_->LoseDevice();
        return false;
      }
    }
    // the parts the encoding threads have yet to finish, under the lock
    unsigned left = parts - 1;
    std::atomic<bool> lost = false;
    auto part = [&](unsigned k) {
      InflightCommandBuffer unused;
      WMTSampleBufferAttachmentInfo unsampled{};
      for (auto i = bounds[k]; i < bounds[k + 1] && !lost; i++)
        if (!EncodePass(buffers[k], passes[i].first, unsampled, sparse_mappings, unused, nullptr, syncs[i]))
          lost = true;
    };
    {
      std::lock_guard<mullion::mutex> lock(encode_mutex_);
      for (unsigned k = 1; k < parts; k++)
        encode_jobs_.emplace_back([&, k] {
          {
            // the encoders this thread makes are released with the pool, before the queue's thread hears of it
            auto pool = WMT::MakeAutoreleasePool();
            part(k);
          }
          std::lock_guard<mullion::mutex> lock(encode_mutex_);
          if (!--left)
            encoded_.notify_all();
        });
      encode_changed_.notify_all();
    }
    part(0);
    {
      std::unique_lock<mullion::mutex> lock(encode_mutex_);
      encoded_.wait(lock, [&] { return !left; });
    }
    for (unsigned k = 0; k < parts; k++) {
      for (auto i = bounds[k]; i < bounds[k + 1]; i++)
        scope.inflight->encoders[size_t(passes[i].first->type)]++;
      order_aliases_ = carried[k + 1];
      if (sparse_mappings)
        scope.inflight->sparse_mappings = sparse_mappings;
      if (k + 1 < parts) {
        scope.End();
        scope.Start(std::move(buffers[k + 1]));
      }
    }
    return !lost;
  }

  // the build that gives back the structure serialized at `Source` (CopyRaytracingAccelerationStructure,
  // DESERIALIZE): a serialized structure is its inputs. `read` gives GPU memory
  template <typename Read>
  bool
  Deserialized(
      D3D12_GPU_VIRTUAL_ADDRESS Source, D3D12_GPU_VIRTUAL_ADDRESS Destination, Read &&read,
      D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC &desc, std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> &geometries,
      std::vector<D3D12_RAYTRACING_INSTANCE_DESC> &instances
  ) {
    D3D12_SERIALIZED_RAYTRACING_ACCELERATION_STRUCTURE_HEADER header;
    memcpy(&header, read(Source, sizeof(header)).data(), sizeof(header));
    if (device_->CheckDriverMatchingIdentifier(
            D3D12_SERIALIZED_DATA_RAYTRACING_ACCELERATION_STRUCTURE, &header.DriverMatchingIdentifier
        ) != D3D12_DRIVER_MATCHING_IDENTIFIER_COMPATIBLE_WITH_DEVICE) {
      ERR("CopyRaytracingAccelerationStructure: nothing this device serialized is at the source address");
      return false;
    }
    SerializedInputs described;
    auto pointers = header.NumBottomLevelAccelerationStructurePointersAfterHeader;
    memcpy(&described, read(Source + SerializedInputsOffset(pointers), sizeof(described)).data(), sizeof(described));
    bool top = described.type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    auto bytes = read(Source, SerializedLayout(top, described.count, described.size).data);
    desc = {Destination};
    DeserializedInputs(described, bytes.data(), Source, desc.Inputs, geometries, instances);
    return true;
  }

  // gives a deserialized structure's build the memory an application gives a build: its instances' and its scratch
  bool
  Place(D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC &desc, const std::vector<D3D12_RAYTRACING_INSTANCE_DESC> &instances) {
    auto buffer = [&](D3D12_HEAP_TYPE heap, UINT64 size, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
      D3D12_HEAP_PROPERTIES props{heap};
      D3D12_RESOURCE_DESC of{D3D12_RESOURCE_DIMENSION_BUFFER, 0, std::max<UINT64>(size, 1), 1, 1, 1, DXGI_FORMAT_UNKNOWN,
                             {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, flags};
      device_->CreateCommittedResource(
          &props, D3D12_HEAP_FLAG_NONE, &of, state, nullptr, IID_PPV_ARGS(&own_buffers_.emplace_back())
      );
      return own_buffers_.back().ptr();
    };
    if (desc.Inputs.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL) {
      auto placed = buffer(
          D3D12_HEAP_TYPE_UPLOAD, instances.size() * sizeof(instances[0]), D3D12_RESOURCE_FLAG_NONE,
          D3D12_RESOURCE_STATE_GENERIC_READ
      );
      void *mapped;
      if (!placed || FAILED(placed->Map(0, nullptr, &mapped)))
        return false;
      memcpy(mapped, instances.data(), instances.size() * sizeof(instances[0]));
      desc.Inputs.InstanceDescs = placed->GetGPUVirtualAddress();
    }
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO sizes;
    device_->GetRaytracingAccelerationStructurePrebuildInfo(&desc.Inputs, &sizes);
    auto scratch = buffer(
        D3D12_HEAP_TYPE_DEFAULT, sizes.ScratchDataSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS
    );
    if (scratch)
      desc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    return scratch;
  }

  // a marker is an instant, which no pass is named by
  void STDMETHODCALLTYPE SetMarker(UINT metadata, const void *data, UINT size) {};

  // the queue's events hold the lists executed in them: the queue's thread keeps their path, in order with the lists
  void STDMETHODCALLTYPE
  BeginEvent(UINT metadata, const void *data, UINT size) {
    if (diag::events)
      Hold([this, name = diag::EventName(metadata, data, size)] { diag::PushEvent(queue_events_, name); }, true);
  };

  void STDMETHODCALLTYPE
  EndEvent() {
    if (diag::events)
      Hold([this] { diag::PopEvent(queue_events_); }, true);
  };

  HRESULT STDMETHODCALLTYPE
  Signal(ID3D12Fence *pFence, UINT64 Value) {
    // from the CPU, once the work before has completed and what it left for the CPU is done
    auto work = [this, fence = Com<MTLD3D12Fence, false>(static_cast<MTLD3D12Fence *>(pFence)), Value] {
      // after a submission another thread is in the middle of
      std::lock_guard<mullion::mutex> lock(mutex_commit_);
      retired_.Expect(inflight_cmdbuf_seq_ - 1, [this, fence, Value] {
        Trace("fence ", fence.ptr(), " reaches ", Value);
        FLIGHT_HEAVY(Sync, "fence %p reaches %llu", fence.ptr(), Value);
        fence->Signaled(Value);
      });
    };
    Trace("Signal of fence ", pFence, " to ", Value);
    FLIGHT_HEAVY(Sync, "queue %p: Signal of fence %p to %llu", this, pFence, Value);
    if (!Hold(work))
      work();
    return S_OK;
  };

  HRESULT STDMETHODCALLTYPE
  Wait(ID3D12Fence *pFence, UINT64 Value) {
    auto gate = std::make_shared<Gate>();
    gate->device = device_;
    gate->closed = true;
    static_cast<MTLD3D12Fence *>(pFence)->completion->Expect(Value, [gate] { gate->Open(); });
    auto work = [this, gate] {
      gate_->closed.wait(true);
      {
        // after a submission another thread is in the middle of
        std::scoped_lock lock(mutex_commit_, held_mutex_);
        if (held_stop_)
          return;
        gate_ = gate;
        held_changed_.notify_all();
      }
      // a Signal follows the last command buffer: an empty one is behind the gate for one with no work before it
      if (gate->closed)
        StartCommitting();
    };
    Trace("Wait for fence ", pFence, " at ", Value, gate->closed ? ", which holds the queue" : ", which it has had");
    FLIGHT_HEAVY(Sync, "queue %p: Wait for fence %p at %llu, holding %llu", this, pFence, Value, uint64_t(bool(gate->closed)));
    // MULLION_TRACE_SYNC: only the Waits that hold the queue's work back, which a hang waits on; heavy has every one
    if (gate->closed && !trace::Heavy(trace::Sync))
      TRACE_IN(Sync, "queue ", this, ": Wait for fence ", pFence, " at ", Value, " holds the queue's work");
    if (gate->closed && !Hold(work))
      Hold(work, true);
    return S_OK;
  };

  HRESULT STDMETHODCALLTYPE
  GetTimestampFrequency(UINT64 *pFrequency) {
    if (!pFrequency)
      return FAIL(E_INVALIDARG);
    *pFrequency = timestamp_frequency_;
    return S_OK;
  };

  // the GPU timestamp paired with the performance counter halfway through taking it
  HRESULT STDMETHODCALLTYPE
  GetClockCalibration(UINT64 *gpu_timestamp, UINT64 *cpu_timestamp) {
    if (!gpu_timestamp || !cpu_timestamp)
      return FAIL(E_INVALIDARG);
    LARGE_INTEGER before, after;
    uint64_t metal_cpu;
    QueryPerformanceCounter(&before);
    device_->GetMTLDevice().sampleTimestamps(metal_cpu, *gpu_timestamp);
    *gpu_timestamp = Rescale(*gpu_timestamp, timestamp_frequency_, kGPUTimestampFrequency);
    QueryPerformanceCounter(&after);
    *cpu_timestamp = before.QuadPart + (after.QuadPart - before.QuadPart) / 2;
    return S_OK;
  };

  D3D12_COMMAND_QUEUE_DESC *STDMETHODCALLTYPE
  GetDesc(D3D12_COMMAND_QUEUE_DESC *__ret) {
    *__ret = desc_;
    return __ret;
  };

  HRESULT STDMETHODCALLTYPE
  CreateSwapChain(
      IDXGIFactory1 *pFactory, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc, IDXGISwapChain1 **ppSwapChain
  ) {
    if (desc_.Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
      return FAIL(DXGI_ERROR_INVALID_CALL);
    if (hWnd == GetDesktopWindow())
      return FAIL(E_ACCESSDENIED);
    return mullion::CreateSwapChain(pFactory, device_, this, hWnd, pDesc, pFullscreenDesc, ppSwapChain);
  }

  HRESULT
  Present(
      IUnknown *swapchain, Presenter *presenter, ID3D12Resource *backbuffer, TextureViewKey view,
      HANDLE hLantecyWaitable, double after, uint64_t id, uint64_t number, ID3D12Resource *previous,
      std::vector<PresentCopy> copies
  ) override {
    if (device_->NamesPasses()) {
      std::chrono::steady_clock::time_point unset;
      device_->first_present.compare_exchange_strong(unset, std::chrono::steady_clock::now());
    }
    auto state = presenter->synchronizeLayerProperties();
    if (Holding() &&
        Hold([this, swapchain = Com(swapchain), presenter, backbuffer = Com(backbuffer), view, hLantecyWaitable, after,
              id, number, state, previous = Com(previous), copies] {
          Commit(presenter, backbuffer.ptr(), view, state, hLantecyWaitable, after, id, number, previous.ptr(), copies);
        }))
      return S_OK;
    return Commit(presenter, backbuffer, view, state, hLantecyWaitable, after, id, number, previous, copies);
  }

  void
  DumpFrame(ID3D12Resource *backbuffer, InflightCommandBuffer &inflight, uint64_t id, uint64_t number) {
    static const auto range = [] {
      std::array<uint64_t, 2> range{};
      auto begin = dump_frames.data(), end = begin + dump_frames.size();
      auto first = std::from_chars(begin, end, range[0]);
      if (first.ec == std::errc() && first.ptr != end && *first.ptr == ':') {
        auto count = std::from_chars(first.ptr + 1, end, range[1]);
        if (count.ec == std::errc() && count.ptr == end)
          return range;
      }
      ERR("D3D12 frame range invalid: expected first:count");
      return std::array<uint64_t, 2>{};
    }();
    if (number < range[0] || number - range[0] >= range[1])
      return;
    static const auto directory = [] {
      auto path = env::getEnvVar("MULLION_LOG_PATH");
      if (path.empty() || path == "none") {
        WARN("D3D12 frame directory missing: MULLION_LOG_PATH must name a directory");
        return std::string();
      }
      return path + (path.back() == '/' ? "" : "/");
    }();
    if (directory.empty())
      return;
    D3D12_RESOURCE_DESC desc;
    backbuffer->GetDesc(&desc);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT64 row, size;
    device_->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, &row, &size);
    std::ostringstream name;
    name << directory << env::getExeBaseName() << "_swap_" << GetCurrentProcessId() << "_" << id << "_"
         << std::setfill('0') << std::setw(std::numeric_limits<uint64_t>::digits10 + 1) << number << ".frame";
    auto path = name.str();
    WMTBufferInfo info{.length = size, .options = WMTResourceStorageModeShared};
    info.memory.set(nullptr);
    WMT::Reference<WMT::Buffer> buffer = device_->GetMTLDevice().newBuffer(info);
    if (!buffer) {
      ERR("D3D12 frame failed: ", path, ": readback allocation failed");
      return;
    }
    auto resource = static_cast<MTLD3D12Resource *>(backbuffer);
    auto &view = resource->texture->view(resource->texture->fullView);
    wmtcmd_blit_copy_from_texture_to_buffer copy{.type = WMTBlitCommandCopyFromTextureToBuffer};
    copy.src = view.texture.handle;
    copy.size = {uint32_t(desc.Width), desc.Height, 1};
    copy.dst = buffer.handle;
    copy.bytes_per_row = footprint.Footprint.RowPitch;
    if (diag::trace_encoders)
      inflight.physical[WMTMetal4Blit]++;
    auto blit = inflight.cmdbuf.blitCommandEncoder();
    WaitAll([&](WMT::Fence fence) { blit.waitForFence(fence); });
    blit.encodeCommands((const wmtcmd_blit_nop *)&copy);
    if constexpr (!Metal4)
      blit.updateFence(fence_);
    blit.endEncoding();
    inflight.completed = [present = std::move(inflight.completed), buffer, pixels = info.memory.get(),
                          cmdbuf = inflight.cmdbuf, desc, footprint, row, number, path]() mutable {
      if (present)
        present();
      if (cmdbuf.status() != WMTCommandBufferStatusCompleted) {
        ERR("D3D12 frame failed: ", path, ": GPU work did not complete");
        return;
      }
      std::ofstream file(str::topath(path.c_str()).c_str(), std::ios::binary);
      file << "mullion frame " << number << ' ' << desc.Width << ' ' << desc.Height << ' ' << uint32_t(desc.Format)
           << ' ' << row << '\n';
      for (UINT y = 0; y < desc.Height && file; y++)
        file.write((const char *)pixels + uint64_t(y) * footprint.Footprint.RowPitch, row);
      file.close();
      if (!file)
        ERR("D3D12 frame failed: ", path, ": file could not be written");
      else
        Logger::info(str::format("D3D12 frame written: ", path));
    };
  }

  HRESULT
  Commit(
      Presenter *presenter, ID3D12Resource *backbuffer, TextureViewKey shown, const Presenter::PresentState &state,
      HANDLE hLantecyWaitable, double after, uint64_t id, uint64_t number, ID3D12Resource *previous,
      const std::vector<PresentCopy> &copies
  ) {
    Trace("Present");
    FLIGHT_HEAVY(Present, "queue %p: Present %llu", this, number);
    auto scope = StartCommitting();
    auto &cmdbuf = scope.inflight->cmdbuf;
    if constexpr (Metal4)
      if (!cmdbuf)
        return device_->RemovedReason();
    if (profile_)
      diag::Frame(device_->GetMTLDevice(), profile_->id, inflight_cmdbuf_seq_.load());

    auto g = reinterpret_cast<MTLD3D12Resource *>(backbuffer);
    auto &view = g->texture->view(g->texture->fullView);
    if (!copies.empty()) {
      auto p = reinterpret_cast<MTLD3D12Resource *>(previous);
      auto blit = cmdbuf.blitCommandEncoder();
      WaitAll([&](WMT::Fence fence) { blit.waitForFence(fence); });
      for (auto [to, from] : copies)
        for (uint32_t eye = 0; eye < g->texture->arrayLength(); eye++) {
          wmtcmd_blit_copy_from_texture_to_texture copy{.type = WMTBlitCommandCopyFromTextureToTexture};
          copy.src = p->texture->view(p->texture->fullView).texture.handle;
          copy.src_slice = eye;
          copy.src_origin = {uint32_t(to.left + from.x), uint32_t(to.top + from.y), 0};
          copy.src_size = {uint32_t(to.right - to.left), uint32_t(to.bottom - to.top), 1};
          copy.dst = view.texture.handle;
          copy.dst_slice = eye;
          copy.dst_origin = {uint32_t(to.left), uint32_t(to.top), 0};
          blit.encodeCommands((const wmtcmd_blit_nop *)&copy);
        }
      if constexpr (Metal4) {
        blit.endSegment();
      } else {
        blit.updateFence(fence_);
        blit.endEncoding();
      }
    }

    unsigned present_encoders = 0;
    auto drawable = presenter->encodeCommands(
        MetalCommandBuffer(cmdbuf), g->texture->view(shown).texture, state,
        [&](auto encoder) {
          WaitAll([&](WMT::Fence fence) { encoder.waitForFence(fence, WMTRenderStageFragment); });
        },
        [&](auto encoder) { encoder.updateFence(fence_, WMTRenderStageFragment); }, inflight_cmdbuf_seq_.load(),
        diag::trace_encoders ? &present_encoders : nullptr
    );
    if (diag::trace_encoders) {
      scope.inflight->physical[WMTMetal4Render]++;
      scope.inflight->physical[WMTMetal4Blit] += present_encoders - 1;
    }

    if (!drawable)
      scope.inflight->completed = [presenter = Rc(presenter), after] { presenter->drawToWindow(after); };
    else if (after > 0)
      cmdbuf.presentDrawableAfterMinimumDuration(drawable, after);
    else
      cmdbuf.presentDrawable(drawable);
    if (!dump_frames.empty())
      DumpFrame(backbuffer, *scope.inflight, id, number);
    scope.inflight->semaphore = hLantecyWaitable;
    scope.inflight->presents = true;
    // the work after this present is frame number + 1
    capture_.frame(device_->GetMTLDevice(), number + 1);

    return S_OK;
  }
};

void
SettleCommandQueueFeedback(MTLD3D12Device *pDevice) {
  MTLD3D12CommandQueueImpl<true>::SettleAll(pDevice);
}

HRESULT
CreateCommandQueue(MTLD3D12Device *pDevice, const D3D12_COMMAND_QUEUE_DESC *pDesc, REFIID riid, void **ppCommandQueue) {
  auto create = [&]<bool Metal4>() -> HRESULT {
    auto command_queue = Com(new MTLD3D12CommandQueueImpl<Metal4>(pDevice));
    HRESULT hr = command_queue->Initialize(pDesc);
    if (FAILED(hr))
      return hr;
    return command_queue->QueryInterface(riid, ppCommandQueue);
  };
  return pDevice->UsesMetal4() ? create.template operator()<true>() : create.template operator()<false>();
};

} // namespace mullion