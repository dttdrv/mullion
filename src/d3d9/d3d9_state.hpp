// the render states this build honours, as D3D9's defaults and as the D3D11 state objects a draw binds. a state that
// is not one of these is refused by the device rather than kept by nobody, and a value this build cannot turn into
// D3D11 state is refused too, so a frame is never drawn with a state its application did not ask for (src/d3d9/README).
// D3DRS_TEXTUREFACTOR is here as a value a texture stage's argument can name (d3d9_texture.hpp), not as D3D11 state:
// it reaches a draw through the textured pixel shader's constants. The lighting states are the same kind of value:
// D3DRS_LIGHTING, D3DRS_SPECULARENABLE, D3DRS_AMBIENT, D3DRS_NORMALIZENORMALS, D3DRS_COLORVERTEX and the two material
// sources a lit vertex's reflectances can come from are read by the lit vertex shader's constants (d3d9_lights.hpp), not
// by a D3D11 state object.
#pragma once
#include "d3d9_common.hpp"

namespace mullion::d3d9 {

class RenderState {
public:
  RenderState();

  // whether the device takes this state at all, and whether this value of it is one a draw can honour
  static bool supported(D3DRENDERSTATETYPE state);
  static bool usable_value(D3DRENDERSTATETYPE state, DWORD value);
  // every state the device takes, which is what a state block enumerates
  static const D3DRENDERSTATETYPE *all_states(UINT &count);

  HRESULT set(D3DRENDERSTATETYPE state, DWORD value);
  HRESULT get(D3DRENDERSTATETYPE state, DWORD *value) const;

