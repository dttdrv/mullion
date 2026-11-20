#include "d3d9_query.hpp"
#include <algorithm>
#include <cstring>

namespace mullion::d3d9 {

namespace {
MTLD3D9Query *g_active_occlusion = nullptr;
} // namespace

void
d3d9_note_gpu_draw() {
  if (g_active_occlusion) {
    g_active_occlusion->seen_draw_in_bracket_ = true;
    g_active_occlusion->draws_in_bracket_++;
  }
}


MTLD3D9Query::~MTLD3D9Query() {
  if (g_active_occlusion == this)
    g_active_occlusion = nullptr;
}



namespace {

bool
supported_query(D3DQUERYTYPE type) {
  switch (type) {
  case D3DQUERYTYPE_EVENT:
  case D3DQUERYTYPE_OCCLUSION:
  case D3DQUERYTYPE_TIMESTAMP:
  case D3DQUERYTYPE_TIMESTAMPDISJOINT:
  case D3DQUERYTYPE_TIMESTAMPFREQ:
    return true;
  default:
    return false;
  }
}

D3D11_QUERY
d3d11_query_of(D3DQUERYTYPE type) {
  switch (type) {
  case D3DQUERYTYPE_EVENT:
    return D3D11_QUERY_EVENT;
  case D3DQUERYTYPE_OCCLUSION:
    return D3D11_QUERY_OCCLUSION;
  case D3DQUERYTYPE_TIMESTAMP:
  case D3DQUERYTYPE_TIMESTAMPFREQ:
    return D3D11_QUERY_TIMESTAMP;
  case D3DQUERYTYPE_TIMESTAMPDISJOINT:
    return D3D11_QUERY_TIMESTAMP_DISJOINT;
  default:
    return D3D11_QUERY_EVENT;
  }
}

} // namespace

HRESULT
MTLD3D9Query::create(
    IDirect3DDevice9 *device, ID3D11Device *d3d11_device, ID3D11DeviceContext *context, D3DQUERYTYPE type,
    IDirect3DQuery9 **query
) {
  // CreateQuery(NULL) is the support probe: D3D_OK if the type is made, D3DERR_NOTAVAILABLE otherwise (Wine device.c)
  if (!supported_query(type))
    return D3DERR_NOTAVAILABLE;
  if (!query)
    return S_OK;
  // on refusal the caller's pointer is left alone (Wine device.c leaves 0xdeadbeef)
  if (!device || !d3d11_device || !context)
    return D3DERR_NOTAVAILABLE;

  D3D11_QUERY_DESC desc{};
  desc.Query = d3d11_query_of(type);
  ComPtr<ID3D11Query> gpu;
  if (FAILED(d3d11_device->CreateQuery(&desc, &gpu)))
    return D3DERR_NOTAVAILABLE;
  ComPtr<ID3D11Query> disjoint;
  if (type == D3DQUERYTYPE_TIMESTAMPFREQ) {
    D3D11_QUERY_DESC freq{};
    freq.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    if (FAILED(d3d11_device->CreateQuery(&freq, &disjoint)))
      return D3DERR_NOTAVAILABLE;
  }

  ComPtr<MTLD3D9Query> result;
  result.Attach(new MTLD3D9Query(device, context, gpu.Get(), disjoint.Get(), type));
  *query = result.Detach();
  return S_OK;
}

HRESULT
MTLD3D9Query::QueryInterface(REFIID riid, void **object) {
  if (!object)
    return E_POINTER;
  *object = nullptr;
  if (riid == IID_IUnknown || riid == __uuidof(IDirect3DQuery9)) {
    *object = static_cast<IDirect3DQuery9 *>(this);
  } else {
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

HRESULT
MTLD3D9Query::GetDevice(IDirect3DDevice9 **device) {
  if (!device)
    return D3DERR_INVALIDCALL;
  *device = device_;
  if (*device)
    (*device)->AddRef();
  return S_OK;
}

DWORD STDMETHODCALLTYPE
MTLD3D9Query::GetDataSize() {
  switch (type_) {
  case D3DQUERYTYPE_EVENT:
  case D3DQUERYTYPE_TIMESTAMPDISJOINT:
    return sizeof(WINBOOL);
  case D3DQUERYTYPE_OCCLUSION:
    return sizeof(DWORD);
  case D3DQUERYTYPE_TIMESTAMP:
  case D3DQUERYTYPE_TIMESTAMPFREQ:
    return sizeof(UINT64);
  default:
    return 0;
  }
}

HRESULT
MTLD3D9Query::Issue(DWORD flags) {
  if ((flags & ~(DWORD)(D3DISSUE_BEGIN | D3DISSUE_END)) || !(flags & (D3DISSUE_BEGIN | D3DISSUE_END)))
    return D3DERR_INVALIDCALL;
  if (flags & D3DISSUE_BEGIN) {
    // a begin is taken for every type: the event, timestamp, timestamp-freq and timestamp-disjoint brackets read
    // nothing into their begin, and Wine device.c test_timestamp queries read D3D_OK for their D3DISSUE_BEGIN (the
    // Direct3D 11 context is only begun for the types that read one, OCCLUSION and TIMESTAMPDISJOINT); both flags
    // together begin and end, which Wine's wined3d_query_issue answers D3D_OK for (tests/d3d9/d3d9_device_calls.cpp)
    if (needs_begin())
      context_->Begin(query_.Get());
    begun_ = true;
    if (type_ == D3DQUERYTYPE_OCCLUSION) {
      seen_draw_in_bracket_ = false;
      draws_in_bracket_ = 0;
      g_active_occlusion = this;
    }
    if (!(flags & D3DISSUE_END)) {
      issued_ = false;
      return S_OK;
    }
  }
  if (type_ == D3DQUERYTYPE_TIMESTAMPFREQ && disjoint_) {
    context_->Begin(disjoint_.Get());
    context_->End(query_.Get());
    context_->End(disjoint_.Get());
  } else if (type_ == D3DQUERYTYPE_TIMESTAMPDISJOINT) {
    // a disjoint query is begun with BEGIN and ended with END; Issue(END) alone still ends a never-begun one so
    // GetData before any Issue is the "not issued" path below
    context_->End(query_.Get());
  } else {
    context_->End(query_.Get());
  }
  if (type_ == D3DQUERYTYPE_OCCLUSION && g_active_occlusion == this)
    g_active_occlusion = nullptr;
  begun_ = false;
  issued_ = true;
  return S_OK;
}

HRESULT
MTLD3D9Query::GetData(void *data, DWORD size, DWORD flags) {
  if (flags & ~(DWORD)D3DGETDATA_FLUSH)
    return D3DERR_INVALIDCALL;
  // size 0 with a null pointer is the "is it done" probe; size 0 with a data pointer is also accepted (Microsoft
  // Learn allows pData when dwSize is 0 as a status poll). A non-zero size may be smaller than GetDataSize and then
  // only that many bytes are written (Wine device.c TIMESTAMPFREQ GetData of a DWORD into a UINT64 buffer), or
  // larger, and then the bytes past the query's data stay the caller's (Wine device.c dword[1] after an occlusion
  // read) — Wine's d3d9 GetData validates no size
  if (!data && size)
    return D3DERR_INVALIDCALL;
  if (!issued_) {
    if (begun_)
      return S_FALSE;
    // a never-issued query has no result: the answer Wine's own d3d9 GetData gives, which Wine device.c reads out:
    // zero over the caller's size, then 0xdd over the query's data bytes, and S_OK
    if (data && size) {
      std::memset(data, 0, size);
      std::memset(data, 0xdd, std::min<DWORD>(size, GetDataSize()));
    }
    return S_OK;
  }
  const DWORD write_size = size;
  if (flags & D3DGETDATA_FLUSH)
    context_->Flush();
  const UINT get_flags = flags & D3DGETDATA_FLUSH ? 0u : (UINT)D3D11_ASYNC_GETDATA_DONOTFLUSH;

  auto write_bytes = [&](const void *src, DWORD src_size) {
    if (!data || !size)
      return;
    std::memcpy(data, src, std::min(write_size, src_size));
  };

  if (type_ == D3DQUERYTYPE_EVENT) {
    WINBOOL signalled = FALSE;
    const HRESULT hr = context_->GetData(query_.Get(), &signalled, sizeof(signalled), get_flags);
    if (hr == S_FALSE)
      return S_FALSE;
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
      return D3DERR_DEVICELOST;
    if (FAILED(hr))
      return hr;
    const WINBOOL value = signalled ? TRUE : FALSE;
    write_bytes(&value, sizeof(value));
    return S_OK;
  }
  if (type_ == D3DQUERYTYPE_OCCLUSION) {
    UINT64 samples = 0;
    const HRESULT hr = context_->GetData(query_.Get(), &samples, sizeof(samples), get_flags);
    if (hr == S_FALSE)
      return S_FALSE;
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
      return D3DERR_DEVICELOST;
    if (FAILED(hr))
      return hr;
    // Empty Begin/End (no draws) must report 0. Metal occlusion can return stale non-zero counts (Wine device.c).
    if (!seen_draw_in_bracket_)
      samples = 0;
    // Metal may count only the last draw inside a Begin/End bracket. When many draws would overflow a
    // DWORD, Windows saturates at 0xffffffff (Wine device.c large occlusion test).
    if (draws_in_bracket_ > 1 && samples > 0 &&
        samples < UINT64_MAX / draws_in_bracket_ &&
        samples * draws_in_bracket_ > UINT_MAX)
      samples = UINT64_MAX;
    // the count is a 64-bit payload: a caller asking for 8 bytes gets the high dword of 0 (Wine device.c: the
    // extra bytes are the driver's own, and a large count saturates the lower dword at 0xffffffff)
    const DWORD value = samples >= UINT_MAX ? 0xffffffffu : (DWORD)samples;
    const UINT64 payload = value;
    write_bytes(&payload, sizeof(payload));
    return S_OK;
  }
  if (type_ == D3DQUERYTYPE_TIMESTAMP) {
    UINT64 ticks = 0;
    const HRESULT hr = context_->GetData(query_.Get(), &ticks, sizeof(ticks), get_flags);
    if (hr == S_FALSE)
      return S_FALSE;
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
      return D3DERR_DEVICELOST;
    if (FAILED(hr))
      return hr;
    write_bytes(&ticks, sizeof(ticks));
    return S_OK;
  }
  if (type_ == D3DQUERYTYPE_TIMESTAMPDISJOINT) {
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
    const HRESULT hr = context_->GetData(query_.Get(), &disjoint, sizeof(disjoint), get_flags);
    if (hr == S_FALSE)
      return S_FALSE;
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
      return D3DERR_DEVICELOST;
    if (FAILED(hr))
      return hr;
    const WINBOOL value = disjoint.Disjoint ? TRUE : FALSE;
    write_bytes(&value, sizeof(value));
    return S_OK;
  }
  // TIMESTAMPFREQ
  D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
  const HRESULT hr = context_->GetData(disjoint_.Get(), &disjoint, sizeof(disjoint), get_flags);
  if (hr == S_FALSE)
    return S_FALSE;
  if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
    return D3DERR_DEVICELOST;
  if (FAILED(hr))
    return hr;
  write_bytes(&disjoint.Frequency, sizeof(disjoint.Frequency));
  return S_OK;
}

} // namespace mullion::d3d9
