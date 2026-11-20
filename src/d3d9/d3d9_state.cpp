#include "d3d9_state.hpp"

namespace mullion::d3d9 {

namespace {

// D3D9's comparisons are D3D11's, value for value
D3D11_COMPARISON_FUNC comparison_of(DWORD func) {
  return static_cast<D3D11_COMPARISON_FUNC>(func);
}

// D3D9's point fill draws a primitive's vertices as single pixels, and D3D11 has no fill mode for it: the rasterizer
// stays solid and the draw path issues the vertices as a point list, one pixel each, the size MaxPointSize reports
// (d3d9_device_draw.cpp)
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

// D3D9's four write-mask states are not adjacent: 168 is target 0, and 190..192 are targets 1..3
UINT colour_write_target(D3DRENDERSTATETYPE state) {
  switch (state) {
  case D3DRS_COLORWRITEENABLE:
    return 0;
  case D3DRS_COLORWRITEENABLE1:
    return 1;
  case D3DRS_COLORWRITEENABLE2:
    return 2;
  default:
    return 3;
  }
}

} // namespace

RenderState::RenderState() {
  // The states whose D3D9 default is nonzero, and which are kept in stored_ rather than in a typed field below.
  stored_[D3DRS_LASTPIXEL] = TRUE;
  stored_[D3DRS_CLIPPING] = TRUE;
  stored_[D3DRS_LOCALVIEWER] = TRUE;
  stored_[D3DRS_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
  stored_[D3DRS_AMBIENTMATERIALSOURCE] = D3DMCS_MATERIAL;
  stored_[D3DRS_EMISSIVEMATERIALSOURCE] = D3DMCS_MATERIAL;
  stored_[D3DRS_POINTSIZE] = 0x3F800000;        // 1.0f
  stored_[D3DRS_POINTSIZE_MIN] = 0x3F800000;    // 1.0f
  stored_[D3DRS_POINTSIZE_MAX] = 0x3F800000;    // D3DCAPS9::MaxPointSize
  stored_[D3DRS_POINTSCALE_A] = 0x3F800000;     // 1.0f
  stored_[D3DRS_MULTISAMPLEANTIALIAS] = TRUE;
  stored_[D3DRS_MULTISAMPLEMASK] = 0xFFFFFFFF;
  stored_[D3DRS_PATCHEDGESTYLE] = D3DPATCHEDGE_DISCRETE;
  stored_[D3DRS_POSITIONDEGREE] = D3DDEGREE_CUBIC;
  stored_[D3DRS_NORMALDEGREE] = D3DDEGREE_LINEAR;
  stored_[D3DRS_MINTESSELLATIONLEVEL] = 0x3F800000;  // 1.0f
  stored_[D3DRS_MAXTESSELLATIONLEVEL] = 0x3F800000;  // 1.0f
  stored_[D3DRS_ADAPTIVETESS_Z] = 0x3F800000;        // 1.0f
  stored_[D3DRS_CCW_STENCILFAIL] = D3DSTENCILOP_KEEP;
  stored_[D3DRS_CCW_STENCILZFAIL] = D3DSTENCILOP_KEEP;
  stored_[D3DRS_CCW_STENCILPASS] = D3DSTENCILOP_KEEP;
  stored_[D3DRS_CCW_STENCILFUNC] = D3DCMP_ALWAYS;
  stored_[D3DRS_BLENDFACTOR] = 0xFFFFFFFF;
  stored_[D3DRS_SRCBLENDALPHA] = D3DBLEND_ONE;
  stored_[D3DRS_DESTBLENDALPHA] = D3DBLEND_ZERO;
  stored_[D3DRS_BLENDOPALPHA] = D3DBLENDOP_ADD;
}

bool
RenderState::supported(D3DRENDERSTATETYPE state) {
  UINT count = 0;
  const D3DRENDERSTATETYPE *states = all_states(count);
  for (UINT at = 0; at < count; at++)
    if (states[at] == state)
      return true;
  return false;
}

const D3DRENDERSTATETYPE *
RenderState::all_states(UINT &count) {
  // The complete standard D3D9 render-state set this device accepts. The order is D3D9's state-block order: values
  // that D3D11 has no direct object for are still stored, so a block can capture and restore them faithfully.
  static const D3DRENDERSTATETYPE states[] = {
      D3DRS_ZENABLE,          D3DRS_FILLMODE,          D3DRS_SHADEMODE,         D3DRS_ZWRITEENABLE,
      D3DRS_ALPHATESTENABLE,  D3DRS_LASTPIXEL,         D3DRS_SRCBLEND,         D3DRS_DESTBLEND,
      D3DRS_CULLMODE,         D3DRS_ZFUNC,             D3DRS_ALPHAREF,         D3DRS_ALPHAFUNC,
      D3DRS_DITHERENABLE,     D3DRS_ALPHABLENDENABLE,  D3DRS_FOGENABLE,        D3DRS_SPECULARENABLE,
      D3DRS_FOGCOLOR,         D3DRS_FOGTABLEMODE,      D3DRS_FOGSTART,         D3DRS_FOGEND,
      D3DRS_FOGDENSITY,       D3DRS_RANGEFOGENABLE,    D3DRS_STENCILENABLE,    D3DRS_STENCILFAIL,
      D3DRS_STENCILZFAIL,     D3DRS_STENCILPASS,       D3DRS_STENCILFUNC,      D3DRS_STENCILREF,
      D3DRS_STENCILMASK,      D3DRS_STENCILWRITEMASK,  D3DRS_TEXTUREFACTOR,    D3DRS_WRAP0,
      D3DRS_WRAP1,            D3DRS_WRAP2,             D3DRS_WRAP3,            D3DRS_WRAP4,
      D3DRS_WRAP5,            D3DRS_WRAP6,             D3DRS_WRAP7,            D3DRS_CLIPPING,
      D3DRS_LIGHTING,         D3DRS_AMBIENT,           D3DRS_FOGVERTEXMODE,    D3DRS_COLORVERTEX,
      D3DRS_LOCALVIEWER,      D3DRS_NORMALIZENORMALS,  D3DRS_DIFFUSEMATERIALSOURCE,
      D3DRS_SPECULARMATERIALSOURCE, D3DRS_AMBIENTMATERIALSOURCE, D3DRS_EMISSIVEMATERIALSOURCE,
      D3DRS_VERTEXBLEND,      D3DRS_CLIPPLANEENABLE,   D3DRS_POINTSIZE,        D3DRS_POINTSIZE_MIN,
      D3DRS_POINTSPRITEENABLE, D3DRS_POINTSCALEENABLE, D3DRS_POINTSCALE_A,     D3DRS_POINTSCALE_B,
      D3DRS_POINTSCALE_C,     D3DRS_MULTISAMPLEANTIALIAS, D3DRS_MULTISAMPLEMASK, D3DRS_PATCHEDGESTYLE,
      D3DRS_POINTSIZE_MAX,    D3DRS_INDEXEDVERTEXBLENDENABLE, D3DRS_COLORWRITEENABLE, D3DRS_TWEENFACTOR,
      D3DRS_BLENDOP,          D3DRS_POSITIONDEGREE,    D3DRS_NORMALDEGREE,     D3DRS_SCISSORTESTENABLE,
      D3DRS_SLOPESCALEDEPTHBIAS, D3DRS_ANTIALIASEDLINEENABLE, D3DRS_MINTESSELLATIONLEVEL,
      D3DRS_MAXTESSELLATIONLEVEL, D3DRS_ADAPTIVETESS_X, D3DRS_ADAPTIVETESS_Y, D3DRS_ADAPTIVETESS_Z,
      D3DRS_ADAPTIVETESS_W,   D3DRS_ENABLEADAPTIVETESSELLATION, D3DRS_TWOSIDEDSTENCILMODE,
      D3DRS_CCW_STENCILFAIL,  D3DRS_CCW_STENCILZFAIL,  D3DRS_CCW_STENCILPASS,  D3DRS_CCW_STENCILFUNC,
      D3DRS_COLORWRITEENABLE1, D3DRS_COLORWRITEENABLE2, D3DRS_COLORWRITEENABLE3, D3DRS_BLENDFACTOR,
      D3DRS_SRGBWRITEENABLE,  D3DRS_DEPTHBIAS,         D3DRS_WRAP8,            D3DRS_WRAP9,
      D3DRS_WRAP10,           D3DRS_WRAP11,            D3DRS_WRAP12,           D3DRS_WRAP13,
      D3DRS_WRAP14,           D3DRS_WRAP15,            D3DRS_SEPARATEALPHABLENDENABLE,
      D3DRS_SRCBLENDALPHA,    D3DRS_DESTBLENDALPHA,    D3DRS_BLENDOPALPHA,
  };
  count = (UINT)(sizeof(states) / sizeof(states[0]));
  return states;
}

bool
RenderState::usable_value(D3DRENDERSTATETYPE state, DWORD value) {
  switch (state) {
  case D3DRS_ZENABLE:
    // D3DZB_TRUE, D3DZB_FALSE and D3DZB_USEW are all values D3D9 accepts, even though this device has no w-buffer
    return value == D3DZB_FALSE || value == D3DZB_TRUE || value == D3DZB_USEW;
  case D3DRS_ZWRITEENABLE:
  case D3DRS_ALPHABLENDENABLE:
  case D3DRS_ALPHATESTENABLE:
  case D3DRS_SCISSORTESTENABLE:
  case D3DRS_LIGHTING:
  case D3DRS_SPECULARENABLE:
  case D3DRS_LOCALVIEWER:
  case D3DRS_NORMALIZENORMALS:
  case D3DRS_COLORVERTEX:
  case D3DRS_STENCILENABLE:
  case D3DRS_FOGENABLE:
  case D3DRS_RANGEFOGENABLE:
  case D3DRS_INDEXEDVERTEXBLENDENABLE:
    return value == TRUE || value == FALSE;
  case D3DRS_AMBIENT:
    return true; // every DWORD is a colour, alpha and all
  case D3DRS_CLIPPLANEENABLE:
    // a bit for each of the six planes (D3DCLIPPLANE0..5); the bits past them name planes no device has, which D3D9
    // keeps and ignores
    return true;
  case D3DRS_DIFFUSEMATERIALSOURCE:
  case D3DRS_AMBIENTMATERIALSOURCE:
    // D3DMCS_MATERIAL, D3DMCS_COLOR1 and D3DMCS_COLOR2 are the three sources D3D9 defines; a draw that uses COLOR2
    // without a second vertex colour keeps the state, and the lit path falls back to the material colour
    return value <= D3DMCS_COLOR2;
  case D3DRS_ZFUNC:
  case D3DRS_ALPHAFUNC:
  case D3DRS_STENCILFUNC:
    return value >= D3DCMP_NEVER && value <= D3DCMP_ALWAYS;
  case D3DRS_STENCILFAIL:
  case D3DRS_STENCILZFAIL:
  case D3DRS_STENCILPASS:
    return value >= D3DSTENCILOP_KEEP && value <= D3DSTENCILOP_DECR;
  case D3DRS_STENCILREF:
  case D3DRS_STENCILMASK:
  case D3DRS_STENCILWRITEMASK:
    return true;
  case D3DRS_ALPHAREF:
    // D3D9's reference is an 8-bit alpha in a DWORD, and a value past it is the low eight bits D3D9 compares
    return true;
  case D3DRS_CULLMODE:
    return value >= D3DCULL_NONE && value <= D3DCULL_CCW;
  case D3DRS_FILLMODE:
    return value == D3DFILL_WIREFRAME || value == D3DFILL_SOLID || value == D3DFILL_POINT;
  case D3DRS_SHADEMODE:
    // FLAT, GOURAUD and PHONG are D3D9's three shade modes. PHONG is stored even where the fixed-function path has no
    // per-triangle normals to use, so GetRenderState and state blocks still round-trip the value.
    return value <= D3DSHADE_PHONG;
  case D3DRS_SRCBLEND:
  case D3DRS_DESTBLEND:
    return usable_blend(value);
  case D3DRS_BLENDOP:
    return value >= D3DBLENDOP_ADD && value <= D3DBLENDOP_REVSUBTRACT;
  case D3DRS_COLORWRITEENABLE:
  case D3DRS_COLORWRITEENABLE1:
  case D3DRS_COLORWRITEENABLE2:
  case D3DRS_COLORWRITEENABLE3:
    return value <= 0xF;
  case D3DRS_TEXTUREFACTOR:
  case D3DRS_FOGCOLOR:
  case D3DRS_FOGSTART:
  case D3DRS_FOGEND:
  case D3DRS_FOGDENSITY:
    return true; // every DWORD is a colour, alpha and all
  case D3DRS_FOGTABLEMODE:
  case D3DRS_FOGVERTEXMODE:
    return value == D3DFOG_NONE || value == D3DFOG_LINEAR || value == D3DFOG_EXP || value == D3DFOG_EXP2;
  case D3DRS_VERTEXBLEND:
    // the non-indexed counts, tweening, and no blending (Microsoft Learn, D3DVERTEXBLENDFLAGS). indexed blending is a
    // separate state, and a count past the four matrices this device blends with is not one of these
    return value == D3DVBF_DISABLE || value == D3DVBF_1WEIGHTS || value == D3DVBF_2WEIGHTS || value == D3DVBF_3WEIGHTS ||
           value == D3DVBF_TWEENING || value == D3DVBF_0WEIGHTS;
  case D3DRS_TWEENFACTOR:
    return true;
  default:
    // Every state in all_states() has a stored value. States with a narrower contract are checked above.
    return supported(state);
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
  case D3DRS_COLORWRITEENABLE1:
  case D3DRS_COLORWRITEENABLE2:
  case D3DRS_COLORWRITEENABLE3:
    colour_write_enable_[colour_write_target(state)] = value;
    blend_current_ = false;
    break;
  case D3DRS_TEXTUREFACTOR:
    texture_factor_ = value;
    break;
  case D3DRS_ALPHATESTENABLE:
    alpha_test_enable_ = value;
    break;
  case D3DRS_ALPHAFUNC:
    alpha_func_ = value;
    break;
  case D3DRS_ALPHAREF:
    // Store the full DWORD for GetRenderState round-trip (Wine visual.c sets 0xff70); the alpha test uses the low 8 bits.
    alpha_ref_ = value;
    break;
  case D3DRS_SCISSORTESTENABLE:
    scissor_test_enable_ = value;
    break;
  case D3DRS_LIGHTING:
    lighting_ = value;
    break;
  case D3DRS_SPECULARENABLE:
    specular_enable_ = value;
    break;
  case D3DRS_LOCALVIEWER:
    local_viewer_ = value;
    break;
  case D3DRS_AMBIENT:
    ambient_ = value;
    break;
  case D3DRS_NORMALIZENORMALS:
    normalize_normals_ = value;
    break;
  case D3DRS_COLORVERTEX:
    colour_vertex_ = value;
    break;
  case D3DRS_DIFFUSEMATERIALSOURCE:
    diffuse_source_ = value;
    break;
  case D3DRS_AMBIENTMATERIALSOURCE:
    ambient_source_ = value;
    break;
  case D3DRS_EMISSIVEMATERIALSOURCE:
    emissive_source_ = value;
    break;
  case D3DRS_SPECULARMATERIALSOURCE:
    specular_source_ = value;
    break;
  case D3DRS_STENCILENABLE:
    stencil_enable_ = value;
    depth_stencil_current_ = false;
    break;
  case D3DRS_STENCILFAIL:
    stencil_fail_ = value;
    depth_stencil_current_ = false;
    break;
  case D3DRS_STENCILZFAIL:
    stencil_zfail_ = value;
    depth_stencil_current_ = false;
    break;
  case D3DRS_STENCILPASS:
    stencil_pass_ = value;
    depth_stencil_current_ = false;
    break;
  case D3DRS_STENCILFUNC:
    stencil_func_ = value;
    depth_stencil_current_ = false;
    break;
  case D3DRS_STENCILREF:
    stencil_ref_ = value;
    break;
  case D3DRS_STENCILMASK:
    stencil_mask_ = value;
    depth_stencil_current_ = false;
    break;
  case D3DRS_STENCILWRITEMASK:
    stencil_write_mask_ = value;
    depth_stencil_current_ = false;
    break;
  case D3DRS_FOGENABLE:
    fog_enable_ = value;
    break;
  case D3DRS_FOGCOLOR:
    fog_color_ = value;
    break;
  case D3DRS_FOGTABLEMODE:
    fog_table_mode_ = value;
    break;
  case D3DRS_FOGVERTEXMODE:
    fog_vertex_mode_ = value;
    break;
  case D3DRS_FOGSTART:
    fog_start_ = value;
    break;
  case D3DRS_FOGEND:
    fog_end_ = value;
    break;
  case D3DRS_FOGDENSITY:
    fog_density_ = value;
    break;
  case D3DRS_RANGEFOGENABLE:
    fog_range_enable_ = value;
    break;
  case D3DRS_CLIPPLANEENABLE:
    clip_plane_enable_ = value;
    break;
  case D3DRS_VERTEXBLEND:
    vertex_blend_ = value;
    break;
  case D3DRS_TWEENFACTOR:
    tween_factor_ = value;
    break;
  case D3DRS_INDEXEDVERTEXBLENDENABLE:
    indexed_vertex_blend_ = value;
    break;
  default:
    stored_[state] = value;
    break;
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
  case D3DRS_COLORWRITEENABLE1:
  case D3DRS_COLORWRITEENABLE2:
  case D3DRS_COLORWRITEENABLE3:
    *value = colour_write_enable_[colour_write_target(state)];
    break;
  case D3DRS_TEXTUREFACTOR:
    *value = texture_factor_;
    break;
  case D3DRS_ALPHATESTENABLE:
    *value = alpha_test_enable_;
    break;
  case D3DRS_ALPHAFUNC:
    *value = alpha_func_;
    break;
  case D3DRS_ALPHAREF:
    *value = alpha_ref_;
    break;
  case D3DRS_SCISSORTESTENABLE:
    *value = scissor_test_enable_;
    break;
  case D3DRS_LIGHTING:
    *value = lighting_;
    break;
  case D3DRS_SPECULARENABLE:
    *value = specular_enable_;
    break;
  case D3DRS_LOCALVIEWER:
    *value = local_viewer_;
    break;
  case D3DRS_AMBIENT:
    *value = ambient_;
    break;
  case D3DRS_NORMALIZENORMALS:
    *value = normalize_normals_;
    break;
  case D3DRS_COLORVERTEX:
    *value = colour_vertex_;
    break;
  case D3DRS_DIFFUSEMATERIALSOURCE:
    *value = diffuse_source_;
    break;
  case D3DRS_AMBIENTMATERIALSOURCE:
    *value = ambient_source_;
    break;
  case D3DRS_EMISSIVEMATERIALSOURCE:
    *value = emissive_source_;
    break;
  case D3DRS_SPECULARMATERIALSOURCE:
    *value = specular_source_;
    break;
  case D3DRS_STENCILENABLE:
    *value = stencil_enable_;
    break;
  case D3DRS_STENCILFAIL:
    *value = stencil_fail_;
    break;
  case D3DRS_STENCILZFAIL:
    *value = stencil_zfail_;
    break;
  case D3DRS_STENCILPASS:
    *value = stencil_pass_;
    break;
  case D3DRS_STENCILFUNC:
    *value = stencil_func_;
    break;
  case D3DRS_STENCILREF:
    *value = stencil_ref_;
    break;
  case D3DRS_STENCILMASK:
    *value = stencil_mask_;
    break;
  case D3DRS_STENCILWRITEMASK:
    *value = stencil_write_mask_;
    break;
  case D3DRS_FOGENABLE:
    *value = fog_enable_;
    break;
  case D3DRS_FOGCOLOR:
    *value = fog_color_;
    break;
  case D3DRS_FOGTABLEMODE:
    *value = fog_table_mode_;
    break;
  case D3DRS_FOGVERTEXMODE:
    *value = fog_vertex_mode_;
    break;
  case D3DRS_FOGSTART:
    *value = fog_start_;
    break;
  case D3DRS_FOGEND:
    *value = fog_end_;
    break;
  case D3DRS_FOGDENSITY:
    *value = fog_density_;
    break;
  case D3DRS_RANGEFOGENABLE:
    *value = fog_range_enable_;
    break;
  case D3DRS_CLIPPLANEENABLE:
    *value = clip_plane_enable_;
    break;
  case D3DRS_VERTEXBLEND:
    *value = vertex_blend_;
    break;
  case D3DRS_TWEENFACTOR:
    *value = tween_factor_;
    break;
  case D3DRS_INDEXEDVERTEXBLENDENABLE:
    *value = indexed_vertex_blend_;
    break;
  default:
    *value = stored_[state];
    break;
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
    desc.DepthClipEnable = depth_clip_;
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
    desc.StencilEnable = stencil_enable_ != FALSE;
    desc.StencilReadMask = (UINT8)stencil_mask_;
    desc.StencilWriteMask = (UINT8)stencil_write_mask_;
    desc.FrontFace.StencilFailOp = static_cast<D3D11_STENCIL_OP>(stencil_fail_);
    desc.FrontFace.StencilDepthFailOp = static_cast<D3D11_STENCIL_OP>(stencil_zfail_);
    desc.FrontFace.StencilPassOp = static_cast<D3D11_STENCIL_OP>(stencil_pass_);
    desc.FrontFace.StencilFunc = comparison_of(stencil_func_);
    desc.BackFace = desc.FrontFace;
    depth_stencil_.Reset();
    if (FAILED(device->CreateDepthStencilState(&desc, &depth_stencil_)))
      return D3DERR_INVALIDCALL;
    depth_stencil_current_ = true;
  }
  if (!blend_current_) {
    D3D11_BLEND_DESC desc{};
    desc.AlphaToCoverageEnable = FALSE;
    // D3D9's blend equation is one for every target, but its write mask is per target
    desc.IndependentBlendEnable = TRUE;
    D3D11_RENDER_TARGET_BLEND_DESC target{};
    target.BlendEnable = alpha_blend_enable_ != FALSE;
    target.SrcBlend = blend_of(src_blend_);
    target.DestBlend = blend_of(dest_blend_);
    target.BlendOp = blend_op_of(blend_op_);
    target.SrcBlendAlpha = blend_of(src_blend_);
    target.DestBlendAlpha = blend_of(dest_blend_);
    target.BlendOpAlpha = blend_op_of(blend_op_);
    for (UINT at = 0; at < 8; at++) {
      desc.RenderTarget[at] = target;
      desc.RenderTarget[at].RenderTargetWriteMask = colour_write_enable_[at < 4 ? at : 3] & 0xF;
    }
    blend_.Reset();
    if (FAILED(device->CreateBlendState(&desc, &blend_)))
      return D3DERR_INVALIDCALL;
    blend_current_ = true;
  }

  context->RSSetState(rasterizer_.Get());
  context->OMSetDepthStencilState(depth_stencil_.Get(), stencil_ref_);
  context->OMSetBlendState(blend_.Get(), nullptr, 0xFFFFFFFF);
  return S_OK;
}

} // namespace mullion::d3d9
