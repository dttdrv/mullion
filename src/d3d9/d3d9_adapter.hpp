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

#include "d3d9.h"
#include "com/com_pointer.hpp"
#include "dxgi_interfaces.h"
#include "mullion_format.hpp"
#include <vector>

namespace mullion {

class MTLD3D9Adapter {
public:
  MTLD3D9Adapter(UINT Ordinal, IMTLDXGIAdapter *pAdapter, HMONITOR Monitor);

  HRESULT GetIdentifier(DWORD Flags, D3DADAPTER_IDENTIFIER9 *pIdentifier);

  UINT GetModeCount(const D3DDISPLAYMODEFILTER *pFilter);
  HRESULT EnumModes(const D3DDISPLAYMODEFILTER *pFilter, UINT Mode, D3DDISPLAYMODEEX *pMode);
  HRESULT GetDisplayMode(D3DDISPLAYMODEEX *pMode, D3DDISPLAYROTATION *pRotation);

  HRESULT CheckDeviceType(D3DDEVTYPE DevType, D3DFORMAT AdapterFormat, D3DFORMAT BackBufferFormat, BOOL Windowed);
  HRESULT CheckDeviceFormat(
      D3DDEVTYPE DevType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat
  );
  HRESULT CheckDeviceMultiSampleType(
      D3DDEVTYPE DevType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType,
      DWORD *pQualityLevels
  );
  HRESULT CheckDepthStencilMatch(
      D3DDEVTYPE DevType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat
  );
  HRESULT CheckDeviceFormatConversion(D3DDEVTYPE DevType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat);
  HRESULT GetDeviceCaps(D3DDEVTYPE DevType, D3DCAPS9 *pCaps);

  HMONITOR
  GetMonitor() const {
    return monitor_;
  }

  UINT
  GetOrdinal() const {
    return ordinal_;
  }

  LUID GetLUID();

  WMT::Device
  GetMTLDevice() {
    return adapter_->GetMTLDevice();
  }

  IMTLDXGIAdapter *
  GetDXGIAdapter() {
    return adapter_.ptr();
  }

  FormatCapability GetFormatCapability(D3DFORMAT Format);

private:
  void CacheModes(D3DFORMAT Format);

  UINT ordinal_;
  Com<IMTLDXGIAdapter> adapter_;
  HMONITOR monitor_;
  FormatCapabilityInspector inspector_;
  D3DFORMAT cached_mode_format_ = D3DFMT_UNKNOWN;
  std::vector<D3DDISPLAYMODEEX> modes_;
};

bool D3D9IsDisplayFormat(D3DFORMAT Format);

void D3D9FillCaps(UINT Ordinal, WMT::Device Device, D3DCAPS9 *pCaps);

} // namespace mullion
