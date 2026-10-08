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
#include <atomic>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include "d3d10_1.h"
#include "d3d11_4.h"

namespace dxmt {

constexpr auto kCommandQueueSize = 32u;
const auto dump_frames = env::getEnvVar("DXMT_D3D12_DUMP_FRAMES");

const GUID kD3D12CommandQueueDownlevelUUID = {
    0x38a8c5ef, 0x7ccb, 0x4e81, {0x91, 0x4f, 0xa6, 0xe9, 0xd0, 0x72, 0xc4, 0x94}
};

class MTLD3D12CommandQueueImpl : public MTLD3D12Pageable<MTLD3D12CommandQueue, IMTLSwapChainFactory> {

  D3D12_COMMAND_QUEUE_DESC desc_;

  WMT::Reference<WMT::CommandQueue> queue_;
  WMT::Reference<WMT::Fence> fence_;
  // counts the timestamp resolves into buffers the GPU may read, which wait for the CPU to read their samples
  Rc<Fence> timestamps_read_;
  uint64_t timestamp_resolves_ = 0;

  // Metal fails a command buffer that waits for an event for more than a few seconds, and ignores the queue's
  // later ones, so no command buffer waits: the CPU does. after Wait for a value the fence has yet to reach, the
  // work the application gives the queue is held, and a thread commits it in order once the fence is there
  dxmt::mutex held_mutex_;
  dxmt::condition_variable held_changed_;
  std::deque<std::function<void()>> held_;
  bool holding_ = false, held_stop_ = false;
  // the fence value the worker waits for
  std::shared_ptr<std::atomic<bool>> awaited_;
  dxmt::thread held_thread_;

  // what the queue does and where it waits, with DXMT_LOG_LEVEL=trace: the last lines of a hang name it
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
    std::unique_lock<dxmt::mutex> lock(held_mutex_);
    if (!holding_ && !begin)
      return false;
    if (!held_thread_.joinable())
      held_thread_ = dxmt::thread([this] { CommitHeld(); });
    holding_ = true;
    held_.emplace_back(std::forward<Work>(work));
    held_changed_.notify_all();
    return true;
  }

  bool
  Holding() {
    std::unique_lock<dxmt::mutex> lock(held_mutex_);
    return holding_;
  }

  void
  CommitHeld() {
    env::setThreadName("dxmt-held-work-thread");
    std::unique_lock<dxmt::mutex> lock(held_mutex_);
    for (;;) {
      held_changed_.wait(lock, [&] { return held_stop_ || !held_.empty(); });
      if (held_stop_)
        return;
      // the deque is the lock's: the work is taken out of it before it runs, and its place stays until it is done
      auto work = std::move(held_.front());
      lock.unlock();
      Trace("held work starts");
      work();
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
      auto before = scope.End();
      Trace("tile mappings: waits for the work before");
      before.waitUntilCompleted();
      update(mapping_queue_);
      mapping_queue_.signalEvent(mapped_->sharedEvent(), ++mapping_value_);
      Trace("tile mappings: waits for mapping ", mapping_value_);
      mapped_->wait(mapping_value_);
      Trace("tile mappings: done");
    };
    if (!Hold(work))
      work();
  }

  std::atomic_uint64_t inflight_cmdbuf_seq_ = 1;
  std::atomic_uint64_t inflight_cmdbuf_count_ = 0;
  std::atomic_uint64_t inflight_cmdbuf_stop_ = 0;

  struct InflightCommandBuffer {
    WMT::Reference<WMT::CommandBuffer> cmdbuf{};
    HANDLE semaphore{};
    std::function<void()> completed{};
    // the timestamp queries the command buffer samples: the device's counter sample buffer it holds until it
    // completes, and the query of each sample taken
    WMT::Reference<WMT::CounterSampleBuffer> samples{};
    std::vector<std::pair<Com<MTLD3D12QueryHeap>, uint32_t>> sampled{};
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
    // DXMT_D3D12_GPU_ERRORS: the passes with indirect commands and where their resolvers keep their largest numbers
    std::vector<std::pair<std::string, const RenderEncoderData::Most *>> most{};
    // how many encoders of each kind it has, and whether it presents: what a report of one that does not complete says
    std::array<uint16_t, size_t(EncoderType::AccelerationStructure) + 1> encoders{};
    bool presents = false;
  };

