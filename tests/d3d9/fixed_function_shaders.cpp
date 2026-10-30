// The generator of src/d3d9/d3d9_transform_shaders.hpp and src/d3d9/d3d9_lighting_shaders.hpp, not a test: the
// fixed-function pipeline's vertex shaders, compiled once, offline, with Microsoft's HLSL compiler (the
// d3dcompiler_47.dll a Wine prefix's applications load, the same one tests/d3d9's shaders are compiled by), and written
// out as the bytes D3D11CreateVertexShader takes. Run it under the tests' Wine from the repository's root:
//
//   toolchains/wine/bin/wine buildfresh/tests/d3d9/fixed_function_shaders.exe src/d3d9
//
// and commit the headers it writes. The library never compiles a shader at runtime: a game's shaders are the only DXBC
// it translates, and these are its own (src/d3d9/d3d9_fixed_function_shaders.hpp says the same of the first blobs).
//
// The transform is Direct3D 9's: a D3DFVF_XYZ vertex is a row vector multiplied by the world, view and projection
// matrices the device holds, in that order (Microsoft Learn, "Transforms": v' = v * World * View * Projection), and the
// rasterizer divides by w and maps the result through the viewport the device binds — x and y into the viewport's
// rectangle, z into its MinZ..MaxZ range (D3DVIEWPORT9). Doing the divide and the mapping after the shader is what the
// hardware does for a game's own vertex shader too.
// https://learn.microsoft.com/en-us/windows/win32/direct3d9/transforms
//
// The lighting is Direct3D 9's fixed-function lighting, computed per vertex against the device's eight lights
// (src/d3d9/d3d9_lights.hpp carries the equation and its sources):
//
//   colour = D3DRS_AMBIENT + material.Emissive + sum(enabled light: material.Ambient * light.Ambient)
//          + sum(enabled light: attenuation * (material.Diffuse * light.Diffuse * max(0, n.L)
//                                             + material.Specular * light.Specular * max(0, n.H)^power))
//
// where a point light's attenuation is 1 / (A0 + A1 d + A2 d^2), nothing past its Range, a spot light's cone multiplies
// it, and a directional light has neither. The normal is the vertex's turned by the inverse transpose of the world
// matrix; the eye is the world position the view matrix puts at its origin.
// https://learn.microsoft.com/en-us/windows/win32/direct3d9/mathematics-of-direct3d-lighting
#include <windows.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Shader {
  const char *entry;
  const char *name;
  bool textured;
};

// the untextured and textured variants of the transform's vertex shader. Both carry the colour the pixel shaders read
// through COLOR0, and the textured one the stage's texture coordinates, which its pixel shader samples with.
//
// The matrix is row_major so that the constant buffer holds what a D3DMATRIX holds: D3D9's matrices are row-major, and
// a row vector times one is the clip position. Left at the default, a float4x4 constant would be stored a column to a
// register and the device would have to transpose every matrix it uploads.
const char kTransformSource[] = R"(
cbuffer Transform : register(b0) {
  row_major float4x4 world_view_projection;   // Direct3D 9's world * view * projection, for row vectors
};

struct VSInput {
  float3 position : POSITION;
  float4 colour : COLOR0;
#ifdef TEXTURED
  float2 uv : TEXCOORD0;
#endif
};

struct VSOutput {
  float4 position : SV_POSITION;
  float4 colour : COLOR0;
#ifdef TEXTURED
  float2 uv : TEXCOORD0;
#endif
};

VSOutput main(VSInput input) {
  VSOutput output;
  // a row vector times the composed matrix is the clip position: the divide by w and the viewport's mapping are the
  // rasterizer's, from the viewport the device binds
  output.position = mul(float4(input.position, 1.0), world_view_projection);
  output.colour = input.colour;
#ifdef TEXTURED
  output.uv = input.uv;
#endif
  return output;
}
)";

// the same transform with Direct3D 9's lighting in front of it: the vertex's colour is what the lights make of its
// normal, or its own colour while D3DRS_LIGHTING is off. b0 is the composed transform the unlit shader reads, and b1 the
// lights, the material and the lighting states (src/d3d9/d3d9_lights.cpp's LightConstants, field for field).
const char kLightingSource[] = R"(
cbuffer Transform : register(b0) {
  row_major float4x4 world_view_projection;
};

