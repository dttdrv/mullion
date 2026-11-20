// The generator of src/d3d9/d3d9_transform_shaders.hpp, src/d3d9/d3d9_lighting_shaders.hpp,
// src/d3d9/d3d9_ff_extended_shaders.hpp and src/d3d9/d3d9_clip_blit_shaders.hpp, not a test: the fixed-function
// pipeline's shaders and StretchRect's blit, compiled once, offline, with Microsoft's HLSL compiler (the
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
//
// CLIP_PLANES adds D3D9's six user clip planes (IDirect3DDevice9::SetClipPlane): the device hands them to the shader in
// clip space at b2, a disabled plane as zero, and each one's distance is the plane dotted with the clip position, which
// the rasterizer clips where it is negative (SV_ClipDistance). The distances come after every other output, so the
// pixel shaders that read the unclipped shaders' outputs read the clipped ones' at the same registers.
const char kTransformSource[] = R"(
cbuffer Transform : register(b0) {
  row_major float4x4 world_view_projection;   // Direct3D 9's world * view * projection, for row vectors
};

#ifdef CLIP_PLANES
cbuffer ClipPlanes : register(b2) {
  float4 clip_planes[6];
};
#endif

struct VSInput {
  float3 position : POSITION;
#ifndef NODIFFUSE
  float4 colour : COLOR0;
#endif
#ifdef TEXTURED
#ifdef TEXCOORD3
  float3 uv : TEXCOORD0;
#else
  float2 uv : TEXCOORD0;
#endif
#endif
};

struct VSOutput {
  float4 position : SV_POSITION;
  float4 colour : COLOR0;
  // written even when the FVF carries no coordinates: a game pixel shader bound over the draw may read TEXCOORD0,
  // and D3D9's uninitialized varying is transparent black with w one
  float4 uv : TEXCOORD0;
#ifdef CLIP_PLANES
  float3 clip0 : SV_ClipDistance0;
  float3 clip1 : SV_ClipDistance1;
#endif
};

VSOutput main(VSInput input) {
  VSOutput output;
  // a row vector times the composed matrix is the clip position: the divide by w and the viewport's mapping are the
  // rasterizer's, from the viewport the device binds
  output.position = mul(float4(input.position, 1.0), world_view_projection);
#ifdef NODIFFUSE
  output.colour = 1.0;   // D3D9's default diffuse for a vertex format that carries none
#else
  output.colour = input.colour;
#endif
#ifdef TEXTURED
#ifdef TEXCOORD3
  output.uv = float4(input.uv, 1.0);
#else
  // D3D9 fills the missing components with one: a two-component coordinate is (u, v, 1, 1) for fixed-function
  // sampling. z selects a cube's or volume's face/slice (Wine visual.c test_updatetexture's cube case), while a game
  // pixel shader reads the coordinate the FVF names, whose missing components are zero and whose w is one
  // (visual.c ps_1_4 and its coissue shaders)
#ifdef USERPS
  output.uv = float4(input.uv, 0.0, 1.0);
#else
  output.uv = float4(input.uv, 1.0, 1.0);
#endif
#endif
#else
  output.uv = float4(0.0, 0.0, 0.0, 1.0);
#endif
#ifdef CLIP_PLANES
  output.clip0 = float3(dot(output.position, clip_planes[0]), dot(output.position, clip_planes[1]),
                        dot(output.position, clip_planes[2]));
  output.clip1 = float3(dot(output.position, clip_planes[3]), dot(output.position, clip_planes[4]),
                        dot(output.position, clip_planes[5]));
#endif
  return output;
}
)";

// the same transform with Direct3D 9's lighting in front of it: the vertex's colour is what the lights make of its
// normal, or its own colour while D3DRS_LIGHTING is off. b0 is the composed transform the unlit shader reads, and b1 the
// lights, the material and the lighting states (src/d3d9/d3d9_lights.cpp's LightConstants, field for field).
const char kLightingSource[] = R"(
#if !defined(NODIFFUSE) && !defined(DIFFUSE_COLOUR)
#define DIFFUSE_COLOUR 1
#endif
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
  float4 flags2;                          // x: the material's ambient from the vertex, y: D3DRS_LOCALVIEWER
  float4 view_forward;                    // xyz: the direction to the eye at infinity while D3DRS_LOCALVIEWER is off
  float4 material_sources;                // x ambient, y emissive, z diffuse, w specular: 0 material, 1 COLOR1, 2 COLOR2
};

#ifdef CLIP_PLANES
cbuffer ClipPlanes : register(b2) {
  float4 clip_planes[6];
};
#endif

struct VSInput {
  float3 position : POSITION;
#ifndef NONORMAL
  float3 normal : NORMAL;
#endif
#ifdef DIFFUSE_COLOUR
  float4 colour : COLOR0;
#endif
#ifdef SPECULAR_COLOUR
  float4 specular : COLOR1;
#endif
#ifdef TEXTURED
  float2 uv : TEXCOORD0;
#endif
};

// one material term: D3DMCS_MATERIAL keeps the material's own value, D3DMCS_COLOR1 the vertex's diffuse and
// D3DMCS_COLOR2 its specular. A format that carries no such colour falls back to the material's value.
float4 material_source(float which, float4 material_value, float4 colour1, float4 colour2) {
#if defined(DIFFUSE_COLOUR) && defined(SPECULAR_COLOUR)
  return which < 0.5 ? material_value : (which < 1.5 ? colour1 : colour2);
#elif defined(DIFFUSE_COLOUR)
  return which < 0.5 ? material_value : (which < 1.5 ? colour1 : material_value);
#elif defined(SPECULAR_COLOUR)
  return which < 0.5 ? material_value : (which < 1.5 ? material_value : colour2);
#else
  return material_value;
#endif
}

struct VSOutput {
  float4 position : SV_POSITION;
  float4 colour : COLOR0;
  float4 specular : COLOR1;   // the lighting's specular sum, or COLOR0's while D3DRS_LIGHTING is off
  // written even when the FVF carries no coordinates: see kTransformSource
  float4 uv : TEXCOORD0;
#ifdef CLIP_PLANES
  float3 clip0 : SV_ClipDistance0;
  float3 clip1 : SV_ClipDistance1;
#endif
};

VSOutput main(VSInput input) {
  VSOutput output;
  const float4 model = float4(input.position, 1.0);
  output.position = mul(model, world_view_projection);
  const float3 world_position = mul(model, world).xyz;
 
  // the material's reflectances, and the ones D3DRS_COLORVERTEX takes from the vertex's colour
  // the material's reflectances, and the ones D3DRS_COLORVERTEX takes from the vertex's colours
#ifdef DIFFUSE_COLOUR
  const float4 colour1 = input.colour;
#else
  const float4 colour1 = float4(0.0, 0.0, 0.0, 0.0);
#endif
#ifdef SPECULAR_COLOUR
  const float4 colour2 = input.specular;
#else
  const float4 colour2 = float4(0.0, 0.0, 0.0, 0.0);
#endif
  const float4 diffuse = material_source(material_sources.z, material_diffuse, colour1, colour2);
  const float4 ambient = material_source(material_sources.x, material_ambient, colour1, colour2);
  const float4 emissive = material_source(material_sources.y, material_emissive, colour1, colour2);
  const float4 specular_material = material_source(material_sources.w, material_specular, colour1, colour2);
 
  // a normal is turned by the inverse transpose of the world matrix, and renormalised when the device asks for it
#ifndef NONORMAL
  // a normal is turned by the inverse transpose of the world matrix, and renormalised when the device asks for it
  float3 normal = mul(float4(input.normal, 0.0), normal_matrix).xyz;
  normal = lerp(normal, normalize(normal), flags.z);
#else
  // no normal: a light has no direction to shade, so only the ambient and emissive terms survive
  const float3 normal = float3(0.0, 0.0, 0.0);
#endif
  // the infinite viewer is the same direction at every vertex; the local one points from the vertex to the eye
  const float3 to_eye = normalize(lerp(view_forward.xyz, eye_power.xyz - world_position, flags2.y));
 
  // the ambient is the material reflectance times the global and light ambients, as Wine's ffp shader ends:
  // material.ambient * ambient + material.diffuse * diffuse + material.emissive
  float3 ambient_sum = global_ambient.rgb;
  float3 diffuse_sum = emissive.rgb;
  float3 specular_sum = 0.0;   // the specular is its own output (COLOR1), clamped separately, like D3D9's oD1

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
    if (enabled > 0.5) {                                                                                                \
      ambient_sum += attenuation * light_ambient[at].rgb; \
      diffuse_sum += attenuation * diffuse.rgb * light_diffuse[at].rgb * n_dot_l; \
      specular_sum += attenuation * specular_material.rgb * light_specular[at].rgb * specular;                            \
    }                                                                                                                    \
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

  // a vertex drawn with D3DRS_LIGHTING off is its own colour, alpha and all; the specular passes straight through
  // instead of the lighting's sum while the lighting is on it the lighting's clamped one (D3DVBT... D3DRS_LIGHTING,
  // D3DFVF_SPECULAR)
  // the lighting's specular sum goes out on its own channel (COLOR1) whether the vertex shader is lit-shadowed with
  // D3DTA_SPECULAR or not; the pixel pipeline adds it after blending (Microsoft Learn, D3DRS_SPECULARENABLE). every lit
  // format this build draws carries no specular of its own, so the lighting off leaves it zero
#ifdef SPECULAR_COLOUR
  output.specular = flags.w != 0.0 ? float4(saturate(specular_sum), 1.0) : input.specular;
#else
  output.specular = flags.w != 0.0 ? float4(saturate(specular_sum), 1.0) : float4(0.0, 0.0, 0.0, 0.0);
#endif
#ifdef DIFFUSE_COLOUR
  output.colour = lerp(input.colour, float4(saturate(ambient.rgb * ambient_sum + diffuse_sum), diffuse.a), flags.w);
#else
  output.colour = lerp(float4(1.0, 1.0, 1.0, 1.0), float4(saturate(ambient.rgb * ambient_sum + diffuse_sum), diffuse.a), flags.w);
#endif
#ifdef TEXTURED
  output.uv = float4(input.uv, 0.0, 1.0);
#else
  output.uv = float4(0.0, 0.0, 0.0, 1.0);
#endif
#ifdef CLIP_PLANES
  output.clip0 = float3(dot(output.position, clip_planes[0]), dot(output.position, clip_planes[1]),
                        dot(output.position, clip_planes[2]));
  output.clip1 = float3(dot(output.position, clip_planes[3]), dot(output.position, clip_planes[4]),
                        dot(output.position, clip_planes[5]));
#endif
  return output;
}
)";

