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
};

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

  small_vector<EncoderData, 64> encoder_lists_;

  small_vector<WMT::Reference<WMT::IndirectCommandBuffer>, 4> icb_;
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
        exhausted_ = true;
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

  std::tuple<WMT::Buffer, uint64_t>
  AllocateTempBuffer(size_t Size, size_t Alignment) {
    assert(copy_temp_version_);
    auto [block, offset] = copy_temp_allocator_.allocate(copy_temp_version_, copy_temp_version_ - 1, Size, Alignment);
    return {block.buffer, offset};
  }

  IndirectComputeCommandData *EncodeIndirectComputeCommand(MTLD3D12CommandSignature *pCmdSig, MTLD3D12ComputePipelineState *pPSO, size_t MaxCount);

  // `Pipeline` is the variant of pPSO that the commands draw with
  IndirectRenderCommandData *EncodeIndirectRenderCommand(MTLD3D12CommandSignature *pCmdSig, MTLD3D12GraphicsPipelineState *pPSO, WMT::RenderPipelineState Pipeline, size_t MaxCount, bool tessellation);
};

} // namespace dxmt