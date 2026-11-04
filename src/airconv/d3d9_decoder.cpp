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

#include "d3d9_decoder.hpp"
#include "dxbc_converter.hpp"
#include "airconv_public.h"
#include "adt.hpp"
#include <algorithm>
#include <bit>
#include <cfloat>
#include <cstring>
#include <format>
#include <stack>

namespace mullion::d3d9 {

using namespace dxbc;
using namespace d3d9bc;
using shader::common::ResourceType;
using shader::common::ScalerDataType;

namespace {

constexpr uint32_t kNoPhase = ~0u;
constexpr uint32_t kRuntimeBumpEnv = offsetof(SM30_RUNTIME_CONSTANTS, bump_env) / 16;
constexpr uint32_t kRuntimeBumpLuminance = offsetof(SM30_RUNTIME_CONSTANTS, bump_luminance) / 16;
constexpr uint32_t kRuntimeProjection = offsetof(SM30_RUNTIME_CONSTANTS, projection) / 16;
constexpr uint32_t kRuntimeVec4Count = sizeof(SM30_RUNTIME_CONSTANTS) / 16;

SrcOperandCommon &
common(SrcOperand &src) {
  return std::visit([](auto &o) -> SrcOperandCommon & { return o._; }, src);
}

DstOperandCommon &
common(DstOperand &dst) {
  return std::visit([](auto &o) -> DstOperandCommon & { return o._; }, dst);
}

Swizzle
compose(Swizzle base, std::array<uint8_t, 4> perm) {
  uint8_t c[4] = {base.x, base.y, base.z, base.w};
  return {c[perm[0]], c[perm[1]], c[perm[2]], c[perm[3]]};
}

Swizzle
replicate(uint8_t c) {
  return {c, c, c, c};
}

bool
is_replicate(Swizzle s) {
  return s.x == s.y && s.y == s.z && s.z == s.w;
}

SrcOperand
temp_src(uint32_t reg, Swizzle swz = swizzle_identity, OperandDataType type = OperandDataType::Float) {
  return SrcOperandTemp{._ = {swz, false, false, type}, .regid = reg, .phase = kNoPhase};
}

DstOperand
temp_dst(uint32_t reg, uint32_t mask = 0xF, OperandDataType type = OperandDataType::Float) {
  return DstOperandTemp{._ = {mask, type}, .regid = reg, .phase = kNoPhase};
}

SrcOperand
imm_f(float x, float y, float z, float w) {
  SrcOperandImmediate32 imm{._ = {swizzle_identity, false, false, OperandDataType::Float}};
  imm.fvalue[0] = x;
  imm.fvalue[1] = y;
  imm.fvalue[2] = z;
  imm.fvalue[3] = w;
  return imm;
}

SrcOperand
imm_f(float v) {
  return imm_f(v, v, v, v);
}

SrcOperand
imm_u(uint32_t v) {
  SrcOperandImmediate32 imm{._ = {swizzle_identity, false, false, OperandDataType::Integer}};
  for (auto &u : imm.uvalue)
    u = v;
  return imm;
}

SrcOperand
negate(SrcOperand src) {
  common(src).neg = !common(src).neg;
  return src;
}

SrcOperand
with_swizzle(SrcOperand src, Swizzle swz) {
  common(src).swizzle = swz;
  return src;
}

SrcOperand
component(SrcOperand src, uint8_t c) {
  common(src).swizzle = replicate(((uint8_t *)&common(src).swizzle)[c]);
  return src;
}

InstructionCommon
ic(bool saturate = false) {
  return {saturate, 0};
}

struct Dst {
  DstOperand op;
  bool saturate = false;
  float scale = 1.0f;
  RegisterType type = RegisterType::Temp;
  uint32_t number = 0;

  uint32_t
  mask() {
    return common(op).mask;
  }
};

struct LoopContext {
  BasicBlock *header;
  BasicBlock *latch;
  BasicBlock *exit;
  uint32_t depth;
  bool has_al;
  SrcOperand step;
};

struct IfContext {
  BasicBlock *else_bb;
  BasicBlock *end_bb;
  bool has_else;
};

class Decoder {
public:
  Decoder(SM50ShaderInternal &shader, ShaderData &data, const Version &version, const DecodeOptions &options) :
      sh_(shader),
      d_(data),
      info_(shader.shader_info),
      ver_(version),
      ps_(version.pixel),
      ps1_(version.pixel && version.major == 1),
      ps14_(ps1_ && version.minor >= 4),
      ps1_texture_types_(options.ps1_texture_types),
      shadow_samplers_(options.shadow_samplers),
      fetch4_samplers_(options.fetch4_samplers),
      unbound_samplers_(options.unbound_samplers) {}

  std::optional<std::string>
  run(Reader &reader) {
    cur_ = fresh("entry");
    ret_main_ = fresh("final");
    auto copy = fresh("copy_out");
    auto ret = fresh("return");
    ret->target = BasicBlockReturn{};
    ret_main_->target = BasicBlockUnconditionalBranch{copy};
    copy->target = BasicBlockUnconditionalBranch{ret};
    d_.final_bb = ret_main_;
    d_.copy_bb = copy;

    // before vs_3_0, diffuse components the shader doesn't write read as 1
    if (!ps_ && ver_.major < 3) {
      record_legacy_output(reg::VSOutColorBase, Usage::Color, 0);
      emit(InstMov{
          ic(), DstOperandOutput{._ = {0xF, OperandDataType::Float}, .regid = reg::VSOutColorBase, .phase = kNoPhase},
          imm_f(1.0f, 1.0f, 1.0f, 1.0f)
      });
      emit(InstMov{ic(), temp_dst(reg::OutSpecular), imm_f(0.0f, 0.0f, 0.0f, 0.0f)});
    }

    d3d9bc::Instruction inst;
    while (!error_ && reader.next(inst)) {
      scratch_next_ = 0;
      decode(inst);
    }
    if (error_)
      return error_;
    if (ps1_) {
      emit(InstMov{ic(), temp_dst(reg::OutColorBase), temp_src(0)});
      d_.color_output_mask |= 1;
    }
    if (!loops_.empty() || !ifs_.empty())
      return "unterminated control flow";
    if (std::holds_alternative<BasicBlockUndefined>(cur_->target))
      cur_->target = BasicBlockUnconditionalBranch{func_ret_ ? func_ret_ : ret_main_};

    inline_calls(std::move(call_targets_), func_entries_, [this](const char *name) { return fresh(name); });

    auto copy_out = [&](uint32_t temp, uint32_t output) {
      d_.copy_bb->instructions.push_back(InstMov{
          ic(), DstOperandOutput{._ = {0xF, OperandDataType::Float}, .regid = output, .phase = kNoPhase},
          temp_src(temp)
      });
    };
    for (uint32_t i = 0; i < 4; i++)
      if (d_.color_output_mask & (1 << i))
        copy_out(reg::OutColorBase + i, i);
    if (d_.pos_output_reg != ~0u)
      copy_out(reg::OutPos, d_.pos_output_reg);
    if (writes_specular_) {
      copy_out(reg::OutSpecular, reg::VSOutColorBase + 1);
      // without oFog, vertex fog is the specular alpha
      if (d_.fog_output_reg == ~0u) {
        record_legacy_output(reg::VSOutFog, Usage::Fog, 0);
        d_.copy_bb->instructions.push_back(InstMov{
            ic(), DstOperandOutput{._ = {0xF, OperandDataType::Float}, .regid = reg::VSOutFog, .phase = kNoPhase},
            temp_src(reg::OutSpecular, replicate(3))
        });
      }
    }
    if (d_.fog_output_reg != ~0u)
      copy_out(reg::OutFog, d_.fog_output_reg);

    // cb3 carries the values the compile-time epilogue needs, so every shader declares it
    cb_used_ |= 1 << cb::Runtime;
    info_.tempRegisterCount = reg::TotalTemps;
    for (uint32_t i = 0; i < 4; i++) {
      if (!(cb_used_ & (1 << i)))
        continue;
      ConstantBufferInfo cbv;
      cbv.range = {.range_id = i, .lower_bound = i, .size = 1, .space = 0};
      cbv.size_in_vec4 = i == cb::Float ? 256 : i == cb::Int ? 16 : i == cb::Bool ? 4 : kRuntimeVec4Count;
      info_.cbufferMap[i] = cbv;
    }
    return std::nullopt;
  }

private:
  BasicBlock *
  fresh(const char *name) {
    sh_.bbs.push_back(std::make_unique<BasicBlock>(name));
    return sh_.bbs.back().get();
  }

  // the runtime refuses shaders using constants their version doesn't have, def'd ones included (Wine's
  // test_unsupported_shaders; Wine's own d3d9 does too)
  bool
  check_constant(RegisterType type, uint32_t n) {
    uint32_t limit;
    if (type == RegisterType::Const)
      limit = !ps_ ? 256 : ver_.major < 2 ? 8 : ver_.major < 3 ? 32 : 224;
    else
      limit = !ps_ || ver_.major >= 3 || (ver_.major == 2 && ver_.minor) ? 16 : 0;
    if (n < limit)
      return true;
    fail("d3d9: constant register " + std::to_string(n) + " is out of range for the shader version");
    return false;
  }

  void
  fail(std::string message) {
    if (!error_)
      error_ = std::move(message);
  }

  void
  emit(dxbc::Instruction inst) {
    cur_->instructions.push_back(std::move(inst));
  }

