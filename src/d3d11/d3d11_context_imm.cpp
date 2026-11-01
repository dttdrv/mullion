#include "config/config.hpp"
#include "d3d11_fence.hpp"
#include "d3d11_private.h"
#include "d3d11_query.hpp"
#include "d3d11_video_context.hpp"
#include "mullion_command_queue.hpp"
#include "d3d11_context_impl.cpp"
#include "mullion_context.hpp"
#include "mullion_staging.hpp"

namespace mullion {
struct ContextInternalState {
  using device_mutex_t = d3d11_device_mutex;
  CommandQueue &cmd_queue;
  bool has_dirty_op_since_last_event = false;
  uint64_t acquired_seq = 0;
  std::unordered_map<Texture *, std::pair<Com<D3D11ResourceCommon, false>, std::function<void(ArgumentEncodingContext &)>>> returned;
};


template<typename Object> Rc<Object> forward_rc(Rc<Object>& obj) {
  return std::move(obj);
}

using ImmediateContextBase = MTLD3D11DeviceContextImplBase<ContextInternalState>;

template <>
D3D11_QUERY_DATA_PIPELINE_STATISTICS &
ImmediateContextBase::PipelineCounts() {
  return pipeline_counts_;
}

template <>
template <CommandWithContext<ArgumentEncodingContext> cmd>
void
ImmediateContextBase::EmitST(cmd &&fn) {
  CommandChunk *chk = ctx_state.cmd_queue.CurrentChunk();
  chk->emitcc(std::forward<cmd>(fn));
}

template <>
template <CommandWithContext<ArgumentEncodingContext> cmd>
void
ImmediateContextBase::EmitOP(cmd &&fn) {
  CommandChunk *chk = ctx_state.cmd_queue.CurrentChunk();
  chk->emitcc(std::forward<cmd>(fn));
  ctx_state.has_dirty_op_since_last_event = true;
}

template <>
template <typename T>
moveonly_list<T>
ImmediateContextBase::AllocateCommandData(size_t n) {
  CommandChunk *chk = ctx_state.cmd_queue.CurrentChunk();
  return moveonly_list<T>((T *)chk->allocate_cpu_heap(sizeof(T) * n, alignof(T)), n);
}

template <>
std::tuple<WMT::Buffer, uint64_t>
ImmediateContextBase::AllocateStagingBuffer(size_t size, size_t alignment) {
  return ctx_state.cmd_queue.AllocateStagingBuffer(size, alignment);
}

template <>
void
ImmediateContextBase::UseCopyDestination(Rc<StagingResource> &staging) {
  staging->useCopyDestination(ctx_state.cmd_queue.CurrentSeqId());
}

template <>
void
ImmediateContextBase::UseCopySource(Rc<StagingResource> &staging) {
  staging->useCopySource(ctx_state.cmd_queue.CurrentSeqId());
}

template <>
void
ImmediateContextBase::RecordWrappedUse(D3D11ResourceCommon *resource, const ResourceSubsetState &subset, bool write) {
  auto seq = ctx_state.cmd_queue.CurrentSeqId();
  for (auto &use : resource->wrapped_state->uses)
    if (use.subset.overlapWith(subset))
      (write ? use.written : use.read) = seq;
}

template <>
void
ImmediateContextBase::PrepareTextureUse(std::span<const Rc<Texture>> textures) {
  if (ctx_state.returned.empty())
    return;
  bool split = false;
  for (const auto &texture : textures) {
    auto found = ctx_state.returned.find(texture.ptr());
    if (found == ctx_state.returned.end())
      continue;
    if (!split) {
      InvalidateCurrentPass(true);
      Commit();
      split = true;
    }
    found->second.first->wrapped_state->returned_seq = ctx_state.cmd_queue.CurrentSeqId();
    EmitST([before = std::move(found->second.second)](ArgumentEncodingContext &enc) { before(enc); });
    ctx_state.returned.erase(found);
  }
}

template <>
std::pair<BufferAllocation *, uint32_t>
ImmediateContextBase::GetDynamicBufferAllocation(Rc<DynamicBuffer> &dynamic) {
  return {dynamic->immediateName().ptr(), dynamic->immediateSuballocation()};
}

class MTLD3D11ImmediateContext : public ImmediateContextBase {
public:
  MTLD3D11ImmediateContext(MTLD3D11Device *pDevice, CommandQueue &cmd_queue) :
      ImmediateContextBase(pDevice, ctx_state, pDevice->mutex),
      cmd_queue(cmd_queue),
      ctx_state({cmd_queue}),
      d3dmt_(this, mutex),
      video_(this) {
        ignore_map_flag_no_wait_ = Config::getInstance().getOption<bool>("d3d11.ignoreMapFlagNoWait", false);
      }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) override {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(ID3D11Multithread)) {
      *ppvObject = ref(&d3dmt_);
      return S_OK;
    }

