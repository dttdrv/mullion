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

MTLD3D9VertexDeclaration::MTLD3D9VertexDeclaration(IDirect3DDevice9 *device, std::vector<D3DVERTEXELEMENT9> elements)
    : device_(device), device_ref_(device), elements_(std::move(elements)) {
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
  const UINT bytes = (UINT)((elements_.size() + 1) * sizeof(D3DVERTEXELEMENT9));
  if (!elements) {
    *count = bytes;
    return S_OK;
  }
  if (*count < bytes) {
    *count = bytes;
    return D3DERR_INVALIDCALL;
  }
  memcpy(elements, elements_.data(), elements_.size() * sizeof(D3DVERTEXELEMENT9));
  D3DVERTEXELEMENT9 end{};
  end.Stream = 0xFF;
  end.Type = D3DDECLTYPE_UNUSED;
  elements[elements_.size()] = end;
  *count = bytes;
  return S_OK;
}

} // namespace mullion::d3d9
