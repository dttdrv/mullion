// Direct3D 9's fixed-function lights and material: the eight lights a device has (D3DCAPS9::MaxActiveLights), which of
// them is enabled, and the material a lit vertex's reflectances come from (IDirect3DDevice9::SetLight, LightEnable,
// SetMaterial; Microsoft Learn, "Mathematics of Direct3D Lighting").
//
// A lit vertex's colour is the device's ambient (D3DRS_AMBIENT) plus the material's emissive, plus every enabled light's
// contribution:
//
//   material.Ambient  * light.Ambient
// + material.Diffuse  * light.Diffuse  * max(0, n . L) * attenuation
// + material.Specular * light.Specular * max(0, n . H)^material.Power * attenuation   (D3DRS_SPECULARENABLE)
//
// L points from the vertex to the light, H is the half vector between L and the eye, and attenuation is
// 1 / (Attenuation0 + Attenuation1 * d + Attenuation2 * d * d) for a D3DLIGHT_POINT, nothing past the light's Range, and
// 1 for a D3DLIGHT_DIRECTIONAL. A D3DLIGHT_SPOT is a point light whose intensity also falls off outside its cone
// (Theta/Phi/Falloff). The device's world matrix turns a normal by its inverse transpose, and the eye is the world
// position the view matrix puts at its origin (that is what a specular highlight's half vector needs).
// https://learn.microsoft.com/en-us/windows/win32/direct3d9/mathematics-of-direct3d-lighting
//
// The vertex shader reads the whole of it as one constant buffer (d3d9_lighting_shaders.hpp), so what a draw binds is
// this structure: the state a game set, laid out as the shader declares it.
#pragma once
#include "d3d9_common.hpp"

namespace mullion::d3d9 {

class RenderState;

// the constant buffer the lit vertex shader reads, at b1: every field a float4 or a float4x4 so that its layout is the
// shader's without packing rules in the way
struct LightConstants {
  // the world matrix, and its inverse transpose for the normals (D3D9 transforms a normal by the inverse transpose of
  // the world matrix, and renormalises it when D3DRS_NORMALIZENORMALS says so)
  float world[16];
  float normal_matrix[16];
  float material_ambient[4];
  float material_diffuse[4];
  float material_specular[4];
  float material_emissive[4];
  // D3DRS_AMBIENT, once per draw
  float global_ambient[4];
  // the eye in world space (xyz), and the material's specular power (w)
  float eye_power[4];
  // the eight lights: their colours, a point light's position (xyz) and Range (w), a directional light's Direction
  // (xyz), a spot light's falloff exponent (w), the attenuation terms (xyz) and the light's D3DLIGHTTYPE (w), whether it
  // is enabled (x), and a spot light's cos(Theta/2) and cos(Phi/2)
  float light_ambient[8][4];
  float light_diffuse[8][4];
  float light_specular[8][4];
  float light_position_range[8][4];
  float light_direction_falloff[8][4];
  float light_attenuation_type[8][4];
  float light_enabled[8][4];
  float light_spot[8][4];
  // x: D3DRS_SPECULARENABLE; y: take the diffuse from the vertex (D3DRS_COLORVERTEX with D3DMCS_COLOR1); z: renormalise
  // the transformed normal; w: D3DRS_LIGHTING (a lit draw whose lighting is off is the vertex's own colour)
  float flags[4];
  // x: take the material's ambient from the vertex (D3DRS_COLORVERTEX with D3DMCS_COLOR1)
  float flags2[4];
};

// D3D9's state for the lights and the material, over the render states that gate them
class Lights {
public:
  // D3DCAPS9::MaxActiveLights, the lights a device has: D3D9's own maximum, and the index a call is refused past
  static constexpr UINT kMax = 8;

  Lights();

  // IDirect3DDevice9's four light calls and its two material calls: an index past kMax is D3DERR_INVALIDCALL, as D3D9's
  // is, and so is a null light, material or output
  HRESULT set_light(DWORD index, const D3DLIGHT9 *light);
  HRESULT get_light(DWORD index, D3DLIGHT9 *light) const;
  HRESULT enable_light(DWORD index, WINBOOL enable);
  HRESULT get_light_enable(DWORD index, WINBOOL *enable) const;
  HRESULT set_material(const D3DMATERIAL9 *material);
  HRESULT get_material(D3DMATERIAL9 *material) const;

  // what a lit draw under the device's `world` and `view` matrices, and `render_state`'s lighting states, reads: the
  // material, the global ambient, the eye position and the enabled lights
  void constants(const D3DMATRIX &world, const D3DMATRIX &view, const RenderState &render_state,
                 LightConstants &out) const;

private:
  D3DLIGHT9 lights_[kMax]{};
  WINBOOL enabled_[kMax]{};
  // D3D9's default material is its diffuse white and the rest nothing, which is the one a lit draw that never calls
  // SetMaterial is shaded with
  D3DMATERIAL9 material_{};
};

} // namespace mullion::d3d9
