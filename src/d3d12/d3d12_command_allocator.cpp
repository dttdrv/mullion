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

#include "d3d12_command_allocator.hpp"
#include "com/com_pointer.hpp"
#include <map>

namespace dxmt {

struct GPURecord {
  const char *bytes, *kind;
  size_t length, written_offset, written_length;
};
// a closed list's directory stays read-only while later lists are recorded
struct GPUListRecords {
  EncoderData *root;
  GPUListRecords *previous;
  std::unordered_map<EncoderData *, std::vector<GPURecord>> passes;
};
struct GPURecording {
  std::atomic<GPUListRecords *> head = nullptr;
  ~GPURecording() { Clear(); }
  void Clear() {
    for (auto list = head.exchange(nullptr); list;) {
      auto previous = list->previous;
      delete list;
      list = previous;
    }
  }
};
struct GPURecordChecks {
  struct Snapshot {
    GPURecord record;
    std::string label, bytes;
    uint64_t pass;
  };
  std::vector<Snapshot> snapshots;
  bool checked = false;
};
struct GPURecordTotals {
  dxmt::mutex mutex;
  std::map<std::string, std::pair<uint64_t, uint64_t>> kinds;
};

std::shared_ptr<GPURecordTotals>
CreateGPURecordTotals() { return std::make_shared<GPURecordTotals>(); }

void
SnapshotGPURecords(
    const GPUListRecords *List, EncoderData *Pass, const std::string &Label, std::shared_ptr<GPURecordChecks> &Checks
) {
  if (!List)
    return;
  auto found = List->passes.find(Pass);
  if (found == List->passes.end())
    return;
  if (!Checks)
    Checks = std::make_shared<GPURecordChecks>();
  for (auto &record : found->second) {
    auto &snapshot = Checks->snapshots.emplace_back(record, Label, std::string(record.length, '\0'),
                                                   Pass->type == EncoderType::Null ? 0 : Pass->id);
    // predication may already be writing the excluded span
    memcpy(snapshot.bytes.data(), record.bytes, record.written_offset);
    auto end = record.written_offset + record.written_length;
    memcpy(snapshot.bytes.data() + end, record.bytes + end, record.length - end);
  }
}

void
CompareGPURecords(const std::shared_ptr<GPURecordChecks> &Checks, const std::shared_ptr<GPURecordTotals> &Totals) {
  if (!Checks)
    return;
  std::lock_guard<dxmt::mutex> lock(Totals->mutex);
  if (std::exchange(Checks->checked, true))
    return;
  size_t changed = 0, first = 0, word = 0;
  uint32_t encoded = 0, current = 0;
  for (size_t i = 0; i < Checks->snapshots.size(); i++) {
    auto &snapshot = Checks->snapshots[i];
    auto &record = snapshot.record;
    auto at = std::mismatch(record.bytes, record.bytes + record.written_offset, snapshot.bytes.data()).first;
    if (at == record.bytes + record.written_offset) {
      auto end = record.written_offset + record.written_length;
      at = std::mismatch(record.bytes + end, record.bytes + record.length, snapshot.bytes.data() + end).first;
    }
    auto &totals = Totals->kinds[record.kind];
    totals.first++;
    if (at == record.bytes + record.length)
      continue;
    totals.second++;
    if (changed++ == 0) {
      first = i;
      word = (at - record.bytes) / sizeof(uint32_t);
      auto offset = word * sizeof(uint32_t), length = std::min(sizeof(uint32_t), record.length - offset);
      memcpy(&encoded, snapshot.bytes.data() + offset, length);
      memcpy(&current, record.bytes + offset, length);
    }
  }
  if (changed)
    ERR("D3D12 recording changed: ", Checks->snapshots[first].record.kind, "; pass ", Checks->snapshots[first].pass,
        " ", Checks->snapshots[first].label, "; word ", word, " encoded ", encoded, " current ", current, "; ", changed,
        " of ", Checks->snapshots.size(), " records differ");
}

void
ReportGPURecords(const std::shared_ptr<GPURecordTotals> &Totals) {
  uint64_t compared = 0, differed = 0;
  for (auto &[kind, totals] : Totals->kinds) {
    Logger::info(str::format(
        "D3D12 recording kind: ", kind, "; ", totals.first, " compared; ", totals.second, " differed"
    ));
    compared += totals.first, differed += totals.second;
  }
  Logger::info(str::format("D3D12 recording totals: ", compared, " compared; ", differed, " differed"));
}

void
MTLD3D12CommandAllocatorImpl::RecordGPUHeap(
    const void *Bytes, size_t Length, const char *Kind, size_t WrittenOffset, size_t WrittenLength, EncoderData *Pass
) {
  if (!Length || exhausted_)
    return;
  auto pass = Pass ? Pass : encoder_current ? encoder_current : encoder_last;
  auto root = &encoder_lists_.back();
  auto list = records_->head.load(std::memory_order_relaxed);
  if (!list || list->root != root) {
    list = new GPUListRecords{root, list, {}};
    records_->head.store(list, std::memory_order_release);
  }
  list->passes[pass].push_back({static_cast<const char *>(Bytes), Kind, Length, WrittenOffset, WrittenLength});
}

void
MTLD3D12CommandAllocatorImpl::ExcludeGPUWrite(const void *Bytes, size_t Offset, size_t Length) {
  auto list = records_->head.load(std::memory_order_relaxed);
  if (!list)
    return;
  auto found = list->passes.find(encoder_current);
  if (found == list->passes.end())
    return;
  auto &records = found->second;
  for (auto it = records.rbegin(); it != records.rend(); ++it)
    if (it->bytes == Bytes) {
      it->written_offset = Offset, it->written_length = Length;
      return;
    }
}

const GPUListRecords *
MTLD3D12CommandAllocatorImpl::GPURecordsFor(EncoderData *Root) {
  auto list = records_->head.load(std::memory_order_acquire);
  while (list && list->root != Root)
    list = list->previous;
  return list;
}

MTLD3D12CommandAllocatorImpl::~MTLD3D12CommandAllocatorImpl() {
  ReleaseRuns();
  delete records_;
}

void
MTLD3D12CommandAllocatorImpl::ReleaseRuns() {
  if (records_)
    records_->Clear();
  for (auto [arena, cursor] : {std::pair{&device_->recording.commands, &cpu_cursor_},
                              std::pair{&device_->recording.arguments, &gpu_cursor_}}) {
    for (auto &run : cursor->runs)
      arena->Release(run);
    *cursor = {};
  }
}

MTLD3D12CommandAllocatorImpl::MTLD3D12CommandAllocatorImpl(MTLD3D12Device *pDevice, D3D12_COMMAND_LIST_TYPE Type) :
    MTLD3D12Pageable<MTLD3D12CommandAllocator>(pDevice),
    type_(Type),
    records_(device_->NamesPasses() ? new GPURecording : nullptr),
    clear_uav_(device_->GetMTLDevice(), *this),
    transcode_feedback_(*this),
    clear_rtv_(device_->GetMTLDevice(), *this),
    copy_temp_allocator_(
        {device_->GetMTLDevice(), WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModePrivate}
    ),
    copy_temp_version_(0) {}

HRESULT
CreateCommandAllocator(MTLD3D12Device *pDevice, D3D12_COMMAND_LIST_TYPE Type, REFIID riid, void **ppCommandAllocator) {
  switch (Type) {
  case D3D12_COMMAND_LIST_TYPE_DIRECT:
  case D3D12_COMMAND_LIST_TYPE_BUNDLE:
  case D3D12_COMMAND_LIST_TYPE_COMPUTE:
  case D3D12_COMMAND_LIST_TYPE_COPY:
    break;
  default:
    return E_INVALIDARG;
  }
  auto command_allocator = Com(new MTLD3D12CommandAllocatorImpl(pDevice, Type));
  HRESULT hr = command_allocator->Initialize();
  if (FAILED(hr))
    return hr;
  return command_allocator->QueryInterface(riid, ppCommandAllocator);
}

HRESULT
MTLD3D12CommandAllocatorImpl::Initialize() {

  // the device's arenas, and the one Metal buffer over what the GPU reads: there from the first allocator on
  auto &recording = device_->recording;
  {
    std::lock_guard<dxmt::mutex> lock(recording.mutex);
    if (!recording.commands.Initialize() || !recording.arguments.Initialize())
      return E_OUTOFMEMORY;
    if (!recording.buffer) {
      WMTBufferInfo buffer_info;
      buffer_info.memory.set(recording.arguments.base);
      buffer_info.length = RecordingArena::kSize;
      buffer_info.options = WMTResourceHazardTrackingModeUntracked;
      recording.buffer = device_->GetMTLDevice().newBuffer(buffer_info);
      if (!recording.buffer) {
        ERR("CommandAllocator: failed to allocate gpu buffer");
        return E_FAIL;
      }
      recording.address = buffer_info.gpu_address;
    }
    if (!recording.zeros && !(recording.zeros = recording.arguments.Acquire(1)))
      return E_OUTOFMEMORY;
  }
  cpu_heap_ = recording.commands.base, gpu_heap_ = recording.arguments.base;
  gpu_heap_buffer_ = recording.buffer, gpu_heap_buffer_address_ = recording.address;
  ReleaseRuns();
  exhausted_ = false;
  spill_.clear();

  encoder_current = nullptr;
  encoder_last = nullptr;
  encoder_count_ = 0;

  icb_.clear();
  visibility_.clear();
  visibility_used_ = kVisibilityWindow;
  acceleration_structures_.clear();
  acceleration_structure_inputs_.clear();

  copy_temp_allocator_.free_blocks(copy_temp_version_++);

  return S_OK;
}

HRESULT
STDMETHODCALLTYPE
MTLD3D12CommandAllocatorImpl::QueryInterface(REFIID riid, void **ppvObject) {
  if (ppvObject == nullptr)
    return E_POINTER;

  *ppvObject = nullptr;

  if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
      riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12CommandAllocator)) {
    *ppvObject = ref(this);
    return S_OK;
  }

  if (logQueryInterfaceError(__uuidof(ID3D12CommandQueue), riid)) {
    WARN("D3D12CommandAllocator: Unknown interface query ", str::format(riid));
  }

  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE
