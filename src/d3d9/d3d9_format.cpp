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

#include "d3d9_format.hpp"
#include "mullion_format.hpp"

namespace mullion {

bool
D3D9QueryFormat(D3DFORMAT Format, D3D9_FORMAT_DESC &Desc) {
  Desc = {};
  switch ((uint32_t)Format) {
  case D3DFMT_A8R8G8B8:
    Desc = {DXGI_FORMAT_B8G8R8A8_UNORM, WMTPixelFormatInvalid, D3D9_FORMAT_BACKBUFFER | D3D9_FORMAT_DISPLAY};
    return true;
  case D3DFMT_X8R8G8B8:
    Desc = {DXGI_FORMAT_B8G8R8X8_UNORM, WMTPixelFormatInvalid, D3D9_FORMAT_BACKBUFFER | D3D9_FORMAT_DISPLAY};
    return true;
  case D3DFMT_A8B8G8R8:
    Desc = {DXGI_FORMAT_R8G8B8A8_UNORM};
    return true;
  case D3DFMT_X8B8G8R8:
    Desc = {DXGI_FORMAT_R8G8B8A8_UNORM, WMTPixelFormatRGB1Swizzle};
    return true;
  case D3DFMT_R5G6B5:
    Desc = {DXGI_FORMAT_B5G6R5_UNORM};
    return true;
  case D3DFMT_A1R5G5B5:
    Desc = {DXGI_FORMAT_B5G5R5A1_UNORM};
    return true;
  case D3DFMT_X1R5G5B5:
    Desc = {DXGI_FORMAT_B5G5R5A1_UNORM, WMTPixelFormatRGB1Swizzle};
    return true;
  case D3DFMT_A4R4G4B4:
    Desc = {DXGI_FORMAT_B4G4R4A4_UNORM};
    return true;
  case D3DFMT_X4R4G4B4:
    Desc = {DXGI_FORMAT_B4G4R4A4_UNORM, WMTPixelFormatGBA1Swizzle};
    return true;
  case D3DFMT_A2B10G10R10:
    Desc = {DXGI_FORMAT_R10G10B10A2_UNORM};
    return true;
  case D3DFMT_A2R10G10B10:
    // stored as BGR10A2, which has no DXGI equivalent; see D3D9GetPixelFormat
    Desc = {DXGI_FORMAT_R10G10B10A2_UNORM, WMTPixelFormatInvalid, D3D9_FORMAT_BACKBUFFER | D3D9_FORMAT_DISPLAY};
    return true;
  // D3D9 samples the channels a format lacks as 1
  case D3DFMT_G16R16:
    Desc = {DXGI_FORMAT_R16G16_UNORM, (WMTPixelFormat)(ENCODE_FORMAT_SWIZZLE(0, 2, 3, 1, 1))};
    return true;
  case D3DFMT_A16B16G16R16:
    Desc = {DXGI_FORMAT_R16G16B16A16_UNORM};
    return true;
  case D3DFMT_A8:
    Desc = {DXGI_FORMAT_A8_UNORM};
    return true;
  case D3DFMT_L8:
    Desc = {DXGI_FORMAT_R8_UNORM, WMTPixelFormatRRR1Swizzle};
    return true;
  case D3DFMT_A8L8:
    Desc = {DXGI_FORMAT_R8G8_UNORM, WMTPixelFormatRRRGSwizzle};
    return true;
  case D3DFMT_L16:
    Desc = {DXGI_FORMAT_R16_UNORM, WMTPixelFormatRRR1Swizzle};
    return true;
  case D3DFMT_V8U8:
    Desc = {DXGI_FORMAT_R8G8_SNORM, (WMTPixelFormat)(ENCODE_FORMAT_SWIZZLE(0, 2, 3, 1, 1))};
    return true;
  case D3DFMT_Q8W8V8U8:
    Desc = {DXGI_FORMAT_R8G8B8A8_SNORM};
    return true;
  case D3DFMT_V16U16:
    Desc = {DXGI_FORMAT_R16G16_SNORM, (WMTPixelFormat)(ENCODE_FORMAT_SWIZZLE(0, 2, 3, 1, 1))};
    return true;
  case D3DFMT_Q16W16V16U16:
    Desc = {DXGI_FORMAT_R16G16B16A16_SNORM};
    return true;
  case D3DFMT_R16F:
    Desc = {DXGI_FORMAT_R16_FLOAT, (WMTPixelFormat)(ENCODE_FORMAT_SWIZZLE(0, 2, 1, 1, 1))};
    return true;
  case D3DFMT_G16R16F:
    Desc = {DXGI_FORMAT_R16G16_FLOAT, (WMTPixelFormat)(ENCODE_FORMAT_SWIZZLE(0, 2, 3, 1, 1))};
    return true;
  case D3DFMT_A16B16G16R16F:
    Desc = {DXGI_FORMAT_R16G16B16A16_FLOAT};
    return true;
  case D3DFMT_R32F:
    Desc = {DXGI_FORMAT_R32_FLOAT, (WMTPixelFormat)(ENCODE_FORMAT_SWIZZLE(0, 2, 1, 1, 1))};
    return true;
  case D3DFMT_G32R32F:
    Desc = {DXGI_FORMAT_R32G32_FLOAT, (WMTPixelFormat)(ENCODE_FORMAT_SWIZZLE(0, 2, 3, 1, 1))};
    return true;
  case D3DFMT_A32B32G32R32F:
    Desc = {DXGI_FORMAT_R32G32B32A32_FLOAT};
    return true;
  case D3DFMT_DXT1:
    Desc = {DXGI_FORMAT_BC1_UNORM, WMTPixelFormatInvalid, D3D9_FORMAT_BC};
    return true;
  case D3DFMT_DXT2:
  case D3DFMT_DXT3:
    Desc = {DXGI_FORMAT_BC2_UNORM, WMTPixelFormatInvalid, D3D9_FORMAT_BC};
    return true;
  case D3DFMT_DXT4:
  case D3DFMT_DXT5:
    Desc = {DXGI_FORMAT_BC3_UNORM, WMTPixelFormatInvalid, D3D9_FORMAT_BC};
    return true;
  case D3D9FMT_ATI1:
    // AMD replicates the one channel, which FETCH4 leaves as is (Wine's visual test_fetch4)
    Desc = {DXGI_FORMAT_BC4_UNORM, (WMTPixelFormat)(ENCODE_FORMAT_SWIZZLE(0, 2, 2, 2, 2)), D3D9_FORMAT_BC};
    return true;
  case D3D9FMT_ATI2:
    // D3D9 reads the first BC5 channel as green and the second as red, with blue 1 (Wine's visual test_3dc_formats)
    Desc = {DXGI_FORMAT_BC5_UNORM, (WMTPixelFormat)(ENCODE_FORMAT_SWIZZLE(0, 3, 2, 1, 1)), D3D9_FORMAT_BC};
    return true;
  case D3DFMT_D16_LOCKABLE:
    Desc = {DXGI_FORMAT_D16_UNORM, (WMTPixelFormat)(ENCODE_FORMAT_SWIZZLE(0, 2, 0, 0, 1)), D3D9_FORMAT_DEPTH | D3D9_FORMAT_LOCKABLE};
    return true;
  case D3DFMT_D16:
    Desc = {DXGI_FORMAT_D16_UNORM, (WMTPixelFormat)(ENCODE_FORMAT_SWIZZLE(0, 2, 0, 0, 1)), D3D9_FORMAT_DEPTH};
    return true;
  case D3DFMT_D24X8:
    Desc = {DXGI_FORMAT_D24_UNORM_S8_UINT, (WMTPixelFormat)(ENCODE_FORMAT_SWIZZLE(0, 2, 0, 0, 1)), D3D9_FORMAT_DEPTH};
    return true;
  case D3DFMT_D24S8:
    Desc = {DXGI_FORMAT_D24_UNORM_S8_UINT, (WMTPixelFormat)(ENCODE_FORMAT_SWIZZLE(0, 2, 0, 0, 1)), D3D9_FORMAT_DEPTH | D3D9_FORMAT_STENCIL};
    return true;
  case D3DFMT_D24FS8:
    Desc = {DXGI_FORMAT_D32_FLOAT_S8X24_UINT, (WMTPixelFormat)(ENCODE_FORMAT_SWIZZLE(0, 2, 0, 0, 1)), D3D9_FORMAT_DEPTH | D3D9_FORMAT_STENCIL};
    return true;
  case D3DFMT_D32F_LOCKABLE:
    Desc = {DXGI_FORMAT_D32_FLOAT, WMTPixelFormatInvalid, D3D9_FORMAT_DEPTH | D3D9_FORMAT_LOCKABLE};
    return true;
  case D3D9FMT_INTZ:
  case D3D9FMT_RAWZ:
    // sampling these returns the depth in every channel; AMD's DF16 and DF24 return (d, 0, 0, 1)
    Desc = {
        DXGI_FORMAT_D24_UNORM_S8_UINT, (WMTPixelFormat)(ENCODE_FORMAT_SWIZZLE(0, 2, 2, 2, 2)),
        D3D9_FORMAT_DEPTH | D3D9_FORMAT_STENCIL | D3D9_FORMAT_SAMPLEABLE_DEPTH
    };
    return true;
  case D3D9FMT_DF24:
    Desc = {
        DXGI_FORMAT_D24_UNORM_S8_UINT, (WMTPixelFormat)(ENCODE_FORMAT_SWIZZLE(0, 2, 0, 0, 1)),
        D3D9_FORMAT_DEPTH | D3D9_FORMAT_SAMPLEABLE_DEPTH
    };
    return true;
  case D3D9FMT_DF16:
    Desc = {
        DXGI_FORMAT_D16_UNORM, (WMTPixelFormat)(ENCODE_FORMAT_SWIZZLE(0, 2, 0, 0, 1)),
        D3D9_FORMAT_DEPTH | D3D9_FORMAT_SAMPLEABLE_DEPTH
    };
    return true;
  case D3D9FMT_NULL:
    Desc = {DXGI_FORMAT_UNKNOWN, WMTPixelFormatInvalid, D3D9_FORMAT_NULL_RT};
    return true;
  case D3DFMT_YUY2:
  case D3DFMT_UYVY:
    Desc = {DXGI_FORMAT_UNKNOWN, WMTPixelFormatInvalid, D3D9_FORMAT_CPU_ONLY};
    return true;
  default:
    return false;
  }
}

WMTPixelFormat
D3D9GetPixelFormat(WMT::Device Device, D3DFORMAT Format) {
  D3D9_FORMAT_DESC desc;
  if (!D3D9QueryFormat(Format, desc) || desc.DXGIFormat == DXGI_FORMAT_UNKNOWN)
    return WMTPixelFormatInvalid;
  if (Format == D3DFMT_A2R10G10B10)
    return WMTPixelFormatBGR10A2Unorm;
  MTL_DXGI_FORMAT_DESC metal;
  if (FAILED(MTLQueryDXGIFormat(Device, desc.DXGIFormat, metal)) || metal.PixelFormat == WMTPixelFormatInvalid)
    return WMTPixelFormatInvalid;
  if (desc.Swizzle == WMTPixelFormatInvalid)
    return metal.PixelFormat;
  return (WMTPixelFormat)(ORIGINAL_FORMAT(metal.PixelFormat) | desc.Swizzle);
}

bool
D3D9GetFormatLayout(WMT::Device Device, D3DFORMAT Format, D3D9_FORMAT_LAYOUT &Layout) {
  D3D9_FORMAT_DESC desc;
  if (!D3D9QueryFormat(Format, desc))
    return false;
  // 2x1 pixel blocks of 4 bytes; Windows doesn't enforce them on locks
  if (desc.Flags & D3D9_FORMAT_CPU_ONLY) {
    Layout = {2, 1};
    return true;
  }
  // D3DFMT_NULL has no storage, but locking a lockable one returns memory with a pitch
  if (desc.Flags & D3D9_FORMAT_NULL_RT) {
    Layout = {1, 1};
    return true;
  }
  if (desc.DXGIFormat == DXGI_FORMAT_UNKNOWN)
    return false;
  MTL_DXGI_FORMAT_DESC metal;
  if (FAILED(MTLQueryDXGIFormat(Device, desc.DXGIFormat, metal)))
    return false;
  if (metal.Flag & MTL_DXGI_FORMAT_BC)
    Layout = {metal.BlockSize, 4};
  else if ((desc.Flags & D3D9_FORMAT_DEPTH) && !metal.BytesPerTexel)
    Layout = {4, 1, true};
  else
    Layout = {metal.BytesPerTexel, 1};
  return Layout.BlockBytes != 0;
}

UINT
D3D9GetFormatBitsPerPixel(D3DFORMAT Format) {
  switch (Format) {
  case D3DFMT_A8R8G8B8:
  case D3DFMT_X8R8G8B8:
  case D3DFMT_A2R10G10B10:
    return 32;
  case D3DFMT_R5G6B5:
  case D3DFMT_X1R5G5B5:
  case D3DFMT_A1R5G5B5:
    return 16;
  default:
    return 0;
  }
}

} // namespace mullion