cbuffer Lighting : register(b1) {
  row_major float4x4 world;
  row_major float4x4 normal_matrix;
  float4 material_ambient;
  float4 material_diffuse;
  float4 material_specular;
  float4 material_emissive;
  float4 global_ambient;
  float4 eye_power;                       // xyz: the eye in world space, w: the material's specular power
  float4 light_ambient[8];
  float4 light_diffuse[8];
  float4 light_specular[8];
  float4 light_position_range[8];         // xyz: a point light's position, w: its Range
  float4 light_direction_falloff[8];      // xyz: a directional or spot light's direction, w: the spot falloff exponent
  float4 light_attenuation_type[8];       // xyz: Attenuation0..2, w: the D3DLIGHTTYPE
  float4 light_enabled[8];                // x: 1 while the light is enabled
  float4 light_spot[8];                   // x: cos(Theta / 2), y: cos(Phi / 2)
  float4 flags;                           // x: D3DRS_SPECULARENABLE, y: diffuse from the vertex, z: renormalise the
                                          // normal, w: lighting on
  float4 flags2;                          // x: the material's ambient from the vertex
};

struct VSInput {
  float3 position : POSITION;
  float3 normal : NORMAL;
  float4 colour : COLOR0;
#ifdef TEXTURED
  float2 uv : TEXCOORD0;
#endif
};

struct VSOutput {
  float4 position : SV_POSITION;
  float4 colour : COLOR0;
#ifdef TEXTURED
  float2 uv : TEXCOORD0;
#endif
};

VSOutput main(VSInput input) {
  VSOutput output;
  const float4 model = float4(input.position, 1.0);
  output.position = mul(model, world_view_projection);
  const float3 world_position = mul(model, world).xyz;

  // the material's reflectances, and the ones D3DRS_COLORVERTEX takes from the vertex's colour
  const float4 diffuse = lerp(material_diffuse, input.colour, flags.y);
  const float4 ambient = lerp(material_ambient, input.colour, flags2.x);

  // a normal is turned by the inverse transpose of the world matrix, and renormalised when the device asks for it
  float3 normal = mul(float4(input.normal, 0.0), normal_matrix).xyz;
  normal = lerp(normal, normalize(normal), flags.z);
  const float3 to_eye = normalize(eye_power.xyz - world_position);

  float3 colour = global_ambient.rgb + material_emissive.rgb;

  // one light's contribution, with its index spelled in: the HLSL compiler these blobs are generated with has no loop
  // unrolling, and a constant buffer read has to be at a constant offset, so a loop over the lights does not compile.
  // LIGHT(0) .. LIGHT(7) are the eight lights D3D9's device has (D3DCAPS9::MaxActiveLights)
#define LIGHT(at)                                                                                                      \
  {                                                                                                                    \
    const float enabled = light_enabled[at].x;                                                                          \
    const float type = light_attenuation_type[at].w;                                                                    \
    const float directional = step(2.5, type);                                                                          \
                                                                                                                        \
    /* L: from the vertex to a point light, or the negation of a directional light's direction */                       \
    const float3 delta = light_position_range[at].xyz - world_position;                                                 \
    const float distance = max(length(delta), 1e-6);                                                                    \
    const float3 direction = normalize(light_direction_falloff[at].xyz + float3(1e-6, -1e-6, 1e-6));                    \
    const float3 to_light = lerp(delta / distance, -direction, directional);                                           \
                                                                                                                        \
    /* 1 / (A0 + A1 d + A2 d^2), nothing past the light's Range, and no attenuation at all for a directional one. the  \
       denominator is floored because a light whose three terms are all zero would divide by it */                      \
    const float falloff = max(light_attenuation_type[at].x + light_attenuation_type[at].y * distance +                 \
                                  light_attenuation_type[at].z * distance * distance,                               \
                              1e-6);                                                                                    \
    float attenuation = 1.0 / falloff;                                                                                  \
    attenuation = lerp(attenuation * step(distance, light_position_range[at].w), 1.0, directional);                    \
                                                                                                                        \
    /* a spot light's cone: full inside Theta, nothing past Phi, and a falloff between them */                          \
    const float cd = dot(-(delta / distance), direction);                                                               \
    const float cos_theta = light_spot[at].x;                                                                           \
    const float cos_phi = light_spot[at].y;                                                                             \
    const float cone = saturate((cd - cos_phi) / max(cos_theta - cos_phi, 1e-5));                                       \
    const float spot = pow(cone, max(light_direction_falloff[at].w, 1.0)) * step(cos_phi, cd);                          \
    attenuation = lerp(attenuation, attenuation * spot, (1.0 - directional) * step(1.5, type));                        \
                                                                                                                        \
    const float n_dot_l = saturate(dot(normal, to_light));                                                              \
    const float n_dot_h = saturate(dot(normal, normalize(to_light + to_eye)));                                          \
    const float specular = pow(n_dot_h, max(eye_power.w, 0.0)) * flags.x * step(1e-5, n_dot_l);                         \
                                                                                                                        \
    /* a light that is off contributes nothing, and it is skipped rather than multiplied by zero: a light nothing has  \
       been set for has every attenuation term zero and no Range, which a term above can make infinite, and zero       \
       times an infinite is a NaN that poisons the whole sum (it does not stay in the light's own term) */             \
    if (enabled > 0.5)                                                                                                  \
      colour += ambient.rgb * light_ambient[at].rgb +                                                                    \
                attenuation * (diffuse.rgb * light_diffuse[at].rgb * n_dot_l +                                            \
                               material_specular.rgb * light_specular[at].rgb * specular);                                \
  }

  LIGHT(0)
  LIGHT(1)
  LIGHT(2)
  LIGHT(3)
  LIGHT(4)
  LIGHT(5)
  LIGHT(6)
  LIGHT(7)

#undef LIGHT

  // a vertex drawn with D3DRS_LIGHTING off is its own colour, alpha and all
  output.colour = lerp(input.colour, float4(saturate(colour), diffuse.a), flags.w);
#ifdef TEXTURED
  output.uv = input.uv;
#endif
  return output;
}
)";

