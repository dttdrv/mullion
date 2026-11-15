// ID3D12Device11 to ID3D12Device14 as d3d12.idl of microsoft/DirectX-Headers (SDK 620) declares them, for tests built
// with headers that stop before them: each interface's methods in Microsoft's slots
#pragma once
#include <d3d12.h>

#ifndef __ID3D12Device11_INTERFACE_DEFINED__
#define __ID3D12Device11_INTERFACE_DEFINED__
typedef enum D3D12_SAMPLER_FLAGS { D3D12_SAMPLER_FLAG_NONE = 0 } D3D12_SAMPLER_FLAGS;
typedef struct D3D12_SAMPLER_DESC2 {
  D3D12_FILTER Filter;
  D3D12_TEXTURE_ADDRESS_MODE AddressU, AddressV, AddressW;
  FLOAT MipLODBias;
  UINT MaxAnisotropy;
  D3D12_COMPARISON_FUNC ComparisonFunc;
  union {
    FLOAT FloatBorderColor[4];
    UINT UintBorderColor[4];
  };
  FLOAT MinLOD, MaxLOD;
  D3D12_SAMPLER_FLAGS Flags;
} D3D12_SAMPLER_DESC2;
MIDL_INTERFACE("5405c344-d457-444e-b4dd-2366e45aee39")
ID3D12Device11 : public ID3D12Device10 {
  virtual void STDMETHODCALLTYPE CreateSampler2(const D3D12_SAMPLER_DESC2 *, D3D12_CPU_DESCRIPTOR_HANDLE) = 0;
};
__CRT_UUID_DECL(ID3D12Device11, 0x5405c344, 0xd457, 0x444e, 0xb4, 0xdd, 0x23, 0x66, 0xe4, 0x5a, 0xee, 0x39)
MIDL_INTERFACE("5af5c532-4c91-4cd0-b541-15a405395fc5")
ID3D12Device12 : public ID3D12Device11 {
  // GetResourceAllocationInfo3, which returns a structure: only its slot is needed here
  virtual void STDMETHODCALLTYPE GetResourceAllocationInfo3Slot() = 0;
};
__CRT_UUID_DECL(ID3D12Device12, 0x5af5c532, 0x4c91, 0x4cd0, 0xb5, 0x41, 0x15, 0xa4, 0x05, 0x39, 0x5f, 0xc5)
MIDL_INTERFACE("14eecffc-4df8-40f7-a118-5c816f45695e")
ID3D12Device13 : public ID3D12Device12 {
  virtual HRESULT STDMETHODCALLTYPE OpenExistingHeapFromAddress1(const void *, SIZE_T, REFIID, void **) = 0;
};
__CRT_UUID_DECL(ID3D12Device13, 0x14eecffc, 0x4df8, 0x40f7, 0xa1, 0x18, 0x5c, 0x81, 0x6f, 0x45, 0x69, 0x5e)
MIDL_INTERFACE("5f6e592d-d895-44c2-8e4a-88ad4926d323")
ID3D12Device14 : public ID3D12Device13 {
  virtual HRESULT STDMETHODCALLTYPE CreateRootSignatureFromSubobjectInLibrary(
      UINT, const void *, SIZE_T, LPCWSTR, REFIID, void **
  ) = 0;
};
__CRT_UUID_DECL(ID3D12Device14, 0x5f6e592d, 0xd895, 0x44c2, 0x8e, 0x4a, 0x88, 0xad, 0x49, 0x26, 0xd3, 0x23)
#endif
