// Direct3D 9's fixed-function transform for the vertex formats this build draws, expressed as the shaders and vertex
// layouts the D3D11 core takes so a draw goes down the same road a D3D11 draw does:
//
//  * a vertex the device transforms (D3DFVF_XYZ), whose position is a row vector the device's world, view and
//    projection matrices take to a clip position — v' = v * World * View * Projection, D3D9's order and convention —
//    which the rasterizer divides by w and maps through the viewport (Microsoft Learn, "Transforms");
//  * a vertex already in screen space (D3DFVF_XYZRHW, whose w is 1/rhw), which the shader maps through the viewport
//    itself, since that position is already past the divide.
//
// Both carry a vertex colour and, when the draw samples a texture, the stage's texture coordinates. The rasterizer,
// depth and blend states a draw runs under are the device's render state (d3d9_state.hpp), and the texture stage's
// operation is the constants its pixel shader reads (d3d9_texture.hpp).
#pragma once
#include "d3d9_common.hpp"

namespace mullion::d3d9 {

struct PreTransformedVertex {
  float x, y, z, rhw;
  D3DCOLOR colour;
};

// a vertex that carries the stage's texture coordinates as well (D3DFVF_TEX1, two of them: D3D9's default coordinate
// size)
struct PreTransformedTexturedVertex {
  float x, y, z, rhw;
  D3DCOLOR colour;
  float u, v;
};

inline constexpr UINT kPreTransformedStride = sizeof(PreTransformedVertex);
inline constexpr UINT kPreTransformedTexturedStride = sizeof(PreTransformedTexturedVertex);
// D3DFVF_XYZRHW | D3DFVF_DIFFUSE: the format this build transforms, and the same one with a texture coordinate set
inline constexpr DWORD kPreTransformedFVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;
inline constexpr DWORD kPreTransformedTexturedFVF = kPreTransformedFVF | D3DFVF_TEX1;

// the same two families with D3DFVF_SPECULAR, which the eight-stage combiner's pixel shader reads as COLOR1 for the
// D3DTA_SPECULAR argument and adds to the colour after blending while D3DRS_SPECULARENABLE is on (Microsoft Learn,
// "Texture Blending: the specular argument"). the specular DWORD sits between the diffuse and the coordinates
struct PreTransformedSpecVertex {
  float x, y, z, rhw;
  D3DCOLOR colour;
  D3DCOLOR specular;
};

struct PreTransformedSpecTexturedVertex {
  float x, y, z, rhw;
  D3DCOLOR colour;
  D3DCOLOR specular;
  float u, v;
};

inline constexpr UINT kPreTransformedSpecStride = sizeof(PreTransformedSpecVertex);
inline constexpr UINT kPreTransformedSpecTexturedStride = sizeof(PreTransformedSpecTexturedVertex);
inline constexpr DWORD kPreTransformedSpecFVF = kPreTransformedFVF | D3DFVF_SPECULAR;
inline constexpr DWORD kPreTransformedSpecTexturedFVF = kPreTransformedSpecFVF | D3DFVF_TEX1;

// a vertex the device transforms: its position is in the space the world matrix reads, and the device's matrices take
// it to the clip position the rasterizer divides and maps through the viewport (D3DFVF_XYZ, D3DFVF_XYZ | D3DFVF_TEX1)
struct TransformedVertex {
  float x, y, z;
  D3DCOLOR colour;
};

struct TransformedTexturedVertex {
  float x, y, z;
  D3DCOLOR colour;
  float u, v;
};

// D3DFVF_XYZ alone: position with the material/default diffuse (Wine device.c test_pinned_buffers)
inline constexpr UINT kPositionStride = 12;
inline constexpr DWORD kPositionFVF = D3DFVF_XYZ;
inline constexpr UINT kTransformedStride = sizeof(TransformedVertex);
inline constexpr UINT kTransformedTexturedStride = sizeof(TransformedTexturedVertex);
inline constexpr DWORD kTransformedFVF = D3DFVF_XYZ | D3DFVF_DIFFUSE;
inline constexpr DWORD kTransformedTexturedFVF = kTransformedFVF | D3DFVF_TEX1;
// a textured vertex with no diffuse colour: D3D9 draws it with opaque white (Wine d3d9ex.c test_user_memory). The
// pipeline's layouts all carry the colour, so a draw of one of these gathers its vertices into the diffuse format first
// (packed_fixed_function_fvf and pack_up_vertices / bind_gathered_declaration_stream in d3d9_device_draw.cpp).
inline constexpr DWORD kTransformedTexNoDiffuseFVF = D3DFVF_XYZ | D3DFVF_TEX1;
inline constexpr UINT kTransformedTexNoDiffuseStride = 12 + 8;
inline constexpr DWORD kPreTransformedTexNoDiffuseFVF = D3DFVF_XYZRHW | D3DFVF_TEX1;
inline constexpr UINT kPreTransformedTexNoDiffuseStride = 16 + 8;

// the two transformed families with D3DFVF_SPECULAR, for the same reason as the pre-transformed ones
struct TransformedSpecVertex {
  float x, y, z;
  D3DCOLOR colour;
  D3DCOLOR specular;
};

struct TransformedSpecTexturedVertex {
  float x, y, z;
  D3DCOLOR colour;
  D3DCOLOR specular;
  float u, v;
};

inline constexpr UINT kTransformedSpecStride = sizeof(TransformedSpecVertex);
inline constexpr UINT kTransformedSpecTexturedStride = sizeof(TransformedSpecTexturedVertex);
inline constexpr DWORD kTransformedSpecFVF = kTransformedFVF | D3DFVF_SPECULAR;
inline constexpr DWORD kTransformedSpecTexturedFVF = kTransformedSpecFVF | D3DFVF_TEX1;

// a vertex the device transforms and lights: its normal is in the space the world matrix reads, and the lit vertex
// shader turns it by the inverse transpose of that matrix (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE,
// with or without one set of texture coordinates). D3DFVF_NORMAL's three floats come straight after the position,
// which is the order D3DFVF_XYZ's and D3DFVF_NORMAL's masks put them in
struct LitVertex {
  float x, y, z;
  float nx, ny, nz;
  D3DCOLOR colour;
};

struct LitTexturedVertex {
  float x, y, z;
  float nx, ny, nz;
  D3DCOLOR colour;
  float u, v;
};

inline constexpr UINT kLitStride = sizeof(LitVertex);
inline constexpr UINT kLitTexturedStride = sizeof(LitTexturedVertex);
inline constexpr DWORD kLitFVF = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE;
inline constexpr DWORD kLitTexturedFVF = kLitFVF | D3DFVF_TEX1;
// Wine visual.c test_specular_lighting: D3DFVF_XYZ | D3DFVF_NORMAL with no DIFFUSE — D3D9 uses opaque white
inline constexpr DWORD kLitNoDiffuseFVF = D3DFVF_XYZ | D3DFVF_NORMAL;
inline constexpr DWORD kLitNoDiffuseTexturedFVF = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1;
inline constexpr UINT kLitNoDiffuseStride = 24;          // float3 position + float3 normal
inline constexpr UINT kLitNoDiffuseTexturedStride = 32; // + float2 texcoord
// a lit format with no normal: D3D9 shades only the ambient and emissive terms, and the vertex's colours are the
// material sources (D3DRS_*MATERIALSOURCE). Wine visual.c test_color_vertex.
inline constexpr DWORD kLitNoNormalSpecularFVF = D3DFVF_XYZ | D3DFVF_SPECULAR;
inline constexpr UINT kLitNoNormalSpecularStride = 16;

// what a textured draw binds beside its geometry: the texture stage's operation (d3d9_texture.hpp's constants) and what
// it samples. a stage with no texture bound samples one white texel, since D3D9 leaves the value of a texture argument
// whose stage is processed without a texture undefined and white is what a blend with nothing in it leaves.
struct FixedFunctionSampling {
  ID3D11ShaderResourceView *texture = nullptr;
  ID3D11SamplerState *sampler = nullptr;
  ID3D11ShaderResourceView *texture1 = nullptr;
  ID3D11SamplerState *sampler1 = nullptr;
  UINT32 ops[4] = {};
  UINT32 args[4] = {};
  UINT32 ops1[4] = {};
  UINT32 args1[4] = {};
  float factor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  bool stage1 = false;
  bool transform0 = false;
  float tex0[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  // stage 1's D3DTSS_TEXTURETRANSFORMFLAGS (0 disabled, 2..4 COUNT2..COUNT4, 256 the PROJECTED bit) and matrix
  UINT32 transform1 = 0;
  float tex1[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  // what each stage samples (x: 0 a 2D texture, 1 a cube, 2 a volume), which picks the sampling instruction: a cube's
  // coordinate is a direction and a volume's a point in its box, so neither is read through the 2D declaration
  UINT32 dims[2][4] = {};
};

struct LightConstants;

// the eight-stage combiner's drive (d3d9_ff_eight_stage_shaders.hpp): the stages' operations, arguments and texture
// matrices, the texture factor, the fog, the alpha test and the specular add, as one constant buffer of exactly the
// size the eight-stage pixel shader declares at b0. every operation and argument stays the raw D3D9 value it was set
// with, and the shader decodes modifiers itself (tests/d3d9/fixed_function_shaders.cpp).
struct FixedFunctionSampling8 {
  UINT32 ops[8][4] = {};
  UINT32 args[8][4] = {};
  UINT32 args0[8][4] = {};
  float constants8[8][4] = {};
  float factor[4] = {};
  float fog[4] = {};
  float fogcolor[4] = {};
  UINT32 alpha[4] = {};
  UINT32 misc[4] = {};
  float tex[8][16] = {};
  // what each stage samples (x: 0 a 2D texture, 1 a cube, 2 a volume), as d3d9_ff_eight_stage_shaders.hpp reads it
  UINT32 dims[8][4] = {};
};

static_assert(offsetof(FixedFunctionSampling8, tex) == 37 * 16, "the board the eight-stage pixel shader reads");
static_assert(sizeof(FixedFunctionSampling8) == offsetof(FixedFunctionSampling8, dims) + 8 * 4 * sizeof(UINT32),
              "exactly what the eight-stage pixel shader declares at b0");

// the draw-time caddy of an eight-stage draw: the shader's board and the binding outside it, which must never sit in
// the constant buffer itself
struct FixedFunctionEightStage {
  FixedFunctionSampling8 board;

  // the eight stages' views and samplers, all non-null: a stage with no texture bound gets the device's white texel
  ID3D11ShaderResourceView *views[8] = {};
  ID3D11SamplerState *samplers[8] = {};
  // which pixel combination the draw reads: the FVF carries texture coordinates, D3DFVF_SPECULAR (COLOR1 data), or is lit
  bool textured = false;
  bool vertex_specular = false;
  bool lit = false;
};

// what one fixed-function draw is, as the pipeline needs it to bind: the viewport it is placed and clipped by, the stage
// it samples and the two constants a transformed or lit vertex reads -- which between them pick the shader and layout.
// The draw path works this out from the device state (d3d9_device_draw.cpp); nothing here is D3D11 state.
struct FixedFunctionDraw {
  D3DVIEWPORT9 viewport{};
  RECT clip{};
  // D3DSHADE_FLAT takes one vertex's colour for the whole triangle
  bool flat_shaded = false;
  // the FVF the draw came from, or 0 for a vertex declaration: it picks the lit format's colour inputs
  DWORD fvf = 0;
  // a game pixel shader is bound over this draw (its TEXCOORD0 reads the FVF's coordinate, not the fixed-function fill)
  bool user_pixel = false;
  // the texture stage a format with texture coordinates draws through, or null for one without (or for a stage whose
  // first colour argument is the texture and which has none, which is not processed at all)
  const FixedFunctionSampling *sampling = nullptr;
  // the eight-stage combiner's board a draw needs more than the two stages the extended shader carries takes, or null
  // for a draw that goes through the two-stage paths above
  const FixedFunctionEightStage *eight_stage = nullptr;
  // D3D9's composed world, view and projection for a vertex the device transforms, or null for a vertex already in
  // screen space
  const float *world_view_projection = nullptr;
  // what a lit format's vertex colour is computed from, or null for a format with no normal (d3d9_lights.hpp)
  const LightConstants *lighting = nullptr;
  // D3D9's six user clip planes in clip space, a disabled one zero, for a vertex the device transforms while
  // D3DRS_CLIPPLANEENABLE names any; null draws unclipped (a vertex already in screen space is never clipped by them)
  const float *clip_planes = nullptr;
  bool fog_enabled = false;
  float fog_start = 0.0f;
  float fog_end = 1.0f;
  float fog_density = 1.0f;
  // D3DFOG_NONE, LINEAR, EXP or EXP2: the extended pixel shader reads it in the alpha constant's w
  DWORD fog_table_mode = D3DFOG_NONE;
  float fog_color[4] = {};
  bool alpha_test = false;
  DWORD alpha_func = D3DCMP_ALWAYS;
  DWORD alpha_ref = 0;
  // D3DRS_SPECULARENABLE on a lit draw: the lighting's specular sum (the lit vertex shader's COLOR1) is added to the
  // colour after the last blend stage, unclamped (Microsoft Learn, D3DRS_SPECULARENABLE)
  bool spec_add = false;
  // a blended vertex (D3DFVF_XYZB1..B3): `blend_weights` is how many weight floats the vertex carries, and
  // `blend_active` is how many D3DRS_VERTEXBLEND applies (0 draws with WORLD alone). the four world matrices and the
  // view * projection are what that vertex shader reads; one shader is built for each count, so the count is which
  // shader is bound rather than a constant it branches on
  UINT blend_weights = 0;
  UINT blend_active = 0;
  // the three weights are packed into a D3DCOLOR declaration element rather than three floats
  bool blend_packed = false;
  const float *blend_worlds = nullptr;
  const float *view_projection = nullptr;
  // the draw was gathered into the canonical two-UV layout (position, colour, TEXCOORD0, TEXCOORD1), so the pipeline
  // binds the pair vertex shader and the pixel shader whose stage 1 samples the second channel
  bool uv_pair = false;
  // D3DFVF_XYZ with no DIFFUSE: position-only input layout (COLOR defaults in the IA)
  bool position_only = false;
  // the FVF carries D3DFVF_DIFFUSE, which moves the texture coordinate after the colour in the no-diffuse layouts
  bool diffuse = false;
};

class FixedFunctionPipeline {
public:
  // whether the fixed-function path of this build transforms a vertex format, whether the device transforms its
  // vertices (rather than their being in screen space already), whether they are lit (D3DFVF_NORMAL, whose normals the
  // lighting reads), whether they carry texture coordinates, and how many bytes one of them takes
  static bool supports(DWORD fvf);
  static bool transformed(DWORD fvf);
  // D3DFVF_XYZB1, XYZB2 or XYZB3 with a diffuse colour: the weight count is written to `weights`
  static bool blend_format(DWORD fvf, UINT &weights, bool &diffuse);
  static bool lit(DWORD fvf);
  static bool textured(DWORD fvf);
  // the components the format's first texture coordinate set carries (D3DFVF_TEXCOORDSIZE's inverted encoding)
  static UINT texcoord_width(DWORD fvf);
  static UINT stride(DWORD fvf);
  // an unlit transformed format with more than one two-component texture coordinate set: the draw gathers each stage's
  // D3DTSS_TEXCOORDINDEX selection into the canonical pair layout the two-UV shaders read
  static bool pair_format(DWORD fvf);

  HRESULT initialize(ID3D11Device *device);
  // the eight-stage combiner's draw path: the FVF's vertex-shader family (pre-transformed, transformed or lit, with or
  // without D3DFVF_SPECULAR data), the pixel combination the board names, and the eight stages' samplers it binds
  HRESULT bind_eight(ID3D11DeviceContext *context, const FixedFunctionDraw &draw);
  // the objects initialize made belong to the device that made them: a Reset that makes a new device releases them
  // first, and initializes over the new one
  void release_device();
  // the draw bound: the shader and layout its format picks, the pixel shader its shading mode picks, the viewport it
  // is placed and clipped by, and the texture stage, transform and lights it reads, each at the constant buffer its
  // shader declares (d3d9_transform_shaders.hpp, d3d9_lighting_shaders.hpp). Everything the draw is comes from `draw`;
  // the pipeline owns the D3D11 objects and how they are bound
  HRESULT bind(ID3D11DeviceContext *context, const FixedFunctionDraw &draw);
  // the untextured pixel shader a mixed draw (a game's vertex shader, this pipeline's pixel shader) binds: COLOR0 is
  // interpolator register 1, matching the translator's oD0 (d3d9_fixed_function_shaders.hpp)
  // the stage constants the extended colour pixel shader reads, for a mixed draw (a game's vertex shader, this pixel
  // shader) under fog or the alpha test
  HRESULT bind_stage_constants(ID3D11DeviceContext *context, const FixedFunctionDraw &draw);
  // where the fixed-function textured VS a draw just bound writes TEXCOORD0: 2 for the unlit/extended pre-transformed
  // families, 3 for lit/transformed-specular (which reserve 2 for COLOR1). A game pixel shader bound over the draw is
  // translated against this so the two stages link; the fixed-function pixel shaders keep their own register 2.
  UINT texcoord_slot() const {
    return texcoord_slot_;
  }
  ID3D11PixelShader *colour_pixel_shader(bool flat_shaded, bool extra = false) const {
    if (extra)
      return (flat_shaded ? extended_flat_pixel_shader_ : extended_pixel_shader_).Get();
    return (flat_shaded ? flat_pixel_shader_ : pixel_shader_).Get();
  }

private:
  UINT texcoord_slot_ = 3;
  ComPtr<ID3D11VertexShader> vertex_shader_;
  ComPtr<ID3D11VertexShader> textured_vertex_shader_;
  ComPtr<ID3D11PixelShader> pixel_shader_;
  ComPtr<ID3D11PixelShader> flat_pixel_shader_;
  ComPtr<ID3D11PixelShader> textured_pixel_shader_;
  ComPtr<ID3D11PixelShader> flat_textured_pixel_shader_;
  ComPtr<ID3D11PixelShader> extended_pixel_shader_;
  ComPtr<ID3D11PixelShader> extended_flat_pixel_shader_;
  ComPtr<ID3D11PixelShader> extended_textured_pixel_shader_;
  ComPtr<ID3D11PixelShader> extended_flat_textured_pixel_shader_;
  // the same extended shaders with the lighting's specular input (a lit vertex shader's COLOR1): D3DRS_SPECULARENABLE
  // adds it to the colour after the last blend stage (Microsoft Learn, D3DRS_SPECULARENABLE; Wine's ffp pipeline tail)
  ComPtr<ID3D11PixelShader> extended_spec_pixel_shader_;
  ComPtr<ID3D11PixelShader> extended_spec_flat_pixel_shader_;
  ComPtr<ID3D11PixelShader> extended_spec_textured_pixel_shader_;
  ComPtr<ID3D11PixelShader> extended_spec_flat_textured_pixel_shader_;
  ComPtr<ID3D11InputLayout> vertex_layout_;
  ComPtr<ID3D11InputLayout> textured_vertex_layout_;
  // the transform's path: the vertex shaders of the two vertex the device transforms, their layouts, and the composed
  // matrix they read
  ComPtr<ID3D11VertexShader> transformed_vertex_shader_;
  ComPtr<ID3D11VertexShader> transformed_textured_vertex_shader_;
  // a transformed textured draw with a game pixel shader: its coordinate's missing components are zero, its w one
  ComPtr<ID3D11VertexShader> transformed_textured_user_vertex_shader_;
  ComPtr<ID3D11InputLayout> transformed_vertex_layout_;
  ComPtr<ID3D11InputLayout> transformed_textured_vertex_layout_;
  // the two-UV family: the gathered canonical layout and the vertex shader that passes both coordinate sets through
  ComPtr<ID3D11VertexShader> transformed_pair_vertex_shader_;
  ComPtr<ID3D11InputLayout> transformed_pair_layout_;
  ComPtr<ID3D11PixelShader> extended_textured_uv1_pixel_shader_;
  ComPtr<ID3D11PixelShader> extended_flat_textured_uv1_pixel_shader_;
  // a volume stage's coordinate has a third component the other textured shaders drop; this transform shader carries
  // it through, with a layout for the FVF with and without D3DFVF_DIFFUSE
  ComPtr<ID3D11VertexShader> volume_vertex_shader_;
  ComPtr<ID3D11VertexShader> volume_no_diffuse_vertex_shader_;
  ComPtr<ID3D11InputLayout> volume_textured_vertex_layout_;
  ComPtr<ID3D11InputLayout> volume_diffuse_textured_vertex_layout_;
  // the lit path: the vertex shaders of the two lit formats, their layouts, and the material, lights and lighting states
  // they read at b1
  ComPtr<ID3D11VertexShader> lit_vertex_shader_;
  ComPtr<ID3D11VertexShader> lit_textured_vertex_shader_;
  ComPtr<ID3D11InputLayout> lit_vertex_layout_;
  ComPtr<ID3D11InputLayout> lit_textured_vertex_layout_;
  // the normal-less lit formats, indexed (diffuse ? 1 : 0) | (specular ? 2 : 0): XYZ, XYZ|DIFFUSE, XYZ|SPECULAR,
  // XYZ|DIFFUSE|SPECULAR
  ComPtr<ID3D11VertexShader> lit_no_normal_vertex_shader_[4];
  ComPtr<ID3D11InputLayout> lit_no_normal_layout_[4];
  // the same four vertex shaders with the user clip planes, which read them at b2 and write their distances after
  // every other output (d3d9_clip_blit_shaders.hpp): the input layouts above take them as they are
  ComPtr<ID3D11VertexShader> clipped_transformed_vertex_shader_;
  ComPtr<ID3D11VertexShader> clipped_transformed_textured_vertex_shader_;
  ComPtr<ID3D11VertexShader> clipped_lit_vertex_shader_;
  ComPtr<ID3D11VertexShader> clipped_lit_textured_vertex_shader_;
  ComPtr<ID3D11Buffer> clip_planes_;
  ComPtr<ID3D11Buffer> lighting_;
  ComPtr<ID3D11Buffer> transform_;
  // the eight-stage combiner: the eight pixel-shader combinations (flat × textured × D3DFVF_SPECULAR/lit COLOR1), the
  // vertex shaders its three vertex families need, the COLOR1 input layouts the D3DFVF_SPECULAR formats read, and the
  // stages' board. the lit COLOR1 is the lighting's own specular, so the lit layouts above carry it
  ComPtr<ID3D11PixelShader> eight_pixel_[2][2][2];   // [flat][specular][textured]
  ComPtr<ID3D11VertexShader> eight_pre_spec_[2];     // is-textured
  ComPtr<ID3D11VertexShader> eight_transform_spec_[2];
  // the no-specular eight-stage vertex shaders: a texture coordinate's float4 the eight-stage pixel shaders link to
  ComPtr<ID3D11VertexShader> eight_pre_textured_;
  ComPtr<ID3D11VertexShader> eight_transform_textured_;
  // the U1 family: a one-component transformed coordinate writes (u, 1, 0, 0), which the U2 shader's (u, v, 1, 0)
  // cannot express (Wine visual.c texture_transform_flags_test's FLOAT1 declaration quad)
  ComPtr<ID3D11VertexShader> eight_transform_u1_;
  ComPtr<ID3D11InputLayout> eight_transform_u1_layout_;
  // the wide-coordinate family: a stage whose D3DFVF_TEXCOORDSIZE is three or four samples the coordinate the vertex
  // actually carries, so a D3DTTFF_PROJECTED stage divides by z or w and not by the 1 the two-component shaders fill.
  // [pre-transformed?][colour: none, diffuse, specular][width - 3]
  ComPtr<ID3D11VertexShader> wide_vertex_shader_[2][3][2];
  ComPtr<ID3D11InputLayout> wide_vertex_layout_[2][3][2];
  ComPtr<ID3D11VertexShader> eight_clipped_transform_spec_[2];
  ComPtr<ID3D11VertexShader> eight_clipped_transform_[2];
  ComPtr<ID3D11VertexShader> eight_lit_spec_[2];
  ComPtr<ID3D11VertexShader> eight_clipped_lit_spec_[2];
  ComPtr<ID3D11InputLayout> eight_pre_spec_layout_[2];
  ComPtr<ID3D11InputLayout> eight_transform_spec_layout_[2];
  ComPtr<ID3D11Buffer> stage8_; // the FixedFunctionSampling8 board the eight-stage pixel shader reads at b0
  // blend_vs_[weights - 1][active][colour]: the vertex carries `weights` blend weights, `active` of them are applied,
  // and `colour` says whether it carries a D3DFVF_DIFFUSE channel (otherwise the shader outputs white)
  ComPtr<ID3D11VertexShader> blend_vs_[3][4][2];
  ComPtr<ID3D11InputLayout> blend_layout_[3][2];
  ComPtr<ID3D11InputLayout> blend_packed_layout_;
  std::vector<DWORD> blend_dxbc_[3][4][2];
  ComPtr<ID3D11Buffer> blend_constants_;
  ComPtr<ID3D11Buffer> viewport_;
  ComPtr<ID3D11Buffer> stage_;
  // what a stage with no texture bound samples: one texel of white
  ComPtr<ID3D11Texture2D> white_texture_;
  ComPtr<ID3D11ShaderResourceView> white_view_;
};

} // namespace mullion::d3d9