// the pre-transformed vertex shader D3D9's rasterizer takes for a D3DFVF_XYZRHW vertex of the eight-stage
// combiner. The screen-space position and colour are the pre-transformed shaders' exactly; the additions are the
// texture coordinate and the specular, which the combiner's pixel shader samples with:
//
// UVSIZE selects the FVF's texture coordinate element: 0 for none (the plain D3DFVF_XYZ[| SPECULAR]), 1 for float,
// 2 for float2, 3 for float3 and 4 for float4, which the FVF data alone distinguishes; UVTYPE is that element's HLSL
// type and UVSOURCE the float4 the pixel shader reads, following D3D9's fill of the components the FVF does not
// carry with 0 and 1 (D3DFVF_TEXCOORDSIZE*n* declares the coordinate's count; Wine's fixed-function vertex shaders
// read the attribute as vec4(attrib), which fills the tail the same way). Whatever the size, the stage's coordinate
// leaves the vertex shader as float4: the stage's texture transform, count crop and projection divide all read the
// four members, so the missing members' values matter even before the transform is applied. SPECULAR adds the
// D3DFVF_SPECULAR colour at COLOR1, which D3DTA_SPECULAR reads.
const char kPreTransformedEightStageSource[] = R"(
cbuffer Viewport : register(b0) {
  float4 viewport;                    // x, y, width, height
};

struct VSInput {
  float4 position : POSITION;         // xy in screen pixels, z, and 1/rhw
#ifndef NODIFFUSE
  float4 colour : COLOR0;
#endif
#if UVSIZE >= 1
  UVTYPE uv : TEXCOORD0;
#endif
#ifdef SPECULAR
  float4 specular : COLOR1;
#endif
};

struct VSOutput {
  float4 position : SV_POSITION;
  float4 colour : COLOR0;
#if UVSIZE >= 1
  float4 uv : TEXCOORD0;
#endif
#ifdef SPECULAR
  float4 specular : COLOR1;
#endif
};

VSOutput main(VSInput input) {
  VSOutput output;
  float w = 1.0 / input.position.w;   // D3D9's screen-space vertex carries rhw, the reciprocal of its w
  output.position = float4(((input.position.x - viewport.x) * 2.0 / viewport.z - 1.0) * w,
                           (1.0 - (input.position.y - viewport.y) * 2.0 / viewport.w) * w,
                           input.position.z * w, w);
#ifdef NODIFFUSE
  output.colour = 1.0;                // D3D9's default diffuse for a vertex format that carries none
#else
  output.colour = input.colour;
#endif
#if UVSIZE >= 1
  output.uv = UVSOURCE;
#endif
#ifdef SPECULAR
  output.specular = input.specular;
#endif
  return output;
}
)";

// the eight-stage combiner's pixel shader: the extended shader above generalised to D3D9's eight blend stages, the
// arguments D3DTA actually names on them, and every D3DTTff combination the formatted. The encodings are the blend
// contract the device writes per stage (d3d9_device_draw.cpp fills the buffer):
//   ops[s]:  x colour op, y colour arg1, z colour arg2, w alpha op -- the ops in the blend() encoding below
//            (0 DISABLE, 1 SELECTARG1, 2 SELECTARG2, 3 MODULATE, 4 ADD, 5 SUBTRACT), the args as the raw D3DTA
//            values, modifiers left in place (D3DTA_COMPLEMENT, D3DTA_ALPHAREPLICATE)
//   args[s]: x alpha arg1, y alpha arg2, both D3DTA raw; z the stage's D3DTSS_RESULTARG (5 = D3DTA_TEMP); w the
//            stage's D3DTSS_TEXTURETRANSFORMFLAGS (D3DTTFF values raw: 0 DISABLE, 1..4 COUNT1..COUNT4, or'ed with
//            256 PROJECTED)
//   constants[s]: the stage's D3DTSS_CONSTANT; factor: D3DRS_TEXTUREFACTOR
// A stage whose first colour argument is D3DTA_TEXTURE with no texture bound to the stage is not processed at all
// (Microsoft Learn, "Texture Blending"), which the device expresses by writing D3DTOP_DISABLE into that stage's
// colour op: the current flows through unchanged, and every stage past it is skipped with the whole chain -- which is
// D3DTOP_DISABLE's own behaviour ("disables texture operation for this texture stage and all stages with a greater
// index"). An alpha op keeps its own chain: a stage with an alpha op but a disabled colour op leaves the current's
// alpha alone too (the device never writes that combination).
//
// D3DTA_CURRENT at stage 0 is D3DTA_DIFFUSE; at the stages past it is the result the previous stage made. D3DTA_TEMP
// starts at zero every draw and a stage whose D3DTSS_RESULTARG is D3DTTA_TEMP leaves its result there for the stages
// after it. D3DTA_PREVIOUS is D3DTA_CURRENT by another name.
const char kEightStagePixelSource[] = R"(
cbuffer Stage : register(b0) {
  uint4 ops[8];        // per stage: colour op, colour arg1, colour arg2, alpha op, all raw D3DTEXTUREOP values
  uint4 args[8];       // per stage: alpha arg1, alpha arg2, the result arg (raw D3DTA), the D3DTTFF flags raw
  uint4 args0[8];      // per stage: colour arg0, alpha arg0 (raw D3DTA, MULTIPLYADD and LERP read them), the
                       // D3DTSS_TEXCOORDINDEX pass-through coordinate set the stage names, zero
  float4 constants[8]; // the stages' D3DTSS_CONSTANT
  float4 factor;       // D3DRS_TEXTUREFACTOR, common to the stages
  float4 fog;          // x enable, y start, z end, w density
  float4 fogcolor;
  uint4 alpha;         // x enable, y func, z ref 0..255, w table mode (1 exp, 2 exp2, else linear)
  uint4 misc;          // x: D3DRS_SPECULARENABLE (COLOR1 is added to the colour after blending, Wine glsl_shader.c)
  row_major float4x4 tex[8];   // the stages' texture matrices, row-major as D3DMATRIX holds them (c37-c68)
  uint4 dims[8];       // per stage x: 0 a 2D texture, 1 a cube, 2 a volume -- what the stage's resource is
};

// the texture objects and the coordinate transform are unconditioned (every combo binds all eight samplers, a stage
// with no texture bound gets the device's white fallback, so D3DTA_TEXTURE still reads 1): only the INPUT side is
// macro-gated, by whether the FVF carries the coordinates to start from. A stage samples the dimension its texture
// actually is -- 2D at t0..t7, cube at t8..t15, volume at t16..t23 -- selected by dims, since a cube's coordinate is
// a direction and a volume's a point in its box and neither can be read through a 2D declaration.
Texture2D texture2d[8] : register(t0);
TextureCube texture_cube[8] : register(t8);
Texture3D texture_3d[8] : register(t16);
SamplerState sampler0 : register(s0);
SamplerState sampler1 : register(s1);
SamplerState sampler2 : register(s2);
SamplerState sampler3 : register(s3);
SamplerState sampler4 : register(s4);
SamplerState sampler5 : register(s5);
SamplerState sampler6 : register(s6);
SamplerState sampler7 : register(s7);

float4 argument(uint raw, float4 sampled, float4 diffuse, float4 current, float4 temp, float4 constant_, float4 spec) {
  const uint which = raw & 0xF;   // D3DTA_SELECTMASK
  float4 v;
  if (which == 0) v = diffuse;      // D3DTA_DIFFUSE (CURRENT at stage 0)
  else if (which == 1) v = current; // D3DTA_CURRENT, D3DTA_PREVIOUS after stage 0
  else if (which == 2) v = sampled; // D3DTA_TEXTURE
  else if (which == 3) v = factor;  // D3DTA_TFACTOR
  else if (which == 4) v = spec;    // D3DTA_SPECULAR (COLOR1)
  else if (which == 5) v = temp;    // D3DTA_TEMP
  else if (which == 6) v = constant_;   // D3DTA_CONSTANT
  else v = current;
  if (raw & 0x20u) v = v.aaaa;    // D3DTA_ALPHAREPLICATE
  if (raw & 0x10u) v = 1.0 - v;   // D3DTA_COMPLEMENT
  return v;
}

