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
  // light 0 is D3D9's default light: a white directional light along +z, disabled until LightEnable. Wine's visual.c
  // lighting_test enables it without ever calling SetLight and expects the black face it lit to be the white light's
  lights_[0].Type = D3DLIGHT_DIRECTIONAL;
  lights_[0].Diffuse.r = 1.0f;
  lights_[0].Diffuse.g = 1.0f;
  lights_[0].Diffuse.b = 1.0f;
  lights_[0].Diffuse.a = 0.0f;
  lights_[0].Direction = {0.0f, 0.0f, 1.0f};
}

HRESULT
Lights::set_light(DWORD index, const D3DLIGHT9 *light) {
  if (index >= kSlots || !light)
    return D3DERR_INVALIDCALL;
  lights_[index] = *light;
  defined_[index] = true;
  return S_OK;
}

HRESULT
Lights::get_light(DWORD index, D3DLIGHT9 *light) const {
  if (index >= kSlots || !light || !defined_[index])
    return D3DERR_INVALIDCALL;
  *light = lights_[index];
  return S_OK;
}

HRESULT
Lights::enable_light(DWORD index, WINBOOL enable) {
  if (index >= kSlots)
    return D3DERR_INVALIDCALL;
  // D3D9's light-enable state is 0x80 for an enabled light, not the TRUE bit: Wine's stateblock test records 0x80
  // after SetLight + LightEnable(TRUE). The lit path only asks whether the value is nonzero.
  enabled_[index] = enable ? 0x80 : FALSE;
  enable_known_[index] = true;
  return S_OK;
}

HRESULT
Lights::get_light_enable(DWORD index, WINBOOL *enable) const {
  if (index >= kSlots || !enable)
    return D3DERR_INVALIDCALL;
  // SetLight or LightEnable makes the enable state readable (Wine device.c enables without SetLight first)
  if (!defined_[index] && !enable_known_[index])
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
    // D3D9's lighting drops every light when the world matrix is not affine: Wine visual.c lighting_test's
    // "Lit quad with non-affine matrix" is black with a white light enabled, because a matrix whose last column is
    // not (0, 0, 0, 1) has no inverse transpose to put a normal through. The global ambient and the material's
    // emissive still apply, which the shader reads outside the lights.
    const bool affine = world.m[0][3] == 0.0f && world.m[1][3] == 0.0f && world.m[2][3] == 0.0f &&
                        world.m[3][3] == 1.0f;
    out.light_enabled[at][0] = (enabled_[at] && affine) ? 1.0f : 0.0f;
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
  out.flags2[1] = render_state.local_viewer() ? 1.0f : 0.0f;
  out.flags2[2] = 0.0f;
  out.flags2[3] = 0.0f;
  const bool colour_vertex = render_state.colour_vertex();
  out.material_sources[0] = colour_vertex ? (float)render_state.ambient_source() : 0.0f;
  out.material_sources[1] = colour_vertex ? (float)render_state.emissive_source() : 0.0f;
  out.material_sources[2] = colour_vertex ? (float)render_state.diffuse_source() : 0.0f;
  out.material_sources[3] = colour_vertex ? (float)render_state.specular_source() : 0.0f;
  // the infinite viewer is along the view matrix's forward axis (D3D's view looks down +z), so the direction from a
  // vertex to it is its negation, the same at every vertex
  out.view_forward[0] = -view.m[0][2];
  out.view_forward[1] = -view.m[1][2];
  out.view_forward[2] = -view.m[2][2];
}

