// the draw a game's own shaders make: the vertex declaration it reads its vertices through, the translated programs
// (d3d9_shader_translator.cpp), and the constant buffers its float, integer and boolean tables are uploaded to. the
// geometry, the render states and the targets the draw reads are d3d9_device_draw.cpp's and d3d9_device_targets.cpp's.
#include "d3d9_device.hpp"
#include <vector>

namespace mullion::d3d9 {

namespace {

// D3D9's tables: 256 float4 registers, 16 int4 and 16 booleans
constexpr UINT kFloatConstants = 256;
constexpr UINT kIntConstants = 16;
constexpr UINT kBoolConstants = 16;

// D3D9's end-of-declaration marker, which is not the end of an array (D3DDECL_END() is a brace initialiser, which a
// value's type cannot be given in C++)
D3DVERTEXELEMENT9 end_of_declaration() {
  D3DVERTEXELEMENT9 element{};
  element.Stream = 0xFF;
  element.Type = D3DDECLTYPE_UNUSED;
  return element;
}

} // namespace

HRESULT
MTLD3D9Device::CreateVertexDeclaration(
    const D3DVERTEXELEMENT9 *elements, IDirect3DVertexDeclaration9 **declaration
) {
  if (!elements || !declaration)
    return D3DERR_INVALIDCALL;
  *declaration = nullptr;

  std::vector<D3DVERTEXELEMENT9> list;
  // D3D9's declarations have at most 64 elements; the shader inputs this build places are the first sixteen registers
  for (UINT at = 0; at < 64; at++) {
    const D3DVERTEXELEMENT9 &element = elements[at];
    if (element.Stream == 0xFF)
      break;
    if (element.Stream != 0) {
      ERR_ONCE("D3D9: a vertex declaration that reads a stream but 0 is not implemented yet (instancing and "
               "multi-stream draws are not).");
      return D3DERR_INVALIDCALL;
    }
    if (!to_dxgi_declaration_format(element.Type) || !declaration_type_mask(element.Type)) {
      ERR_ONCE("D3D9: a vertex declaration element of a type this build has no input layout format for: ",
               (uint32_t)element.Type);
      return D3DERR_INVALIDCALL;
    }
    list.push_back(element);
  }
  if (list.empty()) {
    ERR_ONCE("D3D9: a vertex declaration with no elements was refused.");
    return D3DERR_INVALIDCALL;
  }
  if (list.size() > 16) {
    ERR_ONCE("D3D9: a vertex declaration of more than sixteen elements is not placed by this build's shaders.");
    return D3DERR_INVALIDCALL;
  }

  ComPtr<MTLD3D9VertexDeclaration> result;
  result.Attach(new MTLD3D9VertexDeclaration(static_cast<IDirect3DDevice9 *>(this), std::move(list)));
  *declaration = result.Detach();
  return S_OK;
}

HRESULT
MTLD3D9Device::SetVertexDeclaration(IDirect3DVertexDeclaration9 *declaration) {
  if (!declaration) {
    declaration_.Reset();
    declaration_inputs_.Reset();
    if (recording_)
      record(StateKey{StateKey::Declaration, 0, 0});
    return S_OK;
  }
  ComPtr<IMTLD3D9VertexDeclaration> ours;
  if (FAILED(declaration->QueryInterface(
          IID_IMTLD3D9VertexDeclaration, reinterpret_cast<void **>(ours.GetAddressOf())
      )))
    return D3DERR_INVALIDCALL;
  declaration_ = declaration;
  declaration_inputs_ = ours;
  // a declaration and an FVF are exclusive, as D3D9's are: the last one set is the draw's
  fvf_ = 0;
  if (recording_)
    record(StateKey{StateKey::Declaration, 0, 0});
  return S_OK;
}

HRESULT
MTLD3D9Device::GetVertexDeclaration(IDirect3DVertexDeclaration9 **declaration) {
  if (!declaration)
    return D3DERR_INVALIDCALL;
  *declaration = declaration_.Get();
  if (*declaration)
    (*declaration)->AddRef();
  return S_OK;
}

// the six constant buffers the two stages' tables are read from
HRESULT
MTLD3D9Device::ensure_shader_constants() {
  auto create = [&](UINT bytes, ComPtr<ID3D11Buffer> &buffer) -> HRESULT {
    if (buffer)
      return S_OK;
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = bytes;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return d3d11_device_->CreateBuffer(&desc, nullptr, &buffer);
  };
  HRESULT hr = create(kFloatConstants * 4 * sizeof(float), vs_constants_f_);
  if (SUCCEEDED(hr))
    hr = create(kIntConstants * 4 * sizeof(int32_t), vs_constants_i_);
  if (SUCCEEDED(hr))
    hr = create(kBoolConstants * sizeof(WINBOOL), vs_constants_b_);
  if (SUCCEEDED(hr))
    hr = create(kFloatConstants * 4 * sizeof(float), ps_constants_f_);
  if (SUCCEEDED(hr))
    hr = create(kIntConstants * 4 * sizeof(int32_t), ps_constants_i_);
  if (SUCCEEDED(hr))
    hr = create(kBoolConstants * sizeof(WINBOOL), ps_constants_b_);
  if (FAILED(hr)) {
    ERR_ONCE("D3D9: the constant buffers a game's shader reads are not made: 0x", std::hex, hr);
  }
  return hr;
}

// the tables as the shader's constant buffers: D3D9's float registers are one float4 each, its integers one int4 each
// and its booleans one flag each
HRESULT
MTLD3D9Device::upload_shader_constants() {
  auto upload = [&](ID3D11Buffer *buffer, const void *data, UINT bytes) -> HRESULT {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
      return D3DERR_INVALIDCALL;
    memcpy(mapped.pData, data, bytes);
    context_->Unmap(buffer, 0);
    return S_OK;
  };
  HRESULT hr = upload(vs_constants_f_.Get(), vertex_constants_f_.data(), (UINT)(vertex_constants_f_.size() * sizeof(float)));
  if (SUCCEEDED(hr))
    hr = upload(vs_constants_i_.Get(), vertex_constants_i_.data(), (UINT)(vertex_constants_i_.size() * sizeof(int32_t)));
  if (SUCCEEDED(hr))
    hr = upload(vs_constants_b_.Get(), vertex_constants_b_.data(), (UINT)(vertex_constants_b_.size() * sizeof(WINBOOL)));
  if (SUCCEEDED(hr))
    hr = upload(ps_constants_f_.Get(), pixel_constants_f_.data(), (UINT)(pixel_constants_f_.size() * sizeof(float)));
  if (SUCCEEDED(hr))
    hr = upload(ps_constants_i_.Get(), pixel_constants_i_.data(), (UINT)(pixel_constants_i_.size() * sizeof(int32_t)));
  if (SUCCEEDED(hr))
    hr = upload(ps_constants_b_.Get(), pixel_constants_b_.data(), (UINT)(pixel_constants_b_.size() * sizeof(WINBOOL)));
  return hr;
}

// the viewport a draw is placed in and the clip rectangle it is cut by, which every draw of D3D9's runs under
void
MTLD3D9Device::bind_viewport_state() {
  D3D11_VIEWPORT viewport{(float)viewport_.X, (float)viewport_.Y, (float)viewport_.Width,
                          (float)viewport_.Height, viewport_.MinZ,          viewport_.MaxZ};
  context_->RSSetViewports(1, &viewport);
  const D3D11_RECT clip{(LONG)viewport_.X, (LONG)viewport_.Y, (LONG)(viewport_.X + viewport_.Width),
                        (LONG)(viewport_.Y + viewport_.Height)};
  context_->RSSetScissorRects(1, &clip);
}

// a draw of a game's own shaders: both stages are the game's, and the inputs come from the vertex declaration the draw
// reads. the programs are translated against that declaration the first time a draw uses the pair, and the D3D11
// shaders and layout the translation made are kept by the D3D9 shader objects.
HRESULT
MTLD3D9Device::prepare_shader_draw() {
  if (!vertex_shader_) {
    ERR_ONCE("D3D9: a draw with a pixel shader and the fixed-function vertex pipeline is not implemented yet.");
    return D3DERR_INVALIDCALL;
  }
  if (!pixel_shader_) {
    ERR_ONCE("D3D9: a draw with a vertex shader and the fixed-function pixel pipeline is not implemented yet.");
    return D3DERR_INVALIDCALL;
  }
  if (!declaration_inputs_) {
    ERR_ONCE("D3D9: a draw with a game's shader needs the vertex declaration it reads (SetVertexDeclaration); a shader "
             "fed by SetFVF's format is not implemented yet.");
    return D3DERR_INVALIDCALL;
  }

  ComPtr<IMTLD3D9PixelShader> pixel;
  if (FAILED(pixel_shader_->QueryInterface(
          IID_IMTLD3D9PixelShader, reinterpret_cast<void **>(pixel.GetAddressOf())
      )))
    return D3DERR_INVALIDCALL;
  ComPtr<ID3D11PixelShader> pixel_shader;
  HRESULT hr = pixel->compiled(d3d11_device_.Get(), &pixel_shader);
  if (FAILED(hr))
    return hr;

  // the vertex shader is translated against the pixel shader's inputs: the two link by register, so its outputs are
  // written where this shader reads them
  ComPtr<IMTLD3D9VertexShader> vertex;
  if (FAILED(vertex_shader_->QueryInterface(
          IID_IMTLD3D9VertexShader, reinterpret_cast<void **>(vertex.GetAddressOf())
      )))
    return D3DERR_INVALIDCALL;
  ComPtr<ID3D11VertexShader> vertex_shader;
  ComPtr<ID3D11InputLayout> layout;
  hr = vertex->compiled(
      d3d11_device_.Get(), declaration_inputs_->elements(), declaration_inputs_->inputs(), pixel->pixel_inputs(),
      declaration_inputs_->layout(), &vertex_shader, &layout
  );
  if (FAILED(hr))
    return hr;

  hr = ensure_shader_constants();
  if (FAILED(hr))
    return hr;
  hr = upload_shader_constants();
  if (FAILED(hr))
    return hr;

  context_->IASetInputLayout(layout.Get());
  context_->VSSetShader(vertex_shader.Get(), nullptr, 0);
  context_->PSSetShader(pixel_shader.Get(), nullptr, 0);

  // the textures a sampling pixel shader reads: each stage's texture, or the default texture a stage with none has,
  // sampled through the sampler that stage's sampler states make. a program names a sampler by its stage's number
  // (`dcl_2d s#` and `texld r#, t#, s#`), and both are bound to the registers it declared
  if (pixel->samples()) {
    hr = ensure_default_texture();
    if (FAILED(hr))
      return hr;
    ID3D11ShaderResourceView *views[kTextureStages] = {};
    ID3D11SamplerState *samplers[kTextureStages] = {};
    for (UINT stage = 0; stage < kTextureStages; stage++) {
      views[stage] = texture_sources_[stage] ? texture_sources_[stage]->view() : default_texture_view_.Get();
      samplers[stage] = sampler_states_[stage].state(d3d11_device_.Get());
      if (!samplers[stage]) {
        ERR_ONCE("D3D9: the sampler state stage ", stage, " holds makes no D3D11 sampler, so a sampling draw has "
                 "none.");
        return D3DERR_INVALIDCALL;
      }
    }
    context_->PSSetShaderResources(0, kTextureStages, views);
    context_->PSSetSamplers(0, kTextureStages, samplers);
  }
  ID3D11Buffer *vertex_buffers[3] = {vs_constants_f_.Get(), vs_constants_i_.Get(), vs_constants_b_.Get()};
  context_->VSSetConstantBuffers(0, 3, vertex_buffers);
  ID3D11Buffer *pixel_buffers[3] = {ps_constants_f_.Get(), ps_constants_i_.Get(), ps_constants_b_.Get()};
  context_->PSSetConstantBuffers(0, 3, pixel_buffers);
  return S_OK;
}

} // namespace mullion::d3d9
