#include "d3d9_fixed_function.hpp"
#include "d3d9_fixed_function_shaders.hpp"

namespace mullion::d3d9 {

namespace {

// the vertex shader's input: the position and the vertex colour, which D3DFVF_XYZRHW | D3DFVF_DIFFUSE is
const D3D11_INPUT_ELEMENT_DESC kPreTransformedLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
};

// the same with the stage's texture coordinates, which D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1 is
const D3D11_INPUT_ELEMENT_DESC kPreTransformedTexturedLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0},
};

// the pixel shader's stage constants, as the shader declares them (d3d9_fixed_function_shaders.hpp)
struct StageConstants {
  UINT32 ops[4];
  UINT32 args[4];
  float factor[4];
};

} // namespace

bool
FixedFunctionPipeline::supports(DWORD fvf) {
  return fvf == kPreTransformedFVF || fvf == kPreTransformedTexturedFVF;
}

bool
FixedFunctionPipeline::textured(DWORD fvf) {
  return fvf == kPreTransformedTexturedFVF;
}

UINT
FixedFunctionPipeline::stride(DWORD fvf) {
  return fvf == kPreTransformedTexturedFVF ? kPreTransformedTexturedStride : kPreTransformedStride;
}

HRESULT
FixedFunctionPipeline::initialize(ID3D11Device *device) {
  HRESULT hr = device->CreateVertexShader(
      kPreTransformedVertexShader, sizeof(kPreTransformedVertexShader), nullptr, &vertex_shader_
  );
  if (SUCCEEDED(hr))
    hr = device->CreateVertexShader(
        kPreTransformedTexturedVertexShader, sizeof(kPreTransformedTexturedVertexShader), nullptr,
        &textured_vertex_shader_
    );
  if (SUCCEEDED(hr))
    hr = device->CreatePixelShader(kVertexColourPixelShader, sizeof(kVertexColourPixelShader), nullptr, &pixel_shader_);
  if (SUCCEEDED(hr))
    hr = device->CreatePixelShader(
        kFlatVertexColourPixelShader, sizeof(kFlatVertexColourPixelShader), nullptr, &flat_pixel_shader_
    );
  if (SUCCEEDED(hr))
    hr = device->CreatePixelShader(
        kTexturedPixelShader, sizeof(kTexturedPixelShader), nullptr, &textured_pixel_shader_
    );
  if (SUCCEEDED(hr))
    hr = device->CreatePixelShader(
        kFlatTexturedPixelShader, sizeof(kFlatTexturedPixelShader), nullptr, &flat_textured_pixel_shader_
    );
  if (SUCCEEDED(hr))
    hr = device->CreateInputLayout(
        kPreTransformedLayout, ARRAYSIZE(kPreTransformedLayout), kPreTransformedVertexShader,
        sizeof(kPreTransformedVertexShader), &vertex_layout_
    );
  if (SUCCEEDED(hr))
    hr = device->CreateInputLayout(
        kPreTransformedTexturedLayout, ARRAYSIZE(kPreTransformedTexturedLayout), kPreTransformedTexturedVertexShader,
        sizeof(kPreTransformedTexturedVertexShader), &textured_vertex_layout_
    );
  if (FAILED(hr)) {
    ERR("D3D9: the fixed-function shaders this build carries do not load: 0x", std::hex, hr);
    return hr;
  }

  // what the vertex shader maps screen space with: the viewport's origin and size
  D3D11_BUFFER_DESC viewport_desc{};
  viewport_desc.ByteWidth = 16;
  viewport_desc.Usage = D3D11_USAGE_DYNAMIC;
  viewport_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  viewport_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  hr = device->CreateBuffer(&viewport_desc, nullptr, &viewport_);
  if (FAILED(hr))
    return hr;

  // what the textured pixel shader's stage reads: its operation, its arguments and D3DRS_TEXTUREFACTOR
  D3D11_BUFFER_DESC stage_desc{};
  stage_desc.ByteWidth = sizeof(StageConstants);
  stage_desc.Usage = D3D11_USAGE_DYNAMIC;
  stage_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  stage_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  hr = device->CreateBuffer(&stage_desc, nullptr, &stage_);
  if (FAILED(hr))
    return hr;

  // the one white texel a stage with no texture bound samples
  const DWORD white = 0xFFFFFFFF;
  D3D11_SUBRESOURCE_DATA initial{};
  initial.pSysMem = &white;
  initial.SysMemPitch = sizeof(white);
  D3D11_TEXTURE2D_DESC white_desc{};
  white_desc.Width = 1;
  white_desc.Height = 1;
  white_desc.MipLevels = 1;
  white_desc.ArraySize = 1;
  white_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  white_desc.SampleDesc.Count = 1;
  white_desc.Usage = D3D11_USAGE_DEFAULT;
  white_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  hr = device->CreateTexture2D(&white_desc, &initial, &white_texture_);
  if (SUCCEEDED(hr))
    hr = device->CreateShaderResourceView(white_texture_.Get(), nullptr, &white_view_);
  if (FAILED(hr))
    ERR("D3D9: the white texel a stage without a texture samples is not there: 0x", std::hex, hr);
  return hr;
}

