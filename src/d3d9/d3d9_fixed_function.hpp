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

#pragma once

#include "d3d9.h"
#include <cstdint>
#include <vector>

namespace mullion {

// fixed-function state is compiled by generating vs_3_0/ps_3_0 bytecode that reads these constant registers.
namespace ff {

constexpr uint32_t VSWorldViewProj = 0;
constexpr uint32_t VSWorldView = 4;
constexpr uint32_t VSViewportScale = 8;
constexpr uint32_t VSViewportOffset = 9;
constexpr uint32_t VSMaterialDiffuse = 10;
constexpr uint32_t VSMaterialAmbient = 11;
constexpr uint32_t VSMaterialSpecular = 12;
constexpr uint32_t VSMaterialEmissive = 13;
constexpr uint32_t VSAmbient = 14;
// c15 and c19 are defined by the shader
constexpr uint32_t VSMaterialPower = 16; // power, tween factor
constexpr uint32_t VSFog = 17; // start, end, density, 1 / (end - start)
// per light: position or negated direction, spot direction, diffuse, specular, ambient,
// attenuation (a0, a1, a2, range), spot (cos theta/2, cos phi/2, 1 / difference, falloff)
constexpr uint32_t VSLightBase = 20;
constexpr uint32_t VSLightStride = 7;
constexpr uint32_t VSTextureMatrixBase = VSLightBase + 8 * VSLightStride;
// world-view matrices 1-3 for vertex blending (0 is VSWorldView), then the projection alone
constexpr uint32_t VSWorldViewBlend = VSTextureMatrixBase + 8 * 4;
constexpr uint32_t VSProjection = VSWorldViewBlend + 3 * 4;
// indexed blending reads WORLDMATRIX(0-8) * view through a0, three columns each
constexpr uint32_t VSIndexedPalette = VSProjection + 4;
constexpr uint32_t MaxIndexedMatrices = 9;
// D3DRS_POINTSCALE_A, B, C, D3DRS_POINTSIZE * viewport height
constexpr uint32_t VSPointScale = VSIndexedPalette + 3 * MaxIndexedMatrices;
// inverse transpose of the world-view matrix for normals, three columns
constexpr uint32_t VSNormalMatrix = VSPointScale + 1;
constexpr uint32_t VSConstantCount = VSNormalMatrix + 3;

constexpr uint32_t PSTextureFactor = 0;
constexpr uint32_t PSStageConstantBase = 1;
// c15 is defined by the shader
constexpr uint32_t PSBumpMatrixBase = 16;    // D3DTSS_BUMPENVMAT00, 01, 10, 11
constexpr uint32_t PSBumpLuminanceBase = 24; // D3DTSS_BUMPENVLSCALE, D3DTSS_BUMPENVLOFFSET
constexpr uint32_t PSConstantCount = 32;

constexpr uint32_t MaxStages = 8;
constexpr uint32_t MaxLights = 8;

// D3D9FFTexCoord::source values past the 8 input sets
constexpr uint8_t TexGenNormal = 8;
constexpr uint8_t TexGenPosition = 9;
constexpr uint8_t TexGenReflection = 10;
constexpr uint8_t TexGenSphereMap = 11;
constexpr uint8_t TexNone = 0xFF;

constexpr DWORD FogNone = 0xFF;

} // namespace ff

struct D3D9FFTexCoord {
  uint8_t source;
  uint8_t count; // D3DTTFF_COUNTn of the texture transform, 0 when disabled
  bool projected;
};

struct D3D9FFVertexState {
  bool pretransformed;
  bool normal;
  bool diffuse;
  bool specular;
  uint32_t texcoord_mask;
  uint8_t texcoord_size[8];
  bool lighting;
  bool specular_lighting;
  bool local_viewer;
  bool normalize_normals;
  uint32_t light_count;
  D3DLIGHTTYPE light_types[ff::MaxLights];
  D3DMATERIALCOLORSOURCE diffuse_source;
  D3DMATERIALCOLORSOURCE ambient_source;
  D3DMATERIALCOLORSOURCE specular_source;
  D3DMATERIALCOLORSOURCE emissive_source;
  D3D9FFTexCoord texcoords[ff::MaxStages];
  DWORD fog_mode; // D3DFOGMODE computed per vertex, D3DFOG_NONE for specular alpha, ff::FogNone without vertex fog
  bool range_fog;
  uint32_t blend_weights; // D3DRS_VERTEXBLEND weights read from BLENDWEIGHT; blends weights + 1 matrices
  bool tween;             // D3DVBF_TWEENING between POSITION0/1 and NORMAL0/1
  bool indexed;           // matrices picked by BLENDINDICES instead of WORLD0-3
  bool indices_d3dcolor;  // BLENDINDICES is D3DCOLOR, so its bytes load swizzled and normalized
  bool point_size;        // the vertex carries PSIZE
  bool point_scale;       // D3DRS_POINTSCALEENABLE
};

struct D3D9FFStage {
  DWORD color_op;
  DWORD color_arg[3];
  DWORD alpha_op;
  DWORD alpha_arg[3];
  DWORD result_arg;
  bool projected;
  D3DRESOURCETYPE texture_type;
};

struct D3D9FFPixelState {
  D3D9FFStage stages[ff::MaxStages];
  bool specular;
};

std::vector<DWORD> D3D9GenerateFixedFunctionVS(const D3D9FFVertexState &State);
std::vector<DWORD> D3D9GenerateFixedFunctionPS(const D3D9FFPixelState &State);

} // namespace mullion
