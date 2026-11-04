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

#include "d3d9_device_child.hpp"
#include <vector>

namespace mullion {

class MTLD3D9Device;

class MTLD3D9VertexDeclaration final : public MTLD3D9DeviceChild<IDirect3DVertexDeclaration9> {
public:
  MTLD3D9VertexDeclaration(MTLD3D9Device *pDevice, std::vector<D3DVERTEXELEMENT9> &&Elements, DWORD FVF = 0);

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final;
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) final;
  HRESULT STDMETHODCALLTYPE GetDeclaration(D3DVERTEXELEMENT9 *pElement, UINT *pNumElements) final;

  // elements without the D3DDECL_END terminator.
  const std::vector<D3DVERTEXELEMENT9> &
  elements() const {
    return elements_;
  }

  DWORD
  fvf() const {
    return fvf_;
  }

private:
  std::vector<D3DVERTEXELEMENT9> elements_;
  DWORD fvf_;
};

std::vector<D3DVERTEXELEMENT9> D3D9DeclarationFromFVF(DWORD FVF);

UINT D3D9FVFVertexSize(DWORD FVF);

// MTLAttributeFormat for a D3DDECLTYPE, or'd with an SM30_IA_FORMAT_FIXUP << 16. Zero if unsupported.
uint32_t D3D9DeclTypeToAttributeFormat(BYTE Type);

} // namespace mullion
