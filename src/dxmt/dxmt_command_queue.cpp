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

#include "dxmt_command_queue.hpp"
#include "Metal.hpp"
#include "dxmt_statistics.hpp"
#include "util_env.hpp"
#include "util_win32_compat.h"
#include <atomic>

#define ASYNC_ENCODING 1

namespace dxmt {

void *
CommandChunk::allocate_cpu_heap(size_t size, size_t alignment) {
  return queue->AllocateCommandData(size, alignment);
}

WMTSampleBufferAttachmentInfo
CommandQueue::ProfilePass(EncoderData *pass, diag::ProfileBuffer *&profiled) {
  auto &buffer = profile_->buffers[argument_encoding_ctx.currentSeqId() % kCommandChunkCount];
  buffer.presents += pass->type == EncoderType::Present;
  buffer.timestamps += pass->type == EncoderType::SampleTimestamp;
  const char *kind = pass->type == EncoderType::Render            ? "Render"
                     : pass->type == EncoderType::Compute         ? "Compute"
                     : pass->type == EncoderType::Blit            ? "Blit"
                     : pass->type == EncoderType::Clear           ? "Clear"
                     : pass->type == EncoderType::Resolve         ? "Resolve"
                     : pass->type == EncoderType::SpatialUpscale  ? "SpatialUpscale"
                     : pass->type == EncoderType::TemporalUpscale ? "TemporalUpscale"
                                                                  : nullptr;
  if (!kind)
    return {};
  auto seq = pass->id;
  std::string runs, targets;
  auto target = [&](const char *binding, unsigned slot, WMT::Texture texture, unsigned level, unsigned slice) {
    if (texture)
      targets +=
          diag::Line("target", profile_->id, argument_encoding_ctx.currentSeqId(), seq, binding, slot, texture.handle,
                     texture.width(), texture.height(), uint64_t(texture.pixelFormat()), level, slice);
  };
  auto run = [&](uint64_t pipeline) {
    if (!runs.empty())
      runs += '|';
    runs += str::format("Render:", pipeline, "*1*1*0");
  };
  if (pass->type == EncoderType::Render) {
    auto data = static_cast<RenderEncoderData *>(pass);
    for (unsigned i = 0; i < data->colors.size(); i++) {
      auto &color = data->colors[i];
      target("color", i, color.attachment.texture(), color.level, color.slice);
      target("resolve", i, color.resolve_attachment.texture(), color.resolve_level, color.resolve_slice);
    }
    target("depth", 0, data->depth.attachment.texture(), data->depth.level, data->depth.slice);
    target("stencil", 0, data->stencil.attachment.texture(), data->stencil.level, data->stencil.slice);
    auto &commands = argument_encoding_ctx.emulated_cmd;
    if (!data->draw_auto_marshal_tasks.empty())
      run(commands.draw_auto_arguments_marshal);
    if (!data->gs_arg_marshal_tasks.empty())
      run(commands.gs_draw_arguments_marshal);
    if (!data->ts_arg_marshal_tasks.empty())
      run(commands.ts_draw_arguments_marshal);
    auto drawn = diag::PipelineRuns(data->cmd_head, [](uint64_t pipeline) {
      return str::format("Render:", pipeline);
    });
    if (!runs.empty() && !drawn.empty())
      runs += '|';
    runs += drawn;
  } else if (pass->type == EncoderType::Compute) {
    runs = diag::PipelineRuns(static_cast<ComputeEncoderData *>(pass)->cmd_head, [](uint64_t pipeline) {
      return str::format("Compute:", pipeline);
    });
  } else if (pass->type == EncoderType::Clear) {
    auto data = static_cast<ClearEncoderData *>(pass);
    target(data->clear_dsv ? "depth-stencil" : "color", 0, data->attachment.texture(), 0, 0);
  } else if (pass->type == EncoderType::Resolve) {
    auto data = static_cast<ResolveEncoderData *>(pass);
    target("color", 0, data->src.texture(), 0, 0);
    target("resolve", 0, data->dst.texture(), 0, 0);
  } else if (pass->type == EncoderType::SpatialUpscale) {
    auto data = static_cast<SpatialUpscaleData *>(pass);
    target("input", 0, data->backbuffer, 0, 0);
    target("output", 0, data->upscaled, 0, 0);
  } else if (pass->type == EncoderType::TemporalUpscale) {
    auto data = static_cast<TemporalUpscaleData *>(pass);
    target("input", 0, data->input, 0, 0);
    target("output", 0, data->output, 0, 0);
    target("depth", 0, data->depth, 0, 0);
    target("motion", 0, data->motion_vector, 0, 0);
    target("exposure", 0, data->exposure, 0, 0);
  }
  const uint32_t count = pass->type == EncoderType::Render ? WMTRenderTimestampSamples : WMTTimestampSamplesPerStage;
  if (buffer.samples.empty() || count > profile_->samples.limit - buffer.samples.back().used)
    buffer.samples.push_back({profile_->samples.Take(device, false)});
  auto &samples = buffer.samples.back();
  uint32_t first = ~0u;
  WMTSampleBufferAttachmentInfo attachment{};
  if (samples.buffer && count <= profile_->samples.limit - samples.used) {
    first = samples.used;
    samples.used += count;
    attachment = {samples.buffer, first, first + count - 1};
  }
  buffer.passes.push_back(
      {seq, diag::Now(), kind, std::move(runs), std::move(targets), uint32_t(buffer.samples.size() - 1), first, count});
  profiled = &buffer;
  return attachment;
}

CommandQueue::CommandQueue(WMT::Device device) :
    encodeThread([this]() { this->EncodingThread(); }),
    finishThread([this]() { this->WaitForFinishThread(); }),
    device(device),
    commandQueue(device.newCommandQueue(kCommandChunkCount)),
    shared_event_listener(SharedEventListener_create()),
    event_listener_thread([this]() { SharedEventListener_start(this->shared_event_listener); }),
    staging_allocator({
        device, WMTResourceOptionCPUCacheModeWriteCombined | WMTResourceHazardTrackingModeUntracked |
                    WMTResourceStorageModeManaged, false
    }),
    copy_temp_allocator({device, WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModePrivate}),
    argbuf_allocator({
        device,
        WMTResourceHazardTrackingModeUntracked | WMTResourceCPUCacheModeWriteCombined | WMTResourceStorageModeShared
    }),
    cpu_command_allocator({}),
    reftracker_storage_allocator({}),
    cmd_library(device),
    argument_encoding_ctx(*this, device, cmd_library),
    initializer(device),
    counter_pool(device) {
  for (unsigned i = 0; i < kCommandChunkCount; i++) {
    auto &chunk = chunks[i];
    chunk.queue = this;
    chunk.reset();
  };
  event = device.newSharedEvent();

  std::string env = env::getEnvVar("DXMT_CAPTURE_FRAME");

  if (!env.empty()) {
    try {
      capture_state.scheduleNextFrameCapture(std::stoull(env));
    } catch (const std::invalid_argument &) {
    }
  }
}

CommandQueue::~CommandQueue() {
  TRACE("Destructing command queue");
  stopped.store(true);
  ready_for_encode++;
  ready_for_encode.notify_one();
  ready_for_commit++;
  ready_for_commit.notify_one();
  SharedEventListener_destroy(shared_event_listener);
  encodeThread.join();
  finishThread.join();
  for (unsigned i = 0; i < kCommandChunkCount; i++) {
    auto &chunk = chunks[i];
    chunk.reset();
  };
  event_listener_thread.join();
  TRACE("Destructed command queue");
}

void
CommandQueue::CommitCurrentChunk() {
  auto began = profile_ ? diag::Now() : 0;
  auto chunk_id = ready_for_encode.load(std::memory_order_relaxed);
  auto &chunk = chunks[chunk_id % kCommandChunkCount];
  chunk.chunk_id = chunk_id;
  chunk.chunk_event_id = GetNextEventSeqId();
  chunk.frame_ = frame_count;
  chunk.resource_initializer_event_id = initializer.flushToWait();
  auto& statistics = CurrentFrameStatistics();
  statistics.command_buffer_count++;
  if (profile_) {
    profile_->buffers[chunk_id % kCommandChunkCount].submitted = began;
    if (chunk.signal_frame_latency_fence_ != ~0ull)
      diag::Frame(profile_->id, chunk_id);
  }
#if ASYNC_ENCODING
  ready_for_encode.fetch_add(1, std::memory_order_release);
  ready_for_encode.notify_one();

  auto t0 = clock::now();
  chunk_ongoing.wait(kCommandChunkCount - 1, std::memory_order_acquire);
  chunk_ongoing.fetch_add(1, std::memory_order_relaxed);
  auto t1 = clock::now();
  statistics.commit_interval += (t1 - t0);

#else
  CommitChunkInternal(chunk, ready_for_encode.fetch_add(1, std::memory_order_relaxed));
#endif

  cpu_command_allocator.free_blocks(cpu_coherent.signaledValue());
  if (profile_)
    diag::Write(diag::Line("span", "execute", chunk_id, GetCurrentThreadId(), began, diag::Now() - began));
}

void
CommandQueue::CommitChunkInternal(CommandChunk &chunk, uint64_t seq) {

  auto pool = WMT::MakeAutoreleasePool();

  switch (capture_state.getNextAction(chunk.frame_)) {
  case CaptureState::NextAction::StartCapture: {
    WMTCaptureInfo info;
    auto capture_mgr = WMT::CaptureManager::sharedCaptureManager();
    info.capture_object = device;
    info.destination = WMTCaptureDestinationGPUTraceDocument;
    char filename[1024];
    std::time_t now;
    std::time(&now);
    std::strftime(filename, 1024, "_%H'%M'%S_%m-%d-%y.gputrace", std::localtime(&now));
    auto fileUrl = env::getUnixPath(env::getExeBaseName() + "_F." + std::to_string(chunk.frame_) + filename);
    WARN("A new capture will be saved to ", fileUrl);
    info.output_url.set(fileUrl.c_str());

    capture_mgr.startCapture(info);
    break;
  }
  case CaptureState::NextAction::StopCapture: {
    auto capture_mgr = WMT::CaptureManager::sharedCaptureManager();
    capture_mgr.stopCapture();
    break;
  }
  case CaptureState::NextAction::Nothing: {
    if (capture_state.shouldCaptureNextFrame()) {
      capture_state.scheduleNextFrameCapture(chunk.frame_ + 1);
    }
    break;
  }
  }

  auto cmdbuf = commandQueue.commandBuffer();
  chunk.attached_cmdbuf = cmdbuf;
  if (chunk.resource_initializer_event_id) {
    cmdbuf.encodeWaitForEvent(initializer.event(), chunk.resource_initializer_event_id);
  }
  auto began = profile_ ? diag::Now() : 0;
  chunk.encode(chunk.attached_cmdbuf, this->argument_encoding_ctx);
  if (profile_)
    diag::Write(diag::Line("span", "encode", seq, GetCurrentThreadId(), began, diag::Now() - began));
  if (profile_)
    profile_->buffers[seq % kCommandChunkCount].committed = diag::Now();
  cmdbuf.commit();

  ready_for_commit.fetch_add(1, std::memory_order_release);
  ready_for_commit.notify_one();
}

uint32_t
CommandQueue::EncodingThread() {
#if ASYNC_ENCODING
  env::setThreadName("dxmt-encode-thread");
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  uint64_t internal_seq = 1;
  while (!stopped.load()) {
    ready_for_encode.wait(internal_seq, std::memory_order_acquire);
    if (stopped.load())
      break;
    // perform...
    auto &chunk = chunks[internal_seq % kCommandChunkCount];
    CommitChunkInternal(chunk, internal_seq);
    internal_seq++;
  }
  TRACE("encoder thread gracefully terminates");
#endif
  return 0;
}

uint32_t
CommandQueue::WaitForFinishThread() {
  env::setThreadName("dxmt-finish-thread");
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  uint64_t internal_seq = 1;
  while (!stopped.load()) {
    ready_for_commit.wait(internal_seq, std::memory_order_acquire);
    if (stopped.load())
      break;
    auto &chunk = chunks[internal_seq % kCommandChunkCount];
    if (profile_ || chunk.attached_cmdbuf.status() <= WMTCommandBufferStatusScheduled) {
      chunk.attached_cmdbuf.waitUntilCompleted();
    }
    auto completed = profile_ ? diag::Now() : 0;
    if (chunk.attached_cmdbuf.status() == WMTCommandBufferStatusError) {
      ERR("Device error at frame ", chunk.frame_, ": ", chunk.attached_cmdbuf.error().description().getUTF8String());
    }
    if (auto logs = chunk.attached_cmdbuf.logs()) {
      for (auto &log : logs.elements()) {
        ERR("Frame ", chunk.frame_, ": ", log.description().getUTF8String());
      }
    }

    if (profile_) {
      auto &buffer = profile_->buffers[internal_seq % kCommandChunkCount];
      std::vector<std::vector<uint64_t>> clocks;
      for (auto &samples : buffer.samples) {
        auto &clock = clocks.emplace_back(samples.used, ~uint64_t(0));
        if (samples.buffer)
          samples.buffer.resolveCounterRange(0, samples.used, clock.data(), clock.size() * sizeof(uint64_t));
      }
      auto lines = diag::Line("cmdbuf", profile_->id, internal_seq, buffer.committed, completed,
                              chunk.attached_cmdbuf.gpuStartTime(), chunk.attached_cmdbuf.gpuEndTime(),
                              buffer.submitted, buffer.passes.size(), buffer.presents, buffer.timestamps);
      for (auto &pass : buffer.passes) {
        auto line = diag::Line("pass", profile_->id, internal_seq, pass.id, pass.kind, pass.encoding, pass.runs);
        line.pop_back();
        for (uint32_t i = 0; i < pass.count; i++)
          line += str::format('\t', pass.first == ~0u ? ~uint64_t(0) : clocks[pass.buffer][pass.first + i]);
        lines += line + '\n' + pass.targets;
      }
      diag::Write(lines);
      for (auto &samples : buffer.samples)
        if (samples.buffer)
          profile_->samples.Return(std::move(samples.buffer));
      buffer = {};
    }

    if (chunk.completed)
      chunk.completed();
    if (chunk.signal_frame_latency_fence_ != ~0ull)
      frame_latency_fence_.signal(chunk.signal_frame_latency_fence_);

    chunk.reset();
    cpu_coherent.signal(internal_seq);
    chunk_ongoing.fetch_sub(1, std::memory_order_release);
    chunk_ongoing.notify_one();

    staging_allocator.free_blocks(internal_seq);
    copy_temp_allocator.free_blocks(internal_seq);
    argbuf_allocator.free_blocks(internal_seq);

    internal_seq++;
  }
  TRACE("finishing thread gracefully terminates");
  return 0;
}

void CommandQueue::Retain(uint64_t seq, Allocation* allocation) {
  auto &chunk = chunks[seq % kCommandChunkCount];
  auto &tracker = chunk.ref_tracker;
  constexpr size_t block_size = decltype(reftracker_storage_allocator)::block_size;
  while (unlikely(!tracker.track(allocation))) {
    auto [temp_buffer, _] = reftracker_storage_allocator.allocate(seq, cpu_coherent.signaledValue(), block_size, 1);
    tracker.addStorage(temp_buffer.ptr, block_size);
  }
};

} // namespace dxmt