MTLD3D12CommandAllocatorImpl::Reset() {
  if (encoder_last)
    return E_FAIL;
  // "the runtime will not allow a reset while a command list is still being executed": the lists' recording is what
  // the GPU still reads
  if (pending)
    ERR("CommandAllocator: Reset while ", pending.load(), " of its submitted lists have not completed");

  for (auto &encoder_list : encoder_lists_) {
    EncoderData *next = encoder_list.next;
    while (next) {
      switch (next->type) {
      case EncoderType::Null:
        break;
      case EncoderType::Clear:
        reinterpret_cast<ClearEncoderData *>(next)->~ClearEncoderData();
        break;
      case EncoderType::Render:
        reinterpret_cast<RenderEncoderData *>(next)->~RenderEncoderData();
        break;
      case EncoderType::Blit:
        reinterpret_cast<BlitEncoderData *>(next)->~BlitEncoderData();
        break;
      case EncoderType::Compute:
        reinterpret_cast<ComputeEncoderData *>(next)->~ComputeEncoderData();
        break;
      case EncoderType::AccelerationStructure:
        reinterpret_cast<AccelerationStructureEncoderData *>(next)->~AccelerationStructureEncoderData();
        break;
      case EncoderType::Resolve:
        reinterpret_cast<ResolveEncoderData *>(next)->~ResolveEncoderData();
        break;
      case EncoderType::ResolveTimestamps:
        reinterpret_cast<ResolveTimestampsData *>(next)->~ResolveTimestampsData();
        break;
      }
      next = next->next;
    }
  }
  encoder_lists_.clear();

  return Initialize();
};

