#include "d3d11_query.hpp"
#include "dxmt_occlusion_query.hpp"

namespace dxmt {

class OcclusionQuery : public MTLD3DQueryBase<MTLD3D11OcclusionQuery> {
  using MTLD3DQueryBase<MTLD3D11OcclusionQuery>::MTLD3DQueryBase;

  Rc<VisibilityResultQuery> query_ = new VisibilityResultQuery();
  uint64_t accumulated_value_ = 0;

  virtual UINT STDMETHODCALLTYPE
  GetDataSize() override {
    return desc_.Query == D3D11_QUERY_OCCLUSION_PREDICATE ? sizeof(BOOL) : sizeof(UINT64);
  };

  virtual HRESULT
  GetData(void *data) override {
    if (state_ == QueryState::Signaled || query_->getValue(&accumulated_value_)) {
      if (desc_.Query == D3D11_QUERY_OCCLUSION_PREDICATE) {
        *((BOOL *)data) = accumulated_value_ != 0;
      } else {
        *((uint64_t *)data) = accumulated_value_;
      }
      query_ = new VisibilityResultQuery();
      state_ = QueryState::Signaled;
      return S_OK;
    }
    return S_FALSE;
  }

  virtual VisibilityResultQuery *
  Begin() override {
    if (state_ == QueryState::Issued)
      query_ = new VisibilityResultQuery();
    state_ = QueryState::Building;
    return query_.ptr();
  };

  virtual VisibilityResultQuery *
  End() override {
    state_ = QueryState::Issued;
    return query_.ptr();
  };

  virtual void DoDeferredQuery(VisibilityResultQuery *deferred_query) override{
    accumulated_value_ = 0;
    state_ = QueryState::Issued;
    query_ = deferred_query;
  };
};

HRESULT
CreateOcclusionQuery(MTLD3D11Device *pDevice, const D3D11_QUERY_DESC1 *pDesc, ID3D11Query1 **ppQuery) {
  if (ppQuery) {
    *ppQuery = ref(new OcclusionQuery(pDevice, pDesc));
    return S_OK;
  }
  return S_FALSE;
}

class MTLD3D11TimestampQueryImpl : public MTLD3DQueryBase<MTLD3D11TimestampQuery> {
  using MTLD3DQueryBase<MTLD3D11TimestampQuery>::MTLD3DQueryBase;

  Rc<TimestampQuery> query_ = new TimestampQuery();
  uint64_t latest_value_ = 0;

  virtual UINT STDMETHODCALLTYPE
  GetDataSize() override {
    return sizeof(UINT64);
  };

  virtual HRESULT
  GetData(void *data) override {
    if (state_ == QueryState::Signaled || query_->getValue(&latest_value_)) {
      *((uint64_t *)data) = latest_value_;
      query_ = new TimestampQuery();
      state_ = QueryState::Signaled;
      return S_OK;
    }
    return S_FALSE;
  }

  virtual TimestampQuery *
  End() override {
    if (state_ == QueryState::Issued) {
      // discard previous query
      query_ = new TimestampQuery();
    }
    state_ = QueryState::Issued;
    return query_.ptr();
  };
};

HRESULT
CreateTimestampQuery(MTLD3D11Device *pDevice, const D3D11_QUERY_DESC1 *pDesc, ID3D11Query1 **ppQuery) {
  if (ppQuery) {
    *ppQuery = ref(new MTLD3D11TimestampQueryImpl(pDevice, pDesc));
    return S_OK;
  }
  return S_FALSE;
}

class StreamOutputQuery : public MTLD3DQueryBase<MTLD3D11StreamOutputQuery> {
  static constexpr size_t kStreams = D3D11_SO_STREAM_COUNT;

  Rc<Buffer> snapshots_;
  uint64_t event_ = ~0ull;

  // after SO_STATISTICS and SO_OVERFLOW_PREDICATE (stream 0, and all streams), the enumeration goes on with the two
  // of each stream in turn
  bool
  predicate() const {
    return (desc_.Query - D3D11_QUERY_SO_STATISTICS) & 1;
  }
  uint32_t
  first_stream() const {
    return desc_.Query < D3D11_QUERY_SO_STATISTICS_STREAM0 ? 0 : (desc_.Query - D3D11_QUERY_SO_STATISTICS_STREAM0) / 2;
  }
  uint32_t
  streams() const {
    return desc_.Query == D3D11_QUERY_SO_OVERFLOW_PREDICATE ? kStreams : 1;
  }

public:
  StreamOutputQuery(MTLD3D11Device *pDevice, const D3D11_QUERY_DESC1 *pDesc) : MTLD3DQueryBase(pDevice, pDesc) {
    snapshots_ = new Buffer(2 * kStreams * sizeof(D3D11_QUERY_DATA_SO_STATISTICS), pDevice->GetMTLDevice());
    // the CPU reads them: memory a 32-bit process can address
    Flags<BufferAllocationFlag> flags;
#ifdef __i386__
    flags.set(BufferAllocationFlag::CpuPlaced);
#endif
    snapshots_->rename(snapshots_->allocate(flags));
  }

  UINT STDMETHODCALLTYPE
  GetDataSize() override {
    return predicate() ? sizeof(BOOL) : sizeof(D3D11_QUERY_DATA_SO_STATISTICS);
  }

  const Rc<Buffer> &
  Snapshots() override {
    return snapshots_;
  }

  void
  Issue(uint64_t event) override {
    event_ = event;
    state_ = QueryState::Issued;
  }

  HRESULT
  GetData(void *data, uint64_t signaled_event) override {
    if (signaled_event < event_)
      return S_FALSE;
    auto began = static_cast<const D3D11_QUERY_DATA_SO_STATISTICS *>(snapshots_->current()->mappedMemory(0));
    auto ended = began + kStreams;
    D3D11_QUERY_DATA_SO_STATISTICS total{};
    bool overflowed = false;
    for (uint32_t stream = first_stream(); stream < first_stream() + streams(); stream++) {
      auto written = ended[stream].NumPrimitivesWritten - began[stream].NumPrimitivesWritten;
      auto needed = ended[stream].PrimitivesStorageNeeded - began[stream].PrimitivesStorageNeeded;
      total.NumPrimitivesWritten += written;
      total.PrimitivesStorageNeeded += needed;
      overflowed |= written != needed;
    }
    if (predicate())
      *static_cast<BOOL *>(data) = overflowed;
    else
      *static_cast<D3D11_QUERY_DATA_SO_STATISTICS *>(data) = total;
    return S_OK;
  }
};

HRESULT
CreateStreamOutputQuery(MTLD3D11Device *pDevice, const D3D11_QUERY_DESC1 *pDesc, ID3D11Query1 **ppQuery) {
  if (ppQuery) {
    *ppQuery = ref(new StreamOutputQuery(pDevice, pDesc));
    return S_OK;
  }
  return S_FALSE;
}

} // namespace dxmt