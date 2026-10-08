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

#include "d3d12_pageable.hpp"
#include <deque>
#include "airconv_ray.h"
#include "dxmt_command_clear.hpp"
#include "dxmt_command_feedback.hpp"
#include "dxmt_ring_bump_allocator.hpp"

namespace dxmt {

inline std::size_t
align_forward_adjustment(const void *const ptr, const std::size_t &alignment) noexcept {
  const auto iptr = reinterpret_cast<std::uintptr_t>(ptr);
  const auto aligned = (iptr - 1u + alignment) & -alignment;
  return aligned - iptr;
}

inline void *
ptr_add(const void *const p, const std::uintptr_t &amount) noexcept {
  return reinterpret_cast<void *>(reinterpret_cast<std::uintptr_t>(p) + amount);
}

struct IndirectComputeCommandData {
  uint64_t cmd_buf;
  uint64_t max_count;
  uint64_t max_count_buffer;
  uint64_t argument_buffer;
  uint64_t static_samplers;
  uint64_t rootsig_qwords;
  uint32_t rootsig_qwords_stride;
  uint32_t tgsize_x;
  uint32_t tgsize_y;
  uint32_t tgsize_z;
  uint64_t rays;
  uint32_t ray_flags;
};

// what a command signature's resolver makes of an indirect DispatchRays command: the arguments of the pipeline's
// kernel, and the threadgroups Metal runs for it, none for a command past the count
struct IndirectRays {
  SM50_RAY_DISPATCH dispatch;
  uint32_t threadgroups[3];
};

// a command of a tessellated, geometry or mesh ExecuteIndirect on a GPU without mesh commands in indirect command
// buffers (they come with Apple9): what its object stage reads, laid out as a direct draw's TessellationInputs and
// arguments are, and the threadgroups of its draw. the resolver writes it
struct IndirectMeshDraw {
  D3D12_INDEX_BUFFER_VIEW view;
  alignas(16) uint32_t control_points;
  alignas(16) uint32_t arguments[sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) / sizeof(uint32_t)];
  uint32_t threadgroups[3];
};

struct IndirectRenderCommandData {
  uint64_t cmd_buf;
  uint64_t max_count;
  uint64_t max_count_buffer;
  uint64_t argument_buffer;
  uint64_t static_samplers;
  uint64_t rootsig_qwords;
  uint32_t rootsig_qwords_stride;
  uint32_t primitive_type;
  uint64_t vertex_buffer;
  uint64_t index_buffer;
  DXGI_FORMAT index_buffer_format;
  uint32_t vertex_argbuf_stride;
  // tessellation: the command list's TessellationInputs (index buffer view and patch size) the object stage reads, the
  // patch's control points, and the threads a patch takes. threads_per_patch is zero for ordinary pipelines
  uint64_t index_buffer_view;
  uint32_t control_points;
  uint32_t threads_per_patch;
  // the object threadgroups that share a patch group's mesh threadgroups (tessellation_parts)
  uint32_t tessellation_parts;
  // a geometry pipeline: the threads of its object threadgroups and the vertices each starts (get_gs_vertex_count),
  // zero for other pipelines. the object stage reads the index buffer view as under tessellation
  uint32_t geometry_threads;
  uint32_t geometry_increment;
  // a mesh shader pipeline: its amplification and mesh shaders' threadgroup sizes, zero for other pipelines
  uint32_t object_threads[3];
  uint32_t mesh_threads[3];
  // the vertex buffer slots the pipeline's input layout uses, which the vertex buffer table has entries for
  uint32_t vertex_slots;
  // the bound index buffer view's size in bytes: a command draws no indices past it
  uint32_t index_buffer_size;
  // DXMT_D3D12_GPU_ERRORS: where the resolver keeps the largest numbers its commands ask for (kMostWords words)
  uint64_t most;
  // the commands' IndirectMeshDraws, where the resolver writes those instead of commands; zero otherwise
  uint64_t draws;
  // indexed commands of ordinary pipelines are two commands each: the indices the view has, then, for the indices
  // past it, as many of these zero indices of 16 bits (D3D11.3 8.19.2: "the return is 0"), `zero_count` at most
  uint64_t zeros;
  uint32_t zero_count;
};