// runs the signature's resolver over the commands; the caller fills in what it reads
IndirectComputeCommandData *
MTLD3D12CommandAllocatorImpl::EncodeComputeResolver(MTLD3D12CommandSignature *pCmdSig, size_t MaxCount) {
  auto [Ptr, Offset] = AllocateGPUHeap(sizeof(IndirectComputeCommandData), 16);
  if (records_)
    RecordGPUHeap(Ptr, sizeof(IndirectComputeCommandData), "indirect dispatch");
  auto data = new (Ptr) IndirectComputeCommandData{};
  data->max_count = MaxCount;

  auto &cmd_setpso_res = EncodeComputeCommand<wmtcmd_compute_setpso>();
  cmd_setpso_res.type = WMTComputeCommandSetPSO;
  cmd_setpso_res.pso = pCmdSig->compute_resolver;
  cmd_setpso_res.threadgroup_size = {1, 1, 1};

  auto &cmd_argbuf_res = EncodeComputeCommand<wmtcmd_compute_setbuffer>();
  cmd_argbuf_res.type = WMTComputeCommandSetBuffer;
  cmd_argbuf_res.buffer = gpu_heap_buffer_;
  cmd_argbuf_res.offset = Offset;
  cmd_argbuf_res.index = 30;

  auto &cmd_dispatch_res = EncodeComputeCommand<wmtcmd_compute_dispatch>();
  cmd_dispatch_res.type = WMTComputeCommandDispatch;
  cmd_dispatch_res.size = {1, 1, 1};
  return data;
}