// the texture operations, as Wine's ffp fragment ops generate them (toolchains/wine-8.17-src
// dlls/wined3d/glsl_shader.c shader_glsl_ffp_fragment_op, which follows Microsoft Learn's D3DTEXTUREOP formula
// table): `first` is arg1, `second` arg2, `zero` arg0 (MULTIPLYADD and LERP read it), `sampled` this stage's texture
// argument (BLENDTEXTUREALPHA and its PM form blend by it), `diffuse` the vertex colour, `factorf` the texture
// factor and `current` the stage chain's value before this stage. Colour results land in .rgb, alpha ones in .a, and
// each formula's masked substitution keeps both, which the caller picks apart.
float4 blend(
    uint op, float4 first, float4 second, float4 zero, float4 sampled, float4 diffuse, float4 factorf, float4 current
) {
  if (op == 2u) return first;                              // D3DTOP_SELECTARG1
  if (op == 3u) return second;                             // D3DTOP_SELECTARG2
  if (op == 4u) return first * second;                     // D3DTOP_MODULATE
  if (op == 5u) return clamp(first * second * 2.0, 0.0, 1.0);   // D3DTOP_MODULATE2X
  if (op == 6u) return clamp(first * second * 4.0, 0.0, 1.0);   // D3DTOP_MODULATE4X
  if (op == 7u) return clamp(first + second, 0.0, 1.0);    // D3DTOP_ADD
  if (op == 8u) return clamp(first + (second - 0.5), 0.0, 1.0); // D3DTOP_ADDSIGNED
  if (op == 9u) return clamp((first + (second - 0.5)) * 2.0, 0.0, 1.0); // D3DTOP_ADDSIGNED2X
  if (op == 10u) return clamp(first - second, 0.0, 1.0);   // D3DTOP_SUBTRACT
  if (op == 11u) return clamp((1.0 - first) * second + first, 0.0, 1.0); // D3DTOP_ADDSMOOTH
  if (op == 12u) return lerp(second, first, diffuse.w);    // D3DTOP_BLENDDIFFUSEALPHA
  if (op == 13u) return lerp(second, first, sampled.w);    // D3DTOP_BLENDTEXTUREALPHA
  if (op == 14u) return lerp(second, first, factorf.w);    // D3DTOP_BLENDFACTORALPHA
  if (op == 15u) return clamp(second * (1.0 - sampled.w) + first, 0.0, 1.0); // D3DTOP_BLENDTEXTUREALPHAPM
  if (op == 16u) return lerp(second, first, current.w);    // D3DTOP_BLENDCURRENTALPHA
  if (op == 18u) return clamp(second * first.w + first, 0.0, 1.0);      // D3DTOP_MODULATEALPHA_ADDCOLOR
  if (op == 19u)                                      // D3DTOP_MODULATECOLOR_ADDALPHA: the rgb modulates and adds the
    return float4(clamp(first.rgb * second.rgb + first.w, 0.0, 1.0),  // first's alpha; the alpha's masked substitution
                  clamp(first.w * second.w + first.w, 0.0, 1.0));     // multiplies and adds its own alpha like Wine's
  if (op == 20u) return clamp(second * (1.0 - first.w) + first, 0.0, 1.0); // D3DTOP_MODULATEINVALPHA_ADDCOLOR
  if (op == 21u)
    return float4(clamp((1.0 - first.rgb) * second.rgb + first.w, 0.0, 1.0),
                  clamp((1.0 - first.w) * second.w + first.w, 0.0, 1.0));  // D3DTOP_MODULATEINVCOLOR_ADDALPHA
  if (op == 24u) {                                          // D3DTOP_DOTPRODUCT3, to every channel like Wine does
    const float dot3 = clamp(dot(first.xyz - 0.5, second.xyz - 0.5) * 4.0, 0.0, 1.0);
    return float4(dot3, dot3, dot3, dot3);
  }
  if (op == 25u) return clamp(first * second + zero, 0.0, 1.0);          // D3DTOP_MULTIPLYADD
  if (op == 26u) return lerp(second, first, zero);                       // D3DTOP_LERP
  return current;                                                        // anything the device refused stays
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

float4 apply_fog(float4 colour, float depth) {
  // the fog blend is table fog (D3DFOG_* reads the default's w; none, exp or exp2) or the linear span, and it goes
  // on the colour AFTER the last stage and the specular add (Wine's ffp pixel pipeline order, glsl_shader.c)
  if (fog.x != 0) {
    float factor_fog;
    if (alpha.w == 1)
      factor_fog = exp(-depth * fog.w);
    else if (alpha.w == 2)
      factor_fog = exp(-(depth * fog.w) * (depth * fog.w));
    else {
      const float span = max(fog.z - fog.y, 1e-5);
      factor_fog = (fog.z - depth) / span;
    }
    factor_fog = saturate(factor_fog);
    colour.rgb = lerp(fogcolor.rgb, colour.rgb, factor_fog);
  }
  return colour;
}

void apply_alpha_test(float colour_a) {
  if (alpha.x != 0 && !alpha_passes(colour_a, alpha.y, alpha.z))
    discard;
}

float4 texture_coordinate(uint s, float4 uvraw) {
  // D3DTTFF_DISABLE and D3DTTFF_COUNT1 leave the coordinate alone; D3DTTFF_COUNT2 carries no matrix and no
  // projection divide (its divide is the coordinate's own tail, which the fill makes 1). The rest apply the stage's
  // texture matrix first, and a D3DTTFF_PROJECTED stage then divides: COUNT3|PROJECTED divides by the third
  // coordinate (Microsoft Learn, D3DTEXTURETRANSFORMFLAGS), every other projected combination by the fourth, which
  // D3DTTFF_PROJECTED alone acts like COUNT4. The division keeps all four members, and the stage that samples only
  // reads the first two.
  const uint flags = args[s].w;
  if (flags == 0 || flags == 1) return uvraw;
  if (flags == 2) return float4(uvraw.x, uvraw.y, 0.0, 1.0);
  float4 value = mul(uvraw, tex[s]);
  if (flags & 256u) {
    if (flags == (256u | 3u) || flags == 3u)   // COUNT3|PROJECTED, and the invalid 3-equivalent
      value = float4(value.xy / value.z, 0.0, 1.0);
    else
      value = float4(value.xy / value.w, 0.0, 1.0);
  }
  return value;
}

float4 sample_stage(uint s, float4 coordinate) {
  if (s == 0u) {
    if (dims[0].x == 1u) return texture_cube[0].Sample(sampler0, coordinate.xyz);
    if (dims[0].x == 2u) return texture_3d[0].Sample(sampler0, coordinate.xyz);
    return texture2d[0].Sample(sampler0, coordinate.xy);
  }
  if (s == 1u) {
    if (dims[1].x == 1u) return texture_cube[1].Sample(sampler1, coordinate.xyz);
    if (dims[1].x == 2u) return texture_3d[1].Sample(sampler1, coordinate.xyz);
    return texture2d[1].Sample(sampler1, coordinate.xy);
  }
  if (s == 2u) {
    if (dims[2].x == 1u) return texture_cube[2].Sample(sampler2, coordinate.xyz);
    if (dims[2].x == 2u) return texture_3d[2].Sample(sampler2, coordinate.xyz);
    return texture2d[2].Sample(sampler2, coordinate.xy);
  }
  if (s == 3u) {
    if (dims[3].x == 1u) return texture_cube[3].Sample(sampler3, coordinate.xyz);
    if (dims[3].x == 2u) return texture_3d[3].Sample(sampler3, coordinate.xyz);
    return texture2d[3].Sample(sampler3, coordinate.xy);
  }
  if (s == 4u) {
    if (dims[4].x == 1u) return texture_cube[4].Sample(sampler4, coordinate.xyz);
    if (dims[4].x == 2u) return texture_3d[4].Sample(sampler4, coordinate.xyz);
    return texture2d[4].Sample(sampler4, coordinate.xy);
  }
  if (s == 5u) {
    if (dims[5].x == 1u) return texture_cube[5].Sample(sampler5, coordinate.xyz);
    if (dims[5].x == 2u) return texture_3d[5].Sample(sampler5, coordinate.xyz);
    return texture2d[5].Sample(sampler5, coordinate.xy);
  }
  if (s == 6u) {
    if (dims[6].x == 1u) return texture_cube[6].Sample(sampler6, coordinate.xyz);
    if (dims[6].x == 2u) return texture_3d[6].Sample(sampler6, coordinate.xyz);
    return texture2d[6].Sample(sampler6, coordinate.xy);
  }
  if (dims[7].x == 1u) return texture_cube[7].Sample(sampler7, coordinate.xyz);
  if (dims[7].x == 2u) return texture_3d[7].Sample(sampler7, coordinate.xyz);
  return texture2d[7].Sample(sampler7, coordinate.xy);
}

float4 stage_uv(uint s, float4 uv0) {
  // D3DTSS_TEXCOORDINDEX's raw pass-through index (0-7, Wine state.c) names the FVF's coordinate set; a set the vertex
  // does not carry reads zero, like a default unbound attribute does (Wine glsl_shader.c's ffp vertex shaders)
  if (args0[s].z == 0u)
    return uv0;
  return float4(0.0, 0.0, 0.0, 0.0);
}

float4 shade(float4 diffuse, float4 spec, float4 uv) {
  float4 current = diffuse;   // stage 0's D3DTA_CURRENT is D3DTA_DIFFUSE
  float4 temp = 0.0;          // the temp register starts at zero (stage writes it before any stage reads it)

#define STAGE(s, object)                                                                                          \
  {                                                                                                                    \
    /* the raw D3DTOP_DISABLE (1); it stops this stage and every one after it, and a disabled colour      \
       operation skips the stage's alpha too */                                                                    \
    if (ops[s].x == 1u)                                                                                              \
      return current;                                                                                                \
    const float4 sampled_##s = sample_stage(s, texture_coordinate(s, stage_uv(s, uv)));                              \
    const float4 previous = current;                                                                                 \
    const float4 arg1_##s = argument(ops[s].y, sampled_##s, diffuse, previous, temp, constants[s], spec);            \
    const float4 arg2_##s = argument(ops[s].z, sampled_##s, diffuse, previous, temp, constants[s], spec);            \
    const float4 arg0_##s = argument(args0[s].x, sampled_##s, diffuse, previous, temp, constants[s], spec);          \
    /* the stage writes its result to D3DTSS_RESULTARG's register: D3DTA_TEMP leaves the current alone    */           \
    const bool temp_dst_##s = args[s].z == 5u;                                                                       \
    const float4 colour_out_##s = blend(ops[s].x, arg1_##s, arg2_##s, arg0_##s, sampled_##s, diffuse, factor,        \
                                        previous);                                                                   \
    const float4 alpha_arg1_##s = argument(args[s].x, sampled_##s, diffuse, previous, temp, constants[s], spec);    \
    const float4 alpha_arg2_##s = argument(args[s].y, sampled_##s, diffuse, previous, temp, constants[s], spec);    \
    const float4 alpha_arg0_##s = argument(args0[s].y, sampled_##s, diffuse, previous, temp, constants[s], spec);   \
    const float4 alpha_out_##s =                                                                                          \
        ops[s].w != 1u ? blend(ops[s].w, alpha_arg1_##s, alpha_arg2_##s, alpha_arg0_##s, sampled_##s, diffuse, factor,     \
                               previous)                                                                             \
                       : previous;   /* a disabled alpha operation leaves the current's alpha as it is */             \
    if (temp_dst_##s) {                                                                                              \
      temp.xyz = colour_out_##s.xyz;                                                                                 \
      temp.w = alpha_out_##s.a;                                                                                      \
    } else {                                                                                                         \
      current.xyz = colour_out_##s.xyz;                                                                              \
      current.w = alpha_out_##s.a;                                                                                   \
    }                                                                                                                \
  }

  STAGE(0, texture0)
  STAGE(1, texture1)
  STAGE(2, texture2)
  STAGE(3, texture3)
  STAGE(4, texture4)
  STAGE(5, texture5)
  STAGE(6, texture6)
  STAGE(7, texture7)
#undef STAGE

  return current;
}

