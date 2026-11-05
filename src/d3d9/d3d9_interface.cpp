/*
 * Copyright 2026 David Skoland
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

#include "d3d9_interface.hpp"
#include "d3d9_device.hpp"
#include "log/log.hpp"
#include "util_string.hpp"
#include "wsi_monitor.hpp"

namespace mullion {

MTLD3D9::MTLD3D9(bool Extended, IMTLDXGIAdapter *pAdapter) : extended_(Extended) {
  for (uint32_t i = 0;; i++) {
    HMONITOR monitor = wsi::enumMonitors(i);
    if (!monitor)
      break;
    adapters_.push_back(std::make_unique<MTLD3D9Adapter>(i, pAdapter, monitor));
  }
  if (adapters_.empty())
    adapters_.push_back(std::make_unique<MTLD3D9Adapter>(0, pAdapter, wsi::getDefaultMonitor()));
}

HRESULT STDMETHODCALLTYPE
MTLD3D9::QueryInterface(REFIID riid, void **ppvObject) {
  if (!ppvObject)
    return E_POINTER;
  *ppvObject = nullptr;

  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3D9) ||
      (extended_ && riid == __uuidof(IDirect3D9Ex))) {
    *ppvObject = ref(this);
    return S_OK;
  }

  WARN("IDirect3D9::QueryInterface: Unknown interface query ", str::format(riid));
  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9::RegisterSoftwareDevice(void *pInitializeFunction) {
  return D3DERR_INVALIDCALL;
}

UINT STDMETHODCALLTYPE
MTLD3D9::GetAdapterCount() {
  return adapters_.size();
}

HRESULT STDMETHODCALLTYPE
MTLD3D9::GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9 *pIdentifier) {
  if (auto adapter = GetAdapter(Adapter))
    return adapter->GetIdentifier(Flags, pIdentifier);
  return D3DERR_INVALIDCALL;
}

UINT STDMETHODCALLTYPE
MTLD3D9::GetAdapterModeCount(UINT Adapter, D3DFORMAT Format) {
  D3DDISPLAYMODEFILTER filter = {sizeof(filter), Format, D3DSCANLINEORDERING_PROGRESSIVE};
  return GetAdapterModeCountEx(Adapter, &filter);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9::EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE *pMode) {
  if (!pMode)
    return D3DERR_INVALIDCALL;

  D3DDISPLAYMODEFILTER filter = {sizeof(filter), Format, D3DSCANLINEORDERING_PROGRESSIVE};
  D3DDISPLAYMODEEX mode = {sizeof(mode)};
  HRESULT hr = EnumAdapterModesEx(Adapter, &filter, Mode, &mode);
  if (FAILED(hr))
    return hr;

  pMode->Width = mode.Width;
  pMode->Height = mode.Height;
  pMode->RefreshRate = mode.RefreshRate;
  pMode->Format = mode.Format;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9::GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE *pMode) {
  if (!pMode)
    return D3DERR_INVALIDCALL;

  D3DDISPLAYMODEEX mode = {sizeof(mode)};
  HRESULT hr = GetAdapterDisplayModeEx(Adapter, &mode, nullptr);
  if (FAILED(hr))
    return hr;

  pMode->Width = mode.Width;
  pMode->Height = mode.Height;
  pMode->RefreshRate = mode.RefreshRate;
  pMode->Format = mode.Format;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9::CheckDeviceType(
    UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT AdapterFormat, D3DFORMAT BackBufferFormat, BOOL Windowed
) {
  if (auto adapter = GetAdapter(Adapter))
    return adapter->CheckDeviceType(DevType, AdapterFormat, BackBufferFormat, Windowed);
  return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9::CheckDeviceFormat(
    UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType,
    D3DFORMAT CheckFormat
) {
  if (auto adapter = GetAdapter(Adapter))
    return adapter->CheckDeviceFormat(DeviceType, AdapterFormat, Usage, RType, CheckFormat);
  return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9::CheckDeviceMultiSampleType(
    UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType,
    DWORD *pQualityLevels
) {
  if (auto adapter = GetAdapter(Adapter))
    return adapter->CheckDeviceMultiSampleType(DeviceType, SurfaceFormat, Windowed, MultiSampleType, pQualityLevels);
  return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9::CheckDepthStencilMatch(
    UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat,
    D3DFORMAT DepthStencilFormat
) {
  if (auto adapter = GetAdapter(Adapter))
    return adapter->CheckDepthStencilMatch(DeviceType, AdapterFormat, RenderTargetFormat, DepthStencilFormat);
  return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9::CheckDeviceFormatConversion(
    UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat
) {
  if (auto adapter = GetAdapter(Adapter))
    return adapter->CheckDeviceFormatConversion(DeviceType, SourceFormat, TargetFormat);
  return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9::GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9 *pCaps) {
  if (auto adapter = GetAdapter(Adapter))
    return adapter->GetDeviceCaps(DeviceType, pCaps);
  return D3DERR_INVALIDCALL;
}

HMONITOR STDMETHODCALLTYPE
MTLD3D9::GetAdapterMonitor(UINT Adapter) {
  if (auto adapter = GetAdapter(Adapter))
    return adapter->GetMonitor();
  return nullptr;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9::CreateDevice(
    UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
    D3DPRESENT_PARAMETERS *pPresentationParameters, IDirect3DDevice9 **ppReturnedDeviceInterface
) {
  IDirect3DDevice9Ex *device = nullptr;
  HRESULT hr = CreateD3D9Device(
      this, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, nullptr, extended_, &device
  );
  if (ppReturnedDeviceInterface)
    *ppReturnedDeviceInterface = device;
  return hr;
}

UINT STDMETHODCALLTYPE
MTLD3D9::GetAdapterModeCountEx(UINT Adapter, const D3DDISPLAYMODEFILTER *pFilter) {
  if (auto adapter = GetAdapter(Adapter))
    return adapter->GetModeCount(pFilter);
  return 0;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9::EnumAdapterModesEx(UINT Adapter, const D3DDISPLAYMODEFILTER *pFilter, UINT Mode, D3DDISPLAYMODEEX *pMode) {
  if (auto adapter = GetAdapter(Adapter))
    return adapter->EnumModes(pFilter, Mode, pMode);
  return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9::GetAdapterDisplayModeEx(UINT Adapter, D3DDISPLAYMODEEX *pMode, D3DDISPLAYROTATION *pRotation) {
  if (auto adapter = GetAdapter(Adapter))
    return adapter->GetDisplayMode(pMode, pRotation);
  return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9::CreateDeviceEx(
    UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
    D3DPRESENT_PARAMETERS *pPresentationParameters, D3DDISPLAYMODEEX *pFullscreenDisplayMode,
    IDirect3DDevice9Ex **ppReturnedDeviceInterface
) {
  return CreateD3D9Device(
      this, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, pFullscreenDisplayMode, true,
      ppReturnedDeviceInterface
  );
}

HRESULT STDMETHODCALLTYPE
MTLD3D9::GetAdapterLUID(UINT Adapter, LUID *pLUID) {
  if (!pLUID)
    return D3DERR_INVALIDCALL;
  if (auto adapter = GetAdapter(Adapter)) {
    *pLUID = adapter->GetLUID();
    return D3D_OK;
  }
  return D3DERR_INVALIDCALL;
}

HRESULT
CreateD3D9(bool Extended, IDirect3D9Ex **ppDirect3D9Ex) {
  Com<IDXGIFactory1> factory;
  Com<IDXGIAdapter> dxgi_adapter;
  Com<IMTLDXGIAdapter> adapter;

  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    ERR("Direct3DCreate9: failed to create a DXGI factory");
    return D3DERR_NOTAVAILABLE;
  }

  if (FAILED(factory->EnumAdapters(0, &dxgi_adapter)) ||
      FAILED(dxgi_adapter->QueryInterface(IID_PPV_ARGS(&adapter)))) {
    ERR("Direct3DCreate9: no Mullion adapter available");
    return D3DERR_NOTAVAILABLE;
  }

  *ppDirect3D9Ex = new MTLD3D9(Extended, adapter.ptr());
  return D3D_OK;
}

} // namespace mullion
