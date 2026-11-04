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

#pragma once

#include "d3d9_adapter.hpp"
#include "com/com_object.hpp"
#include <memory>
#include <vector>

namespace mullion {

class MTLD3D9 final : public ComObjectWithInitialRef<IDirect3D9Ex> {
public:
  MTLD3D9(bool Extended, IMTLDXGIAdapter *pAdapter);

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final;

  HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void *pInitializeFunction) final;
  UINT STDMETHODCALLTYPE GetAdapterCount() final;
  HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9 *pIdentifier) final;
  UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT Adapter, D3DFORMAT Format) final;
  HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE *pMode) final;
  HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE *pMode) final;
  HRESULT STDMETHODCALLTYPE
  CheckDeviceType(UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT AdapterFormat, D3DFORMAT BackBufferFormat, BOOL Windowed)
      final;
  HRESULT STDMETHODCALLTYPE CheckDeviceFormat(
      UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType,
      D3DFORMAT CheckFormat
  ) final;
  HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(
      UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType,
      DWORD *pQualityLevels
  ) final;
  HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(
      UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat,
      D3DFORMAT DepthStencilFormat
  ) final;
  HRESULT STDMETHODCALLTYPE
  CheckDeviceFormatConversion(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat)
      final;
  HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9 *pCaps) final;
  HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT Adapter) final;
  HRESULT STDMETHODCALLTYPE CreateDevice(
      UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
      D3DPRESENT_PARAMETERS *pPresentationParameters, IDirect3DDevice9 **ppReturnedDeviceInterface
  ) final;

  UINT STDMETHODCALLTYPE GetAdapterModeCountEx(UINT Adapter, const D3DDISPLAYMODEFILTER *pFilter) final;
  HRESULT STDMETHODCALLTYPE
  EnumAdapterModesEx(UINT Adapter, const D3DDISPLAYMODEFILTER *pFilter, UINT Mode, D3DDISPLAYMODEEX *pMode) final;
  HRESULT STDMETHODCALLTYPE
  GetAdapterDisplayModeEx(UINT Adapter, D3DDISPLAYMODEEX *pMode, D3DDISPLAYROTATION *pRotation) final;
  HRESULT STDMETHODCALLTYPE CreateDeviceEx(
      UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
      D3DPRESENT_PARAMETERS *pPresentationParameters, D3DDISPLAYMODEEX *pFullscreenDisplayMode,
      IDirect3DDevice9Ex **ppReturnedDeviceInterface
  ) final;
  HRESULT STDMETHODCALLTYPE GetAdapterLUID(UINT Adapter, LUID *pLUID) final;

  bool
  IsExtended() const {
    return extended_;
  }

  MTLD3D9Adapter *
  GetAdapter(UINT Ordinal) {
    return Ordinal < adapters_.size() ? adapters_[Ordinal].get() : nullptr;
  }

private:
  bool extended_;
  std::vector<std::unique_ptr<MTLD3D9Adapter>> adapters_;
};

HRESULT CreateD3D9(bool Extended, IDirect3D9Ex **ppDirect3D9Ex);

} // namespace mullion