void
write_array(FILE *file, const char *name, const std::vector<unsigned char> &bytes) {
  fprintf(file, "\ninline const unsigned char %s[] = {\n", name);
  for (size_t at = 0; at < bytes.size(); at++) {
    if (at % 12 == 0)
      fprintf(file, "    ");
    fprintf(file, "0x%02x,", bytes[at]);
    if (at % 12 == 11 || at + 1 == bytes.size())
      fprintf(file, "\n");
    else
      fprintf(file, " ");
  }
  fprintf(file, "};\n");
}

bool
compile_profile(
    const char *source, const char *entry, const char *name, const char *profile, const D3D_SHADER_MACRO *macros,
    std::vector<unsigned char> &bytes
) {
  ID3DBlob *code = nullptr, *errors = nullptr;
  const HRESULT hr = D3DCompile(
      source, strlen(source), name, macros, nullptr, entry, profile,
      D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors
  );
  if (errors) {
    printf("%s\n", (const char *)errors->GetBufferPointer());
    errors->Release();
  }
  if (FAILED(hr) || !code) {
    printf("failed: %s does not compile: 0x%08lx\n", name, (unsigned long)hr);
    return false;
  }
  const unsigned char *first = (const unsigned char *)code->GetBufferPointer();
  bytes.assign(first, first + code->GetBufferSize());
  code->Release();
  return true;
}

bool
compile(const char *source, const char *entry, const char *name, std::vector<unsigned char> &bytes, bool textured) {
  D3D_SHADER_MACRO macros[2] = {{"TEXTURED", "1"}, {nullptr, nullptr}};
  return compile_profile(source, entry, name, "vs_5_0", textured ? macros : nullptr, bytes);
}

// extra stages, linear table fog and the fixed-function alpha test, as one pixel shader. stage 1's current argument is
// 3 so it reads stage 0's result rather than the vertex colour (stage 0 still encodes current as 1, which is the vertex
// colour). fog is D3D9's linear table fog of the pixel's depth (Microsoft Learn, D3DRS_FOGTABLEMODE). the alpha test
// multiplies the written alpha by 255 and compares it with D3DRS_ALPHAREF, as the translator does for a game's own
// pixel shader.
const char kExtendedPixelSource[] = R"(
cbuffer Stage : register(b0) {
  uint4 ops;
  uint4 args;
  float4 factor;
  uint4 ops1;
  uint4 args1;
  float4 fog;       // x enable, y start, z end
  float4 fogcolor;
  uint4 alpha;      // x enable, y func, z ref 0..255
  row_major float4x4 tex0;
};

#ifdef TEXTURED
Texture2D texture0 : register(t0);
Texture2D texture1 : register(t1);
SamplerState sampler0 : register(s0);
SamplerState sampler1 : register(s1);
#endif

float4 argument(uint which, float4 sampled, float4 colour, float4 current) {
  if (which == 0) return sampled;
  if (which == 1) return colour;
  if (which == 2) return factor;
  return current;
}

float4 blend(uint op, float4 first, float4 second, float4 current) {
  if (op == 1) return first;
  if (op == 2) return second;
  if (op == 3) return first * second;
  if (op == 4) return first + second;
  if (op == 5) return first - second;
  return current;
}

