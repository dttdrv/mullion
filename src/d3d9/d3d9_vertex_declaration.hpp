// a vertex declaration: the elements a draw's vertices are made of, as D3D11's input layout and the vertex shader's
// input signature both read them. D3D9 gives a shader its inputs by register (v0, v1, ...), and the declaration is what
// says which element of a vertex feeds which register, so a draw translates its shader against the declaration it has
// (d3d9_device_draw.cpp).
#pragma once
#include "d3d9_common.hpp"
#include "d3d9_shader_translator.hpp"
#include <string>
#include <vector>

namespace mullion::d3d9 {

// what the device asks a declaration of this build for
struct IMTLD3D9VertexDeclaration {
  virtual ULONG STDMETHODCALLTYPE AddRef() = 0;
  virtual ULONG STDMETHODCALLTYPE Release() = 0;
  virtual const std::vector<D3DVERTEXELEMENT9> &elements() = 0;
  // the shader's view of it: the register each element feeds, and what it means in a signature
  virtual const std::vector<VertexInput> &inputs() = 0;
  // the layout a D3D11 draw reads it with, whose semantic names are the declaration's usages
  virtual const std::vector<D3D11_INPUT_ELEMENT_DESC> &layout() = 0;
  // the object the device keeps as SetFVF's declaration for a format: it frees it with its cache, and the
  // application's last reference leaves the object alive (D3D9's own ownership and counts)
  virtual void hold_device_cache() = 0;
  virtual void free_device_cache() = 0;
};

extern const IID IID_IMTLD3D9VertexDeclaration;

class MTLD3D9VertexDeclaration final : private Object9,
                                       public IDirect3DVertexDeclaration9,
                                       public IMTLD3D9VertexDeclaration {
public:
  // `counted_device` is false for the declarations SetFVF caches: the device owns those, and only a declaration the
  // application made adds a reference to the device's count
  MTLD3D9VertexDeclaration(IDirect3DDevice9 *device, std::vector<D3DVERTEXELEMENT9> elements,
                           bool counted_device = true);

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override;
  ULONG STDMETHODCALLTYPE AddRef() override { return Object9::AddRef(); }
  ULONG STDMETHODCALLTYPE Release() override { return Object9::Release(); }

  // IDirect3DVertexDeclaration9
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **device) override;
  HRESULT STDMETHODCALLTYPE GetDeclaration(D3DVERTEXELEMENT9 *elements, UINT *count) override;

  // IMTLD3D9VertexDeclaration
  const std::vector<D3DVERTEXELEMENT9> &elements() override { return elements_; }
  const std::vector<VertexInput> &inputs() override { return inputs_; }
  const std::vector<D3D11_INPUT_ELEMENT_DESC> &layout() override { return layout_; }
  void hold_device_cache() override;
  void free_device_cache() override;

private:
  // the device that made the declaration is held through a private reference (DeviceChildRef)
  IDirect3DDevice9 *device_ = nullptr;
  DeviceChildRef device_ref_;
  std::vector<D3DVERTEXELEMENT9> elements_;
  std::vector<VertexInput> inputs_;
  // the semantic names the layout points into, and the layout itself
  std::vector<std::string> names_;
  std::vector<D3D11_INPUT_ELEMENT_DESC> layout_;
};

// the DXGI format an element's D3DDECLTYPE is read as, DXGI_FORMAT_UNKNOWN for a type this build has no format for
DXGI_FORMAT to_dxgi_declaration_format(UINT type);
// SetFVF's format as the declaration a programmable draw reads (Microsoft Learn, D3DFVF)
bool elements_from_fvf(DWORD fvf, std::vector<D3DVERTEXELEMENT9> &elements);
// Reverse of elements_from_fvf for declarations this build's fixed-function path draws: SetVertexDeclaration
// clears GetFVF, but a draw still transforms through the matching FVF (Wine device.c test_draw_primitive).
bool fvf_from_elements(const std::vector<D3DVERTEXELEMENT9> &elements, DWORD &fvf);
// The same, for a declaration whose elements are split over streams or laid out with their own offsets: the format is
// the one its element types and usages name, and a draw gathers the vertices into that format's layout.
bool fvf_from_elements_any_stream(const std::vector<D3DVERTEXELEMENT9> &elements, DWORD &fvf);
// The bytes one element of `type` takes, as D3D9's declaration conversion lays them out.
UINT declaration_element_size(BYTE type);

} // namespace mullion::d3d9
