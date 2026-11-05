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

#include "d3d9_device.hpp"
#include "d3d9_fixed_function.hpp"
#include "d3d9_state_block.hpp"
#include "log/log.hpp"
#include <bit>
#include <cmath>

namespace mullion {

static D3DMATRIX
Multiply(const D3DMATRIX &A, const D3DMATRIX &B) {
  D3DMATRIX r;
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++) {
      float sum = 0;
      for (int k = 0; k < 4; k++)
        sum += A.m[i][k] * B.m[k][j];
      r.m[i][j] = sum;
    }
  return r;
}

static void
StoreColumns(float *Out, const D3DMATRIX &M, int Count) {
  for (int j = 0; j < Count; j++)
    for (int i = 0; i < 4; i++)
      Out[j * 4 + i] = M.m[i][j];
}

// the upper 3x3 of M inverted and transposed, so normals keep their length scaling (D3DRS_NORMALIZENORMALS off)
static D3DMATRIX
NormalMatrix(const D3DMATRIX &M) {
  const auto &m = M.m;
  float c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1], c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2],
        c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
  float det = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
  if (det == 0.0f)
    return M;
  float s = 1.0f / det;
  D3DMATRIX r = {};
  r.m[0][0] = c00 * s;
  r.m[0][1] = c01 * s;
  r.m[0][2] = c02 * s;
  r.m[1][0] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * s;
  r.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * s;
  r.m[1][2] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * s;
  r.m[2][0] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * s;
  r.m[2][1] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * s;
  r.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * s;
  return r;
}

static void
StoreColor(float *Out, const D3DCOLORVALUE &C) {
  Out[0] = C.r;
  Out[1] = C.g;
  Out[2] = C.b;
  Out[3] = C.a;
}

static void
StoreColor(float *Out, D3DCOLOR C) {
  Out[0] = ((C >> 16) & 0xFF) / 255.0f;
  Out[1] = ((C >> 8) & 0xFF) / 255.0f;
  Out[2] = (C & 0xFF) / 255.0f;
  Out[3] = ((C >> 24) & 0xFF) / 255.0f;
}

MTLD3D9ShaderCode *
MTLD3D9Device::FixedFunctionShader(std::vector<DWORD> &&Bytecode, bool Pixel) {
  auto key = Sha1HashState().update(Bytecode.data(), Bytecode.size() * sizeof(DWORD)).update(Pixel).final();
  if (auto it = ff_shaders_.find(key); it != ff_shaders_.end())
    return it->second.get();
  std::unique_ptr<MTLD3D9ShaderCode> code;
  if (FAILED(MTLD3D9ShaderCode::Create(Bytecode.data(), Pixel, code)))
    return nullptr;
  return ff_shaders_.emplace(key, std::move(code)).first->second.get();
}

static uint8_t
DeclTypeSize(BYTE Type) {
  switch (Type) {
  case D3DDECLTYPE_FLOAT1:
    return 1;
  case D3DDECLTYPE_FLOAT2:
  case D3DDECLTYPE_SHORT2:
  case D3DDECLTYPE_SHORT2N:
  case D3DDECLTYPE_USHORT2N:
  case D3DDECLTYPE_FLOAT16_2:
    return 2;
  case D3DDECLTYPE_FLOAT3:
  case D3DDECLTYPE_UDEC3:
  case D3DDECLTYPE_DEC3N:
    return 3;
  default:
    return 4;
  }
}

static void
Transform(float *Out, const D3DVECTOR &V, const D3DMATRIX &M, float W) {
  for (int j = 0; j < 3; j++)
    Out[j] = V.x * M.m[0][j] + V.y * M.m[1][j] + V.z * M.m[2][j] + W * M.m[3][j];
}

static void
Normalize(float *V) {
  float length = std::sqrt(V[0] * V[0] + V[1] * V[1] + V[2] * V[2]);
  if (length > 0)
    for (int i = 0; i < 3; i++)
      V[i] /= length;
}

