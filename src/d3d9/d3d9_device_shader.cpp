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

// FOGENABLE with FOGTABLEMODE, FOGVERTEXMODE and an optional oFog (Wine visual.c fog_with_shader_test).
// With a vs_1_1 program that does not write oFog, FOGVERTEXMODE is ignored and the pixel is fully fogged when
// FOGTABLEMODE is NONE (the "fog coord isn't available" rows in that test). A vs_2_0 or later program that omits
// oFog leaves the coordinate unavailable without that quirk, so it does not fog (tests/d3d9/d3d9_extra.cpp).
FogState resolve_fog_state(
    bool enabled, DWORD table_mode, DWORD vertex_mode, bool writes_fog, UINT vertex_shader_major
) {
  FogState fog;
  if (!enabled)
    return fog;
  if (table_mode != D3DFOG_NONE) {
    fog.source = FogSource::Table;
    fog.mode = table_mode;
  } else if (writes_fog) {
    fog.source = FogSource::FogCoord;
  } else if (vertex_shader_major) {
    // the old vs_1_1 reference fogs fully when oFog is absent; vs_2_0+ leaves it unavailable and unfogged
    if (vertex_shader_major < 2)
      fog.source = FogSource::Full;
  } else if (vertex_mode != D3DFOG_NONE) {
    fog.source = FogSource::VertexZ;
    fog.mode = vertex_mode;
  }
  // with neither a fog viewpoint member nor vertex fog, D3DRS_FOGENABLE fog nothing (Microsoft Learn, D3DRS_FOGTABLEMODE)
  return fog;
}

// D3DTSS_TEXTURETRANSFORMFLAGS with PROJECTED: which texcoord component divides .xy for ps_1.x `tex`
template <typename Stages>
BumpEnvState resolve_bump_env(const Stages &stages) {
  BumpEnvState bump;
  for (UINT stage = 0; stage < BumpEnvState::kStages; stage++)
    stages[stage].bump_env(bump.matrix[stage], bump.lscale[stage], bump.loffset[stage]);
  return bump;
}

// which component of the stage's coordinate divides its x and y, for a D3DTTFF_PROJECTED stage. The rule is the one
// Wine's own fixed-function pipeline encodes (wined3d glsl_shader.c: COUNT3 samples .xyz through a projected sampler,
// everything else .xy) and the one visual.c projected_textures_test records:
//  - a game's own vertex shader writes oT0 whole, and D3D9 divides by its w whatever the flags name (the test's
//    programmable runs are all labelled "like COUNT4 | PROJECTED");
//  - a fixed-function vertex's COUNT3 divides by z and COUNT4 by w;
//  - PROJECTED alone, or an invalid count, means the count the coordinate carries: z for a three-component coordinate
//    and w for a four-component one;
//  - a count the declaration cannot supply leaves the stage unprojected (COUNT3|PROJECTED over a FLOAT2 coordinate),
//    as does COUNT1 and COUNT2, whose projected sample is a two-component one.
TexProjectState resolve_tex_project(DWORD flags, DXGI_FORMAT stage0_dxgi, UINT declared_width, bool game_vertex) {
  TexProjectState project;
  if ((flags & D3DTTFF_PROJECTED) || flags == 0xffffffffu) {
    if (game_vertex) {
      project.divide_component = 3;
    } else {
      const UINT width = declared_width ? std::min<UINT>(declared_width, 4u) : 4u;
      const UINT named = flags & 0xFu;
      UINT count;
      if (flags == 0xffffffffu || named == 0 || named > 4)
        count = width;   // the coordinate's own last component
      else
        count = named;
      if (count > width || count < 3)
        count = 0;       // a component the vertex does not carry, or a two-component projected sample: no divide
      project.divide_component = count ? count - 1 : 0;   // COUNT3→2 (.z), COUNT4→3 (.w)
    }
  }
  // Two-channel signed bump formats: D3D9 samples with A=1 (Wine visual.c signed format / emboss tests)
  project.force_alpha_one =
      stage0_dxgi == DXGI_FORMAT_R8G8_SNORM || stage0_dxgi == DXGI_FORMAT_R16G16_SNORM;
  return project;
}

