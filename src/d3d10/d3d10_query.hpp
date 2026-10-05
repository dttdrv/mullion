#pragma once

#include "d3d10_util.hpp"

namespace dxmt {

// Direct3D 10's face of a query, and of one that is a predicate
class MTLD3D10Query final : public ID3D10Predicate {

public:
  MTLD3D10Query(ID3D11Query *d3d11, ID3D11DeviceContext *d3d11_context) : d3d11(d3d11), d3d11_context(d3d11_context) {}

  ID3D11Query *d3d11;
  ID3D11DeviceContext *d3d11_context;

  HRESULT STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    return d3d11->QueryInterface(riid, ppvObject);
  }

  ULONG STDMETHODCALLTYPE
  AddRef() {
    return d3d11->AddRef();
  }

  ULONG STDMETHODCALLTYPE
  Release() {
    return d3d11->Release();
  }

  void STDMETHODCALLTYPE
  GetDevice(ID3D10Device **ppDevice) {
    GetD3D10Device(d3d11, ppDevice);
  }

  HRESULT STDMETHODCALLTYPE
  GetPrivateData(REFGUID guid, UINT *pDataSize, void *pData) {
    return d3d11->GetPrivateData(guid, pDataSize, pData);
  }

  HRESULT STDMETHODCALLTYPE
  SetPrivateData(REFGUID guid, UINT DataSize, const void *pData) {
    return d3d11->SetPrivateData(guid, DataSize, pData);
  }

  HRESULT STDMETHODCALLTYPE
  SetPrivateDataInterface(REFGUID guid, const IUnknown *pData) {
    return d3d11->SetPrivateDataInterface(guid, pData);
  }

  void STDMETHODCALLTYPE
  Begin() {
    d3d11_context->Begin(d3d11);
  }

  void STDMETHODCALLTYPE
  End() {
    d3d11_context->End(d3d11);
  }

  // Direct3D 10's pipeline statistics are Direct3D 11's without the counters of the stages it lacks, which follow them
  bool
  Statistics() {
    D3D11_QUERY_DESC desc;
    d3d11->GetDesc(&desc);
    return desc.Query == D3D11_QUERY_PIPELINE_STATISTICS;
  }

  HRESULT STDMETHODCALLTYPE
  GetData(void *pData, UINT DataSize, UINT GetDataFlags) {
    if (!Statistics() || !DataSize)
      return d3d11_context->GetData(d3d11, pData, DataSize, GetDataFlags);
    if (DataSize != GetDataSize() || !pData)
      return E_INVALIDARG;
    D3D11_QUERY_DATA_PIPELINE_STATISTICS data;
    HRESULT hr = d3d11_context->GetData(d3d11, &data, sizeof(data), GetDataFlags);
    if (hr == S_OK)
      memcpy(pData, &data, DataSize);
    return hr;
  }

  UINT STDMETHODCALLTYPE
  GetDataSize() {
    return Statistics() ? sizeof(D3D10_QUERY_DATA_PIPELINE_STATISTICS) : d3d11->GetDataSize();
  }

  void STDMETHODCALLTYPE
  GetDesc(D3D10_QUERY_DESC *pDesc) {
    D3D11_QUERY_DESC d3d11_desc;
    d3d11->GetDesc(&d3d11_desc);

    pDesc->Query = D3D10_QUERY(d3d11_desc.Query);
    pDesc->MiscFlags = d3d11_desc.MiscFlags;
  }
};

}; // namespace dxmt