// the words of IndirectRenderCommandData::most: five numbers of the commands and how many commands there were
constexpr unsigned kMostWords = 6;

class MTLD3D12CommandAllocatorImpl : public MTLD3D12Pageable<MTLD3D12CommandAllocator> {
  friend class MTLD3D12GraphicsCommandListImpl;
  friend struct SimpleCommandContext<MTLD3D12CommandAllocatorImpl>;

  D3D12_COMMAND_LIST_TYPE type_;

  // where in an arena the lists record next, in the last of the runs the allocator holds of it
  struct Cursor {
    size_t offset = 0, end = 0;
    std::vector<RecordingArena::Run> runs;
  };
  // the device's arenas (RecordingArena): commands, and what the GPU reads
  void *cpu_heap_ = nullptr;
  Cursor cpu_cursor_;
  // a list asked for more than a heap has: it fails when it is closed
  bool exhausted_ = false;
  std::deque<std::vector<char>> spill_;

  WMT::Reference<WMT::Buffer> gpu_heap_buffer_;
  void *gpu_heap_ = nullptr;
  Cursor gpu_cursor_;
  uint64_t gpu_heap_buffer_address_;

  EncoderData *encoder_last;
  EncoderData *encoder_current;
  size_t encoder_count_;

  // a list keeps its root's address until the queue has committed it, while later lists add roots
  std::deque<EncoderData> encoder_lists_;

  small_vector<WMT::Reference<WMT::IndirectCommandBuffer>, 4> icb_;
  uint64_t visibility_address_ = 0;
  size_t visibility_used_ = kVisibilityWindow;
  // the acceleration structures the lists' commands name, some of them replaced at their addresses since
  std::vector<std::shared_ptr<AccelerationStructure>> acceleration_structures_;
  // and the inputs they kept, which a rebuilt structure replaces
  std::vector<std::shared_ptr<const AccelerationStructureInputs>> acceleration_structure_inputs_;

  ClearUAV<MTLD3D12CommandAllocatorImpl> clear_uav_;
  TranscodeFeedback<MTLD3D12CommandAllocatorImpl> transcode_feedback_;
  ClearRTV<MTLD3D12CommandAllocatorImpl> clear_rtv_;

  RingBumpState<GpuPrivateBufferBlockAllocator> copy_temp_allocator_;
  uint64_t copy_temp_version_;

public:
  MTLD3D12CommandAllocatorImpl(MTLD3D12Device *pDevice, D3D12_COMMAND_LIST_TYPE Type);

  ~MTLD3D12CommandAllocatorImpl() {
    ReleaseRuns();
  }

  // gives what the lists recorded into back to the device
  void
  ReleaseRuns() {
    for (auto [arena, cursor] : {std::pair{&device_->recording.commands, &cpu_cursor_},
                                 std::pair{&device_->recording.arguments, &gpu_cursor_}}) {
      for (auto &run : cursor->runs)
        arena->Release(run);
      *cursor = {};
    }
  }