// the components the declaration's first texture coordinate set carries, 0 when it declares none
UINT declaration_texcoord_width(const std::vector<D3DVERTEXELEMENT9> &elements) {
  for (const auto &element : elements) {
    if (element.Usage == D3DDECLUSAGE_TEXCOORD && element.UsageIndex == 0) {
      const UINT mask = declaration_type_mask(element.Type);
      UINT width = 0;
      for (UINT bit = 0; bit < 4; bit++)
        if (mask & (1u << bit))
          width++;
      return width;
    }
  }
  return 0;
}

DXGI_FORMAT stage0_dxgi_of(IMTLD3D9Texture *source) {
  if (!source || !source->texture())
    return DXGI_FORMAT_UNKNOWN;
  D3D11_TEXTURE2D_DESC desc{};
  source->texture()->GetDesc(&desc);
  return desc.Format;
}

// whether the stage's bound texture is a depth-stencil one: a depth texture is made typeless (d3d9_device_texture.cpp
// to_typeless_depth_format), and a pixel shader of model 2.0 and above comparison-samples it (d3d9_shader.hpp
// ShadowState; Wine wined3d glsl_is_shadow_sampler)
bool stage_binds_depth(IMTLD3D9Texture *source) {
  switch (stage0_dxgi_of(source)) {
  case DXGI_FORMAT_R16_TYPELESS:
  case DXGI_FORMAT_R24G8_TYPELESS:
  case DXGI_FORMAT_R32_TYPELESS:
    return true;
  default:
    return false;
  }
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
  bool terminated = false;
  // D3D9's declarations have at most 64 elements; the shader inputs this build places are the first sixteen registers
  for (UINT at = 0; at < 64; at++) {
    const D3DVERTEXELEMENT9 &element = elements[at];
    if (element.Stream == 0xFF) {
      terminated = true;
      break;
    }
    if (element.Stream >= MTLD3D9Device::kMaxStreams) {
      ERR_ONCE("D3D9: a vertex declaration that reads stream ", (unsigned)element.Stream, " is past MaxStreams (",
               MTLD3D9Device::kMaxStreams, ").");
      return D3DERR_INVALIDCALL;
    }
    if (element.Type == D3DDECLTYPE_UNUSED || !to_dxgi_declaration_format(element.Type) ||
        !declaration_type_mask(element.Type)) {
      // D3DDECLTYPE_UNUSED is only the terminator; an element that carries it is refused with E_FAIL, as D3D9 does
      ERR_ONCE("D3D9: a vertex declaration element of a type this build has no input layout format for: ",
               (uint32_t)element.Type);
      return E_FAIL;
    }
    // every element starts on a DWORD boundary: D3D9 refuses offset 17 to 19 and accepts 16 and 20
    if (element.Offset % 4) {
      ERR_ONCE("D3D9: a vertex declaration element is not DWORD aligned: ", (unsigned)element.Offset);
      return E_FAIL;
    }
    list.push_back(element);
  }
  // An empty declaration (only D3DDECL_END) is valid state: it is what a declaration with no FVF mapping can be, and
  // Wine's device conformance test creates one and expects SetVertexDeclaration/GetFVF to round-trip it.
  if (!terminated) {
    ERR_ONCE("D3D9: a vertex declaration without a D3DDECL_END terminator was refused.");
    return D3DERR_INVALIDCALL;
  }
  if (list.size() > 16) {
    ERR_ONCE("D3D9: a vertex declaration of more than sixteen elements is not placed by this build's shaders.");
    return D3DERR_INVALIDCALL;
  }

  ComPtr<MTLD3D9VertexDeclaration> result;
  result.Attach(new MTLD3D9VertexDeclaration(static_cast<IDirect3DDevice9 *>(this), std::move(list), true));
  *declaration = result.Detach();
  return S_OK;
}

