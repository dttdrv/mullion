/*
 * Copyright 2026 David Skoland
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "d3d9_state_block.hpp"
#include "d3d9_device.hpp"
#include "log/log.hpp"

namespace mullion {

static const D3DRENDERSTATETYPE kPixelRenderStates[] = {
    D3DRS_ZENABLE, D3DRS_FILLMODE, D3DRS_SHADEMODE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_LASTPIXEL,
    D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_ZFUNC, D3DRS_ALPHAREF, D3DRS_ALPHAFUNC, D3DRS_DITHERENABLE,
    D3DRS_ALPHABLENDENABLE, D3DRS_FOGSTART, D3DRS_FOGEND, D3DRS_FOGDENSITY, D3DRS_STENCILENABLE, D3DRS_STENCILFAIL,
    D3DRS_STENCILZFAIL, D3DRS_STENCILPASS, D3DRS_STENCILFUNC, D3DRS_STENCILREF, D3DRS_STENCILMASK,
    D3DRS_STENCILWRITEMASK, D3DRS_TEXTUREFACTOR, D3DRS_WRAP0, D3DRS_WRAP1, D3DRS_WRAP2, D3DRS_WRAP3, D3DRS_WRAP4,
    D3DRS_WRAP5, D3DRS_WRAP6, D3DRS_WRAP7, D3DRS_WRAP8, D3DRS_WRAP9, D3DRS_WRAP10, D3DRS_WRAP11, D3DRS_WRAP12,
    D3DRS_WRAP13, D3DRS_WRAP14, D3DRS_WRAP15, D3DRS_COLORWRITEENABLE, D3DRS_BLENDOP, D3DRS_SCISSORTESTENABLE,
    D3DRS_SLOPESCALEDEPTHBIAS, D3DRS_ANTIALIASEDLINEENABLE, D3DRS_TWOSIDEDSTENCILMODE, D3DRS_CCW_STENCILFAIL,
    D3DRS_CCW_STENCILZFAIL, D3DRS_CCW_STENCILPASS, D3DRS_CCW_STENCILFUNC, D3DRS_COLORWRITEENABLE1,
    D3DRS_COLORWRITEENABLE2, D3DRS_COLORWRITEENABLE3, D3DRS_BLENDFACTOR, D3DRS_SRGBWRITEENABLE, D3DRS_DEPTHBIAS,
    D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_SRCBLENDALPHA, D3DRS_DESTBLENDALPHA, D3DRS_BLENDOPALPHA,
};

static const D3DRENDERSTATETYPE kVertexRenderStates[] = {
    D3DRS_CULLMODE, D3DRS_FOGENABLE, D3DRS_FOGCOLOR, D3DRS_FOGTABLEMODE, D3DRS_FOGSTART, D3DRS_FOGEND,
    D3DRS_FOGDENSITY, D3DRS_RANGEFOGENABLE, D3DRS_AMBIENT, D3DRS_COLORVERTEX, D3DRS_FOGVERTEXMODE, D3DRS_CLIPPING,
    D3DRS_LIGHTING, D3DRS_NORMALIZENORMALS, D3DRS_LOCALVIEWER, D3DRS_EMISSIVEMATERIALSOURCE,
    D3DRS_AMBIENTMATERIALSOURCE, D3DRS_DIFFUSEMATERIALSOURCE, D3DRS_SPECULARMATERIALSOURCE, D3DRS_VERTEXBLEND,
    D3DRS_CLIPPLANEENABLE, D3DRS_POINTSIZE, D3DRS_POINTSIZE_MIN, D3DRS_POINTSPRITEENABLE, D3DRS_POINTSCALEENABLE,
    D3DRS_POINTSCALE_A, D3DRS_POINTSCALE_B, D3DRS_POINTSCALE_C, D3DRS_MULTISAMPLEANTIALIAS, D3DRS_MULTISAMPLEMASK,
    D3DRS_PATCHEDGESTYLE, D3DRS_POINTSIZE_MAX, D3DRS_INDEXEDVERTEXBLENDENABLE, D3DRS_TWEENFACTOR,
    D3DRS_POSITIONDEGREE, D3DRS_NORMALDEGREE, D3DRS_MINTESSELLATIONLEVEL, D3DRS_MAXTESSELLATIONLEVEL,
    D3DRS_ADAPTIVETESS_X, D3DRS_ADAPTIVETESS_Y, D3DRS_ADAPTIVETESS_Z, D3DRS_ADAPTIVETESS_W,
    D3DRS_ENABLEADAPTIVETESSELLATION, D3DRS_SHADEMODE, D3DRS_SPECULARENABLE,
};

static DWORD
SamplerStage(DWORD Index) {
  return Index < caps::MaxSamplersPS ? Index : D3DVERTEXTEXTURESAMPLER0 + (Index - caps::MaxSamplersPS);
}

MTLD3D9StateBlock::MTLD3D9StateBlock(MTLD3D9Device *pDevice) : MTLD3D9DeviceChild(pDevice), parent_(pDevice) {}

MTLD3D9StateBlock::MTLD3D9StateBlock(MTLD3D9Device *pDevice, D3DSTATEBLOCKTYPE Type) :
    MTLD3D9DeviceChild(pDevice),
    parent_(pDevice),
    typed_(true) {
  CaptureType(Type);
  Capture();
}

void
MTLD3D9StateBlock::CaptureType(D3DSTATEBLOCKTYPE Type) {
  bool all = Type == D3DSBT_ALL, pixel = all || Type == D3DSBT_PIXELSTATE, vertex = all || Type == D3DSBT_VERTEXSTATE;
  constexpr uint64_t all_stage_states = (2ull << D3DTSS_CONSTANT) - 2;
  if (all) {
    for (uint32_t i = 0; i < 256; i++)
      render_state_mask_[i] = true;
    for (auto &mask : sampler_state_mask_)
      mask = (2u << D3DSAMP_DMAPOFFSET) - 2;
    stage_state_mask_.fill(all_stage_states);
    texture_mask_ = (1u << kSamplers) - 1;
    for (uint32_t i = 0; i < transforms_.size(); i++)
      transform_mask_[i] = true;
    material_set_ = true;
    stream_mask_ = (1u << caps::MaxStreams) - 1;
    indices_set_ = true;
    viewport_set_ = scissor_set_ = true;
    clip_plane_mask_ = (1u << caps::MaxClipPlanes) - 1;
  }
  if (pixel) {
    for (auto state : kPixelRenderStates)
      render_state_mask_[state] = true;
    for (auto &mask : sampler_state_mask_)
      mask |= ((2u << D3DSAMP_DMAPOFFSET) - 2) & ~(1u << D3DSAMP_DMAPOFFSET);
    for (auto &mask : stage_state_mask_)
      mask |= all_stage_states & ~((1ull << D3DTSS_TEXCOORDINDEX) | (1ull << D3DTSS_TEXTURETRANSFORMFLAGS));
    pixel_shader_set_ = true;
    const_float_mask_[1].set();
    const_int_mask_[1] = const_bool_mask_[1] = (1u << caps::MaxOtherConstants) - 1;
  }
  if (vertex) {
    for (auto state : kVertexRenderStates)
      render_state_mask_[state] = true;
    for (auto &mask : sampler_state_mask_)
      mask |= 1u << D3DSAMP_DMAPOFFSET;
    for (auto &mask : stage_state_mask_)
      mask |= (1ull << D3DTSS_TEXCOORDINDEX) | (1ull << D3DTSS_TEXTURETRANSFORMFLAGS);
    vertex_shader_set_ = declaration_set_ = true;
    stream_freq_mask_ = (1u << caps::MaxStreams) - 1;
    const_float_mask_[0].set();
    const_int_mask_[0] = const_bool_mask_[0] = (1u << caps::MaxOtherConstants) - 1;
    for (DWORD i = 0; i < parent_->LightCount(); i++) {
      lights_[i] = {};
      light_enables_[i] = FALSE;
    }
  }
}

HRESULT STDMETHODCALLTYPE
MTLD3D9StateBlock::QueryInterface(REFIID riid, void **ppvObject) {
  if (!ppvObject)
    return E_POINTER;
  *ppvObject = nullptr;
  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DStateBlock9)) {
    *ppvObject = ref(this);
    return S_OK;
  }
  WARN("IDirect3DStateBlock9::QueryInterface: Unknown interface query ", str::format(riid));
  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9StateBlock::GetDevice(IDirect3DDevice9 **ppDevice) {
  return GetDeviceInternal(ppDevice);
}

template <typename T, typename Getter>
static void
CaptureObject(Com<T> &Out, Getter &&Get) {
  T *object = nullptr;
  Get(&object);
  Out = object;
  if (object)
    object->Release();
}

HRESULT STDMETHODCALLTYPE
MTLD3D9StateBlock::Capture() {
  auto lock = parent_->Lock();
  if (parent_->IsRecordingStateBlock())
    return D3DERR_INVALIDCALL;
  auto *d = parent_;
  for (uint32_t i = 0; i < 256; i++)
    if (render_state_mask_[i])
      d->GetRenderState((D3DRENDERSTATETYPE)i, &render_states_[i]);
  for (uint32_t s = 0; s < kSamplers; s++) {
    for (uint32_t t = D3DSAMP_ADDRESSU; t <= D3DSAMP_DMAPOFFSET; t++)
      if (sampler_state_mask_[s] & (1u << t))
        d->GetSamplerState(SamplerStage(s), (D3DSAMPLERSTATETYPE)t, &sampler_states_[s][t]);
    if (texture_mask_ & (1u << s))
      CaptureObject(textures_[s], [&](IDirect3DBaseTexture9 **p) { d->GetTexture(SamplerStage(s), p); });
  }
  for (uint32_t s = 0; s < 8; s++)
    for (uint32_t t = 0; t <= D3DTSS_CONSTANT; t++)
      if (stage_state_mask_[s] & (1ull << t))
        d->GetTextureStageState(s, (D3DTEXTURESTAGESTATETYPE)t, &stage_states_[s][t]);
  for (uint32_t i = 0; i < transforms_.size(); i++)
    if (transform_mask_[i])
      d->GetTransform((D3DTRANSFORMSTATETYPE)i, &transforms_[i]);
  if (material_set_)
    d->GetMaterial(&material_);
  for (auto &[index, light] : lights_)
    d->GetLight(index, &light);
  for (auto &[index, enable] : light_enables_)
    d->GetLightEnable(index, &enable);
  if (vertex_shader_set_)
    CaptureObject(vertex_shader_, [&](IDirect3DVertexShader9 **p) { d->GetVertexShader(p); });
  if (pixel_shader_set_)
    CaptureObject(pixel_shader_, [&](IDirect3DPixelShader9 **p) { d->GetPixelShader(p); });
  if (declaration_set_) {
    CaptureObject(declaration_, [&](IDirect3DVertexDeclaration9 **p) { d->GetVertexDeclaration(p); });
    d->GetFVF(&fvf_);
  }
  for (uint32_t i = 0; i < caps::MaxStreams; i++) {
    if (stream_mask_ & (1u << i)) {
      // as in native d3d9 (Wine's stateblock tests check it), Capture on a CreateStateBlock block keeps the
      // stream offset it was created with
      UINT offset = streams_[i].offset;
      CaptureObject(streams_[i].buffer, [&](IDirect3DVertexBuffer9 **p) {
        d->GetStreamSource(i, p, &streams_[i].offset, &streams_[i].stride);
      });
      if (typed_ && captured_)
        streams_[i].offset = offset;
    }
    if (stream_freq_mask_ & (1u << i))
      d->GetStreamSourceFreq(i, &stream_freqs_[i]);
  }
  if (indices_set_)
    CaptureObject(indices_, [&](IDirect3DIndexBuffer9 **p) { d->GetIndices(p); });
  for (uint32_t i = 0; i < 256; i++) {
    if (const_float_mask_[0][i])
      d->GetVertexShaderConstantF(i, &const_float_[0][i * 4], 1);
    if (i < caps::MaxFloatConstantsPS && const_float_mask_[1][i])
      d->GetPixelShaderConstantF(i, &const_float_[1][i * 4], 1);
  }
  for (uint32_t i = 0; i < caps::MaxOtherConstants; i++) {
    if (const_int_mask_[0] & (1u << i))
      d->GetVertexShaderConstantI(i, &const_int_[0][i * 4], 1);
    if (const_int_mask_[1] & (1u << i))
      d->GetPixelShaderConstantI(i, &const_int_[1][i * 4], 1);
    if (const_bool_mask_[0] & (1u << i))
      d->GetVertexShaderConstantB(i, &const_bool_[0][i], 1);
    if (const_bool_mask_[1] & (1u << i))
      d->GetPixelShaderConstantB(i, &const_bool_[1][i], 1);
  }
  if (viewport_set_)
    d->GetViewport(&viewport_);
  if (scissor_set_)
    d->GetScissorRect(&scissor_);
  for (uint32_t i = 0; i < caps::MaxClipPlanes; i++)
    if (clip_plane_mask_ & (1u << i))
      d->GetClipPlane(i, clip_planes_[i].data());
  captured_ = true;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9StateBlock::Apply() {
  auto lock = parent_->Lock();
  if (parent_->IsRecordingStateBlock())
    return D3DERR_INVALIDCALL;
  auto *d = parent_;
  for (uint32_t i = 0; i < 256; i++)
    if (render_state_mask_[i])
      d->SetRenderState((D3DRENDERSTATETYPE)i, render_states_[i]);
  for (uint32_t s = 0; s < kSamplers; s++) {
    for (uint32_t t = D3DSAMP_ADDRESSU; t <= D3DSAMP_DMAPOFFSET; t++)
      if (sampler_state_mask_[s] & (1u << t))
        d->SetSamplerState(SamplerStage(s), (D3DSAMPLERSTATETYPE)t, sampler_states_[s][t]);
    if (texture_mask_ & (1u << s))
      d->SetTexture(SamplerStage(s), textures_[s].ptr());
  }
  for (uint32_t s = 0; s < 8; s++)
    for (uint32_t t = 0; t <= D3DTSS_CONSTANT; t++)
      if (stage_state_mask_[s] & (1ull << t))
        d->SetTextureStageState(s, (D3DTEXTURESTAGESTATETYPE)t, stage_states_[s][t]);
  for (uint32_t i = 0; i < transforms_.size(); i++)
    if (transform_mask_[i])
      d->SetTransform((D3DTRANSFORMSTATETYPE)i, &transforms_[i]);
  if (material_set_)
    d->SetMaterial(&material_);
  for (auto &[index, light] : lights_)
    d->SetLight(index, &light);
  for (auto &[index, enable] : light_enables_)
    d->LightEnable(index, enable);
  if (vertex_shader_set_)
    d->SetVertexShader(vertex_shader_.ptr());
  if (pixel_shader_set_)
    d->SetPixelShader(pixel_shader_.ptr());
  // a captured NULL declaration leaves the device's alone (Wine's test_vdecl_apply)
  if (declaration_set_) {
    if (fvf_)
      d->SetFVF(fvf_);
    else if (declaration_)
      d->SetVertexDeclaration(declaration_.ptr());
  }
  for (uint32_t i = 0; i < caps::MaxStreams; i++) {
    if (stream_mask_ & (1u << i))
      d->SetStreamSource(i, streams_[i].buffer.ptr(), streams_[i].offset, streams_[i].stride);
    if (stream_freq_mask_ & (1u << i))
      d->SetStreamSourceFreq(i, stream_freqs_[i]);
  }
  if (indices_set_)
    d->SetIndices(indices_.ptr());
  for (uint32_t i = 0; i < 256; i++) {
    if (const_float_mask_[0][i])
      d->SetVertexShaderConstantF(i, &const_float_[0][i * 4], 1);
    if (i < caps::MaxFloatConstantsPS && const_float_mask_[1][i])
      d->SetPixelShaderConstantF(i, &const_float_[1][i * 4], 1);
  }
  for (uint32_t i = 0; i < caps::MaxOtherConstants; i++) {
    if (const_int_mask_[0] & (1u << i))
      d->SetVertexShaderConstantI(i, &const_int_[0][i * 4], 1);
    if (const_int_mask_[1] & (1u << i))
      d->SetPixelShaderConstantI(i, &const_int_[1][i * 4], 1);
    if (const_bool_mask_[0] & (1u << i))
      d->SetVertexShaderConstantB(i, &const_bool_[0][i], 1);
    if (const_bool_mask_[1] & (1u << i))
      d->SetPixelShaderConstantB(i, &const_bool_[1][i], 1);
  }
  if (viewport_set_)
    d->SetViewport(&viewport_);
  if (scissor_set_)
    d->SetScissorRect(&scissor_);
  for (uint32_t i = 0; i < caps::MaxClipPlanes; i++)
    if (clip_plane_mask_ & (1u << i))
      d->SetClipPlane(i, clip_planes_[i].data());
  return D3D_OK;
}

void
MTLD3D9StateBlock::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) {
  render_states_[State] = Value;
  render_state_mask_[State] = true;
}

void
MTLD3D9StateBlock::SetSamplerState(DWORD Index, D3DSAMPLERSTATETYPE Type, DWORD Value) {
  sampler_states_[Index][Type] = Value;
  sampler_state_mask_[Index] |= 1u << Type;
}

void
MTLD3D9StateBlock::SetTexture(DWORD Index, IDirect3DBaseTexture9 *pTexture) {
  textures_[Index] = pTexture;
  texture_mask_ |= 1u << Index;
}

void
MTLD3D9StateBlock::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) {
  stage_states_[Stage][Type] = Value;
  stage_state_mask_[Stage] |= 1ull << Type;
}

void
MTLD3D9StateBlock::SetTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX &Matrix) {
  transforms_[State] = Matrix;
  transform_mask_[State] = true;
}

void
MTLD3D9StateBlock::SetMaterial(const D3DMATERIAL9 &Material) {
  material_ = Material;
  material_set_ = true;
}

void
MTLD3D9StateBlock::SetLight(DWORD Index, const D3DLIGHT9 &Light) {
  lights_[Index] = Light;
}

void
MTLD3D9StateBlock::LightEnable(DWORD Index, BOOL Enable) {
  light_enables_[Index] = Enable;
}

void
MTLD3D9StateBlock::SetVertexShader(IDirect3DVertexShader9 *pShader) {
  vertex_shader_ = pShader;
  vertex_shader_set_ = true;
}

void
MTLD3D9StateBlock::SetPixelShader(IDirect3DPixelShader9 *pShader) {
  pixel_shader_ = pShader;
  pixel_shader_set_ = true;
}

void
MTLD3D9StateBlock::SetVertexDeclaration(IDirect3DVertexDeclaration9 *pDecl) {
  declaration_ = pDecl;
  fvf_ = 0;
  declaration_set_ = true;
}

void
MTLD3D9StateBlock::SetFVF(DWORD FVF) {
  declaration_ = nullptr;
  fvf_ = FVF;
  declaration_set_ = true;
}

void
MTLD3D9StateBlock::SetStreamSource(UINT Stream, IDirect3DVertexBuffer9 *pBuffer, UINT Offset, UINT Stride) {
  streams_[Stream] = {pBuffer, Offset, Stride};
  stream_mask_ |= 1u << Stream;
}

void
MTLD3D9StateBlock::SetStreamSourceFreq(UINT Stream, UINT Setting) {
  stream_freqs_[Stream] = Setting;
  stream_freq_mask_ |= 1u << Stream;
}

void
MTLD3D9StateBlock::SetIndices(IDirect3DIndexBuffer9 *pIndices) {
  indices_ = pIndices;
  indices_set_ = true;
}

void
MTLD3D9StateBlock::SetConstantF(bool Pixel, UINT Start, const float *pData, UINT Count) {
  std::memcpy(&const_float_[Pixel][Start * 4], pData, Count * 4 * sizeof(float));
  for (UINT i = 0; i < Count; i++)
    const_float_mask_[Pixel][Start + i] = true;
}

void
MTLD3D9StateBlock::SetConstantI(bool Pixel, UINT Start, const int *pData, UINT Count) {
  std::memcpy(&const_int_[Pixel][Start * 4], pData, Count * 4 * sizeof(int));
  for (UINT i = 0; i < Count; i++)
    const_int_mask_[Pixel] |= 1u << (Start + i);
}

void
MTLD3D9StateBlock::SetConstantB(bool Pixel, UINT Start, const BOOL *pData, UINT Count) {
  for (UINT i = 0; i < Count; i++) {
    const_bool_[Pixel][Start + i] = pData[i];
    const_bool_mask_[Pixel] |= 1u << (Start + i);
  }
}

void
MTLD3D9StateBlock::SetViewport(const D3DVIEWPORT9 &Viewport) {
  viewport_ = Viewport;
  viewport_set_ = true;
}

void
MTLD3D9StateBlock::SetScissorRect(const RECT &Rect) {
  scissor_ = Rect;
  scissor_set_ = true;
}

void
MTLD3D9StateBlock::SetClipPlane(DWORD Index, const float *pPlane) {
  std::memcpy(clip_planes_[Index].data(), pPlane, sizeof(float) * 4);
  clip_plane_mask_ |= 1u << Index;
}

} // namespace mullion