#ifdef TEXTURED
#define EIGHT_UV float4 uv : TEXCOORD0;
#define EIGHT_UV_ARG input.uv
#else
#define EIGHT_UV
#define EIGHT_UV_ARG float4(0.0, 0.0, 0.0, 1.0)
#endif

#ifdef SPECULAR
#define EIGHT_SPEC , float4 specular_in : COLOR1
#else
#define EIGHT_SPEC
#endif

#ifdef FLAT
#define EIGHT_FLAT nointerpolation
#else
#define EIGHT_FLAT
#endif

struct PSInput {
  float4 position : SV_POSITION;
  EIGHT_FLAT float4 colour : COLOR0;
  EIGHT_UV
#ifdef SPECULAR
  float4 specular_in : COLOR1;
#endif
};

float4 main(PSInput input) : SV_TARGET {
#ifdef SPECULAR
  const float4 spec_in = input.specular_in;
#else
  const float4 spec_in = float4(0.0, 0.0, 0.0, 0.0);
#endif
  // the specular is added to the colour after blending (Wine's ffp pixel pipeline tail, glsl_shader.c), gated by
  // D3DRS_SPECULARENABLE, and the add is not clamped (Wine's ret + ffp_varying_specular * specular_enable); the fog
  // blend and the alpha test come after it, in Wine's order
  float4 result = shade(input.colour, spec_in, EIGHT_UV_ARG);
  result.rgb += spec_in.rgb * (float)misc.x;
  result = apply_fog(result, input.position.z);
  apply_alpha_test(result.a);
  return result;
}
)";

// StretchRect's blit: a strip of four vertices made from SV_VertexID covers the viewport the device sets to the
// destination rectangle, and each pixel samples the source at the matching point of the source rectangle, which b0
// holds as texture coordinates (left, top, right, bottom). The sampler is the call's filter, point or linear.
const char kBlitVertexSource[] = R"(
cbuffer Blit : register(b0) {
  float4 source_rect;
};

struct VSOutput {
  float4 position : SV_POSITION;
  float2 uv : TEXCOORD0;
};

VSOutput main(uint id : SV_VertexID) {
  const float2 corner = float2(id & 1, id >> 1);
  VSOutput output;
  output.position = float4(corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0, 0.0, 1.0);
  output.uv = lerp(source_rect.xy, source_rect.zw, corner);
  return output;
}
)";

const char kBlitPixelSource[] = R"(
Texture2D source : register(t0);
SamplerState source_sampler : register(s0);

float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
  return source.Sample(source_sampler, uv);
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

// extra stages, table fog and the fixed-function alpha test, as one pixel shader. stage 1's current argument is
// 3 so it reads stage 0's result rather than the vertex colour (stage 0 still encodes current as 1, which is the vertex
// colour). fog is D3D9's table fog of the pixel's depth: linear, exp or exp2, selected by alpha.w (Microsoft Learn,
// D3DRS_FOGTABLEMODE). the alpha test multiplies the written alpha by 255 and compares it with D3DRS_ALPHAREF, as the
// translator does for a game's own pixel shader.
const char kExtendedPixelSource[] = R"(
cbuffer Stage : register(b0) {
  uint4 ops;
  uint4 args;
  float4 factor;
  uint4 ops1;
  uint4 args1;
  float4 fog;       // x enable, y start, z end, w density
  float4 fogcolor;
  uint4 alpha;      // x enable, y func, z ref 0..255, w table mode (1 exp, 2 exp2, else linear)
  row_major float4x4 tex0;
  row_major float4x4 tex1;   // stage 1's D3DTSS_TEXTURE1 matrix, for its D3DTSS_TEXTURETRANSFORMFLAGS
  uint4 misc;       // x: D3DRS_SPECULARENABLE; z/w: stage 0/1 texture matrix enabled, w carrying stage 1's COUNT flags
  uint4 dims[2];    // per stage x: 0 a 2D texture, 1 a cube, 2 a volume -- what the stage's resource is
};

// A stage samples the dimension its texture actually is: a cube is a direction of three components and a volume a
// point in its box, so neither can be read through a 2D declaration (a cube view bound at a Texture2D slot is not a
// sample at all). The three declarations are bound at the same stage's three slots -- 2D at t0+t1, cube at t8+t9 and
// volume at t16+t17 -- and dims says which one the stage reads. A stage with no texture bound reads the device's
// white 2D texel, which is dims 0.
#ifdef TEXTURED
Texture2D texture2d[2] : register(t0);
TextureCube texture_cube[2] : register(t8);
Texture3D texture_3d[2] : register(t16);
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
    // D3DFOG_EXP is e^(-density * d), D3DFOG_EXP2 is e^(-(density * d)^2), and D3DFOG_LINEAR is (end - d) / (end - start)
    // (Microsoft Learn, D3DRS_FOGTABLEMODE). a mode this shader does not name stays on the linear formula
    float factor_fog;
    if (alpha.w == 1)
      factor_fog = exp(-depth * fog.w);
    else if (alpha.w == 2)
      factor_fog = exp(-(depth * fog.w) * (depth * fog.w));
    else {
      const float span = max(fog.z - fog.y, 1e-5);
      factor_fog = (fog.z - depth) / span;
    }
    factor_fog = saturate(factor_fog);
    colour.rgb = lerp(fogcolor.rgb, colour.rgb, factor_fog);
  }
  return colour;
}

#ifdef TEXTURED
float4 sample_stage(uint s, float4 coordinate) {
  if (s == 0u) {
    if (dims[0].x == 1u) return texture_cube[0].Sample(sampler0, coordinate.xyz);
    if (dims[0].x == 2u) return texture_3d[0].Sample(sampler0, coordinate.xyz);
    return texture2d[0].Sample(sampler0, coordinate.xy);
  }
  if (dims[1].x == 1u) return texture_cube[1].Sample(sampler1, coordinate.xyz);
  if (dims[1].x == 2u) return texture_3d[1].Sample(sampler1, coordinate.xyz);
  return texture2d[1].Sample(sampler1, coordinate.xy);
}