  uint32_t
  scratch() {
    if (scratch_next_ >= reg::ScratchCount) {
      fail("d3d9: out of scratch registers");
      return reg::ScratchBase;
    }
    return reg::ScratchBase + scratch_next_++;
  }

  uint32_t
  al_reg() {
    uint32_t depth = loops_.empty() ? 0 : loops_.top().depth;
    return reg::ALBase + depth;
  }

  OperandIndex
  relative_index(const uint32_t *&p, uint32_t offset) {
    uint32_t reg = reg::A0;
    uint8_t comp = 0;
    if (ver_.major >= 2) {
      uint32_t addr = *p++;
      comp = swizzle_component(addr, 0);
      if (register_type(addr) == RegisterType::Loop)
        reg = al_reg();
    }
    return IndexByTempComponent{.regid = reg, .phase = kNoPhase, .component = comp, .offset = offset};
  }

  Swizzle
  read_swizzle(uint32_t token) {
    return {
        (uint8_t)swizzle_component(token, 0), (uint8_t)swizzle_component(token, 1),
        (uint8_t)swizzle_component(token, 2), (uint8_t)swizzle_component(token, 3)
    };
  }

  // decodes a source parameter without its modifier. RegOffset is added to the register number (matrix rows).
  SrcOperand
  src_register(const uint32_t *&p, uint32_t reg_offset, bool &is_not) {
    uint32_t token = *p++;
    auto type = register_type(token);
    uint32_t n = register_number(token) + reg_offset;
    Swizzle swz = read_swizzle(token);
    bool relative = relative_addressing(token);
    is_not = src_modifier(token) == SrcModifier::Not;
    SrcOperandCommon c{swz, false, false, OperandDataType::Float};

    switch (type) {
    case RegisterType::Temp:
      return SrcOperandTemp{._ = c, .regid = n, .phase = kNoPhase};
    case RegisterType::Input:
      if (ps1_)
        record_input(n, Usage::Color, n);
      if (relative)
        return SrcOperandIndexableInput{._ = c, .regindex = relative_index(p, n)};
      return SrcOperandInput{._ = c, .regid = n};
    case RegisterType::Addr:
      if (ps1_ && !ps14_)
        return SrcOperandTemp{._ = c, .regid = reg::PS1TexBase + n, .phase = kNoPhase};
      if (ps14_)
        record_input(reg::PSTexCoordBase + n, Usage::TexCoord, n);
      if (ps_)
        return SrcOperandInput{._ = c, .regid = reg::PSTexCoordBase + n};
      c.read_type = OperandDataType::Integer;
      return SrcOperandTemp{._ = c, .regid = reg::A0, .phase = kNoPhase};
    case RegisterType::Const: {
      if (!relative)
        check_constant(RegisterType::Const, n);
      if (relative) {
        cb_used_ |= 1 << cb::Float;
        return SrcOperandConstantBuffer{
            ._ = c, .rangeid = cb::Float, .rangeindex = cb::Float, .regindex = relative_index(p, n)
        };
      }
      if (auto it = defs_f_.find(n); it != defs_f_.end()) {
        SrcOperandImmediate32 imm{._ = c};
        std::memcpy(imm.uvalue, it->second.data(), 16);
        if (ps1_)
          for (auto &f : imm.fvalue)
            f = std::clamp(f, -1.0f, 1.0f);
        return imm;
      }
      cb_used_ |= 1 << cb::Float;
      if (ps1_) {
        // ps_1_x constants are clamped to [-1, 1]
        uint32_t t = scratch();
        SrcOperandCommon whole{swizzle_identity, false, false, OperandDataType::Float};
        emit(InstFloatBinaryOp{
            ic(), FloatBinaryOp::Max, temp_dst(t),
            SrcOperandConstantBuffer{._ = whole, .rangeid = cb::Float, .rangeindex = cb::Float, .regindex = n},
            imm_f(-1.0f)
        });
        emit(InstFloatBinaryOp{ic(), FloatBinaryOp::Min, temp_dst(t), temp_src(t), imm_f(1.0f)});
        return temp_src(t, swz);
      }
      return SrcOperandConstantBuffer{._ = c, .rangeid = cb::Float, .rangeindex = cb::Float, .regindex = n};
    }
    case RegisterType::ConstInt: {
      check_constant(RegisterType::ConstInt, n);
      c.read_type = OperandDataType::Integer;
      if (auto it = defs_i_.find(n); it != defs_i_.end()) {
        SrcOperandImmediate32 imm{._ = c};
        std::memcpy(imm.uvalue, it->second.data(), 16);
        return imm;
      }
      cb_used_ |= 1 << cb::Int;
      return SrcOperandConstantBuffer{._ = c, .rangeid = cb::Int, .rangeindex = cb::Int, .regindex = n};
    }
    case RegisterType::ConstBool: {
      check_constant(RegisterType::ConstBool, n);
      c.read_type = OperandDataType::Integer;
      if (auto it = defs_b_.find(n); it != defs_b_.end())
        return with_swizzle(imm_u(it->second ? ~0u : 0u), c.swizzle);
      cb_used_ |= 1 << cb::Bool;
      c.swizzle = replicate(n % 4);
      return SrcOperandConstantBuffer{._ = c, .rangeid = cb::Bool, .rangeindex = cb::Bool, .regindex = n / 4};
    }
    case RegisterType::Loop:
      c.read_type = OperandDataType::Integer;
      return SrcOperandTemp{._ = c, .regid = al_reg(), .phase = kNoPhase};
    case RegisterType::MiscType:
      return SrcOperandTemp{._ = c, .regid = n == 0 ? reg::VPos : reg::VFace, .phase = kNoPhase};
    case RegisterType::Predicate:
      c.read_type = OperandDataType::Integer;
      return SrcOperandTemp{._ = c, .regid = reg::P0, .phase = kNoPhase};
    default:
      fail(std::format("d3d9: unsupported source register type {}", (uint32_t)type));
      return temp_src(0);
    }
  }

  SrcOperand
  src(const uint32_t *&p, uint32_t reg_offset = 0) {
    uint32_t token = *p;
    bool is_not;
    SrcOperand operand = src_register(p, reg_offset, is_not);
    auto mod = src_modifier(token);
    auto &c = common(operand);
    switch (mod) {
    case SrcModifier::None:
    case SrcModifier::Not:
      return operand;
    case SrcModifier::Neg:
      c.neg = true;
      return operand;
    case SrcModifier::Abs:
      c.abs = true;
      return operand;
    case SrcModifier::AbsNeg:
      c.abs = true;
      c.neg = true;
      return operand;
    default:
      break;
    }

    uint32_t t = scratch();
    switch (mod) {
    case SrcModifier::Bias:
    case SrcModifier::BiasNeg:
      emit(InstFloatBinaryOp{ic(), FloatBinaryOp::Add, temp_dst(t), operand, imm_f(-0.5f)});
      break;
    case SrcModifier::Sign:
    case SrcModifier::SignNeg:
      emit(InstFloatMAD{ic(), temp_dst(t), operand, imm_f(2.0f), imm_f(-1.0f)});
      break;
    case SrcModifier::Comp:
      emit(InstFloatBinaryOp{ic(), FloatBinaryOp::Add, temp_dst(t), negate(operand), imm_f(1.0f)});
      break;
    case SrcModifier::X2:
    case SrcModifier::X2Neg:
      emit(InstFloatBinaryOp{ic(), FloatBinaryOp::Add, temp_dst(t), operand, operand});
      break;
    case SrcModifier::Dz:
    case SrcModifier::Dw:
      emit(InstFloatBinaryOp{
          ic(), FloatBinaryOp::Div, temp_dst(t), operand, component(operand, mod == SrcModifier::Dz ? 2 : 3)
      });
      break;
    default:
      fail(std::format("d3d9: unsupported source modifier {}", (uint32_t)mod));
      break;
    }
    SrcOperand result = temp_src(t);
    if (mod == SrcModifier::BiasNeg || mod == SrcModifier::SignNeg || mod == SrcModifier::X2Neg)
      common(result).neg = true;
    return result;
  }

  // scalar instructions read one component; vs_1_x without a replicate swizzle uses w.
  SrcOperand
  scalar_src(const uint32_t *&p) {
    SrcOperand operand = src(p);
    auto &c = common(operand);
    if (!is_replicate(c.swizzle))
      c.swizzle = replicate(c.swizzle.w);
    return operand;
  }

  void
  record_input(uint32_t reg, Usage usage, uint32_t index) {
    for (auto &in : d_.inputs)
      if (in.reg == reg)
        return;
    d_.inputs.push_back({reg, usage, index, 0xF, false});
  }

  SrcOperand
  texcoord(uint32_t n, Swizzle swz = swizzle_identity) {
    record_input(reg::PSTexCoordBase + n, Usage::TexCoord, n);
    return SrcOperandInput{._ = {swz, false, false, OperandDataType::Float}, .regid = reg::PSTexCoordBase + n};
  }

  SrcOperand
  runtime(uint32_t index, Swizzle swz = swizzle_identity) {
    return SrcOperandConstantBuffer{
        ._ = {swz, false, false, OperandDataType::Float}, .rangeid = cb::Runtime, .rangeindex = cb::Runtime,
        .regindex = index
    };
  }

  void
  record_legacy_output(uint32_t reg, Usage usage, uint32_t index) {
    for (auto &out : d_.outputs)
      if (out.reg == reg)
        return;
    d_.outputs.push_back({reg, usage, index, 0xF, false});
  }

