#pragma once

// ID3D12Device11 to ID3D12Device15 and their types, as the Agility SDK's d3d12.h has them, for a toolchain whose
// d3d12.h ends at ID3D12Device10. a layer that wraps the device, as Streamline's interposer does, may offer the
// application the newest interface it knows and forward each call to the same slot of the device under it: every slot
// up to ID3D12Device15's last must be there (CONTROL Resonant's NRI calls TryCreateConstantBufferView, slot 87)

#include "d3d12.h"

#ifndef __ID3D12Device11_INTERFACE_DEFINED__
#define __ID3D12Device11_INTERFACE_DEFINED__

typedef enum D3D12_SAMPLER_FLAGS {
  D3D12_SAMPLER_FLAG_NONE = 0,
  D3D12_SAMPLER_FLAG_UINT_BORDER_COLOR = 0x1,
  D3D12_SAMPLER_FLAG_NON_NORMALIZED_COORDINATES = 0x2,
} D3D12_SAMPLER_FLAGS;

typedef struct D3D12_SAMPLER_DESC2 {
  D3D12_FILTER Filter;
  D3D12_TEXTURE_ADDRESS_MODE AddressU;
  D3D12_TEXTURE_ADDRESS_MODE AddressV;
  D3D12_TEXTURE_ADDRESS_MODE AddressW;
  FLOAT MipLODBias;
  UINT MaxAnisotropy;
  D3D12_COMPARISON_FUNC ComparisonFunc;
  union {
    FLOAT FloatBorderColor[4];
    UINT UintBorderColor[4];
  };
  FLOAT MinLOD;
  FLOAT MaxLOD;
  D3D12_SAMPLER_FLAGS Flags;
} D3D12_SAMPLER_DESC2;

MIDL_INTERFACE("5405c344-d457-444e-b4dd-2366e45aee39")
ID3D12Device11 : public ID3D12Device10 {
  virtual void STDMETHODCALLTYPE CreateSampler2(const D3D12_SAMPLER_DESC2 *pDesc,
                                                D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) = 0;
};
__CRT_UUID_DECL(ID3D12Device11, 0x5405c344, 0xd457, 0x444e, 0xb4, 0xdd, 0x23, 0x66, 0xe4, 0x5a, 0xee, 0x39)
#endif

#ifndef __ID3D12Device12_INTERFACE_DEFINED__
#define __ID3D12Device12_INTERFACE_DEFINED__
MIDL_INTERFACE("5af5c532-4c91-4cd0-b541-15a405395fc5")
ID3D12Device12 : public ID3D12Device11 {
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
  virtual D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE GetResourceAllocationInfo3(
      D3D12_RESOURCE_ALLOCATION_INFO * __ret, UINT visibleMask, UINT numResourceDescs,
      const D3D12_RESOURCE_DESC1 *pResourceDescs, const UINT32 *pNumCastableFormats,
      const DXGI_FORMAT *const *ppCastableFormats, D3D12_RESOURCE_ALLOCATION_INFO1 *pResourceAllocationInfo1) = 0;
  D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo3(
      UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC1 *pResourceDescs,
      const UINT32 *pNumCastableFormats, const DXGI_FORMAT *const *ppCastableFormats,
      D3D12_RESOURCE_ALLOCATION_INFO1 *pResourceAllocationInfo1) {
    D3D12_RESOURCE_ALLOCATION_INFO __ret;
    return *GetResourceAllocationInfo3(&__ret, visibleMask, numResourceDescs, pResourceDescs, pNumCastableFormats,
                                       ppCastableFormats, pResourceAllocationInfo1);
  }
#else
  virtual D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo3(
      UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC1 *pResourceDescs,
      const UINT32 *pNumCastableFormats, const DXGI_FORMAT *const *ppCastableFormats,
      D3D12_RESOURCE_ALLOCATION_INFO1 *pResourceAllocationInfo1) = 0;
#endif
};
__CRT_UUID_DECL(ID3D12Device12, 0x5af5c532, 0x4c91, 0x4cd0, 0xb5, 0x41, 0x15, 0xa4, 0x05, 0x39, 0x5f, 0xc5)
#endif

#ifndef __ID3D12Device13_INTERFACE_DEFINED__
#define __ID3D12Device13_INTERFACE_DEFINED__
MIDL_INTERFACE("14eecffc-4df8-40f7-a118-5c816f45695e")
ID3D12Device13 : public ID3D12Device12 {
  virtual HRESULT STDMETHODCALLTYPE OpenExistingHeapFromAddress1(const void *pAddress, SIZE_T size, REFIID riid,
                                                                 void **ppvHeap) = 0;
};
__CRT_UUID_DECL(ID3D12Device13, 0x14eecffc, 0x4df8, 0x40f7, 0xa1, 0x18, 0x5c, 0x81, 0x6f, 0x45, 0x69, 0x5e)
#endif