IndirectComputeCommandData *
MTLD3D12CommandAllocatorImpl::EncodeIndirectComputeCommand(MTLD3D12CommandSignature *pCmdSig, MTLD3D12ComputePipelineState *pPSO, size_t MaxCount) {
  WMTIndirectCommandBufferInfo info;
  info.inherit_buffers = !pCmdSig->UpdateRootArguments;
  info.inherit_pso = 1;
  info.inherit_cull_mode = 0;
  info.inherit_fill_mode = 0;
  info.inherit_front_facing = 0;
  info.inherit_depth_bias = 0;
  info.inherit_depth_clip_mode = 0;
  info.inherit_depth_stencil_state = 0;
  info.support_color_attachment_mapping = 0;
  info.support_dynamic_attribute_stride = 0;
  info.support_ray_tracing = 0;
  info.type = WMTIndirectCommandTypeConcurrentDispatch;
  info.max_vertex_buffer_binding = 0;
  info.max_fragment_buffer_binding = 0;
  info.max_object_buffer_binding = 0;
  info.max_mesh_buffer_binding = 0;
  info.max_kernel_buffer_binding = 31;
  info.max_kernel_threadgroup_memory_binding = 0;
  info.max_object_threadgroup_memory_binding = 0;
  info.gpu_resource_id = 0;

  auto icb = device_->GetMTLDevice().newIndirectCommandBuffer(info, MaxCount, WMTResourceStorageModeShared);

  auto &cmd_use_icb = EncodeComputeCommand<wmtcmd_compute_useresource>();
  cmd_use_icb.type = WMTComputeCommandUseResource;
  cmd_use_icb.usage = WMTResourceUsageRead | WMTResourceUsageWrite;
  cmd_use_icb.resource = icb;

  auto data = EncodeComputeResolver(pCmdSig, MaxCount);
  data->cmd_buf = info.gpu_resource_id;
  data->tgsize_x = pPSO->threadgroup_size.width;
  data->tgsize_y = pPSO->threadgroup_size.height;
  data->tgsize_z = pPSO->threadgroup_size.depth;

  auto &cmd_setpso = EncodeComputeCommand<wmtcmd_compute_setpso>();
  cmd_setpso.type = WMTComputeCommandSetPSO;
  cmd_setpso.pso = pPSO->pso;
  cmd_setpso.threadgroup_size = pPSO->threadgroup_size; // not really used

  auto &cmd = EncodeComputeCommand<wmtcmd_compute_executecommands>();
  cmd.type = WMTComputeCommandExecuteCommandsInBuffer;
  cmd.indirect_command_buffer = icb;
  cmd.location = 0;
  cmd.length = MaxCount;

  icb_.push_back(std::move(icb));

  return data;
}

