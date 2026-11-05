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
#include "dxgiformat.h"
#include "Metal.hpp"

namespace mullion {

constexpr D3DFORMAT D3D9FMT_INTZ = (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z');
constexpr D3DFORMAT D3D9FMT_DF16 = (D3DFORMAT)MAKEFOURCC('D', 'F', '1', '6');
constexpr D3DFORMAT D3D9FMT_DF24 = (D3DFORMAT)MAKEFOURCC('D', 'F', '2', '4');
constexpr D3DFORMAT D3D9FMT_RAWZ = (D3DFORMAT)MAKEFOURCC('R', 'A', 'W', 'Z');
constexpr D3DFORMAT D3D9FMT_NULL = (D3DFORMAT)MAKEFOURCC('N', 'U', 'L', 'L');
constexpr D3DFORMAT D3D9FMT_ATI1 = (D3DFORMAT)MAKEFOURCC('A', 'T', 'I', '1');
constexpr D3DFORMAT D3D9FMT_ATI2 = (D3DFORMAT)MAKEFOURCC('A', 'T', 'I', '2');
constexpr D3DFORMAT D3D9FMT_RESZ = (D3DFORMAT)MAKEFOURCC('R', 'E', 'S', 'Z');
constexpr D3DFORMAT D3D9FMT_ATOC = (D3DFORMAT)MAKEFOURCC('A', 'T', 'O', 'C');

enum D3D9_FORMAT_FLAG : uint32_t {
  D3D9_FORMAT_DEPTH = 1,
  D3D9_FORMAT_STENCIL = 2,
  D3D9_FORMAT_BC = 4,
  D3D9_FORMAT_LOCKABLE = 8,
  // depth formats that can be bound as textures (INTZ, DF16, DF24, RAWZ)
  D3D9_FORMAT_SAMPLEABLE_DEPTH = 16,
  D3D9_FORMAT_BACKBUFFER = 32,
  D3D9_FORMAT_DISPLAY = 64,
  D3D9_FORMAT_NULL_RT = 128,
  // YUY2 and UYVY: system memory layout only, so scratch resources only
  D3D9_FORMAT_CPU_ONLY = 256,
};

struct D3D9_FORMAT_DESC {
  DXGI_FORMAT DXGIFormat = DXGI_FORMAT_UNKNOWN;
  WMTPixelFormat Swizzle = WMTPixelFormatInvalid;
  uint32_t Flags = 0;
};

bool D3D9QueryFormat(D3DFORMAT Format, D3D9_FORMAT_DESC &Desc);

WMTPixelFormat D3D9GetPixelFormat(WMT::Device Device, D3DFORMAT Format);

UINT D3D9GetFormatBitsPerPixel(D3DFORMAT Format);

struct D3D9_FORMAT_LAYOUT {
  uint32_t BlockBytes;
  uint32_t BlockSize;
  // lockable memory only; the GPU texture's layout differs (emulated D24)
  bool CpuOnly = false;

  uint32_t
  Pitch(uint32_t Width) const {
    uint32_t pitch = (Width + BlockSize - 1) / BlockSize * BlockBytes;
    return (pitch + 3) & ~3u;
  }

  uint32_t
  Rows(uint32_t Height) const {
    return (Height + BlockSize - 1) / BlockSize;
  }
};

inline bool
D3D9HasNoAlpha(D3DFORMAT Format) {
  return Format == D3DFMT_X8R8G8B8 || Format == D3DFMT_X1R5G5B5 || Format == D3DFMT_R5G6B5 ||
         Format == D3DFMT_X4R4G4B4 || Format == D3DFMT_X8B8G8R8;
}

bool D3D9GetFormatLayout(WMT::Device Device, D3DFORMAT Format, D3D9_FORMAT_LAYOUT &Layout);

inline bool
D3D9IsDXTn(D3DFORMAT Format) {
  return Format == D3DFMT_DXT1 || Format == D3DFMT_DXT2 || Format == D3DFMT_DXT3 || Format == D3DFMT_DXT4 ||
         Format == D3DFMT_DXT5;
}

// DXT1-5 resources need sizes in whole 4x4 blocks; ATI1/ATI2 and YUV formats don't (Wine's test_surface_blocks)
inline bool
D3D9SizeInWholeBlocks(D3DFORMAT Format, UINT Width, UINT Height) {
  return !D3D9IsDXTn(Format) || (!(Width & 3) && !(Height & 3));
}

} // namespace mullion
