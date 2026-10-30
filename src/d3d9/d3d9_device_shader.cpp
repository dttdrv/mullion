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
    if (element.Stream >= MTLD3D9Device::kMaxStreams) {
      ERR_ONCE("D3D9: a vertex declaration that reads stream ", (unsigned)element.Stream, " is past MaxStreams (",
               MTLD3D9Device::kMaxStreams, ").");
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
  // the alpha test's reference: one register, which a pixel program with the test reads
  if (SUCCEEDED(hr))
    hr = create(4 * sizeof(float), ps_alpha_constants_);
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
  HRESULT hr = S_OK;
  auto maybe = [&](bool &dirty, ID3D11Buffer *buffer, const void *data, UINT bytes) {
    if (FAILED(hr) || !dirty)
      return;
    hr = upload(buffer, data, bytes);
    if (SUCCEEDED(hr))
      dirty = false;
  };
  maybe(vs_f_dirty_, vs_constants_f_.Get(), vertex_constants_f_.data(),
        (UINT)(vertex_constants_f_.size() * sizeof(float)));
  maybe(vs_i_dirty_, vs_constants_i_.Get(), vertex_constants_i_.data(),
        (UINT)(vertex_constants_i_.size() * sizeof(int32_t)));
  maybe(vs_b_dirty_, vs_constants_b_.Get(), vertex_constants_b_.data(),
        (UINT)(vertex_constants_b_.size() * sizeof(WINBOOL)));
  maybe(ps_f_dirty_, ps_constants_f_.Get(), pixel_constants_f_.data(),
        (UINT)(pixel_constants_f_.size() * sizeof(float)));
  maybe(ps_i_dirty_, ps_constants_i_.Get(), pixel_constants_i_.data(),
        (UINT)(pixel_constants_i_.size() * sizeof(int32_t)));
  maybe(ps_b_dirty_, ps_constants_b_.Get(), pixel_constants_b_.data(),
        (UINT)(pixel_constants_b_.size() * sizeof(WINBOOL)));
  if (SUCCEEDED(hr)) {
    const DWORD alpha_ref = state_.render().alpha_ref();
    if (ps_alpha_dirty_ || uploaded_alpha_ref_ != alpha_ref) {
      // D3DRS_ALPHAREF as the 0-to-255 value D3D9's comparison is in, which is what the program reads
      const float alpha[4] = {(float)alpha_ref, 0.0f, 0.0f, 0.0f};
      hr = upload(ps_alpha_constants_.Get(), alpha, sizeof(alpha));
      if (SUCCEEDED(hr)) {
        ps_alpha_dirty_ = false;
        uploaded_alpha_ref_ = alpha_ref;
      }
    }
  }
  return hr;
}

// the viewport a draw is placed in and the clip rectangle it is cut by, which every draw of D3D9's runs under
void
MTLD3D9Device::bind_viewport_state() {
  D3D11_VIEWPORT viewport{(float)viewport_.X, (float)viewport_.Y, (float)viewport_.Width,
                          (float)viewport_.Height, viewport_.MinZ,          viewport_.MaxZ};
  context_->RSSetViewports(1, &viewport);
  // the viewport clips the draw, narrowed by the scissor rectangle while the scissor test is on (d3d9_device_targets.cpp)
  const RECT rect = clip_rect();
  const D3D11_RECT clip{rect.left, rect.top, rect.right, rect.bottom};
  context_->RSSetScissorRects(1, &clip);
}

