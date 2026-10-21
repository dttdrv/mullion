// Direct3D 9's fixed-function transform, for the one vertex format this milestone draws: a vertex already in screen
// space (D3DFVF_XYZRHW, whose w is 1/rhw) with a vertex colour. it is expressed as the two shaders and the vertex
// layout the D3D11 core takes, so a draw goes down the same road a D3D11 draw does.
#pragma once
#include "d3d9_common.hpp"

namespace mullion::d3d9 {

struct PreTransformedVertex {
  float x, y, z, rhw;
  D3DCOLOR colour;
};

inline constexpr UINT kPreTransformedStride = sizeof(PreTransformedVertex);
// D3DFVF_XYZRHW | D3DFVF_DIFFUSE: the format this milestone transforms, and no other
inline constexpr DWORD kPreTransformedFVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;

class FixedFunctionPipeline {
public:
  // whether the fixed-function path of this milestone transforms a vertex format
  static bool supports(DWORD fvf) {
    return fvf == kPreTransformedFVF;
  }

  HRESULT initialize(ID3D11Device *device);
  // the shaders, the layout and the viewport the vertex shader maps screen space through, for a target of this size
  HRESULT bind(ID3D11DeviceContext *context, UINT width, UINT height);

private:
  ComPtr<ID3D11VertexShader> vertex_shader_;
  ComPtr<ID3D11PixelShader> pixel_shader_;
  ComPtr<ID3D11InputLayout> vertex_layout_;
  ComPtr<ID3D11RasterizerState> rasterizer_;
  ComPtr<ID3D11Buffer> viewport_;
};

} // namespace mullion::d3d9