  std::array<InflightCommandBuffer, kCommandQueueSize> inflight_cmdbuf_pool_;
  dxmt::thread inflight_cmdbuf_wait_thread_;

  // a command buffer that does not complete stops the queue with no error from Metal: a thread reports the one the
  // completion thread has waited on for kStuckSeconds, with what Metal says of it and what is in it. a warning: one
  // list of very many draws takes as long
  static constexpr auto kStuckSeconds = std::chrono::seconds(10);
  dxmt::mutex watch_mutex_;
  dxmt::condition_variable watch_changed_;
  uint64_t watched_seq_ = 0;
  bool watch_stop_ = false;
  dxmt::thread watch_thread_;

  void
  Watch() {
    env::setThreadName("dxmt-cmdbuf-watch-thread");
    std::unique_lock<dxmt::mutex> lock(watch_mutex_);
    while (!watch_stop_) {
      auto seq = watched_seq_;
      // the same one still, after the time: it is reported once, and the thread sleeps until another is waited on
      if (watch_changed_.wait_for(lock, kStuckSeconds, [&] { return watch_stop_ || watched_seq_ != seq; }) || !seq)
        continue;
      auto &inflight = inflight_cmdbuf_pool_[seq % kCommandQueueSize];
      std::string encoders;
      for (auto count : inflight.encoders)
        encoders += std::to_string(count) + " ";
      WARN("command buffer ", seq, " of queue ", this, " has not completed for ", kStuckSeconds.count(),
          " s; Metal's status of it is ", uint64_t(inflight.cmdbuf.status()), " (2 committed, 3 scheduled); encoders by kind: ",
          encoders, inflight.presents ? "and a present" : "");
      watch_changed_.wait(lock, [&] { return watch_stop_ || watched_seq_ != seq; });
    }
  }

  // the command buffer the completion thread waits on, 0 for none
  void
  Watched(uint64_t seq) {
    std::lock_guard<dxmt::mutex> lock(watch_mutex_);
    watched_seq_ = seq;
    watch_changed_.notify_all();
  }

  dxmt::mutex mutex_commit_;

  void
  CommandBufferWaitingThread() {
    env::setThreadName("dxmt-cmdbuf-waiting-thread");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    uint64_t internal_seq = 1;
    for (;;) {
      inflight_cmdbuf_seq_.wait(internal_seq, std::memory_order_acquire);
      if (inflight_cmdbuf_stop_.load() == internal_seq)
        break;
      auto &inflight = inflight_cmdbuf_pool_[internal_seq % kCommandQueueSize];

      Trace("waits for command buffer ", internal_seq);
      Watched(internal_seq);
      if (inflight.cmdbuf.status() <= WMTCommandBufferStatusScheduled)
        inflight.cmdbuf.waitUntilCompleted();
      Watched(0);
      Trace("command buffer ", internal_seq, " completed");
      if (inflight.cmdbuf.status() == WMTCommandBufferStatusError) {
        // the first says why; Metal fails what follows a fault of the process's too
        if (SUCCEEDED(device_->GetDeviceRemovedReason())) {
          ERR("Device error: ", inflight.cmdbuf.error().description().getUTF8String());
          for (auto &[label, most] : inflight.most)
            ERR("indirect commands of ", label, ": ", most->words[5], " commands of at most ", most->call->max_count,
                most->call->max_count_buffer ? " with a count buffer" : "", ", arguments at ",
                (void *)most->call->argument_buffer, "; the most instances ", most->words[0], ", vertices or indices ",
                most->words[1], ", start ", most->words[2], ", base vertex ", most->words[3], ", start instance ",
                most->words[4]);
        }
        device_->LoseDevice();
      }

      std::vector<uint64_t> clock(inflight.sampled.size());
      if (inflight.samples) {
        inflight.samples.resolveCounterRange(0, clock.size(), clock.data(), clock.size() * sizeof(uint64_t));
        device_->timestamp_samples.Return(std::move(inflight.samples));
      }
      // samples and resolves in the order the lists had them: a query sampled again after a resolve is another value
      size_t read = 0;
      auto sample = [&](size_t until) {
        for (; read < until; read++)
          inflight.sampled[read].first->timestamps[inflight.sampled[read].second] = clock[read];
      };
      for (auto &resolve : inflight.resolves) {
        sample(resolve.after);
        memcpy(resolve.to, &resolve.heap->timestamps[resolve.first], resolve.count * sizeof(uint64_t));
      }
      sample(clock.size());
      for (auto &allocator : inflight.allocators)
        if (allocator)
          allocator->pending--;
      if (inflight.completed)
        inflight.completed();
      if (inflight.semaphore)
        ReleaseSemaphore(inflight.semaphore, 1, nullptr);

      inflight = {};

      inflight_cmdbuf_count_.fetch_sub(1, std::memory_order_release);
      inflight_cmdbuf_count_.notify_one();

      internal_seq++;
    }
  };