  Dst
  dst(const uint32_t *&p) {
    uint32_t token = *p++;
    Dst result;
    result.type = register_type(token);
    result.number = register_number(token);
    result.saturate = result_modifier(token) & kResultSaturate;
    uint32_t mask = write_mask(token);
    uint32_t n = result.number;
    if (int32_t shift = result_shift(token)) {
      if (!ps1_)
        fail("d3d9: result shift is only valid in ps_1_x");
      result.scale = shift > 0 ? float(1 << shift) : 1.0f / float(1 << -shift);
    }

    switch (result.type) {
    case RegisterType::Temp:
      result.op = temp_dst(n, mask);
      break;
    case RegisterType::Addr:
      if (ps1_ && !ps14_)
        result.op = temp_dst(reg::PS1TexBase + n, mask);
      else
        result.op = temp_dst(reg::A0, mask, OperandDataType::Integer);
      break;
    case RegisterType::Predicate:
      result.op = temp_dst(reg::P0, mask, OperandDataType::Integer);
      break;
    case RegisterType::RastOut: {
      uint32_t reg = n == 0 ? reg::VSOutPos : n == 1 ? reg::VSOutFog : reg::VSOutPSize;
      record_legacy_output(reg, n == 0 ? Usage::Position : n == 1 ? Usage::Fog : Usage::PSize, 0);
      if (n == 0)
        d_.pos_output_reg = reg;
      else if (n == 1)
        d_.fog_output_reg = reg;
      else
        d_.writes_psize = true;
      result.op = temp_dst(n == 0 ? reg::OutPos : n == 1 ? reg::OutFog : reg::OutPSize, mask);
      break;
    }
    case RegisterType::AttrOut:
      // vertex colors before vs_3_0 are clamped to [0, 1] before interpolation
      result.saturate = true;
      record_legacy_output(reg::VSOutColorBase + n, Usage::Color, n);
      if (n == 1) {
        writes_specular_ = true;
        result.op = temp_dst(reg::OutSpecular, mask);
        break;
      }
      result.op = DstOperandOutput{
          ._ = {mask, OperandDataType::Float}, .regid = reg::VSOutColorBase + n, .phase = kNoPhase
      };
      break;
    case RegisterType::Output:
      if (ver_.major >= 3) {
        if (relative_addressing(token))
          result.op = DstOperandIndexableOutput{
              ._ = {mask, OperandDataType::Float}, .regindex = relative_index(p, n), .phase = kNoPhase
          };
        else if (n == d_.pos_output_reg)
          result.op = temp_dst(reg::OutPos, mask);
        else if (n == d_.fog_output_reg)
          result.op = temp_dst(reg::OutFog, mask);
        else if (n == psize_output_reg_) {
          d_.writes_psize = true;
          result.op = temp_dst(reg::OutPSize, mask);
        } else
          result.op = DstOperandOutput{._ = {mask, OperandDataType::Float}, .regid = n, .phase = kNoPhase};
      } else {
        record_legacy_output(reg::VSOutTexCoordBase + n, Usage::TexCoord, n);
        result.op = DstOperandOutput{
            ._ = {mask, OperandDataType::Float}, .regid = reg::VSOutTexCoordBase + n, .phase = kNoPhase
        };
      }
      break;
    case RegisterType::ColorOut:
      d_.color_output_mask |= 1 << n;
      result.op = temp_dst(reg::OutColorBase + n, mask);
      break;
    case RegisterType::DepthOut:
      d_.writes_depth = true;
      result.op = DstOperandOutputDepth{._ = {0x1, OperandDataType::Float}};
      break;
    default:
      fail(std::format("d3d9: unsupported destination register type {}", (uint32_t)result.type));
      result.op = DstOperandNull{};
      break;
    }
    return result;
  }

  SrcOperand
  dst_as_src(const Dst &d) {
    return std::visit(
        patterns{
            [](const DstOperandTemp &t) -> SrcOperand {
              return SrcOperandTemp{
                  ._ = {swizzle_identity, false, false, t._.write_type}, .regid = t.regid, .phase = kNoPhase
              };
            },
            [this](const auto &) -> SrcOperand {
              fail("d3d9: predicated write to a non-temp register");
              return temp_src(0);
            },
        },
        d.op
    );
  }

  // runs Body, which writes Target. Handles predication and saturation for instructions that can't do it inline.
  template <typename Fn>
  void
  write(Dst d, std::optional<SrcOperand> predicate, Fn &&body, bool inline_sat) {
    bool need_temp = (d.saturate && !inline_sat) || (predicate && !is_replicate(common(*predicate).swizzle));
    if (!predicate && d.scale != 1.0f) {
      uint32_t t = scratch();
      body(temp_dst(t, d.mask()), false);
      emit(InstFloatBinaryOp{ic(d.saturate), FloatBinaryOp::Mul, d.op, temp_src(t), imm_f(d.scale)});
      return;
    }
    if (!predicate) {
      if (!need_temp) {
        body(d.op, d.saturate);
        return;
      }
      uint32_t t = scratch();
      body(temp_dst(t, d.mask()), false);
      emit(InstMov{ic(true), d.op, temp_src(t)});
      return;
    }

    auto &pc = common(*predicate);
    if (is_replicate(pc.swizzle)) {
      auto then_bb = fresh("pred_then");
      auto after_bb = fresh("pred_after");
      cur_->target = BasicBlockConditionalBranch{{*predicate, !pred_not_}, then_bb, after_bb};
      cur_ = then_bb;
      if (d.saturate && !inline_sat) {
        uint32_t t = scratch();
        body(temp_dst(t, d.mask()), false);
        emit(InstMov{ic(true), d.op, temp_src(t)});
      } else {
        body(d.op, d.saturate);
      }
      cur_->target = BasicBlockUnconditionalBranch{after_bb};
      cur_ = after_bb;
      return;
    }

    uint32_t t = scratch();
    body(temp_dst(t, d.mask()), d.saturate && inline_sat);
    SrcOperand value = temp_src(t);
    if (d.saturate && !inline_sat) {
      emit(InstMov{ic(true), temp_dst(t, d.mask()), value});
    }
    if (pred_not_)
      emit(InstMovConditional{ic(), d.op, *predicate, dst_as_src(d), value});
    else
      emit(InstMovConditional{ic(), d.op, *predicate, value, dst_as_src(d)});
  }

  std::optional<SrcOperand>
  read_predicate(const uint32_t *&p, bool predicated) {
    pred_not_ = false;
    if (!predicated)
      return std::nullopt;
    uint32_t token = *p;
    bool is_not;
    SrcOperand pred = src_register(p, 0, is_not);
    pred_not_ = src_modifier(token) == SrcModifier::Not;
    return pred;
  }