MTLD3D9ShaderCode *
MTLD3D9Device::FixedFunctionVS(float *C) {
  D3D9FFVertexState s = {};
  bool has_indices = false;
  for (auto &e : vertex_declaration_->elements()) {
    switch (e.Usage) {
    case D3DDECLUSAGE_POSITIONT:
      s.pretransformed = true;
      break;
    case D3DDECLUSAGE_NORMAL:
      s.normal = s.normal || e.UsageIndex == 0;
      break;
    case D3DDECLUSAGE_COLOR:
      if (e.UsageIndex == 0)
        s.diffuse = true;
      else if (e.UsageIndex == 1)
        s.specular = true;
      break;
    case D3DDECLUSAGE_BLENDWEIGHT:
      if (e.UsageIndex == 0)
        s.blend_weights = DeclTypeSize(e.Type);
      break;
    case D3DDECLUSAGE_PSIZE:
      s.point_size = e.UsageIndex == 0;
      break;
    case D3DDECLUSAGE_BLENDINDICES:
      if (e.UsageIndex == 0) {
        has_indices = true;
        s.indices_d3dcolor = e.Type == D3DDECLTYPE_D3DCOLOR;
      }
      break;
    case D3DDECLUSAGE_TEXCOORD:
      if (e.UsageIndex < 8) {
        s.texcoord_mask |= 1 << e.UsageIndex;
        s.texcoord_size[e.UsageIndex] = DeclTypeSize(e.Type);
      }
      break;
    }
  }
  DWORD blend = render_states_[D3DRS_VERTEXBLEND];
  s.tween = blend == D3DVBF_TWEENING;
  s.blend_weights = blend >= D3DVBF_1WEIGHTS && blend <= D3DVBF_3WEIGHTS ? std::min<uint32_t>(blend, s.blend_weights) : 0;
  s.indexed = render_states_[D3DRS_INDEXEDVERTEXBLENDENABLE] && has_indices &&
              (s.blend_weights || blend == D3DVBF_0WEIGHTS);
  s.point_scale = render_states_[D3DRS_POINTSCALEENABLE];
  s.lighting = render_states_[D3DRS_LIGHTING];
  s.specular_lighting = render_states_[D3DRS_SPECULARENABLE];
  s.local_viewer = render_states_[D3DRS_LOCALVIEWER];
  s.normalize_normals = render_states_[D3DRS_NORMALIZENORMALS];
  auto source = [&](D3DRENDERSTATETYPE State) {
    auto value = (D3DMATERIALCOLORSOURCE)render_states_[State];
    if (!render_states_[D3DRS_COLORVERTEX] || (value == D3DMCS_COLOR1 && !s.diffuse) ||
        (value == D3DMCS_COLOR2 && !s.specular))
      return D3DMCS_MATERIAL;
    return value;
  };
  s.diffuse_source = source(D3DRS_DIFFUSEMATERIALSOURCE);
  s.ambient_source = source(D3DRS_AMBIENTMATERIALSOURCE);
  s.specular_source = source(D3DRS_SPECULARMATERIALSOURCE);
  s.emissive_source = source(D3DRS_EMISSIVEMATERIALSOURCE);

  auto &world = transforms_[D3DTS_WORLDMATRIX(0)];
  auto &view = transforms_[D3DTS_VIEW];
  auto world_view = Multiply(world, view);
  StoreColumns(C + 4 * ff::VSWorldViewProj, Multiply(world_view, transforms_[D3DTS_PROJECTION]), 4);
  StoreColumns(C + 4 * ff::VSWorldView, world_view, 4);
  StoreColumns(C + 4 * ff::VSNormalMatrix, NormalMatrix(world_view), 3);
  if (s.indexed)
    for (uint32_t i = 0; i < ff::MaxIndexedMatrices; i++)
      StoreColumns(C + 4 * (ff::VSIndexedPalette + 3 * i), Multiply(transforms_[D3DTS_WORLDMATRIX(i)], view), 3);
  for (uint32_t i = 1; i <= s.blend_weights; i++)
    StoreColumns(C + 4 * (ff::VSWorldViewBlend + 4 * (i - 1)), Multiply(transforms_[D3DTS_WORLDMATRIX(i)], view), 4);
  if (s.blend_weights || s.indexed)
    StoreColumns(C + 4 * ff::VSProjection, transforms_[D3DTS_PROJECTION], 4);

  auto [min_z, max_z] = EffectiveDepthRange();
  float depth_scale = 1.0f / (max_z - min_z);
  float *scale = C + 4 * ff::VSViewportScale, *offset = C + 4 * ff::VSViewportOffset;
  scale[0] = 2.0f / viewport_.Width;
  scale[1] = -2.0f / viewport_.Height;
  scale[2] = depth_scale;
  offset[0] = -1.0f - 2.0f * viewport_.X / viewport_.Width;
  offset[1] = 1.0f + 2.0f * viewport_.Y / viewport_.Height;
  offset[2] = -min_z * depth_scale;
  scale[3] = viewport_.Height;
  float *point = C + 4 * ff::VSPointScale;
  point[0] = std::bit_cast<float>(render_states_[D3DRS_POINTSCALE_A]);
  point[1] = std::bit_cast<float>(render_states_[D3DRS_POINTSCALE_B]);
  point[2] = std::bit_cast<float>(render_states_[D3DRS_POINTSCALE_C]);
  point[3] = std::bit_cast<float>(render_states_[D3DRS_POINTSIZE]) * (s.point_scale ? viewport_.Height : 1.0f);

  StoreColor(C + 4 * ff::VSMaterialDiffuse, material_.Diffuse);
  StoreColor(C + 4 * ff::VSMaterialAmbient, material_.Ambient);
  StoreColor(C + 4 * ff::VSMaterialSpecular, material_.Specular);
  StoreColor(C + 4 * ff::VSMaterialEmissive, material_.Emissive);
  StoreColor(C + 4 * ff::VSAmbient, (D3DCOLOR)render_states_[D3DRS_AMBIENT]);
  C[4 * ff::VSMaterialPower] = material_.Power;
  C[4 * ff::VSMaterialPower + 1] = std::bit_cast<float>(render_states_[D3DRS_TWEENFACTOR]);

  for (auto &[light, enabled] : lights_) {
    if (!enabled || s.light_count == ff::MaxLights)
      continue;
    float *l = C + 4 * (ff::VSLightBase + ff::VSLightStride * s.light_count);
    if (light.Type == D3DLIGHT_DIRECTIONAL) {
      Transform(l, light.Direction, view, 0);
      Normalize(l);
      for (int i = 0; i < 3; i++)
        l[i] = -l[i];
    } else {
      Transform(l, light.Position, view, 1);
    }
    Transform(l + 4, light.Direction, view, 0);
    Normalize(l + 4);
    StoreColor(l + 8, light.Diffuse);
    StoreColor(l + 12, light.Specular);
    StoreColor(l + 16, light.Ambient);
    l[20] = light.Attenuation0;
    l[21] = light.Attenuation1;
    l[22] = light.Attenuation2;
    l[23] = light.Range;
    float inner = std::cos(light.Theta / 2), outer = std::cos(light.Phi / 2);
    l[24] = inner;
    l[25] = outer;
    l[26] = inner > outer ? 1.0f / (inner - outer) : 1e6f;
    l[27] = light.Falloff;
    s.light_types[s.light_count++] = light.Type;
  }

  for (uint32_t i = 0; i < ff::MaxStages; i++) {
    auto &tss = texture_stage_states_[i];
    auto &tc = s.texcoords[i];
    DWORD index = tss[D3DTSS_TEXCOORDINDEX];
    switch (index & 0xFFFF0000) {
    case D3DTSS_TCI_PASSTHRU:
      tc.source = s.texcoord_mask & (1 << (index & 7)) ? index & 7 : ff::TexNone;
      break;
    case D3DTSS_TCI_CAMERASPACENORMAL:
      tc.source = ff::TexGenNormal;
      break;
    case D3DTSS_TCI_CAMERASPACEPOSITION:
      tc.source = ff::TexGenPosition;
      break;
    case D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR:
      tc.source = ff::TexGenReflection;
      break;
    case D3DTSS_TCI_SPHEREMAP:
      tc.source = ff::TexGenSphereMap;
      break;
    default:
      tc.source = ff::TexNone;
      break;
    }
    if (s.pretransformed && tc.source >= 8)
      tc.source = ff::TexNone;
    DWORD flags = tss[D3DTSS_TEXTURETRANSFORMFLAGS];
    // only COUNT2 to COUNT4 transform; COUNT1 and out-of-range counts pass the coordinate through
    DWORD count = flags & ~D3DTTFF_PROJECTED;
    tc.count = s.pretransformed || count < 2 || count > 4 ? 0 : count;
    tc.projected = flags & D3DTTFF_PROJECTED;
    if (tc.count)
      StoreColumns(C + 4 * (ff::VSTextureMatrixBase + 4 * i), transforms_[D3DTS_TEXTURE0 + i], 4);
  }

  s.fog_mode = ff::FogNone;
  if (render_states_[D3DRS_FOGENABLE] && render_states_[D3DRS_FOGTABLEMODE] == D3DFOG_NONE) {
    s.fog_mode = render_states_[D3DRS_FOGVERTEXMODE];
    s.range_fog = render_states_[D3DRS_RANGEFOGENABLE];
    float start = std::bit_cast<float>(render_states_[D3DRS_FOGSTART]);
    float end = std::bit_cast<float>(render_states_[D3DRS_FOGEND]);
    float *fog = C + 4 * ff::VSFog;
    fog[0] = start;
    fog[1] = end;
    fog[2] = std::bit_cast<float>(render_states_[D3DRS_FOGDENSITY]);
    fog[3] = end != start ? 1.0f / (end - start) : 0.0f;
  }
  return FixedFunctionShader(D3D9GenerateFixedFunctionVS(s), false);
}