    if (riid == __uuidof(ID3D11VideoContext)) {
      *ppvObject = ref(&video_);
      return S_OK;
    }

    return ImmediateContextBase::QueryInterface(riid, ppvObject);
  }

  ULONG STDMETHODCALLTYPE
  AddRef() override {
    uint32_t refCount = this->refcount++;
    if (unlikely(!refCount))
      this->m_parent->AddRef();

    return refCount + 1;
  }

  ULONG STDMETHODCALLTYPE
  Release() override {
    uint32_t refCount = --this->refcount;
    D3D11_ASSERT(refCount != ~0u && "try to release a 0 reference object");
    if (unlikely(!refCount))
      this->m_parent->Release();

    return refCount;
  }

  void *
  MapDynamicBuffer(Rc<DynamicBuffer> &dynamic, uint64_t current_seq_id, uint64_t coherent_seq_id) {
    if (auto next_sub = dynamic->nextSuballocation()) {
      EmitST([allocation = dynamic->immediateName(), next_sub](ArgumentEncodingContext &enc) mutable {
        allocation->useSuballocation(next_sub);
      });
    } else {
      dynamic->updateImmediateName(current_seq_id, dynamic->allocate(coherent_seq_id), 0, false);
      EmitST([allocation = dynamic->immediateName(),
              buffer = Rc(dynamic->buffer)](ArgumentEncodingContext &enc) mutable {
        allocation->useSuballocation(0);
        auto _ = buffer->rename(forward_rc(allocation));
      });
    }

    return dynamic->immediateMappedMemory();
  }

  HRESULT
  MapResource(ID3D11Resource *pResource, UINT Subresource, D3D11_MAP MapType, UINT MapFlags,
              D3D11_MAPPED_SUBRESOURCE *pMappedResource) override {
    if (auto &wrapped = GetResourceCommon(pResource)->wrapped_resource) {
      D3D12_RANGE no_read{};
      HRESULT hr = wrapped->Map(Subresource, MapType == D3D11_MAP_WRITE ? &no_read : nullptr,
                                pMappedResource ? &pMappedResource->pData : nullptr);
      if (FAILED(hr))
        return hr;
      auto common = GetResourceCommon(pResource);
      if (ctx_state.returned.contains(GetTexture(pResource).ptr())) {
        PrepareTextureUse({GetTexture(pResource)});
      }
      const auto &use = common->wrapped_state->uses[Subresource];
      auto seq = std::max({ctx_state.acquired_seq, common->wrapped_state->returned_seq,
                          MapType == D3D11_MAP_READ ? use.written : std::max(use.read, use.written)});
      if (seq == cmd_queue.CurrentSeqId()) {
        InvalidateCurrentPass(true);
        Commit();
      }
      if ((MapFlags & D3D11_MAP_FLAG_DO_NOT_WAIT) && cmd_queue.CoherentSeqId() < seq) {
        wrapped->Unmap(Subresource, nullptr);
        if (pMappedResource)
          *pMappedResource = {};
        return DXGI_ERROR_WAS_STILL_DRAWING;
      }
      cmd_queue.WaitCPUFence(seq);
      if (SUCCEEDED(hr) && pMappedResource) {
        pMappedResource->RowPitch = wrapped->GetDesc().Width;
        pMappedResource->DepthPitch = pMappedResource->RowPitch;
      }
      return hr;
    }
    UINT buffer_length = 0, &row_pitch = buffer_length;
    UINT bind_flag = 0, &depth_pitch = bind_flag;
    auto current_seq_id = cmd_queue.CurrentSeqId();
    auto coherent_seq_id = cmd_queue.CoherentSeqId();
    if (auto dynamic = GetDynamicBuffer(pResource, &buffer_length, &bind_flag)) {
      if (MapType == D3D11_MAP_WRITE_DISCARD) {
        if (bind_flag & D3D11_BIND_VERTEX_BUFFER) {
          state_.InputAssembler.VertexBuffers.set_dirty();
        }
        if (bind_flag & D3D11_BIND_CONSTANT_BUFFER) {
          for (auto &stage : state_.ShaderStages) {
            stage.ConstantBuffers.set_dirty();
          }
        }
        if (bind_flag & D3D11_BIND_SHADER_RESOURCE) {
          for (auto &stage : state_.ShaderStages) {
            stage.SRVs.set_dirty();
          }
        }

        pMappedResource->pData = MapDynamicBuffer(dynamic, current_seq_id, coherent_seq_id);
        pMappedResource->RowPitch = buffer_length;
        pMappedResource->DepthPitch = buffer_length;
      } else {
        pMappedResource->pData = dynamic->immediateMappedMemory();
        pMappedResource->RowPitch = buffer_length;
        pMappedResource->DepthPitch = buffer_length;
      }
      return S_OK;
    }
    if (auto dynamic = GetDynamicTexture(pResource, Subresource, &row_pitch, &depth_pitch)) {
      for (auto &stage : state_.ShaderStages) {
        stage.SRVs.set_dirty();
      }

      pMappedResource->pData = MapDynamicBuffer(dynamic, current_seq_id, coherent_seq_id);
      pMappedResource->RowPitch = row_pitch;
      pMappedResource->DepthPitch = depth_pitch;
      return S_OK;
    }
    if (auto dynamic = GetDynamicLinearTexture(pResource, &row_pitch, &depth_pitch)) {
      for (auto &stage : state_.ShaderStages) {
        stage.SRVs.set_dirty();
      }

      dynamic->updateImmediateName(current_seq_id, dynamic->allocate(coherent_seq_id), false);
      EmitST([allocation = dynamic->immediateName(),
            texture = Rc(dynamic->texture)](ArgumentEncodingContext &enc) mutable {
        auto _ = texture->rename(forward_rc(allocation));
      });

      pMappedResource->pData = dynamic->mappedMemory();
      pMappedResource->RowPitch = row_pitch;
      pMappedResource->DepthPitch = depth_pitch;
      return S_OK;
    }
    if (auto staging = GetStagingResource(pResource, Subresource)) {
      if (ignore_map_flag_no_wait_)
        MapFlags &= ~D3D11_MAP_FLAG_DO_NOT_WAIT;

      while (true) {
        auto result = staging->tryMap(coherent_seq_id, MapType & D3D11_MAP_READ, MapType & D3D11_MAP_WRITE);
        if (result == StagingMapResult::Mapped)
          return E_FAIL;
        if (result == StagingMapResult::Renamable) {
          // when write to a buffer that is gpu-readonly
          auto next_name = staging->allocate(coherent_seq_id);
          // can't guarantee a full overwrite
          std::memcpy(staging->mappedMemory(next_name), staging->mappedImmediateMemory(), staging->length);
          staging->updateImmediateName(current_seq_id, next_name);
          EmitST([staging, next_name](ArgumentEncodingContext &enc) mutable { 
            auto _ = staging->buffer()->rename(staging->allocation(next_name));
          });
          result = StagingMapResult::Mappable;
        }
        if (result == StagingMapResult::Mappable) {
          TRACE("staging map ready");
          pMappedResource->pData = staging->mappedImmediateMemory();
          pMappedResource->RowPitch = staging->bytesPerRow;
          pMappedResource->DepthPitch = staging->bytesPerImage;
          return S_OK;
        }
        // even it's in a while loop
        // only the first flush will have effect
        // and the following calls are essentially no-op
        // what the resource waits for is submitted either way, so an application that asks again finds it done
        Flush();
        if (MapFlags & D3D11_MAP_FLAG_DO_NOT_WAIT) {
          return DXGI_ERROR_WAS_STILL_DRAWING;
        }
        TRACE("staging map block");
        auto& statistics = cmd_queue.CurrentFrameStatistics();
        auto t0 = clock::now();
        cmd_queue.WaitCPUFence(coherent_seq_id + uint64_t(result));
        auto t1 = clock::now();
        statistics.sync_count++;
        statistics.sync_interval += (t1 - t0);
        current_seq_id = cmd_queue.CurrentSeqId();
        coherent_seq_id = cmd_queue.CoherentSeqId();
      };
    };
    return E_INVALIDARG;
  }

  void
  STDMETHODCALLTYPE
  Unmap(ID3D11Resource *pResource, UINT Subresource) override {
    std::lock_guard<d3d11_device_mutex> lock(mutex);

    if (unlikely(!pResource))
      return;
    if (auto &wrapped = GetResourceCommon(pResource)->wrapped_resource) {
      wrapped->Unmap(Subresource, nullptr);
      return;
    }
    UINT row_pitch = 0;
    UINT depth_pitch = 0;
    if (auto staging = GetStagingResource(pResource, Subresource)) {
      staging->unmap();
    };
    if (auto dynamic = GetDynamicTexture(pResource, Subresource, &row_pitch, &depth_pitch)) {
      BlitObject texture(device, pResource);
      UpdateTexture(TextureUpdateCommand(texture, Subresource, nullptr),
                    dynamic->buffer, row_pitch, depth_pitch);
    }
  }

  void
  STDMETHODCALLTYPE
  Begin(ID3D11Asynchronous *pAsync) override {
    if (unlikely(!pAsync))
      return;

    std::lock_guard<d3d11_device_mutex> lock(mutex);

    // in theory pAsync could be any of them: { Query, Predicate, Counter }.
    // However `Predicate` and `Counter` are not supported at all
    D3D11_QUERY_DESC desc;
    ((ID3D11Query *)pAsync)->GetDesc(&desc);
    switch (desc.Query) {
    case D3D11_QUERY_TIMESTAMP_DISJOINT:
      static_cast<MTLD3D11Query *>(pAsync)->state_ = QueryState::Building;
      break;
    case D3D11_QUERY_TIMESTAMP:
    case D3D11_QUERY_EVENT:
      break;
    case D3D11_QUERY_OCCLUSION:
    case D3D11_QUERY_OCCLUSION_PREDICATE:
    case D3D11_QUERY_PIPELINE_STATISTICS: {
      if (desc.Query == D3D11_QUERY_PIPELINE_STATISTICS)
        InvalidateCurrentPass(true);
      if (static_cast<MTLD3D11Query *>(pAsync)->state_ == QueryState::Building)
        End(pAsync);
      static_cast<MTLD3D11OcclusionQuery *>(pAsync)->begun = pipeline_counts_;
      auto query = static_cast<MTLD3D11OcclusionQuery *>(pAsync)->Begin();
      VisibilityQueryBoundary(active_visibility_queries_++ != 0);
      EmitST([query = Rc(query)](ArgumentEncodingContext &enc) mutable {
        enc.beginVisibilityResultQuery(std::move(query));
      });
      break;
    }
    case D3D11_QUERY_SO_STATISTICS:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE:
    case D3D11_QUERY_SO_STATISTICS_STREAM0:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0:
    case D3D11_QUERY_SO_STATISTICS_STREAM1:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM1:
    case D3D11_QUERY_SO_STATISTICS_STREAM2:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM2:
    case D3D11_QUERY_SO_STATISTICS_STREAM3:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3: {
      static_cast<MTLD3D11Query *>(pAsync)->state_ = QueryState::Building;
      SnapshotStreamOutputStatistics(static_cast<MTLD3D11StreamOutputQuery *>(pAsync)->Snapshots(), 0);
      break;
    }
    default:
      ERR("Unknown query type ", desc.Query);
      break;
    }
  }

  // See Begin()
  void
  STDMETHODCALLTYPE
  End(ID3D11Asynchronous *pAsync) override {
    if (unlikely(!pAsync))
      return;

    std::lock_guard<d3d11_device_mutex> lock(mutex);

    D3D11_QUERY_DESC desc;
    ((ID3D11Query *)pAsync)->GetDesc(&desc);
    switch (desc.Query) {
    case D3D11_QUERY_TIMESTAMP_DISJOINT:
    case D3D11_QUERY_EVENT: {
      if (ctx_state.has_dirty_op_since_last_event) {
        auto event_id = cmd_queue.GetNextEventSeqId();
        static_cast<MTLD3D11EventQuery *>(pAsync)->Issue(event_id);
        InvalidateCurrentPass(true);
        EmitOP([event_id](ArgumentEncodingContext &enc) mutable {
          enc.signalEvent(event_id);
        });
        promote_flush = true;
        ctx_state.has_dirty_op_since_last_event = false;
      } else {
        static_cast<MTLD3D11EventQuery *>(pAsync)->Issue(cmd_queue.GetCurrentEventSeqId());
      }
      break;
    }
    case D3D11_QUERY_TIMESTAMP: {
      if (auto query = static_cast<MTLD3D11TimestampQuery *>(pAsync)->End()) {
        InvalidateCurrentPass(true);
        EmitOP([query = Rc(query)](ArgumentEncodingContext &enc) mutable {
          enc.sampleTimestamp(std::move(query));
        });
      }
      break;
    }
    case D3D11_QUERY_OCCLUSION:
    case D3D11_QUERY_OCCLUSION_PREDICATE:
    case D3D11_QUERY_PIPELINE_STATISTICS: {
      if (static_cast<MTLD3D11Query *>(pAsync)->state_ != QueryState::Building)
        Begin(pAsync);
      auto occlusion = static_cast<MTLD3D11OcclusionQuery *>(pAsync);
      occlusion->counts = PipelineCountsSince(occlusion->begun);
      auto query = occlusion->End();
      VisibilityQueryBoundary(active_visibility_queries_-- != 0);
      EmitST([query = Rc(query)](ArgumentEncodingContext &enc) mutable {
        enc.endVisibilityResultQuery(std::move(query));
      });
      promote_flush = true;
      if (desc.Query == D3D11_QUERY_PIPELINE_STATISTICS)
        InvalidateCurrentPass(true);
      break;
    }
    case D3D11_QUERY_SO_STATISTICS:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE:
    case D3D11_QUERY_SO_STATISTICS_STREAM0:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0:
    case D3D11_QUERY_SO_STATISTICS_STREAM1:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM1:
    case D3D11_QUERY_SO_STATISTICS_STREAM2:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM2:
    case D3D11_QUERY_SO_STATISTICS_STREAM3:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3: {
      // the event tells when the copy is done
      auto query = static_cast<MTLD3D11StreamOutputQuery *>(pAsync);
      if (query->state_ != QueryState::Building)
        Begin(pAsync);
      SnapshotStreamOutputStatistics(query->Snapshots(), 1);
      auto event_id = cmd_queue.GetNextEventSeqId();
      query->Issue(event_id);
      InvalidateCurrentPass(true);
      EmitOP([event_id](ArgumentEncodingContext &enc) mutable {
        enc.signalEvent(event_id);
      });
      promote_flush = true;
      ctx_state.has_dirty_op_since_last_event = false;
      break;
    }
    default:
      ERR("Unknown query type ", desc.Query);
      break;
    }
  }

  HRESULT
  STDMETHODCALLTYPE
  GetData(ID3D11Asynchronous *pAsync, void *pData, UINT DataSize, UINT GetDataFlags) override {
    std::lock_guard<d3d11_device_mutex> lock(mutex);
    if (!pAsync || (DataSize && !pData))
      return E_INVALIDARG;

    // Allow dataSize to be zero
    if (DataSize && DataSize != pAsync->GetDataSize())
      return E_INVALIDARG;

    if (!DataSize)
      pData = nullptr;
    auto state = static_cast<MTLD3D11Query *>(pAsync)->state_;
    if (state == QueryState::Undefined || state == QueryState::Building)
      return DXGI_ERROR_INVALID_CALL;

    HRESULT hr = S_FALSE;

    D3D11_QUERY_DESC desc;
    ((ID3D11Query *)pAsync)->GetDesc(&desc);
    switch (desc.Query) {
    case D3D11_QUERY_EVENT:
    case D3D11_QUERY_TIMESTAMP_DISJOINT: {
      switch (static_cast<MTLD3D11EventQuery *>(pAsync)->CheckEventState(cmd_queue.SignaledEventSeqId())) {
      case EventState::Pending:
        break;
      case EventState::Stall:
        GetDataFlags &= ~D3D11_ASYNC_GETDATA_DONOTFLUSH;
        break;
      case EventState::Signaled:
        hr = S_OK;
        break;
      }
      break;
    }
    default:
      break;
    }
    switch (desc.Query) {
    case D3D11_QUERY_EVENT: {
      if (pData && hr == S_OK)
        *static_cast<BOOL *>(pData) = TRUE;
      break;
    }
    case D3D11_QUERY_OCCLUSION: {
      uint64_t null_data;
      uint64_t *data_ptr = pData ? (uint64_t *)pData : &null_data;
      hr = static_cast<MTLD3D11OcclusionQuery *>(pAsync)->GetData(data_ptr);
      break;
    }
    case D3D11_QUERY_OCCLUSION_PREDICATE: {
      BOOL null_data;
      BOOL *data_ptr = pData ? (BOOL *)pData : &null_data;
      hr = static_cast<MTLD3D11OcclusionQuery *>(pAsync)->GetData(data_ptr);
      break;
    }
    case D3D11_QUERY_TIMESTAMP: {
      uint64_t null_data;
      uint64_t *data_ptr = pData ? (uint64_t *)pData : &null_data;
      hr = static_cast<MTLD3D11TimestampQuery *>(pAsync)->GetData(data_ptr);
      break;
    }
    case D3D11_QUERY_TIMESTAMP_DISJOINT: {
      if (pData && hr == S_OK) {
        (*static_cast<D3D11_QUERY_DATA_TIMESTAMP_DISJOINT *>(pData)) = {1'000'000'000, FALSE};
      }
      break;
    }
    case D3D11_QUERY_PIPELINE_STATISTICS: {
      D3D11_QUERY_DATA_PIPELINE_STATISTICS null_data;
      hr = static_cast<MTLD3D11OcclusionQuery *>(pAsync)->GetData(pData ? pData : &null_data);
      break;
    }
    case D3D11_QUERY_SO_STATISTICS:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE:
    case D3D11_QUERY_SO_STATISTICS_STREAM0:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0:
    case D3D11_QUERY_SO_STATISTICS_STREAM1:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM1:
    case D3D11_QUERY_SO_STATISTICS_STREAM2:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM2:
    case D3D11_QUERY_SO_STATISTICS_STREAM3:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3: {
      D3D11_QUERY_DATA_SO_STATISTICS null_data;
      hr = static_cast<MTLD3D11StreamOutputQuery *>(pAsync)->GetData(
          pData ? pData : &null_data, cmd_queue.SignaledEventSeqId()
      );
      break;
    }
    default:
      ERR("Unknown query type ", desc.Query);
      return E_FAIL;
    }
    if (hr == S_FALSE && (GetDataFlags & D3D11_ASYNC_GETDATA_DONOTFLUSH) == 0) {
      cmd_queue.CurrentFrameStatistics().event_stall++;
      Flush();
    }
    // polling within a frame until the query finishes is a wait; a query looked at again a frame later is not
    if (diag::profile) {
      auto query = static_cast<MTLD3D11Query *>(pAsync);
      if (query->polled && query->polled_frame != cmd_queue.CurrentFrameSeq())
        query->polled = 0;
      if (hr == S_FALSE && !query->polled)
        query->polled = diag::Now(), query->polled_frame = cmd_queue.CurrentFrameSeq();
      else if (hr == S_OK && query->polled)
        diag::Waited("query", std::exchange(query->polled, 0));
    }
    return hr;
  }

  void
  STDMETHODCALLTYPE
  Flush() override {
    Flush1(D3D11_CONTEXT_TYPE_ALL, NULL);
  }

  void
  STDMETHODCALLTYPE
  Flush1(D3D11_CONTEXT_TYPE Type, HANDLE hEvent) override {
    std::lock_guard<d3d11_device_mutex> lock(mutex);

    if (ctx_state.has_dirty_op_since_last_event || promote_flush) {
      InvalidateCurrentPass(true);
      Commit();
    }
    if (hEvent) {
      auto shared_event_listener = cmd_queue.GetSharedEventListener();
      MTLSharedEvent_setWin32EventAtValue(
          cmd_queue.event, shared_event_listener, hEvent, cmd_queue.GetCurrentEventSeqId(), true
      );
    }
  }

  void PrepareFlush() override {
    InvalidateCurrentPass(true);
  }

  // whether a predicate's data is the value that skips. data that is not there yet is waited for, unless the
  // predicate is a hint, whose operations then run (D3D11.3 20.2)
  bool
  Skips(ID3D11Query *predicate, BOOL value) {
    if (!predicate)
      return false;
    D3D11_QUERY_DESC desc;
    predicate->GetDesc(&desc);
    BOOL data;
    HRESULT hr = GetData(predicate, &data, sizeof(data), D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (hr == S_FALSE && !(desc.MiscFlags & D3D11_QUERY_MISC_PREDICATEHINT)) {
      WaitUntilGPUIdle();
      hr = GetData(predicate, &data, sizeof(data), D3D11_ASYNC_GETDATA_DONOTFLUSH);
    }
    return hr == S_OK && data == value;
  }

  bool
  SkipsPredicated() override {
    return Skips(state_.predicate.ptr(), state_.predicate_value);
  }

  // what a command list changes in the context, then its regions in order, a list it holds executed in its place
  void
  Execute(ID3D11CommandList *pCommandList) {
    Com<MTLD3D11CommandList, false> cmdlist = static_cast<MTLD3D11CommandList *>(pCommandList);
    auto seq_id = ctx_state.cmd_queue.CurrentSeqId();

    promote_flush |= cmdlist->promote_flush;

    // shared by the list's regions, which a wait for a predicate's data puts in chunks of their own
    auto query_list = std::make_shared<std::vector<Rc<VisibilityResultQuery>>>(cmdlist->issued_visibility_query.size());
    for (size_t i = 0; i < cmdlist->issued_visibility_query.size(); i++) {
      (*query_list)[i] = new VisibilityResultQuery();
      D3D11_QUERY_DESC desc;
      cmdlist->issued_visibility_query[i].first->GetDesc(&desc);
      (*query_list)[i]->pipeline_statistics = desc.Query == D3D11_QUERY_PIPELINE_STATISTICS;
      cmdlist->issued_visibility_query[i].first->DoDeferredQuery((*query_list)[i].ptr());
    }

    for (const auto &query : cmdlist->ended_queries) {
      End(query.ptr());
    }

    for (const auto &used_dynamic : cmdlist->used_dynamic_buffers) {
      if (!used_dynamic.latest)
        continue;
      used_dynamic.buffer->updateImmediateName(seq_id, Rc(used_dynamic.allocation), used_dynamic.suballocation, true);
    }

    for (const auto &used_dynamic : cmdlist->used_dynamic_lineartextures) {
      if (!used_dynamic.latest)
        continue;
      used_dynamic.texture->updateImmediateName(seq_id, Rc(used_dynamic.allocation), true);
    }

    for (size_t region = 0; region < cmdlist->regions.size(); region++) {
      if (auto &nested = cmdlist->regions[region].nested) {
        Execute(nested.ptr());
        continue;
      }
      bool skips = Skips(cmdlist->regions[region].predicate.ptr(), cmdlist->regions[region].value);
      PrepareTextureUse(cmdlist->regions[region].textures);
      for (const auto &staging : cmdlist->regions[region].read_staging_resources)
        staging->useCopySource(cmd_queue.CurrentSeqId());
      for (const auto &staging : cmdlist->regions[region].written_staging_resources)
        staging->useCopyDestination(cmd_queue.CurrentSeqId());
      for (const auto &[resource, subset, write] : cmdlist->regions[region].wrapped_uses)
        RecordWrappedUse(resource.ptr(), subset, write);
      std::shared_ptr<std::vector<Rc<TimestampQuery>>> timestamp_queries;
      if (auto &queries = cmdlist->regions[region].issued_timestamp_query; !queries.empty()) {
        timestamp_queries = std::make_shared<std::vector<Rc<TimestampQuery>>>();
        for (const auto &query : queries)
          timestamp_queries->emplace_back(query->End());
      }
      EmitOP([cmdlist, query_list, timestamp_queries, region, skips](ArgumentEncodingContext &enc) {
        // Finished command list should clean up the encoding context
        enc.pushDeferredVisibilityQueries(query_list->data());
        enc.skips_predicated = skips;
        auto previous = enc.deferred_timestamp_queries;
        enc.deferred_timestamp_queries = timestamp_queries ? timestamp_queries->data() : nullptr;
        cmdlist->Execute(enc, region);
        enc.deferred_timestamp_queries = previous;
        enc.skips_predicated = false;
        enc.popDeferredVisibilityQueries();
      });
      const auto &part = cmdlist->regions[region];
      if (!skips) {
        pipeline_counts_.IAVertices += part.pipeline_counts.IAVertices;
        pipeline_counts_.IAPrimitives += part.pipeline_counts.IAPrimitives;
        pipeline_counts_.VSInvocations += part.pipeline_counts.VSInvocations;
        pipeline_counts_.CInvocations += part.pipeline_counts.CInvocations;
        pipeline_counts_.CPrimitives += part.pipeline_counts.CPrimitives;
      }
      if (auto &query = part.pipeline_query) {
        if (part.begins_pipeline_query)
          query->begun = pipeline_counts_;
        else
          query->counts = PipelineCountsSince(query->begun);
      }
      // as End does for a query the immediate context ends: an event tells when the copies are done, here those of
      // the queries the region ended, which a later region's predicate may be
      auto &ended = cmdlist->regions[region].ended_stream_output_queries;
      if (ended.empty())
        continue;
      auto event_id = cmd_queue.GetNextEventSeqId();
      for (auto &query : ended)
        query->Issue(event_id);
      InvalidateCurrentPass(true);
      EmitOP([event_id](ArgumentEncodingContext &enc) mutable { enc.signalEvent(event_id); });
      promote_flush = true;
      ctx_state.has_dirty_op_since_last_event = false;
    }
  }

  void
  STDMETHODCALLTYPE
  ExecuteCommandList(ID3D11CommandList *pCommandList, BOOL RestoreContextState) override {
    std::lock_guard<d3d11_device_mutex> lock(mutex);

    ResetEncodingContextState();
    promote_flush = false;
    Execute(pCommandList);

    if (RestoreContextState)
      RestoreEncodingContextState();
    else
      ResetD3D11ContextState();
  }

  HRESULT
  STDMETHODCALLTYPE
  FinishCommandList(BOOL RestoreDeferredContextState, ID3D11CommandList **ppCommandList) override {
    std::lock_guard<d3d11_device_mutex> lock(mutex);

    return DXGI_ERROR_INVALID_CALL;
  }

  D3D11_DEVICE_CONTEXT_TYPE
  STDMETHODCALLTYPE
  GetType() override {
    return D3D11_DEVICE_CONTEXT_IMMEDIATE;
  }

  virtual void
  WaitUntilGPUIdle() override {
    uint64_t seq = cmd_queue.CurrentSeqId();
    if(!InvalidateCurrentPass())
      Commit();
    cmd_queue.WaitCPUFence(seq);
  };

  void
  Commit() override {
    promote_flush = false;
    D3D11_ASSERT(cmdbuf_state == CommandBufferState::Idle);
    ctx_state.cmd_queue.CommitCurrentChunk();
    ctx_state.has_dirty_op_since_last_event = false;
  };

  HRESULT STDMETHODCALLTYPE Signal(ID3D11Fence *pFence, UINT64 Value) override {
    return SignalEvent(static_cast<MTLD3D11Fence *>(pFence)->event, Value);
  }

  HRESULT SignalEvent(WMT::SharedEvent Event, UINT64 Value) {
    InvalidateCurrentPass();
    EmitOP([event = WMT::Reference<WMT::SharedEvent>(Event), Value](ArgumentEncodingContext &enc) mutable {
      enc.signalEvent(std::move(event), Value);
    });
    Flush();

    return S_OK;
  }

  HRESULT SignalFence(ID3D12Fence *Fence, UINT64 Value) override {
    std::lock_guard<d3d11_device_mutex> lock(mutex);
    PrepareFlush();
    auto &chunk = *cmd_queue.CurrentChunk();
    chunk.completed = [completed = std::move(chunk.completed), fence = Com<ID3D12Fence>(Fence), Value] {
      if (completed)
        completed();
      fence->Signal(Value);
    };
    Commit();
    return S_OK;
  }

  HRESULT WaitEvent(WMT::SharedEvent Event, UINT64 Value, ID3D12Fence *Fence) override {
    Flush();
    ctx_state.acquired_seq = cmd_queue.CurrentSeqId();
    EmitOP([event = WMT::Reference<WMT::SharedEvent>(Event), Value, fence = Com<ID3D12Fence>(Fence)](
               ArgumentEncodingContext &enc) mutable {
      enc.queue().WaitSharedEvent(event, Value);
    });
    return S_OK;
  }

  HRESULT ReturnResource(ID3D11Resource *Resource, std::function<void(ArgumentEncodingContext &)> &&Before) override {
    std::lock_guard<d3d11_device_mutex> lock(mutex);
    InvalidateCurrentPass();
    auto texture = GetTexture(Resource);
    ctx_state.returned.insert_or_assign(
        texture.ptr(), std::make_pair(Com<D3D11ResourceCommon, false>(GetResourceCommon(Resource)), std::move(Before)));
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Wait(ID3D11Fence *pFence, UINT64 Value) override {
    auto fence = static_cast<MTLD3D11Fence *>(pFence);

    Flush();
    EmitOP([event = fence->event, Value](ArgumentEncodingContext &enc) mutable {
      enc.waitEvent(std::move(event), Value);
    });

    return S_OK;
  }

  UINT
  STDMETHODCALLTYPE
  GetContextFlags() override {
    return 0;
  }

  HRESULT
  AcquireSync(ID3D11Resource *pResource, UINT64 Key, DWORD dwMilliseconds) override {
    std::lock_guard<d3d11_device_mutex> lock(mutex);
    auto resource = GetResourceCommon(pResource);
    if (!resource->keyedMutex())
      return DXGI_ERROR_INVALID_CALL;
    return resource->keyedMutex()->acquire(
        [&](uint64_t fence) {
          InvalidateCurrentPass(true);
          EmitOP([event = resource->keyedMutex()->sharedEvent(), fence](ArgumentEncodingContext &enc) mutable {
            enc.waitEvent(std::move(event), fence);
          });
          Flush();
        },
        Key, dwMilliseconds
    );
  }
  HRESULT
  ReleaseSync(ID3D11Resource *pResource, UINT64 Key) override {
    std::lock_guard<d3d11_device_mutex> lock(mutex);
    auto resource = GetResourceCommon(pResource);
    if (!resource->keyedMutex())
      return DXGI_ERROR_INVALID_CALL;
    return resource->keyedMutex()->release(
        [&](uint64_t fence) {
          InvalidateCurrentPass(true);
          EmitOP([event = resource->keyedMutex()->sharedEvent(), fence](ArgumentEncodingContext &enc) mutable {
            enc.signalEvent(std::move(event), fence);
          });
          Flush();
        },
        Key
    );
  }

private:
  std::vector<Com<MTLD3D11OcclusionQuery>> pending_occlusion_queries;
  CommandQueue &cmd_queue;
  ContextInternalState ctx_state;
  std::atomic<uint32_t> refcount = 0;
  D3D11Multithread d3dmt_;
  D3D11VideoContext video_;
  bool ignore_map_flag_no_wait_;
};

std::unique_ptr<MTLD3D11DeviceContextBase>
InitializeImmediateContext(MTLD3D11Device *pDevice, CommandQueue &cmd_queue) {
  return std::make_unique<MTLD3D11ImmediateContext>(pDevice, cmd_queue);
}

}; // namespace mullion