  void
  alu(const d3d9bc::Instruction &inst) {
    const uint32_t *p = inst.operands;
    Dst d = dst(p);
    auto pred = read_predicate(p, inst.predicated);
    auto sat_write = [&](auto &&body) { write(d, pred, body, true); };

    switch (inst.opcode) {
    case Opcode::Mov: {
      SrcOperand a = src(p);
      if (d.type == RegisterType::Addr) {
        uint32_t t = scratch();
        emit(InstFloatUnaryOp{ic(), FloatUnaryOp::RoundNegativeInf, temp_dst(t), a});
        write(d, pred, [&](DstOperand o, bool) {
          emit(InstConvert{ConversionOp::FloatToSigned, o, temp_src(t)});
        }, false);
        break;
      }
      sat_write([&](DstOperand o, bool s) { emit(InstMov{ic(s), o, a}); });
      break;
    }
    case Opcode::Mova: {
      SrcOperand a = src(p);
      uint32_t t = scratch();
      emit(InstFloatUnaryOp{ic(), FloatUnaryOp::RoundNearestEven, temp_dst(t), a});
      write(d, pred, [&](DstOperand o, bool) {
        emit(InstConvert{ConversionOp::FloatToSigned, o, temp_src(t)});
      }, false);
      break;
    }
    case Opcode::Add:
    case Opcode::Sub:
    case Opcode::Mul:
    case Opcode::Min:
    case Opcode::Max: {
      SrcOperand a = src(p);
      SrcOperand b = src(p);
      FloatBinaryOp op = inst.opcode == Opcode::Mul   ? FloatBinaryOp::Mul
                         : inst.opcode == Opcode::Min ? FloatBinaryOp::Min
                         : inst.opcode == Opcode::Max ? FloatBinaryOp::Max
                                                      : FloatBinaryOp::Add;
      if (inst.opcode == Opcode::Sub)
        b = negate(b);
      sat_write([&](DstOperand o, bool s) { emit(InstFloatBinaryOp{ic(s), op, o, a, b}); });
      break;
    }
    case Opcode::Mad: {
      SrcOperand a = src(p), b = src(p), c = src(p);
      sat_write([&](DstOperand o, bool s) { emit(InstFloatMAD{ic(s), o, a, b, c}); });
      break;
    }
    case Opcode::Dp3:
    case Opcode::Dp4: {
      SrcOperand a = src(p), b = src(p);
      uint8_t dim = inst.opcode == Opcode::Dp3 ? 3 : 4;
      sat_write([&](DstOperand o, bool s) { emit(InstDotProduct{ic(s), o, a, b, dim}); });
      break;
    }
    case Opcode::Dp2Add: {
      SrcOperand a = src(p), b = src(p), c = scalar_src(p);
      uint32_t t = scratch();
      emit(InstDotProduct{ic(), temp_dst(t, 0x1), a, b, 2});
      sat_write([&](DstOperand o, bool s) {
        emit(InstFloatBinaryOp{ic(s), FloatBinaryOp::Add, o, temp_src(t, replicate(0)), c});
      });
      break;
    }
    case Opcode::Rcp:
    case Opcode::Rsq:
    case Opcode::Exp:
    case Opcode::ExpP:
    case Opcode::Log:
    case Opcode::LogP: {
      SrcOperand a = scalar_src(p);
      FloatUnaryOp op;
      switch (inst.opcode) {
      case Opcode::Rcp:
        op = FloatUnaryOp::Rcp;
        break;
      case Opcode::Rsq:
        op = FloatUnaryOp::Rsq;
        common(a).abs = true;
        common(a).neg = false;
        break;
      case Opcode::Exp:
      case Opcode::ExpP:
        op = FloatUnaryOp::Exp2;
        break;
      default:
        op = FloatUnaryOp::Log2;
        common(a).abs = true;
        common(a).neg = false;
        break;
      }
      if (op == FloatUnaryOp::Rsq) {
        // rsq of 0 is FLT_MAX rather than infinity on D3D9 hardware (Wine's visual fp_special_test)
        uint32_t t = scratch();
        emit(InstFloatUnaryOp{ic(), op, temp_dst(t, 0x1), a});
        sat_write([&](DstOperand o, bool s) {
          emit(InstFloatBinaryOp{ic(s), FloatBinaryOp::Min, o, temp_src(t, replicate(0)), imm_f(FLT_MAX)});
        });
        break;
      }
      sat_write([&](DstOperand o, bool s) { emit(InstFloatUnaryOp{ic(s), op, o, a}); });
      break;
    }
    case Opcode::Frc: {
      SrcOperand a = src(p);
      sat_write([&](DstOperand o, bool s) { emit(InstFloatUnaryOp{ic(s), FloatUnaryOp::Fraction, o, a}); });
      break;
    }
    case Opcode::Abs: {
      SrcOperand a = src(p);
      common(a).abs = true;
      common(a).neg = false;
      sat_write([&](DstOperand o, bool s) { emit(InstMov{ic(s), o, a}); });
      break;
    }
    case Opcode::Pow: {
      SrcOperand a = scalar_src(p), b = scalar_src(p);
      common(a).abs = true;
      uint32_t t = scratch();
      emit(InstFloatUnaryOp{ic(), FloatUnaryOp::Log2, temp_dst(t, 0x1), a});
      emit(InstFloatBinaryOp{ic(), FloatBinaryOp::Mul, temp_dst(t, 0x1), temp_src(t, replicate(0)), b});
      sat_write([&](DstOperand o, bool s) {
        emit(InstFloatUnaryOp{ic(s), FloatUnaryOp::Exp2, o, temp_src(t, replicate(0))});
      });
      break;
    }
    case Opcode::Slt:
    case Opcode::Sge: {
      SrcOperand a = src(p), b = src(p);
      uint32_t t = scratch();
      auto cmp = inst.opcode == Opcode::Slt ? FloatComparison::LessThan : FloatComparison::GreaterEqual;
      emit(InstFloatCompare{ic(), cmp, temp_dst(t, d.mask(), OperandDataType::Integer), a, b});
      write(d, pred, [&](DstOperand o, bool) {
        emit(InstIntegerBinaryOp{
            IntegerBinaryOp::And, o, temp_src(t, swizzle_identity, OperandDataType::Integer),
            imm_u(std::bit_cast<uint32_t>(1.0f))
        });
      }, false);
      break;
    }
    case Opcode::Sgn: {
      SrcOperand a = src(p);
      uint32_t pos = scratch(), neg = scratch();
      emit(InstFloatCompare{ic(), FloatComparison::LessThan, temp_dst(pos, 0xF, OperandDataType::Integer), imm_f(0.0f), a});
      emit(InstFloatCompare{ic(), FloatComparison::LessThan, temp_dst(neg, 0xF, OperandDataType::Integer), a, imm_f(0.0f)});
      emit(InstMovConditional{ic(), temp_dst(pos), temp_src(pos, swizzle_identity, OperandDataType::Integer), imm_f(1.0f), imm_f(0.0f)});
      emit(InstMovConditional{ic(), temp_dst(neg), temp_src(neg, swizzle_identity, OperandDataType::Integer), imm_f(1.0f), imm_f(0.0f)});
      sat_write([&](DstOperand o, bool s) {
        emit(InstFloatBinaryOp{ic(s), FloatBinaryOp::Add, o, temp_src(pos), negate(temp_src(neg))});
      });
      break;
    }
    case Opcode::Lrp: {
      SrcOperand a = src(p), b = src(p), c = src(p);
      uint32_t t = scratch();
      emit(InstFloatBinaryOp{ic(), FloatBinaryOp::Add, temp_dst(t), b, negate(c)});
      sat_write([&](DstOperand o, bool s) { emit(InstFloatMAD{ic(s), o, a, temp_src(t), c}); });
      break;
    }
    case Opcode::Cmp:
    case Opcode::Cnd: {
      SrcOperand a = src(p), b = src(p), c = src(p);
      // a coissued cnd before ps_1_4 always selects the first value unless it only writes alpha (Wine's cnd_test)
      if (inst.opcode == Opcode::Cnd && ps1_ && !ps14_ && inst.coissue && d.mask() != 0x8) {
        sat_write([&](DstOperand o, bool s) { emit(InstMov{ic(s), o, b}); });
        break;
      }
      uint32_t t = scratch();
      auto mask_dst = temp_dst(t, 0xF, OperandDataType::Integer);
      if (inst.opcode == Opcode::Cmp)
        emit(InstFloatCompare{ic(), FloatComparison::GreaterEqual, mask_dst, a, imm_f(0.0f)});
      else
        emit(InstFloatCompare{ic(), FloatComparison::LessThan, mask_dst, imm_f(0.5f), a});
      sat_write([&](DstOperand o, bool s) {
        emit(InstMovConditional{ic(s), o, temp_src(t, swizzle_identity, OperandDataType::Integer), b, c});
      });
      break;
    }
    case Opcode::Crs: {
      SrcOperand a = src(p), b = src(p);
      uint32_t t = scratch();
      auto &ca = common(a);
      auto &cb_ = common(b);
      SrcOperand a_zxy = with_swizzle(a, compose(ca.swizzle, {2, 0, 1, 3}));
      SrcOperand a_yzx = with_swizzle(a, compose(ca.swizzle, {1, 2, 0, 3}));
      SrcOperand b_yzx = with_swizzle(b, compose(cb_.swizzle, {1, 2, 0, 3}));
      SrcOperand b_zxy = with_swizzle(b, compose(cb_.swizzle, {2, 0, 1, 3}));
      emit(InstFloatBinaryOp{ic(), FloatBinaryOp::Mul, temp_dst(t, 0x7), a_zxy, b_yzx});
      common(d.op).mask &= 0x7;
      sat_write([&](DstOperand o, bool s) { emit(InstFloatMAD{ic(s), o, a_yzx, b_zxy, negate(temp_src(t))}); });
      break;
    }
    case Opcode::Nrm: {
      SrcOperand a = src(p);
      uint32_t t = scratch();
      emit(InstDotProduct{ic(), temp_dst(t, 0x1), a, a, 3});
      emit(InstFloatUnaryOp{ic(), FloatUnaryOp::Rsq, temp_dst(t, 0x1), temp_src(t, replicate(0))});
      // a zero vector normalizes to 0, not NaN
      emit(InstFloatBinaryOp{ic(), FloatBinaryOp::Min, temp_dst(t, 0x1), temp_src(t, replicate(0)), imm_f(FLT_MAX)});
      sat_write([&](DstOperand o, bool s) {
        emit(InstFloatBinaryOp{ic(s), FloatBinaryOp::Mul, o, a, temp_src(t, replicate(0))});
      });
      break;
    }
    case Opcode::Lit: {
      SrcOperand a = src(p);
      uint32_t r = scratch(), t = scratch(), cond = scratch();
      emit(InstMov{ic(), temp_dst(r), imm_f(1.0f, 0.0f, 0.0f, 1.0f)});
      emit(InstFloatBinaryOp{ic(), FloatBinaryOp::Max, temp_dst(t, 0x3), a, imm_f(0.0f)});
      emit(InstMov{ic(), temp_dst(r, 0x2), temp_src(t, replicate(0))});
      emit(InstFloatBinaryOp{ic(), FloatBinaryOp::Max, temp_dst(t, 0x4), component(a, 3), imm_f(-127.9961f)});
      emit(InstFloatBinaryOp{ic(), FloatBinaryOp::Min, temp_dst(t, 0x4), temp_src(t, replicate(2)), imm_f(127.9961f)});
      emit(InstFloatUnaryOp{ic(), FloatUnaryOp::Log2, temp_dst(t, 0x2), temp_src(t, replicate(1))});
      emit(InstFloatBinaryOp{
          ic(), FloatBinaryOp::Mul, temp_dst(t, 0x2), temp_src(t, replicate(1)), temp_src(t, replicate(2))
      });
      emit(InstFloatUnaryOp{ic(), FloatUnaryOp::Exp2, temp_dst(t, 0x2), temp_src(t, replicate(1))});
      emit(InstFloatCompare{
          ic(), FloatComparison::LessThan, temp_dst(cond, 0x1, OperandDataType::Integer), imm_f(0.0f),
          component(a, 0)
      });
      emit(InstMovConditional{
          ic(), temp_dst(r, 0x4), temp_src(cond, replicate(0), OperandDataType::Integer), temp_src(t, replicate(1)),
          imm_f(0.0f)
      });
      sat_write([&](DstOperand o, bool s) { emit(InstMov{ic(s), o, temp_src(r)}); });
      break;
    }
    case Opcode::Dst: {
      SrcOperand a = src(p), b = src(p);
      uint32_t r = scratch();
      emit(InstMov{ic(), temp_dst(r), imm_f(1.0f)});
      emit(InstFloatBinaryOp{ic(), FloatBinaryOp::Mul, temp_dst(r, 0x2), a, b});
      emit(InstMov{ic(), temp_dst(r, 0x4), a});
      emit(InstMov{ic(), temp_dst(r, 0x8), b});
      sat_write([&](DstOperand o, bool s) { emit(InstMov{ic(s), o, temp_src(r)}); });
      break;
    }
    case Opcode::M4x4:
    case Opcode::M4x3:
    case Opcode::M3x4:
    case Opcode::M3x3:
    case Opcode::M3x2: {
      uint32_t rows = inst.opcode == Opcode::M4x4 || inst.opcode == Opcode::M3x4   ? 4
                      : inst.opcode == Opcode::M3x2                                ? 2
                                                                                   : 3;
      uint8_t dim = inst.opcode == Opcode::M4x4 || inst.opcode == Opcode::M4x3 ? 4 : 3;
      SrcOperand a = src(p);
      uint32_t r = scratch();
      const uint32_t *row_token = p;
      for (uint32_t i = 0; i < rows; i++) {
        const uint32_t *q = row_token;
        SrcOperand row = src(q, i);
        emit(InstDotProduct{ic(), temp_dst(r, 1u << i), a, row, dim});
      }
      common(d.op).mask &= (1u << rows) - 1;
      sat_write([&](DstOperand o, bool s) { emit(InstMov{ic(s), o, temp_src(r)}); });
      break;
    }
    case Opcode::SinCos: {
      SrcOperand a = scalar_src(p);
      uint32_t r = scratch();
      emit(InstSinCos{ic(), temp_dst(r, 0x2), temp_dst(r, 0x1), a});
      sat_write([&](DstOperand o, bool s) { emit(InstMov{ic(s), o, temp_src(r)}); });
      break;
    }
    case Opcode::Dsx:
    case Opcode::Dsy: {
      SrcOperand a = src(p);
      bool ddy = inst.opcode == Opcode::Dsy;
      sat_write([&](DstOperand o, bool s) { emit(InstPartialDerivative{ic(s), o, a, ddy, false}); });
      break;
    }
    case Opcode::SetP: {
      SrcOperand a = src(p), b = src(p);
      write(d, pred, [&](DstOperand o, bool) { compare(Comparison(inst.controls & 0x7), o, a, b); }, false);
      break;
    }
    default:
      fail(std::format("d3d9: unsupported instruction {}", (uint32_t)inst.opcode));
      break;
    }
  }