float4 shade(float4 colour, float4 spec, float4 uv, float4 uv1, float depth) {
  // the transform reads the coordinate the vertex shader wrote, whose missing components D3D9 fills with one; a
  // volume or cube stage samples the transformed xyz. misc.z says whether the stage's matrix is enabled: without a
  // matrix the stage samples the coordinate as written (Wine visual.c texture_transform_flags_test)
  float4 uv0 = uv;
  if (misc.z != 0)
    uv0 = mul(uv, tex0);
  const float4 sampled0 = sample_stage(0, uv0);
  // D3DTSS_TEXTURETRANSFORMFLAGS is per stage: stage 1's matrix and count apply to the coordinate stage 1 samples.
  // COUNT2 keeps only the transformed x and y, with z 0 and w 1; a PROJECTED stage divides after the matrix.
  float4 uv1_stage = uv1;
  const uint stage1_flags = misc.w;
  if (stage1_flags >= 2u) {
    uv1_stage = mul(uv1, tex1);
    if (stage1_flags == 2u) {
      uv1_stage = float4(uv1_stage.x, uv1_stage.y, 0.0, 1.0);
    } else if (stage1_flags & 256u) {
      if ((stage1_flags & 0xFu) == 3u)
        uv1_stage = float4(uv1_stage.xy / uv1_stage.z, 0.0, 1.0);
      else
        uv1_stage = float4(uv1_stage.xy / uv1_stage.w, 0.0, 1.0);
    }
  }
  // each stage's result is clamped before the next stage reads it: D3D9's texture blending saturates every stage's
  // output to [0, 1] (Wine visual.c texop_range_test: ADD of the white vertex colour twice is 1.0 when the next stage
  // subtracts the 0.5 texture factor, not 2.0 - 0.5)
  float4 rgb = saturate(blend(ops.x, argument(ops.y, sampled0, colour, colour), argument(ops.z, sampled0, colour, colour), colour));
  float4 alpha0 = saturate(blend(ops.w, argument(args.x, sampled0, colour, colour), argument(args.y, sampled0, colour, colour), colour));
  float4 current = float4(rgb.rgb, alpha0.a);
  if (ops1.x != 0) {
    const float4 sampled1 = sample_stage(1, uv1_stage);
    rgb = saturate(blend(ops1.x, argument(ops1.y, sampled1, colour, current), argument(ops1.z, sampled1, colour, current), current));
    alpha0 = saturate(blend(ops1.w, argument(args1.x, sampled1, colour, current), argument(args1.y, sampled1, colour, current), current));
    current = float4(rgb.rgb, alpha0.a);
  }
  // the specular add is not clamped and comes before the fog blend and the alpha test, in Wine's order
  // (fcp_pipeline's glsl_shader.c; the eight-stage combiner's pixel shader says the same)
  return apply_fog_alpha(current + spec * float4(misc.x, misc.x, misc.x, 0.0), depth);
}
#else
float4 shade(float4 colour, float4 spec, float depth) {
  return apply_fog_alpha(colour + spec * float4(misc.x, misc.x, misc.x, 0.0), depth);
}
#endif

// SPECULAR (D3DRS_SPECULARENABLE on a lit draw) adds the lighting's specular (a lit vertex shader's COLOR1) to the
// colour after the last blend stage: a second input, gated by b0's misc.x. A draw without it binds the shaders above,
// whose signatures the unlit and lit vertex shaders write the same (colour at register 1, uv at 2) -- the SPECULAR
// builds have the specular at 2 and the texture coordinates at 3, the lit vertex shader's own order.
#ifdef SPECULAR
#define SPEC_VALUE_FLAT float4 spec_value = spec;
#define SPEC_VALUE_INPUT float4 spec_value = input.spec;
#else
#define SPEC_VALUE_FLAT const float4 spec_value = float4(0.0, 0.0, 0.0, 0.0);
#define SPEC_VALUE_INPUT const float4 spec_value = float4(0.0, 0.0, 0.0, 0.0);
#endif

#ifdef FLAT
float4 main(float4 position : SV_POSITION, nointerpolation float4 colour : COLOR0
#ifdef SPECULAR
            , float4 spec : COLOR1
#endif
#ifdef TEXTURED
            , float4 uv : TEXCOORD0
#ifdef UV1
            , float4 uv1 : TEXCOORD1
#endif
#endif
            ) : SV_TARGET {
  SPEC_VALUE_FLAT
#ifdef TEXTURED
#ifdef UV1
  return shade(colour, spec_value, uv, uv1, position.z);
#else
  return shade(colour, spec_value, uv, uv, position.z);
#endif
#else
  return shade(colour, spec_value, position.z);
#endif
}
#else
struct PSInput {
  float4 position : SV_POSITION;
  float4 colour : COLOR0;
#ifdef SPECULAR
  float4 spec : COLOR1;
#endif
#ifdef TEXTURED
  float4 uv : TEXCOORD0;
#ifdef UV1
  float4 uv1 : TEXCOORD1;
#endif
#endif
};
float4 main(PSInput input) : SV_TARGET {
  SPEC_VALUE_INPUT
#ifdef TEXTURED
#ifdef UV1
  return shade(input.colour, spec_value, input.uv, input.uv1, input.position.z);
#else
  return shade(input.colour, spec_value, input.uv, input.uv, input.position.z);
#endif
#else
  return shade(input.colour, spec_value, input.position.z);
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
  D3D_SHADER_MACRO spec[] = {{"SPECULAR", "1"}, {nullptr, nullptr}};
  D3D_SHADER_MACRO spec_flat[] = {{"SPECULAR", "1"}, {"FLAT", "1"}, {nullptr, nullptr}};
  D3D_SHADER_MACRO spec_textured[] = {{"SPECULAR", "1"}, {"TEXTURED", "1"}, {nullptr, nullptr}};
  D3D_SHADER_MACRO spec_textured_flat[] = {{"SPECULAR", "1"}, {"TEXTURED", "1"}, {"FLAT", "1"}, {nullptr, nullptr}};
  D3D_SHADER_MACRO textured_uv1[] = {{"TEXTURED", "1"}, {"UV1", "1"}, {nullptr, nullptr}};
  D3D_SHADER_MACRO textured_flat_uv1[] = {{"TEXTURED", "1"}, {"UV1", "1"}, {"FLAT", "1"}, {nullptr, nullptr}};
  std::vector<unsigned char> colour, colour_flat, sample, sample_flat, sample_uv1, sample_flat_uv1;
  std::vector<unsigned char> spec_colour, spec_colour_flat, spec_sample, spec_sample_flat;
  if (!compile_profile(kExtendedPixelSource, "main", "extended_colour", "ps_5_0", none, colour) ||
      !compile_profile(kExtendedPixelSource, "main", "extended_colour_flat", "ps_5_0", flat, colour_flat) ||
      !compile_profile(kExtendedPixelSource, "main", "extended_textured", "ps_5_0", textured, sample) ||
      !compile_profile(kExtendedPixelSource, "main", "extended_textured_flat", "ps_5_0", textured_flat, sample_flat) ||
      !compile_profile(kExtendedPixelSource, "main", "extended_spec_colour", "ps_5_0", spec, spec_colour) ||
      !compile_profile(kExtendedPixelSource, "main", "extended_spec_colour_flat", "ps_5_0", spec_flat,
                       spec_colour_flat) ||
      !compile_profile(kExtendedPixelSource, "main", "extended_spec_textured", "ps_5_0", spec_textured,
                       spec_sample) ||
      !compile_profile(kExtendedPixelSource, "main", "extended_spec_textured_flat", "ps_5_0", spec_textured_flat,
                       spec_sample_flat) ||
      !compile_profile(kExtendedPixelSource, "main", "extended_textured_uv1", "ps_5_0", textured_uv1, sample_uv1) ||
      !compile_profile(kExtendedPixelSource, "main", "extended_textured_flat_uv1", "ps_5_0", textured_flat_uv1,
                       sample_flat_uv1))
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
  write_array(file, "kExtendedSpecColourPixelShader", spec_colour);
  write_array(file, "kExtendedSpecFlatColourPixelShader", spec_colour_flat);
  write_array(file, "kExtendedSpecTexturedPixelShader", spec_sample);
  write_array(file, "kExtendedSpecFlatTexturedPixelShader", spec_sample_flat);
  write_array(file, "kExtendedTexturedUv1PixelShader", sample_uv1);
  write_array(file, "kExtendedFlatTexturedUv1PixelShader", sample_flat_uv1);
  fprintf(file, "\n} // namespace mullion::d3d9\n");
  fclose(file);
  printf("wrote %s (%zu %zu %zu %zu %zu %zu %zu %zu bytes)\n", path, colour.size(), colour_flat.size(), sample.size(),
         sample_flat.size(), spec_colour.size(), spec_colour_flat.size(), spec_sample.size(), spec_sample_flat.size());
  return true;
}

// the transform's vertex shader, generalised the pre-transformed eight-stage source's way: UVSIZE selects the FVF's
// texture coordinate element (none, float, float2, float3 or float4; UVTYPE and UVSOURCE are the macro forms of
// that), SPECULAR adds D3DFVF_SPECULAR at COLOR1, and the stage's coordinate leaves as float4 with the components
// the FVF does not carry at 0 and 1. The transform itself is the composed matrix's, the same as the plain transform
// shaders kTransformSource compiles to -- this family only differs in its input signature, which D3D11's input
// layouts must match, and in carrying the coordinate and the specular the eight-stage combiner reads.
const char kTransformedEightStageSource[] = R"(
cbuffer Transform : register(b0) {
  row_major float4x4 world_view_projection;   // Direct3D 9's world * view * projection, for row vectors
};

#ifdef CLIP_PLANES
cbuffer ClipPlanes : register(b2) {
  float4 clip_planes[6];
};
#endif

struct VSInput {
  float3 position : POSITION;
#ifndef NODIFFUSE
  float4 colour : COLOR0;
#endif
#if UVSIZE >= 1
  UVTYPE uv : TEXCOORD0;
#endif
#ifdef SPECULAR
  float4 specular : COLOR1;
#endif
};