// a draw of a game's own shaders: both stages are the game's, and the inputs come from the vertex declaration the draw
// reads. the programs are translated against that declaration the first time a draw uses the pair, and the D3D11
// shaders and layout the translation made are kept by the D3D9 shader objects.
HRESULT
MTLD3D9Device::prepare_shader_draw() {
  IMTLD3D9VertexDeclaration *declaration = declaration_inputs_.Get();
  ComPtr<MTLD3D9VertexDeclaration> fvf_declaration;
  if (!declaration) {
    std::vector<D3DVERTEXELEMENT9> elements;
    if (!elements_from_fvf(fvf_, elements)) {
      ERR_ONCE("D3D9: a draw with a game's shader needs the vertex declaration it reads, or an FVF this build places.");
      return D3DERR_INVALIDCALL;
    }
    fvf_declaration.Attach(new MTLD3D9VertexDeclaration(this, std::move(elements)));
    declaration = fvf_declaration.Get();
  }

  if (!vertex_shader_) {
    if (!pixel_shader_ || !FixedFunctionPipeline::supports(fvf_)) {
      ERR_ONCE("D3D9: a draw with a pixel shader and the fixed-function vertex pipeline needs an FVF this build "
               "transforms.");
      return D3DERR_INVALIDCALL;
    }
    HRESULT hr = prepare_fixed_function_draw();
    if (FAILED(hr))
      return hr;
    ComPtr<IMTLD3D9PixelShader> pixel;
    if (FAILED(pixel_shader_->QueryInterface(
            IID_IMTLD3D9PixelShader, reinterpret_cast<void **>(pixel.GetAddressOf())
        )))
      return D3DERR_INVALIDCALL;
    AlphaTest alpha;
    alpha.enabled = state_.render().alpha_test_enabled();
    alpha.func = state_.render().alpha_func();
    ComPtr<ID3D11PixelShader> compiled_pixel;
    hr = pixel->compiled(d3d11_device_.Get(), alpha, &compiled_pixel);
    if (FAILED(hr))
      return hr;
    context_->PSSetShader(compiled_pixel.Get(), nullptr, 0);
    if (pixel->samples()) {
      ID3D11ShaderResourceView *views[kTextureStages]{};
      ID3D11SamplerState *samplers[kTextureStages]{};
      for (UINT stage = 0; stage < kTextureStages; stage++) {
        views[stage] = texture_sources_[stage] ? texture_sources_[stage]->view() : nullptr;
        samplers[stage] = sampler_states_[stage].state(d3d11_device_.Get());
      }
      HRESULT tex_hr = ensure_default_texture();
      if (FAILED(tex_hr))
        return tex_hr;
      for (UINT stage = 0; stage < kTextureStages; stage++)
        if (!views[stage])
          views[stage] = default_texture_view_.Get();
      context_->PSSetShaderResources(0, kTextureStages, views);
      context_->PSSetSamplers(0, kTextureStages, samplers);
    }
    return S_OK;
  }

  ComPtr<ID3D11PixelShader> compiled_pixel;
  ID3D11PixelShader *bound_pixel = nullptr;
  std::vector<PixelInputSlot> pixel_inputs;
  bool samples = false;
  if (pixel_shader_) {
    ComPtr<IMTLD3D9PixelShader> pixel;
    if (FAILED(pixel_shader_->QueryInterface(
            IID_IMTLD3D9PixelShader, reinterpret_cast<void **>(pixel.GetAddressOf())
        )))
      return D3DERR_INVALIDCALL;
    AlphaTest alpha;
    alpha.enabled = state_.render().alpha_test_enabled();
    alpha.func = state_.render().alpha_func();
    HRESULT hr = pixel->compiled(d3d11_device_.Get(), alpha, &compiled_pixel);
    if (FAILED(hr))
      return hr;
    bound_pixel = compiled_pixel.Get();
    pixel_inputs = pixel->pixel_inputs();
    samples = pixel->samples();
  } else {
    // mixed: a game's vertex shader, the fixed-function pixel shader. COLOR0 is interpolator 1, where oD0 is written
    if ((state_.render().alpha_test_enabled() && state_.render().alpha_func() != D3DCMP_ALWAYS) ||
        state_.render().fog_enabled()) {
      ERR_ONCE("D3D9: a mixed draw under D3DRS_ALPHATESTENABLE or fog is not drawn yet: the mixed path binds the "
               "untextured colour pixel shader without the extra constants those states read.");
      return D3DERR_INVALIDCALL;
    }
    bound_pixel = fixed_function_.colour_pixel_shader(state_.render().flat_shaded());
    if (!bound_pixel) {
      ERR_ONCE("D3D9: the fixed-function pixel shader a mixed draw needs is not made.");
      return D3DERR_INVALIDCALL;
    }
    PixelInputSlot colour;
    colour.semantic = "COLOR";
    colour.index = 0;
    colour.reg = 1;
    pixel_inputs.push_back(std::move(colour));
  }

  // the vertex shader is translated against the pixel shader's inputs: the two link by register, so its outputs are
  // written where this shader reads them
  ComPtr<IMTLD3D9VertexShader> vertex;
  if (FAILED(vertex_shader_->QueryInterface(
          IID_IMTLD3D9VertexShader, reinterpret_cast<void **>(vertex.GetAddressOf())
      )))
    return D3DERR_INVALIDCALL;
  ComPtr<ID3D11VertexShader> vertex_shader;
  ComPtr<ID3D11InputLayout> layout;
  HRESULT hr = vertex->compiled(
      d3d11_device_.Get(), declaration->elements(), declaration->inputs(), pixel_inputs, declaration->layout(),
      &vertex_shader, &layout
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
  context_->PSSetShader(bound_pixel, nullptr, 0);

  // the textures a sampling pixel shader reads: each stage's texture, or the default texture a stage with none has,
  // sampled through the sampler that stage's sampler states make. a program names a sampler by its stage's number
  // (`dcl_2d s#` and `texld r#, t#, s#`), and both are bound to the registers it declared
  if (samples || vertex->samples()) {
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
    if (vertex->samples()) {
      context_->VSSetShaderResources(0, kTextureStages, views);
      context_->VSSetSamplers(0, kTextureStages, samplers);
    }
  }
  ID3D11Buffer *vertex_buffers[3] = {vs_constants_f_.Get(), vs_constants_i_.Get(), vs_constants_b_.Get()};
  context_->VSSetConstantBuffers(0, 3, vertex_buffers);
  if (pixel_shader_) {
    ID3D11Buffer *pixel_buffers[4] = {
        ps_constants_f_.Get(), ps_constants_i_.Get(), ps_constants_b_.Get(), ps_alpha_constants_.Get()};
    context_->PSSetConstantBuffers(0, 4, pixel_buffers);
  }
  return S_OK;
}

} // namespace mullion::d3d9
