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

#include "d3d9_fixed_function.hpp"
#include "d3d9_bytecode.hpp"
#include "log/log.hpp"
#include <bit>
#include <algorithm>
#include <initializer_list>

namespace mullion {

using namespace d3d9bc;

namespace {

constexpr DWORD kXXXX = 0x00, kYYYY = 0x55, kZZZZ = 0xAA, kWWWW = 0xFF, kXYZW = 0xE4;
constexpr uint32_t kHelperConst = 15; // (0, 1, 0.5, 2)

DWORD
Reg(RegisterType Type, uint32_t N) {
  uint32_t t = (uint32_t)Type;
  return 0x80000000u | N | ((t & 7) << 28) | ((t & 0x18) << 8);
}

DWORD
Dst(RegisterType Type, uint32_t N, uint32_t Mask = 0xF, bool Saturate = false) {
  return Reg(Type, N) | (Mask << 16) | (Saturate ? kResultSaturate << 20 : 0);
}

DWORD
Src(RegisterType Type, uint32_t N, DWORD Swizzle = kXYZW, SrcModifier Mod = SrcModifier::None) {
  return Reg(Type, N) | (Swizzle << 16) | ((DWORD)Mod << 24);
}

DWORD
Swizzled(DWORD Token, DWORD Swizzle) {
  return (Token & ~0x00FF0000u) | (Swizzle << 16);
}

DWORD
Modified(DWORD Token, SrcModifier Mod) {
  return (Token & ~0x0F000000u) | ((DWORD)Mod << 24);
}

DWORD
Temp(uint32_t N, DWORD Swizzle = kXYZW) {
  return Src(RegisterType::Temp, N, Swizzle);
}

DWORD
Helper(DWORD Swizzle, SrcModifier Mod = SrcModifier::None) {
  return Src(RegisterType::Const, kHelperConst, Swizzle, Mod);
}

struct Writer {
  std::vector<DWORD> code;

  void
  op(Opcode Op, std::initializer_list<DWORD> Args, DWORD Controls = 0) {
    code.push_back((DWORD)Op | (Controls << 16) | ((DWORD)Args.size() << 24));
    code.insert(code.end(), Args);
  }

  // for operands of varying length, like relative constants with their address token
  void
  op(Opcode Op, const std::vector<DWORD> &Args) {
    code.push_back((DWORD)Op | ((DWORD)Args.size() << 24));
    code.insert(code.end(), Args.begin(), Args.end());
  }

  void
  dcl(Usage U, uint32_t Index, DWORD DstToken) {
    op(Opcode::Dcl, {0x80000000u | (DWORD)U | (Index << 16), DstToken});
  }

  void
  dcl_sampler(TextureType Type, uint32_t N) {
    op(Opcode::Dcl, {0x80000000u | ((DWORD)Type << 27), Dst(RegisterType::Sampler, N)});
  }

  void
  def_helper() {
    op(Opcode::Def,
       {Dst(RegisterType::Const, kHelperConst), std::bit_cast<DWORD>(0.0f), std::bit_cast<DWORD>(1.0f),
        std::bit_cast<DWORD>(0.5f), std::bit_cast<DWORD>(2.0f)});
  }