struct VSOutput {
  float4 position : SV_POSITION;
  float4 colour : COLOR0;
#ifdef SPECULAR
  float4 specular : COLOR1;
#endif
#if UVSIZE >= 1
  float4 uv : TEXCOORD0;
#endif
#ifdef CLIP_PLANES
  float3 clip0 : SV_ClipDistance0;
  float3 clip1 : SV_ClipDistance1;
#endif
};

VSOutput main(VSInput input) {
  VSOutput output;
  output.position = mul(float4(input.position, 1.0), world_view_projection);
#ifdef NODIFFUSE
  output.colour = 1.0;                // D3D9's default diffuse for a vertex format that carries none
#else
  output.colour = input.colour;
#endif
#ifdef SPECULAR
  output.specular = input.specular;
#endif
#if UVSIZE >= 1
  output.uv = UVSOURCE;
#endif
#ifdef CLIP_PLANES
  output.clip0 = float3(dot(output.position, clip_planes[0]), dot(output.position, clip_planes[1]),
                        dot(output.position, clip_planes[2]));
  output.clip1 = float3(dot(output.position, clip_planes[3]), dot(output.position, clip_planes[4]),
                        dot(output.position, clip_planes[5]));
#endif
  return output;
}
)";

// the eight-stage combiner's pixel shaders, and the vertex shaders its vertex formats need: the pre-transformed and
// transformed families D3DFVF_TEXCOORDSIZE1..4 and D3DFVF_SPECULAR need for the eight-stage pixel shader's inputs
// (a float4 TEXCOORD0 the combiner fills the FVF's missing members of, and a COLOR1 the D3DTA_SPECULAR argument
// reads), and the lit paths with the specular in and out. The pre-transformed UV2/UV1 and the plain transform and
// lit shaders this milestone already had stay in d3d9_fixed_function_shaders.hpp and d3d9_transform_shaders.hpp;
// these are only the ones the eight-stage path binds.
// the transformed vertex shader for a draw that carries more than one texture coordinate set: the device gathers the
// set each stage selected into two float2 channels in a canonical position, colour, TEXCOORD0, TEXCOORD1 layout, and
// this shader passes both through so stage 0 samples the first and stage 1 the second (D3DTSS_TEXCOORDINDEX)
const char kTransformedPairSource[] = R"(
cbuffer Transform : register(b0) {
  row_major float4x4 world_view_projection;
};

struct VSInput {
  float3 position : POSITION;
  float4 colour : COLOR0;
  float2 uv0 : TEXCOORD0;
  float2 uv1 : TEXCOORD1;
};

struct VSOutput {
  float4 position : SV_POSITION;
  float4 colour : COLOR0;
  float4 uv0 : TEXCOORD0;
  float4 uv1 : TEXCOORD1;
};

VSOutput main(VSInput input) {
  VSOutput output;
  output.position = mul(float4(input.position, 1.0), world_view_projection);
  output.colour = input.colour;
  output.uv0 = float4(input.uv0, 1.0, 0.0);
  output.uv1 = float4(input.uv1, 1.0, 0.0);
  return output;
}
)";

bool
write_ext_vs_header(const char *path) {
  static const char *uvtype[4] = {"float", "float2", "float3", "float4"};
  static const char *uvsource[4] = {
      "float4(input.uv.x, 1.0, 0.0, 0.0)",
      "float4(input.uv.x, input.uv.y, 1.0, 0.0)",
      "float4(input.uv.x, input.uv.y, input.uv.z, 1.0)",
      "input.uv",
  };

  std::vector<unsigned char> pre[9], transform[9], lit[4];
  // the same two families without a DIFFUSE channel: a projected draw whose declaration is a position and a wide
  // coordinate only (Wine visual.c projected_textures_test's decl3) has no colour to read, and the combiner supplies
  // D3D9's opaque white for it
  std::vector<unsigned char> pre_nodiffuse[2], transform_nodiffuse[2];
  std::vector<unsigned char> clipped_transform_spec[2], clipped_lit_spec[2];
  D3D_SHADER_MACRO macros[6];
  // zero: u0spec; then u1..u4; then u1spec..u4spec
  const int uvsize[9] = {0, 1, 2, 3, 4, 1, 2, 3, 4};
  for (int which = 0; which < 9; which++) {
    int count = 0;
    if (which == 0 || which >= 5)
      macros[count++] = {"SPECULAR", "1"};
    if (uvsize[which] >= 1) {
      macros[count++] = {"UVSIZE", "1"};
      macros[count++] = {"UVTYPE", uvtype[uvsize[which] - 1]};
      macros[count++] = {"UVSOURCE", uvsource[uvsize[which] - 1]};
    }
    macros[count] = {nullptr, nullptr};
    char name[64];
    snprintf(name, sizeof(name), "pre_which%d", which);
    if (!compile_profile(kPreTransformedEightStageSource, "main", name, "vs_5_0", macros, pre[which]))
      return false;
    snprintf(name, sizeof(name), "transform_which%d", which);
    if (!compile_profile(kTransformedEightStageSource, "main", name, "vs_5_0", macros, transform[which]))
      return false;
  }
  for (int width = 0; width < 2; width++) {
    D3D_SHADER_MACRO macros_nodiffuse[5] = {{"NODIFFUSE", "1"},
                                            {"UVSIZE", "1"},
                                            {"UVTYPE", uvtype[width + 2]},
                                            {"UVSOURCE", uvsource[width + 2]},
                                            {nullptr, nullptr}};
    char name[64];
    snprintf(name, sizeof(name), "pre_nodiffuse_u%d", width + 3);
    if (!compile_profile(kPreTransformedEightStageSource, "main", name, "vs_5_0", macros_nodiffuse,
                         pre_nodiffuse[width]))
      return false;
    snprintf(name, sizeof(name), "transform_nodiffuse_u%d", width + 3);
    if (!compile_profile(kTransformedEightStageSource, "main", name, "vs_5_0", macros_nodiffuse,
                         transform_nodiffuse[width]))
      return false;
  }
  // the lit paths: plain and textured, each with D3DFVF_SPECULAR in (D3DRS_LIGHTING off passes it, on carries the
  // lighting's own specular)
  D3D_SHADER_MACRO lit_plain[] = {{"SPECULAR", "1"}, {nullptr, nullptr}};
  D3D_SHADER_MACRO lit_textured[] = {{"SPECULAR", "1"}, {"TEXTURED", "1"}, {nullptr, nullptr}};
  if (!compile_profile(kLightingSource, "main", "lit_spec", "vs_5_0", lit_plain, lit[0]) ||
      !compile_profile(kLightingSource, "main", "lit_spec_textured", "vs_5_0", lit_textured, lit[2]))
    return false;
  // the clipped variants the eight-stage path draws through (illuminated and transformed vertices with D3DRS_CLIPPLANEENABLE)
  D3D_SHADER_MACRO clipped_lit_spec_macros[] = {{"CLIP_PLANES", "1"}, {"SPECULAR", "1"}, {nullptr, nullptr}};
  D3D_SHADER_MACRO clipped_lit_spec_textured_macros[] = {
      {"CLIP_PLANES", "1"}, {"SPECULAR", "1"}, {"TEXTURED", "1"}, {nullptr, nullptr}};
  D3D_SHADER_MACRO clipped_transform_spec_macros[] = {{"CLIP_PLANES", "1"}, {"SPECULAR", "1"}, {nullptr, nullptr}};
  D3D_SHADER_MACRO clipped_transform_spec_textured_macros[] = {
      {"CLIP_PLANES", "1"}, {"SPECULAR", "1"}, {"TEXTURED", "1"}, {nullptr, nullptr}};
  // the non-specular clipped variants: a transformed draw can need the eight-stage combiner on the state alone
  // (a later stage active, an argument past the two the extended shader knows) with no specular on the FVF
  D3D_SHADER_MACRO clipped_transform_macros[] = {{"CLIP_PLANES", "1"}, {nullptr, nullptr}};
  D3D_SHADER_MACRO clipped_transform_textured_macros[] = {
      {"CLIP_PLANES", "1"}, {"TEXTURED", "1"}, {nullptr, nullptr}};
  if (!compile_profile(kLightingSource, "main", "clipped_lit_spec", "vs_5_0", clipped_lit_spec_macros,
                       clipped_lit_spec[0]) ||
      !compile_profile(kLightingSource, "main", "clipped_lit_spec_textured", "vs_5_0",
                       clipped_lit_spec_textured_macros, clipped_lit_spec[1]) ||
      !compile_profile(kTransformedEightStageSource, "main", "clipped_transform_spec", "vs_5_0",
                       clipped_transform_spec_macros, clipped_transform_spec[0]) ||
      !compile_profile(kTransformedEightStageSource, "main", "clipped_transform_spec_textured", "vs_5_0",
                       clipped_transform_spec_textured_macros, clipped_transform_spec[1]))
    return false;
  std::vector<unsigned char> clipped_transform[2];
  if (!compile_profile(kTransformedEightStageSource, "main", "clipped_transform", "vs_5_0", clipped_transform_macros,
                       clipped_transform[0]) ||
      !compile_profile(kTransformedEightStageSource, "main", "clipped_transform_textured", "vs_5_0",
                       clipped_transform_textured_macros, clipped_transform[1]))
    return false;

  std::vector<unsigned char> pair;
  if (!compile_profile(kTransformedPairSource, "main", "transformed_pair", "vs_5_0", nullptr, pair))
    return false;
  FILE *file = fopen(path, "wb");
  if (!file) {
    printf("failed: no file %s\n", path);
    return false;
  }
  fprintf(file,
          "// generated by tests/d3d9/fixed_function_shaders.cpp: do not edit. the vertex shaders the eight-stage\n"
          "// fixed-function combiner draws through: the pre-transformed and transformed families with the FVF's\n"
          "// texture coordinate size (float, float2, float3 or float4, TEXCOORD0; the components the FVF does not\n"
          "// carry are filled 0 and 1) and its specular (COLOR1, which D3DTA_SPECULAR reads), and the lit paths with\n"
          "// the specular in and out. compiled offline at vs_5_0.\n"
          "#pragma once\n\nnamespace mullion::d3d9 {\n");
  static const char *pre_names[9] = {
      "kPreExtVertexShaderSpec", "kPreExtVertexShaderU1", "kPreExtVertexShaderU2", "kPreExtVertexShaderU3",
      "kPreExtVertexShaderU4", "kPreExtVertexShaderSpecU1", "kPreExtVertexShaderSpecU2", "kPreExtVertexShaderSpecU3",
      "kPreExtVertexShaderSpecU4"};
  static const char *transform_names[9] = {
      "kTransformExtVertexShaderSpec", "kTransformExtVertexShaderU1", "kTransformExtVertexShaderU2",
      "kTransformExtVertexShaderU3", "kTransformExtVertexShaderU4", "kTransformExtVertexShaderSpecU1",
      "kTransformExtVertexShaderSpecU2", "kTransformExtVertexShaderSpecU3", "kTransformExtVertexShaderSpecU4"};
  for (int which = 0; which < 9; which++) {
    write_array(file, pre_names[which], pre[which]);
    write_array(file, transform_names[which], transform[which]);
  }
  write_array(file, "kPreExtVertexShaderNoDiffuseU3", pre_nodiffuse[0]);
  write_array(file, "kPreExtVertexShaderNoDiffuseU4", pre_nodiffuse[1]);
  write_array(file, "kTransformExtVertexShaderNoDiffuseU3", transform_nodiffuse[0]);
  write_array(file, "kTransformExtVertexShaderNoDiffuseU4", transform_nodiffuse[1]);
  write_array(file, "kLitSpecVertexShader", lit[0]);
  write_array(file, "kLitSpecTexturedVertexShader", lit[2]);
  write_array(file, "kClippedLitSpecVertexShader", clipped_lit_spec[0]);
  write_array(file, "kClippedLitSpecTexturedVertexShader", clipped_lit_spec[1]);
  write_array(file, "kClippedTransformExtSpecVertexShader", clipped_transform_spec[0]);
  write_array(file, "kClippedTransformExtSpecTexturedVertexShader", clipped_transform_spec[1]);
  write_array(file, "kClippedTransformExtVertexShader", clipped_transform[0]);
  write_array(file, "kClippedTransformExtTexturedVertexShader", clipped_transform[1]);
  write_array(file, "kTransformedPairVertexShader", pair);
  fprintf(file, "\n} // namespace mullion::d3d9\n");
  fclose(file);
  printf("wrote %s (%zu bytes of shader blobs)\n", path,
         (pre[0].size() + transform[0].size() + lit[0].size() + lit[2].size() + pre_nodiffuse[0].size() +
          transform_nodiffuse[0].size()));
  return true;
}

