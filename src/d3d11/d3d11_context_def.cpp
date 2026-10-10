#include "d3d11_device.hpp"
#include "d3d11_private.h"
#include "d3d11_context_impl.cpp"
#include "dxmt_buffer.hpp"
#include "dxmt_command_queue.hpp"
#include "dxmt_dynamic.hpp"
#include <unordered_set>

namespace dxmt {

struct DynamicBufferAllocation {
  BufferAllocation * allocation;
  uint32_t allocation_id;
  uint32_t suballocation;
};

struct DeferredContextInternalState {
  using device_mutex_t = null_mutex;
  CommandQueue &cmd_queue;
  Com<MTLD3D11CommandList> current_cmdlist;
  std::unordered_map<DynamicBuffer *, DynamicBufferAllocation> current_dynamic_buffer_allocations;
  std::unordered_map<DynamicLinearTexture *, std::pair<TextureAllocation *, uint32_t>> current_dynamic_texture_allocations;
  std::unordered_map<void *, std::pair<Com<MTLD3D11OcclusionQuery>, uint32_t>> building_visibility_queries;
  std::unordered_set<uint32_t> pending_visibility_queries;
};

template<typename Object> Rc<Object> forward_rc(Rc<Object>& obj) {
  return obj;
}

using DeferredContextBase = MTLD3D11DeviceContextImplBase<DeferredContextInternalState>;

template <>
template <CommandWithContext<ArgumentEncodingContext> cmd>
void
DeferredContextBase::EmitST(cmd &&fn) {
  ctx_state.current_cmdlist->Emit(std::forward<cmd>(fn));
}

template <>
template <CommandWithContext<ArgumentEncodingContext> cmd>
void
DeferredContextBase::EmitOP(cmd &&fn) {
  if (!within_predicated_operation_)
    return ctx_state.current_cmdlist->Emit(std::forward<cmd>(fn));
  ctx_state.current_cmdlist->Emit([fn = std::forward<cmd>(fn)](ArgumentEncodingContext &enc) mutable {
    if (!enc.skips_predicated)
      fn(enc);
  });
}

template <>
template <typename T>
moveonly_list<T>
DeferredContextBase::AllocateCommandData(size_t n) {
  return ctx_state.current_cmdlist->AllocateCommandData<T>(n);
}

template <>
std::tuple<WMT::Buffer, uint64_t>
DeferredContextBase::AllocateStagingBuffer(size_t size, size_t alignment) {
  return ctx_state.current_cmdlist->AllocateStagingBuffer(size, alignment);
}

template <>
void DeferredContextBase::UseCopyDestination(Rc<StagingResource> &staging) {
  ctx_state.current_cmdlist->written_staging_resources.push_back(staging);
}

template <>
void DeferredContextBase::UseCopySource(Rc<StagingResource> &staging) {
  ctx_state.current_cmdlist->read_staging_resources.push_back(staging);
}

template <>
std::pair<BufferAllocation *, uint32_t>
DeferredContextBase::GetDynamicBufferAllocation(Rc<DynamicBuffer> &dynamic) {
  D3D11_ASSERT(
      ctx_state.current_dynamic_buffer_allocations.contains(dynamic.ptr()) &&
      "GetDynamicBufferAllocation() failed on deferred context");
  auto &ret = ctx_state.current_dynamic_buffer_allocations.at(dynamic.ptr());
  return {ret.allocation, ret.suballocation};
}

template <>
void
DeferredContextBase::VisibilityQueryBoundary(bool counting) {
  if (dirty_state.any(DirtyState::VisibilityQuery))
    return;
  dirty_state.set(DirtyState::VisibilityQuery);
  bool began = !ctx_state.pending_visibility_queries.empty();
  for (auto query_id : ctx_state.pending_visibility_queries)
    ctx_state.current_cmdlist->issued_visibility_query[query_id].second = true;
  ctx_state.pending_visibility_queries.clear();
  // an enclosing query can count before a deferred Begin is confirmed by a draw (D3D11.3 6.3.6)
  if (counting && began && ++visibility_result_segments_ == VisibilityResultOffsetBumpState::kVisibilitySegments - 1)
    InvalidateCurrentPass(true);
}

class MTLD3D11DeferredContext : public DeferredContextBase {
public:
  MTLD3D11DeferredContext(MTLD3D11Device *pDevice, UINT ContextFlags) :
      DeferredContextBase(pDevice, ctx_state, mutex),
      ctx_state({pDevice->GetDXMTDevice().queue(), {}}),
      context_flag(ContextFlags) {
    device->CreateCommandList((ID3D11CommandList **)&ctx_state.current_cmdlist);
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
    if (unlikely(!refCount)) {
      // what the context still binds is the device's, so the device goes last
      auto parent = this->m_parent;
      delete this;
      parent->Release();
    }

    return refCount;
  }