HRESULT
MTLD3D9Device::SetVertexDeclaration(IDirect3DVertexDeclaration9 *declaration) {
  if (!declaration) {
    declaration_ = nullptr;
    declaration_inputs_ = nullptr;
    if (recording_)
      record(StateKey{StateKey::Declaration, 0, 0});
    return S_OK;
  }
  ComPtr<IMTLD3D9VertexDeclaration> ours;
  if (FAILED(declaration->QueryInterface(
          IID_IMTLD3D9VertexDeclaration, reinterpret_cast<void **>(ours.GetAddressOf())
      )))
    return D3DERR_INVALIDCALL;
  // An explicit declaration is the application's object: D3D9 does not take a COM reference here (Wine's device test
  // checks the count is unchanged). The device keeps the pointer and the private interface, and GetVertexDeclaration
  // is what AddRefs it.
  declaration_ = declaration;
  declaration_inputs_ = ours.Get();
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
  *declaration = declaration_;
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
  // alpha ref + fog params + fog colour (three float4s) for a pixel program that carries either
  if (SUCCEEDED(hr))
    hr = create(3 * 4 * sizeof(float), ps_alpha_constants_);
  // the six user clip planes a clipped vertex program reads at b3
  if (SUCCEEDED(hr))
    hr = create(DeviceState::kClipPlanes * 4 * sizeof(float), vs_clip_planes_);
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
  if (SUCCEEDED(hr) && ps_alpha_constants_) {
    // Always refresh b3: ALPHAREF (low 8 bits) plus fog start/end/density/colour. Small buffer; fog and alpha both
    // change between draws in Wine visual.c without a separate dirty bit for every FOG* state.
    const DWORD alpha_ref = state_.render().alpha_ref();
    float block[12] = {};
    block[0] = (float)(alpha_ref & 0xffu);
    block[4] = state_.render().fog_start();
    block[5] = state_.render().fog_end();
    block[6] = state_.render().fog_density();
    const DWORD fog_color = state_.render().fog_color();
    block[8] = ((fog_color >> 16) & 0xFF) / 255.0f;
    block[9] = ((fog_color >> 8) & 0xFF) / 255.0f;
    block[10] = (fog_color & 0xFF) / 255.0f;
    block[11] = ((fog_color >> 24) & 0xFF) / 255.0f;
    hr = upload(ps_alpha_constants_.Get(), block, sizeof(block));
    if (SUCCEEDED(hr)) {
      ps_alpha_dirty_ = false;
      ps_fog_dirty_ = false;
      uploaded_alpha_ref_ = alpha_ref;
    }
  }
  return hr;
}