  void
  compare(Comparison cmp, DstOperand out, SrcOperand a, SrcOperand b) {
    common(out).write_type = OperandDataType::Integer;
    switch (cmp) {
    case Comparison::Gt:
      emit(InstFloatCompare{ic(), FloatComparison::LessThan, out, b, a});
      break;
    case Comparison::Eq:
      emit(InstFloatCompare{ic(), FloatComparison::Equal, out, a, b});
      break;
    case Comparison::Ge:
      emit(InstFloatCompare{ic(), FloatComparison::GreaterEqual, out, a, b});
      break;
    case Comparison::Lt:
      emit(InstFloatCompare{ic(), FloatComparison::LessThan, out, a, b});
      break;
    case Comparison::Ne:
      emit(InstFloatCompare{ic(), FloatComparison::NotEqual, out, a, b});
      break;
    case Comparison::Le:
      emit(InstFloatCompare{ic(), FloatComparison::GreaterEqual, out, b, a});
      break;
    default:
      fail("d3d9: invalid comparison");
      break;
    }
  }

  BasicBlockCondition
  compare_condition(const d3d9bc::Instruction &inst, const uint32_t *&p) {
    SrcOperand a = src(p), b = src(p);
    uint32_t t = scratch();
    compare(Comparison(inst.controls & 0x7), temp_dst(t, 0x1), component(a, 0), component(b, 0));
    return {temp_src(t, replicate(0), OperandDataType::Integer), true};
  }

  BasicBlockCondition
  bool_condition(const uint32_t *&p) {
    uint32_t token = *p;
    bool is_not;
    SrcOperand operand = src_register(p, 0, is_not);
    auto &c = common(operand);
    c.read_type = OperandDataType::Integer;
    if (!is_replicate(c.swizzle))
      c.swizzle = replicate(c.swizzle.x);
    return {operand, src_modifier(token) != SrcModifier::Not};
  }

  void
  texture(const d3d9bc::Instruction &inst) {
    if (!ps_ && !ver_.atLeast(3, 0)) {
      fail("d3d9: texture sampling in a vertex shader requires vs_3_0");
      return;
    }
    const uint32_t *p = inst.operands;
    Dst d = dst(p);
    auto pred = read_predicate(p, inst.predicated);
    SrcOperand coord = src(p);
    uint32_t sampler_token = *p++;
    uint32_t s = register_number(sampler_token);
    auto resource = use_sampler(s, read_swizzle(sampler_token));
    SrcOperandSampler sampler{.range_id = s, .index = s, .gather_channel = 0};

    auto emit_sample = [&](DstOperand o, bool) {
      if (ps_ && (unbound_samplers_ & (1u << s))) {
        emit(InstMov{ic(), o, with_swizzle(imm_f(0.0f, 0.0f, 0.0f, 1.0f), read_swizzle(sampler_token))});
        return;
      }
      if (shadow_samplers_ & (1u << s)) {
        SrcOperand at = coord;
        if ((inst.controls & 0x3) == kTexLdProject) {
          uint32_t t = scratch();
          emit(InstFloatBinaryOp{ic(), FloatBinaryOp::Div, temp_dst(t), coord, component(coord, 3)});
          at = temp_src(t);
        }
        return shadow_compare(s, at, o);
      }
      if (fetch4_samplers_ & (1u << s)) {
        SrcOperand at = coord;
        if ((inst.controls & 0x3) == kTexLdProject) {
          uint32_t t = scratch();
          emit(InstFloatBinaryOp{ic(), FloatBinaryOp::Div, temp_dst(t), coord, component(coord, 3)});
          at = temp_src(t);
        }
        return fetch4(s, at, o, read_swizzle(sampler_token));
      }
      switch (inst.opcode) {
      case Opcode::TexLdl:
        emit(InstSampleLOD{o, coord, resource, sampler, component(coord, 3), {0, 0, 0}, std::nullopt});
        return;
      case Opcode::TexLdd: {
        SrcOperand dx = src(p), dy = src(p);
        emit(InstSampleDerivative{o, coord, resource, sampler, dx, dy, {0, 0, 0}, std::nullopt, std::nullopt});
        return;
      }
      default:
        break;
      }
      uint32_t mode = inst.controls & 0x3;
      if (mode == kTexLdProject) {
        uint32_t t = scratch();
        emit(InstFloatBinaryOp{ic(), FloatBinaryOp::Div, temp_dst(t), coord, component(coord, 3)});
        emit(InstSample{o, temp_src(t), resource, sampler, {0, 0, 0}, std::nullopt, std::nullopt});
      } else if (mode == kTexLdBias) {
        emit(InstSampleBias{o, coord, resource, sampler, component(coord, 3), {0, 0, 0}, std::nullopt, std::nullopt});
      } else {
        emit(InstSample{o, coord, resource, sampler, {0, 0, 0}, std::nullopt, std::nullopt});
      }
    };
    write(d, pred, emit_sample, false);
  }

  SrcOperandResource
  use_sampler(uint32_t s, Swizzle swz) {
    auto type = TextureType::Texture2D;
    if (auto it = sampler_types_.find(s); it != sampler_types_.end())
      type = it->second;
    if (ps1_) {
      uint32_t bits = (ps1_texture_types_ >> (2 * s)) & 3;
      type = bits == 1 ? TextureType::Cube : bits == 2 ? TextureType::Volume : TextureType::Texture2D;
    }
    if (!info_.srvMap.contains(s)) {
      ShaderResourceViewInfo srv;
      srv.range = {.range_id = s, .lower_bound = s, .size = 1, .space = 0};
      srv.scaler_type = ScalerDataType::Float;
      srv.resource_type = type == TextureType::Cube     ? ResourceType::TextureCube
                          : type == TextureType::Volume ? ResourceType::Texture3D
                                                        : ResourceType::Texture2D;
      srv.structure_stride = -1;
      srv.sampled = true;
      info_.srvMap[s] = srv;
      SamplerInfo sampler;
      sampler.range = srv.range;
      info_.samplerMap[s] = sampler;
    }
    return {.range_id = s, .index = s, .read_swizzle = swz};
  }

