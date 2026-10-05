/*
 * Copyright 2026 Feifan He for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "d3d12.h"
#include "com/com_pointer.hpp"
#include "d3d12_device.hpp"
#include "dxgi_interfaces.h"
#include "log/log.hpp"
#include "util_string.hpp"

namespace dxmt {

Logger Logger::s_instance("d3d12.log");

extern "C" HRESULT WINAPI
D3D12CreateDevice(IUnknown *pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel, REFIID riid, void **ppDevice) {

  Com<IDXGIAdapter> dxgi_adapter = nullptr;
  Com<IDXGIFactory> dxgi_factory = nullptr;
  Com<IMTLDXGIAdapter> dxgi_adapter_mtl = nullptr;

  if (MinimumFeatureLevel < D3D_FEATURE_LEVEL_11_0)
    return E_INVALIDARG;

  HRESULT hr;

  if (!pAdapter) {
    hr = CreateDXGIFactory1(IID_PPV_ARGS(&dxgi_factory));

    if (FAILED(hr)) {
      ERR("D3D12CreateDevice: Failed to create a DXGI factory");
      return hr;
    }

    if (FAILED(hr = dxgi_factory->EnumAdapters(0, &dxgi_adapter))) {
      ERR("D3D12CreateDevice: No default adapter available");
      return hr;
    }
  } else {
    dxgi_adapter = com_cast<IDXGIAdapter>(pAdapter);
  }

  if (FAILED(hr = dxgi_adapter->QueryInterface(IID_PPV_ARGS(&dxgi_adapter_mtl)))) {
    ERR("D3D12CreateDevice: Not a DXMT adapter");
    return hr;
  }

  // the device, if it reaches the minimum feature level
  Com<ID3D12Device> device;
  if (FAILED(hr = dxmt::CreateD3D12Device(dxgi_adapter_mtl.ptr(), IID_PPV_ARGS(&device))))
    return hr;
  D3D12_FEATURE_DATA_FEATURE_LEVELS levels{1, &MinimumFeatureLevel};
  if (FAILED(hr = device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &levels, sizeof(levels))))
    return hr;
  if (levels.MaxSupportedFeatureLevel != MinimumFeatureLevel) {
    WARN("D3D12CreateDevice: feature level ", MinimumFeatureLevel, " is not supported");
    return E_INVALIDARG;
  }
  return ppDevice ? device->QueryInterface(riid, ppDevice) : S_FALSE;
}

extern "C" HRESULT WINAPI
D3D12GetInterface(REFCLSID rcslid, REFIID iid, void **debug) {
  if (debug)
    *debug = nullptr;
  WARN("D3D12GetInterface: Unknown interface query ", dxmt::str::format(iid));
  return E_NOINTERFACE;
}

extern "C" HRESULT WINAPI
D3D12GetDebugInterface(REFIID iid, void **debug) {
  if (debug)
    *debug = nullptr;
  WARN("D3D12GetDebugInterface: Unknown interface query ", dxmt::str::format(iid));
  return E_NOINTERFACE;
}

// features outside the released API, asked for before a device is made: none of them is here, and asking for none
// is granted (D3D12EnableExperimentalFeatures: "S_OK if successful")
extern "C" HRESULT WINAPI
D3D12EnableExperimentalFeatures(UINT NumFeatures, const IID *pIIDs, void *pConfigurationStructs, UINT *pConfigurationStructSizes) {
  return NumFeatures ? E_NOINTERFACE : S_OK;
}

BOOL WINAPI
DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  if (reason != DLL_PROCESS_ATTACH)
    return TRUE;

  DisableThreadLibraryCalls(instance);
  return TRUE;
}

} // namespace dxmt