#pragma once

#include "Metal.hpp"
#include "mullion_diag.hpp"
#include "mullion_buffer.hpp"
#include "rc/util_rc_ptr.hpp"
#include "wsi_platform.hpp"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace mullion {
class VisibilityResultOffsetBumpState {
public:
  // only the first 4096 counting segments of a pass store their counts (tests/native/visibility.swift and .txt);
  // later ones add to old contents or return zero. Apple documents only the offset limit
  static constexpr uint64_t kVisibilitySegments = 4096;

  void
  beginEncoder() {
    assert(!within_encoder);
    assert(!current_data_is_dirty);
    assert(!~previous_offset);
    within_encoder = true;
  }

  bool
  tryGetNextWriteOffset(bool has_active_occlusion_queries, uint64_t &offset) {
    assert(within_encoder);
    offset = getNextWriteOffset(has_active_occlusion_queries);
    if (offset == previous_offset) {
      return false;
    }
    previous_offset = offset;
    return true;
  }

  uint64_t
  getNextReadOffset() {
    if (within_encoder) {
      if (current_data_is_dirty) {
        current_data_is_dirty = false;
        return ++next_offset;
      }
    }
    assert(!current_data_is_dirty);
    return next_offset;
  }

  void
  endEncoder() {
    assert(within_encoder);
    within_encoder = false;
    previous_offset = ~0uLL;
    if (current_data_is_dirty) {
      next_offset++;
      current_data_is_dirty = false;
    }
  }

  uint64_t
  reset() {
    assert(!within_encoder && "encoder still active");
    assert(!current_data_is_dirty && "encoder still active");
    auto ret = next_offset;
    next_offset = 0;
    return ret;
  }

private:
  uint64_t
  getNextWriteOffset(bool has_active_occlusion_queries) {
    if (has_active_occlusion_queries) {
      // each draw counts into its own slot: one counter wraps at 2^32 samples on Apple GPUs (Wine's d3d9
      // test_occlusion_query), and queries add up the slots between their begin and end
      if (current_data_is_dirty)
        next_offset++;
      current_data_is_dirty = true;
      return next_offset;
    }
    return ~0uLL;
  };

  bool within_encoder = false;
  bool current_data_is_dirty = false;
  uint64_t previous_offset = ~0uLL;
  uint64_t next_offset = 0;
};

class VisibilityResultQuery {
public:
  bool pipeline_statistics = false;
  std::vector<std::pair<Rc<Buffer>, uint32_t>> statistics;
  std::vector<std::pair<Rc<Buffer>, uint32_t>> dispatch_statistics;
  void
  incRef() {
    refcount_.fetch_add(1u, std::memory_order_acquire);
  }
  void
  decRef() {
    if (refcount_.fetch_sub(1u, std::memory_order_release) == 1u)
      delete this;
  }

  void
  begin(uint64_t seqId, unsigned offset) {
    accumulated_value_ = 0;
    statistics.clear();
    dispatch_statistics.clear();
    seq_id_begin = seqId;
    occlusion_counter_begin = offset;
    seq_id_end = ~0uLL;
    occlusion_counter_end = ~0uLL;
  }

  void
  end(uint64_t seqId, unsigned offset) {
    seq_id_end = seqId;
    occlusion_counter_end = offset;
    if (seq_id_begin == seq_id_end && occlusion_counter_begin == occlusion_counter_end && statistics.empty() &&
        dispatch_statistics.empty()) {
      seq_id_issued = seq_id_end;
    }
  }

  uint64_t queryEndAt() {
    return seq_id_end;
  };

  void
  issue(uint64_t seqId, std::vector<WMTBufferInfo> &buffers, const std::vector<uint64_t> &offsets) {
    assert(seqId >= seq_id_begin);
    assert(seqId <= seq_id_end);
    auto first = seqId == seq_id_begin ? occlusion_counter_begin : 0;
    auto end = seqId == seq_id_end ? occlusion_counter_end : offsets.back();
    assert(first <= end);
    auto window = std::upper_bound(offsets.begin(), offsets.end(), first) - offsets.begin() - 1;
    while (first < end) {
      auto data = (const uint64_t *)buffers[window].memory.get();
      auto stop = std::min(end, offsets[window + 1]);
      for (; first < stop; first++)
        accumulated_value_ += data[first - offsets[window]];
      window++;
    }
    seq_id_issued = seqId;
  }

