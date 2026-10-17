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

#include "dxmt_texture.hpp"
#include <cstdint>

namespace dxmt {

enum class EncoderType {
  Null,
  Clear,
  Render,
  Blit,
  Compute,
  Resolve,
  ResolveTimestamps,
  Predicate,
  Later,
  // the work so far completes before what follows starts
  Settle,
  AccelerationStructure,
};

class MTLD3D12QueryHeap;

struct Stamp {
  MTLD3D12QueryHeap *heap;
  uint32_t query;
  Stamp *next;
};

struct EncoderData {
  EncoderType type;
  EncoderData *next = nullptr;
  uint64_t id;
  Stamp *stamps = nullptr;
};

struct RenderEncoderColorAttachmentData {
  TextureViewRef attachment;
  enum WMTLoadAction load_action;
  enum WMTStoreAction store_action;
  uint16_t level;
  uint16_t slice;
  uint32_t depth_plane;
  struct WMTClearColor clear_color;
  TextureViewRef resolve_attachment;
  uint16_t resolve_level;
  uint16_t resolve_slice;
  uint32_t resolve_depth_plane;
};

struct RenderEncoderDepthAttachmentData {
  TextureViewRef attachment;
  enum WMTLoadAction load_action;
  enum WMTStoreAction store_action;
  uint16_t level;
  uint16_t slice;
  uint32_t depth_plane;
  float clear_depth;
};

struct RenderEncoderStencilAttachmentData {
  TextureViewRef attachment;
  enum WMTLoadAction load_action;
  enum WMTStoreAction store_action;
  uint16_t level;
  uint16_t slice;
  uint32_t depth_plane;
  uint8_t clear_stencil;
};

struct RenderEncoderData : EncoderData {
  EncoderData *previous;
  std::array<RenderEncoderColorAttachmentData, 8> colors;
  RenderEncoderDepthAttachmentData depth;
  RenderEncoderStencilAttachmentData stencil;
  uint8_t default_raster_sample_count;
  uint16_t render_target_array_length;
  uint32_t render_target_height;
  uint32_t render_target_width;
  wmtcmd_render_nop cmd_head{WMTRenderCommandNop};
  wmtcmd_base *cmd_tail = (wmtcmd_base *)&cmd_head;
  // compute commands that run before the pass: the resolvers of its indirect commands, whose commands it then runs
  wmtcmd_compute_nop before_head{WMTComputeCommandNop};
  wmtcmd_base *before_tail = (wmtcmd_base *)&before_head;
  // MULLION_D3D12_GPU_ERRORS: where each of the pass's indirect commands' resolvers keeps its largest numbers
  struct Most {
    const uint32_t *words;
    const struct IndirectRenderCommandData *call;
    Most *next;
  } *most;
  uint8_t dsv_planar_flags;
  uint8_t dsv_readonly_flags;
  uint8_t render_target_count;
  bool use_visibility_result = 0;
  obj_handle_t visibility_buffer = 0; // where active occlusion queries count
  // of WMT::kPassRectangles
  uint32_t scissors = 0, viewports = 0;
  bool use_tessellation = 0;
  bool use_geometry = 0;
};

struct BlitEncoderData : EncoderData {
  wmtcmd_blit_nop cmd_head;
  wmtcmd_base *cmd_tail;
};

struct ComputeEncoderData : EncoderData {
  wmtcmd_compute_nop cmd_head;
  wmtcmd_base *cmd_tail;
};
struct AccelerationStructure;
struct AccelerationStructureInputs;

// a build or a copy fills a structure when the queue gets to it: its inputs are then `kept`, or those `from` has
struct AccelerationStructureFilled {
  AccelerationStructure *structure;
  AccelerationStructureInputs *kept;
  AccelerationStructure *from;
  AccelerationStructureFilled *next;
};
struct AccelerationStructureEncoderData : EncoderData {
  wmtcmd_accelerationstructure_nop cmd_head;
  wmtcmd_base *cmd_tail;
  // in the order of the pass's commands
  AccelerationStructureFilled *filled, **filled_tail;
};
struct ResolveEncoderData : EncoderData {
  TextureViewRef src;
  TextureViewRef dst;
};

// the passes up to `end` are left out when the predication region's kernel wrote that it skips
struct PredicateEncoderData : EncoderData {
  const uint32_t *skips;
  EncoderData *end;
};

// a command on acceleration structures that cannot be recorded when the application records it: a deserializing
// copy, whose source is in memory only once the work before has run, or a command on a structure that such a copy
// makes. the queue records it when it gets there, on a list of its own
struct LaterEncoderData : EncoderData {
  enum { Copy, Information, Build } command;
  // CopyRaytracingAccelerationStructure
  D3D12_GPU_VIRTUAL_ADDRESS destination, source;
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE mode;
  // EmitRaytracingAccelerationStructurePostbuildInfo, and BuildRaytracingAccelerationStructure's information
  const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *information;
  UINT information_count;
  const D3D12_GPU_VIRTUAL_ADDRESS *sources;
  UINT source_count;
  // BuildRaytracingAccelerationStructure, its geometries in an array
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build;
};

// timestamp queries' samples, copied to a buffer: Metal makes them readable only once their command buffer completes.
// a readback heap's buffer is one the GPU never reads, so the CPU writes it then, through `readback` at its memory
// a pipeline statistics query a readback resolve has the CPU write: the counts known when its work was recorded, and
// the visibility counts of its pass segments with a pixel shader (PSInvocations)
struct StatisticsCount {
  uint64_t counts[14];
  uint32_t query;
  const uint64_t *const *slots;
  uint32_t slot_count;
};

struct ResolveTimestampsData : EncoderData {
  MTLD3D12QueryHeap *heap;
  ID3D12Resource *readback = nullptr;
  void *memory = nullptr;
  uint32_t start;
  uint32_t count;
  WMT::Buffer dst;
  uint64_t dst_offset;
  // a pipeline statistics heap's queries the list ended; the others are copied from the heap
  const StatisticsCount *statistics = nullptr;
  uint32_t statistics_count = 0;
};

}; // namespace dxmt