IndirectRenderCommandData *
MTLD3D12CommandAllocatorImpl::EncodeIndirectRenderCommand(
    MTLD3D12CommandSignature *pCmdSig, MTLD3D12GraphicsPipelineState *pPSO, WMT::RenderPipelineState Pipeline,
    size_t MaxCount, bool tessellation
) {
  // for a tessellated, geometry or mesh command (`tessellation`) the resolver leaves an IndirectMeshDraw, and the
  // caller draws each: mesh commands in indirect command buffers come only with Apple9 GPUs, and one way serves all
  bool draws = tessellation;
  WMTIndirectCommandBufferInfo info;
  info.inherit_buffers = !(pCmdSig->UpdateVertexBuffers || pCmdSig->UpdateIndexBuffer || pCmdSig->UpdateRootArguments);
  info.inherit_pso = 1;
  info.inherit_cull_mode = 1;
  info.inherit_fill_mode = 1;
  info.inherit_front_facing = 1;
  info.inherit_depth_bias = 1;
  info.inherit_depth_clip_mode = 1;
  info.inherit_depth_stencil_state = 1;
  info.support_color_attachment_mapping = 0;
  info.support_dynamic_attribute_stride = 0;
  info.support_ray_tracing = 0;
  info.type = pCmdSig->CommandType == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED ? WMTIndirectCommandTypeDrawIndexed
                                                                                 : WMTIndirectCommandTypeDraw;
  info.max_vertex_buffer_binding = 31;
  info.max_fragment_buffer_binding = 31;
  info.max_object_buffer_binding = 0;
  info.max_mesh_buffer_binding = 0;
  info.max_kernel_buffer_binding = 0;
  info.max_kernel_threadgroup_memory_binding = 0;
  info.max_object_threadgroup_memory_binding = 0;
  info.gpu_resource_id = 0;

  // an indexed command of an ordinary pipeline is two: its indices in the view, and zeros for those past it
  size_t each = !tessellation && pCmdSig->CommandType == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED ? 2 : 1;
  WMT::Reference<WMT::IndirectCommandBuffer> icb;
  if (!draws)
    icb = device_->GetMTLDevice().newIndirectCommandBuffer(info, MaxCount * each, WMTResourceStorageModePrivate);

  auto [Ptr, Offset] = AllocateGPUHeap(sizeof(IndirectRenderCommandData), 16);
  if (records_)
    RecordGPUHeap(Ptr, sizeof(IndirectRenderCommandData), "indirect draw");

  auto data = reinterpret_cast<IndirectRenderCommandData *>(Ptr);

  data->cmd_buf = draws ? 0 : info.gpu_resource_id;
  data->max_count = MaxCount;

  {
    // populated outside
    data->max_count_buffer = 0;
    data->argument_buffer = 0;
    data->rootsig_qwords = 0;
    data->rootsig_qwords_stride = 0;
    data->static_samplers = 0;
    data->vertex_buffer = 0;
    data->vertex_argbuf_stride = 0;
    data->primitive_type = 0;
    data->index_buffer = 0;
    data->index_buffer_format = {};
    data->index_buffer_view = 0;
    data->control_points = 0;
    data->threads_per_patch = pPSO->threads_per_patch;
    data->tessellation_parts = pPSO->tessellation_parts;
    data->vertex_slots = pPSO->slot_mask;
    data->geometry_threads = 0;
    data->geometry_increment = 0;
    for (unsigned i = 0; i < 3; i++) {
      data->object_threads[i] = pPSO->mesh_shader ? pPSO->object_threads[i] : 0;
      data->mesh_threads[i] = pPSO->mesh_shader ? pPSO->mesh_threads[i] : 0;
    }
  }

  auto &zeros = *device_->recording.zeros;
  data->zeros = each > 1 ? gpu_heap_buffer_address_ + zeros.offset : 0;
  data->zero_count = zeros.length / sizeof(uint16_t);
  data->draws = 0;
  if (draws) {
    auto [records, at] = AllocateGPUHeap(MaxCount * sizeof(IndirectMeshDraw), alignof(IndirectMeshDraw));
    memset(records, 0, MaxCount * sizeof(IndirectMeshDraw));
    data->draws = gpu_heap_buffer_address_ + at;
  }

  data->most = 0;
  if (device_->NamesPasses()) {
    auto [words, offset] = AllocateGPUHeap(kMostWords * sizeof(uint32_t), sizeof(uint32_t));
    memset(words, 0, kMostWords * sizeof(uint32_t));
    data->most = gpu_heap_buffer_address_ + offset;
    auto render = static_cast<RenderEncoderData *>(encoder_current);
    render->most = new (AllocateCPUHeap(sizeof(RenderEncoderData::Most), alignof(RenderEncoderData::Most)))
        RenderEncoderData::Most{static_cast<const uint32_t *>(words), data, render->most};
  }

  // the signature's resolver writes the commands in a compute pass before the render pass, which then runs them: a
  // pass that ran what one of its own draws was still writing ran half-written commands now and then
  auto &cmd_setpso_res = EncodeBeforeRender<wmtcmd_compute_setpso>();
  cmd_setpso_res.type = WMTComputeCommandSetPSO;
  cmd_setpso_res.pso = pCmdSig->render_resolver;
  cmd_setpso_res.threadgroup_size = {1, 1, 1};

  auto &cmd_argbuf_res = EncodeBeforeRender<wmtcmd_compute_setbuffer>();
  cmd_argbuf_res.type = WMTComputeCommandSetBuffer;
  cmd_argbuf_res.buffer = gpu_heap_buffer_;
  cmd_argbuf_res.offset = Offset;
  cmd_argbuf_res.index = 30;

  if (!draws) {
    auto &cmd_write_icb = EncodeBeforeRender<wmtcmd_compute_useresource>();
    cmd_write_icb.type = WMTComputeCommandUseResource;
    cmd_write_icb.usage = WMTResourceUsageWrite;
    cmd_write_icb.resource = icb;
  }

  auto &cmd_dispatch_res = EncodeBeforeRender<wmtcmd_compute_dispatch>();
  cmd_dispatch_res.type = WMTComputeCommandDispatch;
  cmd_dispatch_res.size = {1, 1, 1};

  if (draws)
    return data;

  auto &cmd_use_icb = EncodeRenderCommand<wmtcmd_render_useresource>();
  cmd_use_icb.type = WMTRenderCommandUseResource;
  cmd_use_icb.stages = WMTRenderStageVertex;
  cmd_use_icb.usage = WMTResourceUsageRead;
  cmd_use_icb.resource = icb;

  auto &cmd = EncodeRenderCommand<wmtcmd_render_executecommands>();
  cmd.type = WMTRenderCommandExecuteCommandsInBuffer;
  cmd.indirect_command_buffer = icb;
  cmd.location = 0;
  cmd.length = MaxCount * each;

  icb_.push_back(std::move(icb));

  return data;
}