  void *
  MapDynamicBuffer(Rc<DynamicBuffer> &dynamic) {
    auto ret = ctx_state.current_dynamic_buffer_allocations.find(dynamic.ptr());
    if (ret != ctx_state.current_dynamic_buffer_allocations.end()) {
      auto &allocation_state = ret->second;
      auto allocation = allocation_state.allocation;
      auto allocation_id = allocation_state.allocation_id;
      auto &suballocation = allocation_state.suballocation;
      if (allocation->hasSuballocatoin(suballocation + 1)) {
        suballocation += 1;
        ctx_state.current_cmdlist->used_dynamic_buffers[allocation_id].suballocation = suballocation;
        EmitST([allocation, sub = suballocation](ArgumentEncodingContext &enc) mutable {
          allocation->useSuballocation(sub);
        });
        return allocation->mappedMemory(suballocation);
      }
    }
    Rc<BufferAllocation> new_allocation = dynamic->allocate(ctx_state.cmd_queue.CoherentSeqId());
    uint32_t id = ctx_state.current_cmdlist->used_dynamic_buffers.size();
    // track the current allocation in case of a following NO_OVERWRITE map
    if (ret == ctx_state.current_dynamic_buffer_allocations.end()) {
      ctx_state.current_dynamic_buffer_allocations.insert(ret, {dynamic.ptr(), {new_allocation.ptr(), id, 0}});
    } else {
      auto previous_allocation_id = ret->second.allocation_id;
      ctx_state.current_cmdlist->used_dynamic_buffers[previous_allocation_id].latest = false;
      ret->second = {new_allocation.ptr(), id, 0};
    }
    // collect allocated buffers and recycle them when the command list is released
    ctx_state.current_cmdlist->used_dynamic_buffers.push_back(
        used_dynamic_buffer{dynamic.ptr(), new_allocation, 0, true}
    );
    EmitST([allocation = new_allocation, buffer = Rc(dynamic->buffer)](ArgumentEncodingContext &enc) mutable {
      allocation->useSuballocation(0);
      auto _ = buffer->rename(forward_rc(allocation));
    });
    return new_allocation->mappedMemory(0);
  }

