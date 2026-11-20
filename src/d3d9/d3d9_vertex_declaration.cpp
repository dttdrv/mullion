#include "d3d9_vertex_declaration.hpp"

namespace mullion::d3d9 {

const IID IID_IMTLD3D9VertexDeclaration = {0x8f3a5c21, 0x5b7e, 0x4c1d, {0x9a, 0x3e, 0x2f, 0x61, 0x7c, 0x4d, 0x0b, 0x95}};

DXGI_FORMAT
to_dxgi_declaration_format(UINT type) {
  switch (type) {
  case D3DDECLTYPE_FLOAT1:
    return DXGI_FORMAT_R32_FLOAT;
  case D3DDECLTYPE_FLOAT2:
    return DXGI_FORMAT_R32G32_FLOAT;
  case D3DDECLTYPE_FLOAT3:
    return DXGI_FORMAT_R32G32B32_FLOAT;
  case D3DDECLTYPE_FLOAT4:
    return DXGI_FORMAT_R32G32B32A32_FLOAT;
  case D3DDECLTYPE_D3DCOLOR:
    return DXGI_FORMAT_B8G8R8A8_UNORM;
  case D3DDECLTYPE_UBYTE4:
    return DXGI_FORMAT_R8G8B8A8_UINT;
  case D3DDECLTYPE_UBYTE4N:
    return DXGI_FORMAT_R8G8B8A8_UNORM;
  case D3DDECLTYPE_SHORT2:
    return DXGI_FORMAT_R16G16_SINT;
  case D3DDECLTYPE_SHORT2N:
    return DXGI_FORMAT_R16G16_SNORM;
  case D3DDECLTYPE_SHORT4:
    return DXGI_FORMAT_R16G16B16A16_SINT;
  case D3DDECLTYPE_SHORT4N:
    return DXGI_FORMAT_R16G16B16A16_SNORM;
  case D3DDECLTYPE_USHORT2N:
    return DXGI_FORMAT_R16G16_UNORM;
  case D3DDECLTYPE_USHORT4N:
    return DXGI_FORMAT_R16G16B16A16_UNORM;
  case D3DDECLTYPE_UDEC3:
    return DXGI_FORMAT_R10G10B10A2_UINT;
  case D3DDECLTYPE_DEC3N:
    return DXGI_FORMAT_R10G10B10A2_UNORM;
  case D3DDECLTYPE_FLOAT16_2:
    return DXGI_FORMAT_R16G16_FLOAT;
  case D3DDECLTYPE_FLOAT16_4:
    return DXGI_FORMAT_R16G16B16A16_FLOAT;
  default:
    return DXGI_FORMAT_UNKNOWN;
  }
}

MTLD3D9VertexDeclaration::MTLD3D9VertexDeclaration(IDirect3DDevice9 *device, std::vector<D3DVERTEXELEMENT9> elements,
                                                   bool counted_device)
    : device_(device), device_ref_(device, counted_device), elements_(std::move(elements)) {
  // the names the layout points at, and then the layout itself: a layout entry holds the pointer, so the strings have
  // to be where they end up before the entries are made
  names_.reserve(elements_.size());
  for (const auto &element : elements_)
    names_.emplace_back(usage_name(element.Usage));

  inputs_.reserve(elements_.size());
  layout_.reserve(elements_.size());
  for (UINT at = 0; at < elements_.size(); at++) {
    const auto &element = elements_[at];
    VertexInput input;
    input.reg = at;
    input.usage = element.Usage;
    input.usage_index = element.UsageIndex;
    input.mask = declaration_type_mask(element.Type);
    inputs_.push_back(input);

    D3D11_INPUT_ELEMENT_DESC desc{};
    desc.SemanticName = names_[at].c_str();
    desc.SemanticIndex = element.UsageIndex;
    desc.Format = to_dxgi_declaration_format(element.Type);
    desc.InputSlot = element.Stream;
    desc.AlignedByteOffset = element.Offset;
    desc.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
    desc.InstanceDataStepRate = 0;
    layout_.push_back(desc);
  }
}

HRESULT
MTLD3D9VertexDeclaration::QueryInterface(REFIID riid, void **object) {
  if (!object)
    return E_POINTER;
  *object = nullptr;
  if (riid == IID_IUnknown || riid == __uuidof(IDirect3DVertexDeclaration9)) {
    *object = static_cast<IDirect3DVertexDeclaration9 *>(this);
  } else if (riid == IID_IMTLD3D9VertexDeclaration) {
    *object = static_cast<IMTLD3D9VertexDeclaration *>(this);
  } else {
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

HRESULT
MTLD3D9VertexDeclaration::GetDevice(IDirect3DDevice9 **device) {
  if (!device)
    return D3DERR_INVALIDCALL;
  *device = device_;
  if (*device)
    (*device)->AddRef();
  return S_OK;
}

HRESULT
MTLD3D9VertexDeclaration::GetDeclaration(D3DVERTEXELEMENT9 *elements, UINT *count) {
  if (!count)
    return D3DERR_INVALIDCALL;
  // D3D9's pNumElements is an element count, including the D3DDECL_END terminator, not a byte count.
  const UINT count_with_end = (UINT)elements_.size() + 1;
  const UINT capacity = *count;
  *count = count_with_end;
  if (!elements)
    return S_OK;
  // Wine's conformance test calls this with a zero in/out count and a large enough buffer; zero means "no declared
  // capacity", while a nonzero count that is too small is still a caller error.
  if (capacity && capacity < count_with_end)
    return D3DERR_INVALIDCALL;
  memcpy(elements, elements_.data(), elements_.size() * sizeof(D3DVERTEXELEMENT9));
  D3DVERTEXELEMENT9 end{};
  end.Stream = 0xFF;
  end.Type = D3DDECLTYPE_UNUSED;
  elements[elements_.size()] = end;
  return S_OK;
}

namespace {

// the bytes an element of `type` occupies, which is what the next element's offset is laid out from
WORD
declaration_type_size(BYTE type) {
  switch (type) {
  case D3DDECLTYPE_FLOAT1:
  case D3DDECLTYPE_D3DCOLOR:
  case D3DDECLTYPE_UBYTE4:
  case D3DDECLTYPE_UBYTE4N:
  case D3DDECLTYPE_SHORT2:
  case D3DDECLTYPE_SHORT2N:
  case D3DDECLTYPE_USHORT2N:
  case D3DDECLTYPE_UDEC3:
  case D3DDECLTYPE_DEC3N:
  case D3DDECLTYPE_FLOAT16_2:
    return 4;
  case D3DDECLTYPE_FLOAT2:
  case D3DDECLTYPE_SHORT4:
  case D3DDECLTYPE_SHORT4N:
  case D3DDECLTYPE_USHORT4N:
  case D3DDECLTYPE_FLOAT16_4:
    return 8;
  case D3DDECLTYPE_FLOAT3:
    return 12;
  case D3DDECLTYPE_FLOAT4:
    return 16;
  default:
    return 0;
  }
}

} // namespace

UINT
declaration_element_size(BYTE type) {
  return declaration_type_size(type);
}

void
MTLD3D9VertexDeclaration::hold_device_cache() {
  set_device_owned(true);
  // the device's cache is the object's owner: the creating reference is dropped, and the public count starts at zero
  Release();
}

void
MTLD3D9VertexDeclaration::free_device_cache() {
  device_ = nullptr;
  device_ref_.detach();
  destroy_device_owned();
}

bool
elements_from_fvf(DWORD fvf, std::vector<D3DVERTEXELEMENT9> &elements) {
  elements.clear();
  WORD offset = 0;
  auto push = [&](BYTE type, BYTE usage, BYTE usage_index) {
    D3DVERTEXELEMENT9 element{};
    element.Stream = 0;
    element.Offset = offset;
    element.Type = type;
    element.Method = D3DDECLMETHOD_DEFAULT;
    element.Usage = usage;
    element.UsageIndex = usage_index;
    elements.push_back(element);
    offset = (WORD)(offset + declaration_type_size(type));
  };

  // D3D9's own conversion (Wine: dlls/d3d9/vertexdeclaration.c, vdecl_convert_fvf). The position bits say which
  // position element there is and whether a blend element follows; the LASTBETA bits move the last blend value into a
  // separate BLENDINDICES element, which shortens the blend weights by one value.
  const DWORD position = fvf & D3DFVF_POSITION_MASK;
  const bool has_position = position != 0;
  const DWORD blend = fvf & D3DFVF_XYZB5;
  const bool has_blend = blend > D3DFVF_XYZRHW;
  const bool has_blend_indices = has_blend &&
                                 (blend == D3DFVF_XYZB5 || (fvf & D3DFVF_LASTBETA_D3DCOLOR) ||
                                  (fvf & D3DFVF_LASTBETA_UBYTE4));
  UINT blend_count = has_blend ? 1 + ((blend - D3DFVF_XYZB1) >> 1) : 0;
  if (has_blend_indices)
    blend_count--;

  if (has_position) {
    if (!has_blend && (fvf & D3DFVF_XYZRHW))
      push(D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITIONT, 0);
    else if (!has_blend && (fvf & D3DFVF_XYZW) == D3DFVF_XYZW)
      push(D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITION, 0);
    else
      push(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0);
  }
  if (has_blend && blend_count) {
    BYTE type;
    if (blend == D3DFVF_XYZB2 && (fvf & D3DFVF_LASTBETA_D3DCOLOR))
      type = D3DDECLTYPE_D3DCOLOR;
    else if (blend_count == 1)
      type = D3DDECLTYPE_FLOAT1;
    else if (blend_count == 2)
      type = D3DDECLTYPE_FLOAT2;
    else if (blend_count == 3)
      type = D3DDECLTYPE_FLOAT3;
    else if (blend_count == 4)
      type = D3DDECLTYPE_FLOAT4;
    else
      return false;
    push(type, D3DDECLUSAGE_BLENDWEIGHT, 0);
  }
  if (has_blend_indices) {
    const bool ubyte4 = (fvf & D3DFVF_LASTBETA_UBYTE4) ||
                        (blend == D3DFVF_XYZB2 && (fvf & D3DFVF_LASTBETA_D3DCOLOR));
    const BYTE type = ubyte4                              ? D3DDECLTYPE_UBYTE4
                      : (fvf & D3DFVF_LASTBETA_D3DCOLOR) ? D3DDECLTYPE_D3DCOLOR
                                                         : D3DDECLTYPE_FLOAT1;
    push(type, D3DDECLUSAGE_BLENDINDICES, 0);
  }
  if (fvf & D3DFVF_NORMAL)
    push(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_NORMAL, 0);
  if (fvf & D3DFVF_PSIZE)
    push(D3DDECLTYPE_FLOAT1, D3DDECLUSAGE_PSIZE, 0);
  if (fvf & D3DFVF_DIFFUSE)
    push(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 0);
  if (fvf & D3DFVF_SPECULAR)
    push(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 1);
  const UINT tex = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
  if (tex > 8)
    return false;
  for (UINT at = 0; at < tex; at++) {
    // The two bits above each texture coordinate are its component count: FORMAT1 is code 3, FORMAT2 code 0,
    // FORMAT3 code 1 and FORMAT4 code 2 (d3d9types.h, D3DFVF_TEXCOORDSIZEn).
    const DWORD format = (fvf >> (16 + at * 2)) & 0x3;
    const BYTE type = format == D3DFVF_TEXTUREFORMAT1 ? D3DDECLTYPE_FLOAT1
                      : format == D3DFVF_TEXTUREFORMAT3 ? D3DDECLTYPE_FLOAT3
                      : format == D3DFVF_TEXTUREFORMAT4 ? D3DDECLTYPE_FLOAT4
                                                       : D3DDECLTYPE_FLOAT2;
    push(type, D3DDECLUSAGE_TEXCOORD, (BYTE)at);
  }
  return true;
}

namespace {

const DWORD kFvfCandidates[] = {
    D3DFVF_XYZ,
    D3DFVF_XYZ | D3DFVF_DIFFUSE,
    // a declaration can carry a texture coordinate with no colour at all (Wine visual.c texture_transform_flags_test's
    // position + TEXCOORD declaration)
    D3DFVF_XYZ | D3DFVF_TEX1,
    D3DFVF_XYZ | D3DFVF_TEX2,
    D3DFVF_XYZ | D3DFVF_TEX3,
    D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1,
    D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX2,
    D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX3,
    D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE,
    D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1,
    D3DFVF_XYZRHW | D3DFVF_DIFFUSE,
    D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1,
    D3DFVF_XYZB1 | D3DFVF_DIFFUSE,
    D3DFVF_XYZB2 | D3DFVF_DIFFUSE,
    D3DFVF_XYZB3 | D3DFVF_DIFFUSE,
    // the same formats with their single texture coordinate declared as one, three or four components: the size bits
    // are D3DFVF_TEXCOORD1SIZE(1..4), and the two-component form is the plain candidate above
    D3DFVF_XYZ | D3DFVF_TEX1 | (D3DFVF_TEXTUREFORMAT1 << 16),
    D3DFVF_XYZ | D3DFVF_TEX1 | (D3DFVF_TEXTUREFORMAT3 << 16),
    D3DFVF_XYZ | D3DFVF_TEX1 | (D3DFVF_TEXTUREFORMAT4 << 16),
    D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1 | (D3DFVF_TEXTUREFORMAT1 << 16),
    D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1 | (D3DFVF_TEXTUREFORMAT3 << 16),
    D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1 | (D3DFVF_TEXTUREFORMAT4 << 16),
};

} // namespace

bool
fvf_from_elements(const std::vector<D3DVERTEXELEMENT9> &elements, DWORD &fvf) {
  for (DWORD candidate : kFvfCandidates) {
    std::vector<D3DVERTEXELEMENT9> generated;
    if (!elements_from_fvf(candidate, generated))
      continue;
    if (generated.size() != elements.size())
      continue;
    bool match = true;
    for (size_t at = 0; at < elements.size(); at++) {
      const D3DVERTEXELEMENT9 &want = generated[at];
      const D3DVERTEXELEMENT9 &have = elements[at];
      if (want.Stream != have.Stream || want.Offset != have.Offset || want.Type != have.Type ||
          want.Method != have.Method || want.Usage != have.Usage || want.UsageIndex != have.UsageIndex) {
        match = false;
        break;
      }
    }
    if (match) {
      fvf = candidate;
      return true;
    }
  }
  return false;
}

bool
fvf_from_elements_any_stream(const std::vector<D3DVERTEXELEMENT9> &elements, DWORD &fvf) {
  for (DWORD candidate : kFvfCandidates) {
    std::vector<D3DVERTEXELEMENT9> generated;
    if (!elements_from_fvf(candidate, generated))
      continue;
    if (generated.size() != elements.size())
      continue;
    // the stream, the offset and the order are the layout a declaration chose, not the format it names: a declaration
    // whose elements are the format's in another order (Wine visual.c texop_test's position, texcoord, colour) is the
    // same FVF, and the gather into the format's layout reorders it (bind_gathered_declaration_stream)
    std::vector<bool> matched(elements.size(), false);
    bool match = true;
    for (const D3DVERTEXELEMENT9 &want : generated) {
      bool found = false;
      for (size_t at = 0; at < elements.size(); at++) {
        const D3DVERTEXELEMENT9 &have = elements[at];
        if (matched[at] || want.Type != have.Type || want.Method != have.Method || want.Usage != have.Usage ||
            want.UsageIndex != have.UsageIndex)
          continue;
        matched[at] = true;
        found = true;
        break;
      }
      if (!found) {
        match = false;
        break;
      }
    }
    if (match) {
      fvf = candidate;
      return true;
    }
  }
  return false;
}

} // namespace mullion::d3d9