#ifndef __ID3D12Device14_INTERFACE_DEFINED__
#define __ID3D12Device14_INTERFACE_DEFINED__
MIDL_INTERFACE("5f6e592d-d895-44c2-8e4a-88ad4926d323")
ID3D12Device14 : public ID3D12Device13 {
  virtual HRESULT STDMETHODCALLTYPE CreateRootSignatureFromSubobjectInLibrary(
      UINT nodeMask, const void *pLibraryBlob, SIZE_T blobLengthInBytes, LPCWSTR subobjectName, REFIID riid,
      void **ppvRootSignature) = 0;
};
__CRT_UUID_DECL(ID3D12Device14, 0x5f6e592d, 0xd895, 0x44c2, 0x8e, 0x4a, 0x88, 0xad, 0x49, 0x26, 0xd3, 0x23)
#endif

#ifndef __ID3D12Device15_INTERFACE_DEFINED__
#define __ID3D12Device15_INTERFACE_DEFINED__

typedef enum D3D12_TRIM_NOTIFICATION_FLAGS {
  D3D12_TRIM_NOTIFICATION_FLAG_NONE = 0,
  D3D12_TRIM_NOTIFICATION_FLAG_PERIODIC_TRIM = 0x1,
  D3D12_TRIM_NOTIFICATION_FLAG_RESTART_PERIODIC_TRIM = 0x2,
  D3D12_TRIM_NOTIFICATION_FLAG_TRIM_TO_BUDGET = 0x4,
} D3D12_TRIM_NOTIFICATION_FLAGS;

typedef struct D3D12_TRIM_NOTIFICATION {
  void *pContext;
  D3D12_TRIM_NOTIFICATION_FLAGS Flags;
  UINT64 NumBytesToTrim;
} D3D12_TRIM_NOTIFICATION;

typedef void(__stdcall *D3D12_PFN_TRIM_NOTIFICATION_CALLBACK)(const D3D12_TRIM_NOTIFICATION *);

typedef struct D3D12_REGISTER_TRIM_NOTIFICATION {
  D3D12_PFN_TRIM_NOTIFICATION_CALLBACK pfnCallback;
  void *pContext;
  DWORD CallbackCookie;
} D3D12_REGISTER_TRIM_NOTIFICATION;

typedef enum D3D12_QUERY_HEAP_FLAGS {
  D3D12_QUERY_HEAP_FLAG_NONE = 0,
  D3D12_QUERY_HEAP_FLAG_CPU_RESOLVE = 1,
} D3D12_QUERY_HEAP_FLAGS;

MIDL_INTERFACE("76cff76f-1e9b-4450-8cdc-34f1af788e5b")
ID3D12Device15 : public ID3D12Device14 {
  virtual HRESULT STDMETHODCALLTYPE RegisterTrimNotificationCallback(D3D12_REGISTER_TRIM_NOTIFICATION *pData) = 0;
  virtual HRESULT STDMETHODCALLTYPE UnregisterTrimNotificationCallback(DWORD CallbackCookie) = 0;
  virtual HRESULT STDMETHODCALLTYPE TryCreateShaderResourceView(
      ID3D12Resource *pResource, const D3D12_SHADER_RESOURCE_VIEW_DESC *pDesc,
      D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) = 0;
  virtual HRESULT STDMETHODCALLTYPE TryCreateUnorderedAccessView(
      ID3D12Resource *pResource, ID3D12Resource *pCounterResource, const D3D12_UNORDERED_ACCESS_VIEW_DESC *pDesc,
      D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) = 0;
  virtual HRESULT STDMETHODCALLTYPE TryCreateConstantBufferView(
      const D3D12_CONSTANT_BUFFER_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) = 0;
  virtual HRESULT STDMETHODCALLTYPE TryCreateSampler2(const D3D12_SAMPLER_DESC2 *pDesc,
                                                      D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) = 0;
  virtual HRESULT STDMETHODCALLTYPE TryCreateRenderTargetView(
      ID3D12Resource *pResource, const D3D12_RENDER_TARGET_VIEW_DESC *pDesc,
      D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) = 0;
  virtual HRESULT STDMETHODCALLTYPE TryCreateDepthStencilView(
      ID3D12Resource *pResource, const D3D12_DEPTH_STENCIL_VIEW_DESC *pDesc,
      D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) = 0;
  virtual HRESULT STDMETHODCALLTYPE TryCreateSamplerFeedbackUnorderedAccessView(
      ID3D12Resource *pTargetedResource, ID3D12Resource *pFeedbackResource,
      D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateQueryHeap1(const D3D12_QUERY_HEAP_DESC *pDesc, D3D12_QUERY_HEAP_FLAGS Flags,
                                                     REFIID riid, void **ppvHeap) = 0;
  virtual HRESULT STDMETHODCALLTYPE ResolveQueryData(ID3D12QueryHeap *pQueryHeap, D3D12_QUERY_TYPE Type,
                                                     UINT StartIndex, UINT NumQueries, void *pResolvedQueryData) = 0;
};
__CRT_UUID_DECL(ID3D12Device15, 0x76cff76f, 0x1e9b, 0x4450, 0x8c, 0xdc, 0x34, 0xf1, 0xaf, 0x78, 0x8e, 0x5b)
#endif