HRESULT
FixedFunctionPipeline::bind(
    ID3D11DeviceContext *context, const D3DVIEWPORT9 &viewport, bool flat_shaded,
    const FixedFunctionSampling *sampling
) {
  if (!vertex_shader_ || !textured_vertex_shader_ || !pixel_shader_ || !flat_pixel_shader_ ||
      !textured_pixel_shader_ || !flat_textured_pixel_shader_ || !vertex_layout_ || !textured_vertex_layout_ ||
      !viewport_)
    return D3DERR_INVALIDCALL;

  D3D11_VIEWPORT d3d11_viewport{(float)viewport.X, (float)viewport.Y, (float)viewport.Width,
                                (float)viewport.Height, viewport.MinZ,          viewport.MaxZ};
  context->RSSetViewports(1, &d3d11_viewport);
  // D3D9's viewport is a clip rectangle as well as a place: the scissor is what clips the draw to it
  const D3D11_RECT clip{(LONG)viewport.X, (LONG)viewport.Y, (LONG)(viewport.X + viewport.Width),
                        (LONG)(viewport.Y + viewport.Height)};
  context->RSSetScissorRects(1, &clip);
  context->VSSetShader(sampling ? textured_vertex_shader_.Get() : vertex_shader_.Get(), nullptr, 0);
  context->IASetInputLayout(sampling ? textured_vertex_layout_.Get() : vertex_layout_.Get());
  context->PSSetShader(
      sampling ? (flat_shaded ? flat_textured_pixel_shader_.Get() : textured_pixel_shader_.Get())
               : (flat_shaded ? flat_pixel_shader_.Get() : pixel_shader_.Get()),
      nullptr, 0
  );

  if (sampling) {
    ID3D11ShaderResourceView *texture = sampling->texture ? sampling->texture : white_view_.Get();
    if (!texture || !sampling->sampler)
      return D3DERR_INVALIDCALL;
    context->PSSetShaderResources(0, 1, &texture);
    ID3D11SamplerState *sampler = sampling->sampler;
    context->PSSetSamplers(0, 1, &sampler);

    StageConstants constants{};
    memcpy(constants.ops, sampling->ops, sizeof(constants.ops));
    memcpy(constants.args, sampling->args, sizeof(constants.args));
    memcpy(constants.factor, sampling->factor, sizeof(constants.factor));
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(stage_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
      return D3DERR_INVALIDCALL;
    memcpy(mapped.pData, &constants, sizeof(constants));
    context->Unmap(stage_.Get(), 0);
    ID3D11Buffer *stage = stage_.Get();
    context->PSSetConstantBuffers(0, 1, &stage);
  } else {
    // an untextured draw samples nothing: the texture a draw before it sampled is not left bound to this one
    ID3D11ShaderResourceView *none = nullptr;
    context->PSSetShaderResources(0, 1, &none);
  }

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