  // the rasterizer, depth-stencil and blend states the values make, bound to the context for the next draw
  HRESULT apply(ID3D11Device *device, ID3D11DeviceContext *context);
  bool flat_shaded() const {
    return shade_mode_ == D3DSHADE_FLAT;
  }
  // the colour a texture stage's D3DTA_TFACTOR argument names, as D3D9's D3DCOLOR is
  DWORD texture_factor() const {
    return texture_factor_;
  }
  // D3D9's alpha test, which a pixel shader carries: it compares the alpha the program writes with D3DRS_ALPHAREF using
  // D3DRS_ALPHAFUNC, and a pixel that fails is not written (Microsoft Learn, D3DRS_ALPHATESTENABLE). the three states
  // are these, and the test is the pixel program's (d3d9_shader_translator.cpp puts it in the program)
  bool alpha_test_enabled() const {
    return alpha_test_enable_ != FALSE;
  }
  DWORD alpha_func() const {
    return alpha_func_;
  }
  DWORD alpha_ref() const {
    return alpha_ref_;
  }
  // D3DRS_SCISSORTESTENABLE, which clips every draw to the device's scissor rectangle (IDirect3DDevice9::SetScissorRect)
  bool scissor_test_enabled() const {
    return scissor_test_enable_ != FALSE;
  }
  // the lighting states a lit vertex reads (d3d9_lights.cpp): whether lighting is on at all, whether the specular term is
  // added, whether a transformed normal is renormalised, the device's ambient colour, and whether the material's diffuse
  // and ambient reflectances come from the vertex's colour (D3DRS_COLORVERTEX with each source's D3DMCS value)
  bool lighting_enabled() const {
    return lighting_ != FALSE;
  }
  bool specular_enabled() const {
    return specular_enable_ != FALSE;
  }
  bool normalize_normals() const {
    return normalize_normals_ != FALSE;
  }
  bool colour_vertex() const {
    return colour_vertex_ != FALSE;
  }
  DWORD ambient() const {
    return ambient_;
  }
  DWORD diffuse_source() const {
    return diffuse_source_;
  }
  DWORD ambient_source() const {
    return ambient_source_;
  }
  DWORD stencil_ref() const {
    return stencil_ref_;
  }
  bool fog_enabled() const {
    return fog_enable_ != FALSE;
  }
  DWORD fog_table_mode() const {
    return fog_table_mode_;
  }
  DWORD fog_vertex_mode() const {
    return fog_vertex_mode_;
  }
  DWORD fog_color() const {
    return fog_color_;
  }
  float fog_start() const {
    float value = 0.0f;
    memcpy(&value, &fog_start_, sizeof(value));
    return value;
  }
  float fog_end() const {
    float value = 0.0f;
    memcpy(&value, &fog_end_, sizeof(value));
    return value;
  }

private:
  // what D3D9's device starts with (the defaults D3DRENDERSTATETYPE names for each of these states). ZENABLE starts
  // TRUE only for a device whose chain made an automatic depth-stencil surface, which the device sets when it makes
  // one; a device made without EnableAutoDepthStencil starts with depth testing off, as D3D9's does (Microsoft Learn,
  // D3DRS_ZENABLE: "D3DZB_TRUE if a depth stencil was created along with the swap chain ... and D3DZB_FALSE otherwise").
  DWORD z_enable_ = FALSE;
  DWORD z_write_enable_ = TRUE;
  DWORD z_func_ = D3DCMP_LESSEQUAL;
  DWORD cull_mode_ = D3DCULL_CCW;
  DWORD fill_mode_ = D3DFILL_SOLID;
  DWORD shade_mode_ = D3DSHADE_GOURAUD;
  DWORD alpha_blend_enable_ = FALSE;
  DWORD src_blend_ = D3DBLEND_ONE;
  DWORD dest_blend_ = D3DBLEND_ZERO;
  DWORD blend_op_ = D3DBLENDOP_ADD;
  DWORD colour_write_enable_ = 0xF;
  DWORD texture_factor_ = 0xFFFFFFFF;
  // D3D9's defaults for the alpha test: off, always passing, reference 0
  DWORD alpha_test_enable_ = FALSE;
  DWORD alpha_func_ = D3DCMP_ALWAYS;
  DWORD alpha_ref_ = 0;
  DWORD scissor_test_enable_ = FALSE;
  // D3D9's documented defaults: lighting on, the specular term off, no ambient, normals as the game gives them, and the
  // material's diffuse and ambient taken from the vertex's colour when it carries one
  DWORD lighting_ = TRUE;
  DWORD specular_enable_ = FALSE;
  DWORD ambient_ = 0;
  DWORD normalize_normals_ = FALSE;
  DWORD colour_vertex_ = TRUE;
  DWORD diffuse_source_ = D3DMCS_COLOR1;
  DWORD ambient_source_ = D3DMCS_COLOR1;
  // D3D9's stencil test, as D3D11's depth-stencil state: off, always passing, keep on every outcome, and a reference
  // of 0 with every bit of the stencil writable (Microsoft Learn, D3DRS_STENCILENABLE)
  DWORD stencil_enable_ = FALSE;
  DWORD stencil_fail_ = D3DSTENCILOP_KEEP;
  DWORD stencil_zfail_ = D3DSTENCILOP_KEEP;
  DWORD stencil_pass_ = D3DSTENCILOP_KEEP;
  DWORD stencil_func_ = D3DCMP_ALWAYS;
  DWORD stencil_ref_ = 0;
  DWORD stencil_mask_ = 0xFFFFFFFF;
  DWORD stencil_write_mask_ = 0xFFFFFFFF;
  // fog is stored so GetRenderState matches SetRenderState; RasterCaps names none until a pixel test reads it
  DWORD fog_enable_ = FALSE;
  DWORD fog_color_ = 0;
  DWORD fog_table_mode_ = D3DFOG_NONE;
  DWORD fog_vertex_mode_ = D3DFOG_NONE;
  DWORD fog_start_ = 0;
  DWORD fog_end_ = 0x3F800000; // 1.0f
  DWORD fog_density_ = 0x3F800000;
  DWORD fog_range_enable_ = FALSE;

  // the objects the values made, and whether one of them is out of date
  ComPtr<ID3D11RasterizerState> rasterizer_;
  ComPtr<ID3D11DepthStencilState> depth_stencil_;
  ComPtr<ID3D11BlendState> blend_;
  bool rasterizer_current_ = false;
  bool depth_stencil_current_ = false;
  bool blend_current_ = false;
};

} // namespace mullion::d3d9