  HRESULT
  Initialize();

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject);

  HRESULT STDMETHODCALLTYPE Reset();

  HRESULT STDMETHODCALLTYPE CreateCommandList(
      UINT NodeMask, D3D12_COMMAND_LIST_TYPE Type, ID3D12PipelineState *pInitialPipelineState, REFIID riid,
      void **ppCommandList
  );

  void
  InvalidateCurrentPass() {
    if (!encoder_current)
      return;
    encoder_last->next = encoder_current;
    encoder_last = encoder_current;

    encoder_current = nullptr;
    encoder_count_++;
  }

  HRESULT
  StartRecord(EncoderData **pStartEncoder) {
    if (encoder_last)
      return E_INVALIDARG;
    *pStartEncoder = encoder_last = &encoder_lists_.emplace_back(EncoderType::Null, nullptr);
    return S_OK;
  }

  HRESULT
  EndRecord(size_t *pEncoderCount) {
    if (!encoder_last)
      return E_FAIL;
    if (encoder_current)
      InvalidateCurrentPass();
    encoder_last = nullptr;
    *pEncoderCount = encoder_count_;
    encoder_count_ = 0;
    return exhausted_ ? E_OUTOFMEMORY : S_OK;
  }

  // where in an arena `Length` bytes are: after the cursor, or in a new run of the arena's when the cursor's has no
  // room. an arena without room ends the list (EndRecord, and Close cuts what it recorded off): what it records from
  // then on goes to memory of the allocator's own, which no list holds, written over again and again
  std::optional<size_t>
  Allocate(RecordingArena &arena, Cursor &cursor, size_t Length, size_t Alignment) {
    auto aligned = cursor.offset + align_forward_adjustment((void *)cursor.offset, Alignment);
    if (unlikely(Length > cursor.end - std::min(aligned, cursor.end))) {
      // a run starts at a block, which is aligned for anything recorded
      auto run = arena.Acquire(Length);
      if (!run) {
        if (!std::exchange(exhausted_, true))
          ERR("CommandAllocator: the recording arena has no room for ", Length, " bytes more; the list is cut off");
        if (spill_.empty() || spill_.back().size() < Length + Alignment)
          spill_.emplace_back(Length + Alignment);
        auto at = (size_t)spill_.back().data();
        return at + align_forward_adjustment((void *)at, Alignment) - (size_t)arena.base;
      }
      cursor.runs.push_back(*run);
      aligned = run->offset;
      cursor.end = run->offset + run->length;
    }
    cursor.offset = aligned + Length;
    return aligned;
  }

  void *
  AllocateCPUHeap(size_t Length, size_t Alignment) {
    auto at = Allocate(device_->recording.commands, cpu_cursor_, Length, Alignment);
    return at ? ptr_add(cpu_heap_, *at) : nullptr;
  }

  template <typename T>
  T *
  AllocatePass() {
    auto p = (new (AllocateCPUHeap(sizeof(T), alignof(T))) T());
    // what logs call the pass
    static std::atomic<uint64_t> passes;
    p->id = ++passes;
    encoder_current = p;
    return p;
  };

  template <typename T>
  T *
  AllocateCommandData(size_t Count) {
    return (T *)AllocateCPUHeap(sizeof(T) * Count, alignof(T));
  };

  template <typename cmd_struct>
  cmd_struct &
  EncodeRenderCommand() {
    assert(encoder_current->type == EncoderType::Render);
    auto encoder = static_cast<RenderEncoderData *>(encoder_current);
    auto storage = (cmd_struct *)AllocateCPUHeap(sizeof(cmd_struct), 16);
    encoder->cmd_tail->next.set(storage);
    encoder->cmd_tail = (wmtcmd_base *)storage;
    storage->next.set(nullptr);
    return *storage;
  }

  template <typename cmd_struct>
  cmd_struct &
  EncodeBeforeRender() {
    assert(encoder_current->type == EncoderType::Render);
    auto encoder = static_cast<RenderEncoderData *>(encoder_current);
    auto storage = (cmd_struct *)AllocateCPUHeap(sizeof(cmd_struct), 16);
    encoder->before_tail->next.set(storage);
    encoder->before_tail = (wmtcmd_base *)storage;
    storage->next.set(nullptr);
    return *storage;
  }

  template <typename cmd_struct>
  cmd_struct &
  EncodeBlitCommand() {
    assert(encoder_current->type == EncoderType::Blit);
    auto encoder = static_cast<BlitEncoderData *>(encoder_current);
    auto storage = (cmd_struct *)AllocateCPUHeap(sizeof(cmd_struct), 16);
    encoder->cmd_tail->next.set(storage);
    encoder->cmd_tail = (wmtcmd_base *)storage;
    storage->next.set(nullptr);
    return *storage;
  }

  template <typename cmd_struct>
  cmd_struct &
  EncodeComputeCommand() {
    assert(encoder_current->type == EncoderType::Compute);
    auto encoder = static_cast<ComputeEncoderData *>(encoder_current);
    auto storage = (cmd_struct *)AllocateCPUHeap(sizeof(cmd_struct), 16);
    encoder->cmd_tail->next.set(storage);
    encoder->cmd_tail = (wmtcmd_base *)storage;
    storage->next.set(nullptr);
    return *storage;
  }

  template <typename cmd_struct>
  cmd_struct &
  EncodeAccelerationStructureCommand() {
    assert(encoder_current->type == EncoderType::AccelerationStructure);
    auto encoder = static_cast<AccelerationStructureEncoderData *>(encoder_current);
    auto storage = (cmd_struct *)AllocateCPUHeap(sizeof(cmd_struct), 16);
    encoder->cmd_tail->next.set(storage);
    encoder->cmd_tail = (wmtcmd_base *)storage;
    storage->next.set(nullptr);
    return *storage;
  }

  std::tuple<void *, size_t>
  AllocateGPUHeap(size_t Length, size_t Alignment) {
    auto at = Length ? Allocate(device_->recording.arguments, gpu_cursor_, Length, Alignment) : std::nullopt;
    return {at ? ptr_add(gpu_heap_, *at) : nullptr, at.value_or(0)};
  }

  // a render pass stores the counts of its first 4096 counting segments; later ones it adds to what the buffer held,
  // and far ones it does not write (measured on an Apple10 GPU; Apple documents the offset's limit alone, "Maximum
  // visibility query offset" in the Metal feature set tables). so occlusion queries count in windows of that many
  // slots, each a buffer, and a pass counts in one window: the window's buffer, the next slot's GPU address, and
  // its offset in the window. without a window the list fails when it is closed
  static constexpr size_t kVisibilitySegments = 4096;
  static constexpr size_t kVisibilityWindow = (1 + kVisibilitySegments) * sizeof(uint64_t);
  std::vector<WMT::Reference<WMT::Buffer>> visibility_;
  std::tuple<WMT::Buffer, uint64_t, size_t>
  VisibilitySlot() {
    if (visibility_used_ == kVisibilityWindow) {
      WMTBufferInfo info;
      info.memory.set(nullptr);
      info.length = kVisibilityWindow;
      info.options = WMTResourceHazardTrackingModeUntracked;
      visibility_.push_back(device_->GetMTLDevice().newBuffer(info));
      exhausted_ |= !visibility_.back();
      visibility_address_ = info.gpu_address;
      // offset 0 is the one counting is turned off at, and a pass takes an offset once
      visibility_used_ = sizeof(uint64_t);
    }
    auto used = std::exchange(visibility_used_, visibility_used_ + sizeof(uint64_t));
    return {visibility_.back(), visibility_address_ + used, used};
  }

  std::tuple<WMT::Buffer, uint64_t>
  AllocateTempBuffer(size_t Size, size_t Alignment) {
    assert(copy_temp_version_);
    auto [block, offset] = copy_temp_allocator_.allocate(copy_temp_version_, copy_temp_version_ - 1, Size, Alignment);
    return {block.buffer, offset};
  }

  IndirectComputeCommandData *EncodeComputeResolver(MTLD3D12CommandSignature *pCmdSig, size_t MaxCount);
  IndirectComputeCommandData *EncodeIndirectComputeCommand(MTLD3D12CommandSignature *pCmdSig, MTLD3D12ComputePipelineState *pPSO, size_t MaxCount);

  // `Pipeline` is the variant of pPSO that the commands draw with
  IndirectRenderCommandData *EncodeIndirectRenderCommand(MTLD3D12CommandSignature *pCmdSig, MTLD3D12GraphicsPipelineState *pPSO, WMT::RenderPipelineState Pipeline, size_t MaxCount, bool tessellation);
};

} // namespace dxmt