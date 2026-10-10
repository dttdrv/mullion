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

#include <functional>
#include "d3d12_command_allocator.hpp"
#include "d3d12_acceleration_structure.hpp"
#include "airconv_ray.h"
#include "com/com_pointer.hpp"
#include "dxmt_format.hpp"
#include "dxmt_geometry.hpp"
#include <span>
#include "util_env.hpp"

namespace dxmt {

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
  // the resolves the render pass runs when it ends
  std::vector<std::pair<D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_PARAMETERS, D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_SUBRESOURCE_PARAMETERS>>
      pass_resolves_;

  std::array<D3D12_VERTEX_BUFFER_VIEW, D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> vertex_buffers_;

  uint64_t index_buffer_address;
  WMT::Buffer index_buffer;
  WMTIndexType index_type;
  uint64_t index_offset;
  D3D12_INDEX_BUFFER_VIEW index_view_{}; // what tessellation's object stage reads indices through
  D3D12_STREAM_OUTPUT_BUFFER_VIEW so_views_[4]{};
  // per stream, the active statistics query's result, or 0
  uint64_t so_statistics_[4]{};

  UINT num_rtvs;
  D3D12_CPU_DESCRIPTOR_HANDLE rtvs[8];
  D3D12_CPU_DESCRIPTOR_HANDLE dsv;

  D3D12_PRIMITIVE_TOPOLOGY topology_;

