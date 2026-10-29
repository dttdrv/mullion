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

inline constexpr UINT kTransformedStride = sizeof(TransformedVertex);
inline constexpr UINT kTransformedTexturedStride = sizeof(TransformedTexturedVertex);
inline constexpr DWORD kTransformedFVF = D3DFVF_XYZ | D3DFVF_DIFFUSE;
inline constexpr DWORD kTransformedTexturedFVF = kTransformedFVF | D3DFVF_TEX1;

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

// what a textured draw binds beside its geometry: the texture stage's operation (d3d9_texture.hpp's constants) and what
// it samples. a stage with no texture bound samples one white texel, since D3D9 leaves the value of a texture argument
// whose stage is processed without a texture undefined and white is what a blend with nothing in it leaves.
struct FixedFunctionSampling {
  ID3D11ShaderResourceView *texture = nullptr;
  ID3D11SamplerState *sampler = nullptr;
  UINT32 ops[4] = {};
  UINT32 args[4] = {};
  float factor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
};

struct LightConstants;

// what one fixed-function draw is, as the pipeline needs it to bind: the viewport it is placed and clipped by, the stage
// it samples and the two constants a transformed or lit vertex reads -- which between them pick the shader and layout.
// The draw path works this out from the device state (d3d9_device_draw.cpp); nothing here is D3D11 state.
struct FixedFunctionDraw {
  D3DVIEWPORT9 viewport{};
  RECT clip{};
  // D3DSHADE_FLAT takes one vertex's colour for the whole triangle
  bool flat_shaded = false;
  // the texture stage a format with texture coordinates draws through, or null for one without (or for a stage whose
  // first colour argument is the texture and which has none, which is not processed at all)
  const FixedFunctionSampling *sampling = nullptr;
  // D3D9's composed world, view and projection for a vertex the device transforms, or null for a vertex already in
  // screen space
  const float *world_view_projection = nullptr;
  // what a lit format's vertex colour is computed from, or null for a format with no normal (d3d9_lights.hpp)
  const LightConstants *lighting = nullptr;
};

class FixedFunctionPipeline {
public:
  // whether the fixed-function path of this build transforms a vertex format, whether the device transforms its
  // vertices (rather than their being in screen space already), whether they are lit (D3DFVF_NORMAL, whose normals the
  // lighting reads), whether they carry texture coordinates, and how many bytes one of them takes
  static bool supports(DWORD fvf);
  static bool transformed(DWORD fvf);
  static bool lit(DWORD fvf);
  static bool textured(DWORD fvf);
  static UINT stride(DWORD fvf);

  HRESULT initialize(ID3D11Device *device);
  // the objects initialize made belong to the device that made them: a Reset that makes a new device releases them
  // first, and initializes over the new one
  void release_device();
  // the draw bound: the shader and layout its format picks, the pixel shader its shading mode picks, the viewport it
  // is placed and clipped by, and the texture stage, transform and lights it reads, each at the constant buffer its
  // shader declares (d3d9_transform_shaders.hpp, d3d9_lighting_shaders.hpp). Everything the draw is comes from `draw`;
  // the pipeline owns the D3D11 objects and how they are bound
  HRESULT bind(ID3D11DeviceContext *context, const FixedFunctionDraw &draw);

private:
  ComPtr<ID3D11VertexShader> vertex_shader_;
  ComPtr<ID3D11VertexShader> textured_vertex_shader_;
  ComPtr<ID3D11PixelShader> pixel_shader_;
  ComPtr<ID3D11PixelShader> flat_pixel_shader_;
  ComPtr<ID3D11PixelShader> textured_pixel_shader_;
  ComPtr<ID3D11PixelShader> flat_textured_pixel_shader_;
  ComPtr<ID3D11InputLayout> vertex_layout_;
  ComPtr<ID3D11InputLayout> textured_vertex_layout_;
  // the transform's path: the vertex shaders of the two vertex the device transforms, their layouts, and the composed
  // matrix they read
  ComPtr<ID3D11VertexShader> transformed_vertex_shader_;
  ComPtr<ID3D11VertexShader> transformed_textured_vertex_shader_;
  ComPtr<ID3D11InputLayout> transformed_vertex_layout_;
  ComPtr<ID3D11InputLayout> transformed_textured_vertex_layout_;
  // the lit path: the vertex shaders of the two lit formats, their layouts, and the material, lights and lighting states
  // they read at b1
  ComPtr<ID3D11VertexShader> lit_vertex_shader_;
  ComPtr<ID3D11VertexShader> lit_textured_vertex_shader_;
  ComPtr<ID3D11InputLayout> lit_vertex_layout_;
  ComPtr<ID3D11InputLayout> lit_textured_vertex_layout_;
  ComPtr<ID3D11Buffer> lighting_;
  ComPtr<ID3D11Buffer> transform_;
  ComPtr<ID3D11Buffer> viewport_;
  ComPtr<ID3D11Buffer> stage_;
  // what a stage with no texture bound samples: one texel of white
  ComPtr<ID3D11Texture2D> white_texture_;
  ComPtr<ID3D11ShaderResourceView> white_view_;
};

} // namespace mullion::d3d9