  struct CommittingScope {
    MTLD3D12CommandQueueImpl *queue;
    std::lock_guard<dxmt::mutex> lock;
    InflightCommandBuffer *inflight;
    WMT::Reference<WMT::Object> pool;

    CommittingScope(MTLD3D12CommandQueueImpl *queue) :
        queue(queue), lock(queue->mutex_commit_) {
      Start();
    };

    // a slot is the scope's from here to its retirement: it is waited for and counted under the lock, so two
    // submitters never pass one free slot, and counted before the completion thread hears of it
    void
    Start() {
      queue->inflight_cmdbuf_count_.wait(kCommandQueueSize, std::memory_order_acquire);
      queue->inflight_cmdbuf_count_.fetch_add(1, std::memory_order_relaxed);
      auto seq = queue->inflight_cmdbuf_seq_.load(std::memory_order_relaxed);
      inflight = &queue->inflight_cmdbuf_pool_[seq % kCommandQueueSize];
      pool = WMT::MakeAutoreleasePool();
      inflight->cmdbuf = queue->queue_.commandBuffer();
    };

    WMT::Reference<WMT::CommandBuffer>
    End() {
      auto ended = inflight->cmdbuf;
      inflight = nullptr;
      ended.commit();
      queue->inflight_cmdbuf_seq_.fetch_add(1, std::memory_order_release);
      queue->inflight_cmdbuf_seq_.notify_one();
      pool = nullptr;
      return ended;
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

public:
  MTLD3D12CommandQueueImpl(MTLD3D12Device *pDevice) :
      MTLD3D12Pageable<MTLD3D12CommandQueue, IMTLSwapChainFactory>(pDevice),
      inflight_cmdbuf_wait_thread_([this]() { this->CommandBufferWaitingThread(); }),
      watch_thread_([this] { Watch(); }) {}

  ~MTLD3D12CommandQueueImpl() {
    if (held_thread_.joinable()) {
      {
        std::lock_guard<dxmt::mutex> lock(held_mutex_);
        held_stop_ = true;
        held_changed_.notify_all();
        // a wait the queue will never see the end of
        if (awaited_) {
          *awaited_ = true;
          awaited_->notify_all();
        }
      }
      held_thread_.join();
    }
    std::lock_guard<dxmt::mutex> lock(mutex_commit_);
    // the marker first: the completion thread reads it as soon as the sequence moves
    inflight_cmdbuf_stop_.store(inflight_cmdbuf_seq_.load());
    inflight_cmdbuf_seq_.fetch_add(1);
    inflight_cmdbuf_seq_.notify_one();
    inflight_cmdbuf_wait_thread_.join();
    {
      std::lock_guard<dxmt::mutex> watch(watch_mutex_);
      watch_stop_ = true;
      watch_changed_.notify_all();
    }
    watch_thread_.join();
  }

  HRESULT
  Initialize(const D3D12_COMMAND_QUEUE_DESC *pDesc) {
    // TODO: validate and normalize
    desc_ = *pDesc;
    desc_.NodeMask = 1; // typically 1 GPU only

    auto metal_device = device_->GetMTLDevice();
    queue_ = metal_device.newCommandQueue(kCommandQueueSize);
    if (!queue_)
      return E_FAIL;
    queue_.addResidencySet(device_->GetGlobalResidencySet());

    fence_ = metal_device.newFence();
    timestamps_read_ = new Fence(metal_device);

    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

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

    return E_NOINTERFACE;
  }

  void STDMETHODCALLTYPE UpdateTileMappings(
      ID3D12Resource *resource, UINT region_count, const D3D12_TILED_RESOURCE_COORDINATE *region_start_coordinates,
      const D3D12_TILE_REGION_SIZE *region_sizes, ID3D12Heap *heap, UINT range_count,
      const D3D12_TILE_RANGE_FLAGS *range_flags, const UINT *heap_range_offsets, const UINT *range_tile_counts,
      D3D12_TILE_MAPPING_FLAGS flags
  ) {
    // D3D12's regions and heap ranges, walked tile by tile together, become Metal operations over runs of tiles along
    // x with consecutive heap tiles. Metal applies them in order, so a tile named twice keeps the last mapping
    Tiling tiling(resource);
    D3D12_TILED_RESOURCE_COORDINATE coordinate{};
    D3D12_TILE_REGION_SIZE size{region_start_coordinates ? 1 : tiling.total};
    D3D12_TILE_RANGE_FLAGS range_flag = D3D12_TILE_RANGE_FLAG_NONE;
    UINT range_size = ~0u, range_offset = 0;
    std::vector<WMTSparseTextureMapping> operations;
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
        auto op = tiling.place(tiling.index(coordinate, size, region_tile));
        op.mode = range_flag == D3D12_TILE_RANGE_FLAG_NULL ? WMTSparseMappingModeUnmap : WMTSparseMappingModeMap;
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
    Map([texture = tiling.texture, res = Com(static_cast<MTLD3D12Resource *>(resource)),
         heap = Com(static_cast<MTLD3D12Heap *>(heap)), operations = std::move(operations)](WMT::MTL4CommandQueue queue) {
      WMT::Object heap_handle = heap != nullptr ? heap->heap : WMT::Heap{};
      if (texture) {
        queue.updateMappings(res->texture->current()->texture(), true, heap_handle, operations);
        return;
      }
      // a buffer's tiles are its x
      std::vector<WMTSparseBufferMapping> ranges;
      for (auto &op : operations)
        ranges.push_back({op.mode, op.origin.x, op.size.width, op.heap_tile});
      queue.updateMappings(res->buffer->current()->buffer(), false, heap_handle, ranges);
    });
  };

  void STDMETHODCALLTYPE CopyTileMappings(
      ID3D12Resource *dst_resource, const D3D12_TILED_RESOURCE_COORDINATE *dst_region_start_coordinate,
      ID3D12Resource *src_resource, const D3D12_TILED_RESOURCE_COORDINATE *src_region_start_coordinate,
      const D3D12_TILE_REGION_SIZE *region_size, D3D12_TILE_MAPPING_FLAGS flags
  ) {
    Tiling source(src_resource), destination(dst_resource);
    if (source.texture != destination.texture) {
      ERR("CopyTileMappings: Metal copies mappings between buffers or between textures only");
      return;
    }
    std::vector<WMTSparseTextureMappingCopy> operations;
    for (UINT n = 0; n < region_size->NumTiles; n++) {
      auto from = source.place(source.index(*src_region_start_coordinate, *region_size, n));
      auto to = destination.place(destination.index(*dst_region_start_coordinate, *region_size, n));
      operations.push_back({from.origin, from.size, from.level, from.slice, to.origin, to.level, to.slice});
    }
    // regions of one resource may overlap, with the result of a copy through a temporary one. Metal copies the tiles
    // in order, so a copy to later tiles goes from its last tile back, as memmove does
    if (src_resource == dst_resource && region_size->NumTiles &&
        destination.index(*dst_region_start_coordinate, *region_size, 0) >
            source.index(*src_region_start_coordinate, *region_size, 0))
      std::reverse(operations.begin(), operations.end());
    Map([texture = source.texture, src = Com(static_cast<MTLD3D12Resource *>(src_resource)),
         dst = Com(static_cast<MTLD3D12Resource *>(dst_resource)),
         operations = std::move(operations)](WMT::MTL4CommandQueue queue) {
      if (texture) {
        queue.copyMappings(src->texture->current()->texture(), dst->texture->current()->texture(), true, operations);
        return;
      }
      std::vector<WMTSparseBufferMappingCopy> ranges;
      for (auto &op : operations)
        ranges.push_back({op.origin.x, 1, op.destination_origin.x});
      queue.copyMappings(src->buffer->current()->buffer(), dst->buffer->current()->buffer(), false, ranges);
    });
  };

  void STDMETHODCALLTYPE
  ExecuteCommandLists(UINT Count, ID3D12CommandList *const *ppCommandLists) {
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
    auto work = [this, recorded = std::move(recorded), allocators = std::move(allocators)]() mutable {
      Commit(recorded, std::move(allocators));
    };
    if (!Hold(work))
      work();
    Trace("ExecuteCommandLists returns");
  }

  void
  Commit(const std::vector<EncoderData *> &recorded, std::vector<Com<MTLD3D12CommandAllocator>> &&allocators = {}) {
    device_->CheckAtomicLocks();
    auto scope = StartCommitting();
    // the lists' allocators stay, and count the lists as pending, until the last command buffer of theirs completes
    struct Keeps {
      CommittingScope &scope;
      std::vector<Com<MTLD3D12CommandAllocator>> allocators;
      ~Keeps() { scope.inflight->allocators = std::move(allocators); }
    } keeps{scope, std::move(allocators)};
    static const bool isolate = !env::getEnvVar("DXMT_D3D12_ISOLATE").empty();
    WMT::CommandBuffer cmdbuf = scope.inflight->cmdbuf;
    // ends the command buffer and waits for it: what the work so far leaves in memory is then there
    auto settle = [&] {
      auto ended = scope.End();
      ended.waitUntilCompleted();
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
    auto encode = [&](auto &encode, EncoderData *current) -> void {
      // records commands on the queue's list, which is free once what it recorded before has run, as are that
      // recording's buffers, and encodes them here
      auto record = [&](auto &&commands) {
        if (own_list_) {
          own_allocator_->Reset();
          own_list_->Reset(own_allocator_.ptr(), nullptr);
        } else if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&own_allocator_))) ||
                   FAILED(device_->CreateCommandList(
                       0, D3D12_COMMAND_LIST_TYPE_DIRECT, own_allocator_.ptr(), nullptr, IID_PPV_ARGS(&own_list_)
                   ))) {
          return;
        }
        auto list = static_cast<MTLD3D12GraphicsCommandList *>(own_list_.ptr());
        list->leaves_for_later = false;
        own_buffers_.clear();
        if (!commands(list))
          return;
        list->Close();
        own++;
        encode(encode, list->entry);
        own--;
      };
      const uint32_t *decided = nullptr;
      while (current) {
        // with DXMT_D3D12_ISOLATE set every pass is a command buffer that is waited for, and the trace names each:
        // the last one named is the one the GPU does not finish
        if (isolate) {
          settle();
          Trace("pass ", current->id, " of kind ", uint32_t(current->type), " starts");
        }
        scope.inflight->encoders[size_t(current->type)]++;
        switch (current->type) {
        case EncoderType::Null:
          break;
        case EncoderType::Clear: {
          auto data = static_cast<ClearEncoderData *>(current);
          {
            WMTRenderPassInfo info;
            WMT::InitializeRenderPassInfo(info);
            if (data->clear_dsv) {
              if (data->clear_dsv & 1) {
                info.depth.clear_depth = data->depth_stencil.first;
                info.depth.texture = data->attachment.texture();
                info.depth.load_action = WMTLoadActionClear;
                info.depth.store_action = WMTStoreActionStore;
                info.depth.depth_plane = data->depth_plane;
              }
              if (data->clear_dsv & 2) {
                info.stencil.clear_stencil = data->depth_stencil.second;
                info.stencil.texture = data->attachment.texture();
                info.stencil.load_action = WMTLoadActionClear;
                info.stencil.store_action = WMTStoreActionStore;
                info.stencil.depth_plane = data->depth_plane;
              }
              info.render_target_width = data->width;
              info.render_target_height = data->height;
            } else {
              info.colors[0].clear_color = data->color;
              info.colors[0].texture = data->attachment.texture();
              info.colors[0].load_action = WMTLoadActionClear;
              info.colors[0].store_action = WMTStoreActionStore;
              info.colors[0].depth_plane = data->depth_plane;
            }
            info.render_target_array_length = data->array_length;
            auto encoder = cmdbuf.renderCommandEncoder(info);
            encoder.setLabel(WMT::String::string("ClearPass", WMTUTF8StringEncoding));
            encoder.waitForFence(fence_, WMTRenderStageFragment);
            encoder.updateFence(fence_, WMTRenderStageFragment);
            encoder.endEncoding();
          }
          break;
        }
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
            render_pass_info.render_target_array_length = data->render_target_array_length;
            render_pass_info.render_target_width = data->render_target_width;
            render_pass_info.render_target_height = data->render_target_height;
            render_pass_info.visibility_buffer = data->visibility_buffer;
          }
          if (data->before_head.next.get()) {
            auto before = cmdbuf.computeCommandEncoder(false);
            before.waitForFence(fence_);
            before.encodeCommands(&data->before_head);
            before.updateFence(fence_);
            before.endEncoding();
          }
          auto encoder = cmdbuf.renderCommandEncoder(render_pass_info);
          if (device_->NamesPasses()) {
            auto label = "render " + device_->PassName(current->id);
            encoder.setLabel(WMT::String::string(label.c_str(), WMTUTF8StringEncoding));
            for (auto most = data->most; most; most = most->next)
              scope.inflight->most.emplace_back(label, most);
          }
          // the object and mesh stages read what earlier passes wrote as the vertex stage does
          encoder.waitForFence(fence_, WMTRenderStagePreRaster);
          encoder.encodeCommands(&data->cmd_head);
          encoder.updateFence(fence_, WMTRenderStageFragment);
          encoder.endEncoding();
          break;
        }
        case EncoderType::Blit: {
          auto data = static_cast<BlitEncoderData *>(current);
          WMT::CounterSampleBuffer samples{};
          uint32_t sample = 0;
          if (data->timestamps) {
            auto &pool = device_->timestamp_samples;
            // a command buffer has one buffer of samples: when it is full the next begins
            if (scope.inflight->samples && scope.inflight->sampled.size() == pool.limit) {
              scope.End();
              scope.Start();
              cmdbuf = scope.inflight->cmdbuf;
            }
            if (!scope.inflight->samples)
              scope.inflight->samples = pool.Take(device_->GetMTLDevice());
            if (scope.inflight->samples) {
              samples = scope.inflight->samples;
              sample = scope.inflight->sampled.size();
              scope.inflight->sampled.push_back({data->timestamps, data->timestamp_query});
            } else {
              ERR("EndQuery: Metal has no counter sample buffer for a timestamp");
            }
          }
          WMTSampleBufferAttachmentInfo attachment{samples, sample, ~0ull /* MTLCounterDontSample */};
          auto encoder = samples ? cmdbuf.blitCommandEncoderWithSampleBuffers(&attachment, 1)
                                 : cmdbuf.blitCommandEncoder();
          encoder.waitForFence(fence_);
          encoder.encodeCommands(&data->cmd_head);
          encoder.updateFence(fence_);
          encoder.endEncoding();
          break;
        }
        case EncoderType::AccelerationStructure: {
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
          auto encoder = cmdbuf.accelerationStructureCommandEncoder();
          encoder.waitForFence(fence_);
          encoder.encodeCommands(&data->cmd_head);
          encoder.updateFence(fence_);
          encoder.endEncoding();
          break;
        }
        case EncoderType::Compute: {
          auto data = static_cast<ComputeEncoderData *>(current);
          auto encoder = cmdbuf.computeCommandEncoder(false);
          if (device_->NamesPasses())
            encoder.setLabel(WMT::String::string(("compute " + device_->PassName(current->id)).c_str(), WMTUTF8StringEncoding));
          encoder.waitForFence(fence_);
          encoder.encodeCommands(&data->cmd_head);
          encoder.updateFence(fence_);
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
          // the command buffer ends here; when it has completed and the CPU has read the samples, the next one
          // copies them
          WMTBufferInfo info{data->count * sizeof(uint64_t), WMTResourceStorageModeShared};
          auto staging = device_->GetMTLDevice().newBuffer(info);
          auto value = ++timestamp_resolves_;
          scope.inflight->completed = [heap = Com(data->heap), staging, mapped = info.memory.ptr, first = data->start,
                                       count = data->count, read = timestamps_read_, value] {
            memcpy(mapped, &heap->timestamps[first], count * sizeof(uint64_t));
            read->signal(value);
          };
          scope.End();
          Trace("waits for timestamps ", value);
          timestamps_read_->wait(value);
          scope.Start();
          cmdbuf = scope.inflight->cmdbuf;
          auto encoder = cmdbuf.blitCommandEncoder();
          encoder.waitForFence(fence_);
          encoder.copyFromBuffer(staging, 0, data->dst, data->dst_offset, data->count * sizeof(uint64_t));
          encoder.updateFence(fence_);
          encoder.endEncoding();
          break;
        }
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
            auto encoder = cmdbuf.blitCommandEncoder();
            encoder.waitForFence(fence_);
            if (allocation)
              encoder.copyFromBuffer(allocation->buffer(), offset, staging, 0, length);
            encoder.updateFence(fence_);
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
        case EncoderType::Resolve: {
          auto data = static_cast<ResolveEncoderData *>(current);

          WMTRenderPassInfo info;
          WMT::InitializeRenderPassInfo(info);
          info.colors[0].texture = data->src.texture();
          info.colors[0].load_action = WMTLoadActionLoad;
          info.colors[0].store_action = WMTStoreActionStoreAndMultisampleResolve;
          info.colors[0].resolve_texture = data->dst.texture();

          auto encoder = cmdbuf.renderCommandEncoder(info);
          encoder.waitForFence(fence_, WMTRenderStageFragment);
          encoder.setLabel(WMT::String::string("ResolvePass", WMTUTF8StringEncoding));
          encoder.updateFence(fence_, WMTRenderStageFragment);
          encoder.endEncoding();

          break;
        }
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
    for (auto entry : recorded)
      encode(encode, entry);
  };

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

