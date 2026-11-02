#pragma once

#include <dxgi1_6.h>
#include <d3d12.h>
#include "util_d3dkmt.h"
#include "Metal.hpp"
#include "com/com_guid.hpp"

DEFINE_COM_INTERFACE("acdf3ef1-b33a-4cb6-97bd-1c1974827e6d", IMTLDXGIAdapter)
    : public IDXGIAdapter4 {
  virtual WMT::Device STDMETHODCALLTYPE GetMTLDevice() = 0;
};

DEFINE_COM_INTERFACE("6bfa1657-9cb1-471a-a4fb-7cacf8a81207", IMTLDXGIDevice)
    : public IDXGIDevice3 {
  virtual WMT::Device STDMETHODCALLTYPE GetMTLDevice() = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateSwapChain(
      IDXGIFactory1 * pFactory, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc,
      IDXGISwapChain1 **ppSwapChain) = 0;
};

DEFINE_COM_INTERFACE("6c030003-460a-4b19-909b-38f97d203e45",
                     IMTLSwapChainFactory)
    : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE CreateSwapChain(
      IDXGIFactory1 * pFactory, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc,
      IDXGISwapChain1 **ppSwapChain) = 0;
};

// Direct3D 11 on 12 shares resources between modules that each have their own objects: only Metal ones cross, with
// what the other module needs to make its own around them
struct MullionMetalResource {
  obj_handle_t texture, buffer;
  // how it was made, and its allocation's flags (TextureAllocationFlag or BufferAllocationFlag)
  WMTTextureInfo texture_info;
  WMTBufferInfo buffer_info;
  uint32_t flags;
};

struct ID3D12Resource;
struct ID3D12CommandQueue;
struct ID3D12Fence;

// what D3D11On12 needs of a Mullion D3D12 device, which another module made: the device and adapter its D3D11 device
// shares, the Metal objects behind its resources, and its queues' and fences' work in terms of a Metal shared event,
// which a D3D11 context signals and waits for (d3d11_on12.cpp)
DEFINE_COM_INTERFACE("14d37310-be0f-4275-a5d0-bbff1e87300a", IMTLD3D11On12Source)
    : public IUnknown {
  virtual WMT::Device STDMETHODCALLTYPE GetMTLDevice() = 0;
  virtual HRESULT STDMETHODCALLTYPE GetAdapter(REFIID riid, void **ppAdapter) = 0;
  // the Metal texture or buffer a resource of this device has, retained (the caller releases it), and the other 0;
  // a buffer's GPU address and CPU mapping (null when the CPU can't see it) come with it. E_INVALIDARG for another
  // device's resource or one without a single Metal object (a reserved resource)
  virtual HRESULT STDMETHODCALLTYPE GetResourceObjects(
      ID3D12Resource *pResource, MullionMetalResource *pInfo
  ) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateResourceOfMetal(
      const D3D12_RESOURCE_DESC *pDesc, const MullionMetalResource *pInfo, REFIID riid, void **ppResource) = 0;
  // signals `Event` to `Value` once `pFence` reaches `FenceValue`
  virtual HRESULT STDMETHODCALLTYPE
  SignalEventAtFence(ID3D12Fence * pFence, UINT64 FenceValue, WMT::SharedEvent Event, UINT64 Value) = 0;
};

DEFINE_COM_INTERFACE("c9f9c239-56b1-4fe6-b04d-3bf035f409cb", IMTLDXGISwapChain)
    : public IDXGISwapChain4 {
  virtual HRESULT STDMETHODCALLTYPE SetWindowReservation(IUnknown *reservation) = 0;
};

static constexpr IID MULLION_NVEXT_GUID = mullion::guid::make_guid("ba0af616-4a43-4259-815c-db3b89829905");

namespace mullion {
enum class VendorExtension {
  None,
  Nvidia,
};

extern VendorExtension g_extension_enabled;
} // namespace mullion