  // D3D9 samples depth textures as a comparison of the coordinate's z with the stored depth, in every channel.
  // Metal's sample_compare returned 0 for these textures through our bindings, so the comparison is done here
  // on a plain sample; that matches point-filtered shadow maps but not hardware PCF under linear filtering.
  void
  shadow_compare(uint32_t stage, SrcOperand coord, DstOperand out) {
    auto resource = use_sampler(stage, swizzle_identity);
    SrcOperandSampler sampler{.range_id = stage, .index = stage, .gather_channel = 0};
    uint32_t d = scratch(), pass = scratch();
    if (shadow_samplers_ & (1u << (16 + stage))) {
      // PCF: compare the bilinear footprint, gathered as (x0, y1), (x1, y1), (x1, y0), (x0, y0), then blend it
      uint32_t size = scratch(), f = scratch();
      emit(InstResourceInfo{temp_dst(size, 0x3), imm_u(0), resource, InstResourceInfo::M::none});
      emit(InstFloatMAD{ic(), temp_dst(f, 0x3), coord, temp_src(size), imm_f(-0.5f)});
      emit(InstFloatUnaryOp{ic(), FloatUnaryOp::Fraction, temp_dst(f, 0x3), temp_src(f)});
      emit(InstGather{temp_dst(d), coord, resource, sampler, imm_u(0), std::nullopt});
      emit(InstFloatCompare{
          ic(), FloatComparison::GreaterEqual, temp_dst(pass, 0xF, OperandDataType::Integer), temp_src(d),
          component(coord, 2)
      });
      emit(InstMovConditional{ic(), temp_dst(d), temp_src(pass, swizzle_identity, OperandDataType::Integer), imm_f(1.0f), imm_f(0.0f)});
      uint32_t row = scratch();
      // row.x = top (y0) row, row.y = bottom (y1) row, blended across x
      emit(InstFloatBinaryOp{ic(), FloatBinaryOp::Add, temp_dst(row, 0x1), temp_src(d, replicate(2)), negate(temp_src(d, replicate(3)))});
      emit(InstFloatMAD{ic(), temp_dst(row, 0x1), temp_src(row, replicate(0)), temp_src(f, replicate(0)), temp_src(d, replicate(3))});
      emit(InstFloatBinaryOp{ic(), FloatBinaryOp::Add, temp_dst(row, 0x2), temp_src(d, replicate(1)), negate(temp_src(d, replicate(0)))});
      emit(InstFloatMAD{ic(), temp_dst(row, 0x2), temp_src(row, replicate(1)), temp_src(f, replicate(0)), temp_src(d, replicate(0))});
      emit(InstFloatBinaryOp{ic(), FloatBinaryOp::Add, temp_dst(row, 0x4), temp_src(row, replicate(1)), negate(temp_src(row, replicate(0)))});
      emit(InstFloatMAD{ic(), out, temp_src(row, replicate(2)), temp_src(f, replicate(1)), temp_src(row, replicate(0))});
      return;
    }
    emit(InstSample{temp_dst(d), coord, resource, sampler, {0, 0, 0}, std::nullopt, std::nullopt});
    emit(InstFloatCompare{
        ic(), FloatComparison::GreaterEqual, temp_dst(pass, 0x1, OperandDataType::Integer), temp_src(d, replicate(0)),
        component(coord, 2)
    });
    emit(InstMovConditional{ic(), out, temp_src(pass, replicate(0), OperandDataType::Integer), imm_f(1.0f), imm_f(0.0f)});
  }

  // AMD's FETCH4: the first channel of the 2x2 footprint whose top-left texel the coordinate falls in, as
  // (x+1, y), (x, y+1), (x+1, y+1), (x, y); gather picks the footprint around the nearest texel corner, so the
  // coordinate moves half a texel first
  void
  fetch4(uint32_t stage, SrcOperand coord, DstOperand out, Swizzle swz = swizzle_identity) {
    auto resource = use_sampler(stage, swizzle_identity);
    uint32_t size = scratch(), at = scratch(), r = scratch();
    emit(InstResourceInfo{temp_dst(size, 0x3), imm_u(0), resource, InstResourceInfo::M::rcp});
    emit(InstMov{ic(), temp_dst(at), coord});
    emit(InstFloatMAD{ic(), temp_dst(at, 0x3), temp_src(size), imm_f(0.5f), coord});
    SrcOperandSampler sampler{.range_id = stage, .index = stage, .gather_channel = 0};
    emit(InstGather{temp_dst(r), temp_src(at), resource, sampler, imm_u(0), std::nullopt});
    emit(InstMov{ic(), out, temp_src(r, compose({2, 0, 1, 3}, {swz.x, swz.y, swz.z, swz.w}))});
  }

  void
  ps1_sample(uint32_t stage, SrcOperand coord, DstOperand out) {
    if (unbound_samplers_ & (1u << stage)) {
      emit(InstMov{ic(), out, imm_f(0.0f, 0.0f, 0.0f, 1.0f)});
      return;
    }
    if (shadow_samplers_ & (1u << stage))
      return shadow_compare(stage, coord, out);
    if (fetch4_samplers_ & (1u << stage))
      return fetch4(stage, coord, out);
    auto resource = use_sampler(stage, swizzle_identity);
    SrcOperandSampler sampler{.range_id = stage, .index = stage, .gather_channel = 0};
    emit(InstSample{out, coord, resource, sampler, {0, 0, 0}, std::nullopt, std::nullopt});
  }

  // samples Stage into a fresh scratch register and returns it.
  SrcOperand
  ps1_sample(uint32_t stage, SrcOperand coord) {
    uint32_t t = scratch();
    ps1_sample(stage, coord, temp_dst(t));
    return temp_src(t);
  }

  // coord + bump matrix of Stage * du/dv, as texbem and bem compute it
  SrcOperand
  bump(uint32_t stage, SrcOperand base, SrcOperand delta) {
    uint32_t t = scratch();
    emit(InstMov{ic(), temp_dst(t), base});
    emit(InstFloatMAD{ic(), temp_dst(t, 0x3), component(delta, 0), runtime(kRuntimeBumpEnv + stage, {0, 1, 0, 1}), temp_src(t)});
    emit(InstFloatMAD{ic(), temp_dst(t, 0x3), component(delta, 1), runtime(kRuntimeBumpEnv + stage, {2, 3, 2, 3}), temp_src(t)});
    return temp_src(t);
  }

  void
  matrix_row(uint32_t stage, SrcOperand value) {
    if (matrix_row_ > 2) {
      fail("d3d9: too many texm3x pad instructions");
      return;
    }
    emit(InstDotProduct{ic(), temp_dst(reg::PS1Matrix, 1u << matrix_row_), texcoord(stage), value, 3});
    matrix_stages_[matrix_row_++] = stage;
  }

  void
  write_depth(SrcOperand z, SrcOperand w) {
    // z / w, or 1 when w is 0
    uint32_t t = scratch(), cond = scratch();
    emit(InstFloatBinaryOp{ic(), FloatBinaryOp::Div, temp_dst(t, 0x1), z, w});
    emit(InstFloatCompare{ic(), FloatComparison::Equal, temp_dst(cond, 0x1, OperandDataType::Integer), w, imm_f(0.0f)});
    emit(InstMovConditional{
        ic(true), DstOperandOutputDepth{._ = {0x1, OperandDataType::Float}},
        temp_src(cond, replicate(0), OperandDataType::Integer), imm_f(1.0f), temp_src(t, replicate(0))
    });
    d_.writes_depth = true;
  }

  // reflects the eye vector E about the normal in PS1Matrix: 2 N (N.E) / (N.N) - E.
  SrcOperand
  reflect(SrcOperand eye) {
    SrcOperand n = temp_src(reg::PS1Matrix);
    uint32_t t = scratch(), r = scratch();
    emit(InstDotProduct{ic(), temp_dst(t, 0x1), n, eye, 3});
    emit(InstDotProduct{ic(), temp_dst(t, 0x2), n, n, 3});
    emit(InstFloatBinaryOp{ic(), FloatBinaryOp::Add, temp_dst(t, 0x1), temp_src(t, replicate(0)), temp_src(t, replicate(0))});
    emit(InstFloatBinaryOp{ic(), FloatBinaryOp::Div, temp_dst(t, 0x1), temp_src(t, replicate(0)), temp_src(t, replicate(1))});
    emit(InstFloatMAD{ic(), temp_dst(r), n, temp_src(t, replicate(0)), negate(eye)});
    return temp_src(r);
  }

  // D3DTTFF_PROJECTED divides by the component the one-hot selector picks, and by 1 without one:
  // dot(tc, sel) + 1 - dot(sel, 1), exact either way
  SrcOperand
  projected_texcoord(uint32_t m) {
    uint32_t w = scratch(), coord = scratch();
    emit(InstDotProduct{ic(), temp_dst(w, 0x1), runtime(kRuntimeProjection + m), imm_f(-1.0f), 4});
    emit(InstFloatBinaryOp{ic(), FloatBinaryOp::Add, temp_dst(w, 0x1), temp_src(w, replicate(0)), imm_f(1.0f)});
    emit(InstDotProduct{ic(), temp_dst(w, 0x2), texcoord(m), runtime(kRuntimeProjection + m), 4});
    emit(InstFloatBinaryOp{ic(), FloatBinaryOp::Add, temp_dst(w, 0x1), temp_src(w, replicate(0)), temp_src(w, replicate(1))});
    emit(InstFloatBinaryOp{ic(), FloatBinaryOp::Div, temp_dst(coord), texcoord(m), temp_src(w, replicate(0))});
    return temp_src(coord);
  }

