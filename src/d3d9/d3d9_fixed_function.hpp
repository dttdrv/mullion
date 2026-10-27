// Direct3D 9's fixed-function transform for the vertex formats this build draws: a vertex already in screen space
// (D3DFVF_XYZRHW, whose w is 1/rhw) with a vertex colour, and, when the draw samples a texture, the stage's texture
// coordinates. it is expressed as the shaders and the vertex layout the D3D11 core takes, so a draw goes down the same
// road a D3D11 draw does. the rasterizer, depth and blend states a draw runs under are the device's render state
// (d3d9_state.hpp), and the texture stage's operation is the constants its pixel shader reads (d3d9_texture.hpp).
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

class FixedFunctionPipeline {
public:
  // whether the fixed-function path of this build transforms a vertex format, whether its vertices carry texture
  // coordinates, and how many bytes one of them takes
  static bool supports(DWORD fvf);
  static bool textured(DWORD fvf);
  static UINT stride(DWORD fvf);

  HRESULT initialize(ID3D11Device *device);
  // the shaders, the layout and the viewport the vertex shader maps screen space through, for the viewport a draw is
  // clipped to; flat_shaded picks the pixel shader that takes one vertex's colour for the whole triangle, and sampling
  // is the texture stage a format with texture coordinates draws through (null for one without)
  HRESULT bind(
      ID3D11DeviceContext *context, const D3DVIEWPORT9 &viewport, bool flat_shaded,
      const FixedFunctionSampling *sampling
  );

private:
  ComPtr<ID3D11VertexShader> vertex_shader_;
  ComPtr<ID3D11VertexShader> textured_vertex_shader_;
  ComPtr<ID3D11PixelShader> pixel_shader_;
  ComPtr<ID3D11PixelShader> flat_pixel_shader_;
  ComPtr<ID3D11PixelShader> textured_pixel_shader_;
  ComPtr<ID3D11PixelShader> flat_textured_pixel_shader_;
  ComPtr<ID3D11InputLayout> vertex_layout_;
  ComPtr<ID3D11InputLayout> textured_vertex_layout_;
  ComPtr<ID3D11Buffer> viewport_;
  ComPtr<ID3D11Buffer> stage_;
  // what a stage with no texture bound samples: one texel of white
  ComPtr<ID3D11Texture2D> white_texture_;
  ComPtr<ID3D11ShaderResourceView> white_view_;
};

} // namespace mullion::d3d9
