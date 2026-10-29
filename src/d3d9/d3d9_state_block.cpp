// a state block's values, read from and written to the device through its own interfaces: what a block carries is the
// state this device holds (d3d9_state_block.hpp), so every value it reads or writes is a call the application could have
// made itself, and a state the device refuses is refused to the block too rather than kept by neither.
#include "d3d9_state_block.hpp"
#include "d3d9_device.hpp"

namespace mullion::d3d9 {

const IID IID_IMTLD3D9StateBlock = {0x8b0c3e52, 0x16a4, 0x4c7b, {0x9a, 0x11, 0x3f, 0x68, 0x2c, 0x55, 0xd0, 0x87}};

namespace {

// whether a render state is pixel state, vertex state, or both, as D3D9's three lists name them (Microsoft Learn,
// "Saving Pixel State With a StateBlock" and "Saving Vertex State With a StateBlock"): the alpha test, the blend, the
// depth test and the colour write are the pixel pipeline's; the cull mode is the vertex pipeline's
bool pixel_state(D3DRENDERSTATETYPE state) {
  return state != D3DRS_CULLMODE;
}

bool vertex_state(D3DRENDERSTATETYPE state) {
  return state == D3DRS_CULLMODE;
}

bool carries(D3DSTATEBLOCKTYPE type, D3DRENDERSTATETYPE state) {
  switch (type) {
  case D3DSBT_PIXELSTATE:
    return pixel_state(state);
  case D3DSBT_VERTEXSTATE:
    return vertex_state(state);
  default:
    return pixel_state(state) || vertex_state(state);
  }
}

} // namespace

HRESULT
MTLD3D9StateBlock::QueryInterface(REFIID riid, void **object) {
  if (!object)
    return E_POINTER;
  *object = nullptr;
  if (riid == IID_IUnknown || riid == __uuidof(IDirect3DStateBlock9)) {
    *object = static_cast<IDirect3DStateBlock9 *>(this);
  } else {
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

HRESULT
MTLD3D9StateBlock::GetDevice(IDirect3DDevice9 **device) {
  if (!device || !device_)
    return D3DERR_INVALIDCALL;
  *device = static_cast<IDirect3DDevice9 *>(device_);
  (*device)->AddRef();
  return S_OK;
}

// the states `type` names, with the values the device holds now. D3D9's pixel state is the pixel render states, the
// texture stage states, the sampler states, the textures and the pixel shader with its constants; vertex state is the
// vertex render states and the vertex shader with its constants; and all device state is both, plus the stream source,
// the indices and the viewport (which D3D9's all-state list has and the other two do not)
HRESULT
MTLD3D9StateBlock::capture_all() {
  if (!device_)
    return D3DERR_INVALIDCALL;
  const bool pixel = type_ != D3DSBT_VERTEXSTATE;
  const bool vertex = type_ != D3DSBT_PIXELSTATE;

  keys_.clear();
  // the render states are the pixel pipeline's and the vertex pipeline's: each kind of block carries its own
  {
    UINT count = 0;
    const D3DRENDERSTATETYPE *states = RenderState::all_states(count);
    for (UINT at = 0; at < count; at++)
      if (carries(type_, states[at]))
        keys_.push_back(StateKey{StateKey::Render, (UINT)states[at], 0});
  }
  if (pixel) {
    UINT stage_types = 0;
    const D3DTEXTURESTAGESTATETYPE *types = TextureStageState9::all_types(stage_types);
    for (UINT at = 0; at < stage_types; at++)
      keys_.push_back(StateKey{StateKey::TextureStage, 0, (UINT)types[at]});

    UINT sampler_types = 0;
    const D3DSAMPLERSTATETYPE *samplers = SamplerState9::all_types(sampler_types);
    for (UINT stage = 0; stage < MTLD3D9Device::kTextureStages; stage++)
      for (UINT at = 0; at < sampler_types; at++)
        keys_.push_back(StateKey{StateKey::Sampler, stage, (UINT)samplers[at]});

    for (UINT stage = 0; stage < MTLD3D9Device::kTextureStages; stage++)
      keys_.push_back(StateKey{StateKey::Texture, stage, 0});

    keys_.push_back(StateKey{StateKey::PixelShader, 0, 0});
    keys_.push_back(StateKey{StateKey::PixelConstantF, 0, 256});
    keys_.push_back(StateKey{StateKey::PixelConstantI, 0, 16});
    keys_.push_back(StateKey{StateKey::PixelConstantB, 0, 16});
  }
  if (vertex) {
    keys_.push_back(StateKey{StateKey::VertexShader, 0, 0});
    keys_.push_back(StateKey{StateKey::VertexConstantF, 0, 256});
    keys_.push_back(StateKey{StateKey::VertexConstantI, 0, 16});
    keys_.push_back(StateKey{StateKey::VertexConstantB, 0, 16});
  }
  if (type_ == D3DSBT_ALL) {
    keys_.push_back(StateKey{StateKey::FVF, 0, 0});
    keys_.push_back(StateKey{StateKey::Declaration, 0, 0});
    keys_.push_back(StateKey{StateKey::Stream, 0, 0});
    keys_.push_back(StateKey{StateKey::Indices, 0, 0});
    keys_.push_back(StateKey{StateKey::Viewport, 0, 0});
  }

  values_.assign(keys_.size(), StateValue{});
  for (size_t at = 0; at < keys_.size(); at++)
    if (FAILED(read(keys_[at], values_[at])))
      return D3DERR_INVALIDCALL;
  return S_OK;
}

// the states a recording touched, with the values they have now
HRESULT
MTLD3D9StateBlock::capture_recorded(const std::vector<StateKey> &keys) {
  if (!device_)
    return D3DERR_INVALIDCALL;
  keys_.clear();
  values_.clear();
  for (const StateKey &key : keys) {
    StateValue value;
    if (FAILED(read(key, value)))
      return D3DERR_INVALIDCALL;
    keys_.push_back(key);
    values_.push_back(std::move(value));
  }
  // a block made by recording carries exactly what was recorded, whatever its type was asked for
  return S_OK;
}

HRESULT
MTLD3D9StateBlock::Capture() {
  if (!device_)
    return D3DERR_INVALIDCALL;
  // D3D9 refuses a capture while the device is recording a block
  if (device_->state_block_recording())
    return D3DERR_INVALIDCALL;
  for (size_t at = 0; at < keys_.size(); at++)
    if (FAILED(read(keys_[at], values_[at])))
      return D3DERR_INVALIDCALL;
  return S_OK;
}

HRESULT
MTLD3D9StateBlock::Apply() {
  if (!device_)
    return D3DERR_INVALIDCALL;
  if (device_->state_block_recording())
    return D3DERR_INVALIDCALL;
  for (size_t at = 0; at < keys_.size(); at++)
    if (FAILED(write(keys_[at], values_[at])))
      return D3DERR_INVALIDCALL;
  return S_OK;
}

HRESULT
MTLD3D9StateBlock::read(const StateKey &key, StateValue &value) {
  IDirect3DDevice9 *device = static_cast<IDirect3DDevice9 *>(device_);
  switch (key.kind) {
  case StateKey::Render:
    return device->GetRenderState((D3DRENDERSTATETYPE)key.index, &value.value);
  case StateKey::TextureStage:
    return device->GetTextureStageState(key.index, (D3DTEXTURESTAGESTATETYPE)key.index2, &value.value);
  case StateKey::Sampler:
    return device->GetSamplerState(key.index, (D3DSAMPLERSTATETYPE)key.index2, &value.value);
  case StateKey::Texture: {
    ComPtr<IDirect3DBaseTexture9> texture;
    const HRESULT hr = device->GetTexture(key.index, &texture);
    value.object = texture;
    return hr;
  }
  case StateKey::VertexShader: {
    ComPtr<IDirect3DVertexShader9> shader;
    const HRESULT hr = device->GetVertexShader(&shader);
    value.object = shader;
    return hr;
  }
  case StateKey::PixelShader: {
    ComPtr<IDirect3DPixelShader9> shader;
    const HRESULT hr = device->GetPixelShader(&shader);
    value.object = shader;
    return hr;
  }
  case StateKey::VertexConstantF:
  case StateKey::VertexConstantI:
  case StateKey::VertexConstantB:
  case StateKey::PixelConstantF:
  case StateKey::PixelConstantI:
  case StateKey::PixelConstantB: {
    const bool vertex = key.kind == StateKey::VertexConstantF || key.kind == StateKey::VertexConstantI ||
                        key.kind == StateKey::VertexConstantB;
    const UINT count = key.index2;
    if (key.kind == StateKey::VertexConstantF || key.kind == StateKey::PixelConstantF) {
      value.float_range.assign((size_t)count * 4, 0.0f);
      return vertex ? device->GetVertexShaderConstantF(key.index, value.float_range.data(), count)
                    : device->GetPixelShaderConstantF(key.index, value.float_range.data(), count);
    }
    if (key.kind == StateKey::VertexConstantI || key.kind == StateKey::PixelConstantI) {
      value.int_range.assign((size_t)count * 4, 0);
      return vertex ? device->GetVertexShaderConstantI(key.index, value.int_range.data(), count)
                    : device->GetPixelShaderConstantI(key.index, value.int_range.data(), count);
    }
    value.flag_range.assign(count, FALSE);
    return vertex ? device->GetVertexShaderConstantB(key.index, value.flag_range.data(), count)
                  : device->GetPixelShaderConstantB(key.index, value.flag_range.data(), count);
  }
  case StateKey::FVF:
    return device->GetFVF(&value.value);
  case StateKey::Declaration: {
    ComPtr<IDirect3DVertexDeclaration9> declaration;
    const HRESULT hr = device->GetVertexDeclaration(&declaration);
    value.object = declaration;
    return hr;
  }
  case StateKey::Stream: {
    ComPtr<IDirect3DVertexBuffer9> buffer;
    const HRESULT hr = device->GetStreamSource(key.index, &buffer, &value.offset, &value.stride);
    value.object = buffer;
    return hr;
  }
  case StateKey::Indices: {
    ComPtr<IDirect3DIndexBuffer9> indices;
    const HRESULT hr = device->GetIndices(&indices);
    value.object = indices;
    return hr;
  }
  case StateKey::Viewport:
    return device->GetViewport(&value.viewport);
  }
  return D3DERR_INVALIDCALL;
}

HRESULT
MTLD3D9StateBlock::write(const StateKey &key, const StateValue &value) {
  IDirect3DDevice9 *device = static_cast<IDirect3DDevice9 *>(device_);
  switch (key.kind) {
  case StateKey::Render:
    return device->SetRenderState((D3DRENDERSTATETYPE)key.index, value.value);
  case StateKey::TextureStage:
    return device->SetTextureStageState(key.index, (D3DTEXTURESTAGESTATETYPE)key.index2, value.value);
  case StateKey::Sampler:
    return device->SetSamplerState(key.index, (D3DSAMPLERSTATETYPE)key.index2, value.value);
  case StateKey::Texture:
    return device->SetTexture(key.index, static_cast<IDirect3DBaseTexture9 *>(value.object.Get()));
  case StateKey::VertexShader:
    return device->SetVertexShader(static_cast<IDirect3DVertexShader9 *>(value.object.Get()));
  case StateKey::PixelShader:
    return device->SetPixelShader(static_cast<IDirect3DPixelShader9 *>(value.object.Get()));
  case StateKey::VertexConstantF:
    return device->SetVertexShaderConstantF(key.index, value.float_range.data(), key.index2);
  case StateKey::VertexConstantI:
    return device->SetVertexShaderConstantI(key.index, value.int_range.data(), key.index2);
  case StateKey::VertexConstantB:
    return device->SetVertexShaderConstantB(key.index, value.flag_range.data(), key.index2);
  case StateKey::PixelConstantF:
    return device->SetPixelShaderConstantF(key.index, value.float_range.data(), key.index2);
  case StateKey::PixelConstantI:
    return device->SetPixelShaderConstantI(key.index, value.int_range.data(), key.index2);
  case StateKey::PixelConstantB:
    return device->SetPixelShaderConstantB(key.index, value.flag_range.data(), key.index2);
  case StateKey::FVF:
    return device->SetFVF(value.value);
  case StateKey::Declaration:
    return device->SetVertexDeclaration(static_cast<IDirect3DVertexDeclaration9 *>(value.object.Get()));
  case StateKey::Stream:
    return device->SetStreamSource(key.index, static_cast<IDirect3DVertexBuffer9 *>(value.object.Get()), value.offset,
                                   value.stride);
  case StateKey::Indices:
    return device->SetIndices(static_cast<IDirect3DIndexBuffer9 *>(value.object.Get()));
  case StateKey::Viewport:
    return device->SetViewport(&value.viewport);
  }
  return D3DERR_INVALIDCALL;
}

} // namespace mullion::d3d9