  void STDMETHODCALLTYPE SetMarker(UINT metadata, const void *data, UINT size) {};

  void STDMETHODCALLTYPE BeginEvent(UINT metadata, const void *data, UINT size) {};

  void STDMETHODCALLTYPE EndEvent() {};

  HRESULT STDMETHODCALLTYPE
  Signal(ID3D12Fence *pFence, UINT64 Value) {
    // from the CPU, once the work before has completed and what it left for the CPU is done
    auto work = [this, fence = Com(static_cast<MTLD3D12Fence *>(pFence)), Value] {
      StartCommitting().inflight->completed = [this, fence, Value] {
        Trace("fence ", fence.ptr(), " reaches ", Value);
        fence->Reach(Value);
      };
    };
    Trace("Signal of fence ", pFence, " to ", Value);
    if (!Hold(work))
      work();
    return S_OK;
  };

  HRESULT STDMETHODCALLTYPE
  Wait(ID3D12Fence *pFence, UINT64 Value) {
    auto reached = static_cast<MTLD3D12Fence *>(pFence)->Expect(Value);
    auto work = [this, reached] {
      {
        std::lock_guard<dxmt::mutex> lock(held_mutex_);
        if (held_stop_)
          return;
        awaited_ = reached;
      }
      reached->wait(false);
    };
    Trace("Wait for fence ", pFence, " at ", Value, *reached ? ", which it has had" : ", which holds the queue");
    if (!Hold(work) && !*reached)
      Hold(work, true);
    return S_OK;
  };

