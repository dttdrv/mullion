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
#include "util_md5.hpp"
#include "DXBCParser/BlobContainer.h"
#include "util_string.hpp"
#include <algorithm>
#include <atomic>

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
    if (!dxgi_adapter)
      return E_INVALIDARG;
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

static std::atomic<bool> experimental_shader_models;

bool
ShaderContainerHolds(const void *container, size_t size) {
  if (FAILED(microsoft::CDXBCParser().ReadDXBC(container, size)))
    return false;
  auto hash = md5::checkDxbcHash(container, size);
  return hash == md5::DxbcHash::Holds || (hash == md5::DxbcHash::None && experimental_shader_models);
}

// features outside the released API, asked for before a device is made and granted all or none
// (D3D12EnableExperimentalFeatures: "S_OK if successful", E_NOINTERFACE for a feature that is not known). the one
// that is here is experimental shader models, with which Direct3D takes shaders that carry no hash (INF-0004)
extern "C" HRESULT WINAPI
D3D12EnableExperimentalFeatures(UINT NumFeatures, const IID *pIIDs, void *pConfigurationStructs, UINT *pConfigurationStructSizes) {
  static const GUID shader_models = {0x76f5573e, 0xf13a, 0x40f5, {0xb2, 0x97, 0x81, 0xce, 0x9e, 0x18, 0x93, 0x3f}};
  if (NumFeatures && !pIIDs)
    return E_INVALIDARG;
  if (!std::all_of(pIIDs, pIIDs + NumFeatures, [](REFIID iid) { return iid == shader_models; }))
    return E_NOINTERFACE;
  if (NumFeatures)
    experimental_shader_models = true;
  return S_OK;
}

BOOL WINAPI
DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  if (reason != DLL_PROCESS_ATTACH)
    return TRUE;

  DisableThreadLibraryCalls(instance);
  return TRUE;
}

} // namespace dxmt