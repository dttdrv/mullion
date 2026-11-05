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

// D3D9 shader model 1-3 token stream ("d3dbc"). Header-only so the d3d9 runtime can share it.
// reference: https://learn.microsoft.com/en-us/windows-hardware/drivers/display/shader-code-tokens

#pragma once

#include <cstddef>
#include <cstdint>

namespace mullion::d3d9bc {

enum class Opcode : uint16_t {
  Nop = 0,
  Mov = 1,
  Add = 2,
  Sub = 3,
  Mad = 4,
  Mul = 5,
  Rcp = 6,
  Rsq = 7,
  Dp3 = 8,
  Dp4 = 9,
  Min = 10,
  Max = 11,
  Slt = 12,
  Sge = 13,
  Exp = 14,
  Log = 15,
  Lit = 16,
  Dst = 17,
  Lrp = 18,
  Frc = 19,
  M4x4 = 20,
  M4x3 = 21,
  M3x4 = 22,
  M3x3 = 23,
  M3x2 = 24,
  Call = 25,
  CallNz = 26,
  Loop = 27,
  Ret = 28,
  EndLoop = 29,
  Label = 30,
  Dcl = 31,
  Pow = 32,
  Crs = 33,
  Sgn = 34,
  Abs = 35,
  Nrm = 36,
  SinCos = 37,
  Rep = 38,
  EndRep = 39,
  If = 40,
  IfC = 41,
  Else = 42,
  EndIf = 43,
  Break = 44,
  BreakC = 45,
  Mova = 46,
  DefB = 47,
  DefI = 48,
  TexCoord = 64,
  TexKill = 65,
  Tex = 66,
  TexBem = 67,
  TexBemL = 68,
  TexReg2AR = 69,
  TexReg2GB = 70,
  TexM3x2Pad = 71,
  TexM3x2Tex = 72,
  TexM3x3Pad = 73,
  TexM3x3Tex = 74,
  TexM3x3Diff = 75,
  TexM3x3Spec = 76,
  TexM3x3VSpec = 77,
  ExpP = 78,
  LogP = 79,
  Cnd = 80,
  Def = 81,
  TexReg2RGB = 82,
  TexDp3Tex = 83,
  TexM3x2Depth = 84,
  TexDp3 = 85,
  TexM3x3 = 86,
  TexDepth = 87,
  Cmp = 88,
  Bem = 89,
  Dp2Add = 90,
  Dsx = 91,
  Dsy = 92,
  TexLdd = 93,
  SetP = 94,
  TexLdl = 95,
  BreakP = 96,
  Phase = 0xFFFD,
  Comment = 0xFFFE,
  End = 0xFFFF,
};

enum class RegisterType : uint8_t {
  Temp = 0,
  Input = 1,
  Const = 2,
  Addr = 3, // a0 in vertex shaders, t# in pixel shaders
  RastOut = 4,
  AttrOut = 5,
  Output = 6, // oT# before vs_3_0, o# in vs_3_0
  ConstInt = 7,
  ColorOut = 8,
  DepthOut = 9,
  Sampler = 10,
  Const2 = 11,
  Const3 = 12,
  Const4 = 13,
  ConstBool = 14,
  Loop = 15,
  TempFloat16 = 16,
  MiscType = 17,
  Label = 18,
  Predicate = 19,
};

enum class Usage : uint8_t {
  Position = 0,
  BlendWeight = 1,
  BlendIndices = 2,
  Normal = 3,
  PSize = 4,
  TexCoord = 5,
  Tangent = 6,
  Binormal = 7,
  TessFactor = 8,
  PositionT = 9,
  Color = 10,
  Fog = 11,
  Depth = 12,
  Sample = 13,
};

enum class TextureType : uint8_t {
  Unknown = 0,
  Texture2D = 2,
  Cube = 3,
  Volume = 4,
};

enum class SrcModifier : uint8_t {
  None = 0,
  Neg = 1,
  Bias = 2,
  BiasNeg = 3,
  Sign = 4,
  SignNeg = 5,
  Comp = 6,
  X2 = 7,
  X2Neg = 8,
  Dz = 9,
  Dw = 10,
  Abs = 11,
  AbsNeg = 12,
  Not = 13,
};

enum class Comparison : uint8_t {
  Gt = 1,
  Eq = 2,
  Ge = 3,
  Lt = 4,
  Ne = 5,
  Le = 6,
};

constexpr uint32_t kResultSaturate = 1;
constexpr uint32_t kResultPartialPrecision = 2;
constexpr uint32_t kResultCentroid = 4;

constexpr uint32_t kTexLdProject = 1;
constexpr uint32_t kTexLdBias = 2;

inline RegisterType
register_type(uint32_t token) {
  return RegisterType(((token >> 28) & 0x7) | ((token >> 8) & 0x18));
}

inline uint32_t
register_number(uint32_t token) {
  return token & 0x7FF;
}

inline bool
relative_addressing(uint32_t token) {
  return token & (1u << 13);
}

inline uint32_t
write_mask(uint32_t token) {
  return (token >> 16) & 0xF;
}

inline uint32_t
result_modifier(uint32_t token) {
  return (token >> 20) & 0xF;
}

inline int32_t
result_shift(uint32_t token) {
  int32_t shift = (token >> 24) & 0xF;
  return shift >= 8 ? shift - 16 : shift;
}

inline uint32_t
swizzle_component(uint32_t token, uint32_t index) {
  return (token >> (16 + index * 2)) & 0x3;
}

inline SrcModifier
src_modifier(uint32_t token) {
  return SrcModifier((token >> 24) & 0xF);
}

struct Version {
  bool pixel;
  uint32_t major;
  uint32_t minor;