// the viewport a draw is placed in and the clip rectangle it is cut by, which every draw of D3D9's runs under
void
MTLD3D9Device::bind_viewport_state() {
  // the half pixel D3D9's integer pixel centers need on a rasterizer that samples at half-integer ones, and the
  // depth range's degenerate-range rule (d3d9_common.hpp, to_d3d11_viewport)
  D3D11_VIEWPORT viewport = to_d3d11_viewport(viewport_, viewport_.MinZ, viewport_.MaxZ);
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
MTLD3D9Device::ensure_fog_passthrough_ps() {
  if (fog_passthrough_ps_)
    return S_OK;
  // Wine visual.c fog_with_shader_test pixel_shader_code: ps_1_1 mov r0, v0
  static const DWORD kPassthrough[] = {
      0xffff0101u, 0x00000001u, 0x800f0000u, 0x90e40000u, 0x0000ffffu,
  };
  return create_pixel_shader(kPassthrough, &fog_passthrough_ps_, false);
}

HRESULT
MTLD3D9Device::ensure_pos_tex_passthrough_vs() {
  if (pos_tex_passthrough_vs_)
    return S_OK;
  // Wine visual.c projected_textures_test vertex_shader: vs_1_1 mov oPos,v0; mov oT0,v1
  static const DWORD kPassthrough[] = {
      0xfffe0101u, 0x0000001fu, 0x80000000u, 0x900f0000u, 0x0000001fu, 0x80000005u, 0x900f0001u,
      0x00000001u, 0xc00f0000u, 0x90e40000u, 0x00000001u, 0xe00f0000u, 0x90e40001u, 0x0000ffffu,
  };
  return create_vertex_shader(kPassthrough, &pos_tex_passthrough_vs_, false);
}

HRESULT
MTLD3D9Device::ensure_declaration_passthrough_vs(IMTLD3D9VertexDeclaration &declaration,
                                                 IDirect3DVertexShader9 **shader) {
  const std::vector<D3DVERTEXELEMENT9> &elements = declaration.elements();
  const auto cached = declaration_passthroughs_.find(elements);
  if (cached != declaration_passthroughs_.end()) {
    *shader = cached->second.Get();
    (*shader)->AddRef();
    return S_OK;
  }
  // the position and every colour, coordinate and other usage the declaration supplies, as a vs_3_0 the translator
  // writes: each input declares the element's usage at the element's register, the position passes through as
  // SV_Position (a transformed vertex's POSITIONT is already screen-space), and every other element is handed to the
  // pixel stage under the semantic its usage spells. A usage the pixel stage reads none of is still written, at the
  // free slot the translator picks (read_slot's fallback), so the program is one per declaration and not per draw
  std::vector<DWORD> program;
  const auto push = [&program](std::initializer_list<DWORD> tokens) { program.insert(program.end(), tokens); };
  push({0xfffe0300u});
  // the input declarations: one per element, the usage the element carries at the register the element sits at
  for (UINT at = 0; at < elements.size(); at++)
    push({0x0200001fu, 0x80000000u | elements[at].Usage | (elements[at].UsageIndex << 16),
          0x900f0000u | at});
  // the outputs, in the order the elements carry them: the position first, then what the other usages name
  struct Out {
    UINT usage, index, reg; // the o# the mov writes
  };
  std::vector<Out> outputs;
  UINT next_reg = 0;
  for (UINT at = 0; at < elements.size(); at++) {
    const D3DVERTEXELEMENT9 &element = elements[at];
    if (element.Stream == 0xFF)
      break;
    // both positions pass through as the output the rasteriser reads: a POSITIONT the program maps through the
    // viewport (d3d9_shader_translator.cpp), and a POSITION whose fixed-function transform is the identity the
    // default matrices make, so the clip-space value the vertex carries is what the rasteriser takes (Wine
    // visual.c projected_textures_test's D3DFVF_XYZ draws with wide coordinates)
    UINT usage = element.Usage == D3DDECLUSAGE_POSITIONT ? D3DDECLUSAGE_POSITION : element.Usage;
    if (std::any_of(outputs.begin(), outputs.end(), [&](const Out &out) {
          return out.usage == usage && out.index == element.UsageIndex;
        }))
      continue; // two elements of one usage (two streams' worth of the same semantic) read at the first
    if (next_reg > 10)
      break; // vs_3_0's output register file is o0..o10
    outputs.push_back({usage, element.UsageIndex, next_reg++});
  }
  for (const Out &out : outputs)
    push({0x0200001fu, 0x80000000u | out.usage | (out.index << 16), 0xe00f0000u | out.reg});
  // the moves: each output reads the element it was named from, whole (the translator completes a partial one)
  UINT at_element = 0;
  for (UINT at = 0; at < elements.size(); at++) {
    const D3DVERTEXELEMENT9 &element = elements[at];
    if (element.Stream == 0xFF)
      break;
    UINT usage = element.Usage == D3DDECLUSAGE_POSITIONT ? D3DDECLUSAGE_POSITION : element.Usage;
    auto named = std::find_if(outputs.begin(), outputs.end(), [&](const Out &out) {
      return out.usage == usage && out.index == element.UsageIndex;
    });
    if (named == outputs.end())
      continue;
    push({0x02000001u, 0xe00f0000u | named->reg, 0x90e40000u | at});
    (void)at_element;
  }
  push({0x0000ffffu});

  ComPtr<IDirect3DVertexShader9> made;
  HRESULT hr = create_vertex_shader(program.data(), &made, false);
  if (FAILED(hr)) {
    ERR_ONCE("D3D9: the passthrough a declaration-driven draw needs could not be made: its translator refused it.");
    return hr;
  }
  auto [entry, inserted] = declaration_passthroughs_.try_emplace(elements, std::move(made));
  *shader = entry->second.Get();
  (*shader)->AddRef();
  return S_OK;
}

HRESULT
MTLD3D9Device::prepare_shader_draw() {
  IMTLD3D9VertexDeclaration *declaration = declaration_inputs_;
  ComPtr<MTLD3D9VertexDeclaration> fvf_declaration;
  if (!declaration) {
    std::vector<D3DVERTEXELEMENT9> elements;
    if (!elements_from_fvf(fvf_, elements)) {
      ERR_ONCE("D3D9: a draw with a game's shader needs the vertex declaration it reads, or an FVF this build places.");
      return D3DERR_INVALIDCALL;
    }
    fvf_declaration.Attach(new MTLD3D9VertexDeclaration(this, std::move(elements), false));
    declaration = fvf_declaration.Get();
  }

  // When no VS is bound, FF handles common FVFs; declarations FF cannot express (FLOAT3/4 texcoords for projected
  // textures) use a position+texcoord passthrough VS instead (Wine visual.c projected_textures_test).
  ComPtr<IDirect3DVertexShader9> passthrough_hold;
  IDirect3DVertexShader9 *effective_vs = vertex_shader_;
  bool transformed_draw = false;
  if (!vertex_shader_) {
    if (!pixel_shader_) {
      ERR_ONCE("D3D9: a programmable draw needs a vertex or pixel shader.");
      return D3DERR_INVALIDCALL;
    }
    // a declaration whose texture coordinate is three or four components wide carries the q a projected transform
    // divides by; the fixed-function vertex shaders read a two-component coordinate, so a projected draw of such a
    // declaration takes the position-and-texcoord passthrough the game's own shaders use (Wine visual.c
    // projected_textures_test draws D3DTTFF_COUNT3/COUNT4 | PROJECTED through decl3's FLOAT4 texcoord)
    const DWORD candidate_fvf = fixed_function_fvf();
    const UINT encoded_width = (candidate_fvf >> 16) & 3u;
    const UINT coordinate_width = encoded_width == 3 ? 1u : encoded_width + 2u;
    const DWORD transform_flags = texture_stages_[0].texture_transform_flags();
    const bool needs_wide_coordinate =
        coordinate_width >= 3 &&
        ((transform_flags & D3DTTFF_PROJECTED) != 0 || (transform_flags & 0xFu) >= D3DTTFF_COUNT3);
    if (!needs_wide_coordinate && FixedFunctionPipeline::supports(candidate_fvf)) {
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
      const FogState fog = resolve_fog_state(
          state_.render().fog_enabled(), state_.render().fog_table_mode(), state_.render().fog_vertex_mode(), false,
          0
      );
      // no game vertex shader: the fixed-function (or its passthrough) vertex stage writes the coordinate, and the
      // divisor follows the flags and the declaration's width
      const TexProjectState project = resolve_tex_project(
          texture_stages_[0].texture_transform_flags(), stage0_dxgi_of(texture_sources_[0]),
          declaration_texcoord_width(declaration->elements()), false
      );
  const BumpEnvState bump = resolve_bump_env(texture_stages_);
  ShadowState shadow;
  for (UINT stage = 0; stage < kTextureStages; stage++)
    if (stage_binds_depth(texture_sources_[stage]))
      shadow.mask |= 1u << stage;
  // the fixed-function VS this draw just bound writes TEXCOORD0 at 2 (unlit families) or 3 (lit/extended families)
  const UINT texcoord_slot = fixed_function_.texcoord_slot();
  ComPtr<ID3D11PixelShader> compiled_pixel;
  hr = pixel->compiled(
      d3d11_device_.Get(), alpha, fog, project, bump, shadow, texcoord_slot, state_.render().flat_shaded(),
      &compiled_pixel
  );
  if (FAILED(hr))
    return hr;
  context_->PSSetShader(compiled_pixel.Get(), nullptr, 0);
  hr = ensure_shader_constants();
  if (FAILED(hr))
    return hr;
  hr = upload_shader_constants();
  if (FAILED(hr))
    return hr;
  ID3D11Buffer *pixel_buffers[4] = {
      ps_constants_f_.Get(), ps_constants_i_.Get(), ps_constants_b_.Get(), ps_alpha_constants_.Get()};
  context_->PSSetConstantBuffers(0, 4, pixel_buffers);
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
    // the declaration's own shapes decide the passthrough: its dcls match the elements it carries, so a declaration
    // whose first element is a POSITIONT (a transformed monster declaration) or whose elements sit at their own
    // offsets draws the same way a game's shaders would read it (Wine pretransformed_varying_test, projected test)
    ComPtr<IDirect3DVertexShader9> declaration_passthrough;
    HRESULT hr = ensure_declaration_passthrough_vs(*declaration, &declaration_passthrough);
    if (FAILED(hr))
      return hr;
    passthrough_hold = declaration_passthrough;
    effective_vs = declaration_passthrough.Get();
    // a transformed declaration's vertices are in screen space, which the program maps through the viewport (b4)
    transformed_draw = std::any_of(
        declaration->elements().begin(), declaration->elements().end(),
        [](const D3DVERTEXELEMENT9 &element) { return element.Usage == D3DDECLUSAGE_POSITIONT; }
    );
  }

  // the vertex shader is queried first so FogCoord vs Full can follow oFog before the pixel program is chosen
  ComPtr<IMTLD3D9VertexShader> vertex;
  if (FAILED(effective_vs->QueryInterface(
          IID_IMTLD3D9VertexShader, reinterpret_cast<void **>(vertex.GetAddressOf())
      )))
    return D3DERR_INVALIDCALL;

  ComPtr<ID3D11PixelShader> compiled_pixel;
  ID3D11PixelShader *bound_pixel = nullptr;
  std::vector<PixelInputSlot> pixel_inputs;
  bool samples = false;
  // the fixed-function pipeline bound the pixel stage itself, so it is left as it is
  bool pipeline_pixel = false;
  const FogState fog = resolve_fog_state(
      state_.render().fog_enabled(), state_.render().fog_table_mode(), state_.render().fog_vertex_mode(),
      vertex->writes_fog(), vertex->major()
  );
  const TexProjectState project = resolve_tex_project(
      texture_stages_[0].texture_transform_flags(), stage0_dxgi_of(texture_sources_[0]),
      declaration_texcoord_width(declaration->elements()), vertex_shader_ != nullptr
  );
  const BumpEnvState bump = resolve_bump_env(texture_stages_);
  ShadowState shadow;
  for (UINT stage = 0; stage < kTextureStages; stage++)
    if (stage_binds_depth(texture_sources_[stage]))
      shadow.mask |= 1u << stage;
  if (pixel_shader_) {
    ComPtr<IMTLD3D9PixelShader> pixel;
    if (FAILED(pixel_shader_->QueryInterface(
            IID_IMTLD3D9PixelShader, reinterpret_cast<void **>(pixel.GetAddressOf())
        )))
      return D3DERR_INVALIDCALL;
    AlphaTest alpha;
    alpha.enabled = state_.render().alpha_test_enabled();
    alpha.func = state_.render().alpha_func();
    // a game's vertex shader is translated against this signature, where the translator writes TEXCOORD0 at 3
    HRESULT hr = pixel->compiled(
        d3d11_device_.Get(), alpha, fog, project, bump, shadow, 3, state_.render().flat_shaded(), &compiled_pixel
    );
    if (FAILED(hr))
      return hr;
    bound_pixel = compiled_pixel.Get();
    pixel_inputs = pixel->pixel_inputs();
    samples = pixel->samples();
  } else {
    // mixed: a game's vertex shader, the fixed-function pixel shader. COLOR0 is interpolator 1, where oD0 is written.
    // fog and the alpha test are the extended colour shader's (Microsoft Learn, D3DRS_ALPHATESTENABLE / FOG*)
    const bool alpha = state_.render().alpha_test_enabled() && state_.render().alpha_func() != D3DCMP_ALWAYS;
    const DWORD mixed_fvf = fixed_function_fvf();
    const bool mixed_textured = FixedFunctionPipeline::textured(mixed_fvf) &&
                                (texture_sources_[0] || !texture_stages_[0].skipped_without_texture());
    if (mixed_textured) {
      // the draw samples a fixed-function texture stage, which the colour-only shader below ignores: the pipeline
      // assembles that stage (its operations, matrix, D3DTTFF flags, samplers, fog and alpha state) and binds its own
      // pixel shader, and the game's vertex shader is bound over the vertex stage it set. D3D11 links the two stages by
      // semantic, so the coordinate's slot is only what this output signature names.
      HRESULT hr = prepare_fixed_function_draw();
      if (FAILED(hr))
        return hr;
      PixelInputSlot colour;
      colour.semantic = "COLOR";
      colour.index = 0;
      colour.reg = 1;
      pixel_inputs.push_back(std::move(colour));
      PixelInputSlot uv;
      uv.semantic = "TEXCOORD";
      uv.index = 0;
      uv.reg = fixed_function_.texcoord_slot();
      pixel_inputs.push_back(std::move(uv));
      pipeline_pixel = true;
    } else if (fog.source == FogSource::FogCoord) {
      // oFog is the blend factor; the FF colour shader only knows table fog from SV_Position.z, so a passthrough
      // pixel program carries the fog epilogue instead (Wine fog_with_shader_test foggy vs + no ps)
      HRESULT hr = ensure_fog_passthrough_ps();
      if (FAILED(hr))
        return hr;
      ComPtr<IMTLD3D9PixelShader> pixel;
      if (FAILED(fog_passthrough_ps_->QueryInterface(
              IID_IMTLD3D9PixelShader, reinterpret_cast<void **>(pixel.GetAddressOf())
          )))
        return D3DERR_INVALIDCALL;
      AlphaTest alpha_state;
      alpha_state.enabled = state_.render().alpha_test_enabled();
      alpha_state.func = state_.render().alpha_func();
      hr = pixel->compiled(
          d3d11_device_.Get(), alpha_state, fog, project, bump, ShadowState{}, 3, state_.render().flat_shaded(),
          &compiled_pixel
      );
      if (FAILED(hr))
        return hr;
      bound_pixel = compiled_pixel.Get();
      pixel_inputs = pixel->pixel_inputs();
      samples = pixel->samples();
    } else {
      // Table / VertexZ / Full via the extended colour shader. Full uses start=end=0 linear (factor → 0).
      const bool table_or_z = fog.source == FogSource::Table || fog.source == FogSource::VertexZ;
      const bool full_fog = fog.source == FogSource::Full;
      const bool fog_ff = table_or_z || full_fog;
      bound_pixel = fixed_function_.colour_pixel_shader(state_.render().flat_shaded(), alpha || fog_ff);
      if (!bound_pixel) {
        ERR_ONCE("D3D9: the fixed-function pixel shader a mixed draw needs is not made.");
        return D3DERR_INVALIDCALL;
      }
      if (alpha || fog_ff) {
        FixedFunctionDraw extras{};
        extras.fog_enabled = fog_ff;
        if (full_fog) {
          extras.fog_start = 0.0f;
          extras.fog_end = 0.0f;
          extras.fog_density = 1.0f;
          extras.fog_table_mode = D3DFOG_LINEAR;
        } else {
          extras.fog_start = state_.render().fog_start();
          extras.fog_end = state_.render().fog_end();
          extras.fog_density = state_.render().fog_density();
          extras.fog_table_mode = fog.mode;
        }
        const DWORD fog_color = state_.render().fog_color();
        extras.fog_color[0] = ((fog_color >> 16) & 0xFF) / 255.0f;
        extras.fog_color[1] = ((fog_color >> 8) & 0xFF) / 255.0f;
        extras.fog_color[2] = (fog_color & 0xFF) / 255.0f;
        extras.fog_color[3] = ((fog_color >> 24) & 0xFF) / 255.0f;
        extras.alpha_test = alpha;
        extras.alpha_func = state_.render().alpha_func();
        extras.alpha_ref = state_.render().alpha_ref() & 0xffu;
        HRESULT hr = fixed_function_.bind_stage_constants(context_.Get(), extras);
        if (FAILED(hr))
          return hr;
      }
      PixelInputSlot colour;
      colour.semantic = "COLOR";
      colour.index = 0;
      colour.reg = 1;
      pixel_inputs.push_back(std::move(colour));
    }
  }
  // the user clip planes D3DRS_CLIPPLANEENABLE names, which a vertex shader's draw takes in clip space
  float clip_planes[DeviceState::kClipPlanes * 4];
  const bool clipped = state_.clip_space_planes(false, clip_planes);
  ComPtr<ID3D11VertexShader> vertex_shader;
  ComPtr<ID3D11InputLayout> layout;
  // the streams SetStreamSourceFreq marks as per-instance data: the input layout's slot classes follow them, and they
  // are part of the compiled variant's identity (a draw can put the same declaration's streams in other roles)
  DWORD instanced_streams = 0;
  for (UINT stream = 0; stream < kMaxStreams; stream++) {
    if (stream_freq_[stream] & D3DSTREAMSOURCE_INSTANCEDATA)
      instanced_streams |= 1u << stream;
  }
  HRESULT hr = vertex->compiled(
      d3d11_device_.Get(), declaration->elements(), declaration->inputs(), pixel_inputs, declaration->layout(),
      instanced_streams, clipped, &vertex_shader, &layout
  );
  if (FAILED(hr))
    return hr;

  hr = ensure_shader_constants();
  if (FAILED(hr))
    return hr;
  hr = upload_shader_constants();
  if (FAILED(hr))
    return hr;
  if (clipped) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(vs_clip_planes_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
      return D3DERR_INVALIDCALL;
    memcpy(mapped.pData, clip_planes, sizeof(clip_planes));
    context_->Unmap(vs_clip_planes_.Get(), 0);
  }

  context_->IASetInputLayout(layout.Get());
  context_->VSSetShader(vertex_shader.Get(), nullptr, 0);
  if (!pipeline_pixel)
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
    // the vertex samplers are stages 16..19 and are a namespace of their own: they bind at the vertex shader's
    // registers 0..3, where its dcl_2d s0..s3 and texldl read them (Microsoft Learn, D3DVERTEXTEXTURESAMPLER)
    if (vertex->samples()) {
      ID3D11ShaderResourceView *vertex_views[kVertexTextureStages] = {};
      ID3D11SamplerState *vertex_samplers[kVertexTextureStages] = {};
      for (UINT at = 0; at < kVertexTextureStages; at++) {
        const UINT stage = kTextureStages + at;
        vertex_views[at] = texture_sources_[stage] ? texture_sources_[stage]->view() : default_texture_view_.Get();
        vertex_samplers[at] = sampler_states_[stage].state(d3d11_device_.Get());
        if (!vertex_samplers[at]) {
          ERR_ONCE("D3D9: the vertex sampler state stage ", stage, " holds makes no D3D11 sampler.");
          return D3DERR_INVALIDCALL;
        }
      }
      context_->VSSetShaderResources(0, kVertexTextureStages, vertex_views);
      context_->VSSetSamplers(0, kVertexTextureStages, vertex_samplers);
    }
  }
  // the viewport a transformed (POSITIONT) vertex program maps its screen-space position through (b4)
  if (transformed_draw) {
    if (!vs_viewport_) {
      D3D11_BUFFER_DESC viewport_desc{};
      viewport_desc.ByteWidth = 16;
      viewport_desc.Usage = D3D11_USAGE_DYNAMIC;
      viewport_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      viewport_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
      if (FAILED(d3d11_device_->CreateBuffer(&viewport_desc, nullptr, &vs_viewport_))) {
        ERR_ONCE("D3D9: the transformed vertices' viewport buffer could not be made.");
        return D3DERR_INVALIDCALL;
      }
    }
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(context_->Map(vs_viewport_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
      return D3DERR_INVALIDCALL;
    const float viewport_constant[4] = {(float)viewport_.X, (float)viewport_.Y, (float)viewport_.Width,
                                        (float)viewport_.Height};
    memcpy(mapped.pData, viewport_constant, sizeof(viewport_constant));
    context_->Unmap(vs_viewport_.Get(), 0);
  }
  ID3D11Buffer *vertex_buffers[5] = {vs_constants_f_.Get(), vs_constants_i_.Get(), vs_constants_b_.Get(),
                                     clipped ? vs_clip_planes_.Get() : nullptr, vs_viewport_.Get()};
  context_->VSSetConstantBuffers(0, 5, vertex_buffers);
  if (pixel_shader_) {
    ID3D11Buffer *pixel_buffers[4] = {
        ps_constants_f_.Get(), ps_constants_i_.Get(), ps_constants_b_.Get(), ps_alpha_constants_.Get()};
    context_->PSSetConstantBuffers(0, 4, pixel_buffers);
  }
  return S_OK;
}

} // namespace mullion::d3d9