  // copies a modified source into Scratch so another modifier can be applied to it.
  DWORD
  plain(DWORD Token, uint32_t Scratch) {
    if (src_modifier(Token) == SrcModifier::None)
      return Token;
    op(Opcode::Mov, {Dst(RegisterType::Temp, Scratch), Token});
    return Temp(Scratch);
  }
};

std::vector<DWORD>
Operands(DWORD Dst, DWORD A, std::pair<DWORD, DWORD> B) {
  std::vector<DWORD> tokens = {Dst, A, B.first};
  if (B.second)
    tokens.push_back(B.second);
  return tokens;
}

DWORD
MaterialSource(D3DMATERIALCOLORSOURCE Source, uint32_t MaterialConst) {
  if (Source == D3DMCS_COLOR1)
    return Src(RegisterType::Input, 2);
  if (Source == D3DMCS_COLOR2)
    return Src(RegisterType::Input, 3);
  return Src(RegisterType::Const, MaterialConst);
}

} // namespace

std::vector<DWORD>
D3D9GenerateFixedFunctionVS(const D3D9FFVertexState &S) {
  Writer w;
  w.code.push_back(0xFFFE0300);
  auto in = [](uint32_t n, DWORD swizzle = kXYZW) { return Src(RegisterType::Input, n, swizzle); };
  auto out = [](uint32_t n, uint32_t mask = 0xF, bool sat = false) { return Dst(RegisterType::Output, n, mask, sat); };
  auto c = [](uint32_t n, DWORD swizzle = kXYZW) { return Src(RegisterType::Const, n, swizzle); };
  auto r = [](uint32_t n, uint32_t mask = 0xF, bool sat = false) { return Dst(RegisterType::Temp, n, mask, sat); };
  auto t = [](uint32_t n, DWORD swizzle = kXYZW, SrcModifier mod = SrcModifier::None) {
    return Src(RegisterType::Temp, n, swizzle, mod);
  };
  // c19 = (epsilon, -1, log2(e), 0)
  constexpr uint32_t kHelper2 = 19;
  constexpr uint32_t kFogOut = 11;
  // r6 view position, r7 view normal, r8 vertex-to-eye, r9-r11 ambient/diffuse/specular sums
  constexpr uint32_t P = 6, N = 7, V = 8, Amb = 9, Diff = 10, Spec = 11;

  w.dcl(S.pretransformed ? Usage::PositionT : Usage::Position, 0, Dst(RegisterType::Input, 0));
  if (S.normal)
    w.dcl(Usage::Normal, 0, Dst(RegisterType::Input, 1));
  if (S.diffuse)
    w.dcl(Usage::Color, 0, Dst(RegisterType::Input, 2));
  if (S.specular)
    w.dcl(Usage::Color, 1, Dst(RegisterType::Input, 3));
  for (uint32_t i = 0; i < 8; i++)
    if (S.texcoord_mask & (1 << i))
      w.dcl(Usage::TexCoord, i, Dst(RegisterType::Input, 4 + i));
  bool blend = (S.blend_weights || S.indexed) && !S.pretransformed;
  if (blend && S.blend_weights)
    w.dcl(Usage::BlendWeight, 0, Dst(RegisterType::Input, 12));
  if (blend && S.indexed)
    w.dcl(Usage::BlendIndices, 0, Dst(RegisterType::Input, 15));
  bool tween = S.tween && !S.pretransformed;
  if (tween) {
    w.dcl(Usage::Position, 1, Dst(RegisterType::Input, 13));
    if (S.normal)
      w.dcl(Usage::Normal, 1, Dst(RegisterType::Input, 14));
  }
  w.dcl(Usage::Position, 0, out(0));
  w.dcl(Usage::Color, 0, out(1));
  w.dcl(Usage::Color, 1, out(2));
  for (uint32_t i = 0; i < ff::MaxStages; i++)
    if (S.texcoords[i].source != ff::TexNone)
      w.dcl(Usage::TexCoord, i, out(3 + i));
  if (S.fog_mode != ff::FogNone)
    w.dcl(Usage::Fog, 0, out(kFogOut, 0x1));
  constexpr uint32_t kPSizeOut = 12;
  bool psize = (S.point_size || S.point_scale) && !S.pretransformed;
  if (S.point_size)
    w.dcl(Usage::PSize, 0, Dst(RegisterType::Input, 16));
  if (psize)
    w.dcl(Usage::PSize, 0, out(kPSizeOut, 0x1));
  w.def_helper();
  w.op(Opcode::Def, {Dst(RegisterType::Const, kHelper2), std::bit_cast<DWORD>(1e-6f), std::bit_cast<DWORD>(-1.0f),
                     std::bit_cast<DWORD>(1.442695f), std::bit_cast<DWORD>(0.0f)});
  // c18 = palette stride for UBYTE4 and for normalized D3DCOLOR blend indices
  constexpr uint32_t kIndexScale = 18;
  if (S.indexed)
    w.op(Opcode::Def, {Dst(RegisterType::Const, kIndexScale), std::bit_cast<DWORD>(3.0f), std::bit_cast<DWORD>(765.0f),
                       std::bit_cast<DWORD>(0.0f), std::bit_cast<DWORD>(0.0f)});

  // r29 and r30 hold the tweened position and normal; untransformed vertices ignore the input w and use 1
  DWORD pos = S.pretransformed ? in(0) : t(29), nrm = tween && S.normal ? t(30) : in(1);
  if (tween) {
    w.op(Opcode::Add, {r(29), in(13), Src(RegisterType::Input, 0, kXYZW, SrcModifier::Neg)});
    w.op(Opcode::Mad, {r(29), t(29), c(ff::VSMaterialPower, kYYYY), in(0)});
    if (S.normal) {
      w.op(Opcode::Add, {r(30), in(14), Src(RegisterType::Input, 1, kXYZW, SrcModifier::Neg)});
      w.op(Opcode::Mad, {r(30), t(30), c(ff::VSMaterialPower, kYYYY), in(1)});
    }
  } else if (!S.pretransformed) {
    w.op(Opcode::Mov, {r(29, 0x7), in(0)});
  }
  if (!S.pretransformed)
    w.op(Opcode::Mov, {r(29, 0x8), Helper(kYYYY)});

  if (S.pretransformed) {
    w.op(Opcode::Mad, {r(0, 0x7), in(0), c(ff::VSViewportScale), c(ff::VSViewportOffset)});
    // an RHW of 0 counts as 1, as in wined3d (Wine's visual test_table_fog_zw)
    w.op(Opcode::Sge, {r(1, 0x8), Src(RegisterType::Input, 0, kWWWW, SrcModifier::AbsNeg), Helper(kXXXX)});
    w.op(Opcode::Add, {r(1, 0x8), t(1, kWWWW), in(0, kWWWW)});
    w.op(Opcode::Rcp, {r(0, 0x8), t(1, kWWWW)});
    w.op(Opcode::Mul, {out(0, 0x7), t(0), t(0, kWWWW)});
    w.op(Opcode::Mov, {out(0, 0x8), t(0, kWWWW)});
  } else if (blend) {
    // r6 and r12 sum the weighted view-space positions and normals; the last weight is 1 - the others
    w.op(Opcode::Mov, {r(P), Helper(kXXXX)});
    w.op(Opcode::Mov, {r(12), Helper(kXXXX)});
    w.op(Opcode::Mov, {r(25, 0x1), Helper(kYYYY)});
    if (S.indexed) {
      // r31 = palette offsets (index * 3); D3DCOLOR indices arrive as normalized (b, g, r, a) bytes
      DWORD indices = S.indices_d3dcolor ? in(15, 0xC6) : in(15);
      w.op(Opcode::Mul, {r(31), indices, c(kIndexScale, S.indices_d3dcolor ? kYYYY : kXXXX)});
    }
    for (uint32_t i = 0; i <= S.blend_weights; i++) {
      uint32_t base = i == 0 ? ff::VSWorldView : ff::VSWorldViewBlend + 4 * (i - 1);
      if (S.indexed)
        w.op(Opcode::Mova, {Dst(RegisterType::Addr, 0, 0x1), t(31, (i * 0x55) & 0xFF)});
      // relative constants take a second token naming the address register component
      auto column = [&](uint32_t k) -> std::pair<DWORD, DWORD> {
        if (S.indexed)
          return {c(ff::VSIndexedPalette + k) | (1u << 13), Src(RegisterType::Addr, 0, kXXXX)};
        return {c(base + k), 0};
      };
      DWORD weight = i < S.blend_weights ? in(12, (i * 0x55) & 0xFF) : t(25, kXXXX);
      if (i < S.blend_weights)
        w.op(Opcode::Add, {r(25, 0x1), t(25, kXXXX), Src(RegisterType::Input, 12, (i * 0x55) & 0xFF, SrcModifier::Neg)});
      for (uint32_t k = 0; k < 3; k++)
        w.op(Opcode::Dp4, Operands(r(26, 1u << k), pos, column(k)));
      w.op(Opcode::Mad, {r(P, 0x7), t(26), weight, t(P)});
      if (S.normal) {
        for (uint32_t k = 0; k < 3; k++)
          w.op(Opcode::Dp3, Operands(r(26, 1u << k), nrm, column(k)));
        w.op(Opcode::Mad, {r(12, 0x7), t(26), weight, t(12)});
      }
    }
    w.op(Opcode::Mov, {r(P, 0x8), Helper(kYYYY)});
    for (uint32_t i = 0; i < 4; i++)
      w.op(Opcode::Dp4, {out(0, 1 << i), t(P), c(ff::VSProjection + i)});
  } else {
    for (uint32_t i = 0; i < 4; i++)
      w.op(Opcode::Dp4, {out(0, 1 << i), pos, c(ff::VSWorldViewProj + i)});
  }

  bool lit = S.lighting && !S.pretransformed;
  bool needs_normal = (lit && S.normal) || std::any_of(std::begin(S.texcoords), std::end(S.texcoords), [](auto &tc) {
                        return tc.source == ff::TexGenNormal || tc.source == ff::TexGenReflection ||
                               tc.source == ff::TexGenSphereMap;
                      });
  bool needs_position = !S.pretransformed;
  if (needs_position) {
    if (!blend) {
      for (uint32_t i = 0; i < 3; i++)
        w.op(Opcode::Dp4, {r(P, 1 << i), pos, c(ff::VSWorldView + i)});
      w.op(Opcode::Mov, {r(P, 0x8), Helper(kYYYY)});
    }
    if (S.local_viewer) {
      w.op(Opcode::Nrm, {r(V, 0x7), t(P, kXYZW, SrcModifier::Neg)});
    } else {
      w.op(Opcode::Mov, {r(V), Helper(kXXXX)});
      w.op(Opcode::Mov, {r(V, 0x4), c(kHelper2, kYYYY)});
    }
  }
  if (needs_normal) {
    if (S.normal) {
      if (!blend)
        for (uint32_t i = 0; i < 3; i++)
          w.op(Opcode::Dp3, {r(12, 1 << i), nrm, c(ff::VSNormalMatrix + i)});
      w.op(S.normalize_normals ? Opcode::Nrm : Opcode::Mov, {r(N, 0x7), t(12)});
    } else {
      w.op(Opcode::Mov, {r(N), Helper(kXXXX)});
    }
  }

  if (lit) {
    DWORD diffuse = MaterialSource(S.diffuse_source, ff::VSMaterialDiffuse);
    DWORD ambient = MaterialSource(S.ambient_source, ff::VSMaterialAmbient);
    DWORD specular = MaterialSource(S.specular_source, ff::VSMaterialSpecular);
    DWORD emissive = MaterialSource(S.emissive_source, ff::VSMaterialEmissive);
    w.op(Opcode::Mov, {r(Amb), c(ff::VSAmbient)});
    w.op(Opcode::Mov, {r(Diff), Helper(kXXXX)});
    w.op(Opcode::Mov, {r(Spec), Helper(kXXXX)});
    for (uint32_t i = 0; i < S.light_count; i++) {
      uint32_t B = ff::VSLightBase + ff::VSLightStride * i;
      auto type = S.light_types[i];
      // r12 light vector, r13.x attenuation * spot
      if (type == D3DLIGHT_DIRECTIONAL) {
        w.op(Opcode::Mov, {r(12), c(B)});
        w.op(Opcode::Mov, {r(13, 0x1), Helper(kYYYY)});
      } else {
        w.op(Opcode::Add, {r(12, 0x7), c(B), t(P, kXYZW, SrcModifier::Neg)});
        w.op(Opcode::Dp3, {r(13, 0x2), t(12), t(12)});
        w.op(Opcode::Rsq, {r(13, 0x4), t(13, kYYYY)});
        w.op(Opcode::Mul, {r(12, 0x7), t(12), t(13, kZZZZ)});
        w.op(Opcode::Rcp, {r(13, 0x8), t(13, kZZZZ)});
        w.op(Opcode::Mad, {r(14, 0x1), c(B + 5, kZZZZ), t(13, kYYYY), c(B + 5, kXXXX)});
        w.op(Opcode::Mad, {r(14, 0x1), c(B + 5, kYYYY), t(13, kWWWW), t(14, kXXXX)});
        w.op(Opcode::Rcp, {r(13, 0x1), t(14, kXXXX)});
        w.op(Opcode::Sge, {r(14, 0x2), c(B + 5, kWWWW), t(13, kWWWW)});
        w.op(Opcode::Mul, {r(13, 0x1), t(13, kXXXX), t(14, kYYYY)});
        if (type == D3DLIGHT_SPOT) {
          // pow(saturate((rho - cos phi/2) / (cos theta/2 - cos phi/2)), falloff), zero outside the cone
          w.op(Opcode::Dp3, {r(14, 0x4), t(12, kXYZW, SrcModifier::Neg), c(B + 1)});
          w.op(Opcode::Add, {r(14, 0x4), t(14, kZZZZ), Src(RegisterType::Const, B + 6, kYYYY, SrcModifier::Neg)});
          w.op(Opcode::Mul, {r(14, 0x4, true), t(14, kZZZZ), c(B + 6, kZZZZ)});
          w.op(Opcode::Slt, {r(15, 0x1), Helper(kXXXX), t(14, kZZZZ)});
          w.op(Opcode::Max, {r(14, 0x4), t(14, kZZZZ), c(kHelper2, kXXXX)});
          w.op(Opcode::Pow, {r(14, 0x8), t(14, kZZZZ), c(B + 6, kWWWW)});
          w.op(Opcode::Mul, {r(14, 0x8), t(14, kWWWW), t(15, kXXXX)});
          w.op(Opcode::Mul, {r(13, 0x1), t(13, kXXXX), t(14, kWWWW)});
        }
      }
      w.op(Opcode::Mad, {r(Amb, 0x7), c(B + 4), t(13, kXXXX), t(Amb)});
      if (!S.normal)
        continue;
      w.op(Opcode::Dp3, {r(14, 0x1), t(N), t(12)});
      w.op(Opcode::Slt, {r(15, 0x2), Helper(kXXXX), t(14, kXXXX)});
      w.op(Opcode::Max, {r(14, 0x1), t(14, kXXXX), Helper(kXXXX)});
      w.op(Opcode::Mul, {r(14, 0x1), t(14, kXXXX), t(13, kXXXX)});
      w.op(Opcode::Mad, {r(Diff, 0x7), c(B + 2), t(14, kXXXX), t(Diff)});
      if (S.specular_lighting) {
        w.op(Opcode::Add, {r(16, 0x7), t(12), t(V)});
        w.op(Opcode::Nrm, {r(17, 0x7), t(16)});
        w.op(Opcode::Dp3, {r(14, 0x2), t(N), t(17)});
        w.op(Opcode::Max, {r(14, 0x2), t(14, kYYYY), c(kHelper2, kXXXX)});
        w.op(Opcode::Pow, {r(14, 0x2), t(14, kYYYY), c(ff::VSMaterialPower, kXXXX)});
        w.op(Opcode::Mul, {r(14, 0x2), t(14, kYYYY), t(13, kXXXX)});
        w.op(Opcode::Mul, {r(14, 0x2), t(14, kYYYY), t(15, kYYYY)});
        w.op(Opcode::Mad, {r(Spec, 0x7), c(B + 3), t(14, kYYYY), t(Spec)});
      }
    }
    w.op(Opcode::Mad, {r(18, 0x7), t(Amb), ambient, emissive});
    w.op(Opcode::Mad, {out(1, 0x7, true), t(Diff), diffuse, t(18)});
    w.op(Opcode::Mov, {out(1, 0x8), Swizzled(diffuse, kWWWW)});
    w.op(Opcode::Mul, {out(2, 0x7, true), t(Spec), specular});
    w.op(Opcode::Mov, {out(2, 0x8), S.specular ? in(3, kWWWW) : Helper(kXXXX)});
  } else {
    w.op(Opcode::Mov, {out(1), S.diffuse ? in(2) : Helper(kYYYY)});
    w.op(Opcode::Mov, {out(2), S.specular ? in(3) : Helper(kXXXX)});
  }

  for (uint32_t i = 0; i < ff::MaxStages; i++) {
    auto &tc = S.texcoords[i];
    if (tc.source == ff::TexNone)
      continue;
    constexpr uint32_t Coord = 20, Result = 21;
    if (tc.source < 8) {
      w.op(Opcode::Mov, {r(Coord), in(4 + tc.source)});
      // D3D pads a transformed coordinate with 1 after its last component and 0 after that, an untransformed one
      // with 0, and a projected untransformed one divides by its last component
      uint32_t size = std::max<uint32_t>(S.texcoord_size[tc.source], 1);
      if (tc.count) {
        if (size < 4)
          w.op(Opcode::Mov, {r(Coord, 1u << size), Helper(kYYYY)});
        if (size < 3)
          w.op(Opcode::Mov, {r(Coord, (0xFu << (size + 1)) & 0xF), Helper(kXXXX)});
      } else if (!S.pretransformed) {
        if (size < 4)
          w.op(Opcode::Mov, {r(Coord, (0xFu << size) & 0xF), Helper(kXXXX)});
        if (tc.projected && size < 4)
          w.op(Opcode::Mov, {r(Coord, 0x8), t(Coord, ((size - 1) * 0x55) & 0xFF)});
      }
    } else if (tc.source == ff::TexGenNormal) {
      w.op(Opcode::Mov, {r(Coord), t(N)});
      if (tc.count)
        w.op(Opcode::Mov, {r(Coord, 0x8), Helper(kYYYY)});
      else
        w.op(Opcode::Mov, {r(Coord, 0x8), tc.projected ? t(N, kZZZZ) : Helper(kXXXX)});
    } else if (tc.source == ff::TexGenPosition) {
      w.op(Opcode::Mov, {r(Coord), t(P)});
    } else {
      // reflect the eye-to-vertex direction about the normal
      w.op(Opcode::Mov, {r(22, 0x7), t(V, kXYZW, SrcModifier::Neg)});
      w.op(Opcode::Dp3, {r(23, 0x1), t(N), t(22)});
      w.op(Opcode::Add, {r(23, 0x1), t(23, kXXXX), t(23, kXXXX)});
      w.op(Opcode::Mad, {r(Coord, 0x7), t(N, kXYZW, SrcModifier::Neg), t(23, kXXXX), t(22)});
      w.op(Opcode::Mov, {r(Coord, 0x8), Helper(kYYYY)});
      if (tc.source == ff::TexGenSphereMap) {
        // u = Rx / m + 0.5, v = -Ry / m + 0.5, m = 2 |R - (0, 0, 1)| with the camera looking down +z
        w.op(Opcode::Mov, {r(27), t(Coord)});
        w.op(Opcode::Add, {r(27, 0x4), t(27, kZZZZ), Src(RegisterType::Const, kHelperConst, kYYYY, SrcModifier::Neg)});
        w.op(Opcode::Dp3, {r(28, 0x1), t(27), t(27)});
        w.op(Opcode::Rsq, {r(28, 0x1), t(28, kXXXX)});
        w.op(Opcode::Mul, {r(28, 0x1), t(28, kXXXX), Helper(kZZZZ)});
        w.op(Opcode::Mad, {r(Coord, 0x1), t(Coord, kXXXX), t(28, kXXXX), Helper(kZZZZ)});
        w.op(Opcode::Mad, {r(Coord, 0x2), t(Coord, kYYYY, SrcModifier::Neg), t(28, kXXXX), Helper(kZZZZ)});
        w.op(Opcode::Mov, {r(Coord, 0x4), Helper(kXXXX)});
      }
    }
    uint32_t coord = Coord;
    if (tc.count) {
      for (uint32_t k = 0; k < tc.count; k++)
        w.op(Opcode::Dp4, {r(Result, 1u << k), t(Coord), c(ff::VSTextureMatrixBase + 4 * i + k)});
      if (tc.count < 4)
        w.op(Opcode::Mov, {r(Result, (0xFu << tc.count) & 0xF), Helper(kXXXX)});
      coord = Result;
    }
    if (tc.projected && tc.count > 1 && tc.count < 4)
      w.op(Opcode::Mov, {r(coord, 0x8), t(coord, ((tc.count - 1) * 0x55) & 0xFF)});
    w.op(Opcode::Mov, {out(3 + i), t(coord)});
  }

  if (S.fog_mode != ff::FogNone) {
    if (S.fog_mode == D3DFOG_NONE || S.pretransformed) {
      w.op(Opcode::Mov, {out(kFogOut, 0x1), S.specular ? in(3, kWWWW) : Helper(kYYYY)});
    } else {
      constexpr uint32_t F = 24;
      if (S.range_fog) {
        w.op(Opcode::Dp3, {r(F, 0x1), t(P), t(P)});
        w.op(Opcode::Rsq, {r(F, 0x1), t(F, kXXXX)});
        w.op(Opcode::Rcp, {r(F, 0x1), t(F, kXXXX)});
      } else {
        // vertex fog takes the eye distance as |z| (Wine's visual test_negative_fixedfunction_fog)
        w.op(Opcode::Mov, {r(F, 0x1), Src(RegisterType::Temp, P, kZZZZ, SrcModifier::Abs)});
      }
      if (S.fog_mode == D3DFOG_LINEAR) {
        w.op(Opcode::Add, {r(F, 0x1), c(ff::VSFog, kYYYY), t(F, kXXXX, SrcModifier::Neg)});
        w.op(Opcode::Mul, {out(kFogOut, 0x1, true), t(F, kXXXX), c(ff::VSFog, kWWWW)});
      } else {
        w.op(Opcode::Mul, {r(F, 0x1), t(F, kXXXX), c(ff::VSFog, kZZZZ)});
        if (S.fog_mode == D3DFOG_EXP2)
          w.op(Opcode::Mul, {r(F, 0x1), t(F, kXXXX), t(F, kXXXX)});
        w.op(Opcode::Mul, {r(F, 0x1), t(F, kXXXX), c(kHelper2, kZZZZ)});
        w.op(Opcode::Exp, {out(kFogOut, 0x1, true), t(F, kXXXX, SrcModifier::Neg)});
      }
    }
  }
  if (psize) {
    // the vertex's PSIZE or D3DRS_POINTSIZE, scaled by viewport height * sqrt(1 / (A + B d + C d^2))
    constexpr uint32_t Size = 27;
    if (S.point_size)
      w.op(Opcode::Mov, {r(Size, 0x1), in(16, kXXXX)});
    else
      w.op(Opcode::Mov, {r(Size, 0x1), c(ff::VSPointScale, kWWWW)});
    if (S.point_scale) {
      if (S.point_size)
        w.op(Opcode::Mul, {r(Size, 0x1), t(Size, kXXXX), c(ff::VSViewportScale, kWWWW)});
      w.op(Opcode::Dp3, {r(28, 0x1), t(P), t(P)});
      w.op(Opcode::Rsq, {r(28, 0x2), t(28, kXXXX)});
      w.op(Opcode::Rcp, {r(28, 0x2), t(28, kYYYY)});
      w.op(Opcode::Mad, {r(28, 0x4), c(ff::VSPointScale, kYYYY), t(28, kYYYY), c(ff::VSPointScale, kXXXX)});
      w.op(Opcode::Mad, {r(28, 0x4), c(ff::VSPointScale, kZZZZ), t(28, kXXXX), t(28, kZZZZ)});
      w.op(Opcode::Rsq, {r(28, 0x4), t(28, kZZZZ)});
      w.op(Opcode::Mul, {r(Size, 0x1), t(Size, kXXXX), t(28, kZZZZ)});
    }
    w.op(Opcode::Mov, {out(kPSizeOut, 0x1), t(Size, kXXXX)});
  }
  w.code.push_back(0x0000FFFF);
  return std::move(w.code);
}

namespace {

DWORD
StageArg(DWORD Arg, uint32_t Stage) {
  DWORD token;
  switch (Arg & D3DTA_SELECTMASK) {
  case D3DTA_DIFFUSE:
    token = Src(RegisterType::Input, 0);
    break;
  case D3DTA_TEXTURE:
    token = Temp(1);
    break;
  case D3DTA_TFACTOR:
    token = Src(RegisterType::Const, ff::PSTextureFactor);
    break;
  case D3DTA_SPECULAR:
    token = Src(RegisterType::Input, 1);
    break;
  case D3DTA_TEMP:
    token = Temp(2);
    break;
  case D3DTA_CONSTANT:
    token = Src(RegisterType::Const, ff::PSStageConstantBase + Stage);
    break;
  default:
    token = Temp(0);
    break;
  }
  if (Arg & D3DTA_ALPHAREPLICATE)
    token = Swizzled(token, kWWWW);
  if (Arg & D3DTA_COMPLEMENT)
    token = Modified(token, SrcModifier::Comp);
  return token;
}

bool
UsesTexture(DWORD Op, const DWORD *Args) {
  if (Op == D3DTOP_BLENDTEXTUREALPHA || Op == D3DTOP_BLENDTEXTUREALPHAPM)
    return true;
  for (uint32_t i = 0; i < 3; i++)
    if ((Args[i] & D3DTA_SELECTMASK) == D3DTA_TEXTURE)
      return true;
  return false;
}

void
EmitStageOp(Writer &w, DWORD Op, const DWORD *Args, uint32_t Stage, uint32_t Mask) {
  DWORD a0 = StageArg(Args[0], Stage), a1 = StageArg(Args[1], Stage), a2 = StageArg(Args[2], Stage);
  DWORD d = Dst(RegisterType::Temp, 5, Mask, true);
  DWORD scratch = Dst(RegisterType::Temp, 3);
  switch (Op) {
  case D3DTOP_SELECTARG1:
    return w.op(Opcode::Mov, {d, a1});
  case D3DTOP_SELECTARG2:
    return w.op(Opcode::Mov, {d, a2});
  case D3DTOP_MODULATE:
    return w.op(Opcode::Mul, {d, a1, a2});
  case D3DTOP_MODULATE2X:
    w.op(Opcode::Mul, {scratch, a1, a2});
    return w.op(Opcode::Add, {d, Temp(3), Temp(3)});
  case D3DTOP_MODULATE4X:
    w.op(Opcode::Mul, {scratch, a1, a2});
    w.op(Opcode::Add, {scratch, Temp(3), Temp(3)});
    return w.op(Opcode::Add, {d, Temp(3), Temp(3)});
  case D3DTOP_ADD:
    return w.op(Opcode::Add, {d, a1, a2});
  case D3DTOP_ADDSIGNED:
    w.op(Opcode::Add, {scratch, a1, a2});
    return w.op(Opcode::Add, {d, Temp(3), Helper(kZZZZ, SrcModifier::Neg)});
  case D3DTOP_ADDSIGNED2X:
    w.op(Opcode::Add, {scratch, a1, a2});
    w.op(Opcode::Add, {scratch, Temp(3), Helper(kZZZZ, SrcModifier::Neg)});
    return w.op(Opcode::Add, {d, Temp(3), Temp(3)});
  case D3DTOP_SUBTRACT:
    return w.op(Opcode::Add, {d, a1, Modified(w.plain(a2, 4), SrcModifier::Neg)});
  case D3DTOP_ADDSMOOTH:
    return w.op(Opcode::Mad, {d, Modified(w.plain(a1, 4), SrcModifier::Comp), a2, a1});
  case D3DTOP_BLENDDIFFUSEALPHA:
    return w.op(Opcode::Lrp, {d, Src(RegisterType::Input, 0, kWWWW), a1, a2});
  case D3DTOP_BLENDTEXTUREALPHA:
    return w.op(Opcode::Lrp, {d, Temp(1, kWWWW), a1, a2});
  case D3DTOP_BLENDFACTORALPHA:
    return w.op(Opcode::Lrp, {d, Src(RegisterType::Const, ff::PSTextureFactor, kWWWW), a1, a2});
  case D3DTOP_BLENDCURRENTALPHA:
    return w.op(Opcode::Lrp, {d, Temp(0, kWWWW), a1, a2});
  case D3DTOP_BLENDTEXTUREALPHAPM:
    return w.op(Opcode::Mad, {d, a2, Src(RegisterType::Temp, 1, kWWWW, SrcModifier::Comp), a1});
  case D3DTOP_MODULATEALPHA_ADDCOLOR:
    return w.op(Opcode::Mad, {d, Swizzled(a1, kWWWW), a2, a1});
  case D3DTOP_MODULATECOLOR_ADDALPHA:
    return w.op(Opcode::Mad, {d, a1, a2, Swizzled(a1, kWWWW)});
  case D3DTOP_MODULATEINVALPHA_ADDCOLOR:
    return w.op(Opcode::Mad, {d, Modified(Swizzled(w.plain(a1, 4), kWWWW), SrcModifier::Comp), a2, a1});
  case D3DTOP_MODULATEINVCOLOR_ADDALPHA:
    return w.op(Opcode::Mad, {d, Modified(w.plain(a1, 4), SrcModifier::Comp), a2, Swizzled(a1, kWWWW)});
  case D3DTOP_DOTPRODUCT3:
    w.op(Opcode::Mad, {scratch, a1, Helper(kWWWW), Helper(kYYYY, SrcModifier::Neg)});
    w.op(Opcode::Mad, {Dst(RegisterType::Temp, 4), a2, Helper(kWWWW), Helper(kYYYY, SrcModifier::Neg)});
    return w.op(Opcode::Dp3, {Dst(RegisterType::Temp, 5, 0xF, true), Temp(3), Temp(4)});
  case D3DTOP_MULTIPLYADD:
    return w.op(Opcode::Mad, {d, a1, a2, a0});
  case D3DTOP_LERP:
    return w.op(Opcode::Lrp, {d, a0, a1, a2});
  default: {
    WARN_ONCE("D3D9: unsupported fixed-function texture op ", Op);
    return w.op(Opcode::Mov, {d, Temp(0)});
  }
  }
}

TextureType
SamplerType(D3DRESOURCETYPE Type) {
  return Type == D3DRTYPE_CUBETEXTURE ? TextureType::Cube : Type == D3DRTYPE_VOLUMETEXTURE ? TextureType::Volume
                                                                                          : TextureType::Texture2D;
}

} // namespace

std::vector<DWORD>
D3D9GenerateFixedFunctionPS(const D3D9FFPixelState &S) {
  Writer w;
  w.code.push_back(0xFFFF0300);

  uint32_t stage_count = 0, texcoord_mask = 0;
  for (; stage_count < ff::MaxStages; stage_count++) {
    auto &stage = S.stages[stage_count];
    if (stage.color_op == D3DTOP_DISABLE)
      break;
    if (stage.texture_type)
      texcoord_mask |= 1 << stage_count;
  }

  w.dcl(Usage::Color, 0, Dst(RegisterType::Input, 0));
  w.dcl(Usage::Color, 1, Dst(RegisterType::Input, 1));
  for (uint32_t i = 0; i < 8; i++)
    if (texcoord_mask & (1 << i))
      w.dcl(Usage::TexCoord, i, Dst(RegisterType::Input, 2 + i));
  for (uint32_t i = 0; i < stage_count; i++)
    if (S.stages[i].texture_type)
      w.dcl_sampler(SamplerType(S.stages[i].texture_type), i);
  w.def_helper();

  w.op(Opcode::Mov, {Dst(RegisterType::Temp, 0), Src(RegisterType::Input, 0)});
  w.op(Opcode::Mov, {Dst(RegisterType::Temp, 2), Helper(kXXXX)});
  // a BUMPENVMAP stage samples (du, dv[, l]) into r6 and offsets the next stage's texture coordinates
  int bump = -1;
  bool bump_luminance = false;
  for (uint32_t i = 0; i < stage_count; i++) {
    auto &stage = S.stages[i];
    if (stage.color_op == D3DTOP_BUMPENVMAP || stage.color_op == D3DTOP_BUMPENVMAPLUMINANCE) {
      if (stage.texture_type) {
        w.op(Opcode::Tex, {Dst(RegisterType::Temp, 6), Src(RegisterType::Input, 2 + i), Src(RegisterType::Sampler, i)});
        bump = i;
        bump_luminance = stage.color_op == D3DTOP_BUMPENVMAPLUMINANCE;
      }
      continue;
    }
    bool bumped = bump >= 0 && (uint32_t)bump + 1 == i;
    bool alpha_enabled = stage.alpha_op != D3DTOP_DISABLE;
    // an op that reads an unbound texture passes the current value through; the other op still runs.
    bool color_skipped = !stage.texture_type && UsesTexture(stage.color_op, stage.color_arg);
    bool alpha_skipped = !alpha_enabled || (!stage.texture_type && UsesTexture(stage.alpha_op, stage.alpha_arg));
    if (color_skipped && alpha_skipped)
      continue;
    if (stage.texture_type &&
        (UsesTexture(stage.color_op, stage.color_arg) || (alpha_enabled && UsesTexture(stage.alpha_op, stage.alpha_arg)))) {
      if (bumped) {
        uint32_t matrix = ff::PSBumpMatrixBase + bump, luminance = ff::PSBumpLuminanceBase + bump;
        w.op(Opcode::Mov, {Dst(RegisterType::Temp, 7), Src(RegisterType::Input, 2 + i)});
        w.op(Opcode::Mad, {Dst(RegisterType::Temp, 7, 0x3), Temp(6, kXXXX), Src(RegisterType::Const, matrix, 0x44), Temp(7)});
        w.op(Opcode::Mad, {Dst(RegisterType::Temp, 7, 0x3), Temp(6, kYYYY), Src(RegisterType::Const, matrix, 0xEE), Temp(7)});
        w.op(Opcode::Tex, {Dst(RegisterType::Temp, 1), Temp(7), Src(RegisterType::Sampler, i)});
        if (bump_luminance) {
          w.op(Opcode::Mad, {Dst(RegisterType::Temp, 8, 0x1, true), Temp(6, kZZZZ), Src(RegisterType::Const, luminance, kXXXX),
                             Src(RegisterType::Const, luminance, kYYYY)});
          w.op(Opcode::Mul, {Dst(RegisterType::Temp, 1), Temp(1), Temp(8, kXXXX)});
        }
      } else {
        w.op(Opcode::Tex, {Dst(RegisterType::Temp, 1), Src(RegisterType::Input, 2 + i), Src(RegisterType::Sampler, i)},
             stage.projected ? kTexLdProject : 0);
      }
    }
    if (color_skipped)
      w.op(Opcode::Mov, {Dst(RegisterType::Temp, 5, 0x7), Temp(0)});
    else
      EmitStageOp(w, stage.color_op, stage.color_arg, i, 0x7);
    if (color_skipped || stage.color_op != D3DTOP_DOTPRODUCT3) {
      if (!alpha_skipped)
        EmitStageOp(w, stage.alpha_op, stage.alpha_arg, i, 0x8);
      else
        w.op(Opcode::Mov, {Dst(RegisterType::Temp, 5, 0x8), Temp(0)});
    }
    w.op(Opcode::Mov, {Dst(RegisterType::Temp, stage.result_arg == D3DTA_TEMP ? 2u : 0u), Temp(5)});
  }
  if (S.specular)
    w.op(Opcode::Add, {Dst(RegisterType::Temp, 0, 0x7, true), Temp(0), Src(RegisterType::Input, 1)});
  w.op(Opcode::Mov, {Dst(RegisterType::ColorOut, 0), Temp(0)});
  w.code.push_back(0x0000FFFF);
  return std::move(w.code);
}

} // namespace mullion