  bool
  atLeast(uint32_t Major, uint32_t Minor) const {
    return major > Major || (major == Major && minor >= Minor);
  }
};

inline bool
read_version(uint32_t token, Version &version) {
  uint32_t type = token >> 16;
  if (type != 0xFFFE && type != 0xFFFF)
    return false;
  version = {type == 0xFFFF, (token >> 8) & 0xFF, token & 0xFF};
  return version.major >= 1 && version.major <= 3;
}

struct Instruction {
  Opcode opcode;
  uint32_t controls;
  bool predicated;
  bool coissue;
  const uint32_t *operands;
  uint32_t length;
};

// walks instructions after the version token. SM1 has no length field, so the parameter count is derived from the
// high bit that every parameter token carries.
class Reader {
public:
  Reader(const uint32_t *tokens, size_t count) : begin_(tokens), cur_(tokens + 1), end_(tokens + count) {
    read_version(tokens[0], version_);
  }

  const Version &
  version() const {
    return version_;
  }

  bool
  next(Instruction &inst) {
    while (cur_ < end_) {
      uint32_t token = *cur_++;
      Opcode op = Opcode(token & 0xFFFF);
      if (op == Opcode::Comment) {
        cur_ += (token >> 16) & 0x7FFF;
        continue;
      }
      if (op == Opcode::End)
        return false;
      inst.opcode = op;
      inst.controls = (token >> 16) & 0xFF;
      inst.predicated = token & (1u << 28);
      inst.coissue = token & (1u << 30);
      inst.operands = cur_;
      if (version_.major >= 2) {
        inst.length = (token >> 24) & 0xF;
      } else {
        uint32_t length = 0;
        while (cur_ + length < end_ && (cur_[length] & 0x80000000u))
          length++;
        if (op == Opcode::Def)
          length = 5;
        inst.length = length;
      }
      if (cur_ + inst.length > end_)
        return false;
      cur_ += inst.length;
      return true;
    }
    return false;
  }

private:
  const uint32_t *begin_;
  const uint32_t *cur_;
  const uint32_t *end_;
  Version version_ = {};
};

inline const char *
usage_name(Usage usage) {
  switch (usage) {
  case Usage::Position:
    return "position";
  case Usage::BlendWeight:
    return "blendweight";
  case Usage::BlendIndices:
    return "blendindices";
  case Usage::Normal:
    return "normal";
  case Usage::PSize:
    return "psize";
  case Usage::TexCoord:
    return "texcoord";
  case Usage::Tangent:
    return "tangent";
  case Usage::Binormal:
    return "binormal";
  case Usage::TessFactor:
    return "tessfactor";
  case Usage::PositionT:
    return "positiont";
  case Usage::Color:
    return "color";
  case Usage::Fog:
    return "fog";
  case Usage::Depth:
    return "depth";
  case Usage::Sample:
    return "sample";
  }
  return "unknown";
}

} // namespace mullion::d3d9bc