namespace {

float
dot3(const float *a, const float *b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void
normalise3(float *v) {
  const float length = std::sqrt(dot3(v, v));
  if (length > 0.0f)
    for (int at = 0; at < 3; at++)
      v[at] /= length;
}

float
saturate(float value) {
  return value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
}

float
lerp(float a, float b, float t) {
  return a + (b - a) * t;
}

} // namespace

// the shader's own steps in its own order (tests/d3d9/fixed_function_shaders.cpp's kLightingSource), so a vertex
// ProcessVertices lights is the colour a draw of the same vertex would have
void
light_vertex(const LightConstants &c, const float position[3], const float normal_in[3], const float colour[4],
             float out[4]) {
  float world_position[3];
  for (int column = 0; column < 3; column++)
    world_position[column] = position[0] * c.world[column] + position[1] * c.world[4 + column] +
                             position[2] * c.world[8 + column] + c.world[12 + column];

  // the one vertex colour this mirror has is COLOR1; a COLOR2 source falls back to it, which is what a ProcessVertices
  // of a format with one colour can mean
  float diffuse[4], ambient[4], emissive[4], specular_material[4];
  for (int at = 0; at < 4; at++) {
    auto pick = [&](float which, const float *material_value) -> float {
      if (which < 0.5f)
        return material_value[at];
      return colour[at];
    };
    diffuse[at] = pick(c.material_sources[2], c.material_diffuse);
    ambient[at] = pick(c.material_sources[0], c.material_ambient);
    emissive[at] = pick(c.material_sources[1], c.material_emissive);
    specular_material[at] = pick(c.material_sources[3], c.material_specular);
  }

  float normal[3];
  for (int column = 0; column < 3; column++)
    normal[column] = normal_in[0] * c.normal_matrix[column] + normal_in[1] * c.normal_matrix[4 + column] +
                     normal_in[2] * c.normal_matrix[8 + column];
  if (c.flags[2] > 0.5f)
    normalise3(normal);
  float to_eye[3];
  for (int at = 0; at < 3; at++)
    to_eye[at] = lerp(c.eye_power[at] - world_position[at], c.view_forward[at], 1.0f - c.flags2[1]);
  normalise3(to_eye);

  float ambient_sum[3], lit[3];
  for (int at = 0; at < 3; at++) {
    ambient_sum[at] = c.global_ambient[at];
    lit[at] = emissive[at];
  }

  for (UINT light = 0; light < Lights::kMax; light++) {
    if (c.light_enabled[light][0] <= 0.5f)
      continue;
    const float type = c.light_attenuation_type[light][3];
    const float directional = type >= 2.5f ? 1.0f : 0.0f;
    float delta[3] = {c.light_position_range[light][0] - world_position[0],
                      c.light_position_range[light][1] - world_position[1],
                      c.light_position_range[light][2] - world_position[2]};
    const float distance = std::fmax(std::sqrt(dot3(delta, delta)), 1e-6f);
    float direction[3] = {c.light_direction_falloff[light][0] + 1e-6f, c.light_direction_falloff[light][1] - 1e-6f,
                          c.light_direction_falloff[light][2] + 1e-6f};
    normalise3(direction);
    float to_light[3];
    for (int at = 0; at < 3; at++)
      to_light[at] = lerp(delta[at] / distance, -direction[at], directional);

    const float falloff = std::fmax(c.light_attenuation_type[light][0] + c.light_attenuation_type[light][1] * distance +
                                        c.light_attenuation_type[light][2] * distance * distance,
                                    1e-6f);
    float attenuation = 1.0f / falloff;
    attenuation = lerp(attenuation * (distance <= c.light_position_range[light][3] ? 1.0f : 0.0f), 1.0f, directional);

    float from_light[3] = {-delta[0] / distance, -delta[1] / distance, -delta[2] / distance};
    const float cd = dot3(from_light, direction);
    const float cos_theta = c.light_spot[light][0];
    const float cos_phi = c.light_spot[light][1];
    const float cone = saturate((cd - cos_phi) / std::fmax(cos_theta - cos_phi, 1e-5f));
    const float spot = std::pow(cone, std::fmax(c.light_direction_falloff[light][3], 1.0f)) * (cd >= cos_phi ? 1.0f : 0.0f);
    attenuation = lerp(attenuation, attenuation * spot, (1.0f - directional) * (type >= 1.5f ? 1.0f : 0.0f));

    const float n_dot_l = saturate(dot3(normal, to_light));
    float half[3] = {to_light[0] + to_eye[0], to_light[1] + to_eye[1], to_light[2] + to_eye[2]};
    normalise3(half);
    const float n_dot_h = saturate(dot3(normal, half));
    const float specular =
        std::pow(n_dot_h, std::fmax(c.eye_power[3], 0.0f)) * c.flags[0] * (n_dot_l >= 1e-5f ? 1.0f : 0.0f);
    for (int at = 0; at < 3; at++) {
      ambient_sum[at] += attenuation * c.light_ambient[light][at];
      lit[at] += attenuation * (diffuse[at] * c.light_diffuse[light][at] * n_dot_l +
                                specular_material[at] * c.light_specular[light][at] * specular);
    }
  }

  for (int at = 0; at < 3; at++)
    out[at] = lerp(colour[at], saturate(ambient[at] * ambient_sum[at] + lit[at]), c.flags[3]);
  out[3] = lerp(colour[3], diffuse[3], c.flags[3]);
}

} // namespace mullion::d3d9