  bool
  getValue(uint64_t *value) {
    if (seq_id_end <= seq_id_issued) {
      *value = accumulated_value_;
      return true;
    }
    return false;
  }

  void
  reset() {
    accumulated_value_ = 0;
    seq_id_begin = ~0uLL;
    occlusion_counter_begin = ~0uLL;
    seq_id_end = ~0uLL;
    occlusion_counter_end = ~0uLL;
    seq_id_issued = 0;
  };

private:
  uint64_t accumulated_value_ = 0;
  uint64_t seq_id_begin = ~0uLL;
  uint64_t occlusion_counter_begin = ~0uLL;
  uint64_t seq_id_end = ~0uLL;
  uint64_t occlusion_counter_end = ~0uLL;
  uint64_t seq_id_issued = 0;
  std::atomic<uint32_t> refcount_ = {0u};
};

class VisibilityResultReadback {
public:
  VisibilityResultReadback(
      WMT::Device device, uint64_t seq_id, uint64_t num_results, std::vector<Rc<VisibilityResultQuery>> &queries,
      std::vector<uint64_t> &&offsets
  ) :
      seq_id(seq_id),
      queries(queries),
      visibility_result_offsets(std::move(offsets)) {
    visibility_result_offsets.push_back(num_results);
    for (size_t i = 0; i + 1 < visibility_result_offsets.size(); i++) {
      auto &info = visibility_result_heap_info.emplace_back();
      info.options = WMTResourceHazardTrackingModeUntracked;
      info.length = (visibility_result_offsets[i + 1] - visibility_result_offsets[i]) * sizeof(uint64_t);
#ifdef __i386__
      info.length = (info.length + MULLION_PAGE_SIZE - 1) / MULLION_PAGE_SIZE * MULLION_PAGE_SIZE;
      info.memory.set(wsi::aligned_malloc(info.length, MULLION_PAGE_SIZE));
#endif
      visibility_result_windows.emplace_back(device.newBuffer(info));
    }
  }
  ~VisibilityResultReadback() {
    for (auto query : queries)
      query->issue(seq_id, visibility_result_heap_info, visibility_result_offsets);
#ifdef __i386__
    for (auto &info : visibility_result_heap_info)
      wsi::aligned_free(info.memory.get());
#endif
  }

  VisibilityResultReadback(const VisibilityResultReadback &) = delete;
  VisibilityResultReadback(VisibilityResultReadback &&) = delete;

  uint64_t seq_id;
  std::vector<Rc<VisibilityResultQuery>> queries;
  std::vector<uint64_t> visibility_result_offsets;
  std::vector<WMTBufferInfo> visibility_result_heap_info;
  std::vector<WMT::Reference<WMT::Buffer>> visibility_result_windows;
};

class TimestampQuery {
public:
  // Metal MTLCounters.h defines MTLCounterErrorValue as all bits set.
  static constexpr uint64_t kCounterErrorValue = UINT64_MAX;

  void
  incRef() {
    refcount_.fetch_add(1u, std::memory_order_acquire);
  }
  void
  decRef() {
    if (refcount_.fetch_sub(1u, std::memory_order_release) == 1u)
      delete this;
  }

  HRESULT
  getValue(uint64_t *value) {
    auto status = status_.load(std::memory_order_acquire);
    if (status == S_OK)
      *value = cached_value_;
    return status;
  }

  void
  issue(uint64_t sampled_data) {
    cached_value_ = sampled_data;
    status_.store(sampled_data == kCounterErrorValue ? E_FAIL : S_OK, std::memory_order_release);
  }

private:
  uint64_t cached_value_ = 0;
  std::atomic<HRESULT> status_ = {S_FALSE};
  std::atomic<uint32_t> refcount_ = {0u};
};

using TimestampQueryList = std::vector<std::pair<Rc<TimestampQuery>, uint64_t>>;