// the eight-stage combiner's pixel shaders: FLAT / TEXTURED / SPECULAR select nointerpolation, the texture
// coordinates the FVF carries (a draw without them samples vec4(0)) and the COLOR1 the D3DTA_SPECULAR argument reads
bool
write_eight_stage_header(const char *path) {
  D3D_SHADER_MACRO none[] = {{nullptr, nullptr}};
  D3D_SHADER_MACRO flat[] = {{"FLAT", "1"}, {nullptr, nullptr}};
  D3D_SHADER_MACRO texted[] = {{"TEXTURED", "1"}, {nullptr, nullptr}};
  D3D_SHADER_MACRO texted_flat[] = {{"TEXTURED", "1"}, {"FLAT", "1"}, {nullptr, nullptr}};
  D3D_SHADER_MACRO spec[] = {{"SPECULAR", "1"}, {nullptr, nullptr}};
  D3D_SHADER_MACRO spec_flat[] = {{"SPECULAR", "1"}, {"FLAT", "1"}, {nullptr, nullptr}};
  D3D_SHADER_MACRO spec_texted[] = {{"SPECULAR", "1"}, {"TEXTURED", "1"}, {nullptr, nullptr}};
  D3D_SHADER_MACRO spec_texted_flat[] = {{"SPECULAR", "1"}, {"TEXTURED", "1"}, {"FLAT", "1"}, {nullptr, nullptr}};
  std::vector<unsigned char> colour, colour_flat, colour_spec, colour_spec_flat, sample, sample_flat, sample_spec,
      sample_spec_flat;
  if (!compile_profile(kEightStagePixelSource, "main", "eight_colour", "ps_5_0", none, colour) ||
      !compile_profile(kEightStagePixelSource, "main", "eight_colour_flat", "ps_5_0", flat, colour_flat) ||
      !compile_profile(kEightStagePixelSource, "main", "eight_colour_spec", "ps_5_0", spec, colour_spec) ||
      !compile_profile(kEightStagePixelSource, "main", "eight_colour_spec_flat", "ps_5_0", spec_flat,
                       colour_spec_flat) ||
      !compile_profile(kEightStagePixelSource, "main", "eight_sample", "ps_5_0", texted, sample) ||
      !compile_profile(kEightStagePixelSource, "main", "eight_sample_flat", "ps_5_0", texted_flat, sample_flat) ||
      !compile_profile(kEightStagePixelSource, "main", "eight_sample_spec", "ps_5_0", spec_texted, sample_spec) ||
      !compile_profile(kEightStagePixelSource, "main", "eight_sample_spec_flat", "ps_5_0", spec_texted_flat,
                       sample_spec_flat))
    return false;

  FILE *file = fopen(path, "wb");
  if (!file) {
    printf("failed: no file %s\n", path);
    return false;
  }
  fprintf(file,
          "// generated by tests/d3d9/fixed_function_shaders.cpp: do not edit. the eight-stage fixed-function\n"
          "// combiner's pixel shaders: every stage of DDCAPS9::MaxTextureBlendStages, the D3DTA arguments and their\n"
          "// D3DTA_COMPLEMENT and D3DTA_ALPHAREPLICATE modifiers, D3DTA_TEMP with D3DTSS_RESULTARG, the texture\n"
          "// transform flags (D3DTTff counts and D3DTTFF_PROJECTED divides), the D3DTA_SPECULAR argument, table fog\n"
          "// and D3DRS_ALPHATESTENABLE. compiled offline at ps_5_0. FLAT is the nointerpolation read, TEXTURED the\n"
          "// FVF's texture coordinates, SPECULAR the COLOR1 input.\n"
          "#pragma once\n\nnamespace mullion::d3d9 {\n");
  write_array(file, "kEightStageColourPixelShader", colour);
  write_array(file, "kEightStageFlatColourPixelShader", colour_flat);
  write_array(file, "kEightStageColourSpecPixelShader", colour_spec);
  write_array(file, "kEightStageFlatColourSpecPixelShader", colour_spec_flat);
  write_array(file, "kEightStageTexturedPixelShader", sample);
  write_array(file, "kEightStageFlatTexturedPixelShader", sample_flat);
  write_array(file, "kEightStageTexturedSpecPixelShader", sample_spec);
  write_array(file, "kEightStageFlatTexturedSpecPixelShader", sample_spec_flat);
  fprintf(file, "\n} // namespace mullion::d3d9\n");
  fclose(file);
  printf("wrote %s (%zu %zu %zu %zu %zu %zu %zu %zu bytes)\n", path, colour.size(), colour_flat.size(),
         colour_spec.size(), colour_spec_flat.size(), sample.size(), sample_flat.size(), sample_spec.size(),
         sample_spec_flat.size());
  return true;
}

// the transform's textured vertex shader for a draw with a game pixel shader: the FVF fills the coordinate's missing
// components with zero and its w with one, while fixed-function sampling fills them with one (visual.c ps_1_4 versus
// test_updatetexture's cube case)
bool
write_user_transform_header(const char *path) {
  D3D_SHADER_MACRO macros[] = {{"TEXTURED", "1"}, {"USERPS", "1"}, {nullptr, nullptr}};
  std::vector<unsigned char> shader;
  if (!compile_profile(kTransformSource, "main", "transformed_user", "vs_5_0", macros, shader))
    return false;
  FILE *file = fopen(path, "wb");
  if (!file) {
    printf("failed: no file %s\n", path);
    return false;
  }
  fprintf(file,
          "// generated by tests/d3d9/fixed_function_shaders.cpp: do not edit. the fixed-function transform's\n"
          "// textured vertex shader for a draw with a game pixel shader: the FVF's missing coordinate components\n"
          "// are zero and w is one (Wine visual.c ps_1_4). compiled offline at vs_5_0.\n"
          "#pragma once\n\nnamespace mullion::d3d9 {\n");
  write_array(file, "kTransformedTexturedUserVertexShader", shader);
  fprintf(file, "\n} // namespace mullion::d3d9\n");
  fclose(file);
  printf("wrote %s (%zu bytes)\n", path, shader.size());
  return true;
}