MTLD3D9ShaderCode *
MTLD3D9Device::FixedFunctionPS(float *C) {
  D3D9FFPixelState s = {};
  for (uint32_t i = 0; i < ff::MaxStages; i++) {
    auto &tss = texture_stage_states_[i];
    auto &stage = s.stages[i];
    stage.color_op = tss[D3DTSS_COLOROP];
    stage.color_arg[0] = tss[D3DTSS_COLORARG0];
    stage.color_arg[1] = tss[D3DTSS_COLORARG1];
    stage.color_arg[2] = tss[D3DTSS_COLORARG2];
    stage.alpha_op = tss[D3DTSS_ALPHAOP];
    stage.alpha_arg[0] = tss[D3DTSS_ALPHAARG0];
    stage.alpha_arg[1] = tss[D3DTSS_ALPHAARG1];
    stage.alpha_arg[2] = tss[D3DTSS_ALPHAARG2];
    stage.result_arg = tss[D3DTSS_RESULTARG];
    stage.projected = tss[D3DTSS_TEXTURETRANSFORMFLAGS] & D3DTTFF_PROJECTED;
    if (auto texture = textures_[i].texture)
      stage.texture_type = texture->GetType();
    StoreColor(C + 4 * (ff::PSStageConstantBase + i), (D3DCOLOR)tss[D3DTSS_CONSTANT]);
    for (uint32_t k = 0; k < 4; k++)
      C[4 * (ff::PSBumpMatrixBase + i) + k] = std::bit_cast<float>(tss[D3DTSS_BUMPENVMAT00 + k]);
    C[4 * (ff::PSBumpLuminanceBase + i)] = std::bit_cast<float>(tss[D3DTSS_BUMPENVLSCALE]);
    C[4 * (ff::PSBumpLuminanceBase + i) + 1] = std::bit_cast<float>(tss[D3DTSS_BUMPENVLOFFSET]);
  }
  s.specular = render_states_[D3DRS_SPECULARENABLE];
  StoreColor(C + 4 * ff::PSTextureFactor, (D3DCOLOR)render_states_[D3DRS_TEXTUREFACTOR]);
  return FixedFunctionShader(D3D9GenerateFixedFunctionPS(s), true);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix) {
  auto lock = Lock();
  if (!pMatrix || (UINT)State >= transforms_.size())
    return D3DERR_INVALIDCALL;
  if (recording_) {
    recording_->SetTransform(State, *pMatrix);
    return D3D_OK;
  }
  transforms_[State] = *pMatrix;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix) {
  auto lock = Lock();
  if (!pMatrix || (UINT)State >= transforms_.size())
    return D3DERR_INVALIDCALL;
  *pMatrix = transforms_[State];
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::MultiplyTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix) {
  auto lock = Lock();
  if (!pMatrix || (UINT)State >= transforms_.size())
    return D3DERR_INVALIDCALL;
  // applies to the device even while a state block records (Wine's test_multiply_transform)
  transforms_[State] = Multiply(*pMatrix, transforms_[State]);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) {
  auto lock = Lock();
  if (Stage >= texture_stage_states_.size() || Type > D3DTSS_CONSTANT)
    return D3DERR_INVALIDCALL;
  if (recording_) {
    recording_->SetTextureStageState(Stage, Type, Value);
    return D3D_OK;
  }
  texture_stage_states_[Stage][Type] = Value;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue) {
  auto lock = Lock();
  if (!pValue || Stage >= texture_stage_states_.size() || Type > D3DTSS_CONSTANT)
    return D3DERR_INVALIDCALL;
  *pValue = texture_stage_states_[Stage][Type];
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetMaterial(const D3DMATERIAL9 *pMaterial) {
  auto lock = Lock();
  if (!pMaterial)
    return D3DERR_INVALIDCALL;
  if (recording_) {
    recording_->SetMaterial(*pMaterial);
    return D3D_OK;
  }
  material_ = *pMaterial;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetMaterial(D3DMATERIAL9 *pMaterial) {
  auto lock = Lock();
  if (!pMaterial)
    return D3DERR_INVALIDCALL;
  *pMaterial = material_;
  return D3D_OK;
}

static constexpr DWORD kMaxLightIndex = 4096;

static D3DLIGHT9
DefaultLight() {
  D3DLIGHT9 light = {};
  light.Type = D3DLIGHT_DIRECTIONAL;
  light.Diffuse = {1.0f, 1.0f, 1.0f, 0.0f};
  light.Direction = {0.0f, 0.0f, 1.0f};
  return light;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetLight(DWORD Index, const D3DLIGHT9 *pLight) {
  auto lock = Lock();
  if (!pLight || Index >= kMaxLightIndex)
    return D3DERR_INVALIDCALL;
  if (recording_) {
    recording_->SetLight(Index, *pLight);
    return D3D_OK;
  }
  if (Index >= lights_.size())
    lights_.resize(Index + 1, {DefaultLight(), false});
  lights_[Index].first = *pLight;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetLight(DWORD Index, D3DLIGHT9 *pLight) {
  auto lock = Lock();
  if (!pLight || Index >= lights_.size())
    return D3DERR_INVALIDCALL;
  *pLight = lights_[Index].first;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::LightEnable(DWORD Index, BOOL Enable) {
  auto lock = Lock();
  if (Index >= kMaxLightIndex)
    return D3DERR_INVALIDCALL;
  if (recording_) {
    recording_->LightEnable(Index, Enable);
    return D3D_OK;
  }
  if (Index >= lights_.size())
    lights_.resize(Index + 1, {DefaultLight(), false});
  lights_[Index].second = Enable;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetLightEnable(DWORD Index, BOOL *pEnable) {
  auto lock = Lock();
  if (!pEnable || Index >= lights_.size())
    return D3DERR_INVALIDCALL;
  // native d3d9 reports enabled lights as 128
  *pEnable = lights_[Index].second ? 128 : 0;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9 **ppSB) {
  auto lock = Lock();
  InitReturnPtr(ppSB);
  if (!ppSB || (Type != D3DSBT_ALL && Type != D3DSBT_PIXELSTATE && Type != D3DSBT_VERTEXSTATE) || recording_)
    return D3DERR_INVALIDCALL;
  *ppSB = ref(new MTLD3D9StateBlock(this, Type));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::BeginStateBlock() {
  auto lock = Lock();
  if (recording_)
    return D3DERR_INVALIDCALL;
  recording_ = new MTLD3D9StateBlock(this);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::EndStateBlock(IDirect3DStateBlock9 **ppSB) {
  auto lock = Lock();
  // a failing call leaves *ppSB untouched
  if (!ppSB || !recording_)
    return D3DERR_INVALIDCALL;
  *ppSB = ref(recording_.ptr());
  recording_ = nullptr;
  return D3D_OK;
}

} // namespace mullion
