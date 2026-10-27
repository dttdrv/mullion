#include "d3d9_state.hpp"

namespace mullion::d3d9 {

namespace {

// D3D9's comparisons are D3D11's, value for value
D3D11_COMPARISON_FUNC comparison_of(DWORD func) {
  return static_cast<D3D11_COMPARISON_FUNC>(func);
}

// D3D9's point fill draws a triangle's vertices as single pixels, which D3D11 has no fill mode for: it is refused as a
// value rather than drawn as a solid triangle
D3D11_FILL_MODE fill_of(DWORD fill) {
  return fill == D3DFILL_WIREFRAME ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
}

// D3D9's front face is the counter-clockwise one on the screen, which is D3D11's FrontCounterClockwise; a cull mode
// that names a winding is the face D3D11 culls with that turned on
D3D11_CULL_MODE cull_of(DWORD cull) {
  switch (cull) {
  case D3DCULL_NONE:
    return D3D11_CULL_NONE;
  case D3DCULL_CCW:
    return D3D11_CULL_FRONT;
  default:
    return D3D11_CULL_BACK;
  }
}

D3D11_BLEND blend_of(DWORD blend) {
  switch (blend) {
  case D3DBLEND_ZERO:
    return D3D11_BLEND_ZERO;
  case D3DBLEND_ONE:
    return D3D11_BLEND_ONE;
  case D3DBLEND_SRCCOLOR:
    return D3D11_BLEND_SRC_COLOR;
  case D3DBLEND_INVSRCCOLOR:
    return D3D11_BLEND_INV_SRC_COLOR;
  case D3DBLEND_SRCALPHA:
    return D3D11_BLEND_SRC_ALPHA;
  case D3DBLEND_INVSRCALPHA:
    return D3D11_BLEND_INV_SRC_ALPHA;
  case D3DBLEND_DESTALPHA:
    return D3D11_BLEND_DEST_ALPHA;
  case D3DBLEND_INVDESTALPHA:
    return D3D11_BLEND_INV_DEST_ALPHA;
  case D3DBLEND_DESTCOLOR:
    return D3D11_BLEND_DEST_COLOR;
  case D3DBLEND_INVDESTCOLOR:
    return D3D11_BLEND_INV_DEST_COLOR;
  case D3DBLEND_SRCALPHASAT:
    return D3D11_BLEND_SRC_ALPHA_SAT;
  case D3DBLEND_BLENDFACTOR:
    return D3D11_BLEND_BLEND_FACTOR;
  default:
    return D3D11_BLEND_INV_BLEND_FACTOR;
  }
}

D3D11_BLEND_OP blend_op_of(DWORD op) {
  switch (op) {
  case D3DBLENDOP_SUBTRACT:
    return D3D11_BLEND_OP_SUBTRACT;
  case D3DBLENDOP_MIN:
    return D3D11_BLEND_OP_MIN;
  case D3DBLENDOP_MAX:
    return D3D11_BLEND_OP_MAX;
  case D3DBLENDOP_REVSUBTRACT:
    return D3D11_BLEND_OP_REV_SUBTRACT;
  default:
    return D3D11_BLEND_OP_ADD;
  }
}

// the blend factors a single source and destination can be: BOTHSRCALPHA and BOTHINVSRCALPHA need a second source,
// which this milestone's pixel shader does not write, and the blend factor is a state (D3DRS_BLENDFACTOR) this build
// cannot set, so a draw is not blended with a factor nobody named
bool usable_blend(DWORD blend) {
  return blend >= D3DBLEND_ZERO && blend <= D3DBLEND_SRCALPHASAT;
}

} // namespace

RenderState::RenderState() = default;

bool
RenderState::supported(D3DRENDERSTATETYPE state) {
  switch (state) {
  case D3DRS_ZENABLE:
  case D3DRS_ZWRITEENABLE:
  case D3DRS_ZFUNC:
  case D3DRS_CULLMODE:
  case D3DRS_FILLMODE:
  case D3DRS_SHADEMODE:
  case D3DRS_ALPHABLENDENABLE:
  case D3DRS_SRCBLEND:
  case D3DRS_DESTBLEND:
  case D3DRS_BLENDOP:
  case D3DRS_COLORWRITEENABLE:
  case D3DRS_TEXTUREFACTOR:
    return true;
  default:
    return false;
  }
}

bool
RenderState::usable_value(D3DRENDERSTATETYPE state, DWORD value) {
  switch (state) {
  case D3DRS_ZENABLE:
  case D3DRS_ZWRITEENABLE:
  case D3DRS_ALPHABLENDENABLE:
    return value == TRUE || value == FALSE;
  case D3DRS_ZFUNC:
    return value >= D3DCMP_NEVER && value <= D3DCMP_ALWAYS;
  case D3DRS_CULLMODE:
    return value >= D3DCULL_NONE && value <= D3DCULL_CCW;
  case D3DRS_FILLMODE:
    return value == D3DFILL_WIREFRAME || value == D3DFILL_SOLID;
  case D3DRS_SHADEMODE:
    // PHONG is a shade mode D3D9's fixed-function path gives a triangle's own normals to, which this milestone has none
    // of: GOURAUD and FLAT are the two a vertex's colour makes
    return value == D3DSHADE_FLAT || value == D3DSHADE_GOURAUD;
  case D3DRS_SRCBLEND:
  case D3DRS_DESTBLEND:
    return usable_blend(value);
  case D3DRS_BLENDOP:
    return value >= D3DBLENDOP_ADD && value <= D3DBLENDOP_REVSUBTRACT;
  case D3DRS_COLORWRITEENABLE:
    return value <= 0xF;
  case D3DRS_TEXTUREFACTOR:
    return true; // every DWORD is a colour, alpha and all
  default:
    return false;
  }
}

HRESULT
RenderState::set(D3DRENDERSTATETYPE state, DWORD value) {
  if (!supported(state))
    return D3DERR_INVALIDCALL;
  if (!usable_value(state, value))
    return D3DERR_INVALIDCALL;

  switch (state) {
  case D3DRS_ZENABLE:
    z_enable_ = value;
    depth_stencil_current_ = false;
    break;
  case D3DRS_ZWRITEENABLE:
    z_write_enable_ = value;
    depth_stencil_current_ = false;
    break;
  case D3DRS_ZFUNC:
    z_func_ = value;
    depth_stencil_current_ = false;
    break;
  case D3DRS_CULLMODE:
    cull_mode_ = value;
    rasterizer_current_ = false;
    break;
  case D3DRS_FILLMODE:
    fill_mode_ = value;
    rasterizer_current_ = false;
    break;
  case D3DRS_SHADEMODE:
    shade_mode_ = value;
    break;
  case D3DRS_ALPHABLENDENABLE:
    alpha_blend_enable_ = value;
    blend_current_ = false;
    break;
  case D3DRS_SRCBLEND:
    src_blend_ = value;
    blend_current_ = false;
    break;
  case D3DRS_DESTBLEND:
    dest_blend_ = value;
    blend_current_ = false;
    break;
  case D3DRS_BLENDOP:
    blend_op_ = value;
    blend_current_ = false;
    break;
  case D3DRS_COLORWRITEENABLE:
    colour_write_enable_ = value;
    blend_current_ = false;
    break;
  case D3DRS_TEXTUREFACTOR:
    texture_factor_ = value;
    break;
  default:
    return D3DERR_INVALIDCALL;
  }
  return S_OK;
}

HRESULT
RenderState::get(D3DRENDERSTATETYPE state, DWORD *value) const {
  if (!value || !supported(state))
    return D3DERR_INVALIDCALL;
  switch (state) {
  case D3DRS_ZENABLE:
    *value = z_enable_;
    break;
  case D3DRS_ZWRITEENABLE:
    *value = z_write_enable_;
    break;
  case D3DRS_ZFUNC:
    *value = z_func_;
    break;
  case D3DRS_CULLMODE:
    *value = cull_mode_;
    break;
  case D3DRS_FILLMODE:
    *value = fill_mode_;
    break;
  case D3DRS_SHADEMODE:
    *value = shade_mode_;
    break;
  case D3DRS_ALPHABLENDENABLE:
    *value = alpha_blend_enable_;
    break;
  case D3DRS_SRCBLEND:
    *value = src_blend_;
    break;
  case D3DRS_DESTBLEND:
    *value = dest_blend_;
    break;
  case D3DRS_BLENDOP:
    *value = blend_op_;
    break;
  case D3DRS_COLORWRITEENABLE:
    *value = colour_write_enable_;
    break;
  case D3DRS_TEXTUREFACTOR:
    *value = texture_factor_;
    break;
  default:
    return D3DERR_INVALIDCALL;
  }
  return S_OK;
}

HRESULT
RenderState::apply(ID3D11Device *device, ID3D11DeviceContext *context) {
  if (!rasterizer_current_) {
    // D3D9's viewport is a clip rectangle as well as a place: D3D11's scissor is the only thing that clips a draw to
    // one, so it is on for every frame
    D3D11_RASTERIZER_DESC desc{};
    desc.FillMode = fill_of(fill_mode_);
    desc.CullMode = cull_of(cull_mode_);
    desc.FrontCounterClockwise = TRUE;
    desc.DepthBias = 0;
    desc.DepthBiasClamp = 0.0f;
    desc.SlopeScaledDepthBias = 0.0f;
    desc.DepthClipEnable = TRUE;
    desc.ScissorEnable = TRUE;
    desc.MultisampleEnable = FALSE;
    desc.AntialiasedLineEnable = FALSE;
    rasterizer_.Reset();
    if (FAILED(device->CreateRasterizerState(&desc, &rasterizer_)))
      return D3DERR_INVALIDCALL;
    rasterizer_current_ = true;
  }
  if (!depth_stencil_current_) {
    D3D11_DEPTH_STENCIL_DESC desc{};
    desc.DepthEnable = z_enable_ != FALSE;
    // a depth test that is off writes nothing either, as D3D9's does
    desc.DepthWriteMask = (z_enable_ && z_write_enable_) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    desc.DepthFunc = comparison_of(z_func_);
    desc.StencilEnable = FALSE;
    depth_stencil_.Reset();
    if (FAILED(device->CreateDepthStencilState(&desc, &depth_stencil_)))
      return D3DERR_INVALIDCALL;
    depth_stencil_current_ = true;
  }
  if (!blend_current_) {
    D3D11_BLEND_DESC desc{};
    desc.AlphaToCoverageEnable = FALSE;
    desc.IndependentBlendEnable = FALSE;
    D3D11_RENDER_TARGET_BLEND_DESC &target = desc.RenderTarget[0];
    target.BlendEnable = alpha_blend_enable_ != FALSE;
    target.SrcBlend = blend_of(src_blend_);
    target.DestBlend = blend_of(dest_blend_);
    target.BlendOp = blend_op_of(blend_op_);
    target.SrcBlendAlpha = blend_of(src_blend_);
    target.DestBlendAlpha = blend_of(dest_blend_);
    target.BlendOpAlpha = blend_op_of(blend_op_);
    target.RenderTargetWriteMask = colour_write_enable_ & 0xF;
    blend_.Reset();
    if (FAILED(device->CreateBlendState(&desc, &blend_)))
      return D3DERR_INVALIDCALL;
    blend_current_ = true;
  }

  context->RSSetState(rasterizer_.Get());
  context->OMSetDepthStencilState(depth_stencil_.Get(), 0);
  context->OMSetBlendState(blend_.Get(), nullptr, 0xFFFFFFFF);
  return S_OK;
}

} // namespace mullion::d3d9