  HRESULT
  MapResource(ID3D11Resource *pResource, UINT Subresource, D3D11_MAP MapType, UINT MapFlags,
              D3D11_MAPPED_SUBRESOURCE *pMappedResource) override {
    UINT buffer_length = 0, &row_pitch = buffer_length;
    UINT bind_flag = 0, &depth_pitch = bind_flag;
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

        pMappedResource->pData = MapDynamicBuffer(dynamic);
        pMappedResource->RowPitch = buffer_length;
        pMappedResource->DepthPitch = buffer_length;
      } else {
        auto ret = ctx_state.current_dynamic_buffer_allocations.find(dynamic.ptr());
        if (ret == ctx_state.current_dynamic_buffer_allocations.end()) {
          ERR("DeferredContext: Invalid NO_OVERWRITE map on deferred context occurs without any prior DISCARD map.");
          return D3D11_ERROR_DEFERRED_CONTEXT_MAP_WITHOUT_INITIAL_DISCARD;
        }
        auto &allocation_state = ret->second;
        pMappedResource->pData = allocation_state.allocation->mappedMemory(allocation_state.suballocation);
        pMappedResource->RowPitch = buffer_length;
        pMappedResource->DepthPitch = buffer_length;
      }
      return S_OK;
    }
    if (auto dynamic = GetDynamicLinearTexture(pResource, &row_pitch, &depth_pitch)) {
      for (auto &stage : state_.ShaderStages) {
        stage.SRVs.set_dirty();
      }
      Rc<TextureAllocation> new_allocation = dynamic->allocate(ctx_state.cmd_queue.CoherentSeqId());
      uint32_t id = ctx_state.current_cmdlist->used_dynamic_lineartextures.size();
      auto ret = ctx_state.current_dynamic_texture_allocations.find(dynamic.ptr());
      if (ret == ctx_state.current_dynamic_texture_allocations.end()) {
        ctx_state.current_dynamic_texture_allocations.insert(
            ret, {dynamic.ptr(), {new_allocation.ptr(), id}});
      } else {
        auto previous_allocation_id = ret->second.second;
        ctx_state.current_cmdlist->used_dynamic_lineartextures[previous_allocation_id].latest = false;
        ret->second = {new_allocation.ptr(), id};
      }
      // collect allocated buffers and recycle them when the command list is released
      ctx_state.current_cmdlist->used_dynamic_lineartextures.push_back({dynamic, new_allocation, false});
      EmitST([allocation = new_allocation, texture = Rc(dynamic->texture)](ArgumentEncodingContext &enc) mutable {
        auto _ = texture->rename(forward_rc(allocation));
      });

      pMappedResource->pData = new_allocation->mappedMemory;
      pMappedResource->RowPitch = row_pitch;
      pMappedResource->DepthPitch = depth_pitch;
      return S_OK;
    }
    if (auto dynamic = GetDynamicTexture(pResource, Subresource, &row_pitch, &depth_pitch)) {
      for (auto &stage : state_.ShaderStages) {
        stage.SRVs.set_dirty();
      }
      pMappedResource->pData = MapDynamicBuffer(dynamic);
      pMappedResource->RowPitch = row_pitch;
      pMappedResource->DepthPitch = depth_pitch;
      return S_OK;
    }
    return E_INVALIDARG;
  }

  void
  STDMETHODCALLTYPE
  Unmap(ID3D11Resource *pResource, UINT Subresource) override {
    UINT buffer_length = 0, &row_pitch = buffer_length;
    UINT bind_flag = 0, &depth_pitch = bind_flag;
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

    D3D11_QUERY_DESC desc;
    ((ID3D11Query *)pAsync)->GetDesc(&desc);
    switch (desc.Query) {
    case D3D11_QUERY_TIMESTAMP:
    case D3D11_QUERY_EVENT:
      break;
    case D3D11_QUERY_OCCLUSION:
    case D3D11_QUERY_OCCLUSION_PREDICATE: {
      auto building_query = ctx_state.building_visibility_queries.find(pAsync);
      if (building_query != ctx_state.building_visibility_queries.end()) {
        D3D11_ASSERT(0 && "unexpected branch condition hit, please file an issue.");
        End(pAsync);
      }
      VisibilityQueryBoundary(true);
      auto query_id = ctx_state.current_cmdlist->issued_visibility_query.size();
      ctx_state.current_cmdlist->issued_visibility_query.emplace_back(
          static_cast<MTLD3D11OcclusionQuery *>(pAsync), false
      );
      ctx_state.pending_visibility_queries.insert(query_id);
      EmitST([list = ctx_state.current_cmdlist.ptr(), query_id](ArgumentEncodingContext &enc) {
        if (list->issued_visibility_query[query_id].second)
          enc.beginVisibilityResultQuery(enc.currentDeferredVisibilityQuery(query_id));
      });
      ctx_state.building_visibility_queries.insert(
          {(void *)pAsync, {static_cast<MTLD3D11OcclusionQuery *>(pAsync), query_id}}
      );
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
      SnapshotStreamOutputStatistics(static_cast<MTLD3D11StreamOutputQuery *>(pAsync)->Snapshots(), 0);
      break;
    }
    case D3D11_QUERY_TIMESTAMP_DISJOINT:
    case D3D11_QUERY_PIPELINE_STATISTICS: {
      // ignore
      break;
    }
    default:
      ERR("DeferredContext: unhandled query type ", desc.Query);
      break;
    }
  }

  void
  STDMETHODCALLTYPE
  End(ID3D11Asynchronous *pAsync) override {
    if (unlikely(!pAsync))
      return;

    D3D11_QUERY_DESC desc;
    ((ID3D11Query *)pAsync)->GetDesc(&desc);
    switch (desc.Query) {
    case D3D11_QUERY_EVENT:
    case D3D11_QUERY_TIMESTAMP_DISJOINT:
      promote_flush = true;
      ctx_state.current_cmdlist->issued_event_query.push_back(static_cast<MTLD3D11EventQuery *>(pAsync));
      break;
    case D3D11_QUERY_TIMESTAMP: {
      promote_flush = true;
      ctx_state.current_cmdlist->issued_timestamp_query.push_back(static_cast<MTLD3D11TimestampQuery *>(pAsync));
      break;
    }
    case D3D11_QUERY_OCCLUSION:
    case D3D11_QUERY_OCCLUSION_PREDICATE: {
      auto building_query = ctx_state.building_visibility_queries.find(pAsync);
      if (building_query == ctx_state.building_visibility_queries.end()) {
        // need to figure out if it's the intended behavior
        D3D11_ASSERT(0 && "unexpected branch condition hit, please file an issue.");
        // no corresponding Begin()
        WARN("DeferredContext: An occclusion query End() is called without corresponding Begin()");
        return;
      }
      promote_flush = true;
      auto query_id = building_query->second.second;
      bool counted = !dirty_state.any(DirtyState::VisibilityQuery);
      VisibilityQueryBoundary(true);
      if (counted && ctx_state.current_cmdlist->issued_visibility_query[query_id].second &&
          ++visibility_result_segments_ == VisibilityResultOffsetBumpState::kVisibilitySegments - 1)
        InvalidateCurrentPass(true);
      EmitST([list = ctx_state.current_cmdlist.ptr(), query_id](ArgumentEncodingContext &enc) {
        auto query = enc.currentDeferredVisibilityQuery(query_id);
        if (list->issued_visibility_query[query_id].second) {
          enc.endVisibilityResultQuery(std::move(query));
        } else {
          query->begin(enc.currentSeqId(), 0);
          query->end(enc.currentSeqId(), 0);
        }
      });
      ctx_state.pending_visibility_queries.erase(query_id);
      ctx_state.building_visibility_queries.erase(building_query);
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
      // the copy is done once the list has run: the immediate context issues the query then
      auto query = static_cast<MTLD3D11StreamOutputQuery *>(pAsync);
      SnapshotStreamOutputStatistics(query->Snapshots(), 1);
      ctx_state.current_cmdlist->regions.back().ended_stream_output_queries.push_back(query);
      promote_flush = true;
      break;
    }
    case D3D11_QUERY_PIPELINE_STATISTICS: {
      // ignore
      break;
    }
    default:
      ERR("DeferredContext: unhandled query type ", desc.Query);
      break;
    }
  }

  HRESULT
  STDMETHODCALLTYPE
  GetData(ID3D11Asynchronous *pAsync, void *pData, UINT DataSize, UINT GetDataFlags) override {
    return E_FAIL;
  }

  void
  STDMETHODCALLTYPE
  Flush() override {
    // nop
  }

  void
  STDMETHODCALLTYPE
  Flush1(D3D11_CONTEXT_TYPE Type, HANDLE hEvent) override {
    // nop
  }

  void
  STDMETHODCALLTYPE
  ExecuteCommandList(ID3D11CommandList *pCommandList, BOOL RestoreContextState) override{
    for (auto query_id : ctx_state.pending_visibility_queries)
      ctx_state.current_cmdlist->issued_visibility_query[query_id].second = true;
    ctx_state.pending_visibility_queries.clear();
    // the list is a region of the one being recorded, and runs where it is when that one does; it starts from the
    // default state and leaves it, as a list does on the immediate context
    ResetEncodingContextState();
    auto &regions = ctx_state.current_cmdlist->regions;
    regions.emplace_back().nested = pCommandList;
    regions.emplace_back();
    if (RestoreContextState)
      RestoreEncodingContextState();
    else
      ResetD3D11ContextState();
  }

  HRESULT 
  STDMETHODCALLTYPE
  FinishCommandList(BOOL RestoreDeferredContextState, ID3D11CommandList **ppCommandList) override {
    ResetEncodingContextState();

    ctx_state.current_cmdlist->promote_flush = promote_flush;
    promote_flush = false;

    for (const auto &[_, building_query] : ctx_state.building_visibility_queries) {
      End(building_query.first.ptr());
    }
    ctx_state.building_visibility_queries.clear();
    ctx_state.current_dynamic_buffer_allocations.clear();
    ctx_state.current_dynamic_texture_allocations.clear();

    *ppCommandList = std::move(ctx_state.current_cmdlist);
    device->CreateCommandList((ID3D11CommandList **)&ctx_state.current_cmdlist);

    if (RestoreDeferredContextState)
      RestoreEncodingContextState();
    else
      ResetD3D11ContextState();

    return S_OK;
  }

  D3D11_DEVICE_CONTEXT_TYPE 
  STDMETHODCALLTYPE
  GetType() override {
    return D3D11_DEVICE_CONTEXT_DEFERRED;
  }

  // the operation goes into the list's region of the predication that is set
  bool
  SkipsPredicated() override {
    auto &regions = ctx_state.current_cmdlist->regions;
    if (regions.back().predicate != state_.predicate || regions.back().value != state_.predicate_value) {
      regions.emplace_back();
      regions.back().predicate = state_.predicate;
      regions.back().value = state_.predicate_value;
    }
    return false;
  }

  virtual void WaitUntilGPUIdle() override{
      // nop
  };

  virtual void PrepareFlush() override{
      // nop
  };

  void Commit() override{
      // nop
  };

  HRESULT STDMETHODCALLTYPE
  Signal(ID3D11Fence *pFence, UINT64 Value) override {
    return DXGI_ERROR_INVALID_CALL;
  }

  HRESULT STDMETHODCALLTYPE
  Wait(ID3D11Fence *pFence, UINT64 Value) override {
    return DXGI_ERROR_INVALID_CALL;
  }

  UINT
  STDMETHODCALLTYPE
  GetContextFlags() override {
    return context_flag;
  }

  HRESULT
  AcquireSync(ID3D11Resource *pResource, UINT64 Key, DWORD dwMilliseconds) override {
    return DXGI_ERROR_INVALID_CALL;
  }
  HRESULT
  ReleaseSync(ID3D11Resource *pResource, UINT64 Key) override {
    return DXGI_ERROR_INVALID_CALL;
  }

