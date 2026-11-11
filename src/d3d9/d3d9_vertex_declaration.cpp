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

#include "d3d9_vertex_declaration.hpp"
#include "d3d9_device.hpp"
#include "airconv_public.h"
#include "log/log.hpp"
#include "util_string.hpp"

namespace mullion {

MTLD3D9VertexDeclaration::MTLD3D9VertexDeclaration(
    MTLD3D9Device *pDevice, std::vector<D3DVERTEXELEMENT9> &&Elements, DWORD FVF
) :
    MTLD3D9DeviceChild(pDevice),
    elements_(std::move(Elements)),
    fvf_(FVF) {}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexDeclaration::QueryInterface(REFIID riid, void **ppvObject) {
  if (!ppvObject)
    return E_POINTER;
  *ppvObject = nullptr;
  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DVertexDeclaration9)) {
    *ppvObject = ref(this);
    return S_OK;
  }
  WARN("IDirect3DVertexDeclaration9::QueryInterface: Unknown interface query ", str::format(riid));
  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexDeclaration::GetDevice(IDirect3DDevice9 **ppDevice) {
  return GetDeviceInternal(ppDevice);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexDeclaration::GetDeclaration(D3DVERTEXELEMENT9 *pElement, UINT *pNumElements) {
  if (!pNumElements)
    return D3DERR_INVALIDCALL;
  UINT count = elements_.size() + 1;
  if (pElement) {
    std::copy(elements_.begin(), elements_.end(), pElement);
    pElement[elements_.size()] = D3DDECL_END();
  }
  *pNumElements = count;
  return D3D_OK;
}

std::vector<D3DVERTEXELEMENT9>
D3D9DeclarationFromFVF(DWORD FVF) {
  std::vector<D3DVERTEXELEMENT9> elements;
  WORD offset = 0;
  auto add = [&](BYTE type, BYTE usage, BYTE index, WORD size) {
    elements.push_back({0, offset, type, D3DDECLMETHOD_DEFAULT, usage, index});
    offset += size;
  };

  DWORD position = FVF & D3DFVF_POSITION_MASK;
  uint32_t blend_weights = 0;
  switch (position) {
  case D3DFVF_XYZ:
    add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0, 12);
    break;
  case D3DFVF_XYZRHW:
    add(D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITIONT, 0, 16);
    break;
  case D3DFVF_XYZW:
    add(D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITION, 0, 16);
    break;
  case D3DFVF_XYZB1:
  case D3DFVF_XYZB2:
  case D3DFVF_XYZB3:
  case D3DFVF_XYZB4:
  case D3DFVF_XYZB5:
    add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0, 12);
    blend_weights = (position - D3DFVF_XYZB1) / 2 + 1;
    break;
  default:
    break;
  }

  // native quirks (Wine's test_fvf_decl_conversion): XYZB5 without LASTBETA makes the fifth value FLOAT1 indices,
  // and XYZB2 with LASTBETA_D3DCOLOR declares a D3DCOLOR weight and UBYTE4 indices
  if (blend_weights == 5 && !(FVF & (D3DFVF_LASTBETA_UBYTE4 | D3DFVF_LASTBETA_D3DCOLOR))) {
    add(D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_BLENDWEIGHT, 0, 16);
    add(D3DDECLTYPE_FLOAT1, D3DDECLUSAGE_BLENDINDICES, 0, 4);
    blend_weights = 0;
  } else if (blend_weights == 2 && (FVF & D3DFVF_LASTBETA_D3DCOLOR)) {
    add(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_BLENDWEIGHT, 0, 4);
    add(D3DDECLTYPE_UBYTE4, D3DDECLUSAGE_BLENDINDICES, 0, 4);
    blend_weights = 0;
  }
  if (blend_weights) {
    bool last_index = FVF & (D3DFVF_LASTBETA_UBYTE4 | D3DFVF_LASTBETA_D3DCOLOR);
    uint32_t weights = last_index ? blend_weights - 1 : blend_weights;
    static const BYTE float_types[] = {D3DDECLTYPE_FLOAT1, D3DDECLTYPE_FLOAT2, D3DDECLTYPE_FLOAT3, D3DDECLTYPE_FLOAT4};
    if (weights)
      add(float_types[std::min(weights, 4u) - 1], D3DDECLUSAGE_BLENDWEIGHT, 0, weights * 4);
    if (last_index)
      add(FVF & D3DFVF_LASTBETA_UBYTE4 ? D3DDECLTYPE_UBYTE4 : D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_BLENDINDICES, 0, 4);
  }

  if (FVF & D3DFVF_NORMAL)
    add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_NORMAL, 0, 12);
  if (FVF & D3DFVF_PSIZE)
    add(D3DDECLTYPE_FLOAT1, D3DDECLUSAGE_PSIZE, 0, 4);
  if (FVF & D3DFVF_DIFFUSE)
    add(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 0, 4);
  if (FVF & D3DFVF_SPECULAR)
    add(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 1, 4);

  uint32_t tex_count = (FVF & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
  for (uint32_t i = 0; i < tex_count && i < 8; i++) {
    switch ((FVF >> (16 + i * 2)) & 0x3) {
    case D3DFVF_TEXTUREFORMAT1:
      add(D3DDECLTYPE_FLOAT1, D3DDECLUSAGE_TEXCOORD, i, 4);
      break;
    case D3DFVF_TEXTUREFORMAT3:
      add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_TEXCOORD, i, 12);
      break;
    case D3DFVF_TEXTUREFORMAT4:
      add(D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_TEXCOORD, i, 16);
      break;
    default:
      add(D3DDECLTYPE_FLOAT2, D3DDECLUSAGE_TEXCOORD, i, 8);
      break;
    }
  }
  return elements;
}

