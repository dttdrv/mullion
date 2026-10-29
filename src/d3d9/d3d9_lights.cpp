#include "d3d9_lights.hpp"
#include "d3d9_state.hpp"
#include <cmath>

namespace mullion::d3d9 {

namespace {

// D3D9's light types are D3DLIGHT9's, and the shader switches on them as the numbers D3DLIGHTTYPE has
static_assert(D3DLIGHT_POINT == 1 && D3DLIGHT_SPOT == 2 && D3DLIGHT_DIRECTIONAL == 3,
              "the lit vertex shader reads D3DLIGHTTYPE's numbers");

D3DCOLORVALUE
colour_of(DWORD colour) {
  D3DCOLORVALUE value{};
  value.r = ((colour >> 16) & 0xFF) / 255.0f;
  value.g = ((colour >> 8) & 0xFF) / 255.0f;
  value.b = (colour & 0xFF) / 255.0f;
  value.a = ((colour >> 24) & 0xFF) / 255.0f;
  return value;
}

void
store(float *out, const D3DCOLORVALUE &colour) {
  out[0] = colour.r;
  out[1] = colour.g;
  out[2] = colour.b;
  out[3] = colour.a;
}

// the inverse of an upper-left 3 by 3 block and of an affine matrix's translation, which is what a normal's inverse
// transpose and the eye position need. `inverse` is written in the source matrix's row-major order
bool
inverse_3x3(const D3DMATRIX &matrix, float *inverse) {
  const float a = matrix.m[0][0], b = matrix.m[0][1], c = matrix.m[0][2];
  const float d = matrix.m[1][0], e = matrix.m[1][1], f = matrix.m[1][2];
  const float g = matrix.m[2][0], h = matrix.m[2][1], i = matrix.m[2][2];
  const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
  if (std::fabs(det) < 1e-20f)
    return false;
  const float scale = 1.0f / det;
  inverse[0] = (e * i - f * h) * scale;
  inverse[1] = (c * h - b * i) * scale;
  inverse[2] = (b * f - c * e) * scale;
  inverse[3] = (f * g - d * i) * scale;
  inverse[4] = (a * i - c * g) * scale;
  inverse[5] = (c * d - a * f) * scale;
  inverse[6] = (d * h - e * g) * scale;
  inverse[7] = (b * g - a * h) * scale;
  inverse[8] = (a * e - b * d) * scale;
  return true;
}

} // namespace

Lights::Lights() {
  // D3D9's default material is its diffuse, white, and nothing else (a lit draw that never calls SetMaterial is shaded
  // with it), and every light starts disabled, so a lit frame with no light set is the device's ambient
  material_.Diffuse = colour_of(0xFFFFFFFF);
}

HRESULT
Lights::set_light(DWORD index, const D3DLIGHT9 *light) {
  if (index >= kMax || !light)
    return D3DERR_INVALIDCALL;
  lights_[index] = *light;
  return S_OK;
}

HRESULT
Lights::get_light(DWORD index, D3DLIGHT9 *light) const {
  if (index >= kMax || !light)
    return D3DERR_INVALIDCALL;
  *light = lights_[index];
  return S_OK;
}

HRESULT
Lights::enable_light(DWORD index, WINBOOL enable) {
  if (index >= kMax)
    return D3DERR_INVALIDCALL;
  enabled_[index] = enable ? TRUE : FALSE;
  return S_OK;
}

HRESULT
Lights::get_light_enable(DWORD index, WINBOOL *enable) const {
  if (index >= kMax || !enable)
    return D3DERR_INVALIDCALL;
  *enable = enabled_[index];
  return S_OK;
}

HRESULT
Lights::set_material(const D3DMATERIAL9 *material) {
  if (!material)
    return D3DERR_INVALIDCALL;
  material_ = *material;
  return S_OK;
}

HRESULT
Lights::get_material(D3DMATERIAL9 *material) const {
  if (!material)
    return D3DERR_INVALIDCALL;
  *material = material_;
  return S_OK;
}

void
Lights::constants(
    const D3DMATRIX &world, const D3DMATRIX &view, const RenderState &render_state, LightConstants &out
) const {
  out = LightConstants{};
  for (int row = 0; row < 4; row++)
    for (int column = 0; column < 4; column++)
      out.world[row * 4 + column] = world.m[row][column];

  // a normal is transformed by the inverse transpose of the world matrix (Microsoft Learn, "Mathematics of Direct3D
  // Lighting"): the inverse of the upper 3 by 3, transposed. a world with no inverse leaves the identity there, so a
  // normal is at least passed through rather than zeroed
  float normal[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
  float inverse[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
  if (inverse_3x3(world, inverse)) {
    for (int row = 0; row < 3; row++)
      for (int column = 0; column < 3; column++)
        normal[row * 4 + column] = inverse[column * 3 + row];
  }
  for (int at = 0; at < 16; at++)
    out.normal_matrix[at] = normal[at];

  // the material's four reflectances, and the ones D3DRS_COLORVERTEX takes from the vertex's colours: D3DMCS_MATERIAL
  // is the material's own, and D3DMCS_COLOR1 the vertex's diffuse, which is the one vertex colour this build's lit
  // formats carry (D3DMCS_COLOR2 needs a specular vertex colour, and the formats here have none)
  store(out.material_ambient, material_.Ambient);
  store(out.material_diffuse, material_.Diffuse);
  store(out.material_specular, material_.Specular);
  store(out.material_emissive, material_.Emissive);

  store(out.global_ambient, colour_of(render_state.ambient()));

  // the eye is the world position the view matrix puts at its own origin: the last row of the inverse of an affine view
  // matrix, which is -t * R^-1 for the translation t and the upper 3 by 3 R
  float eye[3] = {0.0f, 0.0f, 0.0f};
  float view_inverse[9];
  if (inverse_3x3(view, view_inverse)) {
    for (int column = 0; column < 3; column++)
      for (int at = 0; at < 3; at++)
        eye[column] -= view.m[3][at] * view_inverse[at * 3 + column];
  }
  out.eye_power[0] = eye[0];
  out.eye_power[1] = eye[1];
  out.eye_power[2] = eye[2];
  out.eye_power[3] = material_.Power;

  for (UINT at = 0; at < kMax; at++) {
    const D3DLIGHT9 &light = lights_[at];
    store(out.light_ambient[at], light.Ambient);
    store(out.light_diffuse[at], light.Diffuse);
    store(out.light_specular[at], light.Specular);
    out.light_position_range[at][0] = light.Position.x;
    out.light_position_range[at][1] = light.Position.y;
    out.light_position_range[at][2] = light.Position.z;
    out.light_position_range[at][3] = light.Range;
    out.light_direction_falloff[at][0] = light.Direction.x;
    out.light_direction_falloff[at][1] = light.Direction.y;
    out.light_direction_falloff[at][2] = light.Direction.z;
    out.light_direction_falloff[at][3] = light.Falloff;
    out.light_attenuation_type[at][0] = light.Attenuation0;
    out.light_attenuation_type[at][1] = light.Attenuation1;
    out.light_attenuation_type[at][2] = light.Attenuation2;
    out.light_attenuation_type[at][3] = (float)light.Type;
    out.light_enabled[at][0] = enabled_[at] ? 1.0f : 0.0f;
    // a spot light's cone is Theta (the angle of its fully lit inner cone) and Phi (the angle at which it ends), so the
    // cosines of their halves are what a direction is tested against
    out.light_spot[at][0] = std::cos(light.Theta * 0.5f);
    out.light_spot[at][1] = std::cos(light.Phi * 0.5f);
  }

  out.flags[0] = render_state.specular_enabled() ? 1.0f : 0.0f;
  out.flags[1] = (render_state.colour_vertex() && render_state.diffuse_source() == D3DMCS_COLOR1) ? 1.0f : 0.0f;
  out.flags[2] = render_state.normalize_normals() ? 1.0f : 0.0f;
  out.flags[3] = render_state.lighting_enabled() ? 1.0f : 0.0f;
  out.flags2[0] = (render_state.colour_vertex() && render_state.ambient_source() == D3DMCS_COLOR1) ? 1.0f : 0.0f;
}

} // namespace mullion::d3d9
