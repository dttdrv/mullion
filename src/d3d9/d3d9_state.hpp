// the render states this milestone honours, as D3D9's defaults and as the D3D11 state objects a draw binds. a state
// that is not one of these is refused by the device rather than kept by nobody, and a value this build cannot turn
// into D3D11 state is refused too, so a frame is never drawn with a state its application did not ask for
// (src/d3d9/README).
#pragma once
#include "d3d9_common.hpp"

namespace mullion::d3d9 {

class RenderState {
public:
  RenderState();

  // whether the device takes this state at all, and whether this value of it is one a draw can honour
  static bool supported(D3DRENDERSTATETYPE state);
  static bool usable_value(D3DRENDERSTATETYPE state, DWORD value);

  HRESULT set(D3DRENDERSTATETYPE state, DWORD value);
  HRESULT get(D3DRENDERSTATETYPE state, DWORD *value) const;

  // the rasterizer, depth-stencil and blend states the values make, bound to the context for the next draw
  HRESULT apply(ID3D11Device *device, ID3D11DeviceContext *context);
  bool flat_shaded() const {
    return shade_mode_ == D3DSHADE_FLAT;
  }

private:
  // what D3D9's device starts with (the defaults its documentation names for each of these states)
  DWORD z_enable_ = TRUE;
  DWORD z_write_enable_ = TRUE;
  DWORD z_func_ = D3DCMP_LESSEQUAL;
  DWORD cull_mode_ = D3DCULL_CW;
  DWORD fill_mode_ = D3DFILL_SOLID;
  DWORD shade_mode_ = D3DSHADE_GOURAUD;
  DWORD alpha_blend_enable_ = FALSE;
  DWORD src_blend_ = D3DBLEND_SRCALPHA;
  DWORD dest_blend_ = D3DBLEND_INVSRCALPHA;
  DWORD blend_op_ = D3DBLENDOP_ADD;
  DWORD colour_write_enable_ = 0xF;

  // the objects the values made, and whether one of them is out of date
  ComPtr<ID3D11RasterizerState> rasterizer_;
  ComPtr<ID3D11DepthStencilState> depth_stencil_;
  ComPtr<ID3D11BlendState> blend_;
  bool rasterizer_current_ = false;
  bool depth_stencil_current_ = false;
  bool blend_current_ = false;
};

} // namespace mullion::d3d9
