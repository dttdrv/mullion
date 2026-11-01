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

#include "mullion_texture.hpp"
#include <cstdint>

namespace mullion {

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
  ComputeParts,
  AccelerationStructure,
};

class MTLD3D12QueryHeap;

struct Stamp {
  MTLD3D12QueryHeap *heap;
  uint32_t query;
  Stamp *next;
};

// how a pass is ordered with the passes before it (MULLION_D3D12_PASS_OVERLAP, MTLD3D12CommandQueue's PassSync):
// Ordered waits for all of them; Independent, a draw's or dispatch's pass right after another with nothing that orders
// work between (a barrier, a copy, a clear, a query...), waits only for what came before that ordering point; Shared
// is an Independent render pass that draws to what an earlier one since the ordering point drew to, whose pixels
// then wait for that one's
enum class PassOverlap : uint8_t {
  Ordered,
  Independent,
  Shared,
};

struct EncoderData {
  EncoderType type;
  EncoderData *next = nullptr;
  uint64_t id;
  Stamp *stamps = nullptr;
  // the path of the application's events the pass began in, each name ended by diag::kEventEnd (diag::events)
  const char *event_path = nullptr;
  PassOverlap overlap = PassOverlap::Ordered;
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
  // the pass's sample pattern (WMTRenderPassInfo)
  uint8_t programmed_samples = 0;
  std::array<WMTSamplePosition, std::size(WMTRenderPassInfo{}.sample_positions)> sample_positions{};
  uint16_t render_target_array_length;
  uint32_t render_target_height;
  uint32_t render_target_width;
  // a pass without attachments begun by a draw that rasterizes nothing (MTLD3D12GraphicsPipelineState's rasterless),
  // a pixel in size: one that is 16384 by 16384 costs the GPU most of a millisecond. a draw that rasterizes begins
  // another
  bool rasterless = false;
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
  // where the resolvers of the pass's indirect commands leave those they left out: for a geometry command its object
  // threadgroups, for stream output (`scratch`) the scratch bytes it needed
  struct Omitted {
    WMT::Buffer buffer;
    uint64_t offset;
    uint64_t words;
    Omitted *next;
    bool scratch = false;
  } *omitted = nullptr;
  uint8_t dsv_planar_flags;
  uint8_t dsv_readonly_flags;
  uint8_t render_target_count;
  bool use_visibility_result = 0;
  obj_handle_t visibility_buffer = 0; // where active occlusion queries count
  // of WMT::kPassRectangles
  uint32_t scissors = 0, viewports = 0;
  bool use_tessellation = 0;
  bool use_geometry = 0;
  // a clear pass's: what another clear needs to join it (JoinableClear)
  uint8_t clear_sample_count = 0;
};

struct BlitEncoderData : EncoderData {
  wmtcmd_blit_nop cmd_head;
  wmtcmd_base *cmd_tail;
};

struct ComputeEncoderData : EncoderData {
  wmtcmd_compute_nop cmd_head;
  wmtcmd_base *cmd_tail;
  // it dispatches rays, whose kernel reads the sparse buffer table even when there is none
  bool rays = false;
};

// bindings recorded once; the queue submits each part before making the next
struct ComputePartsEncoderData : ComputeEncoderData {
  WMTSize size;
  const D3D12_DISPATCH_ARGUMENTS *indirect = nullptr;
  uint64_t at_once;
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
struct ResolveTimestampsData : EncoderData {
  MTLD3D12QueryHeap *heap;
  ID3D12Resource *readback = nullptr;
  void *memory = nullptr;
  uint32_t start;
  uint32_t count;
  WMT::Buffer dst;
  uint64_t dst_offset;
};

}; // namespace mullion
