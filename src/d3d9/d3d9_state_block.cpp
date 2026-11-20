// a state block's values, read from and written to the device through its own interfaces: what a block carries is the
// state this device holds (d3d9_state_block.hpp), so every value it reads or writes is a call the application could have
// made itself, and a state the device refuses is refused to the block too rather than kept by neither.
#include "d3d9_state_block.hpp"
#include "d3d9_device.hpp"

namespace mullion::d3d9 {

const IID IID_IMTLD3D9StateBlock = {0x8b0c3e52, 0x16a4, 0x4c7b, {0x9a, 0x11, 0x3f, 0x68, 0x2c, 0x55, 0xd0, 0x87}};

MTLD3D9StateBlock::MTLD3D9StateBlock(MTLD3D9Device *device, D3DSTATEBLOCKTYPE type, bool counted_device)
    : device_(device), device_ref_(static_cast<IDirect3DDevice9 *>(device), counted_device), type_(type) {}

namespace {

// whether a render state is pixel state, vertex state, or both, as D3D9's three lists name them (Microsoft Learn,
// "Saving Pixel State With a StateBlock" and "Saving Vertex State With a StateBlock"): the alpha test, the blend, the
// depth test and the colour write are the pixel pipeline's; the cull mode and the user clip planes' enables are the
// vertex pipeline's
// D3D9's vertex-state list (Microsoft Learn, "Saving Vertex State With a StateBlock"), as Wine's d3d9 conformance
// suite enumerates it. SHADEMODE and the fog values appear in both lists: a block of either kind carries them.
bool vertex_state(D3DRENDERSTATETYPE state) {
  static const D3DRENDERSTATETYPE states[] = {
      D3DRS_ADAPTIVETESS_W, D3DRS_ADAPTIVETESS_X, D3DRS_ADAPTIVETESS_Y, D3DRS_ADAPTIVETESS_Z,
      D3DRS_AMBIENT, D3DRS_AMBIENTMATERIALSOURCE, D3DRS_CLIPPING, D3DRS_CLIPPLANEENABLE, D3DRS_COLORVERTEX,
      D3DRS_CULLMODE, D3DRS_DIFFUSEMATERIALSOURCE, D3DRS_EMISSIVEMATERIALSOURCE,
      D3DRS_ENABLEADAPTIVETESSELLATION, D3DRS_FOGCOLOR, D3DRS_FOGDENSITY, D3DRS_FOGENABLE, D3DRS_FOGEND,
      D3DRS_FOGSTART, D3DRS_FOGTABLEMODE, D3DRS_FOGVERTEXMODE, D3DRS_INDEXEDVERTEXBLENDENABLE, D3DRS_LIGHTING,
      D3DRS_LOCALVIEWER, D3DRS_MAXTESSELLATIONLEVEL, D3DRS_MINTESSELLATIONLEVEL, D3DRS_MULTISAMPLEANTIALIAS,
      D3DRS_MULTISAMPLEMASK, D3DRS_NORMALDEGREE, D3DRS_NORMALIZENORMALS, D3DRS_PATCHEDGESTYLE,
      D3DRS_POINTSCALE_A, D3DRS_POINTSCALE_B, D3DRS_POINTSCALE_C, D3DRS_POINTSCALEENABLE, D3DRS_POINTSIZE,
      D3DRS_POINTSIZE_MAX, D3DRS_POINTSIZE_MIN, D3DRS_POINTSPRITEENABLE, D3DRS_POSITIONDEGREE,
      D3DRS_RANGEFOGENABLE, D3DRS_SHADEMODE, D3DRS_SPECULARENABLE, D3DRS_SPECULARMATERIALSOURCE,
      D3DRS_TWEENFACTOR, D3DRS_VERTEXBLEND,
  };
  for (D3DRENDERSTATETYPE listed : states)
    if (listed == state)
      return true;
  return false;
}

bool pixel_state(D3DRENDERSTATETYPE state) {
  static const D3DRENDERSTATETYPE states[] = {
      D3DRS_ALPHABLENDENABLE, D3DRS_ALPHAFUNC, D3DRS_ALPHAREF, D3DRS_ALPHATESTENABLE,
      D3DRS_ANTIALIASEDLINEENABLE, D3DRS_BLENDFACTOR, D3DRS_BLENDOP, D3DRS_BLENDOPALPHA, D3DRS_CCW_STENCILFAIL,
      D3DRS_CCW_STENCILPASS, D3DRS_CCW_STENCILZFAIL, D3DRS_COLORWRITEENABLE, D3DRS_COLORWRITEENABLE1,
      D3DRS_COLORWRITEENABLE2, D3DRS_COLORWRITEENABLE3, D3DRS_DEPTHBIAS, D3DRS_DESTBLEND, D3DRS_DESTBLENDALPHA,
      D3DRS_DITHERENABLE, D3DRS_FILLMODE, D3DRS_FOGDENSITY, D3DRS_FOGEND, D3DRS_FOGSTART, D3DRS_LASTPIXEL,
      D3DRS_SCISSORTESTENABLE, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_SHADEMODE, D3DRS_SLOPESCALEDEPTHBIAS,
      D3DRS_SRCBLEND, D3DRS_SRCBLENDALPHA, D3DRS_SRGBWRITEENABLE, D3DRS_STENCILENABLE, D3DRS_STENCILFAIL,
      D3DRS_STENCILFUNC, D3DRS_STENCILMASK, D3DRS_STENCILPASS, D3DRS_STENCILREF, D3DRS_STENCILWRITEMASK,
      D3DRS_STENCILZFAIL, D3DRS_TEXTUREFACTOR, D3DRS_TWOSIDEDSTENCILMODE, D3DRS_WRAP0, D3DRS_WRAP1,
      D3DRS_WRAP10, D3DRS_WRAP11, D3DRS_WRAP12, D3DRS_WRAP13, D3DRS_WRAP14, D3DRS_WRAP15, D3DRS_WRAP2,
      D3DRS_WRAP3, D3DRS_WRAP4, D3DRS_WRAP5, D3DRS_WRAP6, D3DRS_WRAP7, D3DRS_WRAP8, D3DRS_WRAP9,
      D3DRS_ZENABLE, D3DRS_ZFUNC, D3DRS_ZWRITEENABLE,
  };
  for (D3DRENDERSTATETYPE listed : states)
    if (listed == state)
      return true;
  return false;
}

bool carries(D3DSTATEBLOCKTYPE type, D3DRENDERSTATETYPE state) {
  switch (type) {
  case D3DSBT_PIXELSTATE:
    return pixel_state(state);
  case D3DSBT_VERTEXSTATE:
    return vertex_state(state);
  default:
    return true;
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
  recorded_ = false;
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
    for (UINT stage = 0; stage < MTLD3D9Device::kMaxBlendStages; stage++)
      for (UINT at = 0; at < stage_types; at++)
        keys_.push_back(StateKey{StateKey::TextureStage, stage, (UINT)types[at]});

    UINT sampler_types = 0;
    const D3DSAMPLERSTATETYPE *samplers = SamplerState9::all_types(sampler_types);
    for (UINT stage = 0; stage < MTLD3D9Device::kTextureStages; stage++)
      for (UINT at = 0; at < sampler_types; at++)
        keys_.push_back(StateKey{StateKey::Sampler, stage, (UINT)samplers[at]});

    keys_.push_back(StateKey{StateKey::PixelShader, 0, 0});
    keys_.push_back(StateKey{StateKey::PixelConstantF, 0, 256});
    keys_.push_back(StateKey{StateKey::PixelConstantI, 0, 16});
    keys_.push_back(StateKey{StateKey::PixelConstantB, 0, 16});
  }
  if (vertex) {
    // D3D9's vertex state is the one the lights and the material belong to: the eight lights, which of them is enabled,
    // and the material a lit vertex's reflectances come from. an undefined light is not part of the state a block can
    // restore: GetLight answers D3DERR_INVALIDCALL for it, so a block captures only the slots SetLight defined
    keys_.push_back(StateKey{StateKey::Material, 0, 0});
    for (UINT at = 0; at < Lights::kSlots; at++) {
      D3DLIGHT9 light{};
      WINBOOL enabled = FALSE;
      if (SUCCEEDED(device_->GetLight(at, &light))) {
        keys_.push_back(StateKey{StateKey::Light, at, 0});
        keys_.push_back(StateKey{StateKey::LightEnable, at, 0});
      } else if (SUCCEEDED(device_->GetLightEnable(at, &enabled))) {
        // LightEnable without SetLight still records enable state (Wine device.c test_lights)
        keys_.push_back(StateKey{StateKey::LightEnable, at, 0});
      }
    }
    // the vertex declaration is vertex state (Wine's resource stateblock test expects a vertex-only block to restore
    // it); the FVF, stream sources, indices and textures are all-state
    keys_.push_back(StateKey{StateKey::Declaration, 0, 0});
    keys_.push_back(StateKey{StateKey::VertexShader, 0, 0});
    keys_.push_back(StateKey{StateKey::VertexConstantF, 0, 256});
    keys_.push_back(StateKey{StateKey::VertexConstantI, 0, 16});
    keys_.push_back(StateKey{StateKey::VertexConstantB, 0, 16});
    // the vertex samplers (D3DVERTEXTEXTURESAMPLER0..3, API stages 16..19) are vertex state, not the pixel stages
    UINT vertex_sampler_types = 0;
    const D3DSAMPLERSTATETYPE *vertex_samplers = SamplerState9::all_types(vertex_sampler_types);
    for (UINT at = 0; at < MTLD3D9Device::kVertexTextureStages; at++) {
      const UINT stage = D3DVERTEXTEXTURESAMPLER0 + at;
      for (UINT type = 0; type < vertex_sampler_types; type++)
        keys_.push_back(StateKey{StateKey::Sampler, stage, (UINT)vertex_samplers[type]});
    }
  }
  if (type_ == D3DSBT_ALL) {
    // The transforms are all-state: Wine's d3d9 stateblock contract keeps vertex-only and pixel-only blocks from
    // restoring them, while an all-state block captures every transform the device holds.
    keys_.push_back(StateKey{StateKey::Transform, (UINT)D3DTS_WORLD, 0});
    keys_.push_back(StateKey{StateKey::Transform, (UINT)D3DTS_WORLD1, 0});
    keys_.push_back(StateKey{StateKey::Transform, (UINT)D3DTS_WORLD2, 0});
    keys_.push_back(StateKey{StateKey::Transform, (UINT)D3DTS_WORLD3, 0});
    for (UINT world = 4; world < 256; world++)
      keys_.push_back(StateKey{StateKey::Transform, (UINT)D3DTS_WORLDMATRIX(world), 0});
    keys_.push_back(StateKey{StateKey::Transform, (UINT)D3DTS_VIEW, 0});
    keys_.push_back(StateKey{StateKey::Transform, (UINT)D3DTS_PROJECTION, 0});
    for (UINT stage = 0; stage < 8; stage++)
      keys_.push_back(StateKey{StateKey::Transform, (UINT)D3DTS_TEXTURE0 + stage, 0});
    // the six user clip planes are all-state, which neither the pixel nor the vertex list names
    for (UINT plane = 0; plane < DeviceState::kClipPlanes; plane++)
      keys_.push_back(StateKey{StateKey::ClipPlane, plane, 0});
    // Textures belong to the all-state block in D3D9's contract: a pixel-only or vertex-only block leaves them alone.
    for (UINT stage = 0; stage < MTLD3D9Device::kTextureStages; stage++)
      keys_.push_back(StateKey{StateKey::Texture, stage, 0});
    for (UINT at = 0; at < MTLD3D9Device::kVertexTextureStages; at++)
      keys_.push_back(StateKey{StateKey::Texture, D3DVERTEXTEXTURESAMPLER0 + at, 0});
    keys_.push_back(StateKey{StateKey::FVF, 0, 0});
    for (UINT stream = 0; stream < MTLD3D9Device::kMaxStreams; stream++)
      keys_.push_back(StateKey{StateKey::Stream, stream, 0});
    keys_.push_back(StateKey{StateKey::Indices, 0, 0});
    keys_.push_back(StateKey{StateKey::Viewport, 0, 0});
  }

  values_.assign(keys_.size(), StateValue{});
  for (size_t at = 0; at < keys_.size(); at++)
    if (FAILED(read(keys_[at], values_[at])))
      return D3DERR_INVALIDCALL;
  return S_OK;
}

// the states a recording touched, with the values captured as each setter ran
HRESULT
MTLD3D9StateBlock::capture_recorded(const std::vector<StateKey> &keys, const std::vector<StateValue> &values) {
  if (!device_ || keys.size() != values.size())
    return D3DERR_INVALIDCALL;
  keys_ = keys;
  values_ = values;
  recorded_ = true;
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
  for (size_t at = 0; at < keys_.size(); at++) {
    const UINT old_offset = keys_[at].kind == StateKey::Stream ? values_[at].offset : 0;
    StateValue fresh;
    if (FAILED(read(keys_[at], fresh)))
      return D3DERR_INVALIDCALL;
    // Capture of a NULL vertex declaration stores NULL. Apply then leaves the current binding (write() no-ops on a
    // NULL declaration), so Capture(NULL) → Set(decl) → Apply keeps the Set (Wine device.c stateblock declaration).
    // Skipping the update would leave the prior non-null and Apply would restore it instead.
    values_[at] = std::move(fresh);
    // A created all-state block's Capture updates the bound buffer and stride but leaves the stream offset as it was:
    // Wine's d3d9 test contract marks this as the all-state block quirk. A recorded block updates all three.
    if (!recorded_ && keys_[at].kind == StateKey::Stream)
      values_[at].offset = old_offset;
  }
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
  case StateKey::Transform:
    return device->GetTransform((D3DTRANSFORMSTATETYPE)key.index, &value.matrix);
  case StateKey::Light:
    return device->GetLight(key.index, &value.light);
  case StateKey::LightEnable:
    return device->GetLightEnable(key.index, &value.enabled);
  case StateKey::Material:
    return device->GetMaterial(&value.material);
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
  case StateKey::ClipPlane:
    return device->GetClipPlane(key.index, value.floats);
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
  case StateKey::Transform:
    return device->SetTransform((D3DTRANSFORMSTATETYPE)key.index, &value.matrix);
  case StateKey::Light:
    return device->SetLight(key.index, &value.light);
  case StateKey::LightEnable:
    return device->LightEnable(key.index, value.enabled);
  case StateKey::Material:
    return device->SetMaterial(&value.material);
  case StateKey::FVF:
    return device->SetFVF(value.value);
  case StateKey::Declaration:
    // Wine device.c: Apply of a captured/recorded NULL declaration leaves the current binding (Capture(NULL) then
    // Set then Apply keeps the Set). Snapshot restore during recording force-clears separately in record().
    if (!value.object)
      return S_OK;
    return device->SetVertexDeclaration(static_cast<IDirect3DVertexDeclaration9 *>(value.object.Get()));
  case StateKey::Stream:
    return device_->restore_stream_source(key.index, static_cast<IDirect3DVertexBuffer9 *>(value.object.Get()),
                                          value.offset, value.stride);
  case StateKey::Indices:
    return device->SetIndices(static_cast<IDirect3DIndexBuffer9 *>(value.object.Get()));
  case StateKey::Viewport:
    return device->SetViewport(&value.viewport);
  case StateKey::ClipPlane:
    return device->SetClipPlane(key.index, value.floats);
  }
  return D3DERR_INVALIDCALL;
}

} // namespace mullion::d3d9