  UINT num_viewports;
  D3D12_VIEWPORT
  viewports[D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {{}};

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

  template <typename Call>
  bool
  Record(Call &&call) {
    if (type_ != D3D12_COMMAND_LIST_TYPE_BUNDLE)
      return false;
    bundle_.emplace_back(std::forward<Call>(call));
    return true;
  }

  /* a root signature that indexes the heaps directly reads their addresses from its root arguments */
  void
  WriteHeapArguments(MTLD3D12RootSignature *rootsig, uint64_t *staging, DirtyState dirty) {
    if (!rootsig)
      return;
    if (rootsig->ResourceHeapQword != ~0u)
      staging[rootsig->ResourceHeapQword] = resource_heap_;
    if (rootsig->SamplerHeapQword != ~0u)
      staging[rootsig->SamplerHeapQword] = sampler_heap_;
    staging[rootsig->AtomicLocksQword] = device_->GetAtomicLocks();
    dirty_state_.set(dirty);
  }

  FLOAT blend_factor_[4];
  UINT8 stencil_ref_;
  // OMSetDepthBounds' range, NaN as 0; the pass has a range set when depth_bounds_set_
  FLOAT depth_bounds_[2];
  bool depth_bounds_set_ = false;

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
  std::vector<std::pair<MTLD3D12QueryHeap *, OcclusionSum>> occlusion_sums_;

  // a render pass counts samples in a new slot whenever the running queries change: Metal accumulates a slot within
  // a pass, and starts it over in the next
  void
  CountVisibility() {
    auto render = allocator_->encoder_current && allocator_->encoder_current->type == EncoderType::Render
                      ? static_cast<RenderEncoderData *>(allocator_->encoder_current)
                      : nullptr;
    if (!render)
      return;
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
  uint32_t *predication_region_ = nullptr;
  EncoderData *predication_pass_ = nullptr;

  void
  EndPredication() {
    if (!predication_region_)
      return;
    auto [mapped, offset] = allocator_->AllocateGPUHeap(predicated_.size() * sizeof(PredicatedWord), 4);
    if (allocator_->records_)
      allocator_->RecordGPUHeap(
          mapped, predicated_.size() * sizeof(PredicatedWord), "predicate words", 0, 0, predication_pass_
      );
    if (mapped)
      memcpy(mapped, predicated_.data(), predicated_.size() * sizeof(PredicatedWord));
    predication_region_[0] = predicated_.size();
    predication_region_[1] = offset / 4;
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
      predicate->skips = list->predication_region_ + 3;
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
    memset(rtvs, 0, sizeof(rtvs));
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
    stencil_ref_ = 0;
    depth_bounds_[0] = 0.0f;
    depth_bounds_[1] = 1.0f;

    rootsig_graphics_ = nullptr;
    memset(rootarg_graphics_staging_, 0, sizeof(rootarg_graphics_staging_));

    rootsig_compute_ = nullptr;
    memset(rootarg_compute_staging_, 0, sizeof(rootarg_compute_staging_));

    memset(vertex_buffers_.data(), 0, sizeof(vertex_buffers_));

    index_buffer_address = 0;
    index_buffer = {};
    index_type = {};
    index_offset = 0;
    index_view_ = {};

    dirty_state_.clrAll();
  }

  HRESULT
  Initialize(ID3D12CommandAllocator *pAllocator, ID3D12PipelineState *pInitialPipelineState) {
    auto allocator = static_cast<MTLD3D12CommandAllocatorImpl *>(pAllocator);

    if (allocator_ != allocator)
      allocator_ = allocator;

    ResetState(pInitialPipelineState);
    bundle_.clear();
    // a bundle's initial state is its first call
    if (pInitialPipelineState && type_ == D3D12_COMMAND_LIST_TYPE_BUNDLE)
      SetPipelineState(pInitialPipelineState);
    active_queries_.clear();
    visibility_slots_.clear();
    occlusion_sums_.clear();
    predicated_.clear();
    predication_region_ = nullptr;
    left_for_later_ = false;

    encoder_count = std::numeric_limits<size_t>::max();
    return allocator_->StartRecord(&entry);
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12CommandList) || riid == __uuidof(ID3D12GraphicsCommandList) ||
        riid == __uuidof(ID3D12GraphicsCommandList1) || riid == __uuidof(ID3D12GraphicsCommandList2) ||
        riid == __uuidof(ID3D12GraphicsCommandList3) || riid == __uuidof(ID3D12GraphicsCommandList4) ||
        riid == __uuidof(ID3D12GraphicsCommandList5) || riid == __uuidof(ID3D12GraphicsCommandList6) ||
        riid == __uuidof(ID3D12GraphicsCommandList7)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12GraphicsCommandList), riid)) {
      WARN("D3D12GraphicsCommandList: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE
  GetType() {
    return type_;
  }

  HRESULT STDMETHODCALLTYPE
  Close() {
    if (encoder_count < std::numeric_limits<size_t>::max())
      return E_FAIL;
    SumOcclusion();
    EndPredication();
    HRESULT hr = allocator_->EndRecord(&encoder_count);
    recorded_on = allocator_.ptr();
    // a list that did not fit is closed with nothing in it ("If an error was encountered during recording, the error
    // code is returned here", ID3D12GraphicsCommandList::Close)
    if (FAILED(hr))
      entry->next = nullptr;
    return hr;
  };

  HRESULT STDMETHODCALLTYPE
  Reset(ID3D12CommandAllocator *pAllocator, ID3D12PipelineState *pInitialState) {
    if (encoder_count == std::numeric_limits<size_t>::max())
      return E_FAIL;
    return Initialize(pAllocator, pInitialState);
  };

  void STDMETHODCALLTYPE
  ClearState(ID3D12PipelineState *pPipelineState) {
    // predication is state too
    EndPredication();
    allocator_->InvalidateCurrentPass();
    ResetState(pPipelineState);
  };

  std::tuple<uint64_t, uint64_t>
  PopulateVertexBufferTable(uint32_t Count, bool Written = false) {
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

    auto [mapped, offset] = allocator_->AllocateGPUHeap(stride * Count, 16);
    if (allocator_->records_ && !Written)
      allocator_->RecordGPUHeap(mapped, stride * Count, "vertex buffer table");

    for (unsigned i = 0; i < Count; i++) {
      VERTEX_BUFFER_ENTRY *entries = (VERTEX_BUFFER_ENTRY *)(reinterpret_cast<char *>(mapped) + i * stride);
      for (unsigned slot = 0, index = 0; slot < max_slot; slot++) {
        if (!(slot_mask & (1 << slot)))
          continue;
        auto &state = vertex_buffers_[slot];
        entries[index].buffer_handle = state.BufferLocation;
        entries[index].stride = state.StrideInBytes;
        entries[index++].length = state.SizeInBytes;
      };
    }

    return {offset, stride};
  }

  DrawCallStatus
  PreDraw(bool SkipResourceBinding = false) {
    if (!pso_graphics_)
      return DrawCallStatus::Invalid;
    if (!allocator_->encoder_current || allocator_->encoder_current->type != EncoderType::Render) {

      allocator_->InvalidateCurrentPass();
      auto render = allocator_->AllocatePass<RenderEncoderData>();
      render->type = EncoderType::Render;
      render->cmd_head.type = WMTRenderCommandNop;
      render->cmd_head.next.set(0);
      render->cmd_tail = (wmtcmd_base *)&render->cmd_head;
      render->before_head.type = WMTComputeCommandNop;
      render->before_head.next.set(0);
      render->before_tail = (wmtcmd_base *)&render->before_head;
      render->most = nullptr;
      render->dsv_planar_flags = 0;
      render->dsv_readonly_flags = 0;
      render->render_target_count = num_rtvs;

      unsigned render_target_width = 16384, render_target_height = 16384, render_target_array_length = 0;

      unsigned effective_rtvs = 0;
      for (unsigned i = 0; i < num_rtvs; i++) {
        if (!rtvs[i].ptr)
          continue;
        effective_rtvs++;
        auto [Heap, Index] = GetRenderTargetHeap(device_, rtvs[i]);
        auto AttachmentDesc = Heap->GetRenderTarget(Index);
        if (!AttachmentDesc.Texture)
          continue;
        auto &rt = render->colors[i];
        rt.attachment = AttachmentDesc.Texture->view(AttachmentDesc.View);
        rt.depth_plane = AttachmentDesc.DepthPlane;
        rt.load_action = WMTLoadActionLoad;
        rt.store_action = WMTStoreActionStore;
        render_target_width = std::min(render_target_width, AttachmentDesc.Width);
        render_target_height = std::min(render_target_height, AttachmentDesc.Height);
        render_target_array_length = std::max(render_target_array_length, AttachmentDesc.RenderTargetArrayLength);
      }
      while (dsv.ptr) {
        effective_rtvs++;
        auto [Heap, Index] = GetRenderTargetHeap(device_, dsv);
        auto AttachmentDesc = Heap->GetRenderTarget(Index);
        if (!AttachmentDesc.Texture)
          continue;
        auto dsv_planar_flags = DepthStencilPlanarFlags(AttachmentDesc.Texture->pixelFormat(AttachmentDesc.View));
        if (dsv_planar_flags & 1) {
          auto &rt = render->depth;
          rt.attachment = AttachmentDesc.Texture->view(AttachmentDesc.View);
          rt.depth_plane = 0; // DSV cannot be 3D
          rt.load_action = WMTLoadActionLoad;
          rt.store_action = WMTStoreActionStore;
        }
        if (dsv_planar_flags & 2) {
          auto &rt = render->stencil;
          rt.attachment = AttachmentDesc.Texture->view(AttachmentDesc.View);
          rt.depth_plane = 0; // DSV cannot be 3D
          rt.load_action = WMTLoadActionLoad;
          rt.store_action = WMTStoreActionStore;
        }
        render->dsv_planar_flags = dsv_planar_flags;
        // the planes the view does not write: the pipeline's depth stencil state leaves them alone
        render->dsv_readonly_flags =
            AttachmentDesc.Flags & (D3D12_DSV_FLAG_READ_ONLY_DEPTH | D3D12_DSV_FLAG_READ_ONLY_STENCIL);
        render_target_width = std::min(render_target_width, AttachmentDesc.Width);
        render_target_height = std::min(render_target_height, AttachmentDesc.Height);
        render_target_array_length = std::max(render_target_array_length, AttachmentDesc.RenderTargetArrayLength);
        break;
      }
      render->render_target_width = render_target_width;
      render->render_target_height = render_target_height;
      render->render_target_array_length = render_target_array_length;
      if (effective_rtvs == 0) {
        render->default_raster_sample_count = std::max(1u, pso_graphics_->forced_sample_count);
      }

      dirty_state_.set(DirtyState::VertexBuffer, DirtyState::GraphicsRootArguments, DirtyState::GraphicsRootSignature);
      dirty_state_.set(DirtyState::Viewport, DirtyState::ScissorRect);
      dirty_state_.set(DirtyState::BlendFactor, DirtyState::StencilRef, DirtyState::DepthBounds);
      depth_bounds_set_ = false;
      dirty_state_.set(DirtyState::GraphicsPipelineState);
      if (!active_queries_.empty())
        CountVisibility();
    }

    if (dirty_state_.test(DirtyState::GraphicsPipelineState)) {
      // a pipeline for a topology with adjacency is made here, and may fail
      auto pipeline = Pipeline();
      if (!pipeline)
        return DrawCallStatus::Invalid;
      auto &cmd_setpso = allocator_->EncodeRenderCommand<wmtcmd_render_setpso>();
      cmd_setpso.type = WMTRenderCommandSetPSO;
      cmd_setpso.pso = pipeline;
      if (Logger::logLevel() == LogLevel::Trace)
        TRACE("pass ", allocator_->encoder_current->id, ": graphics pipeline ", pso_graphics_->name);
      if (device_->NamesPasses())
        device_->NamePass(allocator_->encoder_current->id, pso_graphics_->name);

      auto &cmd_setdsso = allocator_->EncodeRenderCommand<wmtcmd_render_setdepthstencilstate>();
      cmd_setdsso.type = WMTRenderCommandSetDepthStencilState;
      cmd_setdsso.depth_stencil_state = pso_graphics_->GetDepthStencilState(
        static_cast<RenderEncoderData *>(allocator_->encoder_current)->dsv_planar_flags,
        static_cast<RenderEncoderData *>(allocator_->encoder_current)->dsv_readonly_flags
      );

      auto &cmd_setrs = allocator_->EncodeRenderCommand<wmtcmd_render_setrasterizerstate>();
      cmd_setrs.type = WMTRenderCommandSetRasterizerState;
      cmd_setrs.cull_mode = pso_graphics_->cull_mode;
      cmd_setrs.depth_clip_mode = pso_graphics_->depth_clip_mode;
      cmd_setrs.fill_mode = pso_graphics_->fill_mode;
      cmd_setrs.depth_bias = pso_graphics_->depth_bias;
      cmd_setrs.depth_bias_clamp = pso_graphics_->depth_bias_clamp;
      cmd_setrs.scole_scale = pso_graphics_->scole_scale;
      cmd_setrs.winding = pso_graphics_->winding;

      dirty_state_.clr(DirtyState::GraphicsPipelineState);
    }
    // under tessellation the vertex and hull shaders run in the object stage, the domain shader in the mesh stage; with
    // a geometry shader the vertex shader runs in the object stage, the geometry shader in the mesh stage
    // stream output without a geometry shader also rasterizes through the vertex stage
    // a mesh shader pipeline has an object stage (its amplification shader, if any) and a mesh stage
    bool mesh = pso_graphics_->threads_per_patch || Geometry() || pso_graphics_->mesh_shader;
    bool vertex = !mesh || StreamOutputRasterizes();
    auto geometry_buffer = [&](uint64_t Offset, uint8_t Index) {
      if (mesh) {
        EncodeBuffer(WMTRenderCommandSetObjectBuffer, Offset, Index);
        EncodeBuffer(WMTRenderCommandSetMeshBuffer, Offset, Index);
      }
      if (vertex)
        EncodeBuffer(WMTRenderCommandSetVertexBuffer, Offset, Index);
      EncodeBuffer(WMTRenderCommandSetFragmentBuffer, Offset, Index);
    };
    if (dirty_state_.test(DirtyState::VertexBuffer)) {
      auto [Offset, Stride] = PopulateVertexBufferTable(1);
      if (Stride && mesh)
        EncodeBuffer(WMTRenderCommandSetObjectBuffer, Offset, SM50_BINDING_INDEX_VERTEX_BUFFER);
      if (Stride && vertex)
        EncodeBuffer(WMTRenderCommandSetVertexBuffer, Offset, SM50_BINDING_INDEX_VERTEX_BUFFER);
      dirty_state_.clr(DirtyState::VertexBuffer);
    }

    if (dirty_state_.test(DirtyState::GraphicsRootArguments) && !SkipResourceBinding) {
      if (rootsig_graphics_)
        geometry_buffer(
            EncodeRootArgument(rootsig_graphics_.ptr(), rootarg_graphics_staging_), SM50_BINDING_INDEX_ROOT_ARGUMENTS
        );
      dirty_state_.clr(DirtyState::GraphicsRootArguments);
    }

    if (dirty_state_.test(DirtyState::GraphicsRootSignature) && !SkipResourceBinding) {
      if (rootsig_graphics_)
        geometry_buffer(EncodeStaticSamplers(rootsig_graphics_.ptr()), SM50_BINDING_INDEX_STATIC_SAMPLERS);
      dirty_state_.clr(DirtyState::GraphicsRootSignature);
    }

    // a primitive names one of the viewports and scissors there may be (D3D11.3 15.8.1). a viewport that is not set
    // is of no size and a scissor that is not set is empty (15.6, 15.7): nothing is drawn through either, which an
    // empty scissor says to Metal, whatever viewport it has there
    const UINT count = D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    if (dirty_state_.test(DirtyState::Viewport) && num_viewports) {
      auto metal_viewport = allocator_->AllocateCommandData<WMTViewport>(count);
      for (auto i = 0u; i < count; i++) {
        auto &viewport = viewports[i < num_viewports ? i : 0];
        metal_viewport[i] = {viewport.TopLeftX, viewport.TopLeftY, viewport.Width,
                             viewport.Height,   viewport.MinDepth, viewport.MaxDepth};
      }
      auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_setviewports>();
      cmd.type = WMTRenderCommandSetViewports;
      cmd.viewports.set(metal_viewport);
      cmd.viewport_count = count;
      dirty_state_.clr(DirtyState::Viewport);
    }

    if (dirty_state_.test(DirtyState::ScissorRect)) {
      // Metal wants scissors within the pass
      auto pass = static_cast<RenderEncoderData *>(allocator_->encoder_current);
      LONG width = pass->render_target_width, height = pass->render_target_height;
      auto metal_scissors = allocator_->AllocateCommandData<WMTScissorRect>(count);
      for (auto i = 0u; i < count; i++) {
        if (i < num_scissors && i < num_viewports) {
          auto &d3d_rect = scissors[i];
          LONG left = std::clamp(d3d_rect.left, (LONG)0, width);
          LONG top = std::clamp(d3d_rect.top, (LONG)0, height);
          LONG right = std::clamp(d3d_rect.right, left, width);
          LONG bottom = std::clamp(d3d_rect.bottom, top, height);
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
      dirty_state_.clr(DirtyState::StencilRef);
    }

    // stored depth passes inside the range, clamped to [0, 1]; an inverted range fails everything, and a pass without
    // a depth buffer passes. Metal's test is off at [0, 1]
    bool bounded = pso_graphics_->depth_bounds && dsv.ptr;
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

    return DrawCallStatus::Ordinary;
  }

  void
  EncodeBuffer(WMTRenderCommandType Type, uint64_t Offset, uint8_t Index) {
    auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
    cmd.type = Type;
    cmd.buffer = allocator_->gpu_heap_buffer_;
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
    auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_dxmt_tessellation_mesh_draw>();
    cmd.type = WMTRenderCommandDXMTTessellationMeshDraw;
    cmd.draw_arguments_offset = Offset + sizeof(inputs);
    cmd.instance_count = Args.InstanceCount;
    cmd.threads_per_patch = pso_graphics_->threads_per_patch;
    cmd.patch_per_group = per_group;
    cmd.patch_per_mesh_instance = (patches + per_group - 1) / per_group;
    cmd.parts = pso_graphics_->tessellation_parts;
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

  // stream output without a geometry shader then draws as usual, unless the mesh stage already has
  bool
  StreamOutputRasterizes() {
    return pso_graphics_->so_raster && !PassesAdjacency();
  }

  // the pipeline's variant for the topology: a geometry pipeline has one for strips, and with stream output one
  // for each that only counts
  WMT::RenderPipelineState
  Pipeline(bool counting = false) {
    bool strip = Strip(topology_);
    if (PassesAdjacency())
      return pso_graphics_->AdjacencyPipeline(strip, counting);
    if (counting)
      return strip ? pso_graphics_->so_count_strip : pso_graphics_->so_count;
    return pso_graphics_->geometry_shader && strip ? pso_graphics_->pso_strip : pso_graphics_->pso;
  }

  // a geometry-shader draw runs its vertices in warps of object threadgroups (airconv's layout), which read the draw's
  // D3D12 arguments and its index buffer view, empty for a draw without indices
  template <typename Arguments>
  void
  EncodeGeometryDraw(const Arguments &Args, uint32_t Count, const D3D12_INDEX_BUFFER_VIEW &View) {
    if (!Count || !Args.InstanceCount)
      return;
    auto [vertex_per_warp, increment] = get_gs_vertex_count(topology_, pso_graphics_->vertex_registers);
    // not one primitive fits an object threadgroup's payload
    if (!increment)
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
    EncodeBuffer(WMTRenderCommandSetObjectBuffer, Offset, SM50_BINDING_INDEX_INDEX_BUFFER);
    EncodeBuffer(WMTRenderCommandSetObjectBuffer, Offset + sizeof(View), SM50_BINDING_INDEX_DRAW_ARGUMENTS);
    uint32_t warps = (Count - 1) / increment + 1;
    auto draw = [&] {
      auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_dxmt_geometry_draw>();
      cmd.type = WMTRenderCommandDXMTGeometryDraw;
      cmd.draw_arguments_offset = Offset + sizeof(View);
      cmd.warp_count = warps;
      cmd.instance_count = Args.InstanceCount;
      cmd.vertex_per_warp = vertex_per_warp;
    };
    if (!pso_graphics_->stream_output)
      return draw();
    // stream output (SM50_STREAM_OUTPUT_TARGETS) runs the draw twice: once counting each geometry invocation's
    // primitives, and after a barrier again, writing them and drawing. its scratch area holds the filled sizes, a total
    // per stream and object threadgroup, and a count per stream and invocation (at most a primitive per vertex)
    SM50_STREAM_OUTPUT_TARGETS targets{};
    for (unsigned b = 0; b < std::size(so_views_); b++) {
      targets.address[b] = so_views_[b].BufferLocation;
      targets.size[b] = so_views_[b].SizeInBytes;
      targets.filled[b] = so_views_[b].BufferFilledSizeLocation;
      targets.statistics[b] = so_statistics_[b];
    }
    uint64_t groups = uint64_t(warps) * Args.InstanceCount;
    uint64_t scratch_bytes = sizeof(uint64_t) * 4 + sizeof(uint32_t) * 4 * groups * (1 + vertex_per_warp * pso_graphics_->gs_instances);
    auto [Scratch, ScratchOffset] = allocator_->AllocateGPUHeap(scratch_bytes, 16);
    // a draw whose scratch no heap has room for: the list fails when it is closed
    if (!Scratch)
      return;
    memset(Scratch, 0, scratch_bytes);
    targets.scratch = allocator_->gpu_heap_buffer_address_ + ScratchOffset;
    targets.warps = warps;
    targets.instances = Args.InstanceCount;
    auto [Targets, TargetsOffset] = allocator_->AllocateGPUHeap(sizeof(targets), 16);
    if (allocator_->records_)
      allocator_->RecordGPUHeap(Targets, sizeof(targets), "stream output targets");
    memcpy(Targets, &targets, sizeof(targets));
    EncodeBuffer(WMTRenderCommandSetObjectBuffer, TargetsOffset, SM50_BINDING_INDEX_STREAM_OUTPUT0);
    EncodeBuffer(WMTRenderCommandSetMeshBuffer, TargetsOffset, SM50_BINDING_INDEX_STREAM_OUTPUT0);
    auto set_pso = [&](WMT::RenderPipelineState pso) {
      auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_setpso>();
      cmd.type = WMTRenderCommandSetPSO;
      cmd.pso = pso;
    };
    auto barrier = [&](WMTRenderStages before) {
      auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_memory_barrier>();
      cmd.type = WMTRenderCommandMemoryBarrier;
      cmd.scope = WMTBarrierScopeBuffers;
      cmd.stages_after = WMTRenderStageMesh;
      cmd.stages_before = before;
    };
    set_pso(Pipeline(true));
    draw();
    barrier(WMTRenderStagePreRaster);
    set_pso(Pipeline());
    draw();
    // the next draw may read what this one wrote, as vertices or filled sizes
    barrier(WMTRenderStages(WMTRenderStagePreRaster | WMTRenderStageFragment));
    // without a geometry shader the caller then draws as usual
    if (StreamOutputRasterizes())
      set_pso(pso_graphics_->so_raster);
  }

  void STDMETHODCALLTYPE
  DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation) {
    if (Record([=](auto *l) { l->DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation); }))
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
      if (!StreamOutputRasterizes())
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
    if (Record([=](auto *l) { l->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartVertexLocation, BaseVertexLocation, StartInstanceLocation); }))
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
      if (!StreamOutputRasterizes())
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
  EncodeRootArgument(
      MTLD3D12RootSignature *pRootSig, uint64_t const pStaging[64], UINT Count = 1, bool Written = false
  ) {
    auto [Ptr, Offset] = allocator_->AllocateGPUHeap(sizeof(uint64_t) * pRootSig->UploadQwords * Count, 64);
    if (allocator_->records_ && !Written)
      allocator_->RecordGPUHeap(Ptr, sizeof(uint64_t) * pRootSig->UploadQwords * Count, "root argument table");
    for (unsigned i = 0; i < Count; i++)
      memcpy(
          reinterpret_cast<uint64_t *>(Ptr) + i * pRootSig->UploadQwords, pStaging,
          pRootSig->UploadQwords * sizeof(uint64_t)
      );
    return Offset;
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
  void
  StartComputePass() {
    allocator_->InvalidateCurrentPass();
    auto compute = allocator_->AllocatePass<ComputeEncoderData>();
    compute->type = EncoderType::Compute;
    compute->cmd_head.type = WMTComputeCommandNop;
    compute->cmd_head.next.set(0);
    compute->cmd_tail = (wmtcmd_base *)&compute->cmd_head;
    dirty_state_.set(DirtyState::ComputeRootArguments, DirtyState::ComputeRootSignature);
    dirty_state_.set(DirtyState::ComputePipelineState);
  }

  bool
  PreDispatch(bool SkipResourceBinding = false) {
    if (!allocator_->encoder_current || allocator_->encoder_current->type != EncoderType::Compute)
      StartComputePass();

    if (!pso_compute_)
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
        device_->NamePass(allocator_->encoder_current->id, pso_compute_->name);
    }
    BindComputeRootArguments(SkipResourceBinding);
    return true;
  }

  // the compute root signature's arguments and static samplers, for the pass's kernels
  void
  BindComputeRootArguments(bool SkipResourceBinding = false) {
    if (dirty_state_.test(DirtyState::ComputeRootArguments) && !SkipResourceBinding) {
      if (rootsig_compute_) {
        auto Offset = EncodeRootArgument(rootsig_compute_.ptr(), rootarg_compute_staging_);
        auto &cmd_argbuf = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
        cmd_argbuf.type = WMTComputeCommandSetBuffer;
        cmd_argbuf.buffer = allocator_->gpu_heap_buffer_;
        cmd_argbuf.offset = Offset;
        cmd_argbuf.index = SM50_BINDING_INDEX_ROOT_ARGUMENTS;
      }
      dirty_state_.clr(DirtyState::ComputeRootArguments);
    }

    if (dirty_state_.test(DirtyState::ComputeRootSignature) && !SkipResourceBinding) {
      if (rootsig_compute_) {
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
    if (Record([=](auto *l) { l->Dispatch(X, Y, Z); }))
      return;
    if (!PreDispatch())
      return;

    if (auto offset = Predicated(D3D12_DISPATCH_ARGUMENTS{X, Y, Z}, 0)) {
      auto &cmd = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch_indirect>();
      cmd.type = WMTComputeCommandDispatchIndirect;
      cmd.indirect_args_buffer = allocator_->gpu_heap_buffer_;
      cmd.indirect_args_offset = *offset;
      return;
    }
    if (auto at_once = pso_compute_->groups_work_together ? device_->ThreadsAtOnce() : 0) {
      auto &group = pso_compute_->threadgroup_size;
      DispatchParts({}, {X, Y, Z}, std::max<uint64_t>(at_once / (group.width * group.height * group.depth), 1));
      Isolate(X, Y, Z);
      return;
    }
    auto &cmd_dispatch = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch>();
    cmd_dispatch.type = WMTComputeCommandDispatch;
    cmd_dispatch.size = {X, Y, Z};
    Isolate(X, Y, Z);
  };

  // threadgroups that hand each other work wait for the one group that owns it. an Apple GPU starts every group of
  // a dispatch and, after a few milliseconds, takes turns among those it does not keep running (a few hundred groups
  // of a kernel with state of its own), so that each handover waits a whole round: seconds a dispatch, where a
  // desktop GPU runs the first groups to their end. such a dispatch goes in boxes of at most `at_once` groups, as
  // many as the GPU runs at once and well under what it keeps, one after another, each halved along its longest
  // side until it is one
  void
  DispatchParts(std::array<uint64_t, 3> origin, std::array<uint64_t, 3> size, uint64_t at_once) {
    if (size[0] * size[1] * size[2] > at_once) {
      auto axis = std::max_element(size.begin(), size.end()) - size.begin();
      auto half = size;
      half[axis] /= 2;
      DispatchParts(origin, half, at_once);
      origin[axis] += half[axis];
      size[axis] -= half[axis];
      return DispatchParts(origin, size, at_once);
    }
    auto &part = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch_part>();
    part.type = WMTComputeCommandDispatchPart;
    part.origin = {origin[0], origin[1], origin[2]};
    part.size = {size[0], size[1], size[2]};
  }

  // with DXMT_D3D12_ISOLATE set a dispatch is a pass of its own, which the queue then waits for and times
  void
  Isolate(UINT X, UINT Y, UINT Z) {
    static const bool isolate = !env::getEnvVar("DXMT_D3D12_ISOLATE").empty();
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

  void STDMETHODCALLTYPE
  CopyBufferRegion(
      ID3D12Resource *pDstBuffer, UINT64 DstOffset, ID3D12Resource *pSrcBuffer, UINT64 SrcOffset, UINT64 ByteCount
  ) {
    PredicatedPasses predicated(this);
    if (!pDstBuffer || !pSrcBuffer)
      return;
    if (!PreBlit())
      return;

    auto &cmd_cp = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
    cmd_cp.type = WMTBlitCommandCopyFromBufferToBuffer;
    cmd_cp.src = static_cast<MTLD3D12Resource *>(pSrcBuffer)->buffer->current()->buffer();
    cmd_cp.dst = static_cast<MTLD3D12Resource *>(pDstBuffer)->buffer->current()->buffer();
    cmd_cp.src_offset = SrcOffset;
    cmd_cp.dst_offset = DstOffset;
    cmd_cp.copy_length = ByteCount;
  };

  // a copy between textures. Metal copies within one format: formats of one type group copy from a view of the source
  // in the destination's. what has no such view (depth, and a reinterpret copy: CopyResource's Remarks) goes by its
  // bytes through a buffer, where a block of a compressed format is a texel of the other. `dst_extent` is the
  // destination level's
  void
  CopyTexels(
      Texture *src, const MTL_DXGI_FORMAT_DESC &src_format, uint32_t src_level, uint32_t src_slice, WMTOrigin src_origin,
      WMTSize size, Texture *dst, const MTL_DXGI_FORMAT_DESC &dst_format, uint32_t dst_level, uint32_t dst_slice,
      WMTOrigin dst_origin, const D3D12_BOX &dst_extent
  ) {
    bool same = src->pixelFormat() == dst->pixelFormat();
    bool viewed = !((src_format.Flag ^ dst_format.Flag) & MTL_DXGI_FORMAT_BC) &&
                  !DepthStencilPlanarFlags(src->pixelFormat()) && !DepthStencilPlanarFlags(dst->pixelFormat());
    if (same || viewed) {
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
    if (!PreBlit())
      return;

    auto src_desc = pSrc->pResource->GetDesc();
    auto dst_desc = pDst->pResource->GetDesc();
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
    }
    src_box = pSrcBox ? *pSrcBox : full_src_box;

    // discard invalid & empty box
    if (src_box.left >= src_box.right || src_box.front >= src_box.back || src_box.top >= src_box.bottom)
      return;

    if (pDst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
      auto &dst = static_cast<MTLD3D12Resource *>(pDst->pResource)->texture;
      if (!dst)
        return;

      DecomposeSubresource(dst_desc, pDst->SubresourceIndex, &dst_level, &dst_slice, &dst_planar);

      MTL_DXGI_FORMAT_DESC dst_format;
      if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), dst_desc.Format, dst_format))) {
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

        auto block_width = src_format.Flag & MTL_DXGI_FORMAT_BC ? 4 : 1;
        auto src_depth_pitch = dst->textureType() == WMTTextureType3D
                                   ? (pSrc->PlacedFootprint.Footprint.Height / block_width) * pSrc->PlacedFootprint.Footprint.RowPitch
                                   : 0;

        auto &cmd_cp = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture_withblitoption>();
        cmd_cp.type = WMTBlitCommandCopyFromBufferToTextureWithBlitOption;
        cmd_cp.src = src->current()->buffer();
        cmd_cp.src_offset = pSrc->PlacedFootprint.Offset + (src_box.left / block_width) * src_format.BytesPerTexel +
                            (src_box.top / block_width) * pSrc->PlacedFootprint.Footprint.RowPitch + src_box.front * src_depth_pitch;
        cmd_cp.bytes_per_row = pSrc->PlacedFootprint.Footprint.RowPitch;
        cmd_cp.bytes_per_image = src_depth_pitch;
        // a footprint is whole blocks, which a level smaller than one does not have all the texels of
        auto extent = GetResourceExtent(dst_desc, dst_level);
        cmd_cp.size = {std::min<uint64_t>(src_box.right - src_box.left, extent.right - DstX),
                       std::min<uint64_t>(src_box.bottom - src_box.top, extent.bottom - DstY),
                       src_box.back - src_box.front};
        cmd_cp.dst = dst->current()->texture();
        cmd_cp.level = dst_level;
        cmd_cp.slice = dst_slice;
        cmd_cp.options = (dst_planar_count > 1)
                             ? (dst_planar ? WMTBlitOptionStencilFromDepthStencil : WMTBlitOptionDepthFromDepthStencil)
                             : WMTBlitOptionNone;
        cmd_cp.origin = {DstX, DstY, DstZ};
      } else {
        auto &src = static_cast<MTLD3D12Resource *>(pSrc->pResource)->texture;
        if (!src)
          return;

        MTL_DXGI_FORMAT_DESC src_format;
        if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), src_desc.Format, src_format))) {
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
            src.ptr(), src_format, src_level, src_slice, {src_box.left, src_box.top, src_box.front},
            {src_box.right - src_box.left, src_box.bottom - src_box.top, src_box.back - src_box.front}, dst.ptr(),
            dst_format, dst_level, dst_slice, {DstX, DstY, DstZ}, GetResourceExtent(dst_desc, dst_level)
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
        auto &src = static_cast<MTLD3D12Resource *>(pSrc->pResource)->texture;
        if (!src)
          return;

        MTL_DXGI_FORMAT_DESC src_format;
        if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), src_desc.Format, src_format))) {
          WARN("CopyTextureRegion: unsupported format ", src_desc.Format);
          return;
        }
        auto src_planar_count = src_format.PlanarCount;

        auto block_width = dst_format.Flag & MTL_DXGI_FORMAT_BC ? 4 : 1;
        auto dst_depth_pitch = src->textureType() == WMTTextureType3D
                                   ? (pDst->PlacedFootprint.Footprint.Height / block_width) * pDst->PlacedFootprint.Footprint.RowPitch
                                   : 0;

        auto &cmd_cp = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer_withblitoption>();
        cmd_cp.type = WMTBlitCommandCopyFromTextureToBufferWithBlitOption;
        cmd_cp.src = src->current()->texture();
        cmd_cp.level = src_level;
        cmd_cp.slice = src_slice;
        cmd_cp.origin = {src_box.left, src_box.top, src_box.front};
        cmd_cp.size = {src_box.right - src_box.left, src_box.bottom - src_box.top, src_box.back - src_box.front};
        cmd_cp.dst = dst->current()->buffer();
        cmd_cp.offset = pDst->PlacedFootprint.Offset + (DstX / block_width) * dst_format.BytesPerTexel +
                        (DstY / block_width) * pDst->PlacedFootprint.Footprint.RowPitch + DstZ * dst_depth_pitch;
        cmd_cp.bytes_per_row = pDst->PlacedFootprint.Footprint.RowPitch;
        cmd_cp.bytes_per_image = dst_depth_pitch;
        cmd_cp.options = (src_planar_count > 1)
                             ? (src_planar ? WMTBlitOptionStencilFromDepthStencil : WMTBlitOptionDepthFromDepthStencil)
                             : WMTBlitOptionNone;
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

    if (!PreBlit())
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
    if (Record([=](auto *l) { l->IASetPrimitiveTopology(Topology); }))
      return;
    // a geometry pipeline has one Metal pipeline for strips and one for lists. a pipeline without a geometry shader
    // becomes one for a topology with adjacency, and binds its arguments to other stages
    if (pso_graphics_ && (pso_graphics_->geometry_shader || pso_graphics_->adjacency) && Strip(Topology) != Strip(topology_))
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
    if (Record([=](auto *l) { l->OMSetBlendFactor(f); }))
      return;
    std::copy_n(f, std::size(f), blend_factor_);
    dirty_state_.set(DirtyState::BlendFactor);
  };

  void STDMETHODCALLTYPE
  OMSetStencilRef(UINT StencilRef) {
    if (Record([=](auto *l) { l->OMSetStencilRef(StencilRef); }))
      return;
    if (stencil_ref_ == (UINT8)StencilRef)
      return;
    stencil_ref_ = (UINT8)StencilRef;
    dirty_state_.set(DirtyState::StencilRef);
  };

  void STDMETHODCALLTYPE
  SetPipelineState(ID3D12PipelineState *pPSO) {
    if (Record([pso = Com<ID3D12PipelineState>(pPSO)](auto *l) { l->SetPipelineState(pso.ptr()); }))
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
    // tessellation, geometry shaders and stream output bind root arguments to other stages
    auto stages = [&](MTLD3D12GraphicsPipelineState *p) {
      bool adjacency = p->adjacency && Adjacency(topology_);
      return (p->threads_per_patch || p->geometry_shader || p->mesh_shader || adjacency) | (p->so_raster && !adjacency) << 1;
    };
    if (!pso_graphics_ || stages(pso_graphics_.ptr()) != stages(graphics_pso))
      dirty_state_.set(DirtyState::GraphicsRootArguments, DirtyState::GraphicsRootSignature);
    pso_graphics_ = graphics_pso;
    pso_compute_ = nullptr;
    dirty_state_.set(DirtyState::GraphicsPipelineState, DirtyState::ComputePipelineState, DirtyState::VertexBuffer);
    dirty_state_.set(DirtyState::DepthBounds);
  };

  // passes are already ordered by the queue's fence (each waits for the one before), so a barrier matters only inside
  // a render pass, where draws run unordered: one that orders writes a draw made (UAV, render target, depth, stream
  // output) or aliases memory ends the pass, and the next draw opens one that loads what this one stored
  void STDMETHODCALLTYPE ResourceBarrier(UINT Count, const D3D12_RESOURCE_BARRIER *barriers) {
    if (!allocator_->encoder_current || allocator_->encoder_current->type != EncoderType::Render)
      return;
    constexpr auto draw_writes =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_DEPTH_WRITE |
        D3D12_RESOURCE_STATE_STREAM_OUT;
    for (auto &barrier : std::span(barriers, Count)) {
      if (barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || (barrier.Transition.StateBefore & draw_writes)) {
        allocator_->InvalidateCurrentPass();
        return;
      }
    }
  };

  void STDMETHODCALLTYPE
  ExecuteBundle(ID3D12GraphicsCommandList *CommandList) {
    for (auto &call : static_cast<MTLD3D12GraphicsCommandListImpl *>(CommandList)->bundle_)
      call(this);
  };

  void STDMETHODCALLTYPE SetDescriptorHeaps(UINT HeapCount, ID3D12DescriptorHeap *const *Heaps) {
    if (Record([heaps = std::vector<Com<ID3D12DescriptorHeap>>(Heaps, Heaps + HeapCount)](auto *l) { l->SetDescriptorHeaps(heaps.size(), reinterpret_cast<ID3D12DescriptorHeap *const *>(heaps.data())); }))
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
    if (Record([rs = Com<ID3D12RootSignature>(pRootSignature)](auto *l) { l->SetComputeRootSignature(rs.ptr()); }))
      return;
    if (rootsig_compute_.ptr() == pRootSignature)
      return;
    if (pRootSignature) {
      rootsig_compute_ = static_cast<MTLD3D12RootSignature *>(pRootSignature);
      assert(rootsig_compute_->UploadQwords <= std::size(rootarg_compute_staging_));
      WriteHeapArguments(rootsig_compute_.ptr(), rootarg_compute_staging_, DirtyState::ComputeRootArguments);
    } else {
      rootsig_compute_ = nullptr;
    }
    dirty_state_.set(DirtyState::ComputeRootArguments, DirtyState::ComputeRootSignature);
  };

  void STDMETHODCALLTYPE
  SetGraphicsRootSignature(ID3D12RootSignature *pRootSignature) {
    if (Record([rs = Com<ID3D12RootSignature>(pRootSignature)](auto *l) { l->SetGraphicsRootSignature(rs.ptr()); }))
      return;
    if (rootsig_graphics_.ptr() == pRootSignature)
      return;
    if (pRootSignature) {
      rootsig_graphics_ = static_cast<MTLD3D12RootSignature *>(pRootSignature);
      assert(rootsig_graphics_->UploadQwords <= std::size(rootarg_graphics_staging_));
      WriteHeapArguments(rootsig_graphics_.ptr(), rootarg_graphics_staging_, DirtyState::GraphicsRootArguments);
    } else {
      rootsig_graphics_ = nullptr;
    }
    dirty_state_.set(DirtyState::GraphicsRootArguments, DirtyState::GraphicsRootSignature);
  };

  void STDMETHODCALLTYPE SetComputeRootDescriptorTable(UINT Index, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) {
    if (Record([=](auto *l) { l->SetComputeRootDescriptorTable(Index, BaseDescriptor); }))
      return;
    if (!rootsig_compute_)
      return;
    if (Index >= rootsig_compute_->ParameterSlots)
      return;
    rootarg_compute_staging_[rootsig_compute_->SlotQwordOffsets[Index]] = BaseDescriptor.ptr;
    dirty_state_.set(DirtyState::ComputeRootArguments);
  };

  void STDMETHODCALLTYPE
  SetGraphicsRootDescriptorTable(UINT Index, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) {
    if (Record([=](auto *l) { l->SetGraphicsRootDescriptorTable(Index, BaseDescriptor); }))
      return;
    if (!rootsig_graphics_)
      return;
    if (Index >= rootsig_graphics_->ParameterSlots)
      return;
    rootarg_graphics_staging_[rootsig_graphics_->SlotQwordOffsets[Index]] = BaseDescriptor.ptr;
    dirty_state_.set(DirtyState::GraphicsRootArguments);
  };

  void STDMETHODCALLTYPE SetComputeRoot32BitConstant(UINT Index, UINT Data, UINT DstOffset) {
    if (Record([=](auto *l) { l->SetComputeRoot32BitConstant(Index, Data, DstOffset); }))
      return;
    if (!rootsig_compute_)
      return;
    if (Index >= rootsig_compute_->ParameterSlots)
      return;
    auto dst = reinterpret_cast<uint32_t *>(rootarg_compute_staging_ + rootsig_compute_->SlotQwordOffsets[Index]);
    dst[DstOffset] = Data;
    dirty_state_.set(DirtyState::ComputeRootArguments);
  };

  void STDMETHODCALLTYPE
  SetGraphicsRoot32BitConstant(UINT Index, UINT Data, UINT DstOffset) {
    if (Record([=](auto *l) { l->SetGraphicsRoot32BitConstant(Index, Data, DstOffset); }))
      return;
    if (!rootsig_graphics_)
      return;
    if (Index >= rootsig_graphics_->ParameterSlots)
      return;
    auto dst = reinterpret_cast<uint32_t *>(rootarg_graphics_staging_ + rootsig_graphics_->SlotQwordOffsets[Index]);
    dst[DstOffset] = Data;
    dirty_state_.set(DirtyState::GraphicsRootArguments);
  };

  void STDMETHODCALLTYPE
  SetComputeRoot32BitConstants(UINT Index, UINT ConstantCount, const void *pData, UINT DstOffset) {
    if (Record([=, v = std::vector((const UINT *)pData, (const UINT *)pData + ConstantCount)](auto *l) { l->SetComputeRoot32BitConstants(Index, ConstantCount, v.data(), DstOffset); }))
      return;
    if (!rootsig_compute_)
      return;
    if (Index >= rootsig_compute_->ParameterSlots)
      return;
    auto src = reinterpret_cast<const uint32_t *>(pData);
    auto dst = reinterpret_cast<uint32_t *>(rootarg_compute_staging_ + rootsig_compute_->SlotQwordOffsets[Index]);
    for (unsigned i = 0; i < ConstantCount; i++) {
      dst[i + DstOffset] = src[i];
    }
    dirty_state_.set(DirtyState::ComputeRootArguments);
  };

  void STDMETHODCALLTYPE
  SetGraphicsRoot32BitConstants(UINT Index, UINT ConstantCount, const void *pData, UINT DstOffset) {
    if (Record([=, v = std::vector((const UINT *)pData, (const UINT *)pData + ConstantCount)](auto *l) { l->SetGraphicsRoot32BitConstants(Index, ConstantCount, v.data(), DstOffset); }))
      return;
    if (!rootsig_graphics_)
      return;
    if (Index >= rootsig_graphics_->ParameterSlots)
      return;
    auto src = reinterpret_cast<const uint32_t *>(pData);
    auto dst = reinterpret_cast<uint32_t *>(rootarg_graphics_staging_ + rootsig_graphics_->SlotQwordOffsets[Index]);
    for (unsigned i = 0; i < ConstantCount; i++) {
      dst[i + DstOffset] = src[i];
    }
    dirty_state_.set(DirtyState::GraphicsRootArguments);
  };

  void STDMETHODCALLTYPE SetComputeRootConstantBufferView(UINT Index, D3D12_GPU_VIRTUAL_ADDRESS VA) {
    if (Record([=](auto *l) { l->SetComputeRootConstantBufferView(Index, VA); }))
      return;
    if (!rootsig_compute_)
      return;
    if (Index >= rootsig_compute_->ParameterSlots)
      return;
    rootarg_compute_staging_[rootsig_compute_->SlotQwordOffsets[Index]] = VA;
    dirty_state_.set(DirtyState::ComputeRootArguments);
  };

  void STDMETHODCALLTYPE
  SetGraphicsRootConstantBufferView(UINT Index, D3D12_GPU_VIRTUAL_ADDRESS VA) {
    if (Record([=](auto *l) { l->SetGraphicsRootConstantBufferView(Index, VA); }))
      return;
    if (!rootsig_graphics_)
      return;
    if (Index >= rootsig_graphics_->ParameterSlots)
      return;
    rootarg_graphics_staging_[rootsig_graphics_->SlotQwordOffsets[Index]] = VA;
    dirty_state_.set(DirtyState::GraphicsRootArguments);
  };

  void STDMETHODCALLTYPE SetComputeRootShaderResourceView(UINT Index, D3D12_GPU_VIRTUAL_ADDRESS VA) {
    if (Record([=](auto *l) { l->SetComputeRootShaderResourceView(Index, VA); }))
      return;
    if (!rootsig_compute_)
      return;
    if (Index >= rootsig_compute_->ParameterSlots)
      return;
    rootarg_compute_staging_[rootsig_compute_->SlotQwordOffsets[Index]] = VA;
    dirty_state_.set(DirtyState::ComputeRootArguments);
  };

  void STDMETHODCALLTYPE
  SetGraphicsRootShaderResourceView(UINT Index, D3D12_GPU_VIRTUAL_ADDRESS VA) {
    if (Record([=](auto *l) { l->SetGraphicsRootShaderResourceView(Index, VA); }))
      return;
    if (!rootsig_graphics_)
      return;
    if (Index >= rootsig_graphics_->ParameterSlots)
      return;
    rootarg_graphics_staging_[rootsig_graphics_->SlotQwordOffsets[Index]] = VA;
    dirty_state_.set(DirtyState::GraphicsRootArguments);
  };

  void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(UINT Index, D3D12_GPU_VIRTUAL_ADDRESS VA) {
    if (Record([=](auto *l) { l->SetComputeRootUnorderedAccessView(Index, VA); }))
      return;
    if (!rootsig_compute_)
      return;
    if (Index >= rootsig_compute_->ParameterSlots)
      return;
    rootarg_compute_staging_[rootsig_compute_->SlotQwordOffsets[Index]] = VA;
    dirty_state_.set(DirtyState::ComputeRootArguments);
  };

  void STDMETHODCALLTYPE
  SetGraphicsRootUnorderedAccessView(UINT Index, D3D12_GPU_VIRTUAL_ADDRESS VA) {
    if (Record([=](auto *l) { l->SetGraphicsRootUnorderedAccessView(Index, VA); }))
      return;
    if (!rootsig_graphics_)
      return;
    if (Index >= rootsig_graphics_->ParameterSlots)
      return;
    rootarg_graphics_staging_[rootsig_graphics_->SlotQwordOffsets[Index]] = VA;
    dirty_state_.set(DirtyState::GraphicsRootArguments);
  };

  void STDMETHODCALLTYPE
  IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW *pView) {
    if (Record([view = pView ? std::optional(*pView) : std::nullopt](auto *l) { l->IASetIndexBuffer(view ? &*view : nullptr); }))
      return;
    auto index_buffer_allocation = pView ? device_->LookupBufferByVA(pView->BufferLocation, &index_offset) : nullptr;
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
    if (Record([=, v = Views ? std::vector(Views, Views + Count) : std::vector<D3D12_VERTEX_BUFFER_VIEW>()](auto *l) { l->IASetVertexBuffers(StartSlot, Count, Views ? v.data() : nullptr); }))
      return;
    if (!Views)
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
    allocator_->InvalidateCurrentPass();

    num_rtvs = NumRTV;
    for (unsigned i = 0; i < NumRTV; i++) {
      auto RTV = SingleDescriptor ? D3D12_CPU_DESCRIPTOR_HANDLE{RTVs[0].ptr + i * 32 /* kRTVDSVHeapIncrementalSize */}
                                  : RTVs[i];
      rtvs[i] = RTV;
    }
    dsv = DSV ? *DSV : D3D12_CPU_DESCRIPTOR_HANDLE();
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
    if (Rects) {
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
    allocator_->InvalidateCurrentPass();
    auto encoder_info = allocator_->AllocatePass<ClearEncoderData>();
    encoder_info->type = EncoderType::Clear;
    encoder_info->clear_dsv = CheckedFlags;
    encoder_info->depth_stencil = {Depth, Stencil};
    encoder_info->attachment = AttachmentDesc.Texture->view(AttachmentDesc.View);
    encoder_info->array_length = AttachmentDesc.RenderTargetArrayLength;
    encoder_info->width = AttachmentDesc.Width;
    encoder_info->height = AttachmentDesc.Height;
    encoder_info->depth_plane = 0;

    allocator_->InvalidateCurrentPass();
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
    if (Rects) {
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
    allocator_->InvalidateCurrentPass();
    auto encoder_info = allocator_->AllocatePass<ClearEncoderData>();
    encoder_info->type = EncoderType::Clear;
    encoder_info->clear_dsv = 0;
    encoder_info->color = {Color[0], Color[1], Color[2], Color[3]};
    SanitizeRTVClearColor(AttachmentDesc.Texture->pixelFormat(AttachmentDesc.View), encoder_info->color);
    encoder_info->attachment = AttachmentDesc.Texture->view(AttachmentDesc.View);
    encoder_info->array_length = AttachmentDesc.RenderTargetArrayLength;
    encoder_info->width = AttachmentDesc.Width;
    encoder_info->height = AttachmentDesc.Height;
    encoder_info->depth_plane = AttachmentDesc.DepthPlane;

    allocator_->InvalidateCurrentPass();
  };

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
    D3D12_RECT full_rect;
    switch (Descriptor.type) {
    case ShaderVisibleDescriptorType::UAVBuffer: {
      allocator_->clear_uav_.begin(color, Descriptor.UAVBuffer.buffer);
      full_rect = {
          (LONG)Descriptor.UAVBuffer.slice.byteOffset >> 2, 0,
          (LONG)((Descriptor.UAVBuffer.slice.byteOffset + Descriptor.UAVBuffer.slice.byteLength) >> 2), 1
      };
      break;
    }
    case ShaderVisibleDescriptorType::UAVTexture: {
      allocator_->clear_uav_.begin(color, Descriptor.UAVTexture.texture, Descriptor.UAVTexture.view);
      full_rect = {
          0, 0, (LONG)Descriptor.UAVTexture.texture->width(Descriptor.UAVTexture.view),
          (LONG)Descriptor.UAVTexture.texture->height(Descriptor.UAVTexture.view)
      };
      break;
    }
    case ShaderVisibleDescriptorType::UAVTexelBuffer: {
      allocator_->clear_uav_.begin(color, Descriptor.UAVTexelBuffer.buffer, Descriptor.UAVTexelBuffer.view);
      full_rect = {
          (LONG)Descriptor.UAVTexelBuffer.slice.firstElement, 0,
          (LONG)(Descriptor.UAVTexelBuffer.slice.firstElement + Descriptor.UAVTexelBuffer.slice.elementCount), 1
      };
      break;
    }
    default:
      allocator_->clear_uav_.end();
      return;
    }

    const D3D12_RECT *rects = RectCount > 0 ? pRects : &full_rect;
    UINT rect_count = RectCount > 0 ? RectCount : 1;

    for (unsigned i = 0; i < rect_count; i++) {
      auto &rect = rects[i];
      auto width = rect.right - rect.left;
      auto height = rect.bottom - rect.top;
      if (width <= 0 || height <= 0)
        continue;
      allocator_->clear_uav_.clear(rect.left, rect.top, width, height);
    }

    allocator_->clear_uav_.end();
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
    D3D12_RECT full_rect;
    switch (Descriptor.type) {
    case ShaderVisibleDescriptorType::UAVBuffer: {
      allocator_->clear_uav_.begin(color, Descriptor.UAVBuffer.buffer);
      full_rect = {
          (LONG)Descriptor.UAVBuffer.slice.byteOffset >> 2, 0,
          (LONG)((Descriptor.UAVBuffer.slice.byteOffset + Descriptor.UAVBuffer.slice.byteLength) >> 2), 1
      };
      break;
    }
    case ShaderVisibleDescriptorType::UAVTexture: {
      allocator_->clear_uav_.begin(color, Descriptor.SRVTexture.texture, Descriptor.SRVTexture.view);
      full_rect = {
          0, 0, (LONG)Descriptor.SRVTexture.texture->width(Descriptor.SRVTexture.view),
          (LONG)Descriptor.SRVTexture.texture->height(Descriptor.SRVTexture.view)
      };
      break;
    }
    case ShaderVisibleDescriptorType::UAVTexelBuffer: {
      allocator_->clear_uav_.begin(color, Descriptor.UAVTexelBuffer.buffer, Descriptor.UAVTexelBuffer.view);
      full_rect = {
          (LONG)Descriptor.UAVTexelBuffer.slice.firstElement, 0,
          (LONG)(Descriptor.UAVTexelBuffer.slice.firstElement + Descriptor.UAVTexelBuffer.slice.elementCount), 1
      };
      break;
    }
    default:
      allocator_->clear_uav_.end();
      return;
    }

    const D3D12_RECT *rects = RectCount > 0 ? pRects : &full_rect;
    UINT rect_count = RectCount > 0 ? RectCount : 1;

    for (unsigned i = 0; i < rect_count; i++) {
      auto &rect = rects[i];
      auto width = rect.right - rect.left;
      auto height = rect.bottom - rect.top;
      if (width <= 0 || height <= 0)
        continue;
      allocator_->clear_uav_.clear(rect.left, rect.top, width, height);
    }

    allocator_->clear_uav_.end();
  };

  void STDMETHODCALLTYPE DiscardResource(ID3D12Resource *pResource, const D3D12_DISCARD_REGION *pRegion) {
    // do nothing for now
  };

  void STDMETHODCALLTYPE
  BeginQuery(ID3D12QueryHeap *pHeap, D3D12_QUERY_TYPE Type, UINT Index) {
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
  };

  void STDMETHODCALLTYPE
  EndQuery(ID3D12QueryHeap *pHeap, D3D12_QUERY_TYPE Type, UINT Index) {
    auto heap = static_cast<MTLD3D12QueryHeap *>(pHeap);
    if (Type >= D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 && Type <= D3D12_QUERY_TYPE_SO_STATISTICS_STREAM3) {
      so_statistics_[Type - D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0] = 0;
      return;
    }
    if (Type == D3D12_QUERY_TYPE_TIMESTAMP) {
      // the clock when the work before is done: a blit encoder of its own samples it as it starts. Metal samples only
      // encoders that do something
      allocator_->InvalidateCurrentPass();
      PreBlit();
      auto blit = static_cast<BlitEncoderData *>(allocator_->encoder_current);
      blit->timestamps = heap;
      blit->timestamp_query = Index;
      auto [_, offset] = allocator_->AllocateGPUHeap(sizeof(uint32_t), sizeof(uint32_t));
      auto &fill = allocator_->EncodeBlitCommand<wmtcmd_blit_fillbuffer>();
      fill.type = WMTBlitCommandFillBuffer;
      fill.buffer = allocator_->gpu_heap_buffer_;
      fill.offset = offset;
      fill.length = sizeof(uint32_t);
      fill.value = 0;
      allocator_->InvalidateCurrentPass();
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
  };

  void STDMETHODCALLTYPE
  ResolveQueryData(
      ID3D12QueryHeap *pHeap, D3D12_QUERY_TYPE Type, UINT StartIndex, UINT QueryCount, ID3D12Resource *pDstBuffer,
      UINT64 AlignedDstBufferOffset
  ) {
    auto heap = static_cast<MTLD3D12QueryHeap *>(pHeap);
    auto dst = static_cast<MTLD3D12Resource *>(pDstBuffer)->buffer->current()->buffer();
    SumOcclusion();
    if (!heap->timestamps.empty()) {
      allocator_->InvalidateCurrentPass();
      auto resolve = allocator_->AllocatePass<ResolveTimestampsData>();
      resolve->type = EncoderType::ResolveTimestamps;
      resolve->heap = heap;
      D3D12_HEAP_PROPERTIES placed{};
      pDstBuffer->GetHeapProperties(&placed, nullptr);
      if (placed.Type == D3D12_HEAP_TYPE_READBACK) {
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
    PreBlit();
    auto &cmd = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
    cmd.type = WMTBlitCommandCopyFromBufferToBuffer;
    cmd.src = heap->results;
    cmd.src_offset = StartIndex * heap->result_size;
    cmd.dst = dst;
    cmd.dst_offset = AlignedDstBufferOffset;
    cmd.copy_length = QueryCount * heap->result_size;
  };

  // the region's work follows the predicate as it is when the region starts
  void STDMETHODCALLTYPE SetPredication(ID3D12Resource *pBuffer, UINT64 AlignedBufferOffset, D3D12_PREDICATION_OP Op) {
    EndPredication();
    if (!pBuffer)
      return;
    StartComputePass();
    auto [mapped, offset] = allocator_->AllocateGPUHeap(4 * sizeof(uint32_t), 4);
    if (allocator_->records_) {
      allocator_->RecordGPUHeap(mapped, 4 * sizeof(uint32_t), "predicate header", 3 * sizeof(uint32_t), sizeof(uint32_t));
      predication_pass_ = allocator_->encoder_current;
    }
    predication_region_ = static_cast<uint32_t *>(mapped);
    predication_region_[0] = predication_region_[1] = predication_region_[3] = 0;
    predication_region_[2] = Op == D3D12_PREDICATION_OP_EQUAL_ZERO;
    auto &cmd_setpso = allocator_->EncodeComputeCommand<wmtcmd_compute_setpso>();
    cmd_setpso.type = WMTComputeCommandSetPSO;
    cmd_setpso.pso = device_->predicate;
    cmd_setpso.threadgroup_size = {1, 1, 1};
    const std::pair<obj_handle_t, uint64_t> buffers[] = {
        {static_cast<MTLD3D12Resource *>(pBuffer)->buffer->current()->buffer(), AlignedBufferOffset},
        {allocator_->gpu_heap_buffer_, offset},
        {allocator_->gpu_heap_buffer_, 0},
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
  };

  // annotations for capture tools, with no effect on rendering; D3D11 here treats them the same way
  void STDMETHODCALLTYPE SetMarker(UINT Metadata, const void *data, UINT size) {};

  // annotations for capture tools, with no effect on rendering; D3D11 here treats them the same way
  void STDMETHODCALLTYPE BeginEvent(UINT Metadata, const void *data, UINT size) {};

  void STDMETHODCALLTYPE EndEvent() {};

  void STDMETHODCALLTYPE ExecuteIndirect(
      ID3D12CommandSignature *pCommandSignature, UINT MaxCommandCount, ID3D12Resource *pArgBuffer,
      UINT64 ArgBufferOffset, ID3D12Resource *pCountBuffer, UINT64 CountBufferOffset
  ) {
    if (Record([=, sig = Com<ID3D12CommandSignature>(pCommandSignature), args = Com<ID3D12Resource>(pArgBuffer), count = Com<ID3D12Resource>(pCountBuffer)](auto *l) { l->ExecuteIndirect(sig.ptr(), MaxCommandCount, args.ptr(), ArgBufferOffset, count.ptr(), CountBufferOffset); }))
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
    if (rays || sig->CommandType == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH) {
      IndirectComputeCommandData *cmd;
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
        cmd = allocator_->EncodeIndirectComputeCommand(sig, pso_compute_.ptr(), MaxCommandCount);
      }
      cmd->max_count_buffer = CountBufferAddress;
      PredicateIndirect(cmd);
      cmd->argument_buffer = ArgBufferAddress;
      uint64_t root_arguments = 0;

      if (sig->UpdateRootArguments) {
        root_arguments = EncodeRootArgument(rootsig_compute_.ptr(), rootarg_compute_staging_, MaxCommandCount, true);
        cmd->rootsig_qwords = root_arguments;
        cmd->rootsig_qwords += allocator_->gpu_heap_buffer_address_;
        cmd->rootsig_qwords_stride = rootsig_compute_->UploadQwords;
        cmd->static_samplers = EncodeStaticSamplers(rootsig_compute_.ptr());
        cmd->static_samplers += allocator_->gpu_heap_buffer_address_;
        ResetIndirectState(sig, rootarg_compute_staging_);
        dirty_state_.set(DirtyState::ComputeRootArguments);
      }
      if (rays) {
        // each command is a dispatch of the kernel, of the threadgroups the resolver counts for it
        auto [_, at] = allocator_->AllocateGPUHeap(MaxCommandCount * sizeof(IndirectRays), alignof(IndirectRays));
        cmd->rays = allocator_->gpu_heap_buffer_address_ + at;
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
    if (pso_graphics_ && pso_graphics_->stream_output) {
      ERR("ExecuteIndirect: stream output is not implemented yet");
      return;
    }
    bool encode_binding = tessellation || sig->UpdateRootArguments || sig->UpdateIndexBuffer || sig->UpdateVertexBuffers;
    DrawCallStatus status = PreDraw(encode_binding);
    if (status == DrawCallStatus::Invalid)
      return;
    if (status != DrawCallStatus::Ordinary) {
      IMPLEMENT_ME // TODO: (potential) emulated pipeline
    }

    auto cmd = allocator_->EncodeIndirectRenderCommand(sig, pso_graphics_.ptr(), Pipeline(), MaxCommandCount, tessellation);
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
    if (geometry)
      std::tie(cmd->geometry_threads, cmd->geometry_increment) = get_gs_vertex_count(topology_, pso_graphics_->vertex_registers);
    if (!encode_binding)
      return;
    cmd->rootsig_qwords = EncodeRootArgument(
        rootsig_graphics_.ptr(), rootarg_graphics_staging_, MaxCommandCount, sig->UpdateRootArguments
    );
    cmd->rootsig_qwords += allocator_->gpu_heap_buffer_address_;
    cmd->rootsig_qwords_stride = rootsig_graphics_->UploadQwords;
    cmd->static_samplers = EncodeStaticSamplers(rootsig_graphics_.ptr());
    cmd->static_samplers += allocator_->gpu_heap_buffer_address_;
    auto [VBOffset, VBStride] = PopulateVertexBufferTable(MaxCommandCount, sig->UpdateVertexBuffers);
    cmd->vertex_buffer = allocator_->gpu_heap_buffer_address_ + VBOffset;
    cmd->vertex_argbuf_stride = VBStride;
    ResetIndirectState(sig, rootarg_graphics_staging_);
    for (unsigned slot = 0; slot < vertex_buffers_.size(); slot++)
      if (sig->ResetVertexBuffers & (1u << slot))
        vertex_buffers_[slot] = {};
    if (sig->UpdateIndexBuffer)
      IASetIndexBuffer(nullptr);
    dirty_state_.set(DirtyState::GraphicsRootArguments, DirtyState::VertexBuffer);
    if (!cmd->draws)
      return;
    // a GPU without mesh commands in indirect command buffers: every command is a draw of its own, which binds what
    // the command's stages read and has the threadgroups the resolver left for it (none past the count)
    auto heap = allocator_->gpu_heap_buffer_address_;
    WMTSize object{cmd->geometry_threads, 1, 1}, mesh{1, 1, 1};
    if (dispatch_mesh) {
      object = {cmd->object_threads[0], cmd->object_threads[1], cmd->object_threads[2]};
      mesh = {cmd->mesh_threads[0], cmd->mesh_threads[1], cmd->mesh_threads[2]};
    } else if (cmd->threads_per_patch) {
      object = {cmd->threads_per_patch, 32 / cmd->threads_per_patch, 1};
      mesh = {32, 1, 1};
    }
    for (UINT i = 0; i < MaxCommandCount; i++) {
      auto at = cmd->draws - heap + i * sizeof(IndirectMeshDraw);
      auto roots = cmd->rootsig_qwords - heap + i * cmd->rootsig_qwords_stride * sizeof(uint64_t);
      for (auto stage : {WMTRenderCommandSetObjectBuffer, WMTRenderCommandSetMeshBuffer, WMTRenderCommandSetFragmentBuffer}) {
        EncodeBuffer(stage, roots, SM50_BINDING_INDEX_ROOT_ARGUMENTS);
        EncodeBuffer(stage, cmd->static_samplers - heap, SM50_BINDING_INDEX_STATIC_SAMPLERS);
      }
      if (!dispatch_mesh) {
        EncodeBuffer(
            WMTRenderCommandSetObjectBuffer, cmd->vertex_buffer - heap + i * cmd->vertex_argbuf_stride,
            SM50_BINDING_INDEX_VERTEX_BUFFER
        );
        EncodeBuffer(WMTRenderCommandSetObjectBuffer, at + offsetof(IndirectMeshDraw, view), SM50_BINDING_INDEX_INDEX_BUFFER);
        if (cmd->threads_per_patch)
          EncodeBuffer(
              WMTRenderCommandSetObjectBuffer, at + offsetof(IndirectMeshDraw, control_points), SM50_BINDING_INDEX_PATCH_SIZE
          );
        EncodeBuffer(
            WMTRenderCommandSetObjectBuffer, at + offsetof(IndirectMeshDraw, arguments), SM50_BINDING_INDEX_DRAW_ARGUMENTS
        );
      }
      auto &draw = allocator_->EncodeRenderCommand<wmtcmd_render_draw_meshthreadgroups_indirect>();
      draw.type = WMTRenderCommandDrawMeshThreadgroupsIndirect;
      draw.indirect_args_buffer = allocator_->gpu_heap_buffer_;
      draw.indirect_args_offset = at + offsetof(IndirectMeshDraw, threadgroups);
      draw.object_threadgroup_size = object;
      draw.mesh_threadgroup_size = mesh;
    }
  };

  // the root argument half of D3D12's reset after ExecuteIndirect
  void ResetIndirectState(MTLD3D12CommandSignature *sig, uint64_t *rootargs) {
    for (auto [offset, count] : sig->ResetRootDwords)
      std::fill_n(reinterpret_cast<uint32_t *>(rootargs) + offset, count, 0u);
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
    if (Record([=](auto *l) { l->OMSetDepthBounds(Min, Max); }))
      return;
    depth_bounds_[0] = std::isnan(Min) ? 0.0f : Min;
    depth_bounds_[1] = std::isnan(Max) ? 0.0f : Max;
    dirty_state_.set(DirtyState::DepthBounds);
  }

  void STDMETHODCALLTYPE
  SetSamplePositions(UINT NumSamplesPerPixel, UINT NumPixels, D3D12_SAMPLE_POSITION *pSamplePositions) {
    IMPLEMENT_ME
  }

  // ResolveSubresourceRegion's sampler feedback modes, between a map and an R8_UINT texture or, for a MinMip map
  // that is no array, a buffer of rows. offsets and `pRect`, the rectangle's size, count mip regions of the transcoded
  // mip; UINT_MAX is every subresource (Sampler Feedback spec, "Transcoding")
  void
  TranscodeFeedback(
      bool Encode, MTLD3D12Resource *pMap, UINT MapSubresource, POINT MapOffset, MTLD3D12Resource *pOther,
      UINT OtherSubresource, POINT Offset, const D3D12_RECT *pRect
  ) {
    if (!pMap->feedback)
      return;
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
    // nothing is compressed in Metal, so a resource decompressed in place is as it was
    if (!dst->texture || !src->texture || (dst == src && ResolveMode == D3D12_RESOLVE_MODE_DECOMPRESS))
      return;
    MTL_DXGI_FORMAT_DESC format;
    if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), Format, format))) {
      ERR("ResolveSubresourceRegion: invalid format ", Format);
      return;
    }
    UINT level[2], slice[2], plane[2];
    DecomposeSubresource(dst->GetDesc(), DstSubresource, &level[0], &slice[0], &plane[0]);
    DecomposeSubresource(src->GetDesc(), SrcSubresource, &level[1], &slice[1], &plane[1]);
    // a color subresource is seen in the resolve's format, a depth or stencil plane in the only one it has, which for
    // reading stencil beside depth is the stencil view of both
    auto view = [&](MTLD3D12Resource *resource, bool source) {
      auto &texture = resource->texture;
      auto planes = DepthStencilPlanarFlags(texture->pixelFormat());
      TextureViewDescriptor desc;
      desc.format = !planes                             ? format.PixelFormat
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
    // Metal's own resolve is of a whole attachment into one as large, by averaging colors
    if (ResolveMode == D3D12_RESOLVE_MODE_AVERAGE && !planes && !IsIntegerFormat(format.PixelFormat) && !DstX && !DstY &&
        !memcmp(&rect, &whole, sizeof(rect)) && dst->texture->width(dst_view) == (UINT)whole.right &&
        dst->texture->height(dst_view) == (UINT)whole.bottom) {
      allocator_->InvalidateCurrentPass();
      auto resolve = allocator_->AllocatePass<ResolveEncoderData>();
      resolve->type = EncoderType::Resolve;
      resolve->src = src->texture->view(src_view);
      resolve->dst = dst->texture->view(dst_view);
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
    IMPLEMENT_ME
  }

  void STDMETHODCALLTYPE
  WriteBufferImmediate(
      UINT Count, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *pParams, const D3D12_WRITEBUFFERIMMEDIATE_MODE *pModes
  ) {
    // each value is written in order with the list's other work, whatever the marker mode
    for (auto &param : std::span(pParams, Count))
      WriteImmediate(param.Dest, &param.Value, sizeof(param.Value));
  }

  // the buffer and offset a GPU address is in
  std::pair<obj_handle_t, uint64_t>
  At(D3D12_GPU_VIRTUAL_ADDRESS VA) {
    uint64_t offset = 0;
    auto allocation = device_->LookupBufferByVA(VA, &offset);
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
    auto allocation = device_->LookupBufferByVA(VA, &offset);
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
    pass_resolves_.clear();
    auto resolve = [&](const D3D12_RENDER_PASS_ENDING_ACCESS &end) {
      if (end.Type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE)
        for (auto &sub : std::span(end.Resolve.pSubresourceParameters, end.Resolve.SubresourceCount))
          pass_resolves_.push_back({end.Resolve, sub});
    };
    for (UINT i = 0; i < NumRenderTargets; i++) {
      targets[i] = pRenderTargets[i].cpuDescriptor;
      resolve(pRenderTargets[i].EndingAccess);
    }
    OMSetRenderTargets(NumRenderTargets, targets, FALSE, pDepthStencil ? &pDepthStencil->cpuDescriptor : nullptr);
    for (UINT i = 0; i < NumRenderTargets; i++)
      if (pRenderTargets[i].BeginningAccess.Type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR)
        ClearRenderTargetView(targets[i], pRenderTargets[i].BeginningAccess.Clear.ClearValue.Color, 0, nullptr);
    if (!pDepthStencil)
      return;
    auto &depth = pDepthStencil->DepthBeginningAccess, &stencil = pDepthStencil->StencilBeginningAccess;
    auto flags = (depth.Type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR ? D3D12_CLEAR_FLAG_DEPTH : 0) |
                 (stencil.Type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR ? D3D12_CLEAR_FLAG_STENCIL : 0);
    if (flags)
      ClearDepthStencilView(
          pDepthStencil->cpuDescriptor, (D3D12_CLEAR_FLAGS)flags, depth.Clear.ClearValue.DepthStencil.Depth,
          stencil.Clear.ClearValue.DepthStencil.Stencil, 0, nullptr
      );
    resolve(pDepthStencil->DepthEndingAccess);
    resolve(pDepthStencil->StencilEndingAccess);
  }

  void STDMETHODCALLTYPE
  EndRenderPass() {
    for (auto &[r, sub] : pass_resolves_)
      ResolveSubresourceRegion(
          r.pDstResource, sub.DstSubresource, sub.DstX, sub.DstY, r.pSrcResource, sub.SrcSubresource,
          const_cast<D3D12_RECT *>(&sub.SrcRect), r.Format, r.ResolveMode
      );
    pass_resolves_.clear();
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

  void
  PreAccelerationStructure() {
    if (allocator_->encoder_current && allocator_->encoder_current->type == EncoderType::AccelerationStructure)
      return;
    allocator_->InvalidateCurrentPass();
    auto pass = allocator_->AllocatePass<AccelerationStructureEncoderData>();
    pass->type = EncoderType::AccelerationStructure;
    pass->cmd_head.type = WMTAccelerationStructureCommandNop;
    pass->cmd_head.next.set(0);
    pass->cmd_tail = (wmtcmd_base *)&pass->cmd_head;
    pass->filled = nullptr;
    pass->filled_tail = &pass->filled;
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
    // a top-level structure's instance count is a uint32 the build reads
    obj_handle_t count = 0;
    uint64_t count_offset = 0;
    if (top) {
      auto [mapped, offset] = allocator_->AllocateGPUHeap(sizeof(uint32_t), sizeof(uint32_t));
      *(uint32_t *)mapped = inputs.NumDescs;
      count = allocator_->gpu_heap_buffer_;
      count_offset = offset;
    }
    auto geometries = allocator_->AllocateCommandData<WMTAccelerationStructureGeometry>(top ? 0 : inputs.NumDescs);
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

    if (structure->instances) {
      StartComputePass();
      auto &pso = allocator_->EncodeComputeCommand<wmtcmd_compute_setpso>();
      pso.type = WMTComputeCommandSetPSO;
      pso.pso = device_->acceleration_structure_instances;
      pso.threadgroup_size = {device_->GetSIMDWidth(), 1, 1};
      // DXMTInstanceConversion
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
    PreAccelerationStructure();
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
    PreAccelerationStructure();
    Fills(structure.get(), nullptr, source.get());
    auto &copy = allocator_->EncodeAccelerationStructureCommand<wmtcmd_accelerationstructure_copy>();
    copy.type = compact ? WMTAccelerationStructureCommandCopyAndCompact : WMTAccelerationStructureCommandCopy;
    copy.src = source->structure;
    copy.dst = structure->structure;
  }

  void STDMETHODCALLTYPE
  SetPipelineState1(ID3D12StateObject *pStateObject) {
    state_object_ = static_cast<MTLD3D12StateObject *>(pStateObject);
  }

  // the state object's kernel runs a thread per ray generation shader invocation, with the compute root signature's
  // arguments as the global ones (dispatch_rays in dxmt_command.metal)
  void STDMETHODCALLTYPE
  DispatchRays(const D3D12_DISPATCH_RAYS_DESC *pDesc) {
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
    if (device_->NamesPasses())
      device_->NamePass(allocator_->encoder_current->id, state_object_->name);
    auto &pso = allocator_->EncodeComputeCommand<wmtcmd_compute_setpso>();
    pso.type = WMTComputeCommandSetPSO;
    pso.pso = pipeline;
    pso.threadgroup_size = {device_->GetSIMDWidth(), 1, 1};
    // the next compute dispatch binds its own pipeline again
    dirty_state_.set(DirtyState::ComputePipelineState);
    BindComputeRootArguments();
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
  RSSetShadingRate(D3D12_SHADING_RATE, const D3D12_SHADING_RATE_COMBINER *) {
    ERR("RSSetShadingRate: variable rate shading is not supported yet");
  }

  void STDMETHODCALLTYPE
  RSSetShadingRateImage(ID3D12Resource *) {
    ERR("RSSetShadingRateImage: variable rate shading is not supported yet");
  }

  void STDMETHODCALLTYPE
  DispatchMesh(UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) {
    if (Record([=](auto *l) { l->DispatchMesh(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ); }))
      return;
    if (!pso_graphics_ || !pso_graphics_->mesh_shader || !ThreadGroupCountX || !ThreadGroupCountY || !ThreadGroupCountZ)
      return;
    if (PreDraw() == DrawCallStatus::Invalid)
      return;
    // the groups are the amplification shader's, or without one the mesh shader's
    auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_draw_meshthreadgroups>();
    cmd.type = WMTRenderCommandDrawMeshThreadgroups;
    cmd.threadgroup_per_grid = {ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ};
    auto size = [](const uint32_t threads[3]) { return WMTSize{threads[0], threads[1], threads[2]}; };
    cmd.object_threadgroup_size = size(pso_graphics_->object_threads);
    cmd.mesh_threadgroup_size = size(pso_graphics_->mesh_threads);
  }

  // enhanced barriers order work as ResourceBarrier does: a draw's writes end the pass they were drawn in
  void STDMETHODCALLTYPE
  Barrier(UINT32 NumBarrierGroups, const D3D12_BARRIER_GROUP *pBarrierGroups) {
    if (!allocator_->encoder_current || allocator_->encoder_current->type != EncoderType::Render)
      return;
    constexpr auto draw_writes = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS | D3D12_BARRIER_ACCESS_RENDER_TARGET |
                                 D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE | D3D12_BARRIER_ACCESS_STREAM_OUTPUT;
    // common access is any access, writes included
    auto writes = [&](D3D12_BARRIER_ACCESS access) {
      return access == D3D12_BARRIER_ACCESS_COMMON || (access != D3D12_BARRIER_ACCESS_NO_ACCESS && (access & draw_writes));
    };
    for (auto &group : std::span(pBarrierGroups, NumBarrierGroups))
      for (UINT32 i = 0; i < group.NumBarriers; i++) {
        auto access = group.Type == D3D12_BARRIER_TYPE_GLOBAL    ? group.pGlobalBarriers[i].AccessBefore
                      : group.Type == D3D12_BARRIER_TYPE_TEXTURE ? group.pTextureBarriers[i].AccessBefore
                                                                 : group.pBufferBarriers[i].AccessBefore;
        if (writes(access)) {
          allocator_->InvalidateCurrentPass();
          return;
        }
      }
  }
};

HRESULT STDMETHODCALLTYPE
MTLD3D12CommandAllocatorImpl::CreateCommandList(
    UINT NodeMask, D3D12_COMMAND_LIST_TYPE Type, ID3D12PipelineState *pInitialPipelineState, REFIID riid,
    void **ppCommandList
) {
  if (Type != type_)
    return E_INVALIDARG;

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

}; // namespace dxmt