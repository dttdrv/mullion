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

#include <atomic>
#include <bit>
#include <functional>
#include "d3d12_command_allocator.hpp"
#include "d3d12_acceleration_structure.hpp"
#include "airconv_ray.h"
#include "com/com_pointer.hpp"
#include "mullion_format.hpp"
#include "mullion_geometry.hpp"
#include "mullion_command.hpp"
#include <span>
#include <cmath>
#include "util_env.hpp"
#include "d3d12_interfaces.hpp"
#include "mullion_scaler.hpp"
#include "config/config.hpp"

namespace mullion {

enum class DirtyState {
  VertexBuffer,
  GraphicsRootArguments,
  GraphicsRootSignature,
  Viewport,
  ScissorRect,
  ComputeRootArguments,
  ComputeRootSignature,
  BlendFactor,
  StencilRef,
  DepthBounds,
  GraphicsPipelineState,
  ComputePipelineState,
};

enum class DrawCallStatus {
  Invalid,
  Ordinary,
};

inline bool
to_metal_primitive_type(D3D12_PRIMITIVE_TOPOLOGY topo, WMTPrimitiveType &primitive, uint32_t &control_point_num) {
  control_point_num = 0;
  switch (topo) {
  case D3D_PRIMITIVE_TOPOLOGY_POINTLIST:
    primitive = WMTPrimitiveTypePoint;
    break;
  case D3D_PRIMITIVE_TOPOLOGY_LINELIST:
    primitive = WMTPrimitiveTypeLine;
    break;
  case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:
    primitive = WMTPrimitiveTypeLineStrip;
    break;
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
    primitive = WMTPrimitiveTypeTriangle;
    break;
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
    primitive = WMTPrimitiveTypeTriangleStrip;
    break;
  case D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ:
  case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ:
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ:
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ:
    // geometry
    primitive = WMTPrimitiveTypePoint;
    break;
  case D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_2_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_5_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_6_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_7_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_8_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_9_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_10_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_11_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_12_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_13_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_14_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_15_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_16_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_17_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_18_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_19_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_20_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_21_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_22_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_23_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_24_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_25_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_26_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_27_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_28_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_29_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_30_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_31_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST:
    primitive = WMTPrimitiveTypePoint;
    control_point_num = topo - 32;
    break;
  default:
    return false;
  }
  return true;
}

// `Graphics`CommandList is a really confusing name
class MTLD3D12GraphicsCommandListImpl : public MTLD3D12DeviceChild<MTLD3D12GraphicsCommandList> {

  Com<MTLD3D12CommandAllocatorImpl, false> allocator_;

  /* state */

  Flags<DirtyState> dirty_state_;
  D3D12_COMMAND_LIST_TYPE type_;
  HRESULT recording_error_ = S_OK;
  // the resolves the render pass runs when it ends
  std::vector<std::pair<D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_PARAMETERS, D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_SUBRESOURCE_PARAMETERS>>
      pass_resolves_;
  // the textures the render pass discards when it ends (D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_DISCARD, or a resolve's
  // source it need not preserve)
  std::vector<Texture *> pass_discards_;

  std::array<D3D12_VERTEX_BUFFER_VIEW, D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> vertex_buffers_;

  uint64_t index_buffer_address;
  WMT::Buffer index_buffer;
  WMTIndexType index_type;
  uint64_t index_offset;
  D3D12_INDEX_BUFFER_VIEW index_view_{}; // what tessellation's object stage reads indices through
  // the buffer the last address was found in and the addresses that find it too (LookupBufferByVA's pEnd): index
  // buffers and immediate writes mostly look up one buffer after another. a buffer the list uses lives until it has
  // run, so the entry stays right until Reset, or an aliasing barrier, after which another may take the addresses
  BufferAllocation *va_found_ = nullptr;
  uint64_t va_found_end_ = 0;

  BufferAllocation *
  LookupBuffer(D3D12_GPU_VIRTUAL_ADDRESS VA, uint64_t *pOffset) {
    if (va_found_ && VA >= va_found_->gpuAddress() && VA < va_found_end_) {
      *pOffset = VA - va_found_->gpuAddress();
      return va_found_;
    }
    uint64_t end;
    auto allocation = device_->LookupBufferByVA(VA, pOffset, &end);
    if (allocation && VA < end) {
      va_found_ = allocation;
      va_found_end_ = end;
    }
    return allocation;
  }
  D3D12_STREAM_OUTPUT_BUFFER_VIEW so_views_[4]{};
  // per stream, the active statistics query's result, or 0
  uint64_t so_statistics_[4]{};

  UINT num_rtvs;
  MTL_RENDER_TARGET_DESC rtvs[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT];
  MTL_RENDER_TARGET_DESC dsv;

  D3D12_PRIMITIVE_TOPOLOGY topology_;

  UINT num_viewports;
  D3D12_VIEWPORT
  viewports[D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {{}};
  // where the pass's viewports' depth ranges and raster scale are in the GPU heap, for indirect commands to bind
  uint64_t depth_ranges_ = 0, raster_scale_ = 0;
  // what raster_scale_ holds, which the parts of a mesh grid copy (DispatchMesh)
  uint32_t raster_scale_words_[SM50_RASTER_SCALE_WORDS] = {};

  UINT num_scissors;
  D3D12_RECT
  scissors[D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {{}};

  Com<MTLD3D12GraphicsPipelineState, false> pso_graphics_;
  Com<MTLD3D12RootSignature, false> rootsig_graphics_;
  uint64_t rootarg_graphics_staging_[MTLD3D12RootSignature::kMaxUploadQwords];

  Com<MTLD3D12ComputePipelineState, false> pso_compute_;
  // SetPipelineState1's, which DispatchRays runs
  Com<MTLD3D12StateObject> state_object_;
  Com<MTLD3D12RootSignature, false> rootsig_compute_;
  uint64_t rootarg_compute_staging_[MTLD3D12RootSignature::kMaxUploadQwords];

  // GPU addresses of the bound CBV/SRV/UAV and sampler heaps (SetDescriptorHeaps)
  uint64_t resource_heap_ = 0, sampler_heap_ = 0;

  // a bundle keeps its calls, and ExecuteBundle replays them on the calling list: they read the state the caller set and
  // leave theirs set, as bundles inherit and pass state
  std::vector<std::function<void(MTLD3D12GraphicsCommandList *)>> bundle_;

  // callers test Bundled() before Record(...), so a direct list builds no capture (vector copies, references) per call
  bool
  Bundled() const {
    return type_ == D3D12_COMMAND_LIST_TYPE_BUNDLE;
  }

  template <typename Call>
  bool
  Record(Call &&call) {
    if (type_ != D3D12_COMMAND_LIST_TYPE_BUNDLE)
      return false;
    bundle_.emplace_back(std::forward<Call>(call));
    return true;
  }

  // where a root signature's arguments were last uploaded in this recording, in which of the buffers over what the
  // GPU reads: a pass that starts with the arguments as they were binds that copy again. no buffer once an argument
  // or the root signature changes
  struct UploadedRootArguments {
    obj_handle_t buffer = 0;
    uint64_t offset = 0;
  };
  UploadedRootArguments rootarg_graphics_uploaded_, rootarg_compute_uploaded_;

  // an argument of `dirty`'s root signature is no longer what was uploaded: the next draw or dispatch uploads them
  void
  RootArgumentsChanged(DirtyState dirty) {
    dirty_state_.set(dirty);
    (dirty == DirtyState::GraphicsRootArguments ? rootarg_graphics_uploaded_ : rootarg_compute_uploaded_).buffer = 0;
  }

  // the uploaded arguments of a root signature, which are uploaded first when they have changed since. a recording
  // that keeps what each pass read (GPURecording) uploads them for every pass
  UploadedRootArguments
  UploadRootArguments(MTLD3D12RootSignature *rootsig, const uint64_t *staging, UploadedRootArguments &uploaded) {
    if (!uploaded.buffer || allocator_->records_) {
      uploaded.offset = EncodeRootArgument(rootsig, staging);
      uploaded.buffer = obj_handle_t(allocator_->gpu_heap_buffer_);
    }
    return uploaded;
  }

  /* a root signature that indexes the heaps directly reads their addresses from its root arguments */
  void
  WriteHeapArguments(MTLD3D12RootSignature *rootsig, uint64_t *staging, DirtyState dirty) {
    if (!rootsig)
      return;
    auto write = [&](uint32_t qword, uint64_t value) {
      if (staging[qword] == value)
        return;
      staging[qword] = value;
      RootArgumentsChanged(dirty);
    };
    if (rootsig->ResourceHeapQword != ~0u)
      write(rootsig->ResourceHeapQword, resource_heap_);
    if (rootsig->SamplerHeapQword != ~0u)
      write(rootsig->SamplerHeapQword, sampler_heap_);
    write(rootsig->AtomicLocksQword, device_->GetAtomicLocks());
  }

  // a root constant buffer view of address 0 is a null view, and one the application has not set since the root
  // signature was is undefined to read: both read the device's buffer of zeros, as a null view in a descriptor heap
  // does. root SRVs and UAVs keep address 0: a shader finds a null acceleration structure by it, and a UAV's writes
  // would land in the zeros every null view reads
  void
  NullRootViews(MTLD3D12RootSignature *rootsig, uint64_t *staging, DirtyState dirty) {
    for (uint32_t i = 0; i < rootsig->ParameterSlots; i++) {
      auto &slot = staging[rootsig->SlotQwordOffsets[i]];
      if (rootsig->SlotTypes[i] == D3D12_ROOT_PARAMETER_TYPE_CBV && !slot) {
        slot = device_->GetNullBuffer();
        RootArgumentsChanged(dirty);
      }
    }
  }

  FLOAT blend_factor_[4];
  UINT8 stencil_ref_;
  UINT8 back_stencil_ref_;
  // SetSamplePositions' pattern, of programmed_samples_ samples a pixel
  uint8_t programmed_samples_;
  decltype(RenderEncoderData::sample_positions) sample_positions_;
  // RSSetDepthBias's bias, clamp and slope scale, until a pipeline state is set again
  std::optional<std::array<FLOAT, 3>> depth_bias_;
  // OMSetDepthBounds' range, NaN as 0; the pass has a range set when depth_bounds_set_
  FLOAT depth_bounds_[2];
  bool depth_bounds_set_ = false;
  // state last encoded in the open render/compute pass, to skip redundant Metal calls
  obj_handle_t pass_dsso_ = 0;
  bool pass_rasterizer_set_ = false;
  struct PassRasterizerState {
    WMTCullMode cull_mode;
    WMTDepthClipMode depth_clip_mode;
    WMTTriangleFillMode fill_mode;
    FLOAT depth_bias;
    FLOAT depth_bias_clamp;
    FLOAT scole_scale;
    WMTWinding winding;
  } pass_rasterizer_{};
  obj_handle_t compute_root_buf_ = 0;
  // the last render pass a draw of a pipeline that uses UAVs or stream output was in: a UAV barrier orders nothing
  // inside another
  EncoderData *uav_draw_pass_ = nullptr;
  // the textures the render pass targets_pass_ draws to: a transition of another resource orders nothing its draws
  // wrote, unless they wrote through UAVs or stream output (PassWrote)
  const EncoderData *targets_pass_ = nullptr;
  std::array<const Texture *, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT + 1> targets_{};
  // the allocations DiscardResource left undefined whole, which the next render pass loads nothing of: while the
  // allocator's encoders are as they were then (discard_last_, discard_current_), as a clear, copy or other pass between
  // would give them contents again (DiscardTexture)
  std::vector<const TextureAllocation *> discarded_;
  const EncoderData *discard_last_ = nullptr, *discard_current_ = nullptr;
  // what orders the passes (EncoderData::overlap): events that order work without a pass of their own (barriers,
  // queries, predication, stream output) so far; the last pass a draw or dispatch began, and how many such events
  // there had been then; and what render passes drew to since the last ordering point
  uint64_t order_serial_ = 0, plain_serial_ = 0;
  const EncoderData *plain_pass_ = nullptr;
  std::vector<const Texture *> overlap_targets_;
  // an ordering point since the last pass a draw or dispatch began that the vertex stages of draws after it may
  // depend on (EncoderData::late)
  bool vertex_order_ = false;

  // work after this point waits for work before it: the open pass too, which barriers that do not end it (Order),
  // queries and stream output leave open, and whose later draws or dispatches may read what passes before it wrote.
  // `vertices`: work after it may read in a vertex stage what work before it wrote
  void
  OrderingPoint(bool vertices = true) {
    order_serial_++;
    vertex_order_ |= vertices;
    if (allocator_->encoder_current)
      allocator_->encoder_current->overlap = PassOverlap::Ordered;
  }

  // the overlap of a pass a draw or dispatch just began, after the passes before it are in the list
  void
  Overlap(EncoderData *pass, std::span<const Texture *const> targets) {
    // a list's first pass with nothing before it follows the last pass of the list before, whose targets it may share
    // ("the second command list (D) may begin executing work before all work from the first (C) has finished",
    // ID3D12CommandQueue::ExecuteCommandLists): Close orders it where that pass does not end its list
    bool first = !plain_pass_ && allocator_->encoder_last == entry && !order_serial_;
    bool follows = first || (plain_pass_ && allocator_->encoder_last == plain_pass_ && order_serial_ == plain_serial_);
    if (!follows)
      overlap_targets_.clear();
    bool shared = first || std::ranges::any_of(targets, [&](auto texture) {
      return texture && std::ranges::find(overlap_targets_, texture) != overlap_targets_.end();
    });
    pass->overlap = !follows ? PassOverlap::Ordered : shared ? PassOverlap::Shared : PassOverlap::Independent;
    // only barriers whose work after them is pixel shading, render targets or depth since the last such pass
    pass->late = !first && !follows && plain_pass_ && allocator_->encoder_last == plain_pass_ && !vertex_order_ &&
                 pass->type == EncoderType::Render;
    vertex_order_ = false;
    for (auto texture : targets)
      if (texture)
        overlap_targets_.push_back(texture);
    plain_pass_ = pass;
    plain_serial_ = order_serial_;
  }
  // no aliasing barrier since the last clear pass began (JoinableClear)
  bool clear_joinable_ = false;
  // the depth bounds test of a GPU without one: the samples in the range a kernel found before the pass it is for, until
  // the range changes or a draw writes depth (DepthBoundsSamples)
  WMT::Buffer depth_bounds_samples_{};
  EncoderData *depth_bounds_pass_ = nullptr;
  bool depth_bounds_stale_ = true;
  // variable rate shading: RSSetShadingRate's rate and combiners, and RSSetShadingRateImage's image, whose bytes a copy
  // before the pass gives the pixel shader (SM50_BINDING_INDEX_SHADING_RATE_IMAGE)
  D3D12_SHADING_RATE shading_rate_ = D3D12_SHADING_RATE_1X1;
  D3D12_SHADING_RATE_COMBINER shading_rate_combiners_[2] = {
      D3D12_SHADING_RATE_COMBINER_PASSTHROUGH, D3D12_SHADING_RATE_COMBINER_PASSTHROUGH
  };
  MTLD3D12Resource *shading_rate_image_ = nullptr;
  WMT::Buffer shading_rate_bytes_{};
  uint64_t shading_rate_offset_ = 0;
  EncoderData *shading_rate_pass_ = nullptr;

  // the full clears a later render pass may still take as its load actions (FoldClears): a cleared view is in the
  // render target or depth write state, where copies, dispatches and other passes cannot touch it until a barrier names
  // its resource. so a clear stays foldable across them until then, until a pass draws to it, or until something that
  // needs no barrier (a clear of rectangles, a resolve) uses it. the textures are the clear pass's colors', then its
  // depth stencil view's. a render pass takes the attachments it draws to (FoldClears), and the others stay pending
  // past it, as a clear of one view does until a pass draws to it
  struct PendingClear {
    RenderEncoderData *pass;
    std::array<const Texture *, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT + 1> textures{};
  };
  std::vector<PendingClear> clears_;

  // MULLION_D3D12_HOIST_COPIES (on unless 0): what the last barrier that named a resource left it in, and when, for
  // copies that need not end the render pass (PreCopy). a split barrier's beginning leaves no state
  struct Named {
    UINT subresource;
    D3D12_RESOURCE_STATES state;
    uint64_t serial;
  };
  std::unordered_map<ID3D12Resource *, Named> named_;
  // ResourceBarrier calls so far, and how many there had been when the open render pass began
  uint64_t barrier_serial_ = 0, render_serial_ = 0;
  // the blit pass before the render pass hoisted_for_ that copies hoisted out of it run in
  BlitEncoderData *hoisted_ = nullptr;
  const EncoderData *hoisted_for_ = nullptr;

  static bool
  HoistsCopies() {
    static const bool hoists = env::getEnvVar("MULLION_D3D12_HOIST_COPIES") != "0";
    return hoists;
  }

  void
  BarrierLeaves(ID3D12Resource *resource, UINT subresource, D3D12_RESOURCE_STATES state) {
    if (!HoistsCopies())
      return;
    if (!resource)
      named_.clear();
    else
      named_[resource] = {subresource, state, barrier_serial_};
  }

  // a texture used where no barrier says so: no clear of it before may fold into a later pass. a clear that stays
  // where it is keeps the older clears of its other textures there too, the latest first: one of them folding later
  // would come after it (a clear pass whose other attachments a render pass took no longer names those)
  void
  UsedTexture(const Texture *texture) {
    std::vector<const Texture *> fixed{texture};
    for (auto pending = clears_.size(); pending--;) {
      auto &textures = clears_[pending].textures;
      if (std::ranges::none_of(textures, [&](auto t) { return t && std::ranges::find(fixed, t) != fixed.end(); }))
        continue;
      for (auto t : textures)
        if (t)
          fixed.push_back(t);
      clears_.erase(clears_.begin() + pending);
    }
  }

  // occlusion queries: the running ones, each from the first visibility slot it counts in; the slots' GPU addresses,
  // in order; and the sums the ended ones need (sum_occlusion's), made before their results are read
  struct ActiveQuery {
    MTLD3D12QueryHeap *heap;
    UINT index;
    uint32_t first;
  };
  struct OcclusionSum {
    uint32_t query, first, count, binary;
  };
  std::vector<ActiveQuery> active_queries_;
  std::vector<uint64_t> visibility_slots_;
  // whether a draw counted in the last slot
  bool visibility_drawn_ = false;
  std::vector<std::pair<MTLD3D12QueryHeap *, OcclusionSum>> occlusion_sums_;

  // running and ended queries retain their GPU draw counts and indirect dispatches until summed into the heap
  struct StatisticsSum {
    uint64_t counts[14];
    uint32_t query, size, first_dispatch, dispatch_count, first_predicated, predicated_count;
    uint32_t first_draw, draw_count;
  };
  struct PredicatedStatistics {
    uint64_t vertices, primitives, threads, skips;
  };
  std::vector<std::pair<MTLD3D12QueryHeap *, StatisticsSum>> active_statistics_, statistics_sums_;
  // each indirect dispatch's threads, a word per command (IndirectInvocations)
  struct IndirectInvocations {
    uint64_t counts, length;
  };
  std::vector<IndirectInvocations> indirect_dispatches_;
  std::vector<PredicatedStatistics> predicated_statistics_;
  std::vector<uint64_t> draw_statistics_;

  // the primitives `vertices` vertices of the current topology make
  uint64_t
  Primitives(uint64_t vertices) {
    auto at_least = [&](uint64_t first) { return vertices >= first ? vertices - first + 1 : 0; };
    switch (topology_) {
    case D3D_PRIMITIVE_TOPOLOGY_POINTLIST:
      return vertices;
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST:
      return vertices / 2;
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:
      return at_least(2);
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
      return vertices / 3;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
      return at_least(3);
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ:
      return vertices / 4;
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ:
      return at_least(4);
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ:
      return vertices / 6;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ:
      return vertices >= 6 ? (vertices - 4) / 2 : 0;
    default:
      if (topology_ < D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST)
        return 0;
      return vertices / (topology_ - D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST + 1);
    }
  }

  // the GPU captures pixel executions and strip cuts before later work can change the draw's inputs
  void CountDraw(uint64_t vertices, uint64_t instances, bool indexed = false, uint32_t start = 0) {
    if (active_statistics_.empty())
      return;
    PipelineStatisticsDraw draw{};
    draw.vertices = vertices;
    draw.instances = instances;
    draw.primitives = Primitives(vertices);
    if (indexed && pso_graphics_->strip_cut != D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED) {
      draw.indices = index_view_.BufferLocation;
      draw.index_bytes = index_view_.SizeInBytes;
      draw.index_stride = index_view_.Format == DXGI_FORMAT_R32_UINT ? sizeof(uint32_t) : sizeof(uint16_t);
      draw.start = start;
      draw.cut = pso_graphics_->strip_cut == D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF
                     ? D3D12_16BIT_INDEX_STRIP_CUT_VALUE
                     : D3D12_32BIT_INDEX_STRIP_CUT_VALUE;
      auto [first, step] = get_primitive_vertex_count(topology_);
      if (step != first)
        draw.first = first, draw.step = step;
    }
    if (draw.cut && draw.first)
      draw.threads = draw.step == 1 ? device_->GetSIMDWidth() : 1;
    if (predication_region_)
      draw.skips = allocator_->gpu_heap_buffer_address_ +
                   (reinterpret_cast<char *>(&predication_region_->skips) - static_cast<char *>(allocator_->gpu_heap_));
    auto [result, result_offset] =
        allocator_->AllocateGPUHeap(sizeof(PipelineStatisticsResult), alignof(PipelineStatisticsResult));
    if (allocator_->records_)
      allocator_->RecordGPUHeap(result, sizeof(PipelineStatisticsResult), "draw statistics", 0,
                                sizeof(PipelineStatisticsResult));
    draw.result = allocator_->gpu_heap_buffer_address_ + result_offset;
    draw_statistics_.push_back(draw.result);
    auto [mapped, offset] = allocator_->AllocateGPUHeap(sizeof(draw), alignof(PipelineStatisticsDraw));
    if (allocator_->records_)
      allocator_->RecordGPUHeap(mapped, sizeof(draw), "statistics draw");
    memcpy(mapped, &draw, sizeof(draw));
    EncodePipelineStatisticsDraw([&]<typename T>() -> T & { return allocator_->EncodeRenderCommand<T>(); },
                                 device_->GetLib().PipelineStatistics(), Pipeline(), allocator_->gpu_heap_buffer_,
                                 offset, allocator_->gpu_heap_buffer_, result_offset,
                                 draw.threads ? device_->GetLib().PipelineStatistics(true) : WMT::RenderPipelineState{},
                                 draw.threads);
    // it binds a fragment buffer of its own
    ForgetBuffers();
    uint64_t primitives = Primitives(vertices) * instances;
    vertices *= instances;
    if (predication_region_) {
      auto skips = allocator_->gpu_heap_buffer_address_ +
                   (reinterpret_cast<char *>(&predication_region_->skips) - static_cast<char *>(allocator_->gpu_heap_));
      predicated_statistics_.push_back({vertices, primitives, 0, skips});
      return;
    }
    for (auto &[_, sum] : active_statistics_) {
      sum.counts[0] += vertices;
      sum.counts[1] += primitives;
      sum.counts[2] += vertices;
      sum.counts[5] += primitives;
      sum.counts[6] += primitives;
    }
  }

  // a render pass counts samples in a new slot whenever the running queries change: Metal accumulates a slot within
  // a pass, and starts it over in the next
  void
  CountVisibility() {
    auto render = allocator_->encoder_current && allocator_->encoder_current->type == EncoderType::Render
                      ? static_cast<RenderEncoderData *>(allocator_->encoder_current)
                      : nullptr;
    if (!render)
      return;
    visibility_drawn_ = false;
    auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_setvisibilitymode>();
    cmd.type = WMTRenderCommandSetVisibilityMode;
    cmd.mode = WMTVisibilityResultModeDisabled;
    cmd.offset = 0;
    if (active_queries_.empty())
      return;
    auto [window, address, offset] = allocator_->VisibilitySlot();
    // a pass counts in one window: when that is full the pass ends, and the next pass counts in the next window
    if (render->visibility_buffer && render->visibility_buffer != window.handle)
      return allocator_->InvalidateCurrentPass();
    visibility_slots_.push_back(address);
    render->visibility_buffer = window;
    cmd.mode = WMTVisibilityResultModeCounting;
    cmd.offset = offset;
  }

  // predication: the region's GPU heap dwords that say how much of its draws and dispatches runs, with their recorded
  // values and the ones that run nothing, one of which its kernel (predicate) writes before they run; and the region's
  // header, patched when the region ends
  struct PredicatedWord {
    uint32_t word, value, skipped;
  };
  std::vector<PredicatedWord> predicated_;
  // a region's header, which its kernel reads (mullion_command.metal, PredicationRegion)
  struct PredicationRegion {
    uint32_t count, views, skip_if_zero, skips;
    uint64_t entries;
    MTLD3D12CommandAllocatorImpl::ArgumentsView view[MTLD3D12CommandAllocatorImpl::kArgumentsViews];
  };
  // the kernel's header has room for seven
  static_assert(std::size(PredicationRegion{}.view) <= 7);
  PredicationRegion *predication_region_ = nullptr;
  EncoderData *predication_pass_ = nullptr;

  void
  EndPredication() {
    if (!predication_region_)
      return;
    // what says whether the region was skipped is a word of the region's as the others are
    predicated_.push_back(
        {uint32_t((reinterpret_cast<char *>(&predication_region_->skips) - static_cast<char *>(allocator_->gpu_heap_)) / 4), 0, 1}
    );
    auto [mapped, offset] = allocator_->AllocateGPUHeap(predicated_.size() * sizeof(PredicatedWord), 4);
    if (allocator_->records_)
      allocator_->RecordGPUHeap(
          mapped, predicated_.size() * sizeof(PredicatedWord), "predicate words", 0, 0, predication_pass_
      );
    if (mapped)
      memcpy(mapped, predicated_.data(), predicated_.size() * sizeof(PredicatedWord));
    predication_region_->count = predicated_.size();
    predication_region_->entries = allocator_->gpu_heap_buffer_address_ + offset;
    // the list may have gone on in another buffer since the region began (MTLD3D12CommandAllocatorImpl::Allocate),
    // and what reads a word reads it through the buffer the list was in then: the kernel writes it through each
    predication_region_->views = allocator_->ArgumentsViews(predication_region_->view);
    predication_region_ = nullptr;
    predicated_.clear();
  }

  // a copy, clear or resolve in a predication region: Metal has no way to skip one, so the queue leaves its passes out
  // once the GPU has run the region's kernel
  struct PredicatedPasses {
    MTLD3D12CommandAllocatorImpl *allocator;
    PredicateEncoderData *predicate = nullptr;

    PredicatedPasses(MTLD3D12GraphicsCommandListImpl *list) : allocator(list->allocator_.ptr()) {
      if (!list->predication_region_)
        return;
      allocator->InvalidateCurrentPass();
      predicate = allocator->AllocatePass<PredicateEncoderData>();
      predicate->type = EncoderType::Predicate;
      predicate->skips = &list->predication_region_->skips;
      allocator->InvalidateCurrentPass();
    }

    ~PredicatedPasses() {
      if (!predicate)
        return;
      allocator->InvalidateCurrentPass();
      predicate->end = allocator->AllocatePass<EncoderData>();
      allocator->InvalidateCurrentPass();
    }
  };

  // a draw's or dispatch's arguments, in the GPU heap when predicated, where dword `count` is what a skip zeroes
  template <typename Arguments>
  std::optional<uint64_t>
  Predicated(const Arguments &args, unsigned count) {
    if (!predication_region_)
      return {};
    auto [mapped, offset] = allocator_->AllocateGPUHeap(sizeof(args), 16);
    if (allocator_->records_)
      allocator_->RecordGPUHeap(mapped, sizeof(args), "predicated arguments", count * sizeof(uint32_t), sizeof(uint32_t));
    memcpy(mapped, &args, sizeof(args));
    predicated_.push_back({uint32_t(offset / 4 + count), reinterpret_cast<const uint32_t *>(&args)[count], 0});
    return offset;
  }

  // an ExecuteIndirect's command count, when predicated: its resolver reads it through max_count_buffer, which a skip
  // points at a zero
  template <typename Data>
  void
  PredicateIndirect(Data *data) {
    if (!predication_region_)
      return;
    if (allocator_->records_)
      allocator_->ExcludeGPUWrite(data, offsetof(Data, max_count_buffer), sizeof(data->max_count_buffer));
    auto [zero, zero_offset] = allocator_->AllocateGPUHeap(sizeof(uint32_t), sizeof(uint32_t));
    if (allocator_->records_)
      allocator_->RecordGPUHeap(zero, sizeof(uint32_t), "predicate zero");
    *static_cast<uint32_t *>(zero) = 0;
    uint64_t skipped = allocator_->gpu_heap_buffer_address_ + zero_offset;
    auto word = uint32_t((reinterpret_cast<char *>(&data->max_count_buffer) - (char *)allocator_->gpu_heap_) / 4);
    for (unsigned half = 0; half < 2; half++)
      predicated_.push_back({word + half, uint32_t(data->max_count_buffer >> 32 * half), uint32_t(skipped >> 32 * half)});
  }

  // the ended occlusion queries' sums, once the passes they counted in are over
  void
  SumOcclusion() {
    if (occlusion_sums_.empty())
      return;
    StartComputePass();
    auto upload = [&](const void *data, size_t length, const char *kind) {
      auto [mapped, offset] = allocator_->AllocateGPUHeap(length, 16);
      if (allocator_->records_)
        allocator_->RecordGPUHeap(mapped, length, kind);
      memcpy(mapped, data, length);
      return offset;
    };
    auto slots = upload(visibility_slots_.data(), visibility_slots_.size() * sizeof(uint64_t), "occlusion slots");
    for (auto &window : allocator_->visibility_) {
      auto &cmd = allocator_->EncodeComputeCommand<wmtcmd_compute_useresource>();
      cmd.type = WMTComputeCommandUseResource;
      cmd.usage = WMTResourceUsageRead;
      cmd.resource = window;
    }
    std::stable_sort(occlusion_sums_.begin(), occlusion_sums_.end(), [](auto &x, auto &y) { return x.first < y.first; });
    for (auto first = occlusion_sums_.begin(); first != occlusion_sums_.end();) {
      auto last = std::find_if(first, occlusion_sums_.end(), [&](auto &x) { return x.first != first->first; });
      std::vector<OcclusionSum> sums;
      for (auto it = first; it != last; ++it)
        sums.push_back(it->second);
      auto &cmd_setpso = allocator_->EncodeComputeCommand<wmtcmd_compute_setpso>();
      cmd_setpso.type = WMTComputeCommandSetPSO;
      cmd_setpso.pso = device_->occlusion_sum;
      cmd_setpso.threadgroup_size = {1, 1, 1};
      const std::pair<obj_handle_t, uint64_t> buffers[] = {
          {allocator_->gpu_heap_buffer_, upload(sums.data(), sums.size() * sizeof(OcclusionSum), "occlusion sums")},
          {allocator_->gpu_heap_buffer_, slots},
          {first->first->results, 0},
      };
      for (uint8_t i = 0; i < std::size(buffers); i++) {
        auto &cmd = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
        cmd.type = WMTComputeCommandSetBuffer;
        cmd.buffer = buffers[i].first;
        cmd.offset = buffers[i].second;
        cmd.index = i;
      }
      auto &cmd_dispatch = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch>();
      cmd_dispatch.type = WMTComputeCommandDispatch;
      cmd_dispatch.size = {(uint32_t)sums.size(), 1, 1};
      first = last;
    }
    occlusion_sums_.clear();
    allocator_->InvalidateCurrentPass();
  }

  // the ended pipeline statistics queries' results, once the passes they counted in are over
  void
  SumStatistics() {
    if (statistics_sums_.empty())
      return;
    StartComputePass();
    // an empty list still gets a buffer to bind
    auto upload = [&](const void *data, size_t length, const char *kind) {
      auto [mapped, offset] = allocator_->AllocateGPUHeap(std::max<size_t>(length, 16), 16);
      if (allocator_->records_)
        allocator_->RecordGPUHeap(mapped, std::max<size_t>(length, 16), kind);
      if (length)
        memcpy(mapped, data, length);
      return offset;
    };
    auto dispatches = upload(
        indirect_dispatches_.data(), indirect_dispatches_.size() * sizeof(IndirectInvocations), "statistics dispatches"
    );
    auto predicated = upload(
        predicated_statistics_.data(), predicated_statistics_.size() * sizeof(PredicatedStatistics),
        "predicated statistics"
    );
    auto draws =
        upload(draw_statistics_.data(), draw_statistics_.size() * sizeof(uint64_t), "draw statistics addresses");
    std::stable_sort(statistics_sums_.begin(), statistics_sums_.end(), [](auto &x, auto &y) { return x.first < y.first; });
    for (auto first = statistics_sums_.begin(); first != statistics_sums_.end();) {
      auto last = std::find_if(first, statistics_sums_.end(), [&](auto &x) { return x.first != first->first; });
      std::vector<StatisticsSum> sums;
      for (auto it = first; it != last; ++it)
        sums.push_back(it->second);
      auto &cmd_setpso = allocator_->EncodeComputeCommand<wmtcmd_compute_setpso>();
      cmd_setpso.type = WMTComputeCommandSetPSO;
      cmd_setpso.pso = device_->pipeline_statistics_sum;
      cmd_setpso.threadgroup_size = {1, 1, 1};
      const std::pair<obj_handle_t, uint64_t> buffers[] = {
          {allocator_->gpu_heap_buffer_, upload(sums.data(), sums.size() * sizeof(StatisticsSum), "statistics sums")},
          {allocator_->gpu_heap_buffer_, dispatches},
          {first->first->results, 0},
          {allocator_->gpu_heap_buffer_, predicated},
          {allocator_->gpu_heap_buffer_, draws},
      };
      for (uint8_t i = 0; i < std::size(buffers); i++) {
        auto &cmd = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
        cmd.type = WMTComputeCommandSetBuffer;
        cmd.buffer = buffers[i].first;
        cmd.offset = buffers[i].second;
        cmd.index = i;
      }
      auto &cmd_dispatch = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch>();
      cmd_dispatch.type = WMTComputeCommandDispatch;
      cmd_dispatch.size = {(uint32_t)sums.size(), 1, 1};
      first = last;
    }
    statistics_sums_.clear();
    allocator_->InvalidateCurrentPass();
  }

public:
  MTLD3D12GraphicsCommandListImpl(MTLD3D12Device *pDevice, D3D12_COMMAND_LIST_TYPE Type) :
      MTLD3D12DeviceChild<MTLD3D12GraphicsCommandList>(pDevice),
      type_(Type) {}

  // a list created closed, which records once Reset gives it an allocator (CreateCommandList1)
  void
  InitializeClosed() {
    ResetState(nullptr);
    entry = nullptr;
    encoder_count = 0;
  }

  ~MTLD3D12GraphicsCommandListImpl() {}

  void
  ResetState(ID3D12PipelineState *pInitialPipelineState) {
    pso_graphics_ = nullptr;
    pso_compute_ = nullptr;
    if (auto pso = static_cast<MTLD3D12PipelineState *>(pInitialPipelineState)) {
      if (!pso->IsComputePipelineState)
        pso_graphics_ = static_cast<MTLD3D12GraphicsPipelineState *>(pInitialPipelineState);
      else
        pso_compute_ = static_cast<MTLD3D12ComputePipelineState *>(pInitialPipelineState);
    }

    num_rtvs = {};
    std::fill(std::begin(rtvs), std::end(rtvs), MTL_RENDER_TARGET_DESC{});
    dsv = {};

    topology_ = {};

    num_viewports = {};
    memset(viewports, 0, sizeof(viewports));

    num_scissors = {};
    memset(scissors, 0, sizeof(scissors));

    blend_factor_[0] = 1.0f;
    blend_factor_[1] = 1.0f;
    blend_factor_[2] = 1.0f;
    blend_factor_[3] = 1.0f;
    stencil_ref_ = back_stencil_ref_ = 0;
    programmed_samples_ = 0;
    depth_bias_.reset();
    depth_bounds_[0] = 0.0f;
    depth_bounds_[1] = 1.0f;
    shading_rate_ = D3D12_SHADING_RATE_1X1;
    shading_rate_combiners_[0] = shading_rate_combiners_[1] = D3D12_SHADING_RATE_COMBINER_PASSTHROUGH;
    shading_rate_image_ = nullptr;
    shading_rate_pass_ = nullptr;
    // the samples in the bounds were found for another recording's depth and range, in a pass the allocator may give
    // again
    depth_bounds_pass_ = nullptr;
    depth_bounds_stale_ = true;
    pass_dsso_ = 0;
    pass_rasterizer_set_ = false;
    compute_root_buf_ = 0;

    rootsig_graphics_ = nullptr;
    memset(rootarg_graphics_staging_, 0, sizeof(rootarg_graphics_staging_));
    rootarg_graphics_uploaded_ = {};

    rootsig_compute_ = nullptr;
    memset(rootarg_compute_staging_, 0, sizeof(rootarg_compute_staging_));
    rootarg_compute_uploaded_ = {};

    memset(vertex_buffers_.data(), 0, sizeof(vertex_buffers_));
    std::fill(std::begin(so_views_), std::end(so_views_), D3D12_STREAM_OUTPUT_BUFFER_VIEW{});

    index_buffer_address = 0;
    index_buffer = {};
    index_type = {};
    index_offset = 0;
    index_view_ = {};
    va_found_ = nullptr;
    ForgetBuffers();

    dirty_state_.clrAll();
  }

  HRESULT
  Initialize(ID3D12CommandAllocator *pAllocator, ID3D12PipelineState *pInitialPipelineState) {
    auto allocator = static_cast<MTLD3D12CommandAllocatorImpl *>(pAllocator);

    if (allocator_ != allocator)
      allocator_ = allocator;

    ResetState(pInitialPipelineState);
    bundle_.clear();
    clears_.clear();
    discarded_.clear();
    markers_.clear();
    plain_pass_ = nullptr;
    order_serial_ = 0;
    overlap_targets_.clear();
    named_.clear();
    hoisted_ = nullptr;
    hoisted_for_ = nullptr;
    structure_run_ = {};
    // a bundle's initial state is its first call
    if (pInitialPipelineState && type_ == D3D12_COMMAND_LIST_TYPE_BUNDLE)
      SetPipelineState(pInitialPipelineState);
    active_queries_.clear();
    visibility_slots_.clear();
    occlusion_sums_.clear();
    active_statistics_.clear();
    statistics_sums_.clear();
    indirect_dispatches_.clear();
    draw_statistics_.clear();
    predicated_statistics_.clear();
    predicated_.clear();
    predication_region_ = nullptr;
    left_for_later_ = false;
    events_.clear();

    encoder_count = std::numeric_limits<size_t>::max();
    return allocator_->StartRecord(&entry);
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return FAIL(E_POINTER);

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12CommandList) || riid == __uuidof(ID3D12GraphicsCommandList) ||
        riid == __uuidof(ID3D12GraphicsCommandList1) || riid == __uuidof(ID3D12GraphicsCommandList2) ||
        riid == __uuidof(ID3D12GraphicsCommandList3) || riid == __uuidof(ID3D12GraphicsCommandList4) ||
        riid == __uuidof(ID3D12GraphicsCommandList5) || riid == __uuidof(ID3D12GraphicsCommandList6) ||
        riid == __uuidof(ID3D12GraphicsCommandList7) || riid == __uuidof(ID3D12GraphicsCommandList8) ||
        riid == __uuidof(ID3D12GraphicsCommandList9)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (riid == __uuidof(IMTLD3D12CommandListExt)) {
      AddRef();
      *ppvObject = static_cast<IMTLD3D12CommandListExt *>(&upscale_ext_);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12GraphicsCommandList), riid)) {
      WARN("D3D12GraphicsCommandList: Unknown interface query ", str::format(riid));
    }

    return MTLD3D12DeviceChild::QueryInterface(riid, ppvObject);
  }

  D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE
  GetType() {
    return type_;
  }

  HRESULT STDMETHODCALLTYPE
  Close() {
    if (encoder_count < std::numeric_limits<size_t>::max())
      return FAIL(E_FAIL);
    WriteMarkers();
    SumOcclusion();
    SumStatistics();
    EndPredication();
    // the next list of the call follows this one's last pass as a pass of this list would (Overlap), or a pass that
    // encodes nothing orders it after all before
    auto last = allocator_->encoder_current ? allocator_->encoder_current : allocator_->encoder_last;
    if (!plain_pass_ || last != plain_pass_ || order_serial_ != plain_serial_) {
      allocator_->InvalidateCurrentPass();
      allocator_->AllocatePass<EncoderData>()->type = EncoderType::Null;
    }
    HRESULT hr = allocator_->EndRecord(&encoder_count);
    recorded_on = allocator_.ptr();
    if (SUCCEEDED(recording_error_))
      recording_error_ = hr;
    // a list whose recording failed is closed with nothing in it ("If an error was encountered during recording,
    // the error code is returned here", ID3D12GraphicsCommandList::Close)
    if (FAILED(recording_error_))
      allocator_->Discard(entry);
    return recording_error_;
  };

  HRESULT STDMETHODCALLTYPE
  Reset(ID3D12CommandAllocator *pAllocator, ID3D12PipelineState *pInitialState) {
    if (encoder_count == std::numeric_limits<size_t>::max())
      return FAIL(E_FAIL);
    return FAILED(recording_error_) ? recording_error_ : Initialize(pAllocator, pInitialState);
  };

  void STDMETHODCALLTYPE
  ClearState(ID3D12PipelineState *pPipelineState) {
    // predication is state too
    EndPredication();
    allocator_->InvalidateCurrentPass();
    ResetState(pPipelineState);
  };

  std::tuple<uint64_t, uint64_t>
  PopulateVertexBufferTable() {
    auto slot_mask = pso_graphics_ ? pso_graphics_->slot_mask : 0;
    if (!slot_mask)
      return {0, 0};
    uint32_t max_slot = 32 - __builtin_clz(slot_mask);
    struct VERTEX_BUFFER_ENTRY {
      uint64_t buffer_handle;
      uint32_t stride;
      uint32_t length;
    };
    auto stride = align(sizeof(VERTEX_BUFFER_ENTRY) * max_slot, 16);

    auto [mapped, offset] = allocator_->AllocateGPUHeap(stride, 16);
    if (allocator_->records_)
      allocator_->RecordGPUHeap(mapped, stride, "vertex buffer table");

    VERTEX_BUFFER_ENTRY *entries = (VERTEX_BUFFER_ENTRY *)mapped;
    for (unsigned slot = 0, index = 0; slot < max_slot; slot++) {
      if (!(slot_mask & (1 << slot)))
        continue;
      auto &state = vertex_buffers_[slot];
      entries[index].buffer_handle = state.BufferLocation;
      entries[index].stride = state.StrideInBytes;
      entries[index++].length = state.SizeInBytes;
    };

    return {offset, stride};
  }

  // the samples of the depth stencil view in the depth bounds, a bit each, for the pixel shader's test, in a pass of
  // their own before the draw's (SM50_SHADER_PSO_PIXEL_SHADER_DATA's depth_bounds)
  void
  DepthBoundsSamples() {
    auto depth = dsv.Texture->view(dsv.View).texture;
    auto type = dsv.Texture->textureType(dsv.View);
    uint32_t kind = (type == WMTTextureType2DArray || type == WMTTextureType2DMultisampleArray) +
                    2 * (type == WMTTextureType2DMultisample || type == WMTTextureType2DMultisampleArray);
    // the kernel reads the depth before the pass, so a clear of it stays before the kernel
    UsedTexture(dsv.Texture);
    StartComputePass();
    depth_bounds_samples_ = allocator_->DepthBoundsSamples(uint64_t(dsv.Width) * dsv.Height);
    auto &pso = allocator_->EncodeComputeCommand<wmtcmd_compute_setpso>();
    pso.type = WMTComputeCommandSetPSO;
    pso.pso = device_->depth_bounds_samples[kind];
    pso.threadgroup_size = {8, 8, 1};
    auto &texture = allocator_->EncodeComputeCommand<wmtcmd_compute_settexture>();
    texture.type = WMTComputeCommandSetTexture;
    texture.texture = depth.handle;
    texture.index = 0;
    auto &samples = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
    samples.type = WMTComputeCommandSetBuffer;
    samples.buffer = depth_bounds_samples_;
    samples.offset = 0;
    samples.index = 1;
    // DepthBounds, mullion_command.metal (float2 has 8-byte alignment in MSL, so sizeof(DepthBounds) == 16)
    struct alignas(8) {
      float range[2];
      uint32_t pitch;
      uint32_t pad;
    } bounds{{std::clamp(depth_bounds_[0], 0.0f, 1.0f), std::clamp(depth_bounds_[1], 0.0f, 1.0f)}, dsv.Width, 0};
    static_assert(sizeof(bounds) == 16);
    auto &bytes = allocator_->EncodeComputeCommand<wmtcmd_compute_setbytes>();
    bytes.type = WMTComputeCommandSetBytes;
    bytes.bytes.set(memcpy(allocator_->AllocateCPUHeap(sizeof(bounds), 16), &bounds, sizeof(bounds)));
    bytes.length = sizeof(bounds);
    bytes.index = 0;
    auto &dispatch = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch>();
    dispatch.type = WMTComputeCommandDispatchThreads;
    dispatch.size = {dsv.Width, dsv.Height, 1};
  }

  // whether draws shade coarse pixels: some combination of the rates may be coarser than 1x1
  bool
  CoarseShading() {
    return pso_graphics_ && pso_graphics_->pixel_shader && pso_graphics_->coarse_shading &&
           (shading_rate_ != D3D12_SHADING_RATE_1X1 ||
            shading_rate_combiners_[0] != D3D12_SHADING_RATE_COMBINER_PASSTHROUGH ||
            shading_rate_combiners_[1] != D3D12_SHADING_RATE_COMBINER_PASSTHROUGH);
  }

  // the shading rate image's bytes, a row of its width after another, for the passes after this one's start
  void
  ShadingRateImageBytes() {
    D3D12_RESOURCE_DESC desc;
    shading_rate_image_->GetDesc(&desc);
    uint32_t width = desc.Width, height = desc.Height;
    PreBlit();
    std::tie(shading_rate_bytes_, shading_rate_offset_) = allocator_->AllocateTempBuffer(uint64_t(width) * height, 16);
    auto &copy = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer>();
    copy.type = WMTBlitCommandCopyFromTextureToBuffer;
    copy.src = shading_rate_image_->texture->current()->texture();
    copy.slice = 0;
    copy.level = 0;
    copy.origin = {0, 0, 0};
    copy.size = {width, height, 1};
    copy.dst = shading_rate_bytes_;
    copy.offset = shading_rate_offset_;
    copy.bytes_per_row = width;
    copy.bytes_per_image = width * height;
  }

  // the full clears of a new render pass's attachments become its load actions, the latest first, and their passes
  // go: D3D11.3 5.2.3.1, "The full extent of the resource view is always cleared." passes between a clear and this one
  // are copies, dispatches, other clears and draws to other targets, which cannot touch what it cleared (PendingClear);
  // predication, settling and the like keep it where it is. a clear that stays keeps the older ones of its textures too
  void
  FoldClears(RenderEncoderData *render) {
    std::array<const Texture *, 2 * (D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT + 1)> kept{};
    size_t kept_count = 0;
    for (auto pending = clears_.size(); pending--;) {
      auto clear = clears_[pending].pass;
      auto &textures = clears_[pending].textures;
      auto is_kept = [&](const Texture *texture) {
        return texture && std::ranges::find(kept.begin(), kept.begin() + kept_count, texture) != kept.begin() + kept_count;
      };
      auto keep_all = [&] {
        for (auto texture : textures)
          if (texture && kept_count < kept.size())
            kept[kept_count++] = texture;
      };
      auto folds = [&] {
        if (clear->render_target_width != render->render_target_width ||
            clear->render_target_height != render->render_target_height ||
            clear->render_target_array_length != render->render_target_array_length ||
            // a timestamp query on the clear has the clock when the clear is done
            clear->stamps)
          return false;
        for (auto between = clear->next; between; between = between->next)
          if (between->type != EncoderType::Clear && between->type != EncoderType::Render &&
              between->type != EncoderType::Blit && between->type != EncoderType::Compute &&
              between->type != EncoderType::ComputeParts)
            return false;
        return true;
      };
      if (!folds()) {
        keep_all();
        if (kept_count == kept.size())
          break;
        continue;
      }
      // each attachment on its own: clears of one size join one clear pass whatever passes then draw to them
      // (JoinableClear), so it may hold attachments of this pass and others of a later one. the depth and stencil
      // planes, one texture, go together, and not into a pass that binds them read-only: such a plane may be sampled
      // in the pass, and a load action clears only the tile's copy
      bool stays = false;
      for (unsigned i = 0; i < std::size(clear->colors); i++) {
        auto &cleared = clear->colors[i];
        if (!cleared.attachment)
          continue;
        auto color = std::ranges::find_if(render->colors, [&](auto &rt) {
          return rt.attachment == cleared.attachment && rt.depth_plane == cleared.depth_plane;
        });
        if (color == std::end(render->colors) || is_kept(textures[i])) {
          stays = true;
          continue;
        }
        if (color->load_action == WMTLoadActionLoad) {
          color->load_action = WMTLoadActionClear;
          color->clear_color = cleared.clear_color;
        }
        cleared = {};
        textures[i] = nullptr;
      }
      if (clear->depth.attachment || clear->stencil.attachment) {
        if (!is_kept(textures.back()) && !render->dsv_readonly_flags &&
            (!clear->depth.attachment || clear->depth.attachment == render->depth.attachment) &&
            (!clear->stencil.attachment || clear->stencil.attachment == render->stencil.attachment)) {
          if (clear->depth.attachment && render->depth.load_action == WMTLoadActionLoad) {
            render->depth.load_action = WMTLoadActionClear;
            render->depth.clear_depth = clear->depth.clear_depth;
          }
          if (clear->stencil.attachment && render->stencil.load_action == WMTLoadActionLoad) {
            render->stencil.load_action = WMTLoadActionClear;
            render->stencil.clear_stencil = clear->stencil.clear_stencil;
          }
          clear->depth = {};
          clear->stencil = {};
          textures.back() = nullptr;
        } else {
          stays = true;
        }
      }
      if (stays) {
        keep_all();
        if (kept_count == kept.size())
          break;
        continue;
      }
      // out of the list, which only clear passes leave: the next one, if a clear, follows the one before now
      if (plain_pass_ == clear)
        plain_pass_ = clear->overlap == PassOverlap::Ordered ? nullptr : clear->previous;
      clear->previous->next = clear->next;
      if (clear->next && clear->next->type == EncoderType::Clear)
        static_cast<RenderEncoderData *>(clear->next)->previous = clear->previous;
      if (allocator_->encoder_last == clear)
        allocator_->encoder_last = clear->previous;
      allocator_->encoder_count_--;
      clear->~RenderEncoderData();
      clears_.erase(clears_.begin() + pending);
    }
    // what the pass draws to is used: a clear before it that stayed is not a later pass's
    for (auto texture : targets_)
      if (texture)
        UsedTexture(texture);
  }

  DrawCallStatus
  PreDraw(bool SkipResourceBinding = false) {
    TakeFuller();
    if (!pso_graphics_ || pso_graphics_->failed)
      return DrawCallStatus::Invalid;
    // a draw that tests the depth bounds where the GPU cannot finds the samples that pass first, unless the pass it is
    // in has them for this range and nothing has written depth since
    bool bounds_tested = pso_graphics_->depth_bounds_emulated && pso_graphics_->dsv_mask && dsv.Texture &&
                         (DepthStencilPlanarFlags(dsv.Texture->pixelFormat(dsv.View)) & 1);
    bool bounds_found = false;
    if (bounds_tested && depth_bounds_[0] > depth_bounds_[1])
      return DrawCallStatus::Invalid;
    if (bounds_tested && (depth_bounds_stale_ || !allocator_->encoder_current ||
                          allocator_->encoder_current != depth_bounds_pass_)) {
      DepthBoundsSamples();
      bounds_found = true;
    }
    // the shading rate image, which changes only outside passes, copied before the pass of the first coarse draw
    bool coarse = CoarseShading(), image_copied = false;
    if (coarse && shading_rate_image_ &&
        (!allocator_->encoder_current || allocator_->encoder_current != shading_rate_pass_)) {
      ShadingRateImageBytes();
      image_copied = true;
    }
    // a draw whose scissors or viewports the pass has no room for starts the next pass
    const UINT count = D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    if (auto pass = static_cast<RenderEncoderData *>(allocator_->encoder_current);
        allocator_->encoder_current && allocator_->encoder_current->type == EncoderType::Render &&
        ((dirty_state_.test(DirtyState::ScissorRect) && pass->scissors + count > WMT::kPassRectangles) ||
         (dirty_state_.test(DirtyState::Viewport) && pass->viewports + count > WMT::kPassRectangles) ||
         (pass->rasterless && !pso_graphics_->rasterless)))
      allocator_->InvalidateCurrentPass();
    // Metal counts a slot's samples in 32 bits (tests/native/visibility_width.swift): a draw an occlusion query counts
    // after another in its slot takes one of its own, and only a draw of 2^32 samples alone wraps
    if (visibility_drawn_ && !active_queries_.empty())
      CountVisibility();
    if (!allocator_->encoder_current || allocator_->encoder_current->type != EncoderType::Render) {
      // what was discarded with nothing since loads nothing (DiscardTexture)
      bool discards = !discarded_.empty() && discard_last_ == allocator_->encoder_last &&
                      discard_current_ == allocator_->encoder_current;
      auto discarded = [&](const TextureViewRef &view) {
        return discards && view && std::ranges::find(discarded_, view->allocation) != discarded_.end();
      };

      allocator_->InvalidateCurrentPass();
      auto render = allocator_->AllocatePass<RenderEncoderData>();
      render->type = EncoderType::Render;
      render->render_target_count = num_rtvs;

      unsigned render_target_width = 16384, render_target_height = 16384, render_target_array_length = 0;

      unsigned effective_rtvs = 0, samples = 0;
      bool paired = false;
      render_serial_ = barrier_serial_;
      targets_pass_ = render;
      targets_.fill(nullptr);
      for (unsigned i = 0; i < num_rtvs; i++) {
        if (!(pso_graphics_->rtv_mask & (1u << i)))
          continue;
        auto &AttachmentDesc = rtvs[i];
        if (!AttachmentDesc.Texture)
          continue;
        effective_rtvs++;
        if (AttachmentDesc.Texture->centered || pso_graphics_->forced_sample_count == 1)
          render->programmed_samples = AttachmentDesc.Texture->sampleCount();
        samples = AttachmentDesc.Texture->sampleCount();
        paired = AttachmentDesc.Texture->paired;
        targets_[i] = AttachmentDesc.Texture;
        auto &rt = render->colors[i];
        rt.attachment = AttachmentDesc.Texture->view(AttachmentDesc.View);
        rt.depth_plane = AttachmentDesc.DepthPlane;
        rt.load_action = discarded(rt.attachment) ? WMTLoadActionDontCare : WMTLoadActionLoad;
        rt.store_action = WMTStoreActionStore;
        render_target_width = std::min(render_target_width, AttachmentDesc.Width);
        render_target_height = std::min(render_target_height, AttachmentDesc.Height);
        render_target_array_length = std::max(render_target_array_length, AttachmentDesc.RenderTargetArrayLength);
      }
      if (dsv.Texture && pso_graphics_->dsv_mask) {
        auto &AttachmentDesc = dsv;
        effective_rtvs++;
        if (AttachmentDesc.Texture->centered || pso_graphics_->forced_sample_count == 1)
          render->programmed_samples = AttachmentDesc.Texture->sampleCount();
        samples = AttachmentDesc.Texture->sampleCount();
        paired = AttachmentDesc.Texture->paired;
        targets_.back() = AttachmentDesc.Texture;
        auto dsv_planar_flags =
            DepthStencilPlanarFlags(AttachmentDesc.Texture->pixelFormat(AttachmentDesc.View)) & pso_graphics_->dsv_mask;
        if (dsv_planar_flags & 1) {
          auto &rt = render->depth;
          rt.attachment = AttachmentDesc.Texture->view(AttachmentDesc.View);
          rt.depth_plane = 0; // DSV cannot be 3D
          rt.load_action = discarded(rt.attachment) ? WMTLoadActionDontCare : WMTLoadActionLoad;
          rt.store_action = WMTStoreActionStore;
        }
        if (dsv_planar_flags & 2) {
          auto &rt = render->stencil;
          rt.attachment = AttachmentDesc.Texture->view(AttachmentDesc.View);
          rt.depth_plane = 0; // DSV cannot be 3D
          rt.load_action = discarded(rt.attachment) ? WMTLoadActionDontCare : WMTLoadActionLoad;
          rt.store_action = WMTStoreActionStore;
        }
        render->dsv_planar_flags = dsv_planar_flags;
        // the planes the view does not write: the pipeline's depth stencil state leaves them alone
        render->dsv_readonly_flags =
            AttachmentDesc.Flags & (D3D12_DSV_FLAG_READ_ONLY_DEPTH | D3D12_DSV_FLAG_READ_ONLY_STENCIL);
        render_target_width = std::min(render_target_width, AttachmentDesc.Width);
        render_target_height = std::min(render_target_height, AttachmentDesc.Height);
        render_target_array_length = std::max(render_target_array_length, AttachmentDesc.RenderTargetArrayLength);
      }
      if (!effective_rtvs && pso_graphics_->rasterless) {
        render->rasterless = true;
        render_target_width = render_target_height = 1;
      }
      render->render_target_width = render_target_width;
      render->render_target_height = render_target_height;
      render->render_target_array_length = render_target_array_length;
      // SetSamplePositions' pattern is for targets of its sample count; with others what the draws sample is undefined.
      // paired samples lie where the even ones of their pairs do
      if (programmed_samples_ && programmed_samples_ == samples << paired) {
        render->programmed_samples = samples;
        for (unsigned i = 0; i < samples; i++)
          render->sample_positions[i] = sample_positions_[i << paired];
      }
      if (effective_rtvs == 0) {
        render->default_raster_sample_count = std::max(1u, pso_graphics_->forced_sample_count);
      }

      FoldClears(render);
      Overlap(render, targets_);

      dirty_state_.set(DirtyState::VertexBuffer, DirtyState::GraphicsRootArguments, DirtyState::GraphicsRootSignature);
      dirty_state_.set(DirtyState::Viewport, DirtyState::ScissorRect);
      dirty_state_.set(DirtyState::BlendFactor, DirtyState::StencilRef, DirtyState::DepthBounds);
      depth_bounds_set_ = false;
      pass_dsso_ = 0;
      pass_rasterizer_set_ = false;
      dirty_state_.set(DirtyState::GraphicsPipelineState);
      if (bounds_found) {
        depth_bounds_pass_ = allocator_->encoder_current;
        depth_bounds_stale_ = false;
      }
      if (image_copied)
        shading_rate_pass_ = allocator_->encoder_current;
      if (!active_queries_.empty())
        CountVisibility();
    } else if (vertex_order_) {
      // a draw whose vertices may read what the pass's own work since it began did not
      allocator_->encoder_current->late = false;
    }
    visibility_drawn_ = !active_queries_.empty();

    // the conservative rasterizer's viewports and state, bound again when they or the pipeline change
    bool conservative_dirty = dirty_state_.test(DirtyState::GraphicsPipelineState) || dirty_state_.test(DirtyState::Viewport);
    if (dirty_state_.test(DirtyState::GraphicsPipelineState)) {
      // a pipeline for a topology with adjacency is made here, and may fail
      auto pipeline = Pipeline();
      if (!pipeline || (pso_graphics_->stream_output && !SinglePass() && !Pipeline(true)))
        return DrawCallStatus::Invalid;
      auto &cmd_setpso = allocator_->EncodeRenderCommand<wmtcmd_render_setpso>();
      cmd_setpso.type = WMTRenderCommandSetPSO;
      cmd_setpso.pso = pipeline;
      if (Logger::logLevel() == LogLevel::Trace)
        TRACE("pass ", allocator_->encoder_current->id, ": graphics pipeline ", pso_graphics_->name);
      if (device_->NamesPasses())
        device_->NamePass(allocator_->encoder_current, pso_graphics_->name, pipeline);

      // the planes the pipeline has: a pass may have more (SetPipelineState)
      auto dsso = pso_graphics_->GetDepthStencilState(
          static_cast<RenderEncoderData *>(allocator_->encoder_current)->dsv_planar_flags & pso_graphics_->dsv_mask,
          static_cast<RenderEncoderData *>(allocator_->encoder_current)->dsv_readonly_flags
      );
      if (dsso.handle != pass_dsso_) {
        auto &cmd_setdsso = allocator_->EncodeRenderCommand<wmtcmd_render_setdepthstencilstate>();
        cmd_setdsso.type = WMTRenderCommandSetDepthStencilState;
        cmd_setdsso.depth_stencil_state = dsso;
        pass_dsso_ = dsso.handle;
      }

      // one by one: libstdc++ assigns no std::array to a std::tuple of references
      auto [depth_bias, depth_bias_clamp, scole_scale] =
          pso_graphics_->unbiased ? std::array<FLOAT, 3>{}
                                  : depth_bias_.value_or(std::array{
                                        pso_graphics_->depth_bias, pso_graphics_->depth_bias_clamp, pso_graphics_->scole_scale
                                    });
      // a conservative rasterizer clips depth itself, and its grown triangles' depth goes past the polygon's
      PassRasterizerState rs{
          pso_graphics_->cull_mode,
          pso_graphics_->conservative ? WMTDepthClipModeClamp : pso_graphics_->depth_clip_mode,
          pso_graphics_->fill_mode,
          depth_bias,
          depth_bias_clamp,
          scole_scale,
          pso_graphics_->winding,
      };
      if (!pass_rasterizer_set_ || memcmp(&pass_rasterizer_, &rs, sizeof(rs)) != 0) {
        auto &cmd_setrs = allocator_->EncodeRenderCommand<wmtcmd_render_setrasterizerstate>();
        cmd_setrs.type = WMTRenderCommandSetRasterizerState;
        cmd_setrs.cull_mode = rs.cull_mode;
        cmd_setrs.depth_clip_mode = rs.depth_clip_mode;
        cmd_setrs.fill_mode = rs.fill_mode;
        cmd_setrs.depth_bias = rs.depth_bias;
        cmd_setrs.depth_bias_clamp = rs.depth_bias_clamp;
        cmd_setrs.scole_scale = rs.scole_scale;
        cmd_setrs.winding = rs.winding;
        pass_rasterizer_ = rs;
        pass_rasterizer_set_ = true;
      }

      dirty_state_.clr(DirtyState::GraphicsPipelineState);
    }
    // under tessellation the vertex and hull shaders run in the object stage, the domain shader in the mesh stage; with
    // a geometry shader the vertex shader runs in the object stage, the geometry shader in the mesh stage
    // a mesh shader pipeline has an object stage (its amplification shader, if any) and a mesh stage
    bool mesh = pso_graphics_->threads_per_patch || Geometry() || pso_graphics_->mesh_shader;
    bool vertex = !mesh;
    // the root arguments and static samplers reach the mesh stage where its function reads them
    // (mesh_reads_root_arguments): not the pass-through mesh stage of a pipeline without a geometry shader
    bool mesh_reads = mesh && pso_graphics_->mesh_reads_root_arguments;
    auto geometry_buffer = [&](uint64_t Offset, uint8_t Index, obj_handle_t Earlier = 0) {
      if (mesh)
        EncodeBuffer(WMTRenderCommandSetObjectBuffer, Offset, Index, Earlier);
      if (mesh_reads)
        EncodeBuffer(WMTRenderCommandSetMeshBuffer, Offset, Index, Earlier);
      if (vertex)
        EncodeBuffer(WMTRenderCommandSetVertexBuffer, Offset, Index, Earlier);
      EncodeBuffer(WMTRenderCommandSetFragmentBuffer, Offset, Index, Earlier);
    };
    if (dirty_state_.test(DirtyState::VertexBuffer)) {
      auto [Offset, Stride] = PopulateVertexBufferTable();
      // and the mesh stage, which runs the vertex shader where a list of triangles has it alone (MeshOnly)
      if (Stride && mesh) {
        EncodeBuffer(WMTRenderCommandSetObjectBuffer, Offset, SM50_BINDING_INDEX_VERTEX_BUFFER);
        if (pso_graphics_->mesh_only)
          EncodeBuffer(WMTRenderCommandSetMeshBuffer, Offset, SM50_BINDING_INDEX_VERTEX_BUFFER);
      }
      if (Stride && vertex)
        EncodeBuffer(WMTRenderCommandSetVertexBuffer, Offset, SM50_BINDING_INDEX_VERTEX_BUFFER);
      dirty_state_.clr(DirtyState::VertexBuffer);
    }
    // a tessellated draw's stages read the device's tessellation templates; EncodeBuffer binds them once a pass
    if (pso_graphics_->threads_per_patch)
      if (auto templates = device_->GetTessellationTemplates())
        for (auto stage : {WMTRenderCommandSetObjectBuffer, WMTRenderCommandSetMeshBuffer})
          EncodeBuffer(stage, 0, SM50_BINDING_INDEX_TESSELLATION_TEMPLATES, templates);

    if (dirty_state_.test(DirtyState::GraphicsRootArguments) && !SkipResourceBinding) {
      if (rootsig_graphics_) {
        auto uploaded =
            UploadRootArguments(rootsig_graphics_.ptr(), rootarg_graphics_staging_, rootarg_graphics_uploaded_);
        geometry_buffer(uploaded.offset, SM50_BINDING_INDEX_ROOT_ARGUMENTS, uploaded.buffer);
      }
      dirty_state_.clr(DirtyState::GraphicsRootArguments);
    }

    if (dirty_state_.test(DirtyState::GraphicsRootSignature) && !SkipResourceBinding) {
      if (rootsig_graphics_ && rootsig_graphics_->NumStaticSamplers)
        geometry_buffer(EncodeStaticSamplers(rootsig_graphics_.ptr()), SM50_BINDING_INDEX_STATIC_SAMPLERS);
      dirty_state_.clr(DirtyState::GraphicsRootSignature);
    }

    // a conservative rasterizer's mesh stage grows triangles by pixels of the viewport each chooses, clipping depth
    // as the pipeline does and facing as it does
    if (pso_graphics_->conservative && conservative_dirty) {
      auto [mapped, offset] = allocator_->AllocateGPUHeap(sizeof(SM50_CONSERVATIVE_VIEWPORTS), 16);
      auto data = static_cast<SM50_CONSERVATIVE_VIEWPORTS *>(mapped);
      memset(data, 0, sizeof(*data));
      for (UINT i = 0; i < std::min<UINT>(num_viewports, std::size(data->viewports)); i++)
        data->viewports[i][0] = viewports[i].TopLeftX, data->viewports[i][1] = viewports[i].TopLeftY,
        data->viewports[i][2] = viewports[i].Width, data->viewports[i][3] = viewports[i].Height;
      data->state[0] = pso_graphics_->depth_clip_mode == WMTDepthClipModeClip;
      data->state[1] = pso_graphics_->winding == WMTWindingCounterClockwise;
      if (allocator_->records_)
        allocator_->RecordGPUHeap(mapped, sizeof(*data), "conservative viewports");
      EncodeBuffer(WMTRenderCommandSetMeshBuffer, offset, SM50_BINDING_INDEX_VIEWPORTS);
    }

    // a primitive names one of the viewports and scissors there may be (D3D11.3 15.8.1). a viewport that is not set
    // is of no size and a scissor that is not set is empty (15.6, 15.7): nothing is drawn through either, which an
    // empty scissor says to Metal, whatever viewport it has there
    static_assert(SM50_VIEWPORTS == D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE);
    // the depth ranges, then the blend factor of a pixel shader that blends itself (SM50_SHADER_BLEND)
    if (dirty_state_.test(DirtyState::Viewport) ||
        (dirty_state_.test(DirtyState::BlendFactor) && pso_graphics_->shader_blend)) {
      auto [ranges, ranges_offset] =
          allocator_->AllocateGPUHeap((2 * count + std::size(blend_factor_)) * sizeof(float), alignof(float));
      // with no viewport set, Metal's own has the range 0 to 1
      for (auto i = 0u; i < count; i++) {
        ((float *)ranges)[2 * i] = num_viewports ? viewports[i < num_viewports ? i : 0].MinDepth : 0;
        ((float *)ranges)[2 * i + 1] = num_viewports ? viewports[i < num_viewports ? i : 0].MaxDepth : 1;
      }
      std::copy_n(blend_factor_, std::size(blend_factor_), (float *)ranges + 2 * count);
      EncodeBuffer(WMTRenderCommandSetFragmentBuffer, ranges_offset, SM50_BINDING_INDEX_DEPTH_RANGES);
      depth_ranges_ = allocator_->gpu_heap_buffer_address_ + ranges_offset;
    }
    if (dirty_state_.test(DirtyState::Viewport)) {
      auto [scale, scale_offset] = allocator_->AllocateGPUHeap(SM50_RASTER_SCALE_WORDS * sizeof(float), alignof(float));
      *(float *)scale = 1;
      // a mesh shader's groups from the first (DispatchMesh)
      std::fill_n((uint32_t *)scale + SM50_RASTER_SCALE_MESH_ORIGIN, 3, 0u);
      // with no viewport set, Metal's covers the pass
      auto pass = static_cast<RenderEncoderData *>(allocator_->encoder_current);
      for (auto i = 0u; i < SM50_VIEWPORTS; i++) {
        auto &viewport = viewports[i < num_viewports ? i : 0];
        ((float *)scale)[1 + 2 * i] = num_viewports ? viewport.Width : pass->render_target_width;
        ((float *)scale)[2 + 2 * i] = num_viewports ? viewport.Height : pass->render_target_height;
      }
      if (num_viewports) {
        auto metal_viewport = allocator_->AllocateCommandData<WMTViewport>(count);
        for (auto i = 0u; i < count; i++) {
          auto &viewport = viewports[i < num_viewports ? i : 0];
          metal_viewport[i] = {viewport.TopLeftX, viewport.TopLeftY, viewport.Width,
                               viewport.Height,   viewport.MinDepth, viewport.MaxDepth};
        }
        *(float *)scale =
            EnlargeViewports({metal_viewport, count}, D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);
        auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_setviewports>();
        cmd.type = WMTRenderCommandSetViewports;
        cmd.viewports.set(metal_viewport);
        cmd.viewport_count = count;
        static_cast<RenderEncoderData *>(allocator_->encoder_current)->viewports += count;
      }
      EncodeBuffer(WMTRenderCommandSetVertexBuffer, scale_offset, SM50_BINDING_INDEX_RASTER_SCALE);
      EncodeBuffer(WMTRenderCommandSetMeshBuffer, scale_offset, SM50_BINDING_INDEX_RASTER_SCALE);
      raster_scale_ = allocator_->gpu_heap_buffer_address_ + scale_offset;
      std::copy_n((const uint32_t *)scale, SM50_RASTER_SCALE_WORDS, raster_scale_words_);
      dirty_state_.clr(DirtyState::Viewport);
    }

    if (dirty_state_.test(DirtyState::ScissorRect)) {
      // Metal wants scissors within the pass
      auto pass = static_cast<RenderEncoderData *>(allocator_->encoder_current);
      LONG width = pass->render_target_width, height = pass->render_target_height;
      auto metal_scissors = allocator_->AllocateCommandData<WMTScissorRect>(count);
      for (auto i = 0u; i < count; i++) {
        if (i < num_scissors && i < num_viewports) {
          // a viewport scissors to the pixels its floored extents hold (D3D11.3 15.6.1)
          auto &d3d_rect = scissors[i];
          auto &viewport = viewports[i];
          LONG left = std::clamp(std::max(d3d_rect.left, LONG(std::floor(viewport.TopLeftX))), (LONG)0, width);
          LONG top = std::clamp(std::max(d3d_rect.top, LONG(std::floor(viewport.TopLeftY))), (LONG)0, height);
          LONG right =
              std::clamp(std::min(d3d_rect.right, LONG(std::floor(viewport.TopLeftX + viewport.Width))), left, width);
          LONG bottom =
              std::clamp(std::min(d3d_rect.bottom, LONG(std::floor(viewport.TopLeftY + viewport.Height))), top, height);
          metal_scissors[i] = {uint32_t(left), uint32_t(top), uint32_t(right - left), uint32_t(bottom - top)};
        } else {
          metal_scissors[i] = {};
        }
      }
      auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_setscissorrects>();
      cmd.type = WMTRenderCommandSetScissorRects;
      cmd.scissor_rects.set(metal_scissors);
      // Metal takes as many scissors as it has viewports, or one, and it has one viewport until some are set
      cmd.rect_count = num_viewports ? count : 1;
      pass->scissors += cmd.rect_count;
      dirty_state_.clr(DirtyState::ScissorRect);
    }

    if (dirty_state_.test(DirtyState::BlendFactor)) {
      auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_setblendcolor>();
      cmd.type = WMTRenderCommandSetBlendFactor;
      cmd.red = blend_factor_[0];
      cmd.green = blend_factor_[1];
      cmd.blue = blend_factor_[2];
      cmd.alpha = blend_factor_[3];
      dirty_state_.clr(DirtyState::BlendFactor);
    }

    if (dirty_state_.test(DirtyState::StencilRef)) {
      auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_setstencilref>();
      cmd.type = WMTRenderCommandSetStencilRef;
      cmd.stencil_ref = stencil_ref_;
      cmd.back_stencil_ref = back_stencil_ref_;
      dirty_state_.clr(DirtyState::StencilRef);
    }

    // the pixel shader's test where the GPU has none: whether the draw tests, and the samples that pass
    if (pso_graphics_->depth_bounds_emulated) {
      uint32_t words[2] = {bounds_tested, dsv.Width};
      auto &bytes = allocator_->EncodeRenderCommand<wmtcmd_render_setbytes>();
      bytes.type = WMTRenderCommandSetFragmentBytes;
      bytes.bytes.set(memcpy(allocator_->AllocateCPUHeap(sizeof(words), 16), words, sizeof(words)));
      bytes.length = sizeof(words);
      bytes.index = SM50_BINDING_INDEX_DEPTH_BOUNDS;
      // The shader declares the samples buffer even when the draw has no depth attachment.
      auto &buffer = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
      buffer.type = WMTRenderCommandSetFragmentBuffer;
      buffer.buffer = bounds_tested ? depth_bounds_samples_.handle : device_->GetNullBufferHandle();
      buffer.offset = 0;
      buffer.index = SM50_BINDING_INDEX_DEPTH_BOUNDS_SAMPLES;
    }
    // coarse shading's rates and combiners, and its image's bytes (or none, but a buffer bound)
    if (coarse) {
      D3D12_RESOURCE_DESC desc{};
      if (shading_rate_image_)
        shading_rate_image_->GetDesc(&desc);
      SM50_SHADING_RATE rates{
          uint32_t(shading_rate_),
          {uint32_t(shading_rate_combiners_[0]), uint32_t(shading_rate_combiners_[1])},
          uint32_t(desc.Width),
          desc.Height
      };
      auto &bytes = allocator_->EncodeRenderCommand<wmtcmd_render_setbytes>();
      bytes.type = WMTRenderCommandSetFragmentBytes;
      bytes.bytes.set(memcpy(allocator_->AllocateCPUHeap(sizeof(rates), 16), &rates, sizeof(rates)));
      bytes.length = sizeof(rates);
      bytes.index = SM50_BINDING_INDEX_SHADING_RATE;
      if (shading_rate_image_) {
        auto &buffer = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
        buffer.type = WMTRenderCommandSetFragmentBuffer;
        buffer.buffer = shading_rate_bytes_;
        buffer.offset = shading_rate_offset_;
        buffer.index = SM50_BINDING_INDEX_SHADING_RATE_IMAGE;
      } else {
        auto &none = allocator_->EncodeRenderCommand<wmtcmd_render_setbytes>();
        none.type = WMTRenderCommandSetFragmentBytes;
        none.bytes.set(memset(allocator_->AllocateCPUHeap(16, 16), 0, 16));
        none.length = 16;
        none.index = SM50_BINDING_INDEX_SHADING_RATE_IMAGE;
      }
    }
    // a draw that writes depth changes which samples are in the bounds
    if (pso_graphics_->writes_depth && dsv.Texture && !(dsv.Flags & D3D12_DSV_FLAG_READ_ONLY_DEPTH))
      depth_bounds_stale_ = true;

    // stored depth passes inside the range, clamped to [0, 1]; an inverted range fails everything, and a pass without
    // a depth buffer passes. Metal's test is off at [0, 1]
    bool bounded = pso_graphics_->depth_bounds && dsv.Texture;
    if (bounded && depth_bounds_[0] > depth_bounds_[1])
      return DrawCallStatus::Invalid;
    if (dirty_state_.test(DirtyState::DepthBounds) && (bounded || depth_bounds_set_)) {
      auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_setdepthtestbounds>();
      cmd.type = WMTRenderCommandSetDepthTestBounds;
      cmd.min = bounded ? std::clamp(depth_bounds_[0], 0.0f, 1.0f) : 0.0f;
      cmd.max = bounded ? std::clamp(depth_bounds_[1], 0.0f, 1.0f) : 1.0f;
      depth_bounds_set_ = bounded;
    }
    dirty_state_.clr(DirtyState::DepthBounds);

    if (pso_graphics_->uses_uav || pso_graphics_->stream_output)
      uav_draw_pass_ = allocator_->encoder_current;
    // stream output appends in the order of the draws, across passes too
    if (pso_graphics_->stream_output)
      OrderingPoint();
    return DrawCallStatus::Ordinary;
  }

  // what EncodeBuffer bound in the render pass `pass`, by stage (vertex, fragment, mesh, object) and index: the same
  // buffer again needs only its offset (Metal's set*BufferOffset), and the same offset nothing. an indirect command
  // buffer may leave other bindings behind, so ExecuteIndirect forgets them (ForgetBuffers)
  struct {
    const EncoderData *pass = nullptr;
    obj_handle_t buffer[4][SM50_BINDING_INDEX_TESSELLATION_TEMPLATES + 1];
    uint64_t offset[4][SM50_BINDING_INDEX_TESSELLATION_TEMPLATES + 1];
  } bound_;

  void
  ForgetBuffers() {
    bound_.pass = nullptr;
  }

  // binds the list's buffer over the arguments at an offset, or the earlier one an address was made in (`Earlier`,
  // ArgumentsAt)
  void
  EncodeBuffer(WMTRenderCommandType Type, uint64_t Offset, uint8_t Index, obj_handle_t Earlier = 0) {
    obj_handle_t buffer = Earlier ? Earlier : obj_handle_t(allocator_->gpu_heap_buffer_);
    // each Set*Buffer is followed by its Set*BufferOffset (WMTRenderCommandType)
    unsigned stage = Type == WMTRenderCommandSetVertexBuffer     ? 0
                     : Type == WMTRenderCommandSetFragmentBuffer ? 1
                     : Type == WMTRenderCommandSetMeshBuffer     ? 2
                     : Type == WMTRenderCommandSetObjectBuffer   ? 3
                                                                 : 4;
    if (stage < 4 && Index < std::size(bound_.buffer[0])) {
      if (bound_.pass != allocator_->encoder_current) {
        bound_.pass = allocator_->encoder_current;
        memset(bound_.buffer, 0, sizeof(bound_.buffer));
      }
      auto &buffer_bound = bound_.buffer[stage][Index], &offset_bound = bound_.offset[stage][Index];
      if (buffer_bound == buffer && offset_bound == Offset)
        return;
      if (buffer_bound == buffer) {
        auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_setbufferoffset>();
        cmd.type = WMTRenderCommandType(Type + 1);
        cmd.offset = Offset;
        cmd.index = Index;
        offset_bound = Offset;
        return;
      }
      buffer_bound = buffer;
      offset_bound = Offset;
    }
    auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
    cmd.type = Type;
    cmd.buffer = buffer;
    cmd.offset = Offset;
    cmd.index = Index;
  }

  // what a tessellated draw's object stage reads: the index buffer view, then the patch size, 16-byte aligned
  struct TessellationInputs {
    D3D12_INDEX_BUFFER_VIEW view;
    alignas(16) uint32_t control_points;
  };

  // an indexed draw's view: without an index buffer every index reads 0, as from an empty 16-bit one
  D3D12_INDEX_BUFFER_VIEW
  IndexedView(const D3D12_INDEX_BUFFER_VIEW &View) {
    return View.BufferLocation ? View : D3D12_INDEX_BUFFER_VIEW{0, 0, DXGI_FORMAT_R16_UINT};
  }

  // a tessellated draw runs each group of patches in one object threadgroup of 32 threads (airconv's layout), which
  // reads the draw's D3D12 arguments (their layout is the one it expects) and its TessellationInputs; the view of a
  // draw without indices is empty
  template <typename Arguments>
  void
  EncodeTessellationDraw(const Arguments &Args, uint32_t Count, uint32_t ControlPoints, const D3D12_INDEX_BUFFER_VIEW &View) {
    auto patches = ControlPoints ? Count / ControlPoints : 0, per_group = 32 / pso_graphics_->threads_per_patch;
    if (!patches || !Args.InstanceCount)
      return;
    TessellationInputs inputs{View, ControlPoints};
    auto [Mapped, Offset] = allocator_->AllocateGPUHeap(sizeof(inputs) + sizeof(Args), 16);
    if (allocator_->records_)
      allocator_->RecordGPUHeap(Mapped, sizeof(inputs) + sizeof(Args), "tessellation inputs", sizeof(inputs),
                                predication_region_ ? sizeof(uint32_t) : 0);
    memcpy(Mapped, &inputs, sizeof(inputs));
    memcpy(reinterpret_cast<char *>(Mapped) + sizeof(inputs), &Args, sizeof(Args));
    // the object stage counts patches from the arguments' first word, so a skipped draw has none
    if (predication_region_)
      predicated_.push_back({uint32_t((Offset + sizeof(inputs)) / 4), reinterpret_cast<const uint32_t *>(&Args)[0], 0});
    EncodeBuffer(WMTRenderCommandSetObjectBuffer, Offset, SM50_BINDING_INDEX_INDEX_BUFFER);
    EncodeBuffer(
        WMTRenderCommandSetObjectBuffer, Offset + offsetof(TessellationInputs, control_points),
        SM50_BINDING_INDEX_PATCH_SIZE
    );
    EncodeBuffer(WMTRenderCommandSetObjectBuffer, Offset + sizeof(inputs), SM50_BINDING_INDEX_DRAW_ARGUMENTS);
    auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_mullion_tessellation_mesh_draw>();
    cmd.type = WMTRenderCommandMullionTessellationMeshDraw;
    cmd.draw_arguments_offset = Offset + sizeof(inputs);
    cmd.instance_count = Args.InstanceCount;
    cmd.threads_per_patch = pso_graphics_->threads_per_patch;
    cmd.patch_per_group = per_group;
    cmd.patch_per_mesh_instance = (patches + per_group - 1) / per_group;
    cmd.parts = pso_graphics_->tessellation_parts;
  }

  // a sampler that needs what the pipeline leaves out may have come since it was set (MTLD3D12PipelineState::left_out)
  void
  TakeFuller() {
    while (pso_graphics_ && (pso_graphics_->left_out & device_->sampler_emulations)) {
      pso_graphics_ = static_cast<MTLD3D12GraphicsPipelineState *>(pso_graphics_->Fuller(device_));
      dirty_state_.set(DirtyState::GraphicsPipelineState);
    }
  }

  static bool
  Strip(D3D12_PRIMITIVE_TOPOLOGY Topology) {
    return Topology == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP || Topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP ||
           Topology == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ || Topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ;
  }

  static bool
  Adjacency(D3D12_PRIMITIVE_TOPOLOGY Topology) {
    return Topology >= D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ && Topology <= D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ;
  }

  // whether a pipeline without a geometry shader runs as a geometry pipeline that passes its primitives on: its
  // vertices come with adjacency, which no other pipeline takes
  bool
  PassesAdjacency() {
    return pso_graphics_->adjacency && Adjacency(topology_);
  }

  bool
  Geometry() {
    return pso_graphics_->geometry_shader || PassesAdjacency();
  }

  WMT::RenderPipelineState
  RenderTargets(WMT::RenderPipelineState Pipeline) {
    auto pass = static_cast<RenderEncoderData *>(allocator_->encoder_current);
    uint32_t mask = 0;
    WMTPixelFormat colors[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    for (unsigned i = 0; i < std::size(pass->colors); i++)
      if (pass->colors[i].attachment) {
        mask |= 1u << i;
        if (rtvs[i].Texture)
          colors[i] = rtvs[i].Texture->pixelFormat(rtvs[i].View);
      }
    return pso_graphics_->RenderTargets(
        Pipeline, mask, pass->dsv_planar_flags, !active_statistics_.empty(), !active_queries_.empty(), CoarseShading(),
        dsv.Texture ? dsv.Texture->pixelFormat(dsv.View) : WMTPixelFormatInvalid, colors
    );
  }

  // the pipeline's variant for the topology: a geometry pipeline has one for strips, and with stream output one
  // for each that only counts
  WMT::RenderPipelineState
  Pipeline(bool counting = false) {
    pso_graphics_->WaitPipeline();
    bool strip = Strip(topology_);
    if (PassesAdjacency() || pso_graphics_->patches)
      return RenderTargets(pso_graphics_->TopologyPipeline(topology_, counting));
    if (pso_graphics_->geometry_shader && (strip || counting))
      return RenderTargets(pso_graphics_->GeometryPipeline(strip, counting));
    return RenderTargets(pso_graphics_->pso);
  }

  // whether the draw's pipeline is a mesh function alone, which runs the vertex shader in the mesh stage: a list of
  // triangles passed through (SM50_SHADER_PSO_GEOMETRY_SHADER_DATA's mesh_only)
  bool
  MeshOnly() {
    return pso_graphics_->mesh_only && !PassesAdjacency() && !pso_graphics_->patches && !Strip(topology_);
  }

  // a geometry-shader draw runs its vertices in warps of object threadgroups (airconv's layout), which read the draw's
  // D3D12 arguments and its index buffer view, empty for a draw without indices; or of mesh threadgroups (MeshOnly)
  template <typename Arguments>
  void
  EncodeGeometryDraw(const Arguments &Args, uint32_t Count, const D3D12_INDEX_BUFFER_VIEW &View) {
    if (!Count || !Args.InstanceCount)
      return;
    auto [vertex_per_warp, increment] = get_gs_vertex_count(topology_, pso_graphics_->vertex_registers);
    // not one primitive fits an object threadgroup's payload
    if (!increment)
      return;
    uint32_t warps = (Count - 1) / increment + 1;
    auto parts = geometry_draw_parts(
        warps, Args.InstanceCount,
        get_gs_max_object_threadgroups(
            topology_, pso_graphics_->vertex_registers, pso_graphics_->mesh_instances, -1ull, device_->mesh_bytes_per_draw,
            pso_graphics_->gs_mesh_invocations
        )
    );
    if (!parts)
      return;
    auto [Mapped, Offset] = allocator_->AllocateGPUHeap(sizeof(View) + sizeof(Args), 16);
    if (allocator_->records_)
      allocator_->RecordGPUHeap(Mapped, sizeof(View) + sizeof(Args), "geometry inputs", sizeof(View),
                                predication_region_ ? sizeof(uint32_t) : 0);
    memcpy(Mapped, &View, sizeof(View));
    memcpy(reinterpret_cast<char *>(Mapped) + sizeof(View), &Args, sizeof(Args));
    // the object stage reads the vertex count from the arguments' first word, so a skipped draw has none
    if (predication_region_)
      predicated_.push_back({uint32_t((Offset + sizeof(View)) / 4), reinterpret_cast<const uint32_t *>(&Args)[0], 0});
    bool mesh_only = MeshOnly();
    auto stage = mesh_only ? WMTRenderCommandSetMeshBuffer : WMTRenderCommandSetObjectBuffer;
    EncodeBuffer(stage, Offset, SM50_BINDING_INDEX_INDEX_BUFFER);
    EncodeBuffer(stage, Offset + sizeof(View), SM50_BINDING_INDEX_DRAW_ARGUMENTS);
    auto draw = [&] {
      auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_mullion_geometry_draw>();
      cmd.type = mesh_only ? WMTRenderCommandMullionMeshDraw : WMTRenderCommandMullionGeometryDraw;
      cmd.draw_arguments_offset = Offset + sizeof(View);
      cmd.warp_count = warps;
      cmd.instance_count = Args.InstanceCount;
      cmd.vertex_per_warp = vertex_per_warp;
      cmd.max_object_threadgroups = *parts;
      // a conservative hull's points, a thread each, or a geometry shader's invocations
      cmd.mesh_threads = pso_graphics_->conservative_hull ? SM50_CONSERVATIVE_HULL_POINTS : pso_graphics_->gs_mesh_invocations;
    };
    if (!pso_graphics_->stream_output)
      return draw();
    auto targets = StreamOutputTargets();
    uint64_t groups = uint64_t(warps) * Args.InstanceCount;
    // one pass keeps only the filled sizes
    bool single = SinglePass();
    auto [Scratch, ScratchOffset] = allocator_->AllocateGPUHeap(
        single ? SM50StreamOutputZeroed(0) : SM50StreamOutputScratch(groups, vertex_per_warp * pso_graphics_->gs_instances), 16
    );
    // a draw whose scratch no heap has room for: the list fails when it is closed
    if (!Scratch)
      return;
    targets.scratch = allocator_->gpu_heap_buffer_address_ + ScratchOffset;
    targets.warps = warps;
    targets.instances = Args.InstanceCount;
    auto [Targets, TargetsOffset] = allocator_->AllocateGPUHeap(sizeof(targets), 16);
    if (allocator_->records_)
      allocator_->RecordGPUHeap(Targets, sizeof(targets), "stream output targets");
    memcpy(Targets, &targets, sizeof(targets));
    StreamOut(TargetsOffset, ScratchOffset, draw, [&] {
      auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_draw>();
      cmd = {WMTRenderCommandDraw};
      cmd.primitive_type = WMTPrimitiveTypePoint;
      cmd.vertex_count = SM50StreamOutputZeroed(groups) / sizeof(uint32_t);
      cmd.instance_count = 1;
    }, single);
  }

  // whether the draw's stream output runs in one pass (SM50StreamOutputSinglePass): the pipeline's variant for the
  // topology was made so
  bool
  SinglePass() {
    return !PassesAdjacency() && !pso_graphics_->patches &&
           (Strip(topology_) ? pso_graphics_->so_single_strip : pso_graphics_->so_single);
  }

  // the stream output targets bound, with no scratch area yet
  SM50_STREAM_OUTPUT_TARGETS
  StreamOutputTargets() {
    SM50_STREAM_OUTPUT_TARGETS targets{};
    for (unsigned b = 0; b < std::size(so_views_); b++) {
      targets.address[b] = so_views_[b].BufferLocation;
      targets.size[b] = so_views_[b].SizeInBytes;
      targets.filled[b] = so_views_[b].BufferFilledSizeLocation;
      targets.statistics[b] = so_statistics_[b];
    }
    return targets;
  }

  // stream output (SM50_STREAM_OUTPUT_TARGETS at `TargetsOffset`) runs a draw twice: once counting each geometry
  // invocation's primitives, and after a barrier again, writing them and drawing. its scratch area holds the filled
  // sizes, a total per stream and object threadgroup, and a count per stream and invocation (at most a primitive per
  // vertex). the passes add to all but the counts, which `zero` zeroes first (zero_words), on every execution of the
  // list, replays included. in one pass (`single`, SM50StreamOutputSinglePass) the draw runs once, after its filled
  // sizes are copied to its scratch (stream_output_snapshot), which is all the scratch holds
  template <typename Draw, typename Zero>
  void
  StreamOut(uint64_t TargetsOffset, uint64_t ScratchOffset, Draw draw, Zero zero, bool single = false) {
    EncodeBuffer(WMTRenderCommandSetObjectBuffer, TargetsOffset, SM50_BINDING_INDEX_STREAM_OUTPUT0);
    EncodeBuffer(WMTRenderCommandSetMeshBuffer, TargetsOffset, SM50_BINDING_INDEX_STREAM_OUTPUT0);
    EncodeBuffer(WMTRenderCommandSetVertexBuffer, single ? TargetsOffset : ScratchOffset, kCustomBufferArgumentIndex0);
    auto set_pso = [&](WMT::RenderPipelineState pso) {
      auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_setpso>();
      cmd.type = WMTRenderCommandSetPSO;
      cmd.pso = pso;
    };
    auto barrier = [&](WMTRenderStages after, WMTRenderStages before) {
      auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_memory_barrier>();
      cmd.type = WMTRenderCommandMemoryBarrier;
      cmd.scope = WMTBarrierScopeBuffers;
      cmd.stages_after = after;
      cmd.stages_before = before;
    };
    if (single) {
      set_pso(device_->GetLib().StreamOutputSnapshot());
      auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_draw>();
      cmd = {WMTRenderCommandDraw};
      cmd.primitive_type = WMTPrimitiveTypePoint;
      cmd.vertex_count = std::size(so_views_);
      cmd.instance_count = 1;
      barrier(WMTRenderStageVertex, WMTRenderStagePreRaster);
      set_pso(Pipeline());
      draw();
      barrier(WMTRenderStagePreRaster, WMTRenderStages(WMTRenderStagePreRaster | WMTRenderStageFragment));
      return;
    }
    set_pso(device_->GetLib().ZeroWords());
    zero();
    barrier(WMTRenderStageVertex, WMTRenderStagePreRaster);
    set_pso(Pipeline(true));
    draw();
    // without a geometry shader the object stage streams out
    barrier(WMTRenderStagePreRaster, WMTRenderStagePreRaster);
    set_pso(Pipeline());
    draw();
    // the next draw may read what this one wrote, as vertices or filled sizes
    barrier(WMTRenderStagePreRaster, WMTRenderStages(WMTRenderStagePreRaster | WMTRenderStageFragment));
  }

  void STDMETHODCALLTYPE
  DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation) {
    if (Bundled() && Record([=](auto *l) { l->DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation); }))
      return;
    // a draw of nothing: Metal takes no empty draw
    if (!VertexCountPerInstance || !InstanceCount)
      return;
    WMTPrimitiveType primitive_type;
    uint32_t cp_count;
    if (!to_metal_primitive_type(topology_, primitive_type, cp_count))
      return;
    DrawCallStatus status = PreDraw();
    if (status == DrawCallStatus::Invalid)
      return;
    CountDraw(VertexCountPerInstance, InstanceCount);
    if (pso_graphics_->threads_per_patch) {
      EncodeTessellationDraw(
          D3D12_DRAW_ARGUMENTS{VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation},
          VertexCountPerInstance, cp_count, {}
      );
      return;
    }
    if (Geometry()) {
      EncodeGeometryDraw(
          D3D12_DRAW_ARGUMENTS{VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation},
          VertexCountPerInstance, {}
      );
      return;
    }
    // Metal's indirect draw arguments are D3D12's, instances second
    if (auto offset = Predicated(
            D3D12_DRAW_ARGUMENTS{VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation}, 1
        )) {
      auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_draw_indirect>();
      cmd.type = WMTRenderCommandDrawIndirect;
      cmd.primitive_type = primitive_type;
      cmd.indirect_args_buffer = allocator_->gpu_heap_buffer_;
      cmd.indirect_args_offset = *offset;
      return;
    }

    auto &cmd_draw = allocator_->EncodeRenderCommand<wmtcmd_render_draw>();
    cmd_draw.type = WMTRenderCommandDraw;
    cmd_draw.primitive_type = primitive_type;
    cmd_draw.base_instance = StartInstanceLocation;
    cmd_draw.instance_count = InstanceCount;
    cmd_draw.vertex_start = StartVertexLocation;
    cmd_draw.vertex_count = VertexCountPerInstance;
  };

  void STDMETHODCALLTYPE
  DrawIndexedInstanced(
      UINT IndexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, INT BaseVertexLocation,
      UINT StartInstanceLocation
  ) {
    if (Bundled() && Record([=](auto *l) { l->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartVertexLocation, BaseVertexLocation, StartInstanceLocation); }))
      return;
    if (!IndexCountPerInstance || !InstanceCount)
      return;
    WMTPrimitiveType primitive_type;
    uint32_t cp_count;
    if (!to_metal_primitive_type(topology_, primitive_type, cp_count))
      return;
    DrawCallStatus status = PreDraw();
    if (status == DrawCallStatus::Invalid)
      return;
    CountDraw(IndexCountPerInstance, InstanceCount, true, StartVertexLocation);
    if (pso_graphics_->threads_per_patch) {
      EncodeTessellationDraw(
          D3D12_DRAW_INDEXED_ARGUMENTS{
              IndexCountPerInstance, InstanceCount, StartVertexLocation, BaseVertexLocation, StartInstanceLocation
          },
          IndexCountPerInstance, cp_count, IndexedView(index_view_)
      );
      return;
    }
    if (Geometry()) {
      EncodeGeometryDraw(
          D3D12_DRAW_INDEXED_ARGUMENTS{
              IndexCountPerInstance, InstanceCount, StartVertexLocation, BaseVertexLocation, StartInstanceLocation
          },
          IndexCountPerInstance, IndexedView(index_view_)
      );
      return;
    }
    // Metal reads behind the view; Direct3D reads zero there, including with no index buffer (D3D11.3 8.19.2)
    const uint64_t index_size = index_type == WMTIndexTypeUInt32 ? sizeof(uint32_t) : sizeof(uint16_t);
    const uint64_t there = index_buffer ? index_view_.SizeInBytes / index_size : 0;
    auto draw = [&](WMT::Buffer indices, uint64_t indices_offset, UINT start, UINT count) {
      if (!count)
        return;
      if (auto offset = Predicated(
              D3D12_DRAW_INDEXED_ARGUMENTS{count, InstanceCount, start, BaseVertexLocation, StartInstanceLocation}, 1
          )) {
        auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_draw_indexed_indirect>();
        cmd.type = WMTRenderCommandDrawIndexedIndirect;
        cmd.primitive_type = primitive_type;
        cmd.index_type = index_type;
        cmd.index_buffer = indices;
        cmd.index_buffer_offset = indices_offset;
        cmd.indirect_args_buffer = allocator_->gpu_heap_buffer_;
        cmd.indirect_args_offset = *offset;
        return;
      }
      auto &cmd_draw = allocator_->EncodeRenderCommand<wmtcmd_render_draw_indexed>();
      cmd_draw.type = WMTRenderCommandDrawIndexed;
      cmd_draw.primitive_type = primitive_type;
      cmd_draw.index_type = index_type;
      cmd_draw.index_count = count;
      cmd_draw.index_buffer = indices;
      cmd_draw.index_buffer_offset = indices_offset + start * index_size;
      cmd_draw.instance_count = InstanceCount;
      cmd_draw.base_vertex = BaseVertexLocation;
      cmd_draw.base_instance = StartInstanceLocation;
    };
    // the byte address wraps, including its initial multiplication (D3D11.3 8.6.1, 8.19.1)
    const uint64_t period = (uint64_t(~UINT(0)) + 1) / index_size;
    for (uint64_t consumed = 0; consumed < IndexCountPerInstance;) {
      const UINT start = (StartVertexLocation + consumed) % period;
      const UINT count = std::min<uint64_t>(IndexCountPerInstance - consumed, period - start);
      const UINT inside = start < there ? std::min<uint64_t>(count, there - start) : 0;
      draw(index_buffer, index_offset, start, inside);
      if (auto outside = count - inside) {
        auto [zeros, offset] = allocator_->AllocateGPUHeap(outside * index_size, index_size);
        if (allocator_->records_)
          allocator_->RecordGPUHeap(zeros, outside * index_size, "zero indices");
        if (!zeros)
          return;
        memset(zeros, 0, outside * index_size);
        draw(allocator_->gpu_heap_buffer_, offset, 0, outside);
      }
      consumed += count;
    }
  };

  uint64_t
  EncodeRootArgument(MTLD3D12RootSignature *pRootSig, uint64_t const pStaging[64]) {
    auto [Ptr, Offset] = allocator_->AllocateGPUHeap(sizeof(uint64_t) * pRootSig->UploadQwords, 64);
    if (allocator_->records_)
      allocator_->RecordGPUHeap(Ptr, sizeof(uint64_t) * pRootSig->UploadQwords, "root argument table");
    memcpy(Ptr, pStaging, pRootSig->UploadQwords * sizeof(uint64_t));
    return Offset;
  }

  // room for the root arguments of each of `Count` commands of an ExecuteIndirect whose signature changes some: its
  // resolver writes those of the commands it runs, on the GPU, so recording copies nothing for a command
  uint64_t
  ReserveRootArguments(MTLD3D12RootSignature *pRootSig, UINT Count) {
    return std::get<1>(allocator_->AllocateGPUHeap(sizeof(uint64_t) * pRootSig->UploadQwords * Count, 64));
  }

  uint64_t
  EncodeStaticSamplers(MTLD3D12RootSignature *pRootSig) {
    auto static_sampler_encode_size = sizeof(uint64_t) * pRootSig->NumStaticSamplers * 4;
    auto [Ptr, Offset] = allocator_->AllocateGPUHeap(static_sampler_encode_size, 64);
    if (allocator_->records_)
      allocator_->RecordGPUHeap(Ptr, static_sampler_encode_size, "static samplers");
    memcpy(Ptr, pRootSig->EncodedStaticSamplers, static_sampler_encode_size);
    return Offset;
  }

  // a new compute pass, where the next dispatch binds its state again
  ComputeEncoderData *
  StartComputePass(bool Parts = false) {
    allocator_->InvalidateCurrentPass();
    ComputeEncoderData *compute = Parts ? allocator_->AllocatePass<ComputePartsEncoderData>()
                                       : allocator_->AllocatePass<ComputeEncoderData>();
    compute->type = EncoderType::Compute;
    compute->cmd_head.type = WMTComputeCommandNop;
    compute->cmd_head.next.set(0);
    compute->cmd_tail = (wmtcmd_base *)&compute->cmd_head;
    dirty_state_.set(DirtyState::ComputeRootArguments, DirtyState::ComputeRootSignature);
    dirty_state_.set(DirtyState::ComputePipelineState);
    compute_root_buf_ = 0;
    return compute;
  }

  bool
  PreDispatch(bool SkipResourceBinding = false) {
    if (!allocator_->encoder_current || allocator_->encoder_current->type != EncoderType::Compute)
      Overlap(StartComputePass(), {});

    while (pso_compute_ && (pso_compute_->left_out & device_->sampler_emulations)) {
      pso_compute_ = static_cast<MTLD3D12ComputePipelineState *>(pso_compute_->Fuller(device_));
      dirty_state_.set(DirtyState::ComputePipelineState);
    }
    if (!pso_compute_)
        return false;
    // a pipeline whose Metal compile failed on a compile thread dispatches nothing
    pso_compute_->WaitPipeline();
    if (!pso_compute_->pso)
      return false;

    if (dirty_state_.test(DirtyState::ComputePipelineState)) {
      auto &cmd_setpso = allocator_->EncodeComputeCommand<wmtcmd_compute_setpso>();
      cmd_setpso.type = WMTComputeCommandSetPSO;
      cmd_setpso.pso = pso_compute_->pso;
      cmd_setpso.threadgroup_size = pso_compute_->threadgroup_size;
      dirty_state_.clr(DirtyState::ComputePipelineState);
      if (Logger::logLevel() == LogLevel::Trace)
        TRACE("pass ", allocator_->encoder_current->id, ": compute pipeline ", pso_compute_->name);
      if (device_->NamesPasses())
        device_->NamePass(allocator_->encoder_current, pso_compute_->name, pso_compute_->pso);
    }
    BindComputeRootArguments(SkipResourceBinding);
    return true;
  }

  // the compute root signature's arguments and static samplers, for the pass's kernels
  void
  BindComputeRootArguments(bool SkipResourceBinding = false) {
    if (dirty_state_.test(DirtyState::ComputeRootArguments) && !SkipResourceBinding) {
      if (rootsig_compute_) {
        auto uploaded =
            UploadRootArguments(rootsig_compute_.ptr(), rootarg_compute_staging_, rootarg_compute_uploaded_);
        if (uploaded.buffer == compute_root_buf_) {
          auto &cmd_argbuf = allocator_->EncodeComputeCommand<wmtcmd_compute_setbufferoffset>();
          cmd_argbuf.type = WMTComputeCommandSetBufferOffset;
          cmd_argbuf.offset = uploaded.offset;
          cmd_argbuf.index = SM50_BINDING_INDEX_ROOT_ARGUMENTS;
        } else {
          auto &cmd_argbuf = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
          cmd_argbuf.type = WMTComputeCommandSetBuffer;
          cmd_argbuf.buffer = uploaded.buffer;
          cmd_argbuf.offset = uploaded.offset;
          cmd_argbuf.index = SM50_BINDING_INDEX_ROOT_ARGUMENTS;
          compute_root_buf_ = uploaded.buffer;
        }
      }
      dirty_state_.clr(DirtyState::ComputeRootArguments);
    }

    if (dirty_state_.test(DirtyState::ComputeRootSignature) && !SkipResourceBinding) {
      if (rootsig_compute_ && rootsig_compute_->NumStaticSamplers) {
        auto Offset = EncodeStaticSamplers(rootsig_compute_.ptr());
        auto &cmd_argbuf = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
        cmd_argbuf.type = WMTComputeCommandSetBuffer;
        cmd_argbuf.buffer = allocator_->gpu_heap_buffer_;
        cmd_argbuf.offset = Offset;
        cmd_argbuf.index = SM50_BINDING_INDEX_STATIC_SAMPLERS;
      }
      dirty_state_.clr(DirtyState::ComputeRootSignature);
    }
  }

  void STDMETHODCALLTYPE
  Dispatch(UINT X, UINT Y, UINT Z) {
    if (Bundled() && Record([=](auto *l) { l->Dispatch(X, Y, Z); }))
      return;
    if (pso_compute_ && pso_compute_->groups_work_together) {
      PredicatedPasses predicated(this);
      auto parts = static_cast<ComputePartsEncoderData *>(StartComputePass(true));
      if (!PreDispatch())
        return;
      parts->at_once = pso_compute_->groups_at_once;
      parts->size = {X, Y, Z};
      parts->type = EncoderType::ComputeParts;
      Isolate(X, Y, Z);
      allocator_->InvalidateCurrentPass();
      return;
    }
    if (!PreDispatch())
      return;
    auto &group = pso_compute_->threadgroup_size;
    uint64_t threads = uint64_t(X) * Y * Z * group.width * group.height * group.depth;
    if (predication_region_ && !active_statistics_.empty()) {
      auto skips = allocator_->gpu_heap_buffer_address_ +
                   (reinterpret_cast<char *>(&predication_region_->skips) - static_cast<char *>(allocator_->gpu_heap_));
      predicated_statistics_.push_back({0, 0, threads, skips});
    } else {
      for (auto &[_, sum] : active_statistics_)
        sum.counts[10] += threads;
    }

    if (auto offset = Predicated(D3D12_DISPATCH_ARGUMENTS{X, Y, Z}, 0)) {
      auto &cmd = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch_indirect>();
      cmd.type = WMTComputeCommandDispatchIndirect;
      cmd.indirect_args_buffer = allocator_->gpu_heap_buffer_;
      cmd.indirect_args_offset = *offset;
      return;
    }
    auto &cmd_dispatch = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch>();
    cmd_dispatch.type = WMTComputeCommandDispatch;
    cmd_dispatch.size = {X, Y, Z};
    Isolate(X, Y, Z);
  };

  // with MULLION_D3D12_ISOLATE set a dispatch is a pass of its own, which the queue then waits for and times
  void
  Isolate(UINT X, UINT Y, UINT Z) {
    static const bool isolate = !env::getEnvVar("MULLION_D3D12_ISOLATE").empty();
    if (!isolate || !allocator_->encoder_current)
      return;
    TRACE("pass ", allocator_->encoder_current->id, ": dispatch of ", X, " by ", Y, " by ", Z, " groups");
    allocator_->InvalidateCurrentPass();
  }

  bool
  PreBlit() {
    if (!allocator_->encoder_current || allocator_->encoder_current->type != EncoderType::Blit) {
      allocator_->InvalidateCurrentPass();
      auto render = allocator_->AllocatePass<BlitEncoderData>();
      render->type = EncoderType::Blit;
      render->cmd_head.type = WMTBlitCommandNop;
      render->cmd_head.next.set(0);
      render->cmd_tail = (wmtcmd_base *)&render->cmd_head;
    }
    return true;
  }

  // whether a barrier before the open render pass began, and none since, left `resource` (or its `subresource`) in
  // `state`: no draw of the pass can have touched it but to read it
  bool
  LeftBefore(ID3D12Resource *resource, UINT subresource, D3D12_RESOURCE_STATES state) {
    auto named = named_.find(resource);
    return named != named_.end() && (named->second.state & state) && named->second.serial <= render_serial_ &&
           (named->second.subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES || named->second.subresource == subresource);
  }

  // the pass a copy goes in: a blit pass before the open render pass, when no draw in it can have read what the copy
  // writes or written what it reads (a barrier before the pass left the destination a copy's, and the source an
  // upload heap's or a copy's), else a blit pass after what came before. what a resource never named in a barrier is
  // in is not known, so a copy of one ends the pass. the hoisted pass is the current one until `Hoisted` ends
  struct Hoisted {
    MTLD3D12GraphicsCommandListImpl *list;
    EncoderData *render = nullptr;
    ~Hoisted() {
      if (render)
        list->allocator_->encoder_current = render;
    }
  };

  bool
  PreCopy(
      Hoisted &hoisted, ID3D12Resource *pDst, UINT DstSubresource, ID3D12Resource *pSrc, UINT SrcSubresource
  ) {
    auto render = allocator_->encoder_current;
    D3D12_HEAP_PROPERTIES heap{};
    if (!HoistsCopies() || !render || render->type != EncoderType::Render || render != targets_pass_ ||
        predication_region_ || !LeftBefore(pDst, DstSubresource, D3D12_RESOURCE_STATE_COPY_DEST) ||
        !((SUCCEEDED(pSrc->GetHeapProperties(&heap, nullptr)) && heap.Type == D3D12_HEAP_TYPE_UPLOAD) ||
          LeftBefore(pSrc, SrcSubresource, D3D12_RESOURCE_STATE_COPY_SOURCE)))
      return PreBlit();
    if (hoisted_for_ != render || hoisted_ != allocator_->encoder_last) {
      // a pass of its own, after the passes before the render pass, which follows it when it ends
      allocator_->encoder_current = nullptr;
      hoisted_ = allocator_->AllocatePass<BlitEncoderData>();
      hoisted_->type = EncoderType::Blit;
      hoisted_->cmd_head.type = WMTBlitCommandNop;
      hoisted_->cmd_head.next.set(0);
      hoisted_->cmd_tail = (wmtcmd_base *)&hoisted_->cmd_head;
      allocator_->InvalidateCurrentPass();
      hoisted_for_ = render;
    }
    hoisted.list = this;
    hoisted.render = render;
    allocator_->encoder_current = hoisted_;
    return true;
  }

  void STDMETHODCALLTYPE
  CopyBufferRegion(
      ID3D12Resource *pDstBuffer, UINT64 DstOffset, ID3D12Resource *pSrcBuffer, UINT64 SrcOffset, UINT64 ByteCount
  ) {
    PredicatedPasses predicated(this);
    if (!pDstBuffer || !pSrcBuffer)
      return;
    Hoisted hoisted{this};
    if (!PreCopy(hoisted, pDstBuffer, 0, pSrcBuffer, 0))
      return;

    obj_handle_t src = static_cast<MTLD3D12Resource *>(pSrcBuffer)->buffer->current()->buffer();
    obj_handle_t dst = static_cast<MTLD3D12Resource *>(pDstBuffer)->buffer->current()->buffer();
    // a copy that goes on where the pass's last command, a copy between the same two buffers, ended in both is that
    // copy made longer: Metal runs each copy as a blit of its own, and titles upload small blocks one after another
    // (per-object constants). the buffers differ, so neither part reads what the other writes
    auto last = reinterpret_cast<wmtcmd_blit_copy_from_buffer_to_buffer *>(
        static_cast<BlitEncoderData *>(allocator_->encoder_current)->cmd_tail
    );
    if (src != dst && last->type == WMTBlitCommandCopyFromBufferToBuffer && last->src == src && last->dst == dst &&
        last->src_offset + last->copy_length == SrcOffset && last->dst_offset + last->copy_length == DstOffset) {
      last->copy_length += ByteCount;
      return;
    }
    auto &cmd_cp = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
    cmd_cp.type = WMTBlitCommandCopyFromBufferToBuffer;
    cmd_cp.src = src;
    cmd_cp.dst = dst;
    cmd_cp.src_offset = SrcOffset;
    cmd_cp.dst_offset = DstOffset;
    cmd_cp.copy_length = ByteCount;
  };

  // a copy between textures. Metal copies within one format: formats of one type group copy from a view of the source
  // in the destination's, where the source has one. what has no such view (depth, and a reinterpret copy: CopyResource's Remarks) goes by its
  // bytes through a buffer, where a block of a compressed format is a texel of the other. `dst_extent` is the
  // destination level's
  void
  CopyTexels(
      Texture *src, const MTL_DXGI_FORMAT_DESC &src_format, uint32_t src_level, uint32_t src_slice, WMTOrigin src_origin,
      WMTSize size, Texture *dst, const MTL_DXGI_FORMAT_DESC &dst_format, uint32_t dst_level, uint32_t dst_slice,
      WMTOrigin dst_origin, const D3D12_BOX &dst_extent
  ) {
    bool same = src->pixelFormat() == dst->pixelFormat();
    if (same || (DXGIFormatType(src_format.PlaneFormat) == DXGIFormatType(dst_format.PlaneFormat) &&
                 !DepthStencilPlanarFlags(src->pixelFormat()) && !DepthStencilPlanarFlags(dst->pixelFormat()) &&
                 (Forget_sRGB(src->pixelFormat()) == Forget_sRGB(dst->pixelFormat()) ||
                  (src->usage() & WMTTextureUsagePixelFormatView)))) {
      auto &cmd = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_texture_to_texture>();
      cmd.type = WMTBlitCommandCopyFromTextureToTexture;
      cmd.src = same ? src->current()->texture()
                     : src->view(src->checkViewUseFormat(src->fullView, dst->pixelFormat())).texture;
      cmd.src_level = src_level;
      cmd.src_slice = src_slice;
      cmd.src_origin = src_origin;
      cmd.src_size = size;
      cmd.dst = dst->current()->texture();
      cmd.dst_level = dst_level;
      cmd.dst_slice = dst_slice;
      cmd.dst_origin = dst_origin;
      return;
    }
    const uint32_t src_unit = src_format.Flag & MTL_DXGI_FORMAT_BC ? 4 : 1,
                   dst_unit = dst_format.Flag & MTL_DXGI_FORMAT_BC ? 4 : 1;
    const uint64_t wide = align(size.width, src_unit) / src_unit, high = align(size.height, src_unit) / src_unit;
    // a multisampled texel is its samples
    const uint64_t bytes_per_row = wide * src_format.BytesPerTexel * src->sampleCount(),
                   bytes_per_image = bytes_per_row * high;
    auto [buffer, offset] = allocator_->AllocateTempBuffer(bytes_per_image * size.depth, 256);

    auto &out = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer>();
    out.type = WMTBlitCommandCopyFromTextureToBuffer;
    out.src = src->current()->texture();
    out.level = src_level;
    out.slice = src_slice;
    out.origin = src_origin;
    out.size = size;
    out.dst = buffer;
    out.offset = offset;
    out.bytes_per_row = bytes_per_row;
    out.bytes_per_image = bytes_per_image;

    auto &in = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture>();
    in.type = WMTBlitCommandCopyFromBufferToTexture;
    in.src = buffer;
    in.src_offset = offset;
    in.bytes_per_row = bytes_per_row;
    in.bytes_per_image = bytes_per_image;
    // the units in the destination's texels, which a level smaller than a block does not have all of
    in.size = {std::min<uint64_t>(wide * dst_unit, dst_extent.right - dst_origin.x),
               std::min<uint64_t>(high * dst_unit, dst_extent.bottom - dst_origin.y), size.depth};
    in.dst = dst->current()->texture();
    in.level = dst_level;
    in.slice = dst_slice;
    in.origin = dst_origin;
  }

  void STDMETHODCALLTYPE
  CopyTextureRegion(
      const D3D12_TEXTURE_COPY_LOCATION *pDst, UINT DstX, UINT DstY, UINT DstZ, const D3D12_TEXTURE_COPY_LOCATION *pSrc,
      const D3D12_BOX *pSrcBox
  ) {
    PredicatedPasses predicated(this);
    if (!pDst || !pSrc)
      return;
    auto src_desc = pSrc->pResource->GetDesc();
    auto dst_desc = pDst->pResource->GetDesc();
    // a copy into a texture may run before the render pass, but not one of RGB32 or D24 texels, which a kernel
    // converts
    Hoisted hoisted{this};
    MTL_DXGI_FORMAT_DESC hoisted_format;
    if (pDst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX &&
        SUCCEEDED(MTLQueryDXGIFormat(device_->GetMTLDevice(), dst_desc.Format, hoisted_format)) &&
        !(hoisted_format.Flag & (MTL_DXGI_FORMAT_EMULATED_RGB32 | MTL_DXGI_FORMAT_EMULATED_D24))) {
      if (!PreCopy(hoisted, pDst->pResource, pDst->SubresourceIndex, pSrc->pResource,
                   pSrc->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX ? pSrc->SubresourceIndex : 0))
        return;
    } else if (!PreBlit())
      return;

    uint32_t src_level = 0, src_slice = 0, src_planar = 0;
    uint32_t dst_level = 0, dst_slice = 0, dst_planar = 0;
    D3D12_BOX src_box, full_src_box;

    if (pSrc->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
      full_src_box = {
          0, 0, 0,
          pSrc->PlacedFootprint.Footprint.Width,
          pSrc->PlacedFootprint.Footprint.Height,
          pSrc->PlacedFootprint.Footprint.Depth
      };
    } else {
      DecomposeSubresource(src_desc, pSrc->SubresourceIndex, &src_level, &src_slice, &src_planar);
      full_src_box = GetResourceExtent(src_desc, src_level);
      auto texture = static_cast<MTLD3D12Resource *>(pSrc->pResource)->texture.ptr();
      if (texture && texture->chroma) {
        full_src_box.right = texture->plane(src_planar)->width();
        full_src_box.bottom = texture->plane(src_planar)->height();
      }
    }
    src_box = pSrcBox ? *pSrcBox : full_src_box;

    // discard invalid & empty box
    if (src_box.left >= src_box.right || src_box.front >= src_box.back || src_box.top >= src_box.bottom)
      return;

    if (pDst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
      DecomposeSubresource(dst_desc, pDst->SubresourceIndex, &dst_level, &dst_slice, &dst_planar);
      auto resource = static_cast<MTLD3D12Resource *>(pDst->pResource);
      if (!resource->texture)
        return;
      auto dst = resource->texture->plane(dst_planar);
      auto dst_extent = GetResourceExtent(dst_desc, dst_level);
      if (resource->texture->chroma) {
        dst_extent.right = dst->width();
        dst_extent.bottom = dst->height();
      }

      MTL_DXGI_FORMAT_DESC dst_format;
      if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), dst_desc.Format, dst_format, dst_planar))) {
        WARN("CopyTextureRegion: unsupported format ", dst_desc.Format);
        return;
      }
      auto dst_planar_count = dst_format.PlanarCount;

      if (pSrc->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
        auto &src = static_cast<MTLD3D12Resource *>(pSrc->pResource)->buffer;
        if (!src)
          return;

        MTL_DXGI_FORMAT_DESC src_format;
        if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), pSrc->PlacedFootprint.Footprint.Format, src_format))) {
          WARN("CopyTextureRegion: unsupported format ", pSrc->PlacedFootprint.Footprint.Format);
          return;
        }

        const uint32_t block_width = src_format.Flag & MTL_DXGI_FORMAT_BC ? 4 : 1,
                       dst_block_width = dst_format.Flag & MTL_DXGI_FORMAT_BC ? 4 : 1;
        auto src_depth_pitch = dst->textureType() == WMTTextureType3D
                                   ? (pSrc->PlacedFootprint.Footprint.Height / block_width) * pSrc->PlacedFootprint.Footprint.RowPitch
                                   : 0;

        auto upload = src->current()->buffer();
        const uint64_t src_offset = pSrc->PlacedFootprint.Offset +
                                    (src_box.left / block_width) * src_format.BytesPerTexel +
                                    (src_box.top / block_width) * pSrc->PlacedFootprint.Footprint.RowPitch +
                                    src_box.front * src_depth_pitch;
        uint64_t upload_offset = src_offset;
        uint64_t row_pitch = pSrc->PlacedFootprint.Footprint.RowPitch, image_pitch = src_depth_pitch;
        if (dst_format.Flag & MTL_DXGI_FORMAT_EMULATED_RGB32) {
          WMTSize size{src_box.right - src_box.left, src_box.bottom - src_box.top, src_box.back - src_box.front};
          row_pitch = size.width * MTLGetTexelSize(dst->pixelFormat());
          image_pitch = row_pitch * size.height;
          std::tie(upload, upload_offset) = allocator_->AllocateTempBuffer(image_pitch * size.depth, 256);
          allocator_->rgb32_copy_.run(
              true, src->current()->buffer(), src_offset, upload, upload_offset,
              {pSrc->PlacedFootprint.Footprint.RowPitch, src_depth_pitch, row_pitch, image_pitch}, size
          );
        }
        // a D24 footprint stores UNORM24 codes (D3D11.3 19.1.3.1); Metal stores them as float32.
        if (!dst_planar && (dst_format.Flag & MTL_DXGI_FORMAT_EMULATED_D24)) {
          WMTSize size{src_box.right - src_box.left, src_box.bottom - src_box.top, 1};
          row_pitch = align(size.width * sizeof(float), D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
          std::tie(upload, upload_offset) =
              allocator_->AllocateTempBuffer(row_pitch * size.height, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
          if (!allocator_->CopyD24(true, src->current()->buffer(), src_offset,
                                   pSrc->PlacedFootprint.Footprint.RowPitch, upload, upload_offset, row_pitch, size))
            return;
        }
        auto &cmd_cp = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture_withblitoption>();
        cmd_cp.type = WMTBlitCommandCopyFromBufferToTextureWithBlitOption;
        cmd_cp.src = upload;
        cmd_cp.src_offset = upload_offset;
        cmd_cp.bytes_per_row = row_pitch;
        cmd_cp.bytes_per_image = image_pitch;
        // D3D10.1 reinterpret copies carry one source unit into one destination unit (CopyResource, Remarks)
        cmd_cp.size = {
            std::min<uint64_t>(align(src_box.right - src_box.left, block_width) / block_width * dst_block_width,
                               dst_extent.right - DstX),
            std::min<uint64_t>(align(src_box.bottom - src_box.top, block_width) / block_width * dst_block_width,
                               dst_extent.bottom - DstY),
            src_box.back - src_box.front};
        cmd_cp.dst = dst->current()->texture();
        cmd_cp.level = dst_level;
        cmd_cp.slice = dst_slice;
        cmd_cp.options = (dst_planar_count > 1)
                             ? (dst_planar ? WMTBlitOptionStencilFromDepthStencil : WMTBlitOptionDepthFromDepthStencil)
                             : WMTBlitOptionNone;
        cmd_cp.origin = {DstX, DstY, DstZ};
      } else {
        auto resource = static_cast<MTLD3D12Resource *>(pSrc->pResource);
        if (!resource->texture)
          return;
        auto src = resource->texture->plane(src_planar);

        MTL_DXGI_FORMAT_DESC src_format;
        if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), src_desc.Format, src_format, src_planar))) {
          WARN("CopyTextureRegion: unsupported format ", src_desc.Format);
          return;
        }
        auto src_planar_count = src_format.PlanarCount;

        // copy between depth-stencil texture is tricky
        if (dst_planar_count > 1 || src_planar_count > 1) {
          // in this path, one/both of dst/src would be depth-stencil texture

          if (dst_planar_count > 1 && src_planar_count > 1 && dst_planar != src_planar) {
            WARN("CopyTextureRegion: unmatched planar"); // just in case
            return;
          }

          auto texel_size = (dst_planar == 1 || src_planar == 1) ? 1 : 4;
          auto width = src_box.right - src_box.left;
          auto height = src_box.bottom - src_box.top;
          auto bytes_per_row = align(width * texel_size, 256);
          auto bytes_per_image = bytes_per_row * height;

          auto [temp_buffer, temp_buffer_offset] = allocator_->AllocateTempBuffer(bytes_per_image, 256);

          auto &cmd_to_tmp = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer_withblitoption>();
          cmd_to_tmp.type = WMTBlitCommandCopyFromTextureToBufferWithBlitOption;
          cmd_to_tmp.src = src->current()->texture();
          cmd_to_tmp.level = src_level;
          cmd_to_tmp.slice = src_slice;
          cmd_to_tmp.origin = {src_box.left, src_box.top, src_box.front};
          cmd_to_tmp.size = {width, height, src_box.back - src_box.front};
          cmd_to_tmp.dst = temp_buffer;
          cmd_to_tmp.offset = temp_buffer_offset;
          cmd_to_tmp.bytes_per_image = 0; // DSV cannot be 3D
          cmd_to_tmp.bytes_per_row = bytes_per_row;
          cmd_to_tmp.options = (src_planar_count > 1) ? (src_planar ? WMTBlitOptionStencilFromDepthStencil
                                                                    : WMTBlitOptionDepthFromDepthStencil)
                                                      : WMTBlitOptionNone;

          auto &cmd_to_tex = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture_withblitoption>();
          cmd_to_tex.type = WMTBlitCommandCopyFromBufferToTextureWithBlitOption;
          cmd_to_tex.src = temp_buffer;
          cmd_to_tex.src_offset = temp_buffer_offset;
          cmd_to_tex.bytes_per_image = 0; // DSV cannot be 3D
          cmd_to_tex.bytes_per_row = bytes_per_row;
          cmd_to_tex.dst = dst->current()->texture();
          cmd_to_tex.level = dst_level;
          cmd_to_tex.slice = dst_slice;
          cmd_to_tex.origin = {DstX, DstY, DstZ};
          cmd_to_tex.size = {src_box.right - src_box.left, src_box.bottom - src_box.top, src_box.back - src_box.front};
          cmd_to_tex.options = (dst_planar_count > 1) ? (dst_planar ? WMTBlitOptionStencilFromDepthStencil
                                                                    : WMTBlitOptionDepthFromDepthStencil)
                                                      : WMTBlitOptionNone;
          return;
        }

        CopyTexels(
            src, src_format, src_level, src_slice, {src_box.left, src_box.top, src_box.front},
            {src_box.right - src_box.left, src_box.bottom - src_box.top, src_box.back - src_box.front}, dst,
            dst_format, dst_level, dst_slice, {DstX, DstY, DstZ}, dst_extent
        );
      }
    } else if (pDst->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
      auto &dst = static_cast<MTLD3D12Resource *>(pDst->pResource)->buffer;
      if (!dst)
        return;

      MTL_DXGI_FORMAT_DESC dst_format;
      if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), pDst->PlacedFootprint.Footprint.Format, dst_format))) {
        WARN("CopyTextureRegion: unsupported format ", pDst->PlacedFootprint.Footprint.Format);
        return;
      }

      if (pSrc->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
        auto resource = static_cast<MTLD3D12Resource *>(pSrc->pResource);
        if (!resource->texture)
          return;
        auto src = resource->texture->plane(src_planar);

        MTL_DXGI_FORMAT_DESC src_format;
        if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), src_desc.Format, src_format, src_planar))) {
          WARN("CopyTextureRegion: unsupported format ", src_desc.Format);
          return;
        }
        auto src_planar_count = src_format.PlanarCount;

        auto block_width = dst_format.Flag & MTL_DXGI_FORMAT_BC ? 4 : 1;
        auto dst_depth_pitch = src->textureType() == WMTTextureType3D
                                   ? (pDst->PlacedFootprint.Footprint.Height / block_width) * pDst->PlacedFootprint.Footprint.RowPitch
                                   : 0;

        auto download = dst->current()->buffer();
        const uint64_t dst_offset = pDst->PlacedFootprint.Offset + (DstX / block_width) * dst_format.BytesPerTexel +
                                    (DstY / block_width) * pDst->PlacedFootprint.Footprint.RowPitch +
                                    DstZ * dst_depth_pitch;
        uint64_t download_offset = dst_offset, row_pitch = pDst->PlacedFootprint.Footprint.RowPitch,
                 image_pitch = dst_depth_pitch;
        WMTSize size{src_box.right - src_box.left, src_box.bottom - src_box.top, src_box.back - src_box.front};
        bool rgb32 = src_format.Flag & MTL_DXGI_FORMAT_EMULATED_RGB32;
        if (rgb32) {
          row_pitch = size.width * MTLGetTexelSize(src->pixelFormat());
          image_pitch = row_pitch * size.height;
          std::tie(download, download_offset) = allocator_->AllocateTempBuffer(image_pitch * size.depth, 256);
        }
        auto &cmd_cp = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer_withblitoption>();
        cmd_cp.type = WMTBlitCommandCopyFromTextureToBufferWithBlitOption;
        cmd_cp.src = src->current()->texture();
        cmd_cp.level = src_level;
        cmd_cp.slice = src_slice;
        cmd_cp.origin = {src_box.left, src_box.top, src_box.front};
        cmd_cp.size = {src_box.right - src_box.left, src_box.bottom - src_box.top, src_box.back - src_box.front};
        cmd_cp.dst = download;
        cmd_cp.offset = download_offset;
        cmd_cp.bytes_per_row = row_pitch;
        cmd_cp.bytes_per_image = image_pitch;
        cmd_cp.options = (src_planar_count > 1)
                             ? (src_planar ? WMTBlitOptionStencilFromDepthStencil : WMTBlitOptionDepthFromDepthStencil)
                             : WMTBlitOptionNone;
        if (rgb32)
          allocator_->rgb32_copy_.run(
              false, download, download_offset, dst->current()->buffer(), dst_offset,
              {row_pitch, image_pitch, pDst->PlacedFootprint.Footprint.RowPitch, dst_depth_pitch}, size
          );
        if (!src_planar && (src_format.Flag & MTL_DXGI_FORMAT_EMULATED_D24))
          allocator_->CopyD24(false, download, download_offset, row_pitch, download, download_offset, row_pitch, size);
      } else {
        // between buffers there is CopyBufferRegion: one end of this copy is a texture
        ERR("CopyTextureRegion: both the source and the destination are footprints in buffers");
      }
    }
  };

  void STDMETHODCALLTYPE
  CopyResource(ID3D12Resource *pDstResource, ID3D12Resource *pSrcResource) {
    PredicatedPasses predicated(this);
    auto *pDst = static_cast<MTLD3D12Resource *>(pDstResource);
    auto *pSrc = static_cast<MTLD3D12Resource *>(pSrcResource);
    if (!pDst || !pSrc || (pDst == pSrc))
      return;

    auto DstDesc = pDst->GetDesc();
    auto SrcDesc = pSrc->GetDesc();
    if (DstDesc.Dimension != SrcDesc.Dimension)
      return;

    // a texture of two planes copies itself through CopyTextureRegion
    Hoisted hoisted{this};
    if (DstDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ||
        !(pSrc->texture->chroma || pDst->texture->chroma)) {
      if (!PreCopy(hoisted, pDstResource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, pSrcResource,
                   D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES))
        return;
    } else if (!PreBlit())
      return;

    if (DstDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
      auto &cmd_cp = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
      cmd_cp.type = WMTBlitCommandCopyFromBufferToBuffer;
      cmd_cp.copy_length = SrcDesc.Width;
      cmd_cp.src = pSrc->buffer->current()->buffer();
      cmd_cp.src_offset = 0;
      cmd_cp.dst = pDst->buffer->current()->buffer();
      cmd_cp.dst_offset = 0;
      return;
    }

    if (pSrc->texture->chroma || pDst->texture->chroma) {
      MTL_DXGI_FORMAT_DESC format;
      MTLQueryDXGIFormat(device_->GetMTLDevice(), SrcDesc.Format, format);
      UINT subresources = SrcDesc.MipLevels * SrcDesc.DepthOrArraySize * format.PlanarCount;
      for (UINT sub = 0; sub < subresources; sub++) {
        D3D12_TEXTURE_COPY_LOCATION src{
            pSrcResource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = sub}},
            dst{pDstResource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = sub}};
        CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      }
      return;
    }

    // textures of two formats: every subresource, as a copy of each is
    MTL_DXGI_FORMAT_DESC src_format, dst_format;
    if (pDst->texture->pixelFormat() != pSrc->texture->pixelFormat() &&
        SUCCEEDED(MTLQueryDXGIFormat(device_->GetMTLDevice(), SrcDesc.Format, src_format)) &&
        SUCCEEDED(MTLQueryDXGIFormat(device_->GetMTLDevice(), DstDesc.Format, dst_format))) {
      UINT slices = SrcDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : SrcDesc.DepthOrArraySize;
      for (UINT slice = 0; slice < slices; slice++)
        for (UINT level = 0; level < SrcDesc.MipLevels; level++) {
          auto extent = GetResourceExtent(SrcDesc, level);
          CopyTexels(
              pSrc->texture.ptr(), src_format, level, slice, {}, {extent.right, extent.bottom, extent.back},
              pDst->texture.ptr(), dst_format, level, slice, {}, GetResourceExtent(DstDesc, level)
          );
        }
      return;
    }

    auto &cmd_cp = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_texture>();
    cmd_cp.type = WMTBlitCommandCopyTexture;
    cmd_cp.src = pSrc->texture->current()->texture();
    cmd_cp.dst = pDst->texture->current()->texture();
  };

  void STDMETHODCALLTYPE CopyTiles(
      ID3D12Resource *tiled_resource, const D3D12_TILED_RESOURCE_COORDINATE *tile_region_start_coordinate,
      const D3D12_TILE_REGION_SIZE *tile_region_size, ID3D12Resource *buffer, UINT64 buffer_offset,
      D3D12_TILE_COPY_FLAGS flags
  ) {
    PredicatedPasses predicated(this);
    if (!PreBlit())
      return;
    // the buffer holds each tile as 64KB of rows a full tile wide, in the region's order; packed tiles are undefined
    Tiling tiling(tiled_resource);
    auto res = static_cast<MTLD3D12Resource *>(tiled_resource);
    auto desc = res->GetDesc();
    auto linear = static_cast<MTLD3D12Resource *>(buffer)->buffer->current()->buffer();
    bool in = flags & D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE;
    const UINT64 tile = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
    MTL_DXGI_FORMAT_DESC format{};
    UINT block = tiling.texture && SUCCEEDED(MTLQueryDXGIFormat(device_->GetMTLDevice(), desc.Format, format)) &&
                         (format.Flag & MTL_DXGI_FORMAT_BC)
                     ? 4
                     : 1;
    auto &shape = tiling.shape;
    for (UINT n = 0; n < tile_region_size->NumTiles; n++) {
      auto at = buffer_offset + n * tile;
      auto place = tiling.place(tiling.index(*tile_region_start_coordinate, *tile_region_size, n));
      if (!tiling.texture) {
        auto &cmd = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
        auto tiled = res->buffer->current()->buffer();
        auto from = UINT64(place.origin.x) * tile;
        cmd.type = WMTBlitCommandCopyFromBufferToBuffer;
        cmd.src = in ? linear : tiled;
        cmd.src_offset = in ? at : from;
        cmd.dst = in ? tiled : linear;
        cmd.dst_offset = in ? from : at;
        cmd.copy_length = std::min(tile, desc.Width - from);
        continue;
      }
      if (place.level >= tiling.packed.NumStandardMips)
        continue;
      auto &texture = res->texture;
      auto mip = [&](UINT size) { return std::max(size >> place.level, 1u); };
      WMTOrigin origin{place.origin.x * shape.WidthInTexels, place.origin.y * shape.HeightInTexels, place.origin.z * shape.DepthInTexels};
      WMTSize extent{
          std::min<uint64_t>(shape.WidthInTexels, mip(texture->width()) - origin.x),
          std::min<uint64_t>(shape.HeightInTexels, mip(texture->height()) - origin.y),
          std::min<uint64_t>(shape.DepthInTexels, mip(texture->depth()) - origin.z)
      };
      uint32_t image = tile / shape.DepthInTexels, row = image / (shape.HeightInTexels / block);
      if (in) {
        auto &cmd = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture>();
        cmd.type = WMTBlitCommandCopyFromBufferToTexture;
        cmd.src = linear;
        cmd.src_offset = at;
        cmd.bytes_per_row = row;
        cmd.bytes_per_image = image;
        cmd.size = extent;
        cmd.dst = texture->current()->texture();
        cmd.slice = place.slice;
        cmd.level = place.level;
        cmd.origin = origin;
      } else {
        auto &cmd = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer>();
        cmd.type = WMTBlitCommandCopyFromTextureToBuffer;
        cmd.src = texture->current()->texture();
        cmd.slice = place.slice;
        cmd.level = place.level;
        cmd.origin = origin;
        cmd.size = extent;
        cmd.dst = linear;
        cmd.offset = at;
        cmd.bytes_per_row = row;
        cmd.bytes_per_image = image;
      }
    }
  };

  void STDMETHODCALLTYPE ResolveSubresource(
      ID3D12Resource *pDstResource, UINT DstSubresource, ID3D12Resource *pSrcResource, UINT SrcSubresource,
      DXGI_FORMAT Format
  ) {
    ResolveSubresourceRegion(
        pDstResource, DstSubresource, 0, 0, pSrcResource, SrcSubresource, nullptr, Format, D3D12_RESOLVE_MODE_AVERAGE
    );
  };

  void STDMETHODCALLTYPE
  IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY Topology) {
    if (Bundled() && Record([=](auto *l) { l->IASetPrimitiveTopology(Topology); }))
      return;
    // a geometry pipeline has one Metal pipeline for strips and one for lists. a pipeline without a geometry shader
    // becomes one for a topology with adjacency, and binds its arguments to other stages
    if (pso_graphics_ && (pso_graphics_->geometry_shader || pso_graphics_->adjacency) && Strip(Topology) != Strip(topology_))
      dirty_state_.set(DirtyState::GraphicsPipelineState);
    if (pso_graphics_ && pso_graphics_->patches && Topology != topology_)
      dirty_state_.set(DirtyState::GraphicsPipelineState);
    if (pso_graphics_ && pso_graphics_->adjacency && Adjacency(Topology) != Adjacency(topology_))
      dirty_state_.set(
          DirtyState::GraphicsPipelineState, DirtyState::GraphicsRootArguments, DirtyState::GraphicsRootSignature,
          DirtyState::VertexBuffer
      );
    topology_ = Topology;
  };

  void STDMETHODCALLTYPE
  RSSetViewports(UINT NumViewports, const D3D12_VIEWPORT *pViewports) {
    if (NumViewports > D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE)
      return;
    if (num_viewports == NumViewports &&
        (!NumViewports || memcmp(viewports, pViewports, NumViewports * sizeof(D3D12_VIEWPORT)) == 0))
      return;
    num_viewports = NumViewports;
    for (auto i = 0u; i < NumViewports; i++) {
      viewports[i] = pViewports[i];
    }
    // there is a scissor for each viewport
    dirty_state_.set(DirtyState::Viewport, DirtyState::ScissorRect);
  };

  void STDMETHODCALLTYPE
  RSSetScissorRects(UINT NumRects, const D3D12_RECT *rects) {
    if (NumRects > D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE)
      return;
    if (num_scissors == NumRects && (!NumRects || memcmp(scissors, rects, NumRects * sizeof(D3D12_RECT)) == 0))
      return;
    num_scissors = NumRects;
    for (auto i = 0u; i < NumRects; i++) {
      scissors[i] = rects[i];
    }
    dirty_state_.set(DirtyState::ScissorRect);
  };

  void STDMETHODCALLTYPE
  OMSetBlendFactor(const FLOAT BlendFactors[4]) {
    // NULL is a factor of ones
    decltype(blend_factor_) f;
    BlendFactors ? (void)std::copy_n(BlendFactors, std::size(f), f) : (void)std::fill_n(f, std::size(f), 1.0f);
    if (Bundled() && Record([=](auto *l) { l->OMSetBlendFactor(f); }))
      return;
    if (memcmp(blend_factor_, f, sizeof(f)) == 0)
      return;
    std::copy_n(f, std::size(f), blend_factor_);
    dirty_state_.set(DirtyState::BlendFactor);
  };

  void STDMETHODCALLTYPE
  OMSetStencilRef(UINT StencilRef) {
    OMSetFrontAndBackStencilRef(StencilRef, StencilRef);
  };

  void STDMETHODCALLTYPE
  OMSetFrontAndBackStencilRef(UINT FrontStencilRef, UINT BackStencilRef) {
    if (Bundled() && Record([=](auto *l) { l->OMSetFrontAndBackStencilRef(FrontStencilRef, BackStencilRef); }))
      return;
    if (stencil_ref_ == (UINT8)FrontStencilRef && back_stencil_ref_ == (UINT8)BackStencilRef)
      return;
    stencil_ref_ = (UINT8)FrontStencilRef;
    back_stencil_ref_ = (UINT8)BackStencilRef;
    dirty_state_.set(DirtyState::StencilRef);
  }

  void STDMETHODCALLTYPE
  RSSetDepthBias(FLOAT DepthBias, FLOAT DepthBiasClamp, FLOAT SlopeScaledDepthBias) {
    if (Bundled() && Record([=](auto *l) { l->RSSetDepthBias(DepthBias, DepthBiasClamp, SlopeScaledDepthBias); }))
      return;
    std::array<FLOAT, 3> next{DepthBias, DepthBiasClamp, SlopeScaledDepthBias};
    if (depth_bias_ && memcmp(depth_bias_->data(), next.data(), sizeof(next)) == 0)
      return;
    depth_bias_ = next;
    dirty_state_.set(DirtyState::GraphicsPipelineState);
  }

  void STDMETHODCALLTYPE
  IASetIndexBufferStripCutValue(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE IBStripCutValue) {
    // geometry shader pipelines build the cut in, so OPTIONS15 reports no DynamicIndexBufferStripCutSupported
    ERR("IASetIndexBufferStripCutValue: dynamic strip cut is not supported");
    recording_error_ = E_INVALIDARG;
  }

  void STDMETHODCALLTYPE
  SetPipelineState(ID3D12PipelineState *pPSO) {
    if (Bundled() && Record([pso = Com<ID3D12PipelineState>(pPSO)](auto *l) { l->SetPipelineState(pso.ptr()); }))
      return;
    if (!pPSO) {
      pso_graphics_ = nullptr;
      pso_compute_ = nullptr;
      dirty_state_.set(DirtyState::GraphicsPipelineState, DirtyState::ComputePipelineState);
      return;
    }

    auto pso = static_cast<MTLD3D12PipelineState *>(pPSO);
    if (pso->IsComputePipelineState) {
      auto compute_pso = static_cast<MTLD3D12ComputePipelineState *>(pPSO);
      if (pso_compute_.ptr() == compute_pso)
        return;
      pso_compute_ = compute_pso;
      pso_graphics_ = nullptr;
      dirty_state_.set(DirtyState::GraphicsPipelineState, DirtyState::ComputePipelineState);
      return;
    }

    auto graphics_pso = static_cast<MTLD3D12GraphicsPipelineState *>(pPSO);
    if (pso_graphics_.ptr() == graphics_pso)
      return;
    // another pipeline takes RSSetDepthBias's values back; the one that is set keeps them, as on Windows
    // (vkd3d-proton's test_dynamic_depth_bias, which has the specification take them back there too)
    depth_bias_.reset();
    // tessellation, geometry shaders and stream output bind root arguments to other stages
    // and a pipeline whose mesh stage reads them, or reads the vertex buffers (mesh_only), to others again
    auto stages = [&](MTLD3D12GraphicsPipelineState *p) {
      bool adjacency = p->adjacency && Adjacency(topology_);
      return (p->threads_per_patch || p->geometry_shader || p->mesh_shader || adjacency) |
             p->mesh_reads_root_arguments << 1 | p->mesh_only << 2;
    };
    if (!pso_graphics_ || stages(pso_graphics_.ptr()) != stages(graphics_pso))
      dirty_state_.set(DirtyState::GraphicsRootArguments, DirtyState::GraphicsRootSignature, DirtyState::VertexBuffer);
    // the vertex buffer table has an entry for each slot the pipeline reads, and is bound to the stages above
    if (!pso_graphics_ || pso_graphics_->slot_mask != graphics_pso->slot_mask)
      dirty_state_.set(DirtyState::VertexBuffer);
    // a pass has the targets its first pipeline writes (PreDraw). a pipeline that writes no other target draws in it
    // and leaves the rest of them alone (RenderTargets); one that writes a target the pass lacks starts the next
    bool restart = !pso_graphics_ || pso_graphics_->forced_sample_count != graphics_pso->forced_sample_count;
    if (auto pass = static_cast<RenderEncoderData *>(allocator_->encoder_current);
        !restart && pass && pass->type == EncoderType::Render) {
      for (unsigned i = 0; i < num_rtvs; i++)
        restart |= (graphics_pso->rtv_mask & (1u << i)) && rtvs[i].Texture && !pass->colors[i].attachment;
      restart |= dsv.Texture && (graphics_pso->dsv_mask & DepthStencilPlanarFlags(dsv.Texture->pixelFormat(dsv.View)) &
                                 ~pass->dsv_planar_flags);
    }
    if (restart)
      allocator_->InvalidateCurrentPass();
    if (graphics_pso->shader_blend)
      dirty_state_.set(DirtyState::BlendFactor);
    pso_graphics_ = graphics_pso;
    pso_compute_ = nullptr;
    dirty_state_.set(DirtyState::GraphicsPipelineState, DirtyState::ComputePipelineState);
    dirty_state_.set(DirtyState::DepthBounds);
  };

  // a barrier on `resource`, or on any (null): a clear of it before can no longer fold into a pass after (PendingClear)
  void
  BarrierNames(ID3D12Resource *resource) {
    if (!resource) {
      clears_.clear();
      return;
    }
    if (auto texture = static_cast<MTLD3D12Resource *>(resource)->texture.ptr())
      UsedTexture(texture);
  }

  // whether the open render pass's draws may have written `resource` (null: any resource): it is one of the pass's
  // targets, or they wrote through UAVs or stream output. a buffer is never a target
  bool
  PassWrote(ID3D12Resource *resource) {
    auto pass = allocator_->encoder_current;
    if (!resource || !pass || pass->type != EncoderType::Render || pass == uav_draw_pass_ || pass != targets_pass_)
      return true;
    auto texture = static_cast<MTLD3D12Resource *>(resource)->texture.ptr();
    return texture && std::ranges::find(targets_, texture) != targets_.end();
  }

  // an aliasing barrier in a render pass whose draws wrote nothing but its targets, naming none of them, keeps the pass
  // going: a resource it activates is none of the targets, which the draws after it go on writing, and the memory it
  // takes was written by work before the pass (OrderingPoint orders the open pass after it) or is a target's, stored
  // when the pass ends. one that names a target, or follows a draw's UAV or stream output writes, ends the pass
  bool
  KeptByAliasing(const D3D12_RESOURCE_BARRIER &barrier) {
    auto pass = allocator_->encoder_current;
    if (barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_ALIASING || !pass || pass->type != EncoderType::Render ||
        pass != targets_pass_ || pass == uav_draw_pass_ || !KeepsAliasedPasses())
      return false;
    return !PassWrote(barrier.Aliasing.pResourceBefore) && !PassWrote(barrier.Aliasing.pResourceAfter);
  }

  static bool
  KeepsAliasedPasses() {
    static const bool keeps = env::getEnvVar("MULLION_D3D12_ALIAS_KEEPS_PASS") != "0";
    return keeps;
  }

  // passes are already ordered by the queue's fence (each waits for the one before), so a barrier matters only inside
  // a pass, where draws and dispatches run unordered. one that orders writes a draw made (UAV, render target, depth,
  // stream output) or aliases memory ends the render pass, and the next draw opens one that loads what this one
  // stored. a dispatch writes through UAVs alone and keeps its pass: `dispatched` is a barrier on one, before or after.
  // true when no later barrier of the call can order more; their resources still count (BarrierLeaves, BarrierNames)
  bool
  Order(bool drew, bool dispatched) {
    auto type = allocator_->encoder_current ? allocator_->encoder_current->type : EncoderType::Null;
    if (type == EncoderType::Render && drew)
      allocator_->InvalidateCurrentPass();
    else if (type == EncoderType::Compute && dispatched)
      allocator_->OrderDispatches();
    else
      return type != EncoderType::Render && type != EncoderType::Compute;
    return true;
  }

  void STDMETHODCALLTYPE ResourceBarrier(UINT Count, const D3D12_RESOURCE_BARRIER *barriers) {
    WriteMarkers();
    constexpr auto pixels = D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_DEPTH_WRITE |
                            D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    // the first half of a split barrier orders nothing: the resource is not used until the second half, which orders
    // the work before the first after it ("Split Barriers", D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY)
    auto begins = [](const D3D12_RESOURCE_BARRIER &barrier) {
      return barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION &&
             (barrier.Flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY);
    };
    constexpr auto write_states = D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
                                  D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_STREAM_OUT |
                                  D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_RESOLVE_DEST |
                                  D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
    auto read_to_read = [&](const D3D12_RESOURCE_BARRIER &barrier) {
      return barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION &&
             barrier.Transition.StateBefore != D3D12_RESOURCE_STATE_COMMON &&
             !(barrier.Transition.StateBefore & write_states) && !(barrier.Transition.StateAfter & write_states);
    };
    auto no_order = [&](const D3D12_RESOURCE_BARRIER &barrier) { return begins(barrier) || read_to_read(barrier); };
    if (!std::ranges::all_of(std::span(barriers, Count), no_order))
      OrderingPoint(std::ranges::any_of(std::span(barriers, Count), [&](auto &barrier) {
        auto after = barrier.Transition.StateAfter;
        return !no_order(barrier) &&
               (barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !after || (after & ~pixels));
      }));
    constexpr auto uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS, draw_writes = uav | D3D12_RESOURCE_STATE_RENDER_TARGET |
                       D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_STREAM_OUT;
    // Metal orders passes that reach one memory through a reserved buffer and through another resource only with
    // what the queue encodes for an aliasing barrier between them (EncoderType::Settle; d3d12_sparse_remap)
    if (std::ranges::any_of(std::span(barriers, Count), [](auto &barrier) {
          return barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
        }) && device_->HasSparseBuffers()) {
      va_found_ = nullptr;
      // in a pass that keeps going, the work before it and its draws before the barrier are ordered before its draws
      // after it, and what it stores before the work after it: one Settle before the pass and one after it, however
      // many barriers it has (Starfield's 300 a frame were as many passes, each loading and storing its targets)
      if (std::ranges::all_of(std::span(barriers, Count), [&](auto &barrier) { return KeptByAliasing(barrier); })) {
        clears_.clear();
        named_.clear();
        allocator_->encoder_current->late = false;
        allocator_->SettleAround();
        return;
      }
      clears_.clear();
      named_.clear();
      allocator_->InvalidateCurrentPass();
      allocator_->AllocatePass<EncoderData>()->type = EncoderType::Settle;
      allocator_->InvalidateCurrentPass();
      return;
    }
    barrier_serial_++;
    bool ordered = false;
    for (auto &barrier : std::span(barriers, Count)) {
      if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING)
        clear_joinable_ = false, va_found_ = nullptr;
      if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
        BarrierLeaves(barrier.Transition.pResource, barrier.Transition.Subresource,
                      barrier.Flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY ? D3D12_RESOURCE_STATES(0)
                                                                            : barrier.Transition.StateAfter);
      else
        BarrierLeaves(barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV ? barrier.UAV.pResource : nullptr,
                      D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATES(0));
      if (!clears_.empty())
        BarrierNames(barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ? barrier.Transition.pResource
                     : barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV      ? barrier.UAV.pResource
                                                                            : nullptr);
      if (ordered || begins(barrier))
        continue;
      if (KeptByAliasing(barrier)) {
        allocator_->encoder_current->late = false;
        continue;
      }
      // a UAV barrier orders UAV accesses alone: a render pass whose draws used none keeps going, as a dispatch's would
      if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV && allocator_->encoder_current &&
          allocator_->encoder_current->type == EncoderType::Render &&
          allocator_->encoder_current != uav_draw_pass_) {
        ordered = Order(false, true);
        continue;
      }
      bool other = barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      auto &states = barrier.Transition;
      ordered = Order(other || ((states.StateBefore & draw_writes) && PassWrote(states.pResource)),
                      other || ((states.StateBefore | states.StateAfter) & uav));
    }
  };

  void STDMETHODCALLTYPE
  ExecuteBundle(ID3D12GraphicsCommandList *CommandList) {
    for (auto &call : static_cast<MTLD3D12GraphicsCommandListImpl *>(CommandList)->bundle_)
      call(this);
  };

  void STDMETHODCALLTYPE SetDescriptorHeaps(UINT HeapCount, ID3D12DescriptorHeap *const *Heaps) {
    if (Bundled() && Record([heaps = std::vector<Com<ID3D12DescriptorHeap>>(Heaps, Heaps + HeapCount)](auto *l) { l->SetDescriptorHeaps(heaps.size(), reinterpret_cast<ID3D12DescriptorHeap *const *>(heaps.data())); }))
      return;
    // tables carry their full address in their root argument; only directly indexed heaps need the heaps
    for (unsigned i = 0; i < HeapCount; i++) {
      D3D12_DESCRIPTOR_HEAP_DESC desc;
      D3D12_GPU_DESCRIPTOR_HANDLE start;
      Heaps[i]->GetDesc(&desc);
      Heaps[i]->GetGPUDescriptorHandleForHeapStart(&start);
      (desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? sampler_heap_ : resource_heap_) = start.ptr;
    }
    WriteHeapArguments(rootsig_graphics_.ptr(), rootarg_graphics_staging_, DirtyState::GraphicsRootArguments);
    WriteHeapArguments(rootsig_compute_.ptr(), rootarg_compute_staging_, DirtyState::ComputeRootArguments);
  };

  void STDMETHODCALLTYPE
  SetComputeRootSignature(ID3D12RootSignature *pRootSignature) {
    if (Bundled() && Record([rs = Com<ID3D12RootSignature>(pRootSignature)](auto *l) { l->SetComputeRootSignature(rs.ptr()); }))
      return;
    if (rootsig_compute_.ptr() == pRootSignature)
      return;
    if (pRootSignature) {
      rootsig_compute_ = static_cast<MTLD3D12RootSignature *>(pRootSignature);
      assert(rootsig_compute_->UploadQwords <= std::size(rootarg_compute_staging_));
      WriteHeapArguments(rootsig_compute_.ptr(), rootarg_compute_staging_, DirtyState::ComputeRootArguments);
      NullRootViews(rootsig_compute_.ptr(), rootarg_compute_staging_, DirtyState::ComputeRootArguments);
    } else {
      rootsig_compute_ = nullptr;
    }
    // another root signature uploads another number of arguments
    RootArgumentsChanged(DirtyState::ComputeRootArguments);
    dirty_state_.set(DirtyState::ComputeRootSignature);
  };

  void STDMETHODCALLTYPE
  SetGraphicsRootSignature(ID3D12RootSignature *pRootSignature) {
    if (Bundled() && Record([rs = Com<ID3D12RootSignature>(pRootSignature)](auto *l) { l->SetGraphicsRootSignature(rs.ptr()); }))
      return;
    if (rootsig_graphics_.ptr() == pRootSignature)
      return;
    if (pRootSignature) {
      rootsig_graphics_ = static_cast<MTLD3D12RootSignature *>(pRootSignature);
      assert(rootsig_graphics_->UploadQwords <= std::size(rootarg_graphics_staging_));
      WriteHeapArguments(rootsig_graphics_.ptr(), rootarg_graphics_staging_, DirtyState::GraphicsRootArguments);
      NullRootViews(rootsig_graphics_.ptr(), rootarg_graphics_staging_, DirtyState::GraphicsRootArguments);
    } else {
      rootsig_graphics_ = nullptr;
    }
    // another root signature uploads another number of arguments
    RootArgumentsChanged(DirtyState::GraphicsRootArguments);
    dirty_state_.set(DirtyState::GraphicsRootSignature);
  };

  // a root argument that changes dirties the arguments, which the next draw or dispatch uploads again; one set to what
  // it is already leaves the uploaded copy bound
  void
  SetRootQword(MTLD3D12RootSignature *rootsig, uint64_t *staging, DirtyState dirty, UINT Index, uint64_t Value) {
    if (!rootsig || Index >= rootsig->ParameterSlots)
      return;
    auto &slot = staging[rootsig->SlotQwordOffsets[Index]];
    if (slot == Value)
      return;
    slot = Value;
    RootArgumentsChanged(dirty);
  }

  void
  SetRootConstants(
      MTLD3D12RootSignature *rootsig, uint64_t *staging, DirtyState dirty, UINT Index, UINT Count, const void *Data,
      UINT DstOffset
  ) {
    if (!rootsig || Index >= rootsig->ParameterSlots)
      return;
    // values past the parameter's own are another parameter's arguments, or past them all: Direct3D does not define
    // setting them, and they are left as they are, as is a parameter that is not constants
    auto values = rootsig->SlotConstants[Index];
    if (DstOffset >= values)
      return;
    Count = std::min(Count, values - DstOffset);
    auto dst = reinterpret_cast<uint32_t *>(staging + rootsig->SlotQwordOffsets[Index]) + DstOffset;
    if (!memcmp(dst, Data, Count * sizeof(uint32_t)))
      return;
    memcpy(dst, Data, Count * sizeof(uint32_t));
    RootArgumentsChanged(dirty);
  }

  void STDMETHODCALLTYPE SetComputeRootDescriptorTable(UINT Index, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) {
    if (Bundled() && Record([=](auto *l) { l->SetComputeRootDescriptorTable(Index, BaseDescriptor); }))
      return;
    SetRootQword(rootsig_compute_.ptr(), rootarg_compute_staging_, DirtyState::ComputeRootArguments, Index, BaseDescriptor.ptr);
  };

  void STDMETHODCALLTYPE
  SetGraphicsRootDescriptorTable(UINT Index, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) {
    if (Bundled() && Record([=](auto *l) { l->SetGraphicsRootDescriptorTable(Index, BaseDescriptor); }))
      return;
    SetRootQword(rootsig_graphics_.ptr(), rootarg_graphics_staging_, DirtyState::GraphicsRootArguments, Index, BaseDescriptor.ptr);
  };

  void STDMETHODCALLTYPE SetComputeRoot32BitConstant(UINT Index, UINT Data, UINT DstOffset) {
    if (Bundled() && Record([=](auto *l) { l->SetComputeRoot32BitConstant(Index, Data, DstOffset); }))
      return;
    SetRootConstants(rootsig_compute_.ptr(), rootarg_compute_staging_, DirtyState::ComputeRootArguments, Index, 1, &Data, DstOffset);
  };

  void STDMETHODCALLTYPE
  SetGraphicsRoot32BitConstant(UINT Index, UINT Data, UINT DstOffset) {
    if (Bundled() && Record([=](auto *l) { l->SetGraphicsRoot32BitConstant(Index, Data, DstOffset); }))
      return;
    SetRootConstants(rootsig_graphics_.ptr(), rootarg_graphics_staging_, DirtyState::GraphicsRootArguments, Index, 1, &Data, DstOffset);
  };

  void STDMETHODCALLTYPE
  SetComputeRoot32BitConstants(UINT Index, UINT ConstantCount, const void *pData, UINT DstOffset) {
    if (Bundled() && Record([=, v = std::vector((const UINT *)pData, (const UINT *)pData + ConstantCount)](auto *l) { l->SetComputeRoot32BitConstants(Index, ConstantCount, v.data(), DstOffset); }))
      return;
    SetRootConstants(rootsig_compute_.ptr(), rootarg_compute_staging_, DirtyState::ComputeRootArguments, Index, ConstantCount, pData, DstOffset);
  };

  void STDMETHODCALLTYPE
  SetGraphicsRoot32BitConstants(UINT Index, UINT ConstantCount, const void *pData, UINT DstOffset) {
    if (Bundled() && Record([=, v = std::vector((const UINT *)pData, (const UINT *)pData + ConstantCount)](auto *l) { l->SetGraphicsRoot32BitConstants(Index, ConstantCount, v.data(), DstOffset); }))
      return;
    SetRootConstants(rootsig_graphics_.ptr(), rootarg_graphics_staging_, DirtyState::GraphicsRootArguments, Index, ConstantCount, pData, DstOffset);
  };

  void STDMETHODCALLTYPE SetComputeRootConstantBufferView(UINT Index, D3D12_GPU_VIRTUAL_ADDRESS VA) {
    if (Bundled() && Record([=](auto *l) { l->SetComputeRootConstantBufferView(Index, VA); }))
      return;
    SetRootQword(
        rootsig_compute_.ptr(), rootarg_compute_staging_, DirtyState::ComputeRootArguments, Index,
        VA ? VA : device_->GetNullBuffer()
    );
  };

  void STDMETHODCALLTYPE
  SetGraphicsRootConstantBufferView(UINT Index, D3D12_GPU_VIRTUAL_ADDRESS VA) {
    if (Bundled() && Record([=](auto *l) { l->SetGraphicsRootConstantBufferView(Index, VA); }))
      return;
    SetRootQword(
        rootsig_graphics_.ptr(), rootarg_graphics_staging_, DirtyState::GraphicsRootArguments, Index,
        VA ? VA : device_->GetNullBuffer()
    );
  };

  void STDMETHODCALLTYPE SetComputeRootShaderResourceView(UINT Index, D3D12_GPU_VIRTUAL_ADDRESS VA) {
    if (Bundled() && Record([=](auto *l) { l->SetComputeRootShaderResourceView(Index, VA); }))
      return;
    SetRootQword(rootsig_compute_.ptr(), rootarg_compute_staging_, DirtyState::ComputeRootArguments, Index, VA);
  };

  void STDMETHODCALLTYPE
  SetGraphicsRootShaderResourceView(UINT Index, D3D12_GPU_VIRTUAL_ADDRESS VA) {
    if (Bundled() && Record([=](auto *l) { l->SetGraphicsRootShaderResourceView(Index, VA); }))
      return;
    SetRootQword(rootsig_graphics_.ptr(), rootarg_graphics_staging_, DirtyState::GraphicsRootArguments, Index, VA);
  };

  void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(UINT Index, D3D12_GPU_VIRTUAL_ADDRESS VA) {
    if (Bundled() && Record([=](auto *l) { l->SetComputeRootUnorderedAccessView(Index, VA); }))
      return;
    SetRootQword(rootsig_compute_.ptr(), rootarg_compute_staging_, DirtyState::ComputeRootArguments, Index, VA);
  };

  void STDMETHODCALLTYPE
  SetGraphicsRootUnorderedAccessView(UINT Index, D3D12_GPU_VIRTUAL_ADDRESS VA) {
    if (Bundled() && Record([=](auto *l) { l->SetGraphicsRootUnorderedAccessView(Index, VA); }))
      return;
    SetRootQword(rootsig_graphics_.ptr(), rootarg_graphics_staging_, DirtyState::GraphicsRootArguments, Index, VA);
  };

  void STDMETHODCALLTYPE
  IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW *pView) {
    if (Bundled() && Record([view = pView ? std::optional(*pView) : std::nullopt](auto *l) { l->IASetIndexBuffer(view ? &*view : nullptr); }))
      return;
    // the view bound already: games set one per draw
    if (pView ? index_buffer_address && !memcmp(pView, &index_view_, sizeof(*pView)) : !index_buffer_address)
      return;
    auto index_buffer_allocation = pView ? LookupBuffer(pView->BufferLocation, &index_offset) : nullptr;
    index_view_ = index_buffer_allocation ? *pView : D3D12_INDEX_BUFFER_VIEW{};
    if (index_buffer_allocation) {
      index_buffer_address = pView->BufferLocation;
      index_buffer = index_buffer_allocation->buffer();
      index_type = pView->Format == DXGI_FORMAT_R32_UINT ? WMTIndexTypeUInt32 : WMTIndexTypeUInt16;
    } else {
      index_buffer_address = 0;
      index_buffer = {};
      index_type = {};
      index_offset = {};
    }
  };

  void STDMETHODCALLTYPE
  IASetVertexBuffers(UINT StartSlot, UINT Count, const D3D12_VERTEX_BUFFER_VIEW *Views) {
    // a range past the last slot is invalid, and changes no slot, as in vkd3d-proton
    if (StartSlot >= vertex_buffers_.size() || Count > vertex_buffers_.size() - StartSlot)
      return;
    if (Bundled() && Record([=, v = Views ? std::vector(Views, Views + Count) : std::vector<D3D12_VERTEX_BUFFER_VIEW>()](auto *l) { l->IASetVertexBuffers(StartSlot, Count, Views ? v.data() : nullptr); }))
      return;
    if (!Views)
      return;
    // the vertex buffer table is written again only for views that change
    if (!memcmp(&vertex_buffers_[StartSlot], Views, Count * sizeof(*Views)))
      return;
    for (unsigned Slot = StartSlot; Slot < StartSlot + Count; Slot++) {
      vertex_buffers_[Slot] = Views[Slot - StartSlot];
    }
    dirty_state_.set(DirtyState::VertexBuffer);
  };

  void STDMETHODCALLTYPE SOSetTargets(UINT StartSlot, UINT Count, const D3D12_STREAM_OUTPUT_BUFFER_VIEW *Views) {
    for (UINT i = 0; i < Count && StartSlot + i < std::size(so_views_); i++)
      so_views_[StartSlot + i] = Views ? Views[i] : D3D12_STREAM_OUTPUT_BUFFER_VIEW{};
  };

  void STDMETHODCALLTYPE
  OMSetRenderTargets(
      UINT NumRTV, const D3D12_CPU_DESCRIPTOR_HANDLE *RTVs, WINBOOL SingleDescriptor,
      const D3D12_CPU_DESCRIPTOR_HANDLE *DSV
  ) {
    bool changed = num_rtvs != NumRTV;
    // the views as they are now: a descriptor written over after this call is not the bound one
    auto bind = [&](MTL_RENDER_TARGET_DESC &bound, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
      MTL_RENDER_TARGET_DESC target{};
      if (handle.ptr) {
        auto [Heap, Index] = GetRenderTargetHeap(device_, handle);
        target = Heap->GetRenderTarget(Index);
      }
      changed |= bound != target;
      bound = target;
    };
    num_rtvs = NumRTV;
    auto step = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (unsigned i = 0; i < NumRTV; i++)
      bind(rtvs[i], SingleDescriptor ? D3D12_CPU_DESCRIPTOR_HANDLE{RTVs[0].ptr + i * step} : RTVs[i]);
    bind(dsv, DSV ? *DSV : D3D12_CPU_DESCRIPTOR_HANDLE{});
    if (changed)
      allocator_->InvalidateCurrentPass();
  };

  void STDMETHODCALLTYPE
  ClearDepthStencilView(
      D3D12_CPU_DESCRIPTOR_HANDLE DSV, D3D12_CLEAR_FLAGS Flags, FLOAT Depth, UINT8 Stencil, UINT RectCount,
      const D3D12_RECT *Rects
  ) {
    PredicatedPasses predicated(this);
    auto [Heap, Index] = GetRenderTargetHeap(device_, DSV);
    auto AttachmentDesc = Heap->GetRenderTarget(Index);
    if (!AttachmentDesc.Texture)
      return;
    auto CheckedFlags = Flags & DepthStencilPlanarFlags(AttachmentDesc.Texture->pixelFormat(AttachmentDesc.View));
    if (!CheckedFlags)
      return;
    if ((CheckedFlags & D3D12_CLEAR_FLAG_DEPTH) &&
        (AttachmentDesc.DepthFormat == DXGI_FORMAT_D16_UNORM ||
         AttachmentDesc.DepthFormat == DXGI_FORMAT_D24_UNORM_S8_UINT)) {
      auto maximum = AttachmentDesc.DepthFormat == DXGI_FORMAT_D16_UNORM ? UINT16_MAX : kD24UnormMax;
      auto code = std::lround(std::fmin(std::fmax(double(Depth), 0.0), 1.0) * maximum);
      Depth = float(double(code) / maximum);
      // a Metal clear may truncate; an upper float keeps the rounded D16 code (D3D11.3 3.2.3.6).
      if (AttachmentDesc.DepthFormat == DXGI_FORMAT_D16_UNORM)
        Depth = std::nextafter(Depth, 1.0f);
    }
    // no rectangles clear the whole view, whatever the array is, as vkd3d-proton does
    if (Rects && RectCount) {
      UsedTexture(AttachmentDesc.Texture);
      allocator_->clear_rtv_.begin(AttachmentDesc.Texture, AttachmentDesc.View, 0, CheckedFlags);
      for (unsigned i = 0; i < RectCount; i++) {
        auto rect = Rects[i];
        uint32_t rect_offset_x = std::max(rect.left, (LONG)0);
        uint32_t rect_offset_y = std::max(rect.top, (LONG)0);
        int32_t rect_width = rect.right - rect_offset_x;
        int32_t rect_height = rect.bottom - rect_offset_y;
        if (rect_height <= 0 || rect_width <= 0)
          continue;
        allocator_->clear_rtv_.clear(rect_offset_x, rect_offset_y, rect_width, rect_height, Depth, Stencil);
      }
      allocator_->clear_rtv_.end();
      return;
    }
    // If we are inside an active render pass that already has draws and binds this exact DSV with
    // other attachments (e.g. color or depth), clearing stencil-only in-pass via a fullscreen triangle
    // avoids ending and restarting the TBDR render pass (preventing full color+depth store/load traffic).
    static const bool inpass_stencil_clear = env::getEnvVar("MULLION_INPASS_STENCIL_CLEAR") != "0";
    if (inpass_stencil_clear && CheckedFlags == D3D12_CLEAR_FLAG_STENCIL && !predication_region_ && active_queries_.empty() &&
        active_statistics_.empty() && allocator_->encoder_current &&
        allocator_->encoder_current->type == EncoderType::Render) {
      auto render = static_cast<RenderEncoderData *>(allocator_->encoder_current);
      auto &dsv_view = AttachmentDesc.Texture->view(AttachmentDesc.View);
      bool has_other_attachment = bool(render->depth.attachment);
      for (unsigned i = 0; !has_other_attachment && i < 8; i++)
        has_other_attachment = bool(render->colors[i].attachment);
      if (has_other_attachment && !render->rasterless && !render->use_visibility_result &&
          !(render->dsv_readonly_flags & D3D12_DSV_FLAG_READ_ONLY_STENCIL) &&
          render->stencil.attachment.ptr() == &dsv_view && render->cmd_head.next.ptr != nullptr &&
          render->render_target_array_length <= 1 && AttachmentDesc.Texture->arrayLength(AttachmentDesc.View) <= 1 &&
          render->viewports + 1 <= WMT::kPassRectangles && render->scissors + 1 <= WMT::kPassRectangles) {
        std::array<WMTPixelFormat, 8> color_formats{};
        color_formats.fill(WMTPixelFormatInvalid);
        for (unsigned i = 0; i < 8; i++) {
          if (render->colors[i].attachment && i < num_rtvs && rtvs[i].Texture)
            color_formats[i] = rtvs[i].Texture->pixelFormat(rtvs[i].View);
        }
        auto dsv_fmt = AttachmentDesc.Texture->pixelFormat(AttachmentDesc.View);
        auto depth_fmt = render->depth.attachment ? dsv_fmt : WMTPixelFormatInvalid;
        auto pso = device_->GetLib().InPassStencilClearPipeline(
            color_formats, depth_fmt, dsv_fmt, AttachmentDesc.Texture->sampleCount()
        );
        if (pso) {
          auto &cmd_pso = allocator_->EncodeRenderCommand<wmtcmd_render_setpso>();
          cmd_pso.type = WMTRenderCommandSetPSO;
          cmd_pso.pso = pso;

          auto &cmd_ds = allocator_->EncodeRenderCommand<wmtcmd_render_setdepthstencilstate>();
          cmd_ds.type = WMTRenderCommandSetDepthStencilState;
          cmd_ds.depth_stencil_state = device_->GetLib().ClearDepthStencilState(D3D12_CLEAR_FLAG_STENCIL);

          auto &cmd_rs = allocator_->EncodeRenderCommand<wmtcmd_render_setrasterizerstate>();
          cmd_rs.type = WMTRenderCommandSetRasterizerState;
          cmd_rs.cull_mode = WMTCullModeNone;
          cmd_rs.depth_clip_mode = WMTDepthClipModeClip;
          cmd_rs.fill_mode = WMTTriangleFillModeFill;
          cmd_rs.depth_bias = 0.0f;
          cmd_rs.depth_bias_clamp = 0.0f;
          cmd_rs.scole_scale = 0.0f;
          cmd_rs.winding = WMTWindingClockwise;

          auto &cmd_sref = allocator_->EncodeRenderCommand<wmtcmd_render_setstencilref>();
          cmd_sref.type = WMTRenderCommandSetStencilRef;
          cmd_sref.stencil_ref = Stencil;
          cmd_sref.back_stencil_ref = Stencil;

          auto vp = allocator_->AllocateCommandData<WMTViewport>(1);
          vp[0] = {0.0, 0.0, double(render->render_target_width), double(render->render_target_height), 0.0, 1.0};
          auto &cmd_vp = allocator_->EncodeRenderCommand<wmtcmd_render_setviewports>();
          cmd_vp.type = WMTRenderCommandSetViewports;
          cmd_vp.viewports.set(vp);
          cmd_vp.viewport_count = 1;
          render->viewports += 1;

          auto sc = allocator_->AllocateCommandData<WMTScissorRect>(1);
          sc[0] = {0, 0, render->render_target_width, render->render_target_height};
          auto &cmd_sc = allocator_->EncodeRenderCommand<wmtcmd_render_setscissorrects>();
          cmd_sc.type = WMTRenderCommandSetScissorRects;
          cmd_sc.scissor_rects.set(sc);
          cmd_sc.rect_count = 1;
          render->scissors += 1;

          auto &cmd_draw = allocator_->EncodeRenderCommand<wmtcmd_render_draw>();
          cmd_draw.type = WMTRenderCommandDraw;
          cmd_draw.primitive_type = WMTPrimitiveTypeTriangle;
          cmd_draw.vertex_start = 0;
          cmd_draw.vertex_count = 3;
          cmd_draw.base_instance = 0;
          cmd_draw.instance_count = 1;

          dirty_state_.set(
              DirtyState::GraphicsPipelineState, DirtyState::Viewport, DirtyState::ScissorRect, DirtyState::StencilRef
          );
          return;
        }
      }
    }
    auto encoder_info = JoinableClear(AttachmentDesc);
    if (encoder_info && (encoder_info->depth.attachment || encoder_info->stencil.attachment ||
                         ClearTextures(encoder_info, AttachmentDesc.Texture)))
      encoder_info = nullptr;
    if (!encoder_info) {
      encoder_info = StartClearPass(AttachmentDesc);
    } else if (AttachmentDesc.Texture) {
      const Texture *tex = AttachmentDesc.Texture;
      if (encoder_info->overlap == PassOverlap::Independent &&
          std::ranges::find(overlap_targets_, tex) != overlap_targets_.end())
        encoder_info->overlap = PassOverlap::Shared;
      overlap_targets_.push_back(tex);
    }
    if (!clears_.empty() && clears_.back().pass == encoder_info)
      clears_.back().textures.back() = AttachmentDesc.Texture;
    if (CheckedFlags & D3D12_CLEAR_FLAG_DEPTH) {
      auto &rt = encoder_info->depth;
      rt.attachment = AttachmentDesc.Texture->view(AttachmentDesc.View);
      rt.load_action = WMTLoadActionClear;
      rt.store_action = WMTStoreActionStore;
      rt.clear_depth = Depth;
    }
    if (CheckedFlags & D3D12_CLEAR_FLAG_STENCIL) {
      auto &rt = encoder_info->stencil;
      rt.attachment = AttachmentDesc.Texture->view(AttachmentDesc.View);
      rt.load_action = WMTLoadActionClear;
      rt.store_action = WMTStoreActionStore;
      rt.clear_stencil = Stencil;
    }
  };

  void STDMETHODCALLTYPE
  ClearRenderTargetView(
      D3D12_CPU_DESCRIPTOR_HANDLE RTV, const FLOAT Color[4], UINT RectCount, const D3D12_RECT *Rects
  ) {
    PredicatedPasses predicated(this);
    auto [Heap, Index] = GetRenderTargetHeap(device_, RTV);
    auto AttachmentDesc = Heap->GetRenderTarget(Index);
    if (!AttachmentDesc.Texture)
      return;
    auto stored = RenderTargetColor(AttachmentDesc.Format, Color);
    Color = stored.data();
    // no rectangles clear the whole view, whatever the array is, as vkd3d-proton does
    if (Rects && RectCount) {
      UsedTexture(AttachmentDesc.Texture);
      allocator_->clear_rtv_.begin(AttachmentDesc.Texture, AttachmentDesc.View, AttachmentDesc.DepthPlane);
      for (unsigned i = 0; i < RectCount; i++) {
        auto rect = Rects[i];
        uint32_t rect_offset_x = std::max(rect.left, (LONG)0);
        uint32_t rect_offset_y = std::max(rect.top, (LONG)0);
        int32_t rect_width = rect.right - rect_offset_x;
        int32_t rect_height = rect.bottom - rect_offset_y;
        if (rect_height <= 0 || rect_width <= 0)
          continue;
        allocator_->clear_rtv_.clear(
            rect_offset_x, rect_offset_y, rect_width, rect_height, AttachmentDesc.RenderTargetArrayLength,
            {Color[0], Color[1], Color[2], Color[3]}
        );
      }
      allocator_->clear_rtv_.end();
      return;
    }
    // the first free color of a clear pass of the same size, which has no view of the texture yet
    auto encoder_info = JoinableClear(AttachmentDesc);
    unsigned slot = 0;
    if (encoder_info) {
      while (slot < std::size(encoder_info->colors) && encoder_info->colors[slot].attachment)
        slot++;
      if (slot == std::size(encoder_info->colors) || ClearTextures(encoder_info, AttachmentDesc.Texture))
        encoder_info = nullptr;
    }
    if (!encoder_info) {
      encoder_info = StartClearPass(AttachmentDesc);
      slot = 0;
    } else if (AttachmentDesc.Texture) {
      const Texture *tex = AttachmentDesc.Texture;
      if (encoder_info->overlap == PassOverlap::Independent &&
          std::ranges::find(overlap_targets_, tex) != overlap_targets_.end())
        encoder_info->overlap = PassOverlap::Shared;
      overlap_targets_.push_back(tex);
    }
    if (!clears_.empty() && clears_.back().pass == encoder_info)
      clears_.back().textures[slot] = AttachmentDesc.Texture;
    auto &rt = encoder_info->colors[slot];
    rt.attachment = AttachmentDesc.Texture->view(AttachmentDesc.View);
    rt.depth_plane = AttachmentDesc.DepthPlane;
    rt.load_action = WMTLoadActionClear;
    rt.store_action = WMTStoreActionStore;
    rt.clear_color = {Color[0], Color[1], Color[2], Color[3]};
    SanitizeRTVClearColor(AttachmentDesc.Texture->pixelFormat(AttachmentDesc.View), rt.clear_color);
  };

  // a pass of its own for a full clear of a view, which a draw to the view takes as its load action
  RenderEncoderData *
  StartClearPass(const MTL_RENDER_TARGET_DESC &Desc) {
    allocator_->InvalidateCurrentPass();
    auto clear = allocator_->AllocatePass<RenderEncoderData>();
    clear->type = EncoderType::Clear;
    clear->previous = allocator_->encoder_last;
    clear->render_target_array_length = Desc.RenderTargetArrayLength;
    clear->render_target_width = Desc.Width;
    clear->render_target_height = Desc.Height;
    clear->clear_sample_count = Desc.Texture->sampleCount();
    const Texture *tex = Desc.Texture;
    Overlap(clear, {&tex, 1});
    allocator_->InvalidateCurrentPass();
    clear_joinable_ = true;
    clears_.push_back({clear});
    return clear;
  }

  // whether a clear pass clears a view of `texture` already: one pass takes one view of each, the latest clear's.
  // a pass no later pass can take (PendingClear) is taken as clearing every texture
  bool
  ClearTextures(RenderEncoderData *clear, const Texture *texture) {
    if (clears_.empty() || clears_.back().pass != clear)
      return true;
    return std::ranges::find(clears_.back().textures, texture) != clears_.back().textures.end();
  }

  // the clear pass just before, which a full clear of a view of the same size can join (a color clear another color's
  // or a depth stencil clear's, or that one a color clear's): Metal clears the attachments of one pass for about the
  // cost of one. nothing between the two orders them, but an aliasing barrier, which ends the chance
  RenderEncoderData *
  JoinableClear(const MTL_RENDER_TARGET_DESC &Desc) {
    auto last = allocator_->encoder_last;
    if (!clear_joinable_ || allocator_->encoder_current || !last || last->type != EncoderType::Clear || last->stamps)
      return nullptr;
    auto clear = static_cast<RenderEncoderData *>(last);
    if (clear->render_target_width != Desc.Width || clear->render_target_height != Desc.Height ||
        clear->render_target_array_length != Desc.RenderTargetArrayLength ||
        clear->clear_sample_count != Desc.Texture->sampleCount())
      return nullptr;
    return clear;
  }

  // a UAV clear of a buffer to a value of four equal bytes, when no compute pass is open, is a blit fill: one more
  // command in an open blit pass, or a pass that needs no pipeline. the elements are D3D12's (element_stride words)
  bool
  FillBufferClear(const UAVBufferCPUStorage &View, uint32_t Value, UINT RectCount, const D3D12_RECT *pRects) {
    if (Value != (Value & 0xff) * 0x01010101u || View.buffer->current()->sparse_tile_bytes ||
        (allocator_->encoder_current && allocator_->encoder_current->type == EncoderType::Compute))
      return false;
    if (!View.slice.elementCount || View.slice.byteLength < sizeof(uint32_t))
      return true;
    UINT element_stride =
        std::max<UINT>(1u, UINT(View.slice.byteLength / (uint64_t(View.slice.elementCount) * sizeof(uint32_t))));
    int64_t first = View.slice.byteOffset >> 2, words = View.slice.byteLength >> 2;
    D3D12_RECT whole{0, 0, (LONG)(words / element_stride), 1};
    for (auto &rect : RectCount ? std::span(pRects, RectCount) : std::span(&whole, 1)) {
      int64_t left = std::max<int64_t>(rect.left, 0), right = std::min<int64_t>(rect.right, words / element_stride);
      if (right <= left || rect.bottom <= std::max<LONG>(rect.top, 0) || rect.top >= 1)
        continue;
      PreBlit();
      auto &fill = allocator_->EncodeBlitCommand<wmtcmd_blit_fillbuffer>();
      fill.type = WMTBlitCommandFillBuffer;
      fill.buffer = View.buffer->current()->buffer();
      fill.offset = (first + left * element_stride) * sizeof(uint32_t);
      fill.length = (right - left) * element_stride * sizeof(uint32_t);
      fill.value = Value & 0xff;
    }
    return true;
  }

  // a uint clear that gives an RG11B10Float texture without an integer view (PopulateWMTTextureInfo) a NaN: Metal
  // stores every NaN a kernel writes as one pattern (ClearUAV, UIntClearWrite::SmallFloat), and a blit from a buffer
  // stores the clear's bits as they are. rows of the texel, as many as kBytes hold, go over each rectangle
  bool
  BlitNaNClear(
      const UAVTextureCPUStorage &View, const std::array<uint32_t, 4> &Color, UINT RectCount, const D3D12_RECT *pRects
  ) {
    auto texture = View.texture;
    auto nan = [](uint32_t bits, unsigned mantissa) {
      return (bits >> mantissa & 31) == 31 && (bits & ((1u << mantissa) - 1));
    };
    if (texture->pixelFormat(View.view) != WMTPixelFormatRG11B10Float ||
        (texture->usage() & WMTTextureUsagePixelFormatView) ||
        !(nan(Color[0], 6) || nan(Color[1], 6) || nan(Color[2], 5)))
      return false;
    auto type = texture->textureType(View.view);
    bool volume = type == WMTTextureType3D;
    if (!volume && type != WMTTextureType2D && type != WMTTextureType2DArray)
      return false;
    const uint32_t texel = decltype(allocator_->clear_uav_)::clearColor(Color, WMTPixelFormatRG11B10Float)[0];
    const int64_t width = texture->width(View.view), height = texture->height(View.view);
    // a 3D texture's W slices, or the view's array slices
    const uint32_t first = volume ? View.first_slice : uint32_t(View.view.array_start),
                   count = !volume ? texture->arrayLength(View.view)
                                   : View.slice_count ? View.slice_count : texture->depth(View.view);
    constexpr int64_t kBytes = 65536;
    D3D12_RECT whole{0, 0, LONG(width), LONG(height)};
    for (auto &rect : RectCount ? std::span(pRects, RectCount) : std::span(&whole, 1)) {
      const int64_t left = std::max<int64_t>(rect.left, 0), top = std::max<int64_t>(rect.top, 0),
                    right = std::min<int64_t>(rect.right, width), bottom = std::min<int64_t>(rect.bottom, height);
      if (right <= left || bottom <= top)
        continue;
      const int64_t wide = right - left, rows = std::min(bottom - top, std::max<int64_t>(kBytes / (wide * 4), 1));
      auto [mapped, offset] = allocator_->AllocateGPUHeap(wide * rows * sizeof(texel), alignof(uint32_t));
      if (!mapped)
        return false;
      if (allocator_->records_)
        allocator_->RecordGPUHeap(mapped, wide * rows * sizeof(texel), "NaN clear texels");
      std::fill_n(static_cast<uint32_t *>(mapped), wide * rows, texel);
      PreBlit();
      for (uint32_t slice = first; slice < first + count; slice++)
        for (int64_t y = top; y < bottom; y += rows) {
          auto &copy = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture>();
          copy.type = WMTBlitCommandCopyFromBufferToTexture;
          copy.src = allocator_->gpu_heap_buffer_;
          copy.src_offset = offset;
          copy.bytes_per_row = wide * sizeof(texel);
          copy.bytes_per_image = wide * rows * sizeof(texel);
          copy.size = {uint64_t(wide), uint64_t(std::min(rows, bottom - y)), 1};
          copy.dst = texture->current()->texture();
          copy.level = View.view.mip_start;
          copy.slice = volume ? 0 : slice;
          copy.origin = {uint64_t(left), uint64_t(y), volume ? slice : 0};
        }
    }
    return true;
  }

  // the clear's kernel joins the open compute pass (share_compute_), and the list's next dispatch binds its own
  // pipeline and arguments again
  struct SharedComputePass {
    MTLD3D12GraphicsCommandListImpl *list;
    bool shared;
    SharedComputePass(MTLD3D12GraphicsCommandListImpl *list) :
        list(list),
        shared(list->allocator_->encoder_current && list->allocator_->encoder_current->type == EncoderType::Compute) {
      list->allocator_->share_compute_ = shared;
    }
    ~SharedComputePass() {
      list->allocator_->share_compute_ = false;
      if (shared)
        list->dirty_state_.set(
            DirtyState::ComputePipelineState, DirtyState::ComputeRootArguments, DirtyState::ComputeRootSignature
        );
    }
  };

  // the rectangles of a UAV clear that ClearUAV has begun, or the whole view with none, and the clear's end. a
  // rectangle counts a buffer's elements, and the kernel the buffer's words or the view's texels from the start of
  // the buffer: the view's first is kept in 64 bits, as a view may start 4 GiB or more into a buffer
  void
  ClearUAVRectangles(const ShaderVisibleDescriptorCPUStorage &Descriptor, UINT RectCount, const D3D12_RECT *pRects) {
    uint64_t first = 0;
    int64_t width = 0, height = 1;
    UINT element_stride = 1, first_slice = 0, slice_count = 0;
    switch (Descriptor.type) {
    case ShaderVisibleDescriptorType::UAVBuffer: {
      auto &slice = Descriptor.UAVBuffer.slice;
      if (!slice.elementCount || slice.byteLength < sizeof(uint32_t)) {
        width = 0;
        break;
      }
      element_stride =
          std::max<UINT>(1u, UINT(slice.byteLength / (uint64_t(slice.elementCount) * sizeof(uint32_t))));
      first = slice.byteOffset / sizeof(uint32_t);
      width = slice.byteLength / sizeof(uint32_t) / element_stride;
      break;
    }
    case ShaderVisibleDescriptorType::UAVTexture:
      first_slice = Descriptor.UAVTexture.first_slice;
      slice_count = Descriptor.UAVTexture.slice_count;
      width = Descriptor.UAVTexture.texture->width(Descriptor.UAVTexture.view);
      height = Descriptor.UAVTexture.texture->height(Descriptor.UAVTexture.view);
      break;
    case ShaderVisibleDescriptorType::UAVTexelBuffer:
      first = Descriptor.UAVTexelBuffer.slice.firstElement;
      width = Descriptor.UAVTexelBuffer.slice.elementCount;
      break;
    default:
      break;
    }
    for (UINT i = 0; i < std::max(RectCount, 1u); i++) {
      int64_t left = 0, top = 0, right = width, bottom = height;
      if (RectCount) {
        left = std::max<int64_t>(pRects[i].left, 0), top = std::max<int64_t>(pRects[i].top, 0);
        right = std::min<int64_t>(pRects[i].right, width), bottom = std::min<int64_t>(pRects[i].bottom, height);
      }
      if (right <= left || bottom <= top)
        continue;
      allocator_->clear_uav_.clear(
          first + left * element_stride, top, (right - left) * element_stride, bottom - top, first_slice, slice_count
      );
    }
    allocator_->clear_uav_.end();
  }

  void STDMETHODCALLTYPE
  ClearUnorderedAccessViewUint(
      D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle, D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle, ID3D12Resource *pResource,
      const UINT Values[4], UINT RectCount, const D3D12_RECT *pRects
  ) {
    PredicatedPasses predicated(this);
    auto [Heap, Index] = GetShaderVisibleDescriptorHeap(device_, CpuHandle);
    auto &Descriptor = Heap->GetDescriptor(Index);
    auto color = std::array<uint32_t, 4>({Values[0], Values[1], Values[2], Values[3]});
    // a cleared sampler feedback map wants no mip anywhere, which its texels say with 0 ("Clearing")
    if (pResource && static_cast<MTLD3D12Resource *>(pResource)->feedback) {
      color = {};
      RectCount = 0;
    }
    if (Descriptor.type == ShaderVisibleDescriptorType::UAVTexelBuffer &&
        Descriptor.UAVTexelBuffer.buffer->current()->sparse_tile_bytes) {
      allocator_->ClearSparseTexelBuffer(color, Descriptor.UAVTexelBuffer.buffer, Descriptor.UAVTexelBuffer.view,
          Descriptor.UAVTexelBuffer.slice, RectCount, pRects);
      return;
    }
    if (Descriptor.type == ShaderVisibleDescriptorType::UAVBuffer &&
        FillBufferClear(Descriptor.UAVBuffer, color[0], RectCount, pRects))
      return;
    if (Descriptor.type == ShaderVisibleDescriptorType::UAVTexture &&
        BlitNaNClear(Descriptor.UAVTexture, color, RectCount, pRects))
      return;
    SharedComputePass shared(this);
    switch (Descriptor.type) {
    case ShaderVisibleDescriptorType::UAVBuffer:
      allocator_->clear_uav_.begin(color, Descriptor.UAVBuffer.buffer);
      break;
    case ShaderVisibleDescriptorType::UAVTexture:
      allocator_->clear_uav_.begin(color, Descriptor.UAVTexture.texture, Descriptor.UAVTexture.view);
      break;
    case ShaderVisibleDescriptorType::UAVTexelBuffer:
      allocator_->clear_uav_.begin(color, Descriptor.UAVTexelBuffer.buffer, Descriptor.UAVTexelBuffer.view);
      break;
    default:
      allocator_->clear_uav_.end();
      return;
    }
    ClearUAVRectangles(Descriptor, RectCount, pRects);
  };

  void STDMETHODCALLTYPE
  ClearUnorderedAccessViewFloat(
      D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle, D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle, ID3D12Resource *pResource,
      const float Values[4], UINT RectCount, const D3D12_RECT *pRects
  ) {
    PredicatedPasses predicated(this);
    auto [Heap, Index] = GetShaderVisibleDescriptorHeap(device_, CpuHandle);
    auto &Descriptor = Heap->GetDescriptor(Index);
    auto color = std::array<float, 4>({Values[0], Values[1], Values[2], Values[3]});
    if (Descriptor.type == ShaderVisibleDescriptorType::UAVTexelBuffer &&
        Descriptor.UAVTexelBuffer.buffer->current()->sparse_tile_bytes) {
      allocator_->ClearSparseTexelBuffer(color, Descriptor.UAVTexelBuffer.buffer, Descriptor.UAVTexelBuffer.view,
          Descriptor.UAVTexelBuffer.slice, RectCount, pRects);
      return;
    }
    if (Descriptor.type == ShaderVisibleDescriptorType::UAVBuffer &&
        FillBufferClear(Descriptor.UAVBuffer, std::bit_cast<uint32_t>(color[0]), RectCount, pRects))
      return;
    SharedComputePass shared(this);
    switch (Descriptor.type) {
    case ShaderVisibleDescriptorType::UAVBuffer:
      allocator_->clear_uav_.begin(color, Descriptor.UAVBuffer.buffer);
      break;
    case ShaderVisibleDescriptorType::UAVTexture:
      allocator_->clear_uav_.begin(color, Descriptor.UAVTexture.texture, Descriptor.UAVTexture.view);
      break;
    case ShaderVisibleDescriptorType::UAVTexelBuffer:
      allocator_->clear_uav_.begin(color, Descriptor.UAVTexelBuffer.buffer, Descriptor.UAVTexelBuffer.view);
      break;
    default:
      allocator_->clear_uav_.end();
      return;
    }
    ClearUAVRectangles(Descriptor, RectCount, pRects);
  };

  // DiscardResource of a whole texture: its contents are undefined from here (ID3D12GraphicsCommandList::
  // DiscardResource), so the render pass that drew it last, with nothing after it, stores none of it (an MSAA target
  // resolved in that pass resolves without storing its samples), and the next one loads none. a discard of rectangles
  // or of some subresources does nothing, as before
  void STDMETHODCALLTYPE DiscardResource(ID3D12Resource *pResource, const D3D12_DISCARD_REGION *pRegion) {
    auto resource = static_cast<MTLD3D12Resource *>(pResource);
    if (!resource || !resource->texture || Bundled() || !DiscardsTextures())
      return;
    if (pRegion) {
      auto desc = resource->GetDesc();
      MTL_DXGI_FORMAT_DESC format;
      UINT planes = 1;
      if (SUCCEEDED(MTLQueryDXGIFormat(device_->GetMTLDevice(), desc.Format, format)))
        planes = DepthStencilPlanarFlags(WMTPixelFormat(ORIGINAL_FORMAT(format.PixelFormat))) == 3 ? 2
                 : (format.Flag & MTL_DXGI_FORMAT_VIDEO_PLANAR)                                    ? format.PlanarCount
                                                                                                   : 1;
      UINT all = desc.MipLevels * (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc.DepthOrArraySize) *
                 std::max(planes, 1u);
      if (pRegion->NumRects || pRegion->FirstSubresource || pRegion->NumSubresources < all)
        return;
    }
    DiscardTexture(resource->texture.ptr());
  };

  // MULLION_D3D12_DISCARDS (on unless 0, read once): DiscardResource and the render passes' discarding accesses store
  // and load nothing of what they discard
  static bool
  DiscardsTextures() {
    static const bool discards = env::getEnvVar("MULLION_D3D12_DISCARDS") != "0";
    return discards;
  }

  void
  DiscardTexture(Texture *texture) {
    const TextureAllocation *allocation = texture->current();
    auto uses = [&](const EncoderData *encoder) {
      if (!encoder || encoder->type != EncoderType::Render)
        return false;
      auto pass = static_cast<const RenderEncoderData *>(encoder);
      auto of = [&](const TextureViewRef &view) { return view && view->allocation == allocation; };
      return std::ranges::any_of(pass->colors, [&](auto &color) { return of(color.attachment); }) ||
             of(pass->depth.attachment) || of(pass->stencil.attachment);
    };
    // a pass that draws to it still ends here: what it drew is discarded, and a draw after starts the next
    if (uses(allocator_->encoder_current))
      allocator_->InvalidateCurrentPass();
    if (!allocator_->encoder_current && uses(allocator_->encoder_last)) {
      auto pass = static_cast<RenderEncoderData *>(allocator_->encoder_last);
      for (auto &color : pass->colors)
        if (color.attachment && color.attachment->allocation == allocation) {
          if (color.store_action == WMTStoreActionStore)
            color.store_action = WMTStoreActionDontCare;
          else if (color.store_action == WMTStoreActionStoreAndMultisampleResolve)
            color.store_action = WMTStoreActionMultisampleResolve;
        }
      if (pass->depth.attachment && pass->depth.attachment->allocation == allocation &&
          pass->depth.store_action == WMTStoreActionStore)
        pass->depth.store_action = WMTStoreActionDontCare;
      if (pass->stencil.attachment && pass->stencil.attachment->allocation == allocation &&
          pass->stencil.store_action == WMTStoreActionStore)
        pass->stencil.store_action = WMTStoreActionDontCare;
    }
    if (discard_last_ != allocator_->encoder_last || discard_current_ != allocator_->encoder_current)
      discarded_.clear();
    discarded_.push_back(allocation);
    discard_last_ = allocator_->encoder_last;
    discard_current_ = allocator_->encoder_current;
  }

  void STDMETHODCALLTYPE
  BeginQuery(ID3D12QueryHeap *pHeap, D3D12_QUERY_TYPE Type, UINT Index) {
    // a bundle's query is the calling list's: it counts that list's draws, the bundle's among them
    if (Bundled() &&
        Record([=, heap = Com<ID3D12QueryHeap>(pHeap)](auto *l) { l->BeginQuery(heap.ptr(), Type, Index); }))
      return;
    OrderingPoint();
    // pipeline statistics count the draws and dispatches captured from here
    if (Type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS || Type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS1) {
      auto heap = static_cast<MTLD3D12QueryHeap *>(pHeap);
      StatisticsSum sum{};
      sum.query = Index;
      sum.size = heap->result_size / sizeof(uint64_t);
      sum.first_dispatch = indirect_dispatches_.size();
      sum.first_predicated = predicated_statistics_.size();
      sum.first_draw = draw_statistics_.size();
      active_statistics_.push_back({heap, sum});
      dirty_state_.set(DirtyState::GraphicsPipelineState);
      return;
    }
    // stream output statistics start at zero, and the stream's draws add to them
    if (Type >= D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 && Type <= D3D12_QUERY_TYPE_SO_STATISTICS_STREAM3) {
      auto heap = static_cast<MTLD3D12QueryHeap *>(pHeap);
      PreBlit();
      auto &fill = allocator_->EncodeBlitCommand<wmtcmd_blit_fillbuffer>();
      fill.type = WMTBlitCommandFillBuffer;
      fill.buffer = heap->results;
      fill.offset = Index * heap->result_size;
      fill.length = heap->result_size;
      fill.value = 0;
      so_statistics_[Type - D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0] = heap->results_address + Index * heap->result_size;
      return;
    }
    if (Type != D3D12_QUERY_TYPE_OCCLUSION && Type != D3D12_QUERY_TYPE_BINARY_OCCLUSION) {
      ERR("BeginQuery: query type ", Type, " is not implemented");
      return;
    }
    active_queries_.push_back({static_cast<MTLD3D12QueryHeap *>(pHeap), Index, (uint32_t)visibility_slots_.size()});
    CountVisibility();
    // a pipeline that emulates the depth bounds test has a variant for draws an occlusion query counts
    // (MTLD3D12GraphicsPipelineState::RenderTargets)
    if (pso_graphics_ && pso_graphics_->depth_bounds_emulated)
      dirty_state_.set(DirtyState::GraphicsPipelineState);
  };

  void STDMETHODCALLTYPE
  EndQuery(ID3D12QueryHeap *pHeap, D3D12_QUERY_TYPE Type, UINT Index) {
    if (Bundled() && Record([=, heap = Com<ID3D12QueryHeap>(pHeap)](auto *l) { l->EndQuery(heap.ptr(), Type, Index); }))
      return;
    OrderingPoint();
    auto heap = static_cast<MTLD3D12QueryHeap *>(pHeap);
    if (Type >= D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 && Type <= D3D12_QUERY_TYPE_SO_STATISTICS_STREAM3) {
      so_statistics_[Type - D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0] = 0;
      return;
    }
    if (Type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS || Type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS1) {
      auto query = std::find_if(active_statistics_.begin(), active_statistics_.end(), [&](auto &q) {
        return q.first == heap && q.second.query == Index;
      });
      if (query == active_statistics_.end()) {
        ERR("EndQuery: query type ", Type, " at ", Index, " was not begun");
        return;
      }
      auto sum = query->second;
      sum.dispatch_count = indirect_dispatches_.size() - sum.first_dispatch;
      sum.predicated_count = predicated_statistics_.size() - sum.first_predicated;
      sum.draw_count = draw_statistics_.size() - sum.first_draw;
      // sums run together at the next resolve, so a query ended again before one keeps only its latest result
      std::erase_if(statistics_sums_, [&](auto &s) { return s.first == heap && s.second.query == Index; });
      statistics_sums_.push_back({heap, sum});
      active_statistics_.erase(query);
      dirty_state_.set(DirtyState::GraphicsPipelineState);
      return;
    }
    if (Type == D3D12_QUERY_TYPE_TIMESTAMP) {
      // sampling one shapes the passes around it, which d3d12.timestampQueries=False leaves out: a profiler or a
      // resolution scaler then reads zeros
      if (!device_->timestamp_queries)
        return;
      // the clock "at the instant that the GPU is finished with all the preceding workload" (D3D11.3 20.4.3): the
      // pass that work ends with samples it as it ends, a render pass or the pass of the query before. an open pass
      // goes on, and its end is the clock, later than the query but not by more than its own work, as MoltenVK's
      // timestamps in a pass are: ending it for the query costs a pass. other work gets a blit encoder: Metal
      // samples only encoders that do something
      if (auto open = allocator_->encoder_current;
          open && (open->type == EncoderType::Render || open->type == EncoderType::Compute ||
                   open->type == EncoderType::Blit)) {
        open->stamps = new (allocator_->AllocateCommandData<Stamp>(1)) Stamp{heap, Index, open->stamps};
        return;
      }
      allocator_->InvalidateCurrentPass();
      auto pass = allocator_->encoder_last;
      if (!pass->stamps && pass->type != EncoderType::Render && pass->type != EncoderType::Clear) {
        PreBlit();
        pass = allocator_->encoder_current;
        auto [_, offset] = allocator_->AllocateGPUHeap(sizeof(uint32_t), sizeof(uint32_t));
        auto &fill = allocator_->EncodeBlitCommand<wmtcmd_blit_fillbuffer>();
        fill.type = WMTBlitCommandFillBuffer;
        fill.buffer = allocator_->gpu_heap_buffer_;
        fill.offset = offset;
        fill.length = sizeof(uint32_t);
        fill.value = 0;
        allocator_->InvalidateCurrentPass();
      }
      pass->stamps = new (allocator_->AllocateCommandData<Stamp>(1)) Stamp{heap, Index, pass->stamps};
      return;
    }
    auto query = std::find_if(active_queries_.begin(), active_queries_.end(), [&](auto &q) {
      return q.heap == heap && q.index == Index;
    });
    if (query == active_queries_.end()) {
      ERR("EndQuery: query type ", Type, " at ", Index, " was not begun");
      return;
    }
    auto count = (uint32_t)visibility_slots_.size() - query->first;
    // sums run together at the next resolve, so a query ended again before one keeps only its latest result
    std::erase_if(occlusion_sums_, [&](auto &sum) { return sum.first == heap && sum.second.query == Index; });
    occlusion_sums_.push_back({heap, {Index, query->first, count, Type == D3D12_QUERY_TYPE_BINARY_OCCLUSION}});
    active_queries_.erase(query);
    CountVisibility();
    if (pso_graphics_ && pso_graphics_->depth_bounds_emulated)
      dirty_state_.set(DirtyState::GraphicsPipelineState);
  };

  void STDMETHODCALLTYPE
  ResolveQueryData(
      ID3D12QueryHeap *pHeap, D3D12_QUERY_TYPE Type, UINT StartIndex, UINT QueryCount, ID3D12Resource *pDstBuffer,
      UINT64 AlignedDstBufferOffset
  ) {
    OrderingPoint();
    auto heap = static_cast<MTLD3D12QueryHeap *>(pHeap);
    auto resource = static_cast<MTLD3D12Resource *>(pDstBuffer);
    D3D12_QUERY_HEAP_TYPE heap_type;
    // Microsoft Learn, Queries: only timestamps are supported outside direct lists.
    switch (Type) {
    case D3D12_QUERY_TYPE_OCCLUSION:
    case D3D12_QUERY_TYPE_BINARY_OCCLUSION:
      heap_type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
      break;
    case D3D12_QUERY_TYPE_TIMESTAMP:
      heap_type = type_ == D3D12_COMMAND_LIST_TYPE_COPY ? D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP
                                                     : D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
      break;
    case D3D12_QUERY_TYPE_PIPELINE_STATISTICS:
      heap_type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
      break;
    case D3D12_QUERY_TYPE_PIPELINE_STATISTICS1:
      heap_type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS1;
      break;
    case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0:
    case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM1:
    case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM2:
    case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM3:
      heap_type = D3D12_QUERY_HEAP_TYPE_SO_STATISTICS;
      break;
    default:
      recording_error_ = E_INVALIDARG;
      return;
    }
    if (!heap || !resource || heap->desc.Type != heap_type ||
        (type_ != D3D12_COMMAND_LIST_TYPE_DIRECT &&
         !(Type == D3D12_QUERY_TYPE_TIMESTAMP &&
           (type_ == D3D12_COMMAND_LIST_TYPE_COMPUTE || type_ == D3D12_COMMAND_LIST_TYPE_COPY)))) {
      recording_error_ = E_INVALIDARG;
      return;
    }
    auto desc = resource->GetDesc();
    // ResolveQueryData validates ranges before recording; subtraction keeps overflowing inputs out of them.
    if (StartIndex > heap->desc.Count || QueryCount > heap->desc.Count - StartIndex ||
        AlignedDstBufferOffset % sizeof(uint64_t) || desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        AlignedDstBufferOffset > desc.Width || QueryCount > (desc.Width - AlignedDstBufferOffset) / heap->result_size) {
      recording_error_ = E_INVALIDARG;
      return;
    }
    if (!QueryCount)
      return;
    auto dst = resource->buffer->current()->buffer();
    SumOcclusion();
    SumStatistics();
    if (!heap->timestamps.empty()) {
      // no query sampled: the destination keeps what it holds, and every interval read from it is zero
      if (!device_->timestamp_queries)
        return;
      allocator_->InvalidateCurrentPass();
      auto resolve = allocator_->AllocatePass<ResolveTimestampsData>();
      resolve->type = EncoderType::ResolveTimestamps;
      resolve->heap = heap;
      D3D12_HEAP_PROPERTIES placed{};
      pDstBuffer->GetHeapProperties(&placed, nullptr);
      // custom cached pages are CPU-writable, but only READBACK forbids later GPU reads (D3D12_HEAP_TYPE)
      if (placed.Type == D3D12_HEAP_TYPE_READBACK ||
          (placed.Type == D3D12_HEAP_TYPE_CUSTOM && placed.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK)) {
        if (placed.Type == D3D12_HEAP_TYPE_READBACK)
          resolve->readback = pDstBuffer;
        resolve->memory = static_cast<MTLD3D12Resource *>(pDstBuffer)->buffer->current()->mappedMemory(0);
      }
      resolve->start = StartIndex;
      resolve->count = QueryCount;
      resolve->dst = dst;
      resolve->dst_offset = AlignedDstBufferOffset;
      allocator_->InvalidateCurrentPass();
      return;
    }
    // "Binary occlusion queries write 64-bits per query. The least significant bit is either 0 ... or 1 ... The rest
    // of the bits are 0" (ID3D12GraphicsCommandList::ResolveQueryData), also of a query that counted its samples
    if (Type == D3D12_QUERY_TYPE_BINARY_OCCLUSION) {
      StartComputePass();
      auto &cmd_setpso = allocator_->EncodeComputeCommand<wmtcmd_compute_setpso>();
      cmd_setpso.type = WMTComputeCommandSetPSO;
      cmd_setpso.pso = device_->binary_occlusion_resolve;
      cmd_setpso.threadgroup_size = {1, 1, 1};
      const std::pair<obj_handle_t, uint64_t> buffers[] = {
          {heap->results, uint64_t(StartIndex) * heap->result_size},
          {dst, AlignedDstBufferOffset},
      };
      for (uint8_t i = 0; i < std::size(buffers); i++) {
        auto &cmd = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
        cmd.type = WMTComputeCommandSetBuffer;
        cmd.buffer = buffers[i].first;
        cmd.offset = buffers[i].second;
        cmd.index = i;
      }
      auto &cmd_dispatch = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch>();
      cmd_dispatch.type = WMTComputeCommandDispatch;
      cmd_dispatch.size = {QueryCount, 1, 1};
      allocator_->InvalidateCurrentPass();
      return;
    }
    PreBlit();
    auto &cmd = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
    cmd.type = WMTBlitCommandCopyFromBufferToBuffer;
    cmd.src = heap->results;
    cmd.src_offset = uint64_t(StartIndex) * heap->result_size;
    cmd.dst = dst;
    cmd.dst_offset = AlignedDstBufferOffset;
    cmd.copy_length = uint64_t(QueryCount) * heap->result_size;
  };

  // the region's work follows the predicate as it is when the region starts
  void STDMETHODCALLTYPE SetPredication(ID3D12Resource *pBuffer, UINT64 AlignedBufferOffset, D3D12_PREDICATION_OP Op) {
    OrderingPoint();
    EndPredication();
    if (!pBuffer)
      return;
    // the kernel of a region that begins in a render pass whose draws wrote nothing but its targets goes before the
    // pass, which keeps going: nothing in it can have written the predicate, and the pass is ordered after the kernel
    // as after the work before it (OrderingPoint). 100 regions of a frame were 100 passes, each loading and storing
    // its targets
    auto render = allocator_->encoder_current;
    bool before = render && render->type == EncoderType::Render && render != uav_draw_pass_ && HoistsPredicates();
    if (before)
      allocator_->encoder_current = nullptr;
    StartComputePass();
    auto [mapped, offset] = allocator_->AllocateGPUHeap(sizeof(PredicationRegion), alignof(PredicationRegion));
    if (allocator_->records_) {
      allocator_->RecordGPUHeap(mapped, sizeof(PredicationRegion), "predicate header", offsetof(PredicationRegion, skips),
                                sizeof(uint32_t));
      predication_pass_ = allocator_->encoder_current;
    }
    predication_region_ = new (mapped) PredicationRegion{};
    predication_region_->skip_if_zero = Op == D3D12_PREDICATION_OP_EQUAL_ZERO;
    auto &cmd_setpso = allocator_->EncodeComputeCommand<wmtcmd_compute_setpso>();
    cmd_setpso.type = WMTComputeCommandSetPSO;
    cmd_setpso.pso = device_->predicate;
    cmd_setpso.threadgroup_size = {1, 1, 1};
    const std::pair<obj_handle_t, uint64_t> buffers[] = {
        {static_cast<MTLD3D12Resource *>(pBuffer)->buffer->current()->buffer(), AlignedBufferOffset},
        {allocator_->gpu_heap_buffer_, offset},
    };
    for (uint8_t i = 0; i < std::size(buffers); i++) {
      auto &cmd = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
      cmd.type = WMTComputeCommandSetBuffer;
      cmd.buffer = buffers[i].first;
      cmd.offset = buffers[i].second;
      cmd.index = i;
    }
    auto &cmd_dispatch = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch>();
    cmd_dispatch.type = WMTComputeCommandDispatch;
    cmd_dispatch.size = {1, 1, 1};
    allocator_->InvalidateCurrentPass();
    if (before) {
      allocator_->encoder_current = render;
      render->late = false;
    }
  };

  static bool
  HoistsPredicates() {
    static const bool hoists = env::getEnvVar("MULLION_D3D12_HOIST_PREDICATES") != "0";
    return hoists;
  }

  // a marker is an instant, between passes or inside one, which no pass is named by
  void STDMETHODCALLTYPE SetMarker(UINT Metadata, const void *data, UINT size) {};

  void STDMETHODCALLTYPE
  BeginEvent(UINT Metadata, const void *data, UINT size) {
    if (!diag::events)
      return;
    auto bytes = static_cast<const char *>(data);
    if (Record([=, blob = std::vector<char>(bytes, bytes + size)](auto *l) { l->BeginEvent(Metadata, blob.data(), size); }))
      return;
    std::string path = events_.empty() ? "" : events_.back();
    diag::PushEvent(path, diag::EventName(Metadata, data, size));
    auto copy = static_cast<char *>(allocator_->AllocateCPUHeap(path.size() + 1, 1));
    allocator_->event = events_.emplace_back(strcpy(copy, path.c_str()));
  };

  void STDMETHODCALLTYPE
  EndEvent() {
    if (!diag::events || Record([](auto *l) { l->EndEvent(); }) || events_.empty())
      return;
    events_.pop_back();
    allocator_->event = events_.empty() ? nullptr : events_.back();
  };

  // the paths of the open events, in the allocator's memory as the passes that carry them are; a pass that goes on
  // past an event's start or end keeps the path it began in
  std::vector<const char *> events_;

  void STDMETHODCALLTYPE ExecuteIndirect(
      ID3D12CommandSignature *pCommandSignature, UINT MaxCommandCount, ID3D12Resource *pArgBuffer,
      UINT64 ArgBufferOffset, ID3D12Resource *pCountBuffer, UINT64 CountBufferOffset
  ) {
    if (Bundled() && Record([=, sig = Com<ID3D12CommandSignature>(pCommandSignature), args = Com<ID3D12Resource>(pArgBuffer), count = Com<ID3D12Resource>(pCountBuffer)](auto *l) { l->ExecuteIndirect(sig.ptr(), MaxCommandCount, args.ptr(), ArgBufferOffset, count.ptr(), CountBufferOffset); }))
      return;
    auto sig = static_cast<MTLD3D12CommandSignature *>(pCommandSignature);
    auto arg_buffer = static_cast<MTLD3D12Resource *>(pArgBuffer);
    if (!arg_buffer || !arg_buffer->buffer)
      return;
    auto ArgBufferAddress = arg_buffer->buffer->current()->gpuAddress() + ArgBufferOffset;
    uint64_t CountBufferAddress = 0;
    if (auto count_buffer = static_cast<MTLD3D12Resource *>(pCountBuffer)) {
      if (!count_buffer->buffer)
        return;
      CountBufferAddress = count_buffer->buffer->current()->gpuAddress() + CountBufferOffset;
    }
    bool rays = sig->CommandType == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS;
    if (!MaxCommandCount) {
      ResetIndirectState(sig, rays || sig->CommandType == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH);
      return;
    }
    if (rays || sig->CommandType == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH) {
      IndirectComputeCommandData *cmd;
      bool parts = !rays && pso_compute_ && pso_compute_->groups_work_together;
      uint32_t ray_flags = 0;
      if (rays) {
        // the resolver runs first, in the pass of the dispatches; without a pipeline it has no commands
        if (!allocator_->encoder_current || allocator_->encoder_current->type != EncoderType::Compute)
          StartComputePass();
        cmd = allocator_->EncodeComputeResolver(sig, MaxCommandCount);
        if (!BeginRays(ray_flags)) {
          cmd->max_count = 0;
          return;
        }
      } else {
        if (!PreDispatch(sig->UpdateRootArguments))
          return;
        // commands that only dispatch, all of them, are Metal's own indirect dispatches of the arguments where they
        // are: no resolver writing an indirect command buffer, and no barrier between the two
        if (!parts && !sig->UpdateRootArguments && !CountBufferAddress && !predication_region_ &&
            active_statistics_.empty()) {
          for (UINT i = 0; i < MaxCommandCount; i++) {
            auto &dispatch = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch_indirect>();
            dispatch.type = WMTComputeCommandDispatchIndirect;
            dispatch.indirect_args_buffer = arg_buffer->buffer->current()->buffer();
            dispatch.indirect_args_offset = ArgBufferOffset + uint64_t(i) * sig->ByteStride;
          }
          Isolate(0, 0, 0);
          return;
        }
        cmd = parts ? allocator_->EncodeComputeResolver(sig, MaxCommandCount)
                    : allocator_->EncodeIndirectComputeCommand(sig, pso_compute_.ptr(), MaxCommandCount);
        if (!active_statistics_.empty()) {
          // the resolver's threads write a command each
          size_t size = std::max<size_t>(MaxCommandCount, 1) * sizeof(uint64_t);
          auto [mapped, offset] = allocator_->AllocateGPUHeap(size, alignof(uint64_t));
          if (allocator_->records_)
            allocator_->RecordGPUHeap(mapped, size, "indirect invocations", 0, size);
          cmd->statistics = allocator_->gpu_heap_buffer_address_ + offset;
          indirect_dispatches_.push_back({cmd->statistics, MaxCommandCount});
        }
      }
      cmd->max_count_buffer = CountBufferAddress;
      PredicateIndirect(cmd);
      cmd->argument_buffer = ArgBufferAddress;
      uint64_t root_arguments = 0, static_samplers = 0;

      if (sig->UpdateRootArguments) {
        cmd->rootsig_base = EncodeRootArgument(rootsig_compute_.ptr(), rootarg_compute_staging_);
        cmd->rootsig_base += allocator_->gpu_heap_buffer_address_;
        root_arguments = ReserveRootArguments(rootsig_compute_.ptr(), MaxCommandCount);
        cmd->rootsig_qwords = root_arguments;
        cmd->rootsig_qwords += allocator_->gpu_heap_buffer_address_;
        cmd->rootsig_qwords_stride = rootsig_compute_->UploadQwords;
        static_samplers = EncodeStaticSamplers(rootsig_compute_.ptr());
        cmd->static_samplers = static_samplers + allocator_->gpu_heap_buffer_address_;
        ResetIndirectState(sig, true);
      }
      if (parts) {
        auto [mapped, at] = allocator_->AllocateGPUHeap(MaxCommandCount * sizeof(D3D12_DISPATCH_ARGUMENTS),
                                                       alignof(D3D12_DISPATCH_ARGUMENTS));
        cmd->dispatches = allocator_->gpu_heap_buffer_address_ + at;
        for (UINT i = 0; i < MaxCommandCount; i++) {
          auto pass = static_cast<ComputePartsEncoderData *>(StartComputePass(true));
          PreDispatch(sig->UpdateRootArguments);
          if (sig->UpdateRootArguments) {
            const std::pair<uint64_t, uint8_t> bindings[] = {
                {root_arguments + i * rootsig_compute_->UploadQwords * sizeof(uint64_t), SM50_BINDING_INDEX_ROOT_ARGUMENTS},
                {static_samplers, SM50_BINDING_INDEX_STATIC_SAMPLERS}};
            for (auto [offset, index] : bindings) {
              auto &binding = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
              binding.type = WMTComputeCommandSetBuffer;
              binding.buffer = allocator_->gpu_heap_buffer_;
              binding.offset = offset;
              binding.index = index;
            }
          }
          pass->at_once = pso_compute_->groups_at_once;
          pass->indirect = static_cast<const D3D12_DISPATCH_ARGUMENTS *>(mapped) + i;
          pass->type = EncoderType::ComputeParts;
        }
        allocator_->InvalidateCurrentPass();
      } else if (rays) {
        // each command is a dispatch of the kernel, of the threadgroups the resolver counts for it
        auto [records, at] = allocator_->AllocateGPUHeap(MaxCommandCount * sizeof(IndirectRays), alignof(IndirectRays));
        cmd->rays = allocator_->gpu_heap_buffer_address_ + at;
        // the resolver writes the rest of each command's dispatch
        for (UINT i = 0; i < MaxCommandCount; i++)
          if (records)
            static_cast<IndirectRays *>(records)[i].dispatch.atomic_locks = device_->GetAtomicLocks();
        cmd->ray_flags = ray_flags;
        cmd->tgsize_x = device_->GetSIMDWidth();
        for (UINT i = 0; i < MaxCommandCount; i++, at += sizeof(IndirectRays)) {
          if (sig->UpdateRootArguments) {
            auto &arguments = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
            arguments.type = WMTComputeCommandSetBuffer;
            arguments.buffer = allocator_->gpu_heap_buffer_;
            arguments.offset = root_arguments + i * rootsig_compute_->UploadQwords * sizeof(uint64_t);
            arguments.index = SM50_BINDING_INDEX_ROOT_ARGUMENTS;
          }
          BindRays(at);
          auto &threads = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch_indirect>();
          threads.type = WMTComputeCommandDispatchIndirect;
          threads.indirect_args_buffer = allocator_->gpu_heap_buffer_;
          threads.indirect_args_offset = at + offsetof(IndirectRays, threadgroups);
        }
      }
      Isolate(0, 0, 0);
      return;
    }
    WMTPrimitiveType primitive_type = WMTPrimitiveTypeTriangle;
    uint32_t cp_count = 0;
    // a mesh shader pipeline has no input assembler
    bool dispatch_mesh = sig->CommandType == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
    if (dispatch_mesh != (pso_graphics_ && pso_graphics_->mesh_shader))
      return;
    if (!dispatch_mesh && !to_metal_primitive_type(topology_, primitive_type, cp_count))
      return;
    // a tessellated command binds every buffer itself, as does a geometry pipeline's and a mesh shader pipeline's
    bool geometry = pso_graphics_ && Geometry();
    bool tessellation = pso_graphics_ && (pso_graphics_->threads_per_patch || geometry || dispatch_mesh);
    bool encode_binding = tessellation || sig->UpdateRootArguments || sig->UpdateIndexBuffer || sig->UpdateVertexBuffers;
    DrawCallStatus status = PreDraw(encode_binding);
    if (status == DrawCallStatus::Invalid)
      return;
    if (status != DrawCallStatus::Ordinary) {
      IMPLEMENT_ME // TODO: (potential) emulated pipeline
    }

    // a few commands that only draw, with no count for the GPU to read and nothing a resolver counts, are Metal's own
    // indirect draws of the arguments where they are (they are D3D12_DRAW_ARGUMENTS, as in DrawInstanced): no
    // resolver writing an indirect command buffer in a compute pass before this one, and the buffers stay bound. an
    // indexed command stays with the resolver, which keeps its indices inside the view (D3D11.3 8.19.2), and Metal
    // takes the arguments at a multiple of four bytes only
    if (sig->CommandType == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW && !encode_binding && !CountBufferAddress &&
        !predication_region_ && active_statistics_.empty() && !diag::errors &&
        MaxCommandCount <= kIndirectDrawsInPlace && !(ArgBufferOffset % sizeof(UINT))) {
      for (UINT i = 0; i < MaxCommandCount; i++) {
        auto &draw = allocator_->EncodeRenderCommand<wmtcmd_render_draw_indirect>();
        draw.type = WMTRenderCommandDrawIndirect;
        draw.primitive_type = primitive_type;
        draw.indirect_args_buffer = arg_buffer->buffer->current()->buffer();
        draw.indirect_args_offset = ArgBufferOffset + uint64_t(i) * sig->ByteStride;
      }
      return;
    }

    uint64_t statistics = 0, pixel_statistics = 0;
    if (!active_statistics_.empty() && MaxCommandCount) {
      auto size = (uint64_t(MaxCommandCount) + 1) * sizeof(PipelineStatisticsResult);
      auto [mapped, offset] = allocator_->AllocateGPUHeap(size, alignof(PipelineStatisticsResult));
      if (allocator_->records_)
        allocator_->RecordGPUHeap(mapped, size, "indirect draw statistics", 0, size);
      statistics = allocator_->gpu_heap_buffer_address_ + offset;
      pixel_statistics = statistics + uint64_t(MaxCommandCount) * sizeof(PipelineStatisticsResult);
      for (uint64_t i = 0; i <= MaxCommandCount; i++)
        draw_statistics_.push_back(statistics + i * sizeof(PipelineStatisticsResult));
      EncodeBuffer(WMTRenderCommandSetFragmentBuffer, pixel_statistics - allocator_->gpu_heap_buffer_address_,
                   SM50_BINDING_INDEX_PIPELINE_STATISTICS);
    }
    bool statistics_cuts = false;
    if (statistics && sig->CommandType == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED) {
      auto [first, step] = get_primitive_vertex_count(topology_);
      statistics_cuts = step != first && pso_graphics_->strip_cut != D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    }
    // a count buffer and predication change the count on the GPU, and an indexed signature's extras add to it
    bool runs_max_count =
        !CountBufferAddress && !predication_region_ && sig->CommandType != D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    auto cmd = allocator_->EncodeIndirectRenderCommand(sig, pso_graphics_.ptr(), Pipeline(), MaxCommandCount,
                                                       tessellation, statistics_cuts, runs_max_count);
    ForgetBuffers();
    cmd->statistics = statistics;
    cmd->pixel_statistics = pixel_statistics;
    cmd->depth_ranges = depth_ranges_;
    cmd->raster_scale = raster_scale_;
    if (statistics) {
      auto [first, step] = get_primitive_vertex_count(topology_);
      cmd->statistics_first = dispatch_mesh ? 0 : first;
      cmd->statistics_step = step;
      cmd->statistics_cut = pso_graphics_->strip_cut == D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF
                                ? D3D12_16BIT_INDEX_STRIP_CUT_VALUE
                                : D3D12_32BIT_INDEX_STRIP_CUT_VALUE;
      cmd->statistics_cuts = statistics_cuts;
    }
    cmd->max_count_buffer = CountBufferAddress;
    PredicateIndirect(cmd);
    cmd->argument_buffer = ArgBufferAddress;
    cmd->primitive_type = primitive_type;
    cmd->index_buffer = index_buffer_address;
    cmd->index_buffer_format = index_type == WMTIndexTypeUInt32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    cmd->index_buffer_size = index_view_.SizeInBytes;
    if (tessellation && !dispatch_mesh) {
      // for indexed draws the bound view, or an empty one when the commands set their own; an empty view otherwise
      auto [Mapped, Offset] = allocator_->AllocateGPUHeap(sizeof(TessellationInputs), 16);
      if (allocator_->records_)
        allocator_->RecordGPUHeap(Mapped, sizeof(TessellationInputs), "tessellation inputs");
      *reinterpret_cast<TessellationInputs *>(Mapped) = {
          sig->CommandType != D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED ? D3D12_INDEX_BUFFER_VIEW{}
          : sig->UpdateIndexBuffer                                        ? IndexedView({})
                                                                          : IndexedView(index_view_),
          cp_count
      };
      cmd->index_buffer_view = allocator_->gpu_heap_buffer_address_ + Offset;
      cmd->control_points = cp_count;
    }
    if (geometry) {
      std::tie(cmd->geometry_threads, cmd->geometry_increment) = get_gs_vertex_count(topology_, pso_graphics_->vertex_registers);
      cmd->geometry_max_warps = std::min<uint64_t>(
          get_gs_max_object_threadgroups(
              topology_, pso_graphics_->vertex_registers, pso_graphics_->mesh_instances, -1ull,
              device_->mesh_bytes_per_draw, pso_graphics_->gs_mesh_invocations
          ),
          UINT32_MAX
      );
    }
    if (!encode_binding)
      return;
    // a command has root arguments or a vertex buffer table of its own where the signature changes them: room for
    // each, which the resolver fills from the list's one copy for the commands it runs (rootsig_base, vertex_base).
    // the commands of a signature that does not read the list's copy, a stride of nothing apart
    cmd->rootsig_qwords = EncodeRootArgument(rootsig_graphics_.ptr(), rootarg_graphics_staging_);
    cmd->rootsig_qwords += allocator_->gpu_heap_buffer_address_;
    if (sig->UpdateRootArguments) {
      cmd->rootsig_base = cmd->rootsig_qwords;
      cmd->rootsig_qwords = ReserveRootArguments(rootsig_graphics_.ptr(), MaxCommandCount);
      cmd->rootsig_qwords += allocator_->gpu_heap_buffer_address_;
      cmd->rootsig_qwords_stride = rootsig_graphics_->UploadQwords;
    }
    cmd->static_samplers = EncodeStaticSamplers(rootsig_graphics_.ptr());
    cmd->static_samplers += allocator_->gpu_heap_buffer_address_;
    auto [VBOffset, VBStride] = PopulateVertexBufferTable();
    cmd->vertex_buffer = allocator_->gpu_heap_buffer_address_ + VBOffset;
    if (sig->UpdateVertexBuffers && VBStride) {
      cmd->vertex_base = cmd->vertex_buffer;
      auto [tables, at] = allocator_->AllocateGPUHeap(VBStride * MaxCommandCount, 16);
      cmd->vertex_buffer = allocator_->gpu_heap_buffer_address_ + at;
      cmd->vertex_argbuf_stride = VBStride;
    }
    ResetIndirectState(sig, false);
    if (!cmd->draws)
      return;
    // stream output runs each command as a direct draw (StreamOut), one after another in the list's scratch area
    if (pso_graphics_->stream_output) {
      if (!allocator_->so_scratch_size_) {
        auto budget = device_->so_scratch_budget.load();
        auto [Scratch, ScratchOffset] = allocator_->AllocateGPUHeap(budget, 16);
        // a list whose scratch no heap has room for fails when it is closed
        if (!Scratch)
          return;
        allocator_->so_scratch_offset_ = ScratchOffset;
        allocator_->so_scratch_size_ = budget;
      }
      auto [Targets, TargetsOffset] = allocator_->AllocateGPUHeap(MaxCommandCount * sizeof(SM50_STREAM_OUTPUT_TARGETS), 16);
      if (!Targets)
        return;
      auto targets = StreamOutputTargets();
      targets.scratch = allocator_->gpu_heap_buffer_address_ + allocator_->so_scratch_offset_;
      std::fill_n(static_cast<SM50_STREAM_OUTPUT_TARGETS *>(Targets), MaxCommandCount, targets);
      cmd->so_targets = allocator_->gpu_heap_buffer_address_ + TargetsOffset;
      cmd->so_scratch_size = allocator_->so_scratch_size_;
      cmd->so_invocations = cmd->geometry_threads * pso_graphics_->gs_instances;
    }
    // a GPU without mesh commands in indirect command buffers: every command is a draw of its own, which binds what
    // the command's stages read and has the threadgroups the resolver left for it (none past the count)
    // each address where it is in the buffer over the arguments it was made in: a longer buffer may have followed
    // since (GrowArguments), and what the resolver wrote is read through the buffer it wrote through
    auto draws = allocator_->ArgumentsAt(cmd->draws), roots = allocator_->ArgumentsAt(cmd->rootsig_qwords),
         samplers = allocator_->ArgumentsAt(cmd->static_samplers), vertices = allocator_->ArgumentsAt(cmd->vertex_buffer),
         counts = allocator_->ArgumentsAt(cmd->statistics), targets = allocator_->ArgumentsAt(cmd->so_targets);
    WMTSize object{cmd->geometry_threads, 1, 1}, mesh{pso_graphics_->gs_mesh_invocations, 1, 1};
    // a mesh function alone takes the warps as mesh threadgroups, and what the object stage reads (MeshOnly)
    bool mesh_only = geometry && MeshOnly();
    auto stage = mesh_only ? WMTRenderCommandSetMeshBuffer : WMTRenderCommandSetObjectBuffer;
    if (mesh_only)
      std::swap(object, mesh);
    if (dispatch_mesh) {
      object = {cmd->object_threads[0], cmd->object_threads[1], cmd->object_threads[2]};
      mesh = {cmd->mesh_threads[0], cmd->mesh_threads[1], cmd->mesh_threads[2]};
    } else if (cmd->threads_per_patch) {
      object = {cmd->threads_per_patch, 32 / cmd->threads_per_patch, 1};
      mesh = {32, 1, 1};
    }
    // a geometry draw's object stage adds its origin (SM50_BINDING_INDEX_DRAW_ORIGIN), none here
    if (!dispatch_mesh && !cmd->threads_per_patch) {
      auto [origin, origin_offset] = allocator_->AllocateGPUHeap(2 * sizeof(uint32_t), 8);
      memset(origin, 0, 2 * sizeof(uint32_t));
      EncodeBuffer(stage, origin_offset, SM50_BINDING_INDEX_DRAW_ORIGIN);
    }
    for (UINT i = 0; i < MaxCommandCount; i++) {
      auto at = draws.second + i * sizeof(IndirectMeshDraw);
      auto root = roots.second + i * cmd->rootsig_qwords_stride * sizeof(uint64_t);
      for (auto stage : {WMTRenderCommandSetObjectBuffer, WMTRenderCommandSetMeshBuffer, WMTRenderCommandSetFragmentBuffer}) {
        EncodeBuffer(stage, root, SM50_BINDING_INDEX_ROOT_ARGUMENTS, roots.first);
        EncodeBuffer(stage, samplers.second, SM50_BINDING_INDEX_STATIC_SAMPLERS, samplers.first);
      }
      if (!dispatch_mesh) {
        EncodeBuffer(
            stage, vertices.second + i * cmd->vertex_argbuf_stride, SM50_BINDING_INDEX_VERTEX_BUFFER, vertices.first
        );
        EncodeBuffer(stage, at + offsetof(IndirectMeshDraw, view), SM50_BINDING_INDEX_INDEX_BUFFER, draws.first);
        if (cmd->threads_per_patch)
          EncodeBuffer(
              WMTRenderCommandSetObjectBuffer, at + offsetof(IndirectMeshDraw, control_points),
              SM50_BINDING_INDEX_PATCH_SIZE, draws.first
          );
        EncodeBuffer(stage, at + offsetof(IndirectMeshDraw, arguments), SM50_BINDING_INDEX_DRAW_ARGUMENTS, draws.first);
      }
      // a geometry stage adds its counts to its command's, as a direct draw's does (CountDraw)
      if (cmd->statistics)
        EncodeBuffer(WMTRenderCommandSetMeshBuffer, counts.second + i * sizeof(PipelineStatisticsResult),
                     SM50_BINDING_INDEX_PIPELINE_STATISTICS, counts.first);
      auto draw = [&] {
        auto &draw = allocator_->EncodeRenderCommand<wmtcmd_render_draw_meshthreadgroups_indirect>();
        draw.type = WMTRenderCommandDrawMeshThreadgroupsIndirect;
        draw.indirect_args_buffer = draws.first;
        draw.indirect_args_offset = at + offsetof(IndirectMeshDraw, threadgroups);
        draw.object_threadgroup_size = object;
        draw.mesh_threadgroup_size = mesh;
      };
      if (!cmd->so_targets) {
        draw();
        continue;
      }
      StreamOut(targets.second + i * sizeof(SM50_STREAM_OUTPUT_TARGETS), allocator_->so_scratch_offset_, draw, [&] {
        auto &zero = allocator_->EncodeRenderCommand<wmtcmd_render_draw_indirect>();
        zero.type = WMTRenderCommandDrawIndirect;
        zero.primitive_type = WMTPrimitiveTypePoint;
        zero.indirect_args_buffer = draws.first;
        zero.indirect_args_offset = at + offsetof(IndirectMeshDraw, zero);
      }, SinglePass());
    }
  };

  void ResetIndirectState(MTLD3D12CommandSignature *sig, bool compute) {
    auto rootargs = compute ? rootarg_compute_staging_ : rootarg_graphics_staging_;
    for (auto [offset, count] : sig->ResetRootDwords)
      std::fill_n(reinterpret_cast<uint32_t *>(rootargs) + offset, count, 0u);
    auto dirty = compute ? DirtyState::ComputeRootArguments : DirtyState::GraphicsRootArguments;
    if (auto rootsig = compute ? rootsig_compute_.ptr() : rootsig_graphics_.ptr())
      NullRootViews(rootsig, rootargs, dirty);
    RootArgumentsChanged(dirty);
    if (compute)
      return;
    for (unsigned slot = 0; slot < vertex_buffers_.size(); slot++)
      if (sig->ResetVertexBuffers & (1u << slot))
        vertex_buffers_[slot] = {};
    if (sig->UpdateIndexBuffer)
      IASetIndexBuffer(nullptr);
    dirty_state_.set(DirtyState::VertexBuffer);
  }

  void STDMETHODCALLTYPE
  AtomicCopyBufferUINT(
      ID3D12Resource *pDstBuffer, UINT64 DstOffset, ID3D12Resource *pSrcBuffer, UINT64 SrcOffset, UINT Dependencies,
      ID3D12Resource *const *ppDependentResources, const D3D12_SUBRESOURCE_RANGE_UINT64 *pDependentSubresourceRanges
  ) {
    // the element is copied whole, after the list's work before it, which is where its dependents were written
    CopyBufferRegion(pDstBuffer, DstOffset, pSrcBuffer, SrcOffset, sizeof(UINT));
  }

  void STDMETHODCALLTYPE
  AtomicCopyBufferUINT64(
      ID3D12Resource *pDstBuffer, UINT64 DstOffset, ID3D12Resource *pSrcBuffer, UINT64 SrcOffset, UINT Dependencies,
      ID3D12Resource *const *ppDependentResources, const D3D12_SUBRESOURCE_RANGE_UINT64 *pDependentSubresourceRanges
  ) {
    CopyBufferRegion(pDstBuffer, DstOffset, pSrcBuffer, SrcOffset, sizeof(UINT64));
  }

  void STDMETHODCALLTYPE
  OMSetDepthBounds(FLOAT Min, FLOAT Max) {
    if (Bundled() && Record([=](auto *l) { l->OMSetDepthBounds(Min, Max); }))
      return;
    float bounds[2] = {std::isnan(Min) ? 0.0f : Min, std::isnan(Max) ? 0.0f : Max};
    depth_bounds_stale_ |= bounds[0] != depth_bounds_[0] || bounds[1] != depth_bounds_[1];
    depth_bounds_[0] = bounds[0];
    depth_bounds_[1] = bounds[1];
    dirty_state_.set(DirtyState::DepthBounds);
  }

  void STDMETHODCALLTYPE
  SetSamplePositions(UINT NumSamplesPerPixel, UINT NumPixels, D3D12_SAMPLE_POSITION *pSamplePositions) {
    // one pattern for every pixel (tier 1), or none to take the standard one back; other calls are dropped
    bool reset = !NumSamplesPerPixel && !NumPixels;
    if (!reset && (NumPixels != 1 || !std::has_single_bit(NumSamplesPerPixel) ||
                   NumSamplesPerPixel > sample_positions_.size()))
      return;
    auto positions = std::span(pSamplePositions, reset ? 0 : NumSamplesPerPixel);
    if (programmed_samples_ == positions.size() &&
        std::ranges::equal(positions, std::span(sample_positions_).first(positions.size()), [](auto &a, auto &b) {
          return a.X == b.x && a.Y == b.y;
        }))
      return;
    programmed_samples_ = positions.size();
    std::ranges::transform(positions, sample_positions_.begin(), [](auto &p) { return WMTSamplePosition{p.X, p.Y}; });
    // a render pass has one pattern
    allocator_->InvalidateCurrentPass();
  }

  // ResolveSubresourceRegion's sampler feedback modes, between a map and an R8_UINT texture or, for a MinMip map
  // that is no array, a buffer of rows. offsets and `pRect`, the rectangle's size, count mip regions of the transcoded
  // mip; UINT_MAX is every subresource (Sampler Feedback spec, "Transcoding")
  void
  TranscodeFeedback(
      bool Encode, MTLD3D12Resource *pMap, UINT MapSubresource, POINT MapOffset, MTLD3D12Resource *pOther,
      UINT OtherSubresource, POINT Offset, const D3D12_RECT *pRect
  ) {
    if (!pMap->feedback || (pOther->texture && pOther->GetDesc().Format != DXGI_FORMAT_R8_UINT)) {
      recording_error_ = E_INVALIDARG;
      return;
    }
    auto &desc = *pMap->feedback;
    auto &map = pMap->texture;
    auto &other = pOther->texture;
    bool min_mip = desc.Format == DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE;
    UINT other_mips = other ? other->miplevelCount() : 1;
    // a MinMip map's own subresource is always UINT_MAX: the slice to transcode is the other resource's, which has
    // one mip ("specific semantics are required for MIN_MIP type feedback maps")
    if (min_mip && OtherSubresource != UINT_MAX)
      MapSubresource = OtherSubresource / other_mips * desc.MipLevels;
    bool all = MapSubresource == UINT_MAX;
    UINT first = all || min_mip ? 0 : MapSubresource % desc.MipLevels;
    UINT levels = all && !min_mip ? desc.MipLevels : 1;
    UINT slice = all ? 0 : MapSubresource / desc.MipLevels;
    UINT slices = all ? desc.DepthOrArraySize : 1;
    auto map_view = map->createView({
        .format = WMTPixelFormatR32Uint,
        .type = WMTTextureType2DArray,
        .firstArraySlice = slice,
        .arraySize = slices,
    });
    for (UINT level = first; level < first + levels; level++) {
      UINT other_level = OtherSubresource == UINT_MAX ? level : OtherSubresource % other_mips;
      TextureViewKey view{};
      if (other)
        view = other->createView({
            .format = WMTPixelFormatR8Uint,
            .type = WMTTextureType2DArray,
            .firstMiplevel = other_level,
            .firstArraySlice = OtherSubresource == UINT_MAX ? 0 : OtherSubresource / other_mips,
            .arraySize = slices,
        });
      // what the rectangle has of the mip's regions, and of the texture's texels
      auto size = [&](UINT regions, UINT texels, LONG map_offset, LONG offset, LONG asked) {
        regions = std::max(regions >> level, 1u);
        LONG fits = (LONG)regions - map_offset;
        if (other)
          fits = std::min(fits, (LONG)std::max(texels >> other_level, 1u) - offset);
        return (UINT)std::max(pRect ? std::min(fits, asked) : fits, (LONG)0);
      };
      UINT width = size(
          map->width(), other ? other->width() : 0, MapOffset.x, Offset.x, pRect ? pRect->right - pRect->left : 0
      );
      UINT height = size(
          map->height(), other ? other->height() : 0, MapOffset.y, Offset.y, pRect ? pRect->bottom - pRect->top : 0
      );
      if (!width || !height)
        continue;
      allocator_->transcode_feedback_.run(
          Encode, map, map_view, other, view, pOther->buffer,
          {
              .map_offset = {(UINT)MapOffset.x, (UINT)MapOffset.y},
              .offset = {(UINT)Offset.x, (UINT)Offset.y},
              .level = level,
              .mips = min_mip ? desc.MipLevels : 1u,
              .min_mip = min_mip,
              .pitch = map->width(),
          },
          {width, height, slices}
      );
    }
  }

  void STDMETHODCALLTYPE
  ResolveSubresourceRegion(
      ID3D12Resource *pDstResource, UINT DstSubresource, UINT DstX, UINT DstY, ID3D12Resource *pSrcResource,
      UINT SrcSubresource, D3D12_RECT *pSrcRect, DXGI_FORMAT Format, D3D12_RESOLVE_MODE ResolveMode
  ) {
    // a source with one sample has nothing to resolve: its texels are the result, so it is a copy of the rectangle
    // (Metal resolves only multisampled attachments). CopyTextureRegion opens the predication of its own
    if (ResolveMode != D3D12_RESOLVE_MODE_ENCODE_SAMPLER_FEEDBACK &&
        ResolveMode != D3D12_RESOLVE_MODE_DECODE_SAMPLER_FEEDBACK && pDstResource && pSrcResource && pDstResource != pSrcResource) {
      auto *single = static_cast<MTLD3D12Resource *>(pSrcResource);
      if (single->texture && single->texture->sampleCount() <= 1 &&
          static_cast<MTLD3D12Resource *>(pDstResource)->texture) {
        D3D12_TEXTURE_COPY_LOCATION to{pDstResource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = DstSubresource}};
        D3D12_TEXTURE_COPY_LOCATION from{pSrcResource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = SrcSubresource}};
        D3D12_BOX box{};
        if (pSrcRect)
          box = {(UINT)pSrcRect->left, (UINT)pSrcRect->top, 0, (UINT)pSrcRect->right, (UINT)pSrcRect->bottom, 1};
        CopyTextureRegion(&to, DstX, DstY, 0, &from, pSrcRect ? &box : nullptr);
        return;
      }
    }
    PredicatedPasses predicated(this);
    if (ResolveMode == D3D12_RESOLVE_MODE_ENCODE_SAMPLER_FEEDBACK ||
        ResolveMode == D3D12_RESOLVE_MODE_DECODE_SAMPLER_FEEDBACK) {
      bool encode = ResolveMode == D3D12_RESOLVE_MODE_ENCODE_SAMPLER_FEEDBACK;
      POINT from = pSrcRect ? POINT{pSrcRect->left, pSrcRect->top} : POINT{}, to = {(LONG)DstX, (LONG)DstY};
      TranscodeFeedback(
          encode, static_cast<MTLD3D12Resource *>(encode ? pDstResource : pSrcResource),
          encode ? DstSubresource : SrcSubresource, encode ? to : from,
          static_cast<MTLD3D12Resource *>(encode ? pSrcResource : pDstResource),
          encode ? SrcSubresource : DstSubresource, encode ? from : to, pSrcRect
      );
      return;
    }
    auto *dst = static_cast<MTLD3D12Resource *>(pDstResource), *src = static_cast<MTLD3D12Resource *>(pSrcResource);
    if (!dst->texture || !src->texture || (dst == src && ResolveMode == D3D12_RESOLVE_MODE_DECOMPRESS))
      return;
    // a render pass's attachment, which needs no barrier for EndRenderPass's resolves
    UsedTexture(src->texture.ptr());
    UsedTexture(dst->texture.ptr());
    MTL_DXGI_FORMAT_DESC format;
    if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), Format, format)) ||
        ((format.Flag & MTL_DXGI_FORMAT_TYPELESS) &&
         !(format.Flag & (MTL_DXGI_FORMAT_DEPTH_PLANER | MTL_DXGI_FORMAT_STENCIL_PLANER)))) {
      ERR("ResolveSubresourceRegion: invalid format ", Format);
      recording_error_ = E_INVALIDARG;
      return;
    }
    // a type group keeps its component layout (D3D11.3 19.1.1); bit-copy casts also permit other layouts
    for (auto resource : {src, dst}) {
      if (!DepthStencilPlanarFlags(resource->texture->pixelFormat()) &&
          DXGIFormatType(resource->GetDesc().Format) != DXGIFormatType(Format)) {
        recording_error_ = E_INVALIDARG;
        return;
      }
    }
    UINT level[2], slice[2], plane[2];
    DecomposeSubresource(dst->GetDesc(), DstSubresource, &level[0], &slice[0], &plane[0]);
    DecomposeSubresource(src->GetDesc(), SrcSubresource, &level[1], &slice[1], &plane[1]);
    // a color subresource is seen in the resolve's format without its swizzle, as its channels are stored, a depth or
    // stencil plane in the only one it has, which for reading stencil beside depth is the stencil view of both
    auto view = [&](MTLD3D12Resource *resource, bool source) {
      auto &texture = resource->texture;
      auto planes = DepthStencilPlanarFlags(texture->pixelFormat());
      TextureViewDescriptor desc;
      desc.format = !planes                             ? WMTPixelFormat(ORIGINAL_FORMAT(format.PixelFormat))
                    : source && planes == 3 && plane[1] ? WMTPixelFormatX32_Stencil8
                                                        : texture->pixelFormat();
      desc.type = texture->sampleCount() > 1 ? WMTTextureType2DMultisample : WMTTextureType2D;
      desc.firstArraySlice = slice[source];
      desc.firstMiplevel = level[source];
      return texture->createView(desc);
    };
    auto dst_view = view(dst, false), src_view = view(src, true);
    D3D12_RECT whole{0, 0, (LONG)src->texture->width(src_view), (LONG)src->texture->height(src_view)},
        rect = pSrcRect ? *pSrcRect : whole;
    auto planes = DepthStencilPlanarFlags(dst->texture->pixelFormat());
    // Metal's own resolve is of a whole attachment into one as large, by averaging colors of the formats it resolves
    if (ResolveMode == D3D12_RESOLVE_MODE_AVERAGE && !planes &&
        any_bit_set(device_->GetMTLPixelFormatCapability(format.PixelFormat) & FormatCapability::Resolve) && !DstX && !DstY &&
        !memcmp(&rect, &whole, sizeof(rect)) && dst->texture->width(dst_view) == (UINT)whole.right &&
        dst->texture->height(dst_view) == (UINT)whole.bottom) {
      allocator_->InvalidateCurrentPass();
      auto &source = src->texture->view(src_view), &destination = dst->texture->view(dst_view);
      // the view the render pass just before drew to is resolved as that pass stores it, as D3D11's are
      // (ArgumentEncodingContext::isResolveSignatureMatched): nothing came between to write it
      if (auto last = allocator_->encoder_last; last->type == EncoderType::Render) {
        for (auto &color : static_cast<RenderEncoderData *>(last)->colors) {
          if (!color.attachment || color.store_action != WMTStoreActionStore || color.resolve_attachment ||
              color.attachment->allocation != source.allocation || !(color.attachment->key == source.key))
            continue;
          color.store_action = WMTStoreActionStoreAndMultisampleResolve;
          color.resolve_attachment = destination;
          return;
        }
      }
      auto resolve = allocator_->AllocatePass<ResolveEncoderData>();
      resolve->type = EncoderType::Resolve;
      resolve->src = source;
      resolve->dst = destination;
      allocator_->InvalidateCurrentPass();
      return;
    }
    allocator_->clear_rtv_.begin(dst->texture, dst_view, 0, planes == 3 ? 1 << plane[0] : planes, true);
    allocator_->clear_rtv_.resolve(
        DstX, DstY, rect.right - rect.left, rect.bottom - rect.top, src->texture, src_view, rect.left, rect.top, ResolveMode
    );
    allocator_->clear_rtv_.end();
  }

  void STDMETHODCALLTYPE
  SetViewInstanceMask(UINT Mask) {
    // no pipeline has view instancing (ViewInstancingTier is NOT_SUPPORTED): "The view instance mask is only honored
    // by PSOs that have view instancing enabled" (D3D12 View Instancing)
  }

  void STDMETHODCALLTYPE
  WriteBufferImmediate(
      UINT Count, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *pParams, const D3D12_WRITEBUFFERIMMEDIATE_MODE *pModes
  ) {
    if (pModes)
      for (auto mode : std::span(pModes, Count))
        if (mode != D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT && mode != D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN &&
            mode != D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT) {
          recording_error_ = E_INVALIDARG;
          return;
        }
    // a DEFAULT value is written in order with the list's other work. a marker (MARKER_IN, MARKER_OUT: written once
    // the work before has begun, or has completed) waits for the next barrier, DEFAULT write or Close, which write
    // the markers waiting together: a title writes thousands a frame (dttdrv/mullion#32: Starfield about 5,000), and
    // a write of its own ends the pass around it. nothing else reads or writes a marker's buffer before then, as that
    // needs a barrier out of COPY_DEST, which writing one promotes the buffer to
    for (UINT i = 0; i < Count; i++) {
      auto &param = pParams[i];
      if (pModes && pModes[i] != D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT) {
        // a marker says how far the GPU got, to an application that reads it after a device removal and at no other
        // time (d3d12.immediateMarkers)
        if (device_->immediate_markers)
          markers_.push_back({param.Dest, param.Value});
        continue;
      }
      WriteMarkers();
      WriteImmediate(param.Dest, &param.Value, sizeof(param.Value));
    }
  }

  // WriteBufferImmediate's markers that wait, in the order they were given
  struct Marker {
    D3D12_GPU_VIRTUAL_ADDRESS address;
    UINT value;
  };
  std::vector<Marker> markers_;

  void
  WriteMarkers() {
    for (auto &marker : markers_)
      WriteImmediate(marker.address, &marker.value, sizeof(marker.value));
    markers_.clear();
  }

  // the buffer and offset a GPU address is in
  std::pair<obj_handle_t, uint64_t>
  At(D3D12_GPU_VIRTUAL_ADDRESS VA) {
    uint64_t offset = 0;
    auto allocation = LookupBuffer(VA, &offset);
    return {allocation ? allocation->buffer().handle : 0, offset};
  }

  // bytes between buffers, in order with the list's other work
  void
  CopyBytes(std::pair<obj_handle_t, uint64_t> From, std::pair<obj_handle_t, uint64_t> To, uint64_t Length) {
    if (!From.first || !To.first || !Length || !PreBlit())
      return;
    auto &cmd = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
    cmd.type = WMTBlitCommandCopyFromBufferToBuffer;
    cmd.src = From.first;
    cmd.src_offset = From.second;
    cmd.dst = To.first;
    cmd.dst_offset = To.second;
    cmd.copy_length = Length;
  }

  // bytes at a GPU address, copied from the upload heap in order with the list's other work
  void
  WriteImmediate(D3D12_GPU_VIRTUAL_ADDRESS VA, const void *Data, size_t Length) {
    uint64_t offset;
    auto allocation = LookupBuffer(VA, &offset);
    if (!allocation || !PreBlit())
      return;
    auto [Mapped, Offset] = allocator_->AllocateGPUHeap(Length, sizeof(uint64_t));
    if (allocator_->records_)
      allocator_->RecordGPUHeap(Mapped, Length, "immediate data");
    memcpy(Mapped, Data, Length);
    auto &cmd = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
    cmd.type = WMTBlitCommandCopyFromBufferToBuffer;
    cmd.src = allocator_->gpu_heap_buffer_;
    cmd.src_offset = Offset;
    cmd.dst = allocation->buffer();
    cmd.dst_offset = offset;
    cmd.copy_length = Length;
  }

  // protected sessions are never created (the device reports none), so the only session is none
  void STDMETHODCALLTYPE
  SetProtectedResourceSession(ID3D12ProtectedResourceSession *pSession) {}

  // a render pass is its targets and their clears; EndRenderPass runs its resolves
  void STDMETHODCALLTYPE
  BeginRenderPass(
      UINT NumRenderTargets, const D3D12_RENDER_PASS_RENDER_TARGET_DESC *pRenderTargets,
      const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC *pDepthStencil, D3D12_RENDER_PASS_FLAGS Flags
  ) {
    D3D12_CPU_DESCRIPTOR_HANDLE targets[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT];
    UINT target_count = 0;
    pass_resolves_.clear();
    pass_discards_.clear();
    // a view's texture, and the textures the pass discards as it begins (DiscardTexture, after its clears)
    auto texture_of = [&](D3D12_CPU_DESCRIPTOR_HANDLE handle) -> Texture * {
      auto [Heap, Index] = GetRenderTargetHeap(device_, handle);
      return Heap ? Heap->GetRenderTarget(Index).Texture : nullptr;
    };
    std::vector<Texture *> begin_discards;
    auto discards = [&](const D3D12_RENDER_PASS_BEGINNING_ACCESS &begin, const D3D12_RENDER_PASS_ENDING_ACCESS &end,
                        D3D12_CPU_DESCRIPTOR_HANDLE handle) {
      if (!DiscardsTextures() || (Flags & (D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS | D3D12_RENDER_PASS_FLAG_RESUMING_PASS)))
        return;
      auto texture = texture_of(handle);
      if (!texture)
        return;
      if (begin.Type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD)
        begin_discards.push_back(texture);
      if (end.Type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_DISCARD ||
          (end.Type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE && !end.Resolve.PreserveResolveSource))
        pass_discards_.push_back(texture);
    };
    auto resolve = [&](const D3D12_RENDER_PASS_ENDING_ACCESS &end) {
      if (end.Type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE)
        for (auto &sub : std::span(end.Resolve.pSubresourceParameters, end.Resolve.SubresourceCount))
          pass_resolves_.push_back({end.Resolve, sub});
    };
    // the surfaces bound for raster (Render Passes, "Surfaces That BeginRenderPass Binds For Raster")
    auto bound = [](const D3D12_RENDER_PASS_BEGINNING_ACCESS &begin, bool read_only) {
      UINT type = begin.Type;
      return type == sdk::D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE_LOCAL_SRV
                 ? read_only
                 : type != D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS &&
                       type != sdk::D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE_LOCAL_UAV;
    };
    // a pipeline's render targets are the bound ones, in "the order the raster bound surfaces appear in the render
    // pass RTV list without any gaps"
    for (UINT i = 0; i < NumRenderTargets; i++) {
      if (bound(pRenderTargets[i].BeginningAccess, false))
        targets[target_count++] = pRenderTargets[i].cpuDescriptor;
      resolve(pRenderTargets[i].EndingAccess);
      discards(pRenderTargets[i].BeginningAccess, pRenderTargets[i].EndingAccess, pRenderTargets[i].cpuDescriptor);
    }
    // a depth stencil buffer's planes are one texture: it is discarded where both are, or the format has one
    if (pDepthStencil) {
      auto planes = [&] {
        auto texture = texture_of(pDepthStencil->cpuDescriptor);
        return texture ? DepthStencilPlanarFlags(texture->pixelFormat()) : 0u;
      }();
      auto &db = pDepthStencil->DepthBeginningAccess, &sb = pDepthStencil->StencilBeginningAccess;
      auto &de = pDepthStencil->DepthEndingAccess, &se = pDepthStencil->StencilEndingAccess;
      auto discarding_begin = [](auto &b) { return b.Type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD; };
      auto discarding_end = [](auto &e) {
        return e.Type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_DISCARD ||
               (e.Type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE && !e.Resolve.PreserveResolveSource);
      };
      bool depth = planes & 1, stencil = planes & 2;
      D3D12_RENDER_PASS_BEGINNING_ACCESS begin{};
      D3D12_RENDER_PASS_ENDING_ACCESS end{};
      if ((!depth || discarding_begin(db)) && (!stencil || discarding_begin(sb)) && planes)
        begin.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD;
      if ((!depth || discarding_end(de)) && (!stencil || discarding_end(se)) && planes) {
        end.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE;
        end.Resolve.PreserveResolveSource = FALSE;
      }
      discards(begin, end, pDepthStencil->cpuDescriptor);
    }
    bool depth_stencil = pDepthStencil &&
                         (bound(pDepthStencil->DepthBeginningAccess, Flags & D3D12_RENDER_PASS_FLAG_BIND_READ_ONLY_DEPTH) ||
                          bound(pDepthStencil->StencilBeginningAccess, Flags & D3D12_RENDER_PASS_FLAG_BIND_READ_ONLY_STENCIL));
    OMSetRenderTargets(target_count, targets, FALSE, depth_stencil ? &pDepthStencil->cpuDescriptor : nullptr);
    // a pass that resumes one goes on with what that one left, and clears nothing again: suspending and resuming
    // "results in no rendering difference (versus ENDING_PRESERVE/BEGINNING_PRESERVE)" (Render Passes, HLK Testing)
    bool clears = !(Flags & D3D12_RENDER_PASS_FLAG_RESUMING_PASS);
    for (UINT i = 0; clears && i < NumRenderTargets; i++)
      if (pRenderTargets[i].BeginningAccess.Type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR)
        ClearRenderTargetView(
            pRenderTargets[i].cpuDescriptor, pRenderTargets[i].BeginningAccess.Clear.ClearValue.Color, 0, nullptr
        );
    if (!pDepthStencil) {
      for (auto texture : begin_discards)
        DiscardTexture(texture);
      return;
    }
    auto &depth = pDepthStencil->DepthBeginningAccess, &stencil = pDepthStencil->StencilBeginningAccess;
    auto flags = (depth.Type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR ? D3D12_CLEAR_FLAG_DEPTH : 0) |
                 (stencil.Type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR ? D3D12_CLEAR_FLAG_STENCIL : 0);
    if (clears && flags)
      ClearDepthStencilView(
          pDepthStencil->cpuDescriptor, (D3D12_CLEAR_FLAGS)flags, depth.Clear.ClearValue.DepthStencil.Depth,
          stencil.Clear.ClearValue.DepthStencil.Stencil, 0, nullptr
      );
    resolve(pDepthStencil->DepthEndingAccess);
    resolve(pDepthStencil->StencilEndingAccess);
    for (auto texture : begin_discards)
      DiscardTexture(texture);
  }

  void STDMETHODCALLTYPE
  EndRenderPass() {
    for (auto &[r, sub] : pass_resolves_)
      ResolveSubresourceRegion(
          r.pDstResource, sub.DstSubresource, sub.DstX, sub.DstY, r.pSrcResource, sub.SrcSubresource,
          const_cast<D3D12_RECT *>(&sub.SrcRect), r.Format, r.ResolveMode
      );
    pass_resolves_.clear();
    for (auto texture : pass_discards_)
      DiscardTexture(texture);
    pass_discards_.clear();
    // the pass's targets "are not propagated out to the Command List": a draw after it has none
    OMSetRenderTargets(0, nullptr, FALSE, nullptr);
  }

  // the device reports no meta commands, ray tracing, variable rate shading or mesh shaders, so a valid list never
  // records these
  void STDMETHODCALLTYPE
  InitializeMetaCommand(ID3D12MetaCommand *, const void *, SIZE_T) {
    ERR("InitializeMetaCommand: meta commands are not supported");
  }

  void STDMETHODCALLTYPE
  ExecuteMetaCommand(ID3D12MetaCommand *, const void *, SIZE_T) {
    ERR("ExecuteMetaCommand: meta commands are not supported");
  }

  // a build or a copy of an acceleration structure has a pass of its own, which the queue runs after the one before
  // it: Metal may run the builds of one encoder together, and Direct3D 12's run in their order, over one scratch
  // buffer if the application likes. what each does before its own command needs no pass of its own, though: for
  // builds and copies recorded one after another, the writes of headers and the copies of inputs share the blit pass
  // before the first of them, and the conversions of transforms and instances a compute pass after it, so that N
  // builds take N + 1 or N + 2 of Metal's encoders and not 2N or 3N. that work reads the application's inputs and
  // the headers, and writes headers and buffers of the command's own, none of which a build or copy reads or writes,
  // so it may run before the commands recorded before it. only a conversion of instances reads a header, to find
  // an instance's structure: once a build of the run has converted instances, which then name the structures their
  // addresses had when it was recorded, the next command starts a run of its own
  struct StructureRun {
    // the last command's pass, which is open, and its number: another recording may have a pass where this one was
    const EncoderData *pass = nullptr;
    uint64_t id = 0;
    BlitEncoderData *blit = nullptr;
    ComputeEncoderData *compute = nullptr;
    bool converted = false;
  } structure_run_;

  bool
  JoinsStructures() const {
    auto open = allocator_->encoder_current;
    return open && open == structure_run_.pass && open->id == structure_run_.id && structure_run_.blit &&
           !structure_run_.converted;
  }

  // what a command that joined the run does before its pass goes to the run's blit pass
  void
  StructureBlits(bool Joined) {
    if (Joined)
      allocator_->encoder_current = structure_run_.blit;
  }

  // and to its compute pass, made when the first command asks. the run's passes are linked already: neither is
  // ended again
  ComputeEncoderData *
  StructureCompute(bool Joined) {
    if (!Joined)
      return StartComputePass();
    if (!structure_run_.compute) {
      allocator_->encoder_current = nullptr;
      structure_run_.compute = StartComputePass();
      allocator_->InvalidateCurrentPass();
    }
    allocator_->encoder_current = structure_run_.compute;
    return structure_run_.compute;
  }

  void
  StartAccelerationStructurePass() {
    allocator_->InvalidateCurrentPass();
    auto pass = allocator_->AllocatePass<AccelerationStructureEncoderData>();
    pass->type = EncoderType::AccelerationStructure;
    pass->cmd_head.type = WMTAccelerationStructureCommandNop;
    pass->cmd_head.next.set(0);
    pass->cmd_tail = (wmtcmd_base *)&pass->cmd_head;
    pass->filled = nullptr;
    pass->filled_tail = &pass->filled;
  }

  // the pass of a build or a copy, after the open one for a command that joined the run, which the commands after it
  // may join. `Blit`: the pass its header went to, if one; `Conversion`: the pass that converts its instances
  void
  PreAccelerationStructure(bool Joined, EncoderData *Blit, ComputeEncoderData *Conversion) {
    if (Joined) {
      allocator_->encoder_current = const_cast<EncoderData *>(structure_run_.pass);
      StartAccelerationStructurePass();
      structure_run_.pass = allocator_->encoder_current;
      structure_run_.id = structure_run_.pass->id;
      structure_run_.converted = Conversion;
      return;
    }
    // with nothing done before it, it goes on in the open pass, as it was recorded
    PreAccelerationStructure();
    structure_run_ = {allocator_->encoder_current, allocator_->encoder_current->id,
                      Blit && Blit->type == EncoderType::Blit ? static_cast<BlitEncoderData *>(Blit) : nullptr,
                      Conversion, bool(Conversion)};
  }

  // the open pass of acceleration structure commands, or a new one
  void
  PreAccelerationStructure() {
    if (allocator_->encoder_current && allocator_->encoder_current->type == EncoderType::AccelerationStructure)
      return;
    structure_run_ = {};
    StartAccelerationStructurePass();
  }

  // the current pass's build or copy fills the structure, with inputs it kept or those of the structure it copies
  void
  Fills(AccelerationStructure *Structure, AccelerationStructureInputs *Kept, AccelerationStructure *From) {
    auto pass = static_cast<AccelerationStructureEncoderData *>(allocator_->encoder_current);
    auto filled = allocator_->AllocateCommandData<AccelerationStructureFilled>(1);
    *filled = {Structure, Kept, From, nullptr};
    *pass->filled_tail = filled;
    pass->filled_tail = &filled->next;
  }

  // the structure a build or a copy fills at an address, which the list keeps: the one there when it is large enough,
  // so that what refers to it sees its new contents as it would in the memory, and a new one otherwise. its header
  // goes to the address
  std::shared_ptr<AccelerationStructure>
  AccelerationStructureAt(D3D12_GPU_VIRTUAL_ADDRESS VA, uint64_t Size, uint32_t InstanceCount) {
    auto structure = device_->LookupAccelerationStructure(VA);
    if (!structure || structure->size < Size || structure->instance_count < InstanceCount ||
        !structure->instances != !InstanceCount) {
      structure = std::make_shared<AccelerationStructure>(device_, Size, InstanceCount);
      device_->SetAccelerationStructure(VA, structure);
    }
    allocator_->acceleration_structures_.push_back(structure);
    WriteImmediate(VA, &structure->header, sizeof(structure->header));
    return structure;
  }

  // post-build information of structures, as an array of uint64 at the description's address. only what Metal has is
  // written: it neither serializes structures nor decodes them for tools. `Built`: the inputs of the build whose
  // information this is, which the structure has once the build has run
  void
  EmitPostbuildInfo(
      const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC &Desc,
      std::span<const std::shared_ptr<AccelerationStructure>> Structures, const AccelerationStructureInputs *Built = nullptr
  ) {
    // what the copies that give a structure's inputs back need; nothing, when the inputs were not kept
    auto layout = [&](const AccelerationStructure &structure, auto &&write) {
      if (auto kept = Built ? Built : structure.inputs.get())
        write(kept->top(), kept->count(), kept->size);
      else
        ERR("acceleration structure post-build information ", Desc.InfoType,
            " needs the structure's inputs (d3d12.keepAccelerationStructureInputs)");
    };
    D3D12_GPU_VIRTUAL_ADDRESS address = Desc.DestBuffer;
    for (auto &shared : Structures) {
      auto &structure = *shared;
      // every kind of information but one is a 64-bit size
      uint64_t written = sizeof(uint64_t);
      switch (Desc.InfoType) {
      case D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE: {
        // also where a later compacting copy finds it
        device_->AllocateCompactedSize(structure.compacted_size);
        uint64_t offset;
        auto allocation = device_->LookupBufferByVA(address, &offset);
        const std::pair<obj_handle_t, uint64_t> places[] = {
            {allocation ? allocation->buffer().handle : 0, offset},
            {structure.compacted_size.buffer, structure.compacted_size.offset},
        };
        PreAccelerationStructure();
        for (auto &[buffer, at] : places) {
          if (!buffer)
            continue;
          auto &cmd = allocator_->EncodeAccelerationStructureCommand<wmtcmd_accelerationstructure_write_compacted_size>();
          cmd.type = WMTAccelerationStructureCommandWriteCompactedSize;
          cmd.structure = structure.structure;
          cmd.buffer = buffer;
          cmd.offset = at;
        }
        break;
      }
      case D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_CURRENT_SIZE:
        WriteImmediate(address, &structure.size, sizeof(structure.size));
        break;
      case D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_TOOLS_VISUALIZATION: {
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_TOOLS_VISUALIZATION_DESC info{};
        layout(structure, [&](bool top, uint64_t count, uint64_t size) {
          info.DecodedSizeInBytes = VisualizationLayout(top, count, size).size;
        });
        WriteImmediate(address, &info, sizeof(info));
        break;
      }
      case D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_SERIALIZATION: {
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_SERIALIZATION_DESC info{};
        layout(structure, [&](bool top, uint64_t count, uint64_t size) {
          info = {SerializedLayout(top, count, size).size, top ? count : 0};
        });
        WriteImmediate(address, &info, written = sizeof(info));
        break;
      }
      default:
        ERR("acceleration structure post-build information ", Desc.InfoType, " is not supported");
        break;
      }
      address += written;
    }
  }

  void STDMETHODCALLTYPE
  BuildRaytracingAccelerationStructure(
      const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC *pDesc, UINT NumPostbuildInfoDescs,
      const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *pPostbuildInfoDescs
  ) {
    auto &inputs = pDesc->Inputs;
    bool top = inputs.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    // an update refits its source, into itself or into a structure of its size
    std::shared_ptr<AccelerationStructure> source;
    bool update = inputs.Flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
    if (update)
      source = device_->LookupAccelerationStructure(pDesc->SourceAccelerationStructureData);
    if (auto later = Later(LaterEncoderData::Build, update && !source)) {
      later->build = *pDesc;
      later->information = Kept(pPostbuildInfoDescs, later->information_count = NumPostbuildInfoDescs);
      if (!top) {
        auto geometries = allocator_->AllocateCommandData<D3D12_RAYTRACING_GEOMETRY_DESC>(inputs.NumDescs);
        for (UINT i = 0; i < inputs.NumDescs; i++)
          geometries[i] =
              inputs.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY ? inputs.pGeometryDescs[i] : *inputs.ppGeometryDescs[i];
        later->build.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        later->build.Inputs.pGeometryDescs = geometries;
      }
      return;
    }
    if (update && !source) {
      ERR("BuildRaytracingAccelerationStructure: no acceleration structure to update at the source address");
      return;
    }
    if (source)
      allocator_->acceleration_structures_.push_back(source);
    bool joined = JoinsStructures();
    StructureBlits(joined);
    // a top-level structure's instance count is a uint32 the build reads
    obj_handle_t count = 0;
    uint64_t count_offset = 0;
    if (top) {
      auto [mapped, offset] = allocator_->AllocateGPUHeap(sizeof(uint32_t), sizeof(uint32_t));
      *(uint32_t *)mapped = inputs.NumDescs;
      count = allocator_->gpu_heap_buffer_;
      count_offset = offset;
    }
    auto geometries =
        allocator_->AllocateCommandData<WMTAccelerationStructureGeometry>(top ? 0 : std::max(inputs.NumDescs, 1u));
    auto info = AccelerationStructureInfo(device_, inputs, geometries, true, 0, count, count_offset);
    // elements a stride of zero apart are one element, which Metal reads as many: it is repeated, twice as many with
    // each copy. elements of 16-bit components may lie at any even address ("D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC",
    // VertexBuffer), where Metal does not read them: they are moved
    for (UINT i = 0; !top && i < inputs.NumDescs; i++) {
      auto &desc = inputs.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY ? inputs.pGeometryDescs[i] : *inputs.ppGeometryDescs[i];
      auto elements = AccelerationStructureGeometryElements(device_, desc);
      bool once = !elements.stride && elements.count > 1;
      if (!elements.count || !geometries[i].buffer || !(once || geometries[i].offset % kAccelerationStructureElementAlignment))
        continue;
      uint64_t given = once ? elements.size : (elements.count - 1) * elements.stride + elements.size;
      auto [buffer, offset] = allocator_->AllocateTempBuffer(
          once ? elements.count * elements.size : given, kAccelerationStructureInputAlignment
      );
      std::pair<obj_handle_t, uint64_t> repeated{buffer.handle, offset};
      CopyBytes({geometries[i].buffer, geometries[i].offset}, repeated, given);
      for (uint64_t done = 1; once && done < elements.count; done *= 2)
        CopyBytes(
            repeated, {repeated.first, repeated.second + done * elements.size},
            std::min(done, elements.count - done) * elements.size
        );
      geometries[i].buffer = repeated.first;
      geometries[i].offset = repeated.second;
    }
    // a GPU that takes no row-major transforms builds from column-major copies of them, made before the build
    uint32_t transforms = 0;
    for (UINT i = 0; !top && !device_->row_major_transforms && i < inputs.NumDescs; i++)
      transforms += geometries[i].transform_buffer != 0;
    if (transforms) {
      auto [sources, sources_offset] = allocator_->AllocateGPUHeap(transforms * sizeof(uint64_t), sizeof(uint64_t));
      auto [transposed, transposed_offset] = allocator_->AllocateGPUHeap(
          transforms * 12 * sizeof(float), D3D12_RAYTRACING_TRANSFORM3X4_BYTE_ALIGNMENT
      );
      for (UINT i = 0, k = 0; i < inputs.NumDescs; i++) {
        if (!geometries[i].transform_buffer)
          continue;
        auto &desc = inputs.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY ? inputs.pGeometryDescs[i] : *inputs.ppGeometryDescs[i];
        static_cast<uint64_t *>(sources)[k] = desc.Triangles.Transform3x4;
        geometries[i].transform_buffer = allocator_->gpu_heap_buffer_;
        geometries[i].transform_offset = transposed_offset + k++ * 12 * sizeof(float);
        geometries[i].transform_column_major = true;
      }
      if (allocator_->records_)
        allocator_->RecordGPUHeap(sources, transforms * sizeof(uint64_t), "transform addresses");
      StructureCompute(joined);
      auto &pso = allocator_->EncodeComputeCommand<wmtcmd_compute_setpso>();
      pso.type = WMTComputeCommandSetPSO;
      pso.pso = device_->acceleration_structure_transforms;
      pso.threadgroup_size = {device_->GetSIMDWidth(), 1, 1};
      for (auto [offset, index] : {std::pair{sources_offset, 0}, std::pair{transposed_offset, 1}}) {
        auto &buffer = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
        buffer.type = WMTComputeCommandSetBuffer;
        buffer.buffer = allocator_->gpu_heap_buffer_;
        buffer.offset = offset;
        buffer.index = index;
      }
      auto &dispatch = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch>();
      dispatch.type = WMTComputeCommandDispatchThreads;
      dispatch.size = {transforms, 1, 1};
      StructureBlits(joined);
    }
    uint64_t size = source ? source->size : 0;
    if (!source) {
      auto sizes = info;
      device_->GetMTLDevice().accelerationStructureSizes(sizes);
      size = AccelerationStructureSize(sizes.size);
    }
    auto structure = AccelerationStructureAt(pDesc->DestAccelerationStructureData, size, top ? inputs.NumDescs : 0);
    // Metal wants a buffer of instances also when the count it reads is zero
    info.instance_buffer = structure->instances ? structure->instances.handle : count;
    auto kept = KeepInputs(inputs);
    // the pass its header went to, which the instances' conversion follows
    auto header_pass = allocator_->encoder_current;
    ComputeEncoderData *conversion_pass = nullptr;

    if (structure->instances) {
      conversion_pass = StructureCompute(joined);
      auto &pso = allocator_->EncodeComputeCommand<wmtcmd_compute_setpso>();
      pso.type = WMTComputeCommandSetPSO;
      pso.pso = device_->acceleration_structure_instances;
      pso.threadgroup_size = {device_->GetSIMDWidth(), 1, 1};
      // MullionInstanceConversion
      struct {
        uint64_t instances, kept;
        uint32_t pointers, count;
      } conversion{inputs.InstanceDescs, kept ? kept->data_address : 0,
                   inputs.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS, inputs.NumDescs};
      auto &bytes = allocator_->EncodeComputeCommand<wmtcmd_compute_setbytes>();
      bytes.type = WMTComputeCommandSetBytes;
      bytes.bytes.set(memcpy(allocator_->AllocateCPUHeap(sizeof(conversion), 16), &conversion, sizeof(conversion)));
      bytes.length = sizeof(conversion);
      bytes.index = 0;
      auto &converted = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
      converted.type = WMTComputeCommandSetBuffer;
      converted.buffer = structure->instances;
      converted.offset = 0;
      converted.index = 1;
      auto &dispatch = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch>();
      dispatch.type = WMTComputeCommandDispatchThreads;
      dispatch.size = {inputs.NumDescs, 1, 1};
    }

    uint64_t scratch_offset = 0;
    auto scratch = device_->LookupBufferByVA(pDesc->ScratchAccelerationStructureData, &scratch_offset);
    PreAccelerationStructure(joined, header_pass, conversion_pass);
    if (allocator_->records_ && top)
      allocator_->RecordGPUHeap(
          ptr_add(allocator_->gpu_heap_, count_offset), sizeof(inputs.NumDescs), "instance count"
      );
    Fills(structure.get(), kept.get(), nullptr);
    auto &build = allocator_->EncodeAccelerationStructureCommand<wmtcmd_accelerationstructure_build>();
    build.type = WMTAccelerationStructureCommandBuild;
    build.dst = structure->structure;
    build.src = source ? source->structure.handle : 0;
    build.scratch = scratch ? scratch->buffer().handle : 0;
    build.scratch_offset = scratch_offset;
    build.info = info;

    for (auto &desc : std::span(pPostbuildInfoDescs, NumPostbuildInfoDescs))
      EmitPostbuildInfo(desc, {&structure, 1}, kept.get());
  }

  void STDMETHODCALLTYPE
  EmitRaytracingAccelerationStructurePostbuildInfo(
      const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *pDesc, UINT NumSourceAccelerationStructures,
      const D3D12_GPU_VIRTUAL_ADDRESS *pSourceAccelerationStructureData
  ) {
    std::vector<std::shared_ptr<AccelerationStructure>> structures;
    for (auto address : std::span(pSourceAccelerationStructureData, NumSourceAccelerationStructures))
      structures.push_back(device_->LookupAccelerationStructure(address));
    bool missing = std::ranges::count(structures, nullptr);
    // what a structure was built of is known when the queue gets here
    bool of_inputs = pDesc->InfoType == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_SERIALIZATION ||
                     pDesc->InfoType == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_TOOLS_VISUALIZATION;
    if (auto later = Later(LaterEncoderData::Information, missing || of_inputs)) {
      later->information = Kept(pDesc, later->information_count = 1);
      later->sources = Kept(pSourceAccelerationStructureData, later->source_count = NumSourceAccelerationStructures);
      return;
    }
    if (missing) {
      ERR("EmitRaytracingAccelerationStructurePostbuildInfo: no acceleration structure at a source address");
      return;
    }
    allocator_->acceleration_structures_.insert(
        allocator_->acceleration_structures_.end(), structures.begin(), structures.end()
    );
    EmitPostbuildInfo(*pDesc, structures);
  }

  // a command the queue records when it gets there (LaterEncoderData), unless this is the list it records on: one
  // that cannot be recorded now, and those on acceleration structures after it in the list, to stay in its order
  bool left_for_later_ = false;
  LaterEncoderData *
  Later(decltype(LaterEncoderData::command) Command, bool Needed) {
    if (!leaves_for_later || !(left_for_later_ |= Needed))
      return nullptr;
    allocator_->InvalidateCurrentPass();
    auto later = allocator_->AllocatePass<LaterEncoderData>();
    later->type = EncoderType::Later;
    later->command = Command;
    allocator_->InvalidateCurrentPass();
    return later;
  }

  // an array of a call's, for as long as the list's commands are
  template <typename T>
  const T *
  Kept(const T *Values, size_t Count) {
    auto kept = allocator_->AllocateCommandData<T>(Count);
    std::copy_n(Values, Count, kept);
    return kept;
  }

  // a build's inputs, copied as they are when the build runs. a top-level structure's are written by the kernel that
  // converts its instances
  std::shared_ptr<AccelerationStructureInputs>
  KeepInputs(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS &Inputs) {
    if (!device_->keep_structure_inputs)
      return nullptr;
    // an update is how a build was done, not what was built
    auto kept = std::make_shared<AccelerationStructureInputs>(
        device_, Inputs.Type, Inputs.Flags & ~D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE
    );
    struct Part {
      D3D12_GPU_VIRTUAL_ADDRESS from;
      uint64_t to, length;
    };
    std::vector<Part> parts;
    if (kept->top()) {
      kept->instance_count = Inputs.NumDescs;
      kept->size = Inputs.NumDescs * (sizeof(D3D12_GPU_VIRTUAL_ADDRESS) + sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
    } else {
      // offsets start past 0, which stays what an address that was not given is
      kept->size = kAccelerationStructureInputAlignment;
      for (UINT i = 0; i < Inputs.NumDescs; i++) {
        auto &geometry = kept->geometries.emplace_back(
            Inputs.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY ? Inputs.pGeometryDescs[i] : *Inputs.ppGeometryDescs[i]
        );
        auto lengths = AccelerationStructureGeometryLengths(device_, geometry);
        auto addresses = AccelerationStructureGeometryAddresses(geometry);
        for (size_t part = 0; part < addresses.size() && addresses[part]; part++) {
          if (!*addresses[part] || !lengths[part])
            continue;
          parts.push_back({*addresses[part], kept->size, lengths[part]});
          *addresses[part] = kept->size;
          kept->size = align(kept->size + lengths[part], kAccelerationStructureInputAlignment);
        }
      }
    }
    allocator_->acceleration_structure_inputs_.push_back(kept);
    if (!kept->size)
      return kept;
    WMTBufferInfo info{kept->size, WMTResourceStorageModePrivate};
    kept->data = device_->GetMTLDevice().newBuffer(info);
    kept->data_address = info.gpu_address;
    device_->RegisterResidency(kept->data);
    for (auto &part : parts)
      CopyBytes(At(part.from), {kept->data, part.to}, part.length);
    return kept;
  }

  // a structure's inputs at an address, serialized or decoded for tools (DXR,
  // "D3D12_SERIALIZED_RAYTRACING_ACCELERATION_STRUCTURE_HEADER" and "..._TOOLS_VISUALIZATION_HEADER")
  void
  GiveInputs(const AccelerationStructure &Structure, D3D12_GPU_VIRTUAL_ADDRESS Destination, bool Serialized) {
    auto &kept = Structure.inputs;
    if (!kept) {
      ERR("CopyRaytracingAccelerationStructure: the structure's inputs were not kept "
          "(d3d12.keepAccelerationStructureInputs)");
      return;
    }
    allocator_->acceleration_structure_inputs_.push_back(kept);
    bool top = kept->top();
    uint64_t count = kept->count(), described, data;
    if (Serialized) {
      SerializedLayout layout(top, count, kept->size);
      const D3D12_SERIALIZED_RAYTRACING_ACCELERATION_STRUCTURE_HEADER header{
          kSerializedIdentifier, layout.size, Structure.size, top ? count : 0
      };
      const SerializedInputs inputs{(uint32_t)kept->type, (uint32_t)kept->flags, (uint32_t)count, 0, kept->size};
      WriteImmediate(Destination, &header, sizeof(header));
      WriteImmediate(Destination + layout.inputs, &inputs, sizeof(inputs));
      described = layout.described, data = layout.data;
    } else {
      VisualizationLayout layout(top, count, kept->size);
      const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_TOOLS_VISUALIZATION_HEADER header{kept->type, (UINT)count};
      WriteImmediate(Destination, &header, sizeof(header));
      described = layout.described, data = layout.data;
    }
    if (top) {
      // the pointers a serialized structure has after its header, then the instances
      uint64_t pointers = count * sizeof(D3D12_GPU_VIRTUAL_ADDRESS);
      if (Serialized)
        CopyBytes({kept->data, 0}, At(Destination + SerializedInputsOffset(0)), pointers);
      CopyBytes({kept->data, pointers}, At(Destination + described), kept->size - pointers);
      return;
    }
    // a serialized geometry's data is found from where the data starts; a decoded one's by its address
    auto geometries = kept->geometries;
    if (!Serialized)
      for (auto &geometry : geometries)
        for (auto address : AccelerationStructureGeometryAddresses(geometry))
          if (address && *address)
            *address += Destination + data;
    WriteImmediate(Destination + described, geometries.data(), geometries.size() * sizeof(geometries[0]));
    CopyBytes({kept->data, 0}, At(Destination + data), kept->size);
  }

  void STDMETHODCALLTYPE
  CopyRaytracingAccelerationStructure(
      D3D12_GPU_VIRTUAL_ADDRESS DestAccelerationStructureData, D3D12_GPU_VIRTUAL_ADDRESS SourceAccelerationStructureData,
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE Mode
  ) {
    // a serialized structure is not one yet; the queue builds it, which is not this copy
    bool deserialize = Mode == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE;
    bool compact = Mode == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT;
    bool clone = compact || Mode == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE;
    auto source = deserialize ? nullptr : device_->LookupAccelerationStructure(SourceAccelerationStructureData);
    // what the source was built of, which the other copies give, is known when the queue gets here
    if (auto later = Later(LaterEncoderData::Copy, !source || !clone)) {
      later->destination = DestAccelerationStructureData;
      later->source = SourceAccelerationStructureData;
      later->mode = Mode;
      return;
    }
    if (!source) {
      ERR("CopyRaytracingAccelerationStructure: no acceleration structure at the source address");
      return;
    }
    allocator_->acceleration_structures_.push_back(source);
    if (!clone) {
      GiveInputs(*source, DestAccelerationStructureData, Mode == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_SERIALIZE);
      return;
    }
    // a top-level structure's instances are copied after what converted them, which a pass the copy joined may have
    bool joined = !source->instances && JoinsStructures();
    StructureBlits(joined);
    // a compacted copy is as small as the GPU has said by now, if it was asked
    auto compacted = compact && source->compacted_size.value ? *source->compacted_size.value : 0;
    auto structure =
        AccelerationStructureAt(DestAccelerationStructureData, compacted ? compacted : source->size, source->instance_count);
    if (source->instances && PreBlit()) {
      auto &instances = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
      instances.type = WMTBlitCommandCopyFromBufferToBuffer;
      instances.src = source->instances;
      instances.src_offset = 0;
      instances.dst = structure->instances;
      instances.dst_offset = 0;
      instances.copy_length = source->instance_count * kAccelerationStructureInstanceSize;
    }
    PreAccelerationStructure(joined, allocator_->encoder_current, nullptr);
    Fills(structure.get(), nullptr, source.get());
    auto &copy = allocator_->EncodeAccelerationStructureCommand<wmtcmd_accelerationstructure_copy>();
    copy.type = compact ? WMTAccelerationStructureCommandCopyAndCompact : WMTAccelerationStructureCommandCopy;
    copy.src = source->structure;
    copy.dst = structure->structure;
  }

  // SetPipelineState1 and DispatchRays "can be called from graphics or compute command lists and bundles" (DXR spec,
  // "SetPipelineState1", "DispatchRays"), the calls on acceleration structures "not from bundles"
  void STDMETHODCALLTYPE
  SetPipelineState1(ID3D12StateObject *pStateObject) {
    if (Bundled() && Record([state = Com<ID3D12StateObject>(pStateObject)](auto *l) { l->SetPipelineState1(state.ptr()); }))
      return;
    state_object_ = static_cast<MTLD3D12StateObject *>(pStateObject);
  }

  // the state object's kernel runs a thread per ray generation shader invocation, with the compute root signature's
  // arguments as the global ones (dispatch_rays in mullion_command.metal)
  void STDMETHODCALLTYPE
  DispatchRays(const D3D12_DISPATCH_RAYS_DESC *pDesc) {
    if (Bundled() && Record([desc = *pDesc](auto *l) { l->DispatchRays(&desc); }))
      return;
    // a grid with a dimension of zero has no ray generation shader to run: Metal takes no empty grid
    if (!pDesc->Width || !pDesc->Height || !pDesc->Depth)
      return;
    SM50_RAY_DISPATCH dispatch{
        pDesc->RayGenerationShaderRecord.StartAddress,
        pDesc->MissShaderTable.StartAddress,
        pDesc->MissShaderTable.StrideInBytes,
        pDesc->HitGroupTable.StartAddress,
        pDesc->HitGroupTable.StrideInBytes,
        pDesc->CallableShaderTable.StartAddress,
        pDesc->CallableShaderTable.StrideInBytes,
        {pDesc->Width, pDesc->Height, pDesc->Depth},
    };
    if (!BeginRays(dispatch.pipeline_flags))
      return;
    dispatch.atomic_locks = device_->GetAtomicLocks();
    auto [mapped, offset] = allocator_->AllocateGPUHeap(sizeof(dispatch), alignof(SM50_RAY_DISPATCH));
    if (allocator_->records_)
      allocator_->RecordGPUHeap(mapped, sizeof(dispatch), "ray dispatch");
    memcpy(mapped, &dispatch, sizeof(dispatch));
    BindRays(offset);
    auto &threads = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch>();
    threads.type = WMTComputeCommandDispatchThreads;
    threads.size = {pDesc->Width, pDesc->Height, pDesc->Depth};
  }

  // the state object's pipeline and function table in a compute pass, with the list's root arguments
  bool
  BeginRays(uint32_t &Flags) {
    WMT::ComputePipelineState pipeline;
    WMT::VisibleFunctionTable table;
    if (!state_object_ || !state_object_->GetPipeline(pipeline, table, Flags))
      return false;
    if (!allocator_->encoder_current || allocator_->encoder_current->type != EncoderType::Compute)
      StartComputePass();
    static_cast<ComputeEncoderData *>(allocator_->encoder_current)->rays = true;
    if (device_->NamesPasses())
      device_->NamePass(allocator_->encoder_current, state_object_->name, pipeline);
    auto &pso = allocator_->EncodeComputeCommand<wmtcmd_compute_setpso>();
    pso.type = WMTComputeCommandSetPSO;
    pso.pso = pipeline;
    pso.threadgroup_size = {device_->GetSIMDWidth(), 1, 1};
    // the next compute dispatch binds its own pipeline again
    dirty_state_.set(DirtyState::ComputePipelineState);
    if (rootsig_compute_) {
      BindComputeRootArguments();
    } else {
      // A pipeline with only local root signatures still runs dispatch_rays with both global buffers bound.
      for (auto index : {SM50_BINDING_INDEX_ROOT_ARGUMENTS, SM50_BINDING_INDEX_STATIC_SAMPLERS}) {
        auto &binding = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
        binding.type = WMTComputeCommandSetBuffer;
        binding.buffer = device_->GetNullBufferHandle();
        binding.offset = 0;
        binding.index = index;
      }
    }
    auto &functions = allocator_->EncodeComputeCommand<wmtcmd_compute_setvisiblefunctiontable>();
    functions.type = WMTComputeCommandSetVisibleFunctionTable;
    functions.table = table;
    functions.index = SM50_RAY_BINDING_FUNCTIONS;
    return true;
  }

  // the kernel's SM50_RAY_DISPATCH, in the GPU heap
  void
  BindRays(uint64_t Offset) {
    auto &arguments = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
    arguments.type = WMTComputeCommandSetBuffer;
    arguments.buffer = allocator_->gpu_heap_buffer_;
    arguments.offset = Offset;
    arguments.index = SM50_RAY_BINDING_DISPATCH;
  }

  void STDMETHODCALLTYPE
  RSSetShadingRate(D3D12_SHADING_RATE BaseShadingRate, const D3D12_SHADING_RATE_COMBINER *pCombinerArray) {
    if (Record([=, combiners = pCombinerArray ? std::array{pCombinerArray[0], pCombinerArray[1]}
                                              : std::array<D3D12_SHADING_RATE_COMBINER, 2>{}](auto *l) {
          l->RSSetShadingRate(BaseShadingRate, pCombinerArray ? combiners.data() : nullptr);
        }))
      return;
    shading_rate_ = BaseShadingRate;
    // a null array is passthrough, passthrough
    for (unsigned i = 0; i < 2; i++)
      shading_rate_combiners_[i] = pCombinerArray ? pCombinerArray[i] : D3D12_SHADING_RATE_COMBINER_PASSTHROUGH;
    // the pipeline's variant may change
    dirty_state_.set(DirtyState::GraphicsPipelineState);
  }

  void STDMETHODCALLTYPE
  RSSetShadingRateImage(ID3D12Resource *pShadingRateImage) {
    if (Record([=](auto *l) { l->RSSetShadingRateImage(pShadingRateImage); }))
      return;
    shading_rate_image_ = static_cast<MTLD3D12Resource *>(pShadingRateImage);
    // a new image is copied before the next coarse draw
    shading_rate_pass_ = nullptr;
  }

  void STDMETHODCALLTYPE
  DispatchMesh(UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) {
    if (Bundled() && Record([=](auto *l) { l->DispatchMesh(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ); }))
      return;
    if (!pso_graphics_ || !pso_graphics_->mesh_shader || !ThreadGroupCountX || !ThreadGroupCountY || !ThreadGroupCountZ)
      return;
    if (PreDraw() == DrawCallStatus::Invalid)
      return;
    CountDraw(0, 0);
    // the groups are the amplification shader's, or without one the mesh shader's
    auto size = [](const uint32_t threads[3]) { return WMTSize{threads[0], threads[1], threads[2]}; };
    auto draw = [&](UINT x, UINT y, UINT z) {
      auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_draw_meshthreadgroups>();
      cmd.type = WMTRenderCommandDrawMeshThreadgroups;
      cmd.threadgroup_per_grid = {x, y, z};
      cmd.object_threadgroup_size = size(pso_graphics_->object_threads);
      cmd.mesh_threadgroup_size = size(pso_graphics_->mesh_threads);
    };
    // D3D12 lets a grid have 2^22 groups (Mesh Shader spec, "DispatchMesh API"), Metal's mesh grids fewer (1024
    // before Apple9, then 2^20 - 1): without an amplification shader, a grid past that runs in boxes of whole slices,
    // or of whole rows, or of parts of a row, each with its first group in the raster scale's buffer
    // (SM50_RASTER_SCALE_MESH_ORIGIN). an amplification shader's grid is an object grid, which has no such limit
    uint64_t limit = pso_graphics_->mesh_grid_limit, x = ThreadGroupCountX, y = ThreadGroupCountY,
             z = ThreadGroupCountZ;
    if (pso_graphics_->object_stage || !limit || x * y * z <= limit || !raster_scale_) {
      draw(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
      return;
    }
    uint64_t width = std::min(x, limit), rows = x * y <= limit ? y : std::max<uint64_t>(limit / x, 1),
             slices = x * y <= limit ? limit / (x * y) : 1;
    for (uint64_t k = 0; k < z; k += slices)
      for (uint64_t j = 0; j < y; j += rows)
        for (uint64_t i = 0; i < x; i += width) {
          auto [part, offset] = allocator_->AllocateGPUHeap(SM50_RASTER_SCALE_WORDS * sizeof(uint32_t), alignof(uint32_t));
          if (!part)
            return;
          std::copy_n(raster_scale_words_, SM50_RASTER_SCALE_MESH_ORIGIN, (uint32_t *)part);
          auto origin = (uint32_t *)part + SM50_RASTER_SCALE_MESH_ORIGIN;
          origin[0] = i, origin[1] = j, origin[2] = k;
          EncodeBuffer(WMTRenderCommandSetMeshBuffer, offset, SM50_BINDING_INDEX_RASTER_SCALE);
          draw(std::min(width, x - i), std::min(rows, y - j), std::min(slices, z - k));
        }
    // the next draw binds the raster scale again, from the first group
    dirty_state_.set(DirtyState::Viewport);
  }

  // enhanced barriers order work as ResourceBarrier does
  void STDMETHODCALLTYPE
  Barrier(UINT32 NumBarrierGroups, const D3D12_BARRIER_GROUP *pBarrierGroups) {
    WriteMarkers();
    barrier_serial_++;
    constexpr auto pixels =
        D3D12_BARRIER_SYNC_PIXEL_SHADING | D3D12_BARRIER_SYNC_DEPTH_STENCIL | D3D12_BARRIER_SYNC_RENDER_TARGET;
    // the first half of a split barrier (SyncAfter SPLIT) orders nothing: its second half (SyncBefore SPLIT) orders
    // the work before the first after it (Enhanced Barriers, "Split barriers")
    auto sync_after = [](const D3D12_BARRIER_GROUP &group, UINT32 i) {
      return group.Type == D3D12_BARRIER_TYPE_GLOBAL    ? group.pGlobalBarriers[i].SyncAfter
             : group.Type == D3D12_BARRIER_TYPE_TEXTURE ? group.pTextureBarriers[i].SyncAfter
                                                        : group.pBufferBarriers[i].SyncAfter;
    };
    bool vertices = false, orders = false;
    for (auto &group : std::span(pBarrierGroups, NumBarrierGroups))
      for (UINT32 i = 0; i < group.NumBarriers; i++) {
        auto after = sync_after(group, i);
        if (after == D3D12_BARRIER_SYNC_SPLIT)
          continue;
        orders = true;
        vertices |= !after || (after & ~pixels);
      }
    if (orders)
      OrderingPoint(vertices);
    // an access to or from NO_ACCESS hands memory from one resource to another that aliases it (Enhanced Barriers,
    // Resource Aliasing), after which another may take the addresses. once a reserved buffer exists, an access from
    // NO_ACCESS, which makes a resource the one that uses the memory, is ordered as ResourceBarrier orders an ALIASING
    // barrier; the access to NO_ACCESS before it needs nothing of its own, as that orders all the work before it
    bool aliasing = false, activates = false;
    for (auto &group : std::span(pBarrierGroups, NumBarrierGroups))
      for (UINT32 i = 0; i < group.NumBarriers; i++) {
        auto [before, after] =
            group.Type == D3D12_BARRIER_TYPE_GLOBAL
                ? std::pair{group.pGlobalBarriers[i].AccessBefore, group.pGlobalBarriers[i].AccessAfter}
                : group.Type == D3D12_BARRIER_TYPE_TEXTURE
                      ? std::pair{group.pTextureBarriers[i].AccessBefore, group.pTextureBarriers[i].AccessAfter}
                      : std::pair{group.pBufferBarriers[i].AccessBefore, group.pBufferBarriers[i].AccessAfter};
        aliasing |= before == D3D12_BARRIER_ACCESS_NO_ACCESS || after == D3D12_BARRIER_ACCESS_NO_ACCESS;
        activates |= before == D3D12_BARRIER_ACCESS_NO_ACCESS;
      }
    if (aliasing)
      va_found_ = nullptr;
    if (activates && device_->HasSparseBuffers()) {
      clears_.clear();
      named_.clear();
      allocator_->InvalidateCurrentPass();
      allocator_->AllocatePass<EncoderData>()->type = EncoderType::Settle;
      allocator_->InvalidateCurrentPass();
      return;
    }
    constexpr auto uav = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, draw_writes = uav | D3D12_BARRIER_ACCESS_RENDER_TARGET |
                       D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE | D3D12_BARRIER_ACCESS_STREAM_OUTPUT;
    // common access is any access, writes included
    auto any = [](D3D12_BARRIER_ACCESS access, D3D12_BARRIER_ACCESS of) {
      return access == D3D12_BARRIER_ACCESS_COMMON || (access != D3D12_BARRIER_ACCESS_NO_ACCESS && (access & of));
    };
    bool ordered = false;
    for (auto &group : std::span(pBarrierGroups, NumBarrierGroups))
      for (UINT32 i = 0; i < group.NumBarriers; i++) {
        auto [before, after] =
            group.Type == D3D12_BARRIER_TYPE_GLOBAL
                ? std::pair{group.pGlobalBarriers[i].AccessBefore, group.pGlobalBarriers[i].AccessAfter}
                : group.Type == D3D12_BARRIER_TYPE_TEXTURE
                      ? std::pair{group.pTextureBarriers[i].AccessBefore, group.pTextureBarriers[i].AccessAfter}
                      : std::pair{group.pBufferBarriers[i].AccessBefore, group.pBufferBarriers[i].AccessAfter};
        auto resource = group.Type == D3D12_BARRIER_TYPE_GLOBAL    ? nullptr
                        : group.Type == D3D12_BARRIER_TYPE_TEXTURE ? group.pTextureBarriers[i].pResource
                                                                   : group.pBufferBarriers[i].pResource;
        if (!clears_.empty())
          BarrierNames(resource);
        BarrierLeaves(resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATES(0));
        if (!ordered && sync_after(group, i) != D3D12_BARRIER_SYNC_SPLIT)
          ordered = Order(any(before, draw_writes) && PassWrote(resource), any(before, uav) || any(after, uav));
      }
  }

  // MetalFX (IMTLD3D12CommandListExt): a feature's scaler, and the inputs it was made for
  struct UpscalerCache final : IMTLD3D12Upscaler {
    void STDMETHODCALLTYPE
    Destroy() final {
      delete this;
    }
    std::array<uint32_t, 15> key{};
    Rc<TemporalScaler> temporal;
    Rc<DenoisedScaler> denoised;
    // display-resolution motion vectors brought to the input resolution (DownscaleMotion)
    WMT::Reference<WMT::Texture> motion;
    // what MetalFX's temporal scaler takes on the device, asked when the cache is made
    WMTFXTemporalScalerSupport support{};
    Rc<FrameInterpolator> interpolator;
  };

  // where content of `size` texels that starts at `offset` starts in a texture of `extent`: as far in as it fits
  static uint32_t
  ContentOffset(uint32_t offset, uint32_t size, uint32_t extent) {
    return size >= extent ? 0 : std::min(offset, extent - size);
  }

  // MetalFX reads motion vectors at the input resolution, in its pixels: those DLSS was given at the output resolution
  // (MotionVectorInDisplayRes) are read at each input pixel's place, scaled to pixels by `scale` and then to the input
  // resolution, into the cache's texture, in a pass before the upscale (as D3D11's MTLFXMVScaleContext)
  void
  DownscaleMotion(Texture *motion, WMT::Texture view, UpscalerCache *cache, float scale_x, float scale_y) {
    UsedTexture(motion);
    StartComputePass();
    auto &pso = allocator_->EncodeComputeCommand<wmtcmd_compute_setpso>();
    pso.type = WMTComputeCommandSetPSO;
    pso.pso = device_->downscale_motion;
    pso.threadgroup_size = {8, 4, 1};
    auto &from = allocator_->EncodeComputeCommand<wmtcmd_compute_settexture>();
    from.type = WMTComputeCommandSetTexture;
    from.texture = view.handle;
    from.index = 0;
    auto &to = allocator_->EncodeComputeCommand<wmtcmd_compute_settexture>();
    to.type = WMTComputeCommandSetTexture;
    to.texture = cache->motion.handle;
    to.index = 1;
    const float scale[2] = {scale_x, scale_y};
    auto &bytes = allocator_->EncodeComputeCommand<wmtcmd_compute_setbytes>();
    bytes.type = WMTComputeCommandSetBytes;
    bytes.bytes.set(memcpy(allocator_->AllocateCPUHeap(sizeof(scale), 16), scale, sizeof(scale)));
    bytes.length = sizeof(scale);
    bytes.index = 0;
    auto &dispatch = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch>();
    dispatch.type = WMTComputeCommandDispatchThreads;
    dispatch.size = {uint32_t(cache->motion.width()), uint32_t(cache->motion.height()), 1};
  }

  static Texture *
  UpscaleTexture(ID3D12Resource *resource) {
    return resource ? static_cast<MTLD3D12Resource *>(resource)->texture.ptr() : nullptr;
  }

  static WMT::Texture
  UpscaleView(Texture *texture, WMTPixelFormat format = WMTPixelFormatInvalid) {
    if (!texture)
      return {};
    if (format == WMTPixelFormatInvalid || format == texture->pixelFormat())
      return texture->view(texture->fullView).texture;
    return texture->view(texture->createView({.format = format, .type = WMTTextureType2D})).texture;
  }

  // depth and exposure as MetalFX reads them: a 32- or 16-bit texture a game created typeless is held as unsigned
  // integers, and MetalFX reads those bits as the float they hold (Starfield's depth and exposure are R32_TYPELESS)
  static WMTPixelFormat
  ScalarFloatFormat(WMTPixelFormat format) {
    switch (format) {
    case WMTPixelFormatR32Uint:
      return WMTPixelFormatR32Float;
    case WMTPixelFormatR16Uint:
      return WMTPixelFormatR16Float;
    default:
      return format;
    }
  }

  // a reactive mask as MetalFX reads it: a value from 0 to 1, held in a typeless resource as unsigned integers
  static WMTPixelFormat
  ReactiveMaskFormat(WMTPixelFormat format) {
    return format == WMTPixelFormatR8Uint ? WMTPixelFormatR8Unorm : ScalarFloatFormat(format);
  }

  // the pass, after everything before it and before everything after it
  UpscaleEncoderData *
  RecordUpscale() {
    OrderingPoint();
    allocator_->InvalidateCurrentPass();
    auto pass = allocator_->AllocatePass<UpscaleEncoderData>();
    pass->type = EncoderType::Upscale;
    allocator_->InvalidateCurrentPass();
    OrderingPoint();
    return pass;
  }

  static bool
  UpscaleImage(const Texture *texture) {
    return texture && texture->textureType() == WMTTextureType2D && texture->sampleCount() == 1;
  }

  static bool
  UpscaleSize(const Texture *texture, const Texture *reference) {
    return UpscaleImage(texture) && texture->width() == reference->width() && texture->height() == reference->height();
  }

  static bool
  UpscaleValues(float exposure, float scale_x, float scale_y, float jitter_x, float jitter_y) {
    return std::isfinite(exposure) && exposure > 0 && std::isfinite(scale_x) && std::isfinite(scale_y) &&
           std::isfinite(jitter_x) && std::isfinite(jitter_y);
  }

  static void
  UpscaleUnusual(bool unusual, unsigned which, const char *what) {
    static std::atomic<unsigned> logged{0};
    if (unusual && !(logged.fetch_or(1u << which) & (1u << which)))
      WARN("TemporalUpscale: upscaling ", what, " (logged once)");
  }

  HRESULT
  TemporalUpscale(const MTL_TEMPORAL_UPSCALE_D3D12_DESC *pDesc, IMTLD3D12Upscaler **ppCache) {
    if (!pDesc || !ppCache)
      return E_INVALIDARG;
    auto color = UpscaleTexture(pDesc->Color), output = UpscaleTexture(pDesc->Output),
         depth = UpscaleTexture(pDesc->Depth), motion = UpscaleTexture(pDesc->MotionVector),
         exposure = UpscaleTexture(pDesc->ExposureTexture), reactive = UpscaleTexture(pDesc->ReactiveMask);
    if (!color || !output || !depth || !motion)
      return E_INVALIDARG;
    // inputs DLSS would not be given are still upscaled, as they were before these checks: a game's ray traced
    // frame (Minecraft's) gives one of them and came out black when the upscale was refused. each is logged once
    const auto *motion_size = pDesc->MotionVectorInDisplayRes ? output : color;
    UpscaleUnusual(!UpscaleImage(color) || !UpscaleImage(output), 0, "a color or output texture that is not one 2D image");
    UpscaleUnusual(color == output, 1, "the same texture as input and output");
    UpscaleUnusual(!UpscaleSize(depth, color), 2, "depth of another size than the color");
    UpscaleUnusual(!UpscaleSize(motion, motion_size), 3, "motion vectors of another size than they are marked");
    UpscaleUnusual(output->width() < color->width() || output->height() < color->height(), 4,
                   "an output smaller than the color");
    UpscaleUnusual(pDesc->InputContentWidth > color->width() || pDesc->InputContentHeight > color->height(), 5,
                   "content larger than the color texture");
    UpscaleUnusual(pDesc->ExposureTexture && (!UpscaleImage(exposure) || exposure->width() != 1 || exposure->height() != 1),
                   6, "an exposure texture larger than one texel");
    UpscaleUnusual(!UpscaleValues(pDesc->PreExposure, pDesc->MotionVectorScaleX, pDesc->MotionVectorScaleY,
                                  pDesc->JitterOffsetX, pDesc->JitterOffsetY),
                   7, "a pre-exposure, motion scale or jitter that is not a finite number (pre-exposure above 0)");
    auto motion_format = MotionVectorFormat(motion->pixelFormat());
    // DLSS reads motion vectors through their format ("DLSS uses formatted reads so should handle most input buffer
    // formats", DLSS Programming Guide 3.3): those in one MetalFX does not take, which a shader reads as floats, are
    // read so by the pass below
    bool formatted = motion_format == WMTPixelFormatInvalid && device_->downscale_motion &&
                     !IsIntegerFormat(motion->pixelFormat()) && !DepthStencilPlanarFlags(motion->pixelFormat());
    if (motion_format == WMTPixelFormatInvalid && !formatted) {
      ERR("TemporalUpscale: motion vectors of format ", motion->pixelFormat(), " are not supported");
      return E_INVALIDARG;
    }
    auto cache = static_cast<UpscalerCache *>(*ppCache);
    if (!cache) {
      *ppCache = cache = new UpscalerCache;
      cache->support = device_->GetMTLDevice().temporalScalerSupport();
    }
    // motion vectors at the output resolution are read there by MetalFX where it reads them so (macOS 27).
    // elsewhere, or with the option
    // metalfx.outputResolutionMotionVectors off, they are brought to the input's (DownscaleMotion), as 32-bit floats
    static const bool output_motion =
        Config::getInstance().getOption<bool>("metalfx.outputResolutionMotionVectors", true);
    bool at_output = pDesc->MotionVectorInDisplayRes &&
                     (motion->width() != color->width() || motion->height() != color->height());
    bool native_motion = at_output && output_motion && cache->support.output_resolution_motion_vectors &&
                         motion_format != WMTPixelFormatInvalid && motion->width() == output->width() &&
                         motion->height() == output->height();
    bool display_motion = at_output && !native_motion && device_->downscale_motion;
    bool motion_pass = display_motion || formatted;
    auto scaler_motion_format = motion_pass ? WMTPixelFormatRG32Float : motion_format;
    // the pass writes the input resolution's motion vectors, or as many as it reads where they are at it already
    uint32_t pass_width = !motion_pass ? 0 : display_motion ? color->width() : motion->width(),
             pass_height = !motion_pass ? 0 : display_motion ? color->height() : motion->height();
    auto depth_format = ScalarFloatFormat(depth->pixelFormat());
    // the content, which takes the rest of the color's texture from where it starts where no size is given
    uint32_t content_width = pDesc->InputContentWidth
                                 ? pDesc->InputContentWidth
                                 : color->width() - std::min(pDesc->ColorOffsetX, color->width() - 1),
             content_height = pDesc->InputContentHeight
                                  ? pDesc->InputContentHeight
                                  : color->height() - std::min(pDesc->ColorOffsetY, color->height() - 1);
    // a reactive mask that holds the content is read: one of another kind or smaller is left out, as DLSS without one
    if (reactive &&
        (!UpscaleImage(reactive) || reactive->width() < content_width || reactive->height() < content_height ||
         IsIntegerFormat(ReactiveMaskFormat(reactive->pixelFormat())))) {
      static std::atomic_flag warned;
      if (!warned.test_and_set())
        WARN("TemporalUpscale: a reactive mask of format ", reactive->pixelFormat(), ", ", reactive->width(), "x",
             reactive->height(), ", is left out");
      reactive = nullptr;
    }
    auto reactive_format = reactive ? ReactiveMaskFormat(reactive->pixelFormat()) : WMTPixelFormatInvalid;
    std::array<uint32_t, 15> key{1,
                                 color->width(),
                                 color->height(),
                                 output->width(),
                                 output->height(),
                                 uint32_t(color->pixelFormat()),
                                 uint32_t(output->pixelFormat()),
                                 uint32_t(depth_format),
                                 uint32_t(scaler_motion_format),
                                 uint32_t(!!pDesc->AutoExposure),
                                 uint32_t(display_motion) | uint32_t(native_motion) << 1,
                                 uint32_t(!!pDesc->MotionVectorJittered),
                                 pass_width,
                                 pass_height,
                                 uint32_t(reactive_format)};
    if (!cache->temporal || cache->key != key) {
      WMTFXTemporalScalerInfo info{};
      info.color_format = color->pixelFormat();
      info.output_format = output->pixelFormat();
      info.depth_format = depth_format;
      info.motion_format = scaler_motion_format;
      info.input_width = color->width();
      info.input_height = color->height();
      info.output_width = output->width();
      info.output_height = output->height();
      // the scales MetalFX takes on the device, of which DLSS's optimal settings choose the render size (nvngx)
      info.input_content_min_scale = cache->support.input_content_min_scale;
      info.input_content_max_scale = cache->support.input_content_max_scale;
      info.auto_exposure = pDesc->AutoExposure;
      info.input_content_properties_enabled = true;
      // the scaler is returned before MetalFX has compiled its pipelines, and upscales more slowly until it has, to
      // the same image (MTLFXTemporalScalerDescriptor, requiresSynchronousInitialization): the list is not held
      // through the compilation each time a game changes its resolution or quality mode
      info.requires_synchronous_initialization = false;
      // the pass that brings motion vectors to the input resolution writes them in pixels and leaves their jitter in
      info.jittered_motion_vectors = pDesc->MotionVectorJittered;
      info.reactive_mask_format = reactive_format;
      info.output_resolution_motion_vectors = native_motion;
      cache->temporal = new TemporalScaler(device_->GetMTLDevice(), info, device_->UsesMetal4());
      if (!cache->temporal->scaler().handle) {
        cache->temporal = nullptr;
        ERR("TemporalUpscale: MetalFX has no temporal scaler for ", info.input_width, "x", info.input_height, " to ",
            info.output_width, "x", info.output_height);
        return E_FAIL;
      }
      cache->motion = {};
      if (motion_pass) {
        WMTTextureInfo motion_info{};
        motion_info.type = WMTTextureType2D;
        motion_info.pixel_format = WMTPixelFormatRG32Float;
        motion_info.width = pass_width;
        motion_info.height = pass_height;
        motion_info.depth = 1;
        motion_info.array_length = 1;
        motion_info.mipmap_level_count = 1;
        motion_info.sample_count = 1;
        motion_info.usage = WMTTextureUsageShaderRead | WMTTextureUsageShaderWrite;
        motion_info.options = WMTResourceStorageModePrivate;
        cache->motion = device_->GetMTLDevice().newTexture(motion_info);
        if (!cache->motion) {
          cache->temporal = nullptr;
          ERR("TemporalUpscale: no texture for motion vectors at ", pass_width, "x", pass_height);
          return E_FAIL;
        }
      }
      cache->key = key;
      Logger::info(str::format(
          "MetalFX: temporal upscaling ", info.input_width, "x", info.input_height, " to ", info.output_width, "x",
          info.output_height, device_->UsesMetal4() ? " (Metal 4)" : "",
          display_motion ? ", motion vectors brought from the output resolution" : "",
          native_motion ? ", motion vectors at the output resolution" : "",
          formatted ? str::format(", motion vectors read from format ", motion->pixelFormat()) : "",
          reactive ? ", with a reactive mask" : ""
      ));
      // MULLION_LOG_LEVEL=debug: what the game gives the scaler, for an image that comes out wrong
      if (Logger::logLevel() <= LogLevel::Debug)
        Logger::debug(str::format(
            "MetalFX: color format ", color->pixelFormat(), ", output format ", output->pixelFormat(), " usage ",
            output->usage(), ", depth format ", depth->pixelFormat(), " ", depth->width(), "x", depth->height(),
            ", motion format ", motion_format, " ", motion->width(), "x", motion->height(), ", exposure ",
            exposure ? str::format(exposure->pixelFormat(), " ", exposure->width(), "x", exposure->height()) : "none",
            ", auto exposure ", pDesc->AutoExposure, ", content ", pDesc->InputContentWidth, "x",
            pDesc->InputContentHeight, ", pre-exposure ", pDesc->PreExposure, ", motion scale ",
            pDesc->MotionVectorScaleX, ",", pDesc->MotionVectorScaleY, ", depth reversed ", pDesc->DepthReversed,
            ", motion at output resolution ", pDesc->MotionVectorInDisplayRes
        ));
    }
    // the pass writes the input resolution's motion vectors, or as many as it reads where they are at it already
    if (motion_pass)
      DownscaleMotion(
          motion, UpscaleView(motion, motion_format), cache, pDesc->MotionVectorScaleX, pDesc->MotionVectorScaleY
      );
    auto pass = RecordUpscale();
    pass->temporal = cache->temporal;
    pass->color = UpscaleView(color);
    pass->output = UpscaleView(output);
    pass->depth = UpscaleView(depth, depth_format);
    pass->motion = motion_pass ? WMT::Texture(cache->motion) : UpscaleView(motion, motion_format);
    pass->exposure =
        UpscaleView(exposure, exposure ? ScalarFloatFormat(exposure->pixelFormat()) : WMTPixelFormatInvalid);
    pass->reactive = UpscaleView(reactive, reactive_format);
    // where each input's content starts: within its texture, and in the motion vectors brought to the input resolution
    // where they are. MetalFX reads motion vectors at the output resolution over the content's part of them
    uint32_t motion_x = pDesc->MotionVectorOffsetX, motion_y = pDesc->MotionVectorOffsetY;
    if (display_motion) {
      motion_x = uint64_t(motion_x) * color->width() / motion->width();
      motion_y = uint64_t(motion_y) * color->height() / motion->height();
    }
    const auto *motion_read = display_motion ? color : motion;
    uint32_t motion_width = native_motion ? uint64_t(content_width) * motion->width() / color->width() : content_width,
             motion_height =
                 native_motion ? uint64_t(content_height) * motion->height() / color->height() : content_height;
    pass->props = WMTFXTemporalScalerProps{
        content_width,
        content_height,
        bool(pDesc->InReset),
        bool(pDesc->DepthReversed),
        motion_pass ? 1.0f : pDesc->MotionVectorScaleX,
        motion_pass ? 1.0f : pDesc->MotionVectorScaleY,
        pDesc->JitterOffsetX,
        pDesc->JitterOffsetY,
        pDesc->PreExposure,
        ContentOffset(pDesc->ColorOffsetX, content_width, color->width()),
        ContentOffset(pDesc->ColorOffsetY, content_height, color->height()),
        ContentOffset(pDesc->DepthOffsetX, content_width, depth->width()),
        ContentOffset(pDesc->DepthOffsetY, content_height, depth->height()),
        ContentOffset(motion_x, motion_width, motion_read->width()),
        ContentOffset(motion_y, motion_height, motion_read->height()),
        reactive ? ContentOffset(pDesc->ReactiveMaskOffsetX, content_width, reactive->width()) : 0,
        reactive ? ContentOffset(pDesc->ReactiveMaskOffsetY, content_height, reactive->height()) : 0,
    };
    auto &props = pass->props;
    if (!cache->support.content_offsets &&
        (props.color_offset_x || props.color_offset_y || props.depth_offset_x || props.depth_offset_y ||
         props.motion_offset_x || props.motion_offset_y || props.reactive_offset_x || props.reactive_offset_y)) {
      static std::atomic_flag warned;
      if (!warned.test_and_set())
        WARN("MetalFX: this macOS reads each input's content from its texture's corner, not where it starts");
    }
    return S_OK;
  }

  HRESULT
  DenoisedUpscale(const MTL_DENOISED_UPSCALE_D3D12_DESC *pDesc, IMTLD3D12Upscaler **ppCache) {
    if (!pDesc || !ppCache)
      return E_INVALIDARG;
    auto color = UpscaleTexture(pDesc->Color), output = UpscaleTexture(pDesc->Output),
         depth = UpscaleTexture(pDesc->Depth), motion = UpscaleTexture(pDesc->MotionVector),
         diffuse = UpscaleTexture(pDesc->DiffuseAlbedo), specular = UpscaleTexture(pDesc->SpecularAlbedo),
         normals = UpscaleTexture(pDesc->Normals),
         roughness = pDesc->RoughnessInNormalAlpha ? normals : UpscaleTexture(pDesc->Roughness),
         hit_distance = UpscaleTexture(pDesc->SpecularHitDistance), exposure = UpscaleTexture(pDesc->ExposureTexture);
    if (!color || !output || !depth || !motion || !diffuse || !specular || !normals || !roughness)
      return E_INVALIDARG;
    auto motion_format = MotionVectorFormat(motion->pixelFormat());
    if (motion_format == WMTPixelFormatInvalid) {
      ERR("DenoisedUpscale: motion vectors of format ", motion->pixelFormat(), " are not supported");
      return E_INVALIDARG;
    }
    auto cache = static_cast<UpscalerCache *>(*ppCache);
    if (!cache)
      *ppCache = cache = new UpscalerCache;
    auto depth_format = ScalarFloatFormat(depth->pixelFormat());
    std::array<uint32_t, 15> key{2,
                                 color->width(),
                                 color->height(),
                                 output->width(),
                                 output->height(),
                                 uint32_t(color->pixelFormat()),
                                 uint32_t(output->pixelFormat()),
                                 uint32_t(depth_format),
                                 uint32_t(motion_format),
                                 uint32_t(diffuse->pixelFormat()),
                                 uint32_t(specular->pixelFormat()),
                                 uint32_t(normals->pixelFormat()),
                                 uint32_t(roughness->pixelFormat()),
                                 uint32_t(hit_distance ? hit_distance->pixelFormat() : 0) << 1 | !!pDesc->AutoExposure};
    if (!cache->denoised || cache->key != key) {
      WMTFXDenoisedScalerInfo info{};
      info.color_format = color->pixelFormat();
      info.depth_format = depth_format;
      info.motion_format = motion_format;
      info.diffuse_albedo_format = diffuse->pixelFormat();
      info.specular_albedo_format = specular->pixelFormat();
      info.normal_format = normals->pixelFormat();
      info.roughness_format = roughness->pixelFormat();
      info.specular_hit_distance_format = hit_distance ? hit_distance->pixelFormat() : WMTPixelFormatInvalid;
      info.output_format = output->pixelFormat();
      info.input_width = color->width();
      info.input_height = color->height();
      info.output_width = output->width();
      info.output_height = output->height();
      info.auto_exposure = pDesc->AutoExposure;
      // as the temporal scaler's (TemporalUpscale)
      info.requires_synchronous_initialization = false;
      cache->denoised = new DenoisedScaler(device_->GetMTLDevice(), info, device_->UsesMetal4());
      if (!cache->denoised->scaler().handle) {
        cache->denoised = nullptr;
        ERR("DenoisedUpscale: MetalFX has no denoised scaler for ", info.input_width, "x", info.input_height, " to ",
            info.output_width, "x", info.output_height);
        return E_FAIL;
      }
      cache->key = key;
      Logger::info(str::format(
          "MetalFX: denoised upscaling ", info.input_width, "x", info.input_height, " to ", info.output_width, "x",
          info.output_height, device_->UsesMetal4() ? " (Metal 4)" : ""
      ));
    }
    auto pass = RecordUpscale();
    pass->denoised = cache->denoised;
    auto &args = pass->denoise;
    args.color = UpscaleView(color).handle;
    args.depth = UpscaleView(depth, depth_format).handle;
    args.motion = UpscaleView(motion, motion_format).handle;
    args.diffuse_albedo = UpscaleView(diffuse).handle;
    args.specular_albedo = UpscaleView(specular).handle;
    args.normal = UpscaleView(normals).handle;
    // MetalFX reads roughness from the first channel: packed in the normals' alpha, it is read through a view whose
    // red is their alpha
    args.roughness = pDesc->RoughnessInNormalAlpha
                         ? UpscaleView(normals, WMTPixelFormat(normals->pixelFormat() | WMTPixelFormatAGBASwizzle)).handle
                         : UpscaleView(roughness).handle;
    args.specular_hit_distance = UpscaleView(hit_distance).handle;
    args.exposure =
        UpscaleView(exposure, exposure ? ScalarFloatFormat(exposure->pixelFormat()) : WMTPixelFormatInvalid).handle;
    args.output = UpscaleView(output).handle;
    args.pre_exposure = pDesc->PreExposure;
    args.jitter_offset_x = pDesc->JitterOffsetX;
    args.jitter_offset_y = pDesc->JitterOffsetY;
    args.motion_vector_scale_x = pDesc->MotionVectorScaleX;
    args.motion_vector_scale_y = pDesc->MotionVectorScaleY;
    args.reset = pDesc->InReset;
    args.depth_reversed = pDesc->DepthReversed;
    memcpy(args.world_to_view, pDesc->WorldToView, sizeof(args.world_to_view));
    memcpy(args.view_to_clip, pDesc->ViewToClip, sizeof(args.view_to_clip));
    return S_OK;
  }

  HRESULT
  FrameInterpolate(const MTL_FRAME_INTERPOLATE_D3D12_DESC *pDesc, IMTLD3D12Upscaler **ppCache) {
    if (!pDesc || !ppCache)
      return E_INVALIDARG;
    auto color = UpscaleTexture(pDesc->Color), output = UpscaleTexture(pDesc->Output),
         depth = UpscaleTexture(pDesc->Depth), motion = UpscaleTexture(pDesc->MotionVector),
         ui = UpscaleTexture(pDesc->UI);
    if (!color || !output || !depth || !motion)
      return E_INVALIDARG;
    auto motion_format = MotionVectorFormat(motion->pixelFormat());
    if (motion_format == WMTPixelFormatInvalid) {
      ERR("FrameInterpolate: motion vectors of format ", motion->pixelFormat(), " are not supported");
      return E_INVALIDARG;
    }
    auto depth_format = ScalarFloatFormat(depth->pixelFormat());
    auto cache = static_cast<UpscalerCache *>(*ppCache);
    if (!cache)
      *ppCache = cache = new UpscalerCache;
    std::array<uint32_t, 15> key{3,
                                 depth->width(),
                                 depth->height(),
                                 output->width(),
                                 output->height(),
                                 uint32_t(color->pixelFormat()),
                                 uint32_t(output->pixelFormat()),
                                 uint32_t(depth_format),
                                 uint32_t(motion_format),
                                 uint32_t(ui ? ui->pixelFormat() : WMTPixelFormatInvalid)};
    if (!cache->interpolator || cache->key != key) {
      WMTFXFrameInterpolatorInfo info{};
      info.color_format = color->pixelFormat();
      info.output_format = output->pixelFormat();
      info.depth_format = depth_format;
      info.motion_format = motion_format;
      info.ui_format = ui ? ui->pixelFormat() : WMTPixelFormatInvalid;
      info.input_width = depth->width();
      info.input_height = depth->height();
      info.output_width = output->width();
      info.output_height = output->height();
      cache->interpolator = new FrameInterpolator(device_->GetMTLDevice(), info, device_->UsesMetal4());
      if (!cache->interpolator->interpolator().handle) {
        cache->interpolator = nullptr;
        ERR("FrameInterpolate: MetalFX has no frame interpolator for ", info.input_width, "x", info.input_height,
            " to ", info.output_width, "x", info.output_height);
        return E_FAIL;
      }
      cache->key = key;
      Logger::info(str::format(
          "MetalFX: frame interpolation at ", info.output_width, "x", info.output_height, " from ", info.input_width,
          "x", info.input_height, " depth and motion", ui ? ", with UI" : "", device_->UsesMetal4() ? " (Metal 4)" : ""
      ));
      if (Logger::logLevel() <= LogLevel::Debug)
        Logger::debug(str::format(
            "MetalFX: color format ", color->pixelFormat(), " ", color->width(), "x", color->height(),
            ", output format ", output->pixelFormat(), " usage ", output->usage(), ", depth format ",
            depth->pixelFormat(), ", motion format ", motion_format, " ", motion->width(), "x", motion->height(),
            ", UI ", ui ? str::format(ui->pixelFormat(), " composited ", pDesc->UIComposited) : "none",
            ", motion scale ", pDesc->MotionVectorScaleX, ",", pDesc->MotionVectorScaleY, ", near ", pDesc->Near,
            ", far ", pDesc->Far, ", fov ", pDesc->FieldOfView, ", aspect ", pDesc->AspectRatio, ", depth reversed ",
            pDesc->DepthReversed
        ));
    }
    auto pass = RecordUpscale();
    pass->interpolator = cache->interpolator;
    auto &args = pass->interpolate;
    args.color = UpscaleView(color).handle;
    args.depth = UpscaleView(depth, depth_format).handle;
    args.motion = UpscaleView(motion, motion_format).handle;
    args.ui = UpscaleView(ui).handle;
    args.output = UpscaleView(output).handle;
    args.motion_vector_scale_x = pDesc->MotionVectorScaleX;
    args.motion_vector_scale_y = pDesc->MotionVectorScaleY;
    args.delta_time = pDesc->DeltaTime;
    args.near_plane = pDesc->Near;
    args.far_plane = pDesc->Far;
    args.field_of_view = pDesc->FieldOfView;
    args.aspect_ratio = pDesc->AspectRatio;
    args.jitter_offset_x = pDesc->JitterOffsetX;
    args.jitter_offset_y = pDesc->JitterOffsetY;
    args.reset = pDesc->InReset;
    args.depth_reversed = pDesc->DepthReversed;
    args.ui_composited = ui && pDesc->UIComposited;
    memcpy(args.world_to_view, pDesc->WorldToView, sizeof(args.world_to_view));
    memcpy(args.view_to_clip, pDesc->ViewToClip, sizeof(args.view_to_clip));
    // the content of depth and motion where it fills part of their textures (a game with dynamic resolution renders
    // into the top left of textures of the largest size): as far in as it fits
    uint32_t content_width = pDesc->ContentWidth && pDesc->ContentWidth < depth->width() ? pDesc->ContentWidth : 0,
             content_height = pDesc->ContentHeight && pDesc->ContentHeight < depth->height() ? pDesc->ContentHeight : 0;
    args.content_width = args.content_height = 0;
    args.depth_offset_x = args.depth_offset_y = args.motion_offset_x = args.motion_offset_y = 0;
    if (content_width || content_height) {
      content_width = content_width ? content_width : depth->width();
      content_height = content_height ? content_height : depth->height();
      args.content_width = content_width;
      args.content_height = content_height;
      args.depth_offset_x = ContentOffset(pDesc->DepthOffsetX, content_width, depth->width());
      args.depth_offset_y = ContentOffset(pDesc->DepthOffsetY, content_height, depth->height());
      args.motion_offset_x = ContentOffset(pDesc->MotionVectorOffsetX, content_width, motion->width());
      args.motion_offset_y = ContentOffset(pDesc->MotionVectorOffsetY, content_height, motion->height());
    }
    return S_OK;
  }

  class UpscaleExt final : public IMTLD3D12CommandListExt {
  public:
    explicit UpscaleExt(MTLD3D12GraphicsCommandListImpl *list) : list_(list) {}

    HRESULT STDMETHODCALLTYPE
    QueryInterface(REFIID riid, void **ppvObject) final {
      return list_->QueryInterface(riid, ppvObject);
    }
    ULONG STDMETHODCALLTYPE
    AddRef() final {
      return list_->AddRef();
    }
    ULONG STDMETHODCALLTYPE
    Release() final {
      return list_->Release();
    }
    BOOL STDMETHODCALLTYPE
    SupportsTemporalUpscale() final {
      return list_->device_->GetMTLDevice().supportsFXTemporalScaler();
    }
    BOOL STDMETHODCALLTYPE
    SupportsDenoisedUpscale() final {
      return list_->device_->GetMTLDevice().supportsFXDenoisedScaler(list_->device_->UsesMetal4());
    }
    HRESULT STDMETHODCALLTYPE
    TemporalUpscale(const MTL_TEMPORAL_UPSCALE_D3D12_DESC *pDesc, IMTLD3D12Upscaler **ppCache) final {
      return list_->TemporalUpscale(pDesc, ppCache);
    }
    HRESULT STDMETHODCALLTYPE
    DenoisedUpscale(const MTL_DENOISED_UPSCALE_D3D12_DESC *pDesc, IMTLD3D12Upscaler **ppCache) final {
      return list_->DenoisedUpscale(pDesc, ppCache);
    }
    void STDMETHODCALLTYPE
    GetTemporalUpscaleScales(FLOAT *pMinScale, FLOAT *pMaxScale) final {
      auto support = list_->device_->GetMTLDevice().temporalScalerSupport();
      if (pMinScale)
        *pMinScale = support.input_content_min_scale;
      if (pMaxScale)
        *pMaxScale = support.input_content_max_scale;
    }
    BOOL STDMETHODCALLTYPE
    SupportsFrameInterpolation() final {
      // MetalFX keeps the frames it interpolates from from macOS 27
      uint64_t macos_major_version = 0;
      WMTGetOSVersion(&macos_major_version, nullptr, nullptr);
      return macos_major_version >= 27 && list_->device_->GetMTLDevice().supportsFXTemporalScaler();
    }
    HRESULT STDMETHODCALLTYPE
    FrameInterpolate(const MTL_FRAME_INTERPOLATE_D3D12_DESC *pDesc, IMTLD3D12Upscaler **ppCache) final {
      return list_->FrameInterpolate(pDesc, ppCache);
    }

  private:
    MTLD3D12GraphicsCommandListImpl *list_;
  };
  UpscaleExt upscale_ext_{this};
};

HRESULT STDMETHODCALLTYPE
MTLD3D12CommandAllocatorImpl::CreateCommandList(
    UINT NodeMask, D3D12_COMMAND_LIST_TYPE Type, ID3D12PipelineState *pInitialPipelineState, REFIID riid,
    void **ppCommandList
) {
  if (Type != type_)
    return FAIL(E_INVALIDARG);

  auto cmd_list = Com(new MTLD3D12GraphicsCommandListImpl(device_, Type));
  HRESULT hr = cmd_list->Initialize(this, pInitialPipelineState);
  if (FAILED(hr))
    return hr;
  return cmd_list->QueryInterface(riid, ppCommandList);
}

HRESULT
CreateClosedCommandList(MTLD3D12Device *pDevice, D3D12_COMMAND_LIST_TYPE Type, REFIID riid, void **ppCommandList) {
  InitReturnPtr(ppCommandList);
  auto cmd_list = Com(new MTLD3D12GraphicsCommandListImpl(pDevice, Type));
  cmd_list->InitializeClosed();
  return cmd_list->QueryInterface(riid, ppCommandList);
}

}; // namespace mullion