private:
  DeferredContextInternalState ctx_state;
  null_mutex mutex;
  std::atomic<uint32_t> refcount = 0;
  UINT context_flag;
};

Com<MTLD3D11DeviceContextBase>
CreateDeferredContext(MTLD3D11Device *pDevice, UINT ContextFlags) {
  return new MTLD3D11DeferredContext(pDevice, ContextFlags);
}

class MTLD3D11CommandListPool : public MTLD3D11CommandListPoolBase {
public:
  MTLD3D11CommandListPool(MTLD3D11Device *device) : device(device){};

  ~MTLD3D11CommandListPool(){

  };

  void
  RecycleCommandList(ID3D11CommandList *cmdlist) override {
    std::unique_lock<dxmt::mutex> lock(mutex);
    free_commandlist.push_back((MTLD3D11CommandList *)cmdlist);
  };

  void
  CreateCommandList(ID3D11CommandList **ppCommandList) override {
    std::unique_lock<dxmt::mutex> lock(mutex);
    if (!free_commandlist.empty()) {
      free_commandlist.back()->QueryInterface(IID_PPV_ARGS(ppCommandList));
      free_commandlist.pop_back();
      return;
    }
    auto instance = std::make_unique<MTLD3D11CommandList>(device, this, 0);
    *ppCommandList = ref(instance.get());
    instances.push_back(std::move(instance));
    return;
  };

private:
  std::vector<MTLD3D11CommandList *> free_commandlist;
  std::vector<std::unique_ptr<MTLD3D11CommandList>> instances;
  MTLD3D11Device *device;
  dxmt::mutex mutex;
};

std::unique_ptr<MTLD3D11CommandListPoolBase>
InitializeCommandListPool(MTLD3D11Device *device) {
  return std::make_unique<MTLD3D11CommandListPool>(device);
}

}; // namespace dxmt