bool alpha_passes(float a, uint func, uint ref8) {
  const float value = a * 255.0;
  const float reference = (float)ref8;
  if (func == 1) return false;
  if (func == 2) return value < reference;
  if (func == 3) return value == reference;
  if (func == 4) return value <= reference;
  if (func == 5) return value > reference;
  if (func == 6) return value != reference;
  if (func == 7) return value >= reference;
  return true;
}

float4 apply_fog_alpha(float4 colour, float depth) {
  if (alpha.x != 0 && !alpha_passes(colour.a, alpha.y, alpha.z))
    discard;
  if (fog.x != 0) {
    const float span = max(fog.z - fog.y, 1e-5);
    const float factor_fog = saturate((fog.z - depth) / span);
    colour.rgb = lerp(fogcolor.rgb, colour.rgb, factor_fog);
  }
  return colour;
}

#ifdef TEXTURED
float4 shade(float4 colour, float2 uv, float depth) {
  const float2 uv0 = mul(float4(uv, 0.0, 1.0), tex0).xy;
  const float4 sampled0 = texture0.Sample(sampler0, uv0);
  float4 rgb = blend(ops.x, argument(ops.y, sampled0, colour, colour), argument(ops.z, sampled0, colour, colour), colour);
  float4 alpha0 = blend(ops.w, argument(args.x, sampled0, colour, colour), argument(args.y, sampled0, colour, colour), colour);
  float4 current = float4(rgb.rgb, alpha0.a);
  if (ops1.x != 0) {
    const float4 sampled1 = texture1.Sample(sampler1, uv);
    rgb = blend(ops1.x, argument(ops1.y, sampled1, colour, current), argument(ops1.z, sampled1, colour, current), current);
    alpha0 = blend(ops1.w, argument(args1.x, sampled1, colour, current), argument(args1.y, sampled1, colour, current), current);
    current = float4(rgb.rgb, alpha0.a);
  }
  return apply_fog_alpha(current, depth);
}
#else
float4 shade(float4 colour, float depth) {
  return apply_fog_alpha(colour, depth);
}
#endif

#ifdef FLAT
float4 main(float4 position : SV_POSITION, nointerpolation float4 colour : COLOR0
#ifdef TEXTURED
            , float2 uv : TEXCOORD0
#endif
            ) : SV_TARGET {
#ifdef TEXTURED
  return shade(colour, uv, position.z);
#else
  return shade(colour, position.z);
#endif
}
#else
struct PSInput {
  float4 position : SV_POSITION;
  float4 colour : COLOR0;
#ifdef TEXTURED
  float2 uv : TEXCOORD0;
#endif
};
float4 main(PSInput input) : SV_TARGET {
#ifdef TEXTURED
  return shade(input.colour, input.uv, input.position.z);
#else
  return shade(input.colour, input.position.z);
#endif
}
#endif
)";

bool
write_extended_header(const char *path) {
  D3D_SHADER_MACRO none[] = {{nullptr, nullptr}};
  D3D_SHADER_MACRO flat[] = {{"FLAT", "1"}, {nullptr, nullptr}};
  D3D_SHADER_MACRO textured[] = {{"TEXTURED", "1"}, {nullptr, nullptr}};
  D3D_SHADER_MACRO textured_flat[] = {{"TEXTURED", "1"}, {"FLAT", "1"}, {nullptr, nullptr}};
  std::vector<unsigned char> colour, colour_flat, sample, sample_flat;
  if (!compile_profile(kExtendedPixelSource, "main", "extended_colour", "ps_5_0", none, colour) ||
      !compile_profile(kExtendedPixelSource, "main", "extended_colour_flat", "ps_5_0", flat, colour_flat) ||
      !compile_profile(kExtendedPixelSource, "main", "extended_textured", "ps_5_0", textured, sample) ||
      !compile_profile(kExtendedPixelSource, "main", "extended_textured_flat", "ps_5_0", textured_flat, sample_flat))
    return false;

  FILE *file = fopen(path, "wb");
  if (!file) {
    printf("failed: no file %s\n", path);
    return false;
  }
  fprintf(file,
          "// generated by tests/d3d9/fixed_function_shaders.cpp: do not edit. the fixed-function pixel shaders that\n"
          "// apply a second blend stage, linear table fog and D3DRS_ALPHATESTENABLE. compiled offline at ps_5_0.\n"
          "#pragma once\n\nnamespace mullion::d3d9 {\n");
  write_array(file, "kExtendedColourPixelShader", colour);
  write_array(file, "kExtendedFlatColourPixelShader", colour_flat);
  write_array(file, "kExtendedTexturedPixelShader", sample);
  write_array(file, "kExtendedFlatTexturedPixelShader", sample_flat);
  fprintf(file, "\n} // namespace mullion::d3d9\n");
  fclose(file);
  printf("wrote %s (%zu %zu %zu %zu bytes)\n", path, colour.size(), colour_flat.size(), sample.size(),
         sample_flat.size());
  return true;
}