class TimestampReadback {
public:
  TimestampReadback(
      WMT::Device device, WMT::CommandBuffer cmdbuf, diag::TimestampSamples &pool, uint64_t num_samples,
      TimestampQueryList &&queries
  ) :
      queries_(std::move(queries)), pool_(pool), num_samples_(num_samples), cmdbuf_(cmdbuf),
      no_counters_(!device.supportsFamily(WMTGPUFamilyApple7)) {
    samples_.push_back(pool_.Take(device, false));
    if (!samples_.front())
      return;
    for (auto remaining = num_samples_; remaining > pool_.limit; remaining -= pool_.limit)
      samples_.push_back(pool_.Take(device, false));
  }

  ~TimestampReadback() {
    std::vector<uint64_t> results(num_samples_, TimestampQuery::kCounterErrorValue);
    // CPU resolution follows GPU completion (Apple, Converting a GPU's counter data into a readable format).
    if (cmdbuf_.status() == WMTCommandBufferStatusCompleted)
      for (const auto &[index, source] : ranges_) {
        auto count = source.end_of_encoder_sample_index - source.start_of_encoder_sample_index + 1;
        WMT::CounterSampleBuffer{source.sample_buffer}.resolveCounterRange(
            source.start_of_encoder_sample_index, count, results.data() + index, count * sizeof(uint64_t)
        );
      }
    // Metal writes no sample for a pass without work, and a virtual machine's GPU has no counters: such a query reads
    // the time of the next sample, and a later query still may not read an earlier time. samples that ran out still
    // fail the query
    thread_local uint64_t latest_end = 0;
    if (cmdbuf_.status() == WMTCommandBufferStatusCompleted && (samples_.front() || no_counters_)) {
      latest_end = std::max(cmdbuf_.gpuEndTime(), latest_end);
      auto next = latest_end;
      for (auto result = results.rbegin(); result != results.rend(); ++result) {
        if (!*result || (!samples_.front() && *result == TimestampQuery::kCounterErrorValue))
          *result = next;
        else
          next = *result;
      }
    }
    for (const auto &[query, sample_index] : queries_)
      query->issue(results[sample_index]);
    for (auto &samples : samples_)
      if (samples)
        pool_.Return(std::move(samples));
  }

  TimestampReadback(const TimestampReadback &) = delete;
  TimestampReadback(TimestampReadback &&) = delete;

  WMTSampleBufferAttachmentInfo
  sampleBuffer(uint64_t index) {
    return {samples_.front() ? samples_[index / pool_.limit].handle : 0,
            ~0ull /* MTLCounterDontSample */, index % pool_.limit};
  }

  void
  resolve(uint64_t index, WMTSampleBufferAttachmentInfo source, bool single) {
    if (!source.sample_buffer)
      return;
    auto count = single ? 1 : index % pool_.limit + 1;
    if (!single && count != pool_.limit && index + 1 != num_samples_)
      return;
    source.start_of_encoder_sample_index = source.end_of_encoder_sample_index + 1 - count;
    ranges_.push_back({index + 1 - count, source});
  }

private:
  TimestampQueryList queries_;
  diag::TimestampSamples &pool_;
  std::vector<WMT::Reference<WMT::CounterSampleBuffer>> samples_;
  std::vector<std::pair<uint64_t, WMTSampleBufferAttachmentInfo>> ranges_;
  uint64_t num_samples_;
  WMT::CommandBuffer cmdbuf_;
  bool no_counters_;
};

class TimestampQueryState {
public:
  TimestampQueryState(WMT::Device device) : device_(device) {}

  uint64_t
  addQuery(TimestampQuery *query) {
    queries_.push_back({query, num_samples_});
    return num_samples_++;
  }

  void
  coalesceQuery(TimestampQuery *query) {
    assert(num_samples_);
    queries_.push_back({query, num_samples_ - 1});
  }

  std::unique_ptr<TimestampReadback>
  flush(WMT::CommandBuffer cmdbuf, diag::TimestampSamples &pool) {
    if (num_samples_ == 0)
      return {};

    auto ret = std::make_unique<TimestampReadback>(device_, cmdbuf, pool, num_samples_, std::move(queries_));
    num_samples_ = 0;
    queries_ = {};
    return ret;
  }

private:
  uint64_t num_samples_ = 0;
  TimestampQueryList queries_;
  WMT::Device device_;
};

struct QueryReadbacks {
  std::unique_ptr<VisibilityResultReadback> visibility;
  std::unique_ptr<TimestampReadback> timestamp;
  std::vector<const uint64_t *> omitted;
};

} // namespace mullion
