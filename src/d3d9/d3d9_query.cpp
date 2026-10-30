#include "d3d9_query.hpp"
#include <algorithm>

namespace mullion::d3d9 {

namespace {

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
  if (!query)
    return D3DERR_INVALIDCALL;
  *query = nullptr;
  if (type != D3DQUERYTYPE_EVENT && type != D3DQUERYTYPE_OCCLUSION && type != D3DQUERYTYPE_TIMESTAMP &&
      type != D3DQUERYTYPE_TIMESTAMPFREQ) {
    ERR_ONCE("D3D9: only event, occlusion, timestamp and timestamp-frequency queries are made; a vertex-cache query "
             "is refused rather than answered with a number nothing measured.");
    return D3DERR_INVALIDCALL;
  }
  if (!device || !d3d11_device || !context)
    return D3DERR_INVALIDCALL;

  D3D11_QUERY_DESC desc{};
  desc.Query = d3d11_query_of(type);
  ComPtr<ID3D11Query> gpu;
  if (FAILED(d3d11_device->CreateQuery(&desc, &gpu))) {
    ERR_ONCE("D3D9: the D3D11 device made no query of type ", (unsigned)type, ".");
    return D3DERR_INVALIDCALL;
  }
  ComPtr<ID3D11Query> disjoint;
  if (type == D3DQUERYTYPE_TIMESTAMPFREQ) {
    D3D11_QUERY_DESC freq{};
    freq.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    if (FAILED(d3d11_device->CreateQuery(&freq, &disjoint))) {
      ERR_ONCE("D3D9: the D3D11 device made no timestamp-disjoint query for TIMESTAMPFREQ.");
      return D3DERR_INVALIDCALL;
    }
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
  if ((flags & D3DISSUE_BEGIN) && !needs_begin())
    return D3DERR_INVALIDCALL;
  if (flags & D3DISSUE_BEGIN) {
    context_->Begin(query_.Get());
    issued_ = false;
    return S_OK;
  }
  if (type_ == D3DQUERYTYPE_TIMESTAMPFREQ && disjoint_) {
    context_->Begin(disjoint_.Get());
    context_->End(query_.Get());
    context_->End(disjoint_.Get());
  } else {
    context_->End(query_.Get());
  }
  issued_ = true;
  return S_OK;
}

HRESULT
MTLD3D9Query::GetData(void *data, DWORD size, DWORD flags) {
  if ((!data && size) || (size && size != GetDataSize()) || (flags & ~(DWORD)D3DGETDATA_FLUSH))
    return D3DERR_INVALIDCALL;
  if (!issued_)
    return D3DERR_INVALIDCALL;
  if (flags & D3DGETDATA_FLUSH)
    context_->Flush();
  const UINT get_flags = flags & D3DGETDATA_FLUSH ? 0u : (UINT)D3D11_ASYNC_GETDATA_DONOTFLUSH;

  if (type_ == D3DQUERYTYPE_EVENT) {
    WINBOOL signalled = FALSE;
    const HRESULT hr = context_->GetData(query_.Get(), &signalled, sizeof(signalled), get_flags);
    if (hr == S_FALSE)
      return S_FALSE;
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
      return D3DERR_DEVICELOST;
    if (FAILED(hr))
      return hr;
    if (data && size)
      *(WINBOOL *)data = signalled ? TRUE : FALSE;
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
    if (data && size)
      *(DWORD *)data = (DWORD)std::min<UINT64>(samples, UINT_MAX);
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
    if (data && size)
      *(UINT64 *)data = ticks;
    return S_OK;
  }
  D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
  const HRESULT hr = context_->GetData(disjoint_.Get(), &disjoint, sizeof(disjoint), get_flags);
  if (hr == S_FALSE)
    return S_FALSE;
  if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
    return D3DERR_DEVICELOST;
  if (FAILED(hr))
    return hr;
  if (data && size)
    *(UINT64 *)data = disjoint.Frequency;
  return S_OK;
}

} // namespace mullion::d3d9