  void
  ps1_texture(const d3d9bc::Instruction &inst) {
    const uint32_t *p = inst.operands;
    uint32_t m = register_number(*p);
    Dst d = dst(p);
    auto store = [&](SrcOperand value) {
      write(d, std::nullopt, [&](DstOperand o, bool s) { emit(InstMov{ic(s), o, value}); }, true);
    };

    switch (inst.opcode) {
    case Opcode::Tex: {
      if (ps14_) {
        SrcOperand coord = src(p);
        return store(ps1_sample(m, coord));
      }
      return store(ps1_sample(m, projected_texcoord(m)));
    }
    case Opcode::TexCoord: {
      if (ps14_)
        return store(src(p));
      uint32_t t = scratch();
      emit(InstMov{ic(true), temp_dst(t), texcoord(m)});
      emit(InstMov{ic(), temp_dst(t, 0x8), imm_f(1.0f)});
      return store(temp_src(t));
    }
    case Opcode::TexBem:
    case Opcode::TexBemL: {
      SrcOperand delta = src(p);
      // a projected coordinate is divided before the offset (Wine's visual texbem_test)
      SrcOperand sample = ps1_sample(m, bump(m, projected_texcoord(m), delta));
      if (inst.opcode == Opcode::TexBem)
        return store(sample);
      uint32_t l = scratch();
      emit(InstFloatMAD{
          ic(true), temp_dst(l, 0x1), component(delta, 2), runtime(kRuntimeBumpLuminance + m, replicate(0)),
          runtime(kRuntimeBumpLuminance + m, replicate(1))
      });
      uint32_t t = scratch();
      emit(InstFloatBinaryOp{ic(), FloatBinaryOp::Mul, temp_dst(t), sample, temp_src(l, replicate(0))});
      return store(temp_src(t));
    }
    case Opcode::TexReg2AR:
    case Opcode::TexReg2GB:
    case Opcode::TexReg2RGB: {
      SrcOperand value = src(p);
      auto &c = common(value);
      std::array<uint8_t, 4> perm = inst.opcode == Opcode::TexReg2AR   ? std::array<uint8_t, 4>{3, 0, 0, 0}
                                    : inst.opcode == Opcode::TexReg2GB ? std::array<uint8_t, 4>{1, 2, 2, 2}
                                                                       : std::array<uint8_t, 4>{0, 1, 2, 2};
      return store(ps1_sample(m, with_swizzle(value, compose(c.swizzle, perm))));
    }
    case Opcode::TexM3x2Pad:
    case Opcode::TexM3x3Pad:
      matrix_row(m, src(p));
      return;
    case Opcode::TexM3x2Tex:
    case Opcode::TexM3x2Depth: {
      matrix_row(m, src(p));
      matrix_row_ = 0;
      if (inst.opcode == Opcode::TexM3x2Depth)
        return write_depth(temp_src(reg::PS1Matrix, replicate(0)), temp_src(reg::PS1Matrix, replicate(1)));
      return store(ps1_sample(m, temp_src(reg::PS1Matrix)));
    }
    case Opcode::TexM3x3:
    case Opcode::TexM3x3Tex:
    case Opcode::TexM3x3Spec:
    case Opcode::TexM3x3VSpec: {
      matrix_row(m, src(p));
      matrix_row_ = 0;
      if (inst.opcode == Opcode::TexM3x3) {
        emit(InstMov{ic(), temp_dst(reg::PS1Matrix, 0x8), imm_f(1.0f)});
        return store(temp_src(reg::PS1Matrix));
      }
      if (inst.opcode == Opcode::TexM3x3Tex)
        return store(ps1_sample(m, temp_src(reg::PS1Matrix)));
      SrcOperand eye = temp_src(0);
      if (inst.opcode == Opcode::TexM3x3Spec) {
        eye = src(p);
      } else {
        uint32_t e = scratch();
        for (uint32_t i = 0; i < 3; i++)
          emit(InstMov{ic(), temp_dst(e, 1u << i), texcoord(matrix_stages_[i], replicate(3))});
        eye = temp_src(e);
      }
      return store(ps1_sample(m, reflect(eye)));
    }
    case Opcode::TexDp3Tex:
    case Opcode::TexDp3: {
      SrcOperand value = src(p);
      uint32_t t = scratch();
      emit(InstMov{ic(), temp_dst(t), imm_f(0.0f)});
      emit(InstDotProduct{ic(), temp_dst(t, inst.opcode == Opcode::TexDp3 ? 0xF : 0x1), texcoord(m), value, 3});
      return store(inst.opcode == Opcode::TexDp3 ? temp_src(t) : ps1_sample(m, temp_src(t)));
    }
    case Opcode::TexDepth: {
      SrcOperand r = temp_src(m);
      return write_depth(component(r, 0), component(r, 1));
    }
    case Opcode::Bem: {
      SrcOperand a = src(p), b = src(p);
      return store(bump(m, a, b));
    }
    default:
      fail(std::format("d3d9: unsupported ps_1_x instruction {}", (uint32_t)inst.opcode));
    }
  }

  void
  texkill(const d3d9bc::Instruction &inst) {
    uint32_t token = inst.operands[0];
    uint32_t mask = write_mask(token);
    const uint32_t *p = inst.operands;
    bool is_not;
    // ps_1_x tests xyz, and before ps_1_4 t# names the texture coordinate rather than the texture result
    if (ps1_)
      mask &= 0x7;
    SrcOperand value = ps1_ && !ps14_ && register_type(token) == RegisterType::Addr
                           ? texcoord(register_number(token))
                           : src_register(p, 0, is_not);
    common(value).swizzle = swizzle_identity;
    uint32_t t = scratch();
    emit(InstFloatCompare{
        ic(), FloatComparison::LessThan, temp_dst(t, mask, OperandDataType::Integer), value, imm_f(0.0f)
    });
    uint8_t first = std::countr_zero(mask);
    for (uint8_t c = first + 1; c < 4; c++) {
      if (!(mask & (1 << c)))
        continue;
      emit(InstIntegerBinaryOp{
          IntegerBinaryOp::Or, temp_dst(t, 1u << first, OperandDataType::Integer),
          temp_src(t, replicate(first), OperandDataType::Integer), temp_src(t, replicate(c), OperandDataType::Integer)
      });
    }
    auto discard = fresh("texkill");
    auto after = fresh("after_texkill");
    discard->instructions.push_back(InstPixelDiscard{});
    discard->target = BasicBlockUnconditionalBranch{after};
    cur_->target = BasicBlockConditionalBranch{
        {temp_src(t, replicate(first), OperandDataType::Integer), true}, discard, after
    };
    cur_ = after;
  }

  void
  declaration(const d3d9bc::Instruction &inst) {
    uint32_t usage_token = inst.operands[0];
    uint32_t reg_token = inst.operands[1];
    auto type = register_type(reg_token);
    uint32_t n = register_number(reg_token);
    uint32_t mask = write_mask(reg_token);
    bool centroid = result_modifier(reg_token) & kResultCentroid;
    Usage usage = Usage(usage_token & 0x1F);
    uint32_t usage_index = (usage_token >> 16) & 0xF;

    switch (type) {
    case RegisterType::Sampler:
      sampler_types_[n] = TextureType((usage_token >> 27) & 0xF);
      break;
    case RegisterType::Input:
      if (ps_ && ver_.atLeast(3, 0) && usage == Usage::Position && usage_index == 0) {
        fail("d3d9: ps_3_0 can't declare a position input");
        return;
      }
      if (ps_ && !ver_.atLeast(3, 0)) {
        usage = Usage::Color;
        usage_index = n;
      }
      d_.inputs.push_back({n, usage, usage_index, mask, centroid});
      break;
    case RegisterType::Addr:
      if (ps_)
        d_.inputs.push_back({reg::PSTexCoordBase + n, Usage::TexCoord, n, mask, centroid});
      break;
    case RegisterType::Output:
      d_.outputs.push_back({n, usage, usage_index, mask, false});
      if (usage == Usage::Position && usage_index == 0)
        d_.pos_output_reg = n;
      else if (usage == Usage::Fog)
        d_.fog_output_reg = n;
      else if (usage == Usage::PSize)
        psize_output_reg_ = n;
      break;
    case RegisterType::MiscType:
      if (n == 0) {
        d_.uses_vpos = true;
        emit(InstMov{ic(), temp_dst(reg::VPos), SrcOperandInput{._ = {swizzle_identity, false, false, OperandDataType::Float}, .regid = reg::PSInputVPos}});
        emit(InstFloatBinaryOp{ic(), FloatBinaryOp::Add, temp_dst(reg::VPos, 0x3), temp_src(reg::VPos), imm_f(-0.5f)});
      } else {
        d_.uses_vface = true;
        emit(InstMovConditional{
            ic(), temp_dst(reg::VFace),
            SrcOperandInput{._ = {replicate(0), false, false, OperandDataType::Integer}, .regid = reg::PSInputVFace},
            imm_f(1.0f), imm_f(-1.0f)
        });
      }
      break;
    default:
      break;
    }
  }