template <>
WMT::Reference<WMT::ComputePipelineState>
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::getComputePipeline(std::string name) {
  auto lib = ctx.device_->GetLib().getLibrary();
  auto func = lib.newFunction(name.c_str());
  if (!func)
    return {};
  WMT::Reference<WMT::Error> err;
  auto pso = ctx.device_->GetMTLDevice().newComputePipelineState(func, err);
  if (err) {
    ERR("Failed to create compute PSO: ", err.description().getUTF8String());
  }
  return pso;
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::startComputePass() {
  ctx.InvalidateCurrentPass();
  auto compute = ctx.AllocatePass<ComputeEncoderData>();
  compute->type = EncoderType::Compute;
  compute->cmd_head.type = WMTComputeCommandNop;
  compute->cmd_head.next.set(0);
  compute->cmd_tail = (wmtcmd_base *)&compute->cmd_head;
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::endPass() {
  ctx.InvalidateCurrentPass();
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setComputePSO(WMT::ComputePipelineState pso, WMTSize tgsize) {
  auto &setpso = ctx.EncodeComputeCommand<wmtcmd_compute_setpso>();
  setpso.type = WMTComputeCommandSetPSO;
  setpso.pso = pso;
  setpso.threadgroup_size = tgsize;
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::dispatch(WMTSize size) {
  auto &dispatch = ctx.EncodeComputeCommand<wmtcmd_compute_dispatch>();
  dispatch.type = WMTComputeCommandDispatchThreads;
  dispatch.size = size;
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setComputeTexture(
    uint32_t index, const Rc<Texture> &texture, uint64_t viewId, int flags
) {
  auto &dst_ = texture->view(viewId);
  auto &settex = ctx.EncodeComputeCommand<wmtcmd_compute_settexture>();
  settex.type = WMTComputeCommandSetTexture;
  settex.texture = dst_.texture;
  settex.index = index;
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setComputeTexelBuffer(
    uint32_t index, const Rc<Buffer> &buffer, uint64_t viewId, int flags
) {
  auto &dst_ = buffer->view_(viewId);
  auto &settexbuf = ctx.EncodeComputeCommand<wmtcmd_compute_settexture>();
  settexbuf.type = WMTComputeCommandSetTexture;
  settexbuf.texture = dst_.texture;
  settexbuf.index = index;
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setComputeBuffer(
    uint32_t index, const Rc<Buffer> &buffer, uint32_t offset, uint32_t length, int flags
) {
  auto dst_ = buffer->current();
  auto &setbuf = ctx.EncodeComputeCommand<wmtcmd_compute_setbuffer>();
  setbuf.type = WMTComputeCommandSetBuffer;
  setbuf.buffer = dst_->buffer();
  setbuf.index = index;
  setbuf.offset = 0; // the `offset` and `length` parameter of this function are just for (potential) hazard tracking,
                     // which we don't need here
}

template <>
void *
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setComputeBytes(uint32_t index, uint32_t length) {
  auto &setmeta = ctx.EncodeComputeCommand<wmtcmd_compute_setbytes>();
  setmeta.type = WMTComputeCommandSetBytes;
  void *temp = ctx.AllocateCPUHeap(length, 16);
  setmeta.bytes.set(temp);
  setmeta.length = length;
  setmeta.index = index;
  return temp;
}

template <>
WMT::Library
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::getDefaultLibrary() {
  return ctx.device_->GetLib().getLibrary();
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::startRenderPass() {
  ctx.InvalidateCurrentPass();
  auto render = ctx.AllocatePass<RenderEncoderData>();
  render->type = EncoderType::Render;
  render->before_head.type = WMTComputeCommandNop;
  render->before_head.next.set(0);
  render->before_tail = (wmtcmd_base *)&render->before_head;
  render->most = nullptr;
  render->cmd_head.type = WMTRenderCommandNop;
  render->cmd_head.next.set(0);
  render->cmd_tail = (wmtcmd_base *)&render->cmd_head;
  render->dsv_planar_flags = 0;
  render->dsv_readonly_flags = 0;
  render->render_target_count = 0;
  render->render_target_width = 16384;
  render->render_target_height = 16384;
  render->render_target_array_length = 0;
  render->default_raster_sample_count = 1;
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setColorAttachment(
    uint32_t index, const Rc<Texture> &texture, uint64_t viewId, uint32_t depth_plane
) {
  auto encoder = static_cast<RenderEncoderData *>(ctx.encoder_current);
  encoder->colors[index].attachment = texture->view(viewId);
  encoder->colors[index].depth_plane = depth_plane;
  encoder->colors[index].load_action = WMTLoadActionLoad;
  encoder->colors[index].store_action = WMTStoreActionStore;
  encoder->render_target_count = std::max<uint32_t>(encoder->render_target_count, index + 1);
  encoder->render_target_width = std::min<uint32_t>(encoder->render_target_width, texture->width(viewId));
  encoder->render_target_height = std::min<uint32_t>(encoder->render_target_height, texture->height(viewId));
  encoder->render_target_array_length = texture->textureType() == WMTTextureType3D
                                            ? (texture->depth(viewId) - depth_plane)
                                            : texture->arrayLength(viewId);
  encoder->default_raster_sample_count = texture->sampleCount();
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setDepthStencilAttachment(
    const Rc<Texture> &texture, uint64_t viewId, uint32_t dsv_flag
) {
  auto encoder = static_cast<RenderEncoderData *>(ctx.encoder_current);
  if (dsv_flag & 1) {
    encoder->depth.attachment = texture->view(viewId);
    encoder->depth.load_action = WMTLoadActionLoad;
    encoder->depth.store_action = WMTStoreActionStore;
  }
  if (dsv_flag & 2) {
    encoder->stencil.attachment = texture->view(viewId);
    encoder->stencil.load_action = WMTLoadActionLoad;
    encoder->stencil.store_action = WMTStoreActionStore;
  }
  encoder->dsv_planar_flags = dsv_flag;
  encoder->render_target_width = std::min<uint32_t>(encoder->render_target_width, texture->width(viewId));
  encoder->render_target_height = std::min<uint32_t>(encoder->render_target_height, texture->height(viewId));
  encoder->render_target_array_length = texture->arrayLength(viewId);
  encoder->default_raster_sample_count = texture->sampleCount();
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setRenderPSO(WMT::RenderPipelineState pso) {
  auto &cmd = ctx.EncodeRenderCommand<wmtcmd_render_setpso>();
  cmd.type = WMTRenderCommandSetPSO;
  cmd.pso = pso;
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setViewport(WMTViewport viewport) {
  auto &cmd = ctx.EncodeRenderCommand<wmtcmd_render_setviewport>();
  cmd.type = WMTRenderCommandSetViewport;
  cmd.viewport = viewport;
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setDepthStencilState(WMT::DepthStencilState dsso) {
  auto &cmd = ctx.EncodeRenderCommand<wmtcmd_render_setdepthstencilstate>();
  cmd.type = WMTRenderCommandSetDepthStencilState;
  cmd.depth_stencil_state = dsso;
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setScissorRect(WMTScissorRect rect) {
  // Metal wants a scissor within the pass
  auto encoder = static_cast<RenderEncoderData *>(ctx.encoder_current);
  rect.x = std::min<uint64_t>(rect.x, encoder->render_target_width);
  rect.y = std::min<uint64_t>(rect.y, encoder->render_target_height);
  rect.width = std::min<uint64_t>(rect.width, encoder->render_target_width - rect.x);
  rect.height = std::min<uint64_t>(rect.height, encoder->render_target_height - rect.y);
  auto &cmd = ctx.EncodeRenderCommand<wmtcmd_render_setscissorrect>();
  cmd.type = WMTRenderCommandSetScissorRect;
  cmd.scissor_rect = rect;
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setStencilReference(uint8_t stencil_ref) {
  auto &cmd = ctx.EncodeRenderCommand<wmtcmd_render_setstencilref>();
  cmd.type = WMTRenderCommandSetStencilRef;
  cmd.stencil_ref = stencil_ref;
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setFragmentTexture(
    uint32_t index, const Rc<Texture> &texture, uint64_t viewId
) {
  auto &settex = ctx.EncodeRenderCommand<wmtcmd_render_settexture>();
  settex.type = WMTRenderCommandSetFragmentTexture;
  settex.texture = texture->view(viewId).texture;
  settex.index = index;
}

template <>
void *
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setFragmentBytes(uint32_t index, uint32_t length) {
  auto &setmeta = ctx.EncodeRenderCommand<wmtcmd_render_setbytes>();
  setmeta.type = WMTRenderCommandSetFragmentBytes;
  void *temp = ctx.AllocateCPUHeap(length, 16);
  setmeta.bytes.set(temp);
  setmeta.length = length;
  setmeta.index = index;
  return temp;
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::draw(
    WMTPrimitiveType primitive, uint32_t vertex_start, uint32_t vertex_count, int32_t base_instance,
    uint32_t instance_count
) {
  auto &draw = ctx.EncodeRenderCommand<wmtcmd_render_draw>();
  draw.type = WMTRenderCommandDraw;
  draw.primitive_type = primitive;
  draw.vertex_start = vertex_start;
  draw.vertex_count = vertex_count;
  draw.base_instance = base_instance;
  draw.instance_count = instance_count;
}

}; // namespace dxmt