  HRESULT STDMETHODCALLTYPE
  GetTimestampFrequency(UINT64 *pFrequency) {
    if (!pFrequency)
      return E_INVALIDARG;
    *pFrequency = kGPUTimestampFrequency;
    return S_OK;
  };

  // the GPU timestamp paired with the performance counter halfway through taking it
  HRESULT STDMETHODCALLTYPE
  GetClockCalibration(UINT64 *gpu_timestamp, UINT64 *cpu_timestamp) {
    if (!gpu_timestamp || !cpu_timestamp)
      return E_INVALIDARG;
    LARGE_INTEGER before, after;
    uint64_t metal_cpu;
    QueryPerformanceCounter(&before);
    device_->GetMTLDevice().sampleTimestamps(metal_cpu, *gpu_timestamp);
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
    return dxmt::CreateSwapChain(pFactory, device_, this, hWnd, pDesc, pFullscreenDesc, ppSwapChain);
  }

  HRESULT
  Present(
      IUnknown *swapchain, Presenter *presenter, ID3D12Resource *backbuffer, HANDLE hLantecyWaitable, double after,
      uint64_t id, uint64_t number
  ) {
    if (Holding() &&
        Hold([this, swapchain = Com(swapchain), presenter, backbuffer = Com(backbuffer), hLantecyWaitable, after, id,
              number] { Commit(presenter, backbuffer.ptr(), hLantecyWaitable, after, id, number); }))
      return S_OK;
    return Commit(presenter, backbuffer, hLantecyWaitable, after, id, number);
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
      auto path = env::getEnvVar("DXMT_LOG_PATH");
      if (path.empty() || path == "none") {
        WARN("D3D12 frame directory missing: DXMT_LOG_PATH must name a directory");
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
    auto blit = inflight.cmdbuf.blitCommandEncoder();
    blit.waitForFence(fence_);
    blit.encodeCommands((const wmtcmd_blit_nop *)&copy);
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
      Presenter *presenter, ID3D12Resource *backbuffer, HANDLE hLantecyWaitable, double after, uint64_t id,
      uint64_t number
  ) {
    Trace("Present");
    auto scope = StartCommitting();
    auto &cmdbuf = scope.inflight->cmdbuf;

    auto g = reinterpret_cast<MTLD3D12Resource *>(backbuffer);
    auto &view = g->texture->view(g->texture->fullView);

    auto state = presenter->synchronizeLayerProperties();
    auto drawable = presenter->encodeCommands(
        cmdbuf, view.texture, state.metadata,
        [&](auto encoder) { encoder.waitForFence(fence_, WMTRenderStageFragment); },
        [&](auto encoder) { encoder.updateFence(fence_, WMTRenderStageFragment); }
    );

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

    return S_OK;
  }
};

HRESULT
CreateCommandQueue(MTLD3D12Device *pDevice, const D3D12_COMMAND_QUEUE_DESC *pDesc, REFIID riid, void **ppCommandQueue) {
  auto command_queue = Com(new MTLD3D12CommandQueueImpl(pDevice));
  HRESULT hr = command_queue->Initialize(pDesc);
  if (FAILED(hr))
    return hr;
  return command_queue->QueryInterface(riid, ppCommandQueue);
};

} // namespace dxmt