// a query a game makes and asks the GPU about: D3DQUERYTYPE_EVENT, OCCLUSION, TIMESTAMP and TIMESTAMPFREQ, as
// D3D11 event, occlusion and timestamp queries (Microsoft Learn, IDirect3DQuery9). vertex-cache queries are refused.
#pragma once
#include "d3d9_common.hpp"

namespace mullion::d3d9 {

class MTLD3D9Query final : private Object9, public IDirect3DQuery9 {
public:
  static HRESULT create(
      IDirect3DDevice9 *device, ID3D11Device *d3d11_device, ID3D11DeviceContext *context, D3DQUERYTYPE type,
      IDirect3DQuery9 **query
  );

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override;
  ULONG STDMETHODCALLTYPE AddRef() override { return Object9::AddRef(); }
  ULONG STDMETHODCALLTYPE Release() override { return Object9::Release(); }

  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **device) override;
  D3DQUERYTYPE STDMETHODCALLTYPE GetType() override { return type_; }
  DWORD STDMETHODCALLTYPE GetDataSize() override;
  HRESULT STDMETHODCALLTYPE Issue(DWORD flags) override;
  HRESULT STDMETHODCALLTYPE GetData(void *data, DWORD size, DWORD flags) override;

private:
  MTLD3D9Query(
      IDirect3DDevice9 *device, ID3D11DeviceContext *context, ID3D11Query *query, ID3D11Query *disjoint,
      D3DQUERYTYPE type
  )
      : device_(device), device_ref_(device), context_(context), query_(query), disjoint_(disjoint), type_(type) {}

  bool needs_begin() const {
    return type_ == D3DQUERYTYPE_OCCLUSION;
  }

  IDirect3DDevice9 *device_ = nullptr;
  DeviceChildRef device_ref_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<ID3D11Query> query_;
  ComPtr<ID3D11Query> disjoint_;
  D3DQUERYTYPE type_ = D3DQUERYTYPE_EVENT;
  bool issued_ = false;
};

} // namespace mullion::d3d9