UINT
D3D9FVFVertexSize(DWORD FVF) {
  UINT size = 0;
  for (auto &element : D3D9DeclarationFromFVF(FVF)) {
    switch (element.Type) {
    case D3DDECLTYPE_FLOAT1:
    case D3DDECLTYPE_D3DCOLOR:
    case D3DDECLTYPE_UBYTE4:
      size += 4;
      break;
    case D3DDECLTYPE_FLOAT2:
      size += 8;
      break;
    case D3DDECLTYPE_FLOAT3:
      size += 12;
      break;
    case D3DDECLTYPE_FLOAT4:
      size += 16;
      break;
    default:
      break;
    }
  }
  return size;
}

uint32_t
D3D9DeclTypeToAttributeFormat(BYTE Type) {
  auto fixup = [](uint32_t format, uint32_t fix) { return format | (fix << 16); };
  switch (Type) {
  case D3DDECLTYPE_FLOAT1:
    return WMTAttributeFormatFloat;
  case D3DDECLTYPE_FLOAT2:
    return WMTAttributeFormatFloat2;
  case D3DDECLTYPE_FLOAT3:
    return WMTAttributeFormatFloat3;
  case D3DDECLTYPE_FLOAT4:
    return WMTAttributeFormatFloat4;
  case D3DDECLTYPE_D3DCOLOR:
    return WMTAttributeFormatUChar4Normalized_BGRA;
  case D3DDECLTYPE_UBYTE4:
    return fixup(WMTAttributeFormatUChar4, SM30_IA_FORMAT_FIXUP_UINT_TO_FLOAT);
  case D3DDECLTYPE_SHORT2:
    return fixup(WMTAttributeFormatShort2, SM30_IA_FORMAT_FIXUP_INT_TO_FLOAT);
  case D3DDECLTYPE_SHORT4:
    return fixup(WMTAttributeFormatShort4, SM30_IA_FORMAT_FIXUP_INT_TO_FLOAT);
  case D3DDECLTYPE_UBYTE4N:
    return WMTAttributeFormatUChar4Normalized;
  case D3DDECLTYPE_SHORT2N:
    return WMTAttributeFormatShort2Normalized;
  case D3DDECLTYPE_SHORT4N:
    return WMTAttributeFormatShort4Normalized;
  case D3DDECLTYPE_USHORT2N:
    return WMTAttributeFormatUShort2Normalized;
  case D3DDECLTYPE_USHORT4N:
    return WMTAttributeFormatUShort4Normalized;
  case D3DDECLTYPE_UDEC3:
    return fixup(WMTAttributeFormatUInt1010102Normalized, SM30_IA_FORMAT_FIXUP_UDEC3);
  case D3DDECLTYPE_DEC3N:
    return fixup(WMTAttributeFormatInt1010102Normalized, SM30_IA_FORMAT_FIXUP_DEC3N);
  case D3DDECLTYPE_FLOAT16_2:
    return WMTAttributeFormatHalf2;
  case D3DDECLTYPE_FLOAT16_4:
    return WMTAttributeFormatHalf4;
  default:
    return 0;
  }
}

} // namespace mullion
