#include "d3d9_query.hpp"

namespace mullion::d3d9 {

HRESULT
MTLD3D9Query::create(
    IDirect3DDevice9 *device, ID3D11Device *d3d11_device, ID3D11DeviceContext *context, D3DQUERYTYPE type,
    IDirect3DQuery9 **query
) {
  if (!query)
    return D3DERR_INVALIDCALL;
  *query = nullptr;
  if (type != D3DQUERYTYPE_EVENT) {
    ERR_ONCE("D3D9: only an event query (D3DQUERYTYPE_EVENT) is made yet; an occlusion, timestamp or vertex-cache "
             "query is refused rather than answered with a number nothing measured.");
    return D3DERR_INVALIDCALL;
  }
  if (!device || !d3d11_device || !context)
    return D3DERR_INVALIDCALL;

  D3D11_QUERY_DESC desc{};
  desc.Query = D3D11_QUERY_EVENT;
  ComPtr<ID3D11Query> event_query;
  if (FAILED(d3d11_device->CreateQuery(&desc, &event_query))) {
    ERR_ONCE("D3D9: the D3D11 device made no event query.");
    return D3DERR_INVALIDCALL;
  }

  ComPtr<MTLD3D9Query> result;
  result.Attach(new MTLD3D9Query(device, context, event_query.Get()));
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

// D3D9's Issue: D3DISSUE_END ends the query, and D3DISSUE_BEGIN is only an occlusion query's (an event query ends
// where it is issued, so a begin is refused rather than silently ignored)
HRESULT
MTLD3D9Query::Issue(DWORD flags) {
  if ((flags & ~(DWORD)(D3DISSUE_BEGIN | D3DISSUE_END)) || !(flags & (D3DISSUE_BEGIN | D3DISSUE_END)))
    return D3DERR_INVALIDCALL;
  if (flags & D3DISSUE_BEGIN)
    return D3DERR_INVALIDCALL;
  context_->End(query_.Get());
  issued_ = true;
  return S_OK;
}

// D3D9's GetData: S_OK and a nonzero flag once the GPU has finished what was issued before the query, S_FALSE while it
// has not. D3DGETDATA_FLUSH flushes what is queued before the check, which is the flag a game passes when it is about to
// wait on the answer.
HRESULT
MTLD3D9Query::GetData(void *data, DWORD size, DWORD flags) {
  if (!data || size != GetDataSize() || (flags & ~(DWORD)D3DGETDATA_FLUSH))
    return D3DERR_INVALIDCALL;
  if (!issued_)
    return D3DERR_INVALIDCALL;
  if (flags & D3DGETDATA_FLUSH)
    context_->Flush();

  WINBOOL signalled = FALSE;
  const HRESULT hr = context_->GetData(
      query_.Get(), &signalled, sizeof(signalled),
      flags & D3DGETDATA_FLUSH ? 0u : (UINT)D3D11_ASYNC_GETDATA_DONOTFLUSH
  );
  if (hr != S_OK)
    return S_FALSE;
  // D3D9's event query ends where it was issued: it has to be issued again before it can be waited on again
  issued_ = false;
  *(WINBOOL *)data = signalled ? TRUE : FALSE;
  return S_OK;
}

} // namespace mullion::d3d9
