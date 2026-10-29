#include "d3d9_fixed_function.hpp"
#include "d3d9_fixed_function_shaders.hpp"
#include "d3d9_lights.hpp"
#include "d3d9_lighting_shaders.hpp"
#include "d3d9_transform_shaders.hpp"

namespace mullion::d3d9 {

namespace {

// the vertex shader's input: the position and the vertex colour, which D3DFVF_XYZRHW | D3DFVF_DIFFUSE is
const D3D11_INPUT_ELEMENT_DESC kPreTransformedLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
};

// a vertex the device transforms reads its position and colour, and the textured one the stage's coordinates: the
// transform shaders take three floats of position and two of texture coordinate, which is what D3DFVF_XYZ and
// D3DFVF_TEX1's default of two coordinates are
const D3D11_INPUT_ELEMENT_DESC kTransformedLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
};

const D3D11_INPUT_ELEMENT_DESC kTransformedTexturedLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
};

// the same with the stage's texture coordinates, which D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1 is
const D3D11_INPUT_ELEMENT_DESC kPreTransformedTexturedLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0},
};

// a vertex the device transforms and lights: the normal comes straight after the position, which is the order
// D3DFVF_XYZ | D3DFVF_NORMAL puts them in, and the colour and the stage's coordinates after it
const D3D11_INPUT_ELEMENT_DESC kLitLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
};

const D3D11_INPUT_ELEMENT_DESC kLitTexturedLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
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
  return fvf == kPreTransformedFVF || fvf == kPreTransformedTexturedFVF || fvf == kTransformedFVF ||
         fvf == kTransformedTexturedFVF || fvf == kLitFVF || fvf == kLitTexturedFVF;
}

bool
FixedFunctionPipeline::lit(DWORD fvf) {
  return fvf == kLitFVF || fvf == kLitTexturedFVF;
}

bool
FixedFunctionPipeline::transformed(DWORD fvf) {
  // a lit vertex is one the device transforms too: its position takes the same matrices
  return fvf == kTransformedFVF || fvf == kTransformedTexturedFVF || lit(fvf);
}

bool
FixedFunctionPipeline::textured(DWORD fvf) {
  return fvf == kPreTransformedTexturedFVF || fvf == kTransformedTexturedFVF || fvf == kLitTexturedFVF;
}

