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

#include "d3d9_adapter.hpp"
#include "d3d9_format.hpp"
#include "log/log.hpp"
#include "util_string.hpp"
#include "wsi_monitor.hpp"
#include <algorithm>
#include <cstring>

namespace mullion {

bool
D3D9IsDisplayFormat(D3DFORMAT Format) {
  return Format == D3DFMT_X8R8G8B8 || Format == D3DFMT_A2R10G10B10;
}

static bool
IsBackBufferFormat(D3DFORMAT AdapterFormat, D3DFORMAT BackBufferFormat) {
  switch (AdapterFormat) {
  case D3DFMT_X8R8G8B8:
    return BackBufferFormat == D3DFMT_X8R8G8B8 || BackBufferFormat == D3DFMT_A8R8G8B8;
  case D3DFMT_A2R10G10B10:
    return BackBufferFormat == D3DFMT_A2R10G10B10;
  case D3DFMT_R5G6B5:
    return BackBufferFormat == D3DFMT_R5G6B5;
  case D3DFMT_X1R5G5B5:
    return BackBufferFormat == D3DFMT_X1R5G5B5 || BackBufferFormat == D3DFMT_A1R5G5B5;
  default:
    return false;
  }
}

// modes are only enumerated in these formats, as in wined3d
static bool
IsEnumerableFormat(D3DFORMAT Format) {
  return Format == D3DFMT_X8R8G8B8 || Format == D3DFMT_R5G6B5;
}

MTLD3D9Adapter::MTLD3D9Adapter(UINT Ordinal, IMTLDXGIAdapter *pAdapter, HMONITOR Monitor) :
    ordinal_(Ordinal),
    adapter_(pAdapter),
    monitor_(Monitor) {
  inspector_.Inspect(adapter_->GetMTLDevice());
}

HRESULT
MTLD3D9Adapter::GetIdentifier(DWORD Flags, D3DADAPTER_IDENTIFIER9 *pIdentifier) {
  if (!pIdentifier)
    return D3DERR_INVALIDCALL;

  DXGI_ADAPTER_DESC desc;
  if (FAILED(adapter_->GetDesc(&desc)))
    return D3DERR_INVALIDCALL;

  std::memset(pIdentifier, 0, sizeof(*pIdentifier));

  const char *driver = "mullion_d3d9.dll";
  switch (desc.VendorId) {
  case 0x10DE:
    driver = "nvd3dum.dll";
    break;
  case 0x1002:
    driver = "aticfx32.dll";
    break;
  case 0x8086:
    driver = "igdumd32.dll";
    break;
  }
  std::strncpy(pIdentifier->Driver, driver, sizeof(pIdentifier->Driver) - 1);

  std::string description = str::fromws(desc.Description);
  std::strncpy(pIdentifier->Description, description.c_str(), sizeof(pIdentifier->Description) - 1);

  WCHAR device_name[32] = {};
  if (wsi::getDisplayName(monitor_, device_name)) {
    std::string name = str::fromws(device_name);
    std::strncpy(pIdentifier->DeviceName, name.c_str(), sizeof(pIdentifier->DeviceName) - 1);
  } else {
    std::snprintf(pIdentifier->DeviceName, sizeof(pIdentifier->DeviceName), "\\\\.\\DISPLAY%u", ordinal_ + 1);
  }

  // looks like a recent driver to version checks in games
  pIdentifier->DriverVersion.HighPart = (31 << 16) | 0;
  pIdentifier->DriverVersion.LowPart = (15 << 16) | 5000;
  pIdentifier->VendorId = desc.VendorId;
  pIdentifier->DeviceId = desc.DeviceId;
  pIdentifier->SubSysId = desc.SubSysId;
  pIdentifier->Revision = desc.Revision;

  static_assert(sizeof(GUID) == 16 && sizeof(LUID) == 8);
  GUID guid = {0x6a1d3d90, 0x9d4e, 0x4d1f, {0x9a, 0x5d, 0, 0, 0, 0, 0, 0}};
  std::memcpy(&guid.Data4[0], &desc.AdapterLuid, 8);
  pIdentifier->DeviceIdentifier = guid;
  pIdentifier->WHQLLevel = (Flags & D3DENUM_WHQL_LEVEL) ? 1 : 0;

  return D3D_OK;
}

void
MTLD3D9Adapter::CacheModes(D3DFORMAT Format) {
  if (cached_mode_format_ == Format)
    return;

  cached_mode_format_ = Format;
  modes_.clear();

  if (!D3D9IsDisplayFormat(Format))
    return;

  UINT bpp = D3D9GetFormatBitsPerPixel(Format);
  wsi::WsiMode mode = {};
  for (uint32_t i = 0; wsi::getDisplayMode(monitor_, i, &mode); i++) {
    if (mode.bitsPerPixel && mode.bitsPerPixel != bpp)
      continue;

    D3DDISPLAYMODEEX d3d_mode = {};
    d3d_mode.Size = sizeof(D3DDISPLAYMODEEX);
    d3d_mode.Width = mode.width;
    d3d_mode.Height = mode.height;
    d3d_mode.RefreshRate = mode.refreshRate.denominator
                               ? (mode.refreshRate.numerator + mode.refreshRate.denominator / 2) /
                                     mode.refreshRate.denominator
                               : 0;
    d3d_mode.Format = Format;
    d3d_mode.ScanLineOrdering = mode.interlaced ? D3DSCANLINEORDERING_INTERLACED : D3DSCANLINEORDERING_PROGRESSIVE;

    bool duplicate = std::any_of(modes_.begin(), modes_.end(), [&](const D3DDISPLAYMODEEX &m) {
      return m.Width == d3d_mode.Width && m.Height == d3d_mode.Height && m.RefreshRate == d3d_mode.RefreshRate &&
             m.ScanLineOrdering == d3d_mode.ScanLineOrdering;
    });
    if (!duplicate)
      modes_.push_back(d3d_mode);
  }

  std::sort(modes_.begin(), modes_.end(), [](const D3DDISPLAYMODEEX &a, const D3DDISPLAYMODEEX &b) {
    if (a.Width != b.Width)
      return a.Width < b.Width;
    if (a.Height != b.Height)
      return a.Height < b.Height;
    return a.RefreshRate < b.RefreshRate;
  });
}

static bool
MatchesFilter(const D3DDISPLAYMODEEX &Mode, const D3DDISPLAYMODEFILTER *pFilter) {
  if (pFilter->ScanLineOrdering == D3DSCANLINEORDERING_UNKNOWN)
    return true;
  return Mode.ScanLineOrdering == pFilter->ScanLineOrdering;
}

UINT
MTLD3D9Adapter::GetModeCount(const D3DDISPLAYMODEFILTER *pFilter) {
  if (!pFilter || !IsEnumerableFormat(pFilter->Format))
    return 0;
  CacheModes(pFilter->Format);
  return std::count_if(modes_.begin(), modes_.end(), [&](const D3DDISPLAYMODEEX &m) {
    return MatchesFilter(m, pFilter);
  });
}

HRESULT
MTLD3D9Adapter::EnumModes(const D3DDISPLAYMODEFILTER *pFilter, UINT Mode, D3DDISPLAYMODEEX *pMode) {
  if (!pFilter || !pMode)
    return D3DERR_INVALIDCALL;

  D3DFORMAT format = pFilter->Format;
  if (!IsEnumerableFormat(format))
    return D3DERR_INVALIDCALL;

  CacheModes(format);
  for (auto &m : modes_) {
    if (!MatchesFilter(m, pFilter))
      continue;
    if (Mode-- == 0) {
      *pMode = m;
      return D3D_OK;
    }
  }
  return D3DERR_INVALIDCALL;
}

HRESULT
MTLD3D9Adapter::GetDisplayMode(D3DDISPLAYMODEEX *pMode, D3DDISPLAYROTATION *pRotation) {
  if (pRotation)
    *pRotation = D3DDISPLAYROTATION_IDENTITY;
  if (!pMode)
    return D3D_OK;

  wsi::WsiMode mode = {};
  if (!wsi::getCurrentDisplayMode(monitor_, &mode)) {
    ERR("D3D9: failed to query current display mode");
    return D3DERR_INVALIDCALL;
  }

  pMode->Width = mode.width;
  pMode->Height = mode.height;
  pMode->RefreshRate = mode.refreshRate.denominator
                           ? (mode.refreshRate.numerator + mode.refreshRate.denominator / 2) /
                                 mode.refreshRate.denominator
                           : 0;
  pMode->Format = D3DFMT_X8R8G8B8;
  pMode->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
  return D3D_OK;
}

HRESULT
MTLD3D9Adapter::CheckDeviceType(D3DDEVTYPE DevType, D3DFORMAT AdapterFormat, D3DFORMAT BackBufferFormat, BOOL Windowed) {
  if (DevType != D3DDEVTYPE_HAL)
    return D3DERR_NOTAVAILABLE;

  if (BackBufferFormat == D3DFMT_UNKNOWN && Windowed)
    BackBufferFormat = AdapterFormat;

  if (!D3D9IsDisplayFormat(AdapterFormat))
    return D3DERR_NOTAVAILABLE;

  // windowed back buffers are converted to the desktop format, which can't be A2R10G10B10; fullscreen ones must
  // match a display mode (Wine's test_display_formats)
  if (Windowed) {
    if (AdapterFormat == D3DFMT_A2R10G10B10)
      return D3DERR_NOTAVAILABLE;
    if (FAILED(CheckDeviceFormat(DevType, AdapterFormat, D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, BackBufferFormat)) ||
        FAILED(CheckDeviceFormatConversion(DevType, BackBufferFormat, AdapterFormat)))
      return D3DERR_NOTAVAILABLE;
    return D3D_OK;
  }

  D3DDISPLAYMODEFILTER filter = {sizeof(filter), AdapterFormat, D3DSCANLINEORDERING_UNKNOWN};
  if (!GetModeCount(&filter) || !IsBackBufferFormat(AdapterFormat, BackBufferFormat))
    return D3DERR_NOTAVAILABLE;

  return D3D_OK;
}

FormatCapability
MTLD3D9Adapter::GetFormatCapability(D3DFORMAT Format) {
  WMTPixelFormat pixel_format = D3D9GetPixelFormat(adapter_->GetMTLDevice(), Format);
  if (pixel_format == WMTPixelFormatInvalid)
    return FormatCapability::None;
  auto iter = inspector_.textureCapabilities.find((WMTPixelFormat)ORIGINAL_FORMAT(pixel_format));
  if (iter == inspector_.textureCapabilities.end())
    return FormatCapability::None;
  return iter->second;
}

static bool
HasCap(FormatCapability Caps, FormatCapability Cap) {
  return (int(Caps) & int(Cap)) == int(Cap);
}

HRESULT
MTLD3D9Adapter::CheckDeviceFormat(
    D3DDEVTYPE DevType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat
) {
  if (AdapterFormat == D3DFMT_UNKNOWN)
    return D3DERR_INVALIDCALL;
  // the pseudo format apps probe for NVIDIA's alpha to coverage switch
  if (CheckFormat == (D3DFORMAT)MAKEFOURCC('A', 'T', 'O', 'C'))
    return DevType == D3DDEVTYPE_HAL && RType == D3DRTYPE_SURFACE ? D3D_OK : D3DERR_NOTAVAILABLE;
  // plain surfaces are written with sRGB only as render targets; textures answer either way
  if ((Usage & D3DUSAGE_QUERY_SRGBWRITE) && RType == D3DRTYPE_SURFACE && !(Usage & D3DUSAGE_RENDERTARGET))
    return D3DERR_NOTAVAILABLE;
  if (DevType != D3DDEVTYPE_HAL)
    return D3DERR_NOTAVAILABLE;

  if (!D3D9IsDisplayFormat(AdapterFormat))
    return D3DERR_NOTAVAILABLE;

  if (RType == D3DRTYPE_VERTEXBUFFER)
    return CheckFormat == D3DFMT_VERTEXDATA ? D3D_OK : D3DERR_NOTAVAILABLE;

  if (RType == D3DRTYPE_INDEXBUFFER)
    return (CheckFormat == D3DFMT_INDEX16 || CheckFormat == D3DFMT_INDEX32) ? D3D_OK : D3DERR_NOTAVAILABLE;

  if (Usage & D3DUSAGE_DMAP)
    return D3DERR_NOTAVAILABLE;

  D3D9_FORMAT_DESC desc;
  if (!D3D9QueryFormat(CheckFormat, desc))
    return D3DERR_NOTAVAILABLE;

  if (desc.Flags & D3D9_FORMAT_NULL_RT) {
    if (Usage == D3DUSAGE_RENDERTARGET && (RType == D3DRTYPE_SURFACE || RType == D3DRTYPE_TEXTURE))
      return D3D_OK;
    return D3DERR_NOTAVAILABLE;
  }

  // YUY2/UYVY plain surfaces live in CPU memory and are converted by StretchRect
  if (desc.Flags & D3D9_FORMAT_CPU_ONLY)
    return RType == D3DRTYPE_SURFACE && !(Usage & ~D3DUSAGE_DYNAMIC) ? D3D_OK : D3DERR_NOTAVAILABLE;

  FormatCapability caps = GetFormatCapability(CheckFormat);
  if (caps == FormatCapability::None)
    return D3DERR_NOTAVAILABLE;

  bool is_depth = desc.Flags & D3D9_FORMAT_DEPTH;

  if (Usage & D3DUSAGE_DEPTHSTENCIL) {
    if (!is_depth || !HasCap(caps, FormatCapability::DepthStencil))
      return D3DERR_NOTAVAILABLE;
    if (RType == D3DRTYPE_VOLUME || RType == D3DRTYPE_VOLUMETEXTURE)
      return D3DERR_NOTAVAILABLE;
    // like native drivers, lockable depth formats aren't offered as textures
    if ((desc.Flags & D3D9_FORMAT_LOCKABLE) && RType != D3DRTYPE_SURFACE)
      return D3DERR_NOTAVAILABLE;
  } else if (is_depth && RType != D3DRTYPE_TEXTURE && RType != D3DRTYPE_CUBETEXTURE) {
    // depth formats aren't offered as offscreen plain surfaces, which CreateOffscreenPlainSurface refuses
    return D3DERR_NOTAVAILABLE;
  }

  if (Usage & D3DUSAGE_RENDERTARGET) {
    if (is_depth || (desc.Flags & D3D9_FORMAT_BC) || !HasCap(caps, FormatCapability::Color))
      return D3DERR_NOTAVAILABLE;
    if (RType == D3DRTYPE_VOLUME || RType == D3DRTYPE_VOLUMETEXTURE)
      return D3DERR_NOTAVAILABLE;
  }

  if ((desc.Flags & D3D9_FORMAT_BC) && RType == D3DRTYPE_SURFACE && (Usage & D3DUSAGE_RENDERTARGET))
    return D3DERR_NOTAVAILABLE;

  if ((Usage & D3DUSAGE_QUERY_FILTER) && !HasCap(caps, FormatCapability::Filter))
    return D3DERR_NOTAVAILABLE;

  if ((Usage & D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING) && !HasCap(caps, FormatCapability::Blend))
    return D3DERR_NOTAVAILABLE;

  if (Usage & (D3DUSAGE_QUERY_SRGBREAD | D3DUSAGE_QUERY_SRGBWRITE)) {
    WMTPixelFormat pixel_format = D3D9GetPixelFormat(adapter_->GetMTLDevice(), CheckFormat);
    if (!Is_sRGBVariant(Recall_sRGB((WMTPixelFormat)ORIGINAL_FORMAT(pixel_format))))
      return D3DERR_NOTAVAILABLE;
  }

  if (Usage & D3DUSAGE_QUERY_LEGACYBUMPMAP) {
    if (CheckFormat != D3DFMT_V8U8 && CheckFormat != D3DFMT_Q8W8V8U8 && CheckFormat != D3DFMT_V16U16 &&
        CheckFormat != D3DFMT_Q16W16V16U16)
      return D3DERR_NOTAVAILABLE;
  }

  if (Usage & D3DUSAGE_AUTOGENMIPMAP) {
    if (!HasCap(caps, FormatCapability::Filter) || !HasCap(caps, FormatCapability::Color) || is_depth ||
        (desc.Flags & D3D9_FORMAT_BC))
      return D3DOK_NOAUTOGEN;
  }

  return D3D_OK;
}

HRESULT
MTLD3D9Adapter::CheckDeviceMultiSampleType(
    D3DDEVTYPE DevType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType,
    DWORD *pQualityLevels
) {
  if (pQualityLevels)
    *pQualityLevels = 1;

  if (DevType != D3DDEVTYPE_HAL)
    return D3DERR_NOTAVAILABLE;

  if (SurfaceFormat == D3DFMT_UNKNOWN || MultiSampleType > D3DMULTISAMPLE_16_SAMPLES)
    return D3DERR_INVALIDCALL;

  if (MultiSampleType == D3DMULTISAMPLE_NONE)
    return D3D_OK;

  D3D9_FORMAT_DESC desc;
  if (!D3D9QueryFormat(SurfaceFormat, desc) || (desc.Flags & (D3D9_FORMAT_BC | D3D9_FORMAT_LOCKABLE)))
    return D3DERR_NOTAVAILABLE;

  FormatCapability caps = GetFormatCapability(SurfaceFormat);
  bool is_depth = desc.Flags & D3D9_FORMAT_DEPTH;
  if (!is_depth && !HasCap(caps, FormatCapability::MSAA))
    return D3DERR_NOTAVAILABLE;

  WMT::Device device = adapter_->GetMTLDevice();

  if (MultiSampleType == D3DMULTISAMPLE_NONMASKABLE) {
    DWORD levels = 0;
    for (uint8_t count : {2, 4, 8})
      if (device.supportsTextureSampleCount(count))
        levels++;
    if (!levels)
      return D3DERR_NOTAVAILABLE;
    if (pQualityLevels)
      *pQualityLevels = levels;
    return D3D_OK;
  }

  if (!device.supportsTextureSampleCount((uint8_t)MultiSampleType))
    return D3DERR_NOTAVAILABLE;

  return D3D_OK;
}

HRESULT
MTLD3D9Adapter::CheckDepthStencilMatch(
    D3DDEVTYPE DevType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat
) {
  if (DevType != D3DDEVTYPE_HAL)
    return D3DERR_NOTAVAILABLE;

  if (!D3D9IsDisplayFormat(AdapterFormat))
    return D3DERR_NOTAVAILABLE;

  D3D9_FORMAT_DESC rt_desc, ds_desc;
  if (!D3D9QueryFormat(RenderTargetFormat, rt_desc) || !D3D9QueryFormat(DepthStencilFormat, ds_desc))
    return D3DERR_NOTAVAILABLE;

  if (!(ds_desc.Flags & D3D9_FORMAT_DEPTH) || (rt_desc.Flags & D3D9_FORMAT_DEPTH))
    return D3DERR_NOTAVAILABLE;

  return D3D_OK;
}

HRESULT
MTLD3D9Adapter::CheckDeviceFormatConversion(D3DDEVTYPE DevType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat) {
  if (DevType != D3DDEVTYPE_HAL)
    return D3DERR_NOTAVAILABLE;
  // Windows accepts any format converted to itself, unknown ones included
  if (SourceFormat == TargetFormat)
    return D3D_OK;

  D3D9_FORMAT_DESC src, dst;
  if (!D3D9QueryFormat(SourceFormat, src) || !D3D9QueryFormat(TargetFormat, dst))
    return D3DERR_NOTAVAILABLE;
  // video playback converts YUY2/UYVY into the back buffer formats (Wine's visual test_format_conversion)
  if (src.Flags & D3D9_FORMAT_CPU_ONLY)
    return (dst.Flags & D3D9_FORMAT_BACKBUFFER) || TargetFormat == D3DFMT_R5G6B5 || TargetFormat == D3DFMT_X1R5G5B5 ||
                   TargetFormat == D3DFMT_A1R5G5B5
               ? D3D_OK
               : D3DERR_NOTAVAILABLE;

  if (!(dst.Flags & D3D9_FORMAT_BACKBUFFER) && TargetFormat != D3DFMT_A8B8G8R8 && TargetFormat != D3DFMT_X8B8G8R8 &&
      TargetFormat != D3DFMT_A16B16G16R16F)
    return D3DERR_NOTAVAILABLE;

  if ((src.Flags & (D3D9_FORMAT_DEPTH | D3D9_FORMAT_NULL_RT)) || (dst.Flags & D3D9_FORMAT_DEPTH))
    return D3DERR_NOTAVAILABLE;

  return D3D_OK;
}

HRESULT
MTLD3D9Adapter::GetDeviceCaps(D3DDEVTYPE DevType, D3DCAPS9 *pCaps) {
  if (!pCaps)
    return D3DERR_INVALIDCALL;

  if (DevType != D3DDEVTYPE_HAL)
    return D3DERR_NOTAVAILABLE;

  D3D9FillCaps(ordinal_, adapter_->GetMTLDevice(), pCaps);
  return D3D_OK;
}

LUID
MTLD3D9Adapter::GetLUID() {
  DXGI_ADAPTER_DESC desc = {};
  adapter_->GetDesc(&desc);
  return desc.AdapterLuid;
}

} // namespace mullion
