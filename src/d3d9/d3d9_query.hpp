// a query a game makes and asks the GPU about: D3DQUERYTYPE_EVENT, OCCLUSION, TIMESTAMP and TIMESTAMPFREQ, as
// D3D11 event, occlusion and timestamp queries (Microsoft Learn, IDirect3DQuery9). vertex-cache queries are refused.
#pragma once
#include "d3d9_common.hpp"
#include <cstdint>

namespace mullion::d3d9 {

void d3d9_note_gpu_draw();

class MTLD3D9Query final : private Object9, public IDirect3DQuery9 {
  friend void d3d9_note_gpu_draw();
public:
  ~MTLD3D9Query() override;
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
      : device_(device), device_ref_(device, true), context_(context), query_(query), disjoint_(disjoint), type_(type) {}

  bool needs_begin() const {
    return type_ == D3DQUERYTYPE_OCCLUSION || type_ == D3DQUERYTYPE_TIMESTAMPDISJOINT;
  }

  IDirect3DDevice9 *device_ = nullptr;
  DeviceChildRef device_ref_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<ID3D11Query> query_;
  ComPtr<ID3D11Query> disjoint_;
  D3DQUERYTYPE type_ = D3DQUERYTYPE_EVENT;
  // QUERY_CREATED → never Issue; QUERY_BUILDING → BEGIN without END; QUERY_SIGNALLED → END issued
  bool begun_ = false;
  bool issued_ = false;
  // Cleared at occlusion BEGIN; updated by d3d9_note_gpu_draw while this query is the active occlusion bracket.
  bool seen_draw_in_bracket_ = false;
  UINT draws_in_bracket_ = 0;
};

} // namespace mullion::d3d9