  void
  begin_loop(const d3d9bc::Instruction &inst) {
    if (loops_.size() >= reg::MaxLoopDepth) {
      fail("d3d9: loops nested too deeply");
      return;
    }
    const uint32_t *p = inst.operands;
    bool has_al = inst.opcode == Opcode::Loop;
    if (has_al)
      p++;
    SrcOperand counts = src(p);
    common(counts).read_type = OperandDataType::Integer;
    uint32_t depth = loops_.size();
    uint32_t counter = reg::LoopCounterBase + depth;
    emit(InstMov{ic(), temp_dst(counter, 0x1, OperandDataType::Integer), component(counts, 0)});
    if (has_al)
      emit(InstMov{ic(), temp_dst(reg::ALBase + depth, 0x1, OperandDataType::Integer), component(counts, 1)});

    LoopContext loop{fresh("loop_header"), fresh("loop_latch"), fresh("loop_exit"), depth, has_al, component(counts, 2)};
    auto body = fresh("loop_body");
    cur_->target = BasicBlockUnconditionalBranch{loop.header};
    loop.header->target = BasicBlockConditionalBranch{
        {temp_src(counter, replicate(0), OperandDataType::Integer), true}, body, loop.exit
    };
    if (has_al)
      loop.latch->instructions.push_back(InstIntegerBinaryOp{
          IntegerBinaryOp::Add, temp_dst(reg::ALBase + depth, 0x1, OperandDataType::Integer),
          temp_src(reg::ALBase + depth, replicate(0), OperandDataType::Integer), loop.step
      });
    loop.latch->instructions.push_back(InstIntegerBinaryOp{
        IntegerBinaryOp::Add, temp_dst(counter, 0x1, OperandDataType::Integer),
        temp_src(counter, replicate(0), OperandDataType::Integer), imm_u(~0u)
    });
    loop.latch->target = BasicBlockUnconditionalBranch{loop.header};
    loops_.push(loop);
    cur_ = body;
  }

  void
  end_loop() {
    if (loops_.empty()) {
      fail("d3d9: endloop without loop");
      return;
    }
    auto loop = loops_.top();
    loops_.pop();
    cur_->target = BasicBlockUnconditionalBranch{loop.latch};
    cur_ = loop.exit;
  }

  void
  branch_out(std::optional<BasicBlockCondition> cond) {
    if (loops_.empty()) {
      fail("d3d9: break outside of a loop");
      return;
    }
    auto after = fresh("after_break");
    if (cond)
      cur_->target = BasicBlockConditionalBranch{*cond, loops_.top().exit, after};
    else
      cur_->target = BasicBlockUnconditionalBranch{loops_.top().exit};
    cur_ = after;
  }

  void
  begin_if(BasicBlockCondition cond) {
    IfContext ctx{fresh("else"), fresh("endif"), false};
    auto then_bb = fresh("then");
    cur_->target = BasicBlockConditionalBranch{cond, then_bb, ctx.else_bb};
    ifs_.push(ctx);
    cur_ = then_bb;
  }

  void
  call(uint32_t label, std::optional<BasicBlockCondition> cond) {
    auto after = fresh("after_call");
    BasicBlock *call_bb = cur_;
    if (cond) {
      call_bb = fresh("call");
      cur_->target = BasicBlockConditionalBranch{*cond, call_bb, after};
    }
    call_bb->target = BasicBlockCall{label, after};
    call_targets_.push_back({call_bb, &std::get<BasicBlockCall>(call_bb->target)});
    cur_ = after;
  }

  void
  decode(const d3d9bc::Instruction &inst) {
    const uint32_t *p = inst.operands;
    switch (inst.opcode) {
    case Opcode::Nop:
      break;
    case Opcode::Dcl:
      declaration(inst);
      break;
    case Opcode::Def: {
      std::array<uint32_t, 4> v;
      std::memcpy(v.data(), p + 1, 16);
      check_constant(RegisterType::Const, register_number(p[0]));
      defs_f_[register_number(p[0])] = v;
      break;
    }
    case Opcode::DefI: {
      std::array<uint32_t, 4> v;
      std::memcpy(v.data(), p + 1, 16);
      check_constant(RegisterType::ConstInt, register_number(p[0]));
      defs_i_[register_number(p[0])] = v;
      break;
    }
    case Opcode::DefB:
      check_constant(RegisterType::ConstBool, register_number(p[0]));
      defs_b_[register_number(p[0])] = p[1] != 0;
      break;
    case Opcode::Tex:
      if (ps1_)
        ps1_texture(inst);
      else
        texture(inst);
      break;
    case Opcode::TexLdl:
    case Opcode::TexLdd:
      texture(inst);
      break;
    case Opcode::TexKill:
      texkill(inst);
      break;
    case Opcode::If:
      begin_if(bool_condition(p));
      break;
    case Opcode::IfC:
      begin_if(compare_condition(inst, p));
      break;
    case Opcode::Else:
      if (ifs_.empty() || ifs_.top().has_else) {
        fail("d3d9: unexpected else");
        break;
      }
      cur_->target = BasicBlockUnconditionalBranch{ifs_.top().end_bb};
      cur_ = ifs_.top().else_bb;
      ifs_.top().has_else = true;
      break;
    case Opcode::EndIf: {
      if (ifs_.empty()) {
        fail("d3d9: unexpected endif");
        break;
      }
      auto ctx = ifs_.top();
      ifs_.pop();
      cur_->target = BasicBlockUnconditionalBranch{ctx.end_bb};
      if (!ctx.has_else)
        ctx.else_bb->target = BasicBlockUnconditionalBranch{ctx.end_bb};
      cur_ = ctx.end_bb;
      break;
    }
    case Opcode::Loop:
    case Opcode::Rep:
      begin_loop(inst);
      break;
    case Opcode::EndLoop:
    case Opcode::EndRep:
      end_loop();
      break;
    case Opcode::Break:
      branch_out(std::nullopt);
      break;
    case Opcode::BreakC:
      branch_out(compare_condition(inst, p));
      break;
    case Opcode::BreakP:
      branch_out(bool_condition(p));
      break;
    case Opcode::Call:
      call(register_number(p[0]), std::nullopt);
      break;
    case Opcode::CallNz: {
      uint32_t label = register_number(*p++);
      call(label, bool_condition(p));
      break;
    }
    case Opcode::Label: {
      if (std::holds_alternative<BasicBlockUndefined>(cur_->target))
        cur_->target = BasicBlockUnconditionalBranch{func_ret_ ? func_ret_ : ret_main_};
      auto entry = fresh("func_entry");
      func_ret_ = fresh("func_return");
      func_ret_->target = BasicBlockReturn{};
      func_entries_[register_number(p[0])] = entry;
      cur_ = entry;
      break;
    }
    case Opcode::Ret:
      cur_->target = BasicBlockUnconditionalBranch{func_ret_ ? func_ret_ : ret_main_};
      cur_ = fresh("after_ret");
      break;
    case Opcode::Phase:
      break;
    case Opcode::TexCoord:
    case Opcode::TexBem:
    case Opcode::TexBemL:
    case Opcode::TexReg2AR:
    case Opcode::TexReg2GB:
    case Opcode::TexM3x2Pad:
    case Opcode::TexM3x2Tex:
    case Opcode::TexM3x3Pad:
    case Opcode::TexM3x3Tex:
    case Opcode::TexM3x3Spec:
    case Opcode::TexM3x3VSpec:
    case Opcode::TexReg2RGB:
    case Opcode::TexDp3Tex:
    case Opcode::TexM3x2Depth:
    case Opcode::TexDp3:
    case Opcode::TexM3x3:
    case Opcode::TexDepth:
    case Opcode::Bem:
      if (ps1_)
        ps1_texture(inst);
      else
        fail(std::format("d3d9: instruction {} is only valid in ps_1_x", (uint32_t)inst.opcode));
      break;
    case Opcode::TexM3x3Diff:
      fail("d3d9: texm3x3diff is not supported");
      break;
    default:
      alu(inst);
      break;
    }
  }

  SM50ShaderInternal &sh_;
  ShaderData &d_;
  ShaderInfo &info_;
  Version ver_;
  bool ps_;
  bool ps1_;
  bool ps14_;
  uint32_t ps1_texture_types_;
  uint32_t shadow_samplers_;
  uint32_t fetch4_samplers_;
  uint32_t unbound_samplers_;
  uint32_t matrix_row_ = 0;
  uint32_t matrix_stages_[3] = {};
  BasicBlock *cur_ = nullptr;
  BasicBlock *ret_main_ = nullptr;
  BasicBlock *func_ret_ = nullptr;
  std::map<uint32_t, std::array<uint32_t, 4>> defs_f_;
  std::map<uint32_t, std::array<uint32_t, 4>> defs_i_;
  std::map<uint32_t, bool> defs_b_;
  std::map<uint32_t, TextureType> sampler_types_;
  std::stack<LoopContext> loops_;
  std::stack<IfContext> ifs_;
  std::map<uint32_t, BasicBlock *> func_entries_;
  std::vector<std::pair<BasicBlock *, BasicBlockCall *>> call_targets_;
  uint32_t scratch_next_ = 0;
  uint32_t cb_used_ = 0;
  uint32_t psize_output_reg_ = ~0u;
  bool writes_specular_ = false;
  bool pred_not_ = false;
  std::optional<std::string> error_;
};

} // namespace

std::optional<std::string>
decode_shader(
    const uint32_t *tokens, size_t count, SM50ShaderInternal &shader, ShaderData &data, const DecodeOptions &Options
) {
  Reader reader(tokens, count);
  data.version = reader.version();
  if (data.version.pixel) {
    data.pixel_tokens.assign(tokens, tokens + count);
    data.options = Options;
  }
  Decoder decoder(shader, data, data.version, Options);
  return decoder.run(reader);
}

} // namespace mullion::d3d9
