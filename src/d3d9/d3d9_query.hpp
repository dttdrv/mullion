// a query a game makes and asks the GPU about: D3DQUERYTYPE_EVENT, which says everything issued before its
// Issue(D3DISSUE_END) has finished. it is a D3D11 event query, whose GetData answers S_OK once the GPU has reached the
// fence the query's End put in the command stream, which is exactly what D3D9's event query asks
// (Microsoft Learn, IDirect3DQuery9::GetData: D3DGETDATA_FLUSH flushes the device before the check, and S_FALSE says the
// data is not ready yet). the query types that need a pass of their own (occlusion, timestamps, vertex cache) are not
// made: CreateQuery refuses them with the reason the log carries (src/d3d9/README).
#pragma once
#include "d3d9_common.hpp"

namespace mullion::d3d9 {

class MTLD3D9Query final : private Object9, public IDirect3DQuery9 {
public:
  // the D3D11 query an event query is: made here, so a device that refuses it refuses CreateQuery
  static HRESULT create(
      IDirect3DDevice9 *device, ID3D11Device *d3d11_device, ID3D11DeviceContext *context, D3DQUERYTYPE type,
      IDirect3DQuery9 **query
  );

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override;
  ULONG STDMETHODCALLTYPE AddRef() override { return Object9::AddRef(); }
  ULONG STDMETHODCALLTYPE Release() override { return Object9::Release(); }

  // IDirect3DQuery9
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **device) override;
  D3DQUERYTYPE STDMETHODCALLTYPE GetType() override { return D3DQUERYTYPE_EVENT; }
  DWORD STDMETHODCALLTYPE GetDataSize() override { return sizeof(WINBOOL); }
  HRESULT STDMETHODCALLTYPE Issue(DWORD flags) override;
  HRESULT STDMETHODCALLTYPE GetData(void *data, DWORD size, DWORD flags) override;

private:
  MTLD3D9Query(IDirect3DDevice9 *device, ID3D11DeviceContext *context, ID3D11Query *query)
      : device_(device), device_ref_(device), context_(context), query_(query) {}

  // the device that made the query is held through a private reference (DeviceChildRef), so a query kept after the
  // application released its device still has one to answer GetDevice with
  IDirect3DDevice9 *device_ = nullptr;
  DeviceChildRef device_ref_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<ID3D11Query> query_;
  // whether Issue(D3DISSUE_END) has put the query's fence in the stream, which GetData needs: an unissued event query
  // has nothing to wait for
  bool issued_ = false;
};

} // namespace mullion::d3d9