UINT
FixedFunctionPipeline::stride(DWORD fvf) {
  switch (fvf) {
  case kPreTransformedTexturedFVF:
    return kPreTransformedTexturedStride;
  case kTransformedFVF:
    return kTransformedStride;
  case kTransformedTexturedFVF:
    return kTransformedTexturedStride;
  case kLitFVF:
    return kLitStride;
  case kLitTexturedFVF:
    return kLitTexturedStride;
  default:
    return kPreTransformedStride;
  }
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
  // the transform's vertex shaders, which read the composed matrix at b0 (d3d9_transform_shaders.hpp)
  if (SUCCEEDED(hr))
    hr = device->CreateVertexShader(
        kTransformedVertexShader, sizeof(kTransformedVertexShader), nullptr, &transformed_vertex_shader_
    );
  if (SUCCEEDED(hr))
    hr = device->CreateVertexShader(
        kTransformedTexturedVertexShader, sizeof(kTransformedTexturedVertexShader), nullptr,
        &transformed_textured_vertex_shader_
    );
  if (SUCCEEDED(hr))
    hr = device->CreateInputLayout(
        kTransformedLayout, ARRAYSIZE(kTransformedLayout), kTransformedVertexShader, sizeof(kTransformedVertexShader),
        &transformed_vertex_layout_
    );
  if (SUCCEEDED(hr))
    hr = device->CreateInputLayout(
        kTransformedTexturedLayout, ARRAYSIZE(kTransformedTexturedLayout), kTransformedTexturedVertexShader,
        sizeof(kTransformedTexturedVertexShader), &transformed_textured_vertex_layout_
    );
  // the lit vertex shaders, which read the composed matrix at b0 and the material, the lights and the lighting states
  // at b1 (d3d9_lighting_shaders.hpp)
  if (SUCCEEDED(hr))
    hr = device->CreateVertexShader(kLitVertexShader, sizeof(kLitVertexShader), nullptr, &lit_vertex_shader_);
  if (SUCCEEDED(hr))
    hr = device->CreateVertexShader(
        kLitTexturedVertexShader, sizeof(kLitTexturedVertexShader), nullptr, &lit_textured_vertex_shader_
    );
  if (SUCCEEDED(hr))
    hr = device->CreateInputLayout(
        kLitLayout, ARRAYSIZE(kLitLayout), kLitVertexShader, sizeof(kLitVertexShader), &lit_vertex_layout_
    );
  if (SUCCEEDED(hr))
    hr = device->CreateInputLayout(
        kLitTexturedLayout, ARRAYSIZE(kLitTexturedLayout), kLitTexturedVertexShader, sizeof(kLitTexturedVertexShader),
        &lit_textured_vertex_layout_
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

  // the composed world, view and projection a transformed vertex is multiplied by, which changes every time one of the
  // three does: written before each transformed draw
  D3D11_BUFFER_DESC transform_desc{};
  transform_desc.ByteWidth = 16 * sizeof(float);
  transform_desc.Usage = D3D11_USAGE_DYNAMIC;
  transform_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  transform_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  hr = device->CreateBuffer(&transform_desc, nullptr, &transform_);
  if (FAILED(hr))
    return hr;

  // the lights, the material and the lighting states a lit draw reads, written before each lit draw
  D3D11_BUFFER_DESC lighting_desc{};
  lighting_desc.ByteWidth = sizeof(LightConstants);
  lighting_desc.Usage = D3D11_USAGE_DYNAMIC;
  lighting_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  lighting_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  hr = device->CreateBuffer(&lighting_desc, nullptr, &lighting_);
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

void
FixedFunctionPipeline::release_device() {
  vertex_shader_.Reset();
  textured_vertex_shader_.Reset();
  transformed_vertex_shader_.Reset();
  transformed_textured_vertex_shader_.Reset();
  pixel_shader_.Reset();
  flat_pixel_shader_.Reset();
  textured_pixel_shader_.Reset();
  flat_textured_pixel_shader_.Reset();
  vertex_layout_.Reset();
  textured_vertex_layout_.Reset();
  transformed_vertex_layout_.Reset();
  transformed_textured_vertex_layout_.Reset();
  lit_vertex_shader_.Reset();
  lit_textured_vertex_shader_.Reset();
  lit_vertex_layout_.Reset();
  lit_textured_vertex_layout_.Reset();
  lighting_.Reset();
  transform_.Reset();
  viewport_.Reset();
  stage_.Reset();
  white_texture_.Reset();
  white_view_.Reset();
}

HRESULT
FixedFunctionPipeline::bind(ID3D11DeviceContext *context, const FixedFunctionDraw &draw) {
  const FixedFunctionSampling *sampling = draw.sampling;
  const float *world_view_projection = draw.world_view_projection;
  const LightConstants *lighting = draw.lighting;
  if (!vertex_shader_ || !textured_vertex_shader_ || !pixel_shader_ || !flat_pixel_shader_ ||
      !textured_pixel_shader_ || !flat_textured_pixel_shader_ || !vertex_layout_ || !textured_vertex_layout_ ||
      !viewport_)
    return D3DERR_INVALIDCALL;
  // a vertex the device transforms needs the transform's shaders and the matrix they read
  const bool transformed = world_view_projection != nullptr;
  if (transformed && (!transformed_vertex_shader_ || !transformed_textured_vertex_shader_ ||
                      !transformed_vertex_layout_ || !transformed_textured_vertex_layout_ || !transform_))
    return D3DERR_INVALIDCALL;
  // a lit vertex needs the lit ones as well, and the material and lights they read at b1
  const bool lit = lighting != nullptr;
  if (lit && (!lit_vertex_shader_ || !lit_textured_vertex_shader_ || !lit_vertex_layout_ ||
              !lit_textured_vertex_layout_ || !lighting_ || !transform_))
    return D3DERR_INVALIDCALL;

  D3D11_VIEWPORT d3d11_viewport{(float)draw.viewport.X, (float)draw.viewport.Y, (float)draw.viewport.Width,
                                (float)draw.viewport.Height, draw.viewport.MinZ, draw.viewport.MaxZ};
  context->RSSetViewports(1, &d3d11_viewport);
  // D3D9's viewport is a clip rectangle as well as a place, and the scissor rectangle narrows it: the device's clip
  // rectangle is both (d3d9_device_targets.cpp)
  const D3D11_RECT scissor{draw.clip.left, draw.clip.top, draw.clip.right, draw.clip.bottom};
  context->RSSetScissorRects(1, &scissor);
  context->VSSetShader(
      lit ? (sampling ? lit_textured_vertex_shader_.Get() : lit_vertex_shader_.Get())
          : transformed ? (sampling ? transformed_textured_vertex_shader_.Get() : transformed_vertex_shader_.Get())
                        : (sampling ? textured_vertex_shader_.Get() : vertex_shader_.Get()),
      nullptr, 0
  );
  context->IASetInputLayout(
      lit ? (sampling ? lit_textured_vertex_layout_.Get() : lit_vertex_layout_.Get())
          : transformed ? (sampling ? transformed_textured_vertex_layout_.Get() : transformed_vertex_layout_.Get())
                        : (sampling ? textured_vertex_layout_.Get() : vertex_layout_.Get())
  );
  context->PSSetShader(
      sampling ? (draw.flat_shaded ? flat_textured_pixel_shader_.Get() : textured_pixel_shader_.Get())
               : (draw.flat_shaded ? flat_pixel_shader_.Get() : pixel_shader_.Get()),
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
  if (transformed) {
    // the clip position is the vertex times the device's matrices: the divide by w and the viewport's mapping are the
    // rasterizer's, from the viewport bound above (Direct3D's fixed-function transform, Microsoft Learn, "Transforms")
    if (FAILED(context->Map(transform_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
      return D3DERR_INVALIDCALL;
    memcpy(mapped.pData, world_view_projection, 16 * sizeof(float));
    context->Unmap(transform_.Get(), 0);
    ID3D11Buffer *transform = transform_.Get();
    context->VSSetConstantBuffers(0, 1, &transform);
    if (lit) {
      // the material, the lights and the lighting states the vertex's colour is computed from, at b1
      if (FAILED(context->Map(lighting_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return D3DERR_INVALIDCALL;
      memcpy(mapped.pData, lighting, sizeof(LightConstants));
      context->Unmap(lighting_.Get(), 0);
      ID3D11Buffer *lights = lighting_.Get();
      context->VSSetConstantBuffers(1, 1, &lights);
    }
    return S_OK;
  }
  // a vertex already in screen space is mapped through the viewport by the shader, since it is past the divide and the
  // rasterizer would otherwise map it a second time
  if (FAILED(context->Map(viewport_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    return D3DERR_INVALIDCALL;
  const float constant[4] = {(float)draw.viewport.X, (float)draw.viewport.Y, (float)draw.viewport.Width,
                             (float)draw.viewport.Height};
  memcpy(mapped.pData, constant, sizeof(constant));
  context->Unmap(viewport_.Get(), 0);
  ID3D11Buffer *buffer = viewport_.Get();
  context->VSSetConstantBuffers(0, 1, &buffer);
  return S_OK;
}

} // namespace mullion::d3d9
