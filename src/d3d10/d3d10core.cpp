
#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "d3d11.h"
#include "log/log.hpp"

namespace dxmt {
Logger Logger::s_instance("d3d10core.log");

extern "C" HRESULT WINAPI D3D11CoreCreateDevice(
    IDXGIFactory *pFactory, IDXGIAdapter *pAdapter, UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels,
    UINT FeatureLevels, ID3D11Device **ppDevice
);

extern "C" HRESULT STDMETHODCALLTYPE
D3D10CoreCreateDevice(
    IDXGIFactory *pFactory, IDXGIAdapter *pAdapter, UINT Flags, D3D_FEATURE_LEVEL FeatureLevel, ID3D10Device **ppDevice
) {
  InitReturnPtr(ppDevice);

  Com<ID3D11Device> d3d11_device;

  if (!pAdapter)
    return E_INVALIDARG;

  HRESULT hr = pAdapter->CheckInterfaceSupport(__uuidof(ID3D10Device), nullptr);

  if (FAILED(hr))
    return hr;

  // Direct3D 11 has the same bits for the flags the two share, another for DEBUGGABLE, and none for the rest
  UINT d3d11_flags = Flags & (D3D10_CREATE_DEVICE_SINGLETHREADED | D3D10_CREATE_DEVICE_DEBUG |
                              D3D10_CREATE_DEVICE_SWITCH_TO_REF |
                              D3D10_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS |
                              D3D10_CREATE_DEVICE_BGRA_SUPPORT |
                              D3D10_CREATE_DEVICE_PREVENT_ALTERING_LAYER_SETTINGS_FROM_REGISTRY);
  if (Flags & D3D10_CREATE_DEVICE_DEBUGGABLE)
    d3d11_flags |= D3D11_CREATE_DEVICE_DEBUGGABLE;

  hr = D3D11CoreCreateDevice(pFactory, pAdapter, d3d11_flags | 0x80000000 /* DXMT_D3D10_DEVICE */, &FeatureLevel, 1, &d3d11_device);

  if (FAILED(hr))
    return hr;

  Com<ID3D10Multithread> multithread;
  d3d11_device->QueryInterface(IID_PPV_ARGS(&multithread));
  multithread->SetMultithreadProtected(!(Flags & D3D10_CREATE_DEVICE_SINGLETHREADED));

  return d3d11_device->QueryInterface(IID_PPV_ARGS(ppDevice));
}

extern "C" HRESULT STDMETHODCALLTYPE
D3D10CoreRegisterLayers() {
  // there are no layers to register
  return S_OK;
}
} // namespace dxmt