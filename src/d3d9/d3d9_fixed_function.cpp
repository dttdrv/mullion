#include "d3d9_fixed_function.hpp"
#include "d3d9_fixed_function_shaders.hpp"

namespace mullion::d3d9 {

namespace {

// the vertex shader's input: the position and the vertex colour, which D3DFVF_XYZRHW | D3DFVF_DIFFUSE is
const D3D11_INPUT_ELEMENT_DESC kPreTransformedLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
};

} // namespace

HRESULT
FixedFunctionPipeline::initialize(ID3D11Device *device) {
  HRESULT hr = device->CreateVertexShader(
      kPreTransformedVertexShader, sizeof(kPreTransformedVertexShader), nullptr, &vertex_shader_
  );
  if (SUCCEEDED(hr))
    hr = device->CreatePixelShader(kVertexColourPixelShader, sizeof(kVertexColourPixelShader), nullptr, &pixel_shader_);
  if (SUCCEEDED(hr))
    hr = device->CreateInputLayout(
        kPreTransformedLayout, ARRAYSIZE(kPreTransformedLayout), kPreTransformedVertexShader,
        sizeof(kPreTransformedVertexShader), &vertex_layout_
    );
  if (FAILED(hr)) {
    ERR("D3D9: the fixed-function shaders this build carries do not load: 0x%08x", hr);
    return hr;
  }

  // D3D9's default rasterizer: cull counter-clockwise faces, which is the clockwise winding of a screen-space
  // triangle counted as its front (the y of clip space points the other way than a window's)
  D3D11_RASTERIZER_DESC rasterizer_desc{};
  rasterizer_desc.FillMode = D3D11_FILL_SOLID;
  rasterizer_desc.CullMode = D3D11_CULL_BACK;
  rasterizer_desc.FrontCounterClockwise = TRUE;
  rasterizer_desc.DepthClipEnable = TRUE;
  hr = device->CreateRasterizerState(&rasterizer_desc, &rasterizer_);
  if (FAILED(hr))
    return hr;

  // what the vertex shader maps screen space with: the target's size
  D3D11_BUFFER_DESC viewport_desc{};
  viewport_desc.ByteWidth = 16;
  viewport_desc.Usage = D3D11_USAGE_DYNAMIC;
  viewport_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  viewport_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  return device->CreateBuffer(&viewport_desc, nullptr, &viewport_);
}

HRESULT
FixedFunctionPipeline::bind(ID3D11DeviceContext *context, UINT width, UINT height) {
  if (!vertex_shader_ || !pixel_shader_ || !vertex_layout_ || !viewport_)
    return D3DERR_INVALIDCALL;

  D3D11_VIEWPORT viewport{0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f};
  context->RSSetViewports(1, &viewport);
  context->RSSetState(rasterizer_.Get());
  context->VSSetShader(vertex_shader_.Get(), nullptr, 0);
  context->PSSetShader(pixel_shader_.Get(), nullptr, 0);
  context->IASetInputLayout(vertex_layout_.Get());

  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context->Map(viewport_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    return D3DERR_INVALIDCALL;
  const float size[4] = {(float)width, (float)height, 0.0f, 0.0f};
  memcpy(mapped.pData, size, sizeof(size));
  context->Unmap(viewport_.Get(), 0);
  ID3D11Buffer *constant = viewport_.Get();
  context->VSSetConstantBuffers(0, 1, &constant);
  return S_OK;
}

} // namespace mullion::d3d9