bool
write_header(const char *path, const char *comment, const char *source, const char *first_name, const char *second_name) {
  std::vector<unsigned char> plain, textured;
  if (!compile(source, "main", path, plain, false) || !compile(source, "main", path, textured, true))
    return false;

  FILE *file = fopen(path, "wb");
  if (!file) {
    printf("failed: no file %s\n", path);
    return false;
  }
  fprintf(file, "%s", comment);
  fprintf(file, "#pragma once\n\nnamespace mullion::d3d9 {\n");
  write_array(file, first_name, plain);
  write_array(file, second_name, textured);
  fprintf(file, "\n} // namespace mullion::d3d9\n");
  fclose(file);
  printf("wrote %s (%zu and %zu bytes)\n", path, plain.size(), textured.size());
  return true;
}

const char kTransformComment[] =
    "// generated by tests/d3d9/fixed_function_shaders.cpp: do not edit. the fixed-function transform's vertex\n"
    "// shaders, compiled offline from the source that file carries with Microsoft's HLSL compiler at vs_5_0. a\n"
    "// D3DFVF_XYZ vertex is a row vector multiplied by the world, view and projection matrices the device holds, in\n"
    "// that order (Microsoft Learn, \"Transforms\": v' = v * World * View * Projection); the rasterizer divides by w\n"
    "// and maps the result through the viewport the device binds, x and y into its rectangle and z into its\n"
    "// MinZ..MaxZ range (D3DVIEWPORT9). the pixel shaders these vertex shaders pair with are the fixed-function\n"
    "// ones the untextured and textured paths have always drawn through\n"
    "// (d3d9_fixed_function_shaders.hpp).\n"
    "//\n"
    "// its constant buffer, at b0, is the composed matrix as D3D9's row-major D3DMATRIX holds it, which is what\n"
    "// declaring the matrix row_major makes of the bytes:\n"
    "//   cbuffer Transform : register(b0) { row_major float4x4 world_view_projection; };\n";

const char kLightingComment[] =
    "// generated by tests/d3d9/fixed_function_shaders.cpp: do not edit. the fixed-function lighting's vertex\n"
    "// shaders, compiled offline from the source that file carries with Microsoft's HLSL compiler at vs_5_0. a\n"
    "// D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE vertex is transformed as the transform's shaders transform one,\n"
    "// and its colour is Direct3D 9's lighting of it: the device's ambient plus the material's emissive, the material's\n"
    "// ambient times each enabled light's, and for every enabled light its diffuse and specular through the vertex's\n"
    "// normal, the light's attenuation and (for a spot light) its cone (Microsoft Learn, \"Mathematics of Direct3D\n"
    "// Lighting\"). D3DRS_LIGHTING FALSE draws the vertex's own colour instead, which is the one lerp at the end.\n"
    "//\n"
    "// b0 is the composed world_view_projection the transform's shaders read, and b1 the lights, the material and the\n"
    "// lighting states, which is mullion::d3d9::LightConstants field for field (d3d9_lights.hpp):\n"
    "//   cbuffer Lighting : register(b1) { row_major float4x4 world; row_major float4x4 normal_matrix;\n"
    "//     float4 material_ambient, material_diffuse, material_specular, material_emissive, global_ambient, eye_power;\n"
    "//     float4 light_ambient[8], light_diffuse[8], light_specular[8], light_position_range[8],\n"
    "//            light_direction_falloff[8], light_attenuation_type[8], light_enabled[8], light_spot[8];\n"
    "//     float4 flags, flags2; };\n";

} // namespace

int
main(int argc, char **argv) {
  const std::string directory = argc > 1 ? argv[1] : "src/d3d9";
  if (!write_header((directory + "/d3d9_transform_shaders.hpp").c_str(), kTransformComment, kTransformSource,
                    "kTransformedVertexShader", "kTransformedTexturedVertexShader"))
    return 1;
  if (!write_header((directory + "/d3d9_lighting_shaders.hpp").c_str(), kLightingComment, kLightingSource,
                    "kLitVertexShader", "kLitTexturedVertexShader"))
    return 1;
  if (!write_extended_header((directory + "/d3d9_ff_extended_shaders.hpp").c_str()))
    return 1;
  return 0;
}