bool
write_header(const char *path, const char *comment, const char *source, const char *first_name, const char *second_name,
             bool lighting = false) {
  std::vector<unsigned char> plain, textured;
  if (!compile(source, "main", path, plain, false) || !compile(source, "main", path, textured, true))
    return false;
  // the lighting shader's normal-less variants, one per combination of COLOR0 and COLOR1 the FVF can carry: D3D9's
  // fixed-function lighting of a format with no normal has ambient and emissive only
  std::vector<unsigned char> no_normal, no_normal_diffuse, no_normal_specular, no_normal_both;
  if (lighting) {
    D3D_SHADER_MACRO none[] = {{"NONORMAL", "1"}, {"NODIFFUSE", "1"}, {nullptr, nullptr}};
    D3D_SHADER_MACRO diffuse[] = {{"NONORMAL", "1"}, {nullptr, nullptr}};
    D3D_SHADER_MACRO specular[] = {
        {"NONORMAL", "1"}, {"NODIFFUSE", "1"}, {"SPECULAR_COLOUR", "1"}, {nullptr, nullptr}};
    D3D_SHADER_MACRO both[] = {{"NONORMAL", "1"}, {"SPECULAR_COLOUR", "1"}, {nullptr, nullptr}};
    if (!compile_profile(source, "main", "lit_no_normal", "vs_5_0", none, no_normal) ||
        !compile_profile(source, "main", "lit_no_normal_diffuse", "vs_5_0", diffuse, no_normal_diffuse) ||
        !compile_profile(source, "main", "lit_no_normal_specular", "vs_5_0", specular, no_normal_specular) ||
        !compile_profile(source, "main", "lit_no_normal_both", "vs_5_0", both, no_normal_both))
      return false;
  }

  FILE *file = fopen(path, "wb");
  if (!file) {
    printf("failed: no file %s\n", path);
    return false;
  }
  fprintf(file, "%s", comment);
  fprintf(file, "#pragma once\n\nnamespace mullion::d3d9 {\n");
  write_array(file, first_name, plain);
  write_array(file, second_name, textured);
  if (lighting) {
    write_array(file, "kLitNoNormalVertexShader", no_normal);
    write_array(file, "kLitNoNormalDiffuseVertexShader", no_normal_diffuse);
    write_array(file, "kLitNoNormalSpecularVertexShader", no_normal_specular);
    write_array(file, "kLitNoNormalDiffuseSpecularVertexShader", no_normal_both);
  }
  fprintf(file, "\n} // namespace mullion::d3d9\n");
  fclose(file);
  printf("wrote %s (%zu and %zu bytes)\n", path, plain.size(), textured.size());
  return true;
}

// the transform's volume variant: the same shader with D3D9's third texture coordinate, which selects the slice
bool
write_transform3d_header(const char *path) {
  D3D_SHADER_MACRO textured3[] = {{"TEXTURED", "1"}, {"TEXCOORD3", "1"}, {nullptr, nullptr}};
  D3D_SHADER_MACRO textured3_no_diffuse[] = {
      {"TEXTURED", "1"}, {"TEXCOORD3", "1"}, {"NODIFFUSE", "1"}, {nullptr, nullptr}};
  std::vector<unsigned char> shader, no_diffuse;
  if (!compile_profile(kTransformSource, "main", "transformed_3d", "vs_5_0", textured3, shader) ||
      !compile_profile(kTransformSource, "main", "transformed_3d_no_diffuse", "vs_5_0", textured3_no_diffuse,
                       no_diffuse))
    return false;
  FILE *file = fopen(path, "wb");
  if (!file) {
    printf("failed: no file %s\n", path);
    return false;
  }
  fprintf(file,
          "// generated by tests/d3d9/fixed_function_shaders.cpp: do not edit. the fixed-function transform's\n"
          "// vertex shaders for a volume stage: D3D9's D3DFVF_TEXCOORDSIZE3 coordinate's third component selects\n"
          "// the slice, and the pixel shaders read it from the interpolator's z. the no-diffuse variant declares no\n"
          "// colour input and writes D3D9's white default. compiled offline at vs_5_0.\n"
          "#pragma once\n\nnamespace mullion::d3d9 {\n");
  write_array(file, "kTransformed3dTexturedVertexShader", shader);
  write_array(file, "kTransformed3dNoDiffuseTexturedVertexShader", no_diffuse);
  fprintf(file, "\n} // namespace mullion::d3d9\n");
  fclose(file);
  printf("wrote %s (%zu and %zu bytes)\n", path, shader.size(), no_diffuse.size());
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
    "//     float4 flags, flags2, view_forward, material_sources; };\n";

// the clipped transform and lighting shaders, and StretchRect's blit, as one header of their own
bool
write_clip_blit_header(const char *path) {
  D3D_SHADER_MACRO clipped[] = {{"CLIP_PLANES", "1"}, {nullptr, nullptr}};
  D3D_SHADER_MACRO clipped_textured[] = {{"CLIP_PLANES", "1"}, {"TEXTURED", "1"}, {nullptr, nullptr}};
  std::vector<unsigned char> transformed, transformed_textured, lit, lit_textured, blit_vertex, blit_pixel;
  if (!compile_profile(kTransformSource, "main", "clipped_transformed", "vs_5_0", clipped, transformed) ||
      !compile_profile(kTransformSource, "main", "clipped_transformed_textured", "vs_5_0", clipped_textured,
                       transformed_textured) ||
      !compile_profile(kLightingSource, "main", "clipped_lit", "vs_5_0", clipped, lit) ||
      !compile_profile(kLightingSource, "main", "clipped_lit_textured", "vs_5_0", clipped_textured, lit_textured) ||
      !compile_profile(kBlitVertexSource, "main", "blit_vertex", "vs_5_0", nullptr, blit_vertex) ||
      !compile_profile(kBlitPixelSource, "main", "blit_pixel", "ps_5_0", nullptr, blit_pixel))
    return false;

  FILE *file = fopen(path, "wb");
  if (!file) {
    printf("failed: no file %s\n", path);
    return false;
  }
  fprintf(file,
          "// generated by tests/d3d9/fixed_function_shaders.cpp: do not edit. the fixed-function transform and\n"
          "// lighting vertex shaders with D3D9's six user clip planes, and StretchRect's blit. compiled offline at\n"
          "// vs_5_0 and ps_5_0.\n"
          "//\n"
          "// the clipped shaders are d3d9_transform_shaders.hpp's and d3d9_lighting_shaders.hpp's with one more constant\n"
          "// buffer, the planes in clip space (a disabled plane is zero), and two outputs after every other one:\n"
          "//   cbuffer ClipPlanes : register(b2) { float4 clip_planes[6]; };\n"
          "//   float3 clip0 : SV_ClipDistance0; float3 clip1 : SV_ClipDistance1;   // dot(plane, clip position)\n"
          "//\n"
          "// the blit draws a four-vertex strip from SV_VertexID over the viewport, sampling t0 through s0 at the\n"
          "// source rectangle b0 holds as texture coordinates:\n"
          "//   cbuffer Blit : register(b0) { float4 source_rect; };   // left, top, right, bottom\n"
          "#pragma once\n\nnamespace mullion::d3d9 {\n");
  write_array(file, "kClippedTransformedVertexShader", transformed);
  write_array(file, "kClippedTransformedTexturedVertexShader", transformed_textured);
  write_array(file, "kClippedLitVertexShader", lit);
  write_array(file, "kClippedLitTexturedVertexShader", lit_textured);
  write_array(file, "kBlitVertexShader", blit_vertex);
  write_array(file, "kBlitPixelShader", blit_pixel);
  fprintf(file, "\n} // namespace mullion::d3d9\n");
  fclose(file);
  printf("wrote %s (%zu %zu %zu %zu %zu %zu bytes)\n", path, transformed.size(), transformed_textured.size(),
         lit.size(), lit_textured.size(), blit_vertex.size(), blit_pixel.size());
  return true;
}

} // namespace

int
main(int argc, char **argv) {
  const std::string directory = argc > 1 ? argv[1] : "src/d3d9";
  if (!write_header((directory + "/d3d9_transform_shaders.hpp").c_str(), kTransformComment, kTransformSource,
                    "kTransformedVertexShader", "kTransformedTexturedVertexShader"))
    return 1;
  if (!write_user_transform_header((directory + "/d3d9_transform_user_shader.hpp").c_str()))
    return 1;
  if (!write_transform3d_header((directory + "/d3d9_transform3d_shaders.hpp").c_str()))
    return 1;
  if (!write_header((directory + "/d3d9_lighting_shaders.hpp").c_str(), kLightingComment, kLightingSource,
                    "kLitVertexShader", "kLitTexturedVertexShader", true))
    return 1;
  if (!write_extended_header((directory + "/d3d9_ff_extended_shaders.hpp").c_str()))
    return 1;
  if (!write_ext_vs_header((directory + "/d3d9_ff_ext_vs_shaders.hpp").c_str()))
    return 1;
  if (!write_eight_stage_header((directory + "/d3d9_ff_eight_stage_shaders.hpp").c_str()))
    return 1;
  if (!write_clip_blit_header((directory + "/d3d9_clip_blit_shaders.hpp").c_str()))
    return 1;
  return 0;
}
