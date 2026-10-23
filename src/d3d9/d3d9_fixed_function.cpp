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
    hr = device->CreatePixelShader(
        kFlatVertexColourPixelShader, sizeof(kFlatVertexColourPixelShader), nullptr, &flat_pixel_shader_
    );
  if (SUCCEEDED(hr))
    hr = device->CreateInputLayout(
        kPreTransformedLayout, ARRAYSIZE(kPreTransformedLayout), kPreTransformedVertexShader,
        sizeof(kPreTransformedVertexShader), &vertex_layout_
    );
  if (FAILED(hr)) {
    ERR("D3D9: the fixed-function shaders this build carries do not load: 0x%08x", hr);
    return hr;
  }

  // what the vertex shader maps screen space with: the viewport's origin and size
  D3D11_BUFFER_DESC viewport_desc{};
  viewport_desc.ByteWidth = 16;
  viewport_desc.Usage = D3D11_USAGE_DYNAMIC;
  viewport_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  viewport_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  return device->CreateBuffer(&viewport_desc, nullptr, &viewport_);
}

HRESULT
FixedFunctionPipeline::bind(ID3D11DeviceContext *context, const D3DVIEWPORT9 &viewport, bool flat_shaded) {
  if (!vertex_shader_ || !pixel_shader_ || !flat_pixel_shader_ || !vertex_layout_ || !viewport_)
    return D3DERR_INVALIDCALL;

  D3D11_VIEWPORT d3d11_viewport{(float)viewport.X, (float)viewport.Y, (float)viewport.Width,
                                (float)viewport.Height, viewport.MinZ,          viewport.MaxZ};
  context->RSSetViewports(1, &d3d11_viewport);
  // D3D9's viewport is a clip rectangle as well as a place: the scissor is what clips the draw to it
  const D3D11_RECT clip{(LONG)viewport.X, (LONG)viewport.Y, (LONG)(viewport.X + viewport.Width),
                        (LONG)(viewport.Y + viewport.Height)};
  context->RSSetScissorRects(1, &clip);
  context->VSSetShader(vertex_shader_.Get(), nullptr, 0);
  context->PSSetShader(flat_shaded ? flat_pixel_shader_.Get() : pixel_shader_.Get(), nullptr, 0);
  context->IASetInputLayout(vertex_layout_.Get());

  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context->Map(viewport_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    return D3DERR_INVALIDCALL;
  const float constant[4] = {(float)viewport.X, (float)viewport.Y, (float)viewport.Width, (float)viewport.Height};
  memcpy(mapped.pData, constant, sizeof(constant));
  context->Unmap(viewport_.Get(), 0);
  ID3D11Buffer *buffer = viewport_.Get();
  context->VSSetConstantBuffers(0, 1, &buffer);
  return S_OK;
}

} // namespace mullion::d3d9
