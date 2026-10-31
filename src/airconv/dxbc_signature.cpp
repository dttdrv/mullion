/*
 * Copyright 2026 Feifan He for CodeWeavers
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

#include "dxbc_converter.hpp"
#include <format>

using namespace microsoft;
using namespace mullion::air;
using namespace mullion::shader::common;

namespace mullion::dxbc {

inline air::Interpolation
to_air_interpolation(microsoft::D3D10_SB_INTERPOLATION_MODE mode) {
  using namespace microsoft;
  switch (mode) {
  case D3D10_SB_INTERPOLATION_LINEAR_NOPERSPECTIVE_SAMPLE:
    return air::Interpolation::sample_no_perspective;
  case D3D10_SB_INTERPOLATION_LINEAR_SAMPLE:
    return air::Interpolation::sample_perspective;
  case D3D10_SB_INTERPOLATION_LINEAR_NOPERSPECTIVE_CENTROID:
    return air::Interpolation::centroid_no_perspective;
  case D3D10_SB_INTERPOLATION_LINEAR_CENTROID:
    return air::Interpolation::centroid_perspective;
  case D3D10_SB_INTERPOLATION_CONSTANT:
    return air::Interpolation::flat;
  case D3D10_SB_INTERPOLATION_LINEAR:
    return air::Interpolation::center_perspective;
  case D3D10_SB_INTERPOLATION_LINEAR_NOPERSPECTIVE:
    return air::Interpolation::center_no_perspective;
  default:
    Refuse("an unexpected interpolation mode");
    return air::Interpolation::center_perspective;
  }
}

// an output register's components as varyings, declared once however many elements the register has: `define` takes
// each component's name and type
static void
define_varyings(SM50ShaderInternal *sm50_shader, uint32_t reg, RegisterComponentType declared, auto &&define) {
  if (std::exchange(sm50_shader->varying_registers, sm50_shader->varying_registers | 1u << reg) >> reg & 1)
    return;
  auto types = sm50_shader->register_types(reg, declared);
  for (unsigned c = 0; c < 4; c++)
    define(c, varying_name(reg, c), varying_type(types[c]));
}

// the same from a stage that sets its vertices as a mesh's
static void
define_mesh_varyings(SM50ShaderInternal *sm50_shader, uint32_t reg, RegisterComponentType declared) {
  define_varyings(sm50_shader, reg, declared, [=](unsigned c, std::string name, air::MSLScalerOrVectorType type) {
    auto index = sm50_shader->num_mesh_vertex_data++;
    sm50_shader->func_signature.DefineMeshVertexOutput(OutputMeshData{.user = name, .type = type, .index = index});
    sm50_shader->mesh_output_handlers.push_back([=](MeshOutputContext &output) -> IREffect {
      return pop_mesh_output_vertex_data(reg, 1 << c, index, output.vertex_id, type);
    });
  });
}

void handle_signature_vs(
  CSignatureParser &inputParser, const CSignatureParser &outputParser,
  D3D10ShaderBinary::CInstruction &Inst, SM50ShaderInternal *sm50_shader,
  uint32_t phase_unused
) {
  uint32_t &max_input_register = sm50_shader->max_input_register;
  uint32_t &max_output_register = sm50_shader->max_output_register;
  auto &signature_handlers = sm50_shader->signature_handlers;
  auto &func_signature = sm50_shader->func_signature;
  auto varyings = [&](uint32_t reg, RegisterComponentType declared) {
    define_varyings(sm50_shader, reg, declared, [&](unsigned c, std::string name, air::MSLScalerOrVectorType type) {
      uint32_t assigned_index = func_signature.DefineOutput(OutputVertex{.user = name, .type = type});
      signature_handlers.push_back([=](SignatureContext &ctx) {
        if (ctx.skip_vertex_output)
          return;
        ctx.epilogue >> pop_output_reg(reg, 1 << c, assigned_index);
      });
    });
  };

  auto findInputElement = [&](auto matcher) -> Signature {
    const D3D11_SIGNATURE_PARAMETER *parameters;
    inputParser.GetParameters(&parameters);
    for (unsigned i = 0; i < inputParser.GetNumParameters(); i++) {
      auto sig = Signature(parameters[i]);
      if (matcher(sig)) {
        return sig;
      }
    }
    return Signature();
  };
  auto findOutputElement = [&](auto matcher) -> Signature {
    const D3D11_SIGNATURE_PARAMETER *parameters;
    outputParser.GetParameters(&parameters);
    for (unsigned i = 0; i < outputParser.GetNumParameters(); i++) {
      auto sig = Signature(parameters[i]);
      if (matcher(sig)) {
        return sig;
      }
    }
    return Signature();
  };

  switch (Inst.m_OpCode) {
  case D3D10_SB_OPCODE_DCL_INPUT_SGV: {
    unsigned reg = Inst.m_Operands[0].m_Index[0].m_RegIndex;
    auto mask = Inst.m_Operands[0].m_WriteMask >> 4;
    // auto MinPrecision = Inst.m_Operands[0].m_MinPrecision; // not used
    auto sgv = Inst.m_InputDeclSGV.Name;
    switch (sgv) {
    case D3D10_SB_NAME_VERTEX_ID: {
      signature_handlers.push_back([=](SignatureContext &ctx) {
        ctx.prologue << make_effect_bind([=](struct context ctx) {
          return store_at_vec4_array_masked(
            ctx.resource.input.ptr_int4, ctx.builder.getInt32(reg),
            ctx.resource.vertex_id, mask
          );
        });
      });
      break;
    }
    case D3D10_SB_NAME_INSTANCE_ID: {
      signature_handlers.push_back([=](SignatureContext &ctx) {
        ctx.prologue << make_effect_bind([=](struct context ctx) {
          return store_at_vec4_array_masked(
            ctx.resource.input.ptr_int4, ctx.builder.getInt32(reg),
            ctx.resource.instance_id, mask
          );
        });
      });
      break;
    }
    default:
      // Unexpected/unhandled input system value: the declaration is ignored
      break;
    }
    max_input_register = std::max(reg + 1, max_input_register);
    break;
  }
  case D3D10_SB_OPCODE_DCL_INPUT: {
    D3D10_SB_OPERAND_TYPE RegType = Inst.m_Operands[0].m_Type;

    switch (RegType) {
    case D3D10_SB_OPERAND_TYPE_INPUT: {
      unsigned reg = Inst.m_Operands[0].m_Index[0].m_RegIndex;
      assert(Inst.m_Operands[0].m_IndexDimension == D3D10_SB_OPERAND_INDEX_1D);

      auto mask = Inst.m_Operands[0].m_WriteMask >> 4;
      auto sig = findInputElement([=](Signature &sig) {
        return (sig.reg() == reg) && ((sig.mask() & mask) != 0);
      });
      max_input_register = std::max(reg + 1, max_input_register);
      if (sig.mask() == 0) break;
      signature_handlers.push_back(
        [=, type = (InputAttributeComponentType)sig.componentType(),
         name = sig.consistentAttributeName()](SignatureContext &ctx) {
          if (ctx.ia_layout) {
            for (unsigned i = 0; i < ctx.ia_layout->num_elements; i++) {
              if (ctx.ia_layout->elements[i].reg == reg) {
                ctx.prologue << pull_vertex_input(
                  ctx.func_signature, reg, mask, ctx.ia_layout->elements[i],
                  ctx.ia_layout->slot_mask
                );
                break;
              }
            }
          } else {
            auto assigned_index = ctx.func_signature.DefineInput(
              InputVertexStageIn{.attribute = reg, .type = type, .name = name}
            );
            ctx.prologue << init_input_reg(assigned_index, reg, mask);
          }
        }
      );
      break;
    }
    default:
      // unhandled vertex shader input: the declaration is ignored
      break;
    }
    break;
  }
  case D3D10_SB_OPCODE_DCL_OUTPUT_SIV: {
    unsigned reg = Inst.m_Operands[0].m_Index[0].m_RegIndex;
    auto mask = Inst.m_Operands[0].m_WriteMask >> 4;
    auto siv = Inst.m_OutputDeclSIV.Name;
    switch (siv) {
    case D3D10_SB_NAME_CLIP_DISTANCE: {
      for (unsigned i = 0; i < 4; i++) {
        if (mask & (1 << i)) {
          sm50_shader->clip_distance_scalars.push_back(
            {.component = (uint8_t)(i), .reg = (uint8_t)reg}
          );
        }
      }
      varyings(reg, RegisterComponentType::Float);
      max_output_register = std::max(reg + 1, max_output_register);
      break;
    }
    case D3D10_SB_NAME_CULL_DISTANCE: {
      for (unsigned i = 0; i < 4; i++) {
        if (mask & (1 << i)) {
          sm50_shader->cull_distance_scalars.push_back(
            {.component = (uint8_t)(i), .reg = (uint8_t)reg}
          );
        }
      }
      varyings(reg, RegisterComponentType::Float);
      max_output_register = std::max(reg + 1, max_output_register);
      break;
    }
    case D3D10_SB_NAME_POSITION: {
      auto assigned_index =
        func_signature.DefineOutput(OutputPosition{.type = msl_float4});
      signature_handlers.push_back([=](SignatureContext &ctx) {
        if (ctx.skip_vertex_output)
          return;
        ctx.epilogue >> pop_output_reg_sanitize_pos(reg, mask, assigned_index);
      });
      break;
    }
    case D3D10_SB_NAME_RENDER_TARGET_ARRAY_INDEX: {
      auto assigned_index =
        func_signature.DefineOutput(OutputRenderTargetArrayIndex{});
      signature_handlers.push_back([=](SignatureContext &ctx) {
        if (ctx.skip_vertex_output)
          return;
        ctx.epilogue >> pop_output_reg(reg, mask, assigned_index);
      });
      break;
    }
    case D3D10_SB_NAME_VIEWPORT_ARRAY_INDEX: {
      auto assigned_index =
        func_signature.DefineOutput(OutputViewportArrayIndex{});
      signature_handlers.push_back([=](SignatureContext &ctx) {
        if (ctx.skip_vertex_output)
          return;
        ctx.epilogue >> pop_output_reg(reg, mask, assigned_index);
      });
      break;
    }
    // a varying of the pixel shader's coarse shading variant (SM50_SHADER_PSO_PIXEL_SHADER_DATA's shading_rate_varying)
    case D3D12_SB_NAME_SHADING_RATE:
      varyings(reg, RegisterComponentType::Uint);
      break;
    default:
      // Unexpected/unhandled input system value: the declaration is ignored
      break;
    }
    max_output_register = std::max(reg + 1, max_output_register);
    break;
  }
  case D3D10_SB_OPCODE_DCL_OUTPUT: {
    D3D10_SB_OPERAND_TYPE RegType = Inst.m_Operands[0].m_Type;
    switch (RegType) {
    case D3D10_SB_OPERAND_TYPE_OUTPUT: {
      // normal output register
      auto reg = Inst.m_Operands[0].m_Index[0].m_RegIndex;
      auto mask = Inst.m_Operands[0].m_WriteMask >> 4;
      auto sig = findOutputElement([=](Signature sig) {
        return (sig.reg() == reg) && ((sig.mask() & mask) != 0);
      });
      max_output_register = std::max(reg + 1, max_output_register);
      if (sig.mask() == 0) break;
      varyings(reg, sig.componentType());
      break;
    }
    default:
      // unhandled vertex shader output: the declaration is ignored
      break;
    }
    break;
  }
  default:
    // an unexpected signature declaration: the declaration is ignored
    break;
  }
};

void define_barycentrics(
  SM50ShaderInternal *sm50_shader, uint32_t element, D3D10_SB_INTERPOLATION_MODE mode, bool evaluated
) {
  auto interpolation = to_air_interpolation(mode);
  bool perspective = interpolation == Interpolation::center_perspective ||
                     interpolation == Interpolation::centroid_perspective ||
                     interpolation == Interpolation::sample_perspective;
  bool sample = interpolation == Interpolation::sample_perspective || interpolation == Interpolation::sample_no_perspective;
  bool centroid =
      interpolation == Interpolation::centroid_perspective || interpolation == Interpolation::centroid_no_perspective;
  sm50_shader->shader_info.use_barycentrics = true;
  sm50_shader->ps_fine_shading |= sample || evaluated;
  sm50_shader->signature_handlers.push_back([=](SignatureContext &ctx) {
    bool rows = ctx.barycentric_rows, stepped = sample || centroid || evaluated;
    std::array<uint32_t, 3> coords{rows ? ~0u : ctx.func_signature.DefineInput(InputBarycentricCoord{.perspective = perspective})};
    for (uint32_t k = 0; rows && k < 3; k++)
      coords[k] = ctx.func_signature.DefineInput(InputFragmentStageIn{
          .user = "mullion_barycentrics" + std::to_string((perspective ? 0 : 3) + k),
          .type = msl_float,
          .interpolation = Interpolation::center_no_perspective,
          .pull_mode = false
      });
    auto position = stepped && perspective && !rows ? ctx.func_signature.DefineInput(InputPosition{Interpolation::center_no_perspective}) : ~0u;
    auto sample_index = sample ? ctx.func_signature.DefineInput(InputSampleIndex{}) : ~0u;
    auto coverage = centroid || evaluated ? ctx.func_signature.DefineInput(InputInputCoverage{}) : ~0u;
    ctx.prologue << make_effect([=](struct context ctx) {
      auto &ir = ctx.builder;
      auto &b = ctx.resource.barycentrics[element];
      b.perspective = perspective;
      if (rows) {
        // already linear in the window, times 1/w where perspective
        b.center = llvm::ConstantAggregateZero::get(llvm::FixedVectorType::get(ir.getFloatTy(), 3));
        for (uint32_t k = 0; k < 3; k++)
          b.center = ir.CreateInsertElement(b.center, ctx.function->getArg(coords[k]), k);
        b.value = b.At(ir, nullptr);
      } else {
        b.value = b.center = ctx.function->getArg(coords[0]);
      }
      if (!stepped)
        return std::monostate{};
      if (perspective && !rows) {
        // Metal's position is where its own interpolation takes it, with 1/w there in w: 1/w is affine on the screen
        // too, so a step back along its derivatives gives the center's
        auto mtl_position = ctx.function->getArg(position);
        auto w = ir.CreateExtractElement(mtl_position, 3);
        auto xy = ir.CreateShuffleVector(mtl_position, {0, 1});
        auto off = ir.CreateFSub(ir.CreateFSub(xy, ctx.air.CreateFPUnOp(llvm::air::AIRBuilder::floor, xy)), ctx.air.getFloat2(0.5f, 0.5f));
        auto at_center = ir.CreateFSub(
            w, ir.CreateFAdd(
                   ir.CreateFMul(ctx.air.CreateDerivative(w, false), ir.CreateExtractElement(off, uint64_t(0))),
                   ir.CreateFMul(ctx.air.CreateDerivative(w, true), ir.CreateExtractElement(off, 1))
               )
        );
        b.center = ir.CreateFMul(b.center, ir.CreateVectorSplat(3, at_center));
      }
      b.ddx = ctx.air.CreateDerivative(b.center, false);
      b.ddy = ctx.air.CreateDerivative(b.center, true);
      if (~coverage)
        b.centroid = ctx.air.CreateCentroidOffset(ctx.function->getArg(coverage));
      if (sample)
        b.value = b.At(ir, ctx.air.CreateRasterizerSamplePos(ctx.function->getArg(sample_index)));
      else if (centroid)
        b.value = b.At(ir, b.centroid);
      return std::monostate{};
    });
  });
}

void handle_signature_ps(
  CSignatureParser &inputParser, const CSignatureParser &outputParser,
  D3D10ShaderBinary::CInstruction &Inst, SM50ShaderInternal *sm50_shader,
  uint32_t phase_unused
) {
  uint32_t &max_input_register = sm50_shader->max_input_register;
  uint32_t &max_output_register = sm50_shader->max_output_register;
  uint32_t &pso_valid_output_reg_mask = sm50_shader->pso_valid_output_reg_mask;
  auto &signature_handlers = sm50_shader->signature_handlers;
  auto &func_signature = sm50_shader->func_signature;
  // the components of an input register the shader declares, as varyings
  auto varyings = [&](uint32_t reg, uint32_t mask, RegisterComponentType type, air::Interpolation interpolation) {
    if (interpolation == air::Interpolation::flat || type != RegisterComponentType::Float)
      sm50_shader->ps_flat_input_mask |= 1u << reg;
    else if (interpolation == air::Interpolation::center_no_perspective ||
             interpolation == air::Interpolation::centroid_no_perspective ||
             interpolation == air::Interpolation::sample_no_perspective)
      sm50_shader->ps_noperspective_input_mask |= 1u << reg;
    signature_handlers.push_back([=](SignatureContext &ctx) {
      bool centroid = interpolation == air::Interpolation::centroid_perspective ||
                      interpolation == air::Interpolation::centroid_no_perspective;
      bool pull_mode = (bool(ctx.pull_mode_reg_mask & (1 << reg)) || (ctx.pull_centroids && centroid)) &&
                       interpolation != air::Interpolation::flat;
      bool vertex_value = sm50_shader->shader_info.vertex_value_reg_mask >> reg & 1;
      for (unsigned c = 0; c < 4; c++) {
        if (!(mask >> c & 1))
          continue;
        auto assigned_index = ctx.func_signature.DefineInput(InputFragmentStageIn{
          .user = varying_name(reg, c), .type = varying_type(type), .interpolation = interpolation, .pull_mode = pull_mode,
          .vertex_value = vertex_value
        });
        if (vertex_value) {
          auto &values = ctx.resource.vertex_value_map[reg];
          values.component[c] = assigned_index;
          values.integers |= (type != RegisterComponentType::Float) << c;
          ctx.prologue << init_input_reg_at_vertex(assigned_index, reg, 1 << c, type != RegisterComponentType::Float);
          continue;
        }
        if (!pull_mode) {
          ctx.prologue << init_input_reg(assigned_index, reg, 1 << c);
          continue;
        }
        auto &interpolant = ctx.resource.interpolant_map[reg];
        interpolant.component[c] = assigned_index;
        interpolant.perspective = interpolation == air::Interpolation::sample_perspective ||
                                  interpolation == air::Interpolation::center_perspective ||
                                  interpolation == air::Interpolation::centroid_perspective;
        uint32_t sampleidx_at = ~0u;
        switch (interpolation) {
        case Interpolation::sample_perspective:
        case Interpolation::sample_no_perspective:
          sampleidx_at = ctx.func_signature.DefineInput(InputSampleIndex{});
          break;
        default:
          break;
        }
        ctx.prologue << init_input_reg_with_interpolation(assigned_index, reg, 1 << c, interpolation, sampleidx_at);
      }
    });
  };

  auto findInputElement = [&](auto matcher) -> Signature {
    const D3D11_SIGNATURE_PARAMETER *parameters;
    inputParser.GetParameters(&parameters);
    for (unsigned i = 0; i < inputParser.GetNumParameters(); i++) {
      auto sig = Signature(parameters[i]);
      if (matcher(sig)) {
        return sig;
      }
    }
    return Signature();
  };
  auto findOutputElement = [&](auto matcher) -> Signature {
    const D3D11_SIGNATURE_PARAMETER *parameters;
    outputParser.GetParameters(&parameters);
    for (unsigned i = 0; i < outputParser.GetNumParameters(); i++) {
      auto sig = Signature(parameters[i]);
      if (matcher(sig)) {
        return sig;
      }
    }
    return Signature();
  };

  switch (Inst.m_OpCode) {
  case D3D10_SB_OPCODE_DCL_INPUT_SIV: {
    // an unexpected signature declaration: the declaration is ignored
    break;
  }
  case D3D10_SB_OPCODE_DCL_INPUT_SGV:
    // an unexpected signature declaration: the declaration is ignored
    break;
  case D3D10_SB_OPCODE_DCL_INPUT: {
    D3D10_SB_OPERAND_TYPE RegType = Inst.m_Operands[0].m_Type;

    switch (RegType) {
    case D3D11_SB_OPERAND_TYPE_INPUT_COVERAGE_MASK: {
      auto assigned_index = func_signature.DefineInput(InputInputCoverage{});
      sm50_shader->ps_has_coverage_input = 1;
      signature_handlers.push_back([=](SignatureContext &ctx) {
        ctx.prologue << make_effect([=](struct context ctx) {
          auto attr = ctx.function->getArg(assigned_index);
          ctx.resource.coverage_mask_arg = attr;
          return std::monostate{};
        });
      });
      break;
    }
    case D3D11_SB_OPERAND_TYPE_INNER_COVERAGE:
      // the conservative rasterizer's pixel test has it (conservative_overlap)
      break;
    default:
      // unimplemented vertex input: the declaration is ignored
      break;
    }
    break;
  }
  case D3D10_SB_OPCODE_DCL_INPUT_PS_SIV: {
    unsigned reg = Inst.m_Operands[0].m_Index[0].m_RegIndex;
    auto mask = Inst.m_Operands[0].m_WriteMask >> 4;
    auto siv = Inst.m_InputPSDeclSIV.Name;
    auto interpolation =
      to_air_interpolation(Inst.m_InputPSDeclSIV.InterpolationMode);
    uint32_t assigned_index, sample_index_at = ~0u;
    sm50_shader->ps_fine_shading |=
        interpolation == Interpolation::sample_perspective || interpolation == Interpolation::sample_no_perspective;
    switch (siv) {
    case D3D10_SB_NAME_POSITION:
      if (interpolation == Interpolation::sample_no_perspective) {
        sample_index_at = func_signature.DefineInput(InputSampleIndex{});
        sm50_shader->shader_info.use_samplepos = true;
        interpolation = Interpolation::center_no_perspective;
      }
      // assert(
      //   interpolation == Interpolation::center_no_perspective ||
      //   // in case it's per-sample, FIXME: will this cause problem?
      //   interpolation == Interpolation::sample_no_perspective
      // );
      assigned_index = func_signature.DefineInput(
        // the only supported interpolation for [[position]]
        InputPosition{.interpolation = interpolation}
      );
      break;
    case D3D10_SB_NAME_RENDER_TARGET_ARRAY_INDEX:
      assert(interpolation == Interpolation::flat);
      assigned_index =
        func_signature.DefineInput(InputRenderTargetArrayIndex{});
      break;
    case D3D10_SB_NAME_VIEWPORT_ARRAY_INDEX:
      assert(interpolation == Interpolation::flat);
      assigned_index = func_signature.DefineInput(InputViewportArrayIndex{});
      break;
    case D3D10_SB_NAME_CLIP_DISTANCE:
    case D3D10_SB_NAME_CULL_DISTANCE:
      varyings(reg, mask, RegisterComponentType::Float, interpolation);
      max_input_register = std::max(reg + 1, max_input_register);
      return;
    default:
      // Unexpected/unhandled input system value: the declaration is ignored
      break;
    }
    signature_handlers.push_back([=](SignatureContext &ctx) {
      ctx.prologue << init_input_reg(
        assigned_index, reg, mask, siv == D3D10_SB_NAME_POSITION, sample_index_at
      );
    });
    max_input_register = std::max(reg + 1, max_input_register);
    break;
  }
  case D3D10_SB_OPCODE_DCL_INPUT_PS_SGV: {
    unsigned reg = Inst.m_Operands[0].m_Index[0].m_RegIndex;
    auto mask = Inst.m_Operands[0].m_WriteMask >> 4;
    auto siv = Inst.m_InputPSDeclSIV.Name;
    // these are not interpolated, so the declared mode is ignored: Wine's d3dcompiler_47 leaves it undefined
    uint32_t assigned_index;
    switch (siv) {
    case microsoft::D3D10_SB_NAME_IS_FRONT_FACE:
      assigned_index = func_signature.DefineInput(InputFrontFacing{});
      break;
    case microsoft::D3D10_SB_NAME_SAMPLE_INDEX:
      assigned_index = func_signature.DefineInput(InputSampleIndex{});
      sm50_shader->ps_fine_shading = true;
      break;
    case microsoft::D3D10_SB_NAME_PRIMITIVE_ID:
      assigned_index = func_signature.DefineInput(InputPrimitiveID{});
      break;
    // the coarse pixel's rate, which the coarse shading variant finds (io_binding_map's shading_rate), 1x1 elsewhere
    case D3D12_SB_NAME_SHADING_RATE:
      signature_handlers.push_back([=](SignatureContext &ctx) {
        ctx.prologue << make_effect_bind([=](struct context ctx) {
          auto rate = ctx.resource.shading_rate ? ctx.resource.shading_rate : ctx.builder.getInt32(0);
          return store_at_vec4_array_masked(ctx.resource.input.ptr_int4, ctx.builder.getInt32(reg), rate, mask);
        });
      });
      max_input_register = std::max(reg + 1, max_input_register);
      return;
    default:
      // Unexpected/unhandled input system value: the declaration is ignored
      break;
    }
    signature_handlers.push_back([=](SignatureContext &ctx) {
      ctx.prologue << init_input_reg(assigned_index, reg, mask);
    });
    max_input_register = std::max(reg + 1, max_input_register);
    break;
  }
  case D3D10_SB_OPCODE_DCL_INPUT_PS: {
    unsigned reg = Inst.m_Operands[0].m_Index[0].m_RegIndex;
    auto mask = Inst.m_Operands[0].m_WriteMask >> 4;
    auto interpolation =
      to_air_interpolation(Inst.m_InputPSDecl.InterpolationMode);
    auto sig = findInputElement([=](Signature sig) {
      return (sig.reg() == reg) && ((sig.mask() & mask) != 0);
    });
    max_input_register = std::max(reg + 1, max_input_register);
    if (sig.mask() == 0) break;
    varyings(reg, mask, sig.componentType(), interpolation);
    sm50_shader->ps_fine_shading |=
        interpolation == Interpolation::sample_perspective || interpolation == Interpolation::sample_no_perspective;
    break;
  }
  case D3D10_SB_OPCODE_DCL_OUTPUT_SGV: {
    // dcl_output_sgv should not happen for now: the declaration is ignored
    // only GS PrimitiveID uses this, but we don't support GS for now
    break;
  }
  case D3D10_SB_OPCODE_DCL_OUTPUT_SIV: {
    unsigned reg = Inst.m_Operands[0].m_Index[0].m_RegIndex;
    // auto mask = Inst.m_Operands[0].m_WriteMask >> 4;
    auto siv = Inst.m_OutputDeclSIV.Name;
    switch (siv) {
    default:
      // Unexpected/unhandled input system value: the declaration is ignored
      break;
    }
    max_output_register = std::max(reg + 1, max_output_register);
    break;
  }
  case D3D10_SB_OPCODE_DCL_OUTPUT: {
    D3D10_SB_OPERAND_TYPE RegType = Inst.m_Operands[0].m_Type;
    switch (RegType) {
    case D3D10_SB_OPERAND_TYPE_OUTPUT_DEPTH:
    case D3D11_SB_OPERAND_TYPE_OUTPUT_DEPTH_GREATER_EQUAL:
    case D3D11_SB_OPERAND_TYPE_OUTPUT_DEPTH_LESS_EQUAL: {
      sm50_shader->ps_fine_shading = true;
      sm50_shader->ps_depth_output = true;
      signature_handlers.push_back([=](SignatureContext &ctx) {
        ctx.prologue << make_effect([](struct context ctx) -> std::monostate {
          assert(
            ctx.resource.depth_output_reg == nullptr &&
            "otherwise oDepth is defined twice"
          );
          ctx.resource.depth_output_reg =
            ctx.builder.CreateAlloca(ctx.types._float);
          return {};
        });
      });
      signature_handlers.push_back([=](SignatureContext &ctx) {
        if (ctx.disable_depth_output)
          return;
        auto assigned_index = ctx.func_signature.DefineOutput(OutputDepth{
          .depth_argument =
            RegType == D3D11_SB_OPERAND_TYPE_OUTPUT_DEPTH_GREATER_EQUAL
              ? DepthArgument::greater
            : RegType == D3D11_SB_OPERAND_TYPE_OUTPUT_DEPTH_LESS_EQUAL
              ? DepthArgument::less
              : DepthArgument::any
        });
        // a written depth is clamped to its viewport's range (D3D11.3 17.10), and NaN to its MinDepth, which Metal
        // does only in its clamp mode and discards
        auto ranges = ctx.func_signature.DefineInput(ArgumentBindingBuffer{
          .location_index = SM50_BINDING_INDEX_DEPTH_RANGES,
          .array_size = 1,
          .memory_access = MemoryAccess::read,
          .address_space = AddressSpace::constant,
          .type = msl_float,
          .arg_name = "depth_ranges",
        });
        auto viewport = ctx.func_signature.DefineInput(InputViewportArrayIndex{});
        ctx.epilogue >> [=](pvalue v) {
          return make_irvalue([=](struct context ctx) {
            auto &ir = ctx.builder;
            auto range = [&](unsigned end) {
              auto index = ir.CreateAdd(ir.CreateShl(ctx.function->getArg(viewport), 1), ir.getInt32(end));
              return ir.CreateLoad(ctx.types._float, ir.CreateGEP(ctx.types._float, ctx.function->getArg(ranges), index));
            };
            llvm::Value *depth = ir.CreateLoad(
              ctx.types._float, ir.CreateConstInBoundsGEP1_32(ctx.types._float, ctx.resource.depth_output_reg, 0)
            );
            using llvm::air::AIRBuilder;
            depth = ctx.air.CreateFPBinOp(
              AIRBuilder::fmin, range(1), ctx.air.CreateFPBinOp(AIRBuilder::fmax, range(0), depth, false), false
            );
            return ir.CreateInsertValue(v, depth, {assigned_index});
          });
        };
      });
      break;
    }
    case D3D11_SB_OPERAND_TYPE_OUTPUT_STENCIL_REF: {
      sm50_shader->ps_fine_shading = true;
      signature_handlers.push_back([=](SignatureContext &ctx) {
        ctx.prologue << make_effect([](struct context ctx) -> std::monostate {
          assert(ctx.resource.stencil_ref_reg == nullptr && "otherwise oStencil is defined twice");
          ctx.resource.stencil_ref_reg = ctx.builder.CreateAlloca(ctx.types._int);
          return {};
        });
      });
      auto assigned_index = func_signature.DefineOutput(OutputStencilRef{});
      signature_handlers.push_back([=](SignatureContext &ctx) {
        ctx.epilogue >> [=](pvalue v) {
          return make_irvalue([=](struct context ctx) {
            auto ostencil = ctx.builder.CreateLoad(
                ctx.types._int, ctx.builder.CreateConstInBoundsGEP1_32(ctx.types._int, ctx.resource.stencil_ref_reg, 0)
            );
            return ctx.builder.CreateInsertValue(v, ostencil, {assigned_index});
          });
        };
      });
      break;
    }
    case D3D10_SB_OPERAND_TYPE_OUTPUT_COVERAGE_MASK: {
      signature_handlers.push_back([=](SignatureContext &ctx) {
        ctx.prologue << make_effect([](struct context ctx) -> std::monostate {
          assert(
            ctx.resource.coverage_mask_reg == nullptr &&
            "otherwise oMask is defined twice"
          );
          ctx.resource.coverage_mask_reg =
            ctx.builder.CreateAlloca(ctx.types._int);
          return {};
        });
      });
      auto assigned_index = func_signature.DefineOutput(OutputCoverageMask{});
      signature_handlers.push_back([=](SignatureContext &ctx) {
        ctx.epilogue >> [=](pvalue v) {
          return make_irvalue([=](struct context ctx) {
            llvm::Value *odepth = ctx.builder.CreateLoad(
              ctx.types._int,
              ctx.builder.CreateConstInBoundsGEP1_32(
                ctx.types._int, ctx.resource.coverage_mask_reg, 0
              )
            );
            if (ctx.resource.kept_samples)
              odepth = ctx.builder.CreateAnd(odepth, ctx.resource.kept_samples);
            return ctx.builder.CreateInsertValue(
              v,
              ctx.pso_sample_mask != 0xffffffff
                ? ctx.builder.CreateAnd(odepth, ctx.pso_sample_mask)
                : odepth,
              {assigned_index}
            );
          });
        };
      });
      sm50_shader->ps_has_coverage_output = 1;
      break;
    }

    case D3D10_SB_OPERAND_TYPE_OUTPUT: {
      // normal output register
      auto reg = Inst.m_Operands[0].m_Index[0].m_RegIndex;
      auto mask = Inst.m_Operands[0].m_WriteMask >> 4;
      auto sig = findOutputElement([=](Signature sig) {
        return (sig.reg() == reg) && ((sig.mask() & mask) != 0);
      });
      max_output_register = std::max(reg + 1, max_output_register);
      pso_valid_output_reg_mask |= (1 << reg);
      if (sig.mask() == 0) break;
      auto sig_type = sig.componentType();
      if (sig_type != RegisterComponentType::Float && sig_type != RegisterComponentType::Uint &&
          sig_type != RegisterComponentType::Int) {
        Refuse("a pixel shader output of an unknown component type");
        break;
      }
      signature_handlers.push_back([=](SignatureContext &ctx) {
        auto format = uint32_t(ctx.pixel_formats[reg]);
        auto type = component_type_from_pixel_format(air::MTLPixelFormat(format & 0xff));
        if (type == RegisterComponentType::Unknown)
          type = sig_type;
        uint32_t assigned_index;
        if (ctx.dual_source_blending) {
          if (reg > 1 || (reg == 1 && ctx.shader_blends_dual_source))
            return;
          assigned_index = ctx.func_signature.DefineOutput(OutputRenderTarget{
            .dual_source_blending = true,
            .index = reg,
            .type = to_msl_type(type),
          });
        } else {
          assigned_index = ctx.func_signature.DefineOutput(OutputRenderTarget{
            .dual_source_blending = false,
            .index = reg,
            .type = to_msl_type(type),
          });
        }
        if (type == RegisterComponentType::Float && ctx.unorm_output_reg_mask & (1 << reg))
          ctx.epilogue >> pop_output_reg_fix_unorm(reg, mask, assigned_index);
        else if (format & SM50_PIXEL_FORMAT_CHANNELS)
          ctx.epilogue >> pop_output_reg(reg, mask, assigned_index,
                                         {int(SM50_PIXEL_FORMAT_CHANNEL(format, 0)), int(SM50_PIXEL_FORMAT_CHANNEL(format, 1)),
                                          int(SM50_PIXEL_FORMAT_CHANNEL(format, 2)), int(SM50_PIXEL_FORMAT_CHANNEL(format, 3))});
        else
          ctx.epilogue >> pop_output_reg(reg, mask, assigned_index);
      });
      break;
    }
    default:
      // unhandled pixel shader output: the declaration is ignored
      break;
    }
    break;
  }
  default:
    // unhandled pixel shader output: the declaration is ignored
    break;
  }
};

void handle_signature_hs(
  CSignatureParser &inputParser, const CSignatureParser &outputParser,
  D3D10ShaderBinary::CInstruction &Inst, SM50ShaderInternal *sm50_shader,
  uint32_t phase
) {
  uint32_t &max_input_register = sm50_shader->max_input_register;
  uint32_t &max_output_register = sm50_shader->max_output_register;
  uint32_t &max_patch_constant_output_register =
    sm50_shader->max_patch_constant_output_register;

  switch (Inst.m_OpCode) {
  case D3D10_SB_OPCODE_DCL_INPUT_SGV: {
    unsigned reg = Inst.m_Operands[0].m_Index[0].m_RegIndex;
    // auto mask = Inst.m_Operands[0].m_WriteMask >> 4;
    // auto MinPrecision = Inst.m_Operands[0].m_MinPrecision; // not used
    auto sgv = Inst.m_InputDeclSGV.Name;
    switch (sgv) {
    default:
      // Unexpected/unhandled hull shader sgv: the declaration is ignored
      break;
    }
    max_input_register = std::max(reg + 1, max_input_register);
    break;
  }
  case D3D10_SB_OPCODE_DCL_INPUT: {
    D3D10_SB_OPERAND_TYPE RegType = Inst.m_Operands[0].m_Type;

    switch (RegType) {
    case D3D11_SB_OPERAND_TYPE_OUTPUT_CONTROL_POINT_ID:
    case D3D11_SB_OPERAND_TYPE_INPUT_FORK_INSTANCE_ID:
    case D3D11_SB_OPERAND_TYPE_INPUT_JOIN_INSTANCE_ID:
    case D3D10_SB_OPERAND_TYPE_INPUT_PRIMITIVEID: {
      break;
    }
    case D3D10_SB_OPERAND_TYPE_INPUT: {
      break;
    }
    case D3D11_SB_OPERAND_TYPE_INPUT_PATCH_CONSTANT: {
      break;
    }
    case D3D11_SB_OPERAND_TYPE_INPUT_CONTROL_POINT: {
      break;
    }
    case D3D11_SB_OPERAND_TYPE_OUTPUT_CONTROL_POINT: {
      sm50_shader->shader_info.output_control_point_read = true;
      break;
    }

    default:
      // unhandled hull shader input: the declaration is ignored
      break;
    }
    break;
  }
  case D3D10_SB_OPCODE_DCL_OUTPUT_SGV: {
    // dcl_output_sgv should not happen for now: the declaration is ignored
    // only GS PrimitiveID uses this, but we don't support GS for now
    break;
  }
  case D3D10_SB_OPCODE_DCL_OUTPUT_SIV: {
    unsigned reg = Inst.m_Operands[0].m_Index[0].m_RegIndex;
    auto mask = Inst.m_Operands[0].m_WriteMask >> 4;
    auto siv = Inst.m_OutputDeclSIV.Name;
    switch (siv) {
    case D3D11_SB_NAME_FINAL_QUAD_U_EQ_0_EDGE_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_QUAD_V_EQ_0_EDGE_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_QUAD_U_EQ_1_EDGE_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_QUAD_V_EQ_1_EDGE_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_QUAD_U_INSIDE_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_QUAD_V_INSIDE_TESSFACTOR: {
      for (unsigned i = 0; i < 4; i++) {
        if (mask & (1 << i)) {
          sm50_shader->patch_constant_scalars.push_back(
              {.component = (uint8_t)(i),
               .reg = (uint8_t)reg,
               .tess_factor_index = (int8_t)(siv - D3D11_SB_NAME_FINAL_QUAD_U_EQ_0_EDGE_TESSFACTOR)}
          );
        }
      }
      break;
    }
    case D3D11_SB_NAME_FINAL_TRI_U_EQ_0_EDGE_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_TRI_V_EQ_0_EDGE_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_TRI_W_EQ_0_EDGE_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_TRI_INSIDE_TESSFACTOR: {
      for (unsigned i = 0; i < 4; i++) {
        if (mask & (1 << i)) {
          sm50_shader->patch_constant_scalars.push_back(
              {.component = (uint8_t)(i),
               .reg = (uint8_t)reg,
               .tess_factor_index = (int8_t)(siv - D3D11_SB_NAME_FINAL_TRI_U_EQ_0_EDGE_TESSFACTOR)}
          );
        }
      }
      break;
    }
    case D3D11_SB_NAME_FINAL_LINE_DENSITY_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_LINE_DETAIL_TESSFACTOR: {
      for (unsigned i = 0; i < 4; i++) {
        if (mask & (1 << i)) {
          sm50_shader->patch_constant_scalars.push_back(
              {.component = (uint8_t)(i),
               .reg = (uint8_t)reg,
               .tess_factor_index = (int8_t)(siv - D3D11_SB_NAME_FINAL_LINE_DETAIL_TESSFACTOR)}
          );
        }
      }
      break;
    }
    default:
      // Unexpected/unhandled hull shader output siv: the declaration is ignored
      break;
    }
    if (phase != ~0u) {
      max_patch_constant_output_register =
        std::max(reg + 1, max_patch_constant_output_register);
    } else {
      max_output_register = std::max(reg + 1, max_output_register);
    }
    break;
  }
  case D3D10_SB_OPCODE_DCL_OUTPUT: {
    D3D10_SB_OPERAND_TYPE RegType = Inst.m_Operands[0].m_Type;
    switch (RegType) {
    case D3D10_SB_OPERAND_TYPE_OUTPUT: {
      // normal output register
      auto reg = Inst.m_Operands[0].m_Index[0].m_RegIndex;
      auto mask = Inst.m_Operands[0].m_WriteMask >> 4;
      if (phase != ~0u) {
        assert(
          Inst.m_Operands[0].m_IndexDimension == D3D10_SB_OPERAND_INDEX_1D
        );
        for (unsigned i = 0; i < 4; i++) {
          if (mask & (1 << i)) {
            sm50_shader->patch_constant_scalars.push_back(
              {.component = (uint8_t)(i), .reg = (uint8_t)reg, .tess_factor_index = -1}
            );
          }
        }
        max_patch_constant_output_register =
          std::max(reg + 1, max_patch_constant_output_register);
      } else {
        max_output_register = std::max(reg + 1, max_output_register);
      }
      break;
    }
    default:
      // unhandled hull shader output: the declaration is ignored
      break;
    }
    break;
  }
  default:
    // an unexpected signature declaration: the declaration is ignored
    break;
  }
};

void handle_signature_ds(
  CSignatureParser &inputParser, const CSignatureParser &outputParser,
  D3D10ShaderBinary::CInstruction &Inst, SM50ShaderInternal *sm50_shader,
  uint32_t phase_unused
) {
  uint32_t &max_input_register = sm50_shader->max_input_register;
  uint32_t &max_output_register = sm50_shader->max_output_register;
  auto &func_signature = sm50_shader->func_signature;
  auto &mesh_output_handlers = sm50_shader->mesh_output_handlers;

  auto findOutputElement = [&](auto matcher) -> Signature {
    const D3D11_SIGNATURE_PARAMETER *parameters;
    outputParser.GetParameters(&parameters);
    for (unsigned i = 0; i < outputParser.GetNumParameters(); i++) {
      auto sig = Signature(parameters[i]);
      if (matcher(sig)) {
        return sig;
      }
    }
    return Signature();
  };

  switch (Inst.m_OpCode) {
  case D3D10_SB_OPCODE_DCL_INPUT_SIV: {
    unsigned reg = Inst.m_Operands[0].m_Index[0].m_RegIndex;
    auto siv = Inst.m_OutputDeclSIV.Name;
    switch (siv) {
    case D3D11_SB_NAME_FINAL_QUAD_U_EQ_0_EDGE_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_QUAD_V_EQ_0_EDGE_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_QUAD_U_EQ_1_EDGE_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_QUAD_V_EQ_1_EDGE_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_QUAD_U_INSIDE_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_QUAD_V_INSIDE_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_TRI_U_EQ_0_EDGE_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_TRI_V_EQ_0_EDGE_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_TRI_W_EQ_0_EDGE_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_TRI_INSIDE_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_LINE_DENSITY_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_LINE_DETAIL_TESSFACTOR: {
      max_input_register = std::max(reg + 1, max_input_register);
      break;
    }
    default:
      // unhandled dcl_input_siv in domain shader: the declaration is ignored
      break;
    }

    break;
  }
  case D3D10_SB_OPCODE_DCL_INPUT_SGV: {
    // an unexpected signature declaration: the declaration is ignored
    break;
  }
  case D3D10_SB_OPCODE_DCL_INPUT: {
    D3D10_SB_OPERAND_TYPE RegType = Inst.m_Operands[0].m_Type;

    switch (RegType) {
    case D3D11_SB_OPERAND_TYPE_INPUT_DOMAIN_POINT: {
      // should be defined in convert_dxbc_tessellator_domain_shader()
      break;
    }
    case D3D10_SB_OPERAND_TYPE_INPUT_PRIMITIVEID: {
      // `convert_dxbc_domain_shader()` will always define [[patch_id]] and initialize `io_binding_map.patch_id` field
      break;
    }
    case D3D10_SB_OPERAND_TYPE_INPUT:
      // an unexpected signature declaration: the declaration is ignored
      break;
    case D3D11_SB_OPERAND_TYPE_INPUT_PATCH_CONSTANT: {
      unsigned reg = Inst.m_Operands[0].m_Index[0].m_RegIndex;
      assert(Inst.m_Operands[0].m_IndexDimension == D3D10_SB_OPERAND_INDEX_1D);
      max_input_register = std::max(reg + 1, max_input_register);
      break;
    }
    case D3D11_SB_OPERAND_TYPE_INPUT_CONTROL_POINT:
      break;
    default:
      // unhandeld domain shader input: the declaration is ignored
      break;
    }
    break;
  }
  case D3D10_SB_OPCODE_DCL_OUTPUT_SGV: {
    // dcl_output_sgv should not happen for now: the declaration is ignored
    // only GS PrimitiveID uses this, but we don't support GS for now
    break;
  }
  case D3D10_SB_OPCODE_DCL_OUTPUT_SIV: {
    unsigned reg = Inst.m_Operands[0].m_Index[0].m_RegIndex;
    auto mask = Inst.m_Operands[0].m_WriteMask >> 4;
    auto siv = Inst.m_OutputDeclSIV.Name;
    switch (siv) {
    case D3D10_SB_NAME_CLIP_DISTANCE: {
      for (unsigned i = 0; i < 4; i++) {
        if (mask & (1 << i)) {
          sm50_shader->clip_distance_scalars.push_back(
            {.component = (uint8_t)(i), .reg = (uint8_t)reg}
          );
        }
      }
      define_mesh_varyings(sm50_shader, reg, RegisterComponentType::Float);
      max_output_register = std::max(reg + 1, max_output_register);
      break;
    }
    case D3D10_SB_NAME_CULL_DISTANCE: {
      for (unsigned i = 0; i < 4; i++) {
        if (mask & (1 << i)) {
          sm50_shader->cull_distance_scalars.push_back(
            {.component = (uint8_t)(i), .reg = (uint8_t)reg}
          );
        }
      }
      define_mesh_varyings(sm50_shader, reg, RegisterComponentType::Float);
      max_output_register = std::max(reg + 1, max_output_register);
      break;
    }
    case D3D10_SB_NAME_POSITION: {
      func_signature.DefineMeshVertexOutput(OutputPosition{.type = msl_float4});
      mesh_output_handlers.push_back([=](MeshOutputContext& output) -> IREffect {
        return pop_mesh_output_position(reg, mask, output.vertex_id);
      });
      break;
    }
    case D3D10_SB_NAME_RENDER_TARGET_ARRAY_INDEX: {
      func_signature.DefineMeshPrimitiveOutput(OutputRenderTargetArrayIndex{});
      mesh_output_handlers.push_back([=](MeshOutputContext& output) -> IREffect {
        return pop_mesh_output_render_target_array_index(reg, mask, output.primitive_id);
      });
      break;
    }
    case D3D10_SB_NAME_VIEWPORT_ARRAY_INDEX: {
      func_signature.DefineMeshPrimitiveOutput(OutputViewportArrayIndex{});
      mesh_output_handlers.push_back([=](MeshOutputContext& output) -> IREffect {
        return pop_mesh_output_viewport_array_index(reg, mask, output.primitive_id);
      });
      break;
    }
    default:
      // Unexpected/unhandled input system value: the declaration is ignored
      break;
    }
    max_output_register = std::max(reg + 1, max_output_register);
    break;
  }
  case D3D10_SB_OPCODE_DCL_OUTPUT: {
    D3D10_SB_OPERAND_TYPE RegType = Inst.m_Operands[0].m_Type;
    switch (RegType) {
    case D3D10_SB_OPERAND_TYPE_OUTPUT: {
      // normal output register
      auto reg = Inst.m_Operands[0].m_Index[0].m_RegIndex;
      auto mask = Inst.m_Operands[0].m_WriteMask >> 4;
      auto sig = findOutputElement([=](Signature sig) {
        return (sig.reg() == reg) && ((sig.mask() & mask) != 0);
      });
      max_output_register = std::max(reg + 1, max_output_register);
      if (sig.mask() == 0) break;
      define_mesh_varyings(sm50_shader, reg, sig.componentType());
      break;
    }
    default:
      // unhandled domain shader output: the declaration is ignored
      break;
    }
    break;
  }
  default:
    // an unexpected signature declaration: the declaration is ignored
    break;
  }
};

void handle_signature_cs(
  CSignatureParser &inputParser_unused, const CSignatureParser &outputParser_unused,
  D3D10ShaderBinary::CInstruction &Inst, SM50ShaderInternal *sm50_shader,
  uint32_t phase_unused
) {
  auto &signature_handlers = sm50_shader->signature_handlers;
  auto &func_signature = sm50_shader->func_signature;

  switch (Inst.m_OpCode) {
  case D3D10_SB_OPCODE_DCL_INPUT_SGV: {
    // unhandled compute shader sgv: the declaration is ignored
    break;
  }
  case D3D10_SB_OPCODE_DCL_INPUT: {
    D3D10_SB_OPERAND_TYPE RegType = Inst.m_Operands[0].m_Type;

    switch (RegType) {
    case D3D11_SB_OPERAND_TYPE_INPUT_THREAD_ID: {
      sm50_shader->reads_dispatch_position = true;
      auto assigned_index =
        func_signature.DefineInput(InputThreadPositionInGrid{});
      signature_handlers.push_back([=](SignatureContext &ctx) {
        ctx.prologue << make_effect([=](struct context ctx) {
          auto attr = ctx.function->getArg(assigned_index);
          ctx.resource.thread_id_arg = attr;
          return std::monostate{};
        });
      });
      break;
    }
    case D3D11_SB_OPERAND_TYPE_INPUT_THREAD_GROUP_ID: {
      sm50_shader->reads_dispatch_position = true;
      auto assigned_index =
        func_signature.DefineInput(InputThreadgroupPositionInGrid{});
      signature_handlers.push_back([=](SignatureContext &ctx) {
        ctx.prologue << make_effect([=](struct context ctx) {
          auto attr = ctx.function->getArg(assigned_index);
          ctx.resource.thread_group_id_arg = attr;
          return std::monostate{};
        });
      });
      break;
    }
    case D3D11_SB_OPERAND_TYPE_INPUT_THREAD_ID_IN_GROUP: {
      auto assigned_index =
        func_signature.DefineInput(InputThreadPositionInThreadgroup{});
      signature_handlers.push_back([=](SignatureContext &ctx) {
        ctx.prologue << make_effect([=](struct context ctx) {
          auto attr = ctx.function->getArg(assigned_index);
          ctx.resource.thread_id_in_group_arg = attr;
          return std::monostate{};
        });
      });
      break;
    }
    case D3D11_SB_OPERAND_TYPE_INPUT_THREAD_ID_IN_GROUP_FLATTENED: {
      auto assigned_index =
        func_signature.DefineInput(InputThreadIndexInThreadgroup{});
      signature_handlers.push_back([=](SignatureContext &ctx) {
        ctx.prologue << make_effect([=](struct context ctx) {
          auto attr = ctx.function->getArg(assigned_index);
          ctx.resource.thread_id_in_group_flat_arg = attr;
          return std::monostate{};
        });
      });
      break;
    }
    default:
      // unhandled compute shader input: the declaration is ignored
      break;
    }
    break;
  }
  default:
    // an unexpected signature declaration: the declaration is ignored
    break;
  }
};

void
handle_signature_gs(
    CSignatureParser &inputParser, const CSignatureParser &outputParser, D3D10ShaderBinary::CInstruction &Inst,
    SM50ShaderInternal *sm50_shader, uint32_t phase
) {
  uint32_t &max_output_register = sm50_shader->max_output_register;
  auto &func_signature = sm50_shader->func_signature;
  auto &mesh_output_handlers = sm50_shader->mesh_output_handlers;

  auto findOutputElement = [&](auto matcher) -> Signature {
    const D3D11_SIGNATURE_PARAMETER *parameters;
    outputParser.GetParameters(&parameters);
    for (unsigned i = 0; i < outputParser.GetNumParameters(); i++) {
      auto sig = Signature(parameters[i]);
      if (matcher(sig)) {
        return sig;
      }
    }
    return Signature();
  };

  switch (Inst.m_OpCode) {
  case D3D10_SB_OPCODE_DCL_INPUT_SGV: {
    // auto mask = Inst.m_Operands[0].m_WriteMask >> 4;
    // auto MinPrecision = Inst.m_Operands[0].m_MinPrecision; // not used
    auto sgv = Inst.m_InputDeclSGV.Name;
    switch (sgv) {
    default:
      // Unexpected/unhandled geometry shader sgv: the declaration is ignored
      break;
    }
    break;
  }
  case D3D10_SB_OPCODE_DCL_INPUT_SIV: {
    D3D10_SB_NAME siv = Inst.m_InputDeclSIV.Name;

    switch (siv) {
    case D3D10_SB_NAME_POSITION:
    case D3D10_SB_NAME_CULL_DISTANCE:
    case D3D10_SB_NAME_CLIP_DISTANCE:
      break;
    default:
      // Unexpected/unhandled geometry shader siv: the declaration is ignored
      break;
    }
    break;
  }
  case D3D10_SB_OPCODE_DCL_INPUT: {
    D3D10_SB_OPERAND_TYPE RegType = Inst.m_Operands[0].m_Type;

    switch (RegType) {
    case microsoft::D3D10_SB_OPERAND_TYPE_INPUT: {
      assert(Inst.m_Operands[0].m_IndexDimension == microsoft::D3D10_SB_OPERAND_INDEX_2D);
      break;
    }
    case microsoft::D3D10_SB_OPERAND_TYPE_INPUT_PRIMITIVEID:
    case microsoft::D3D11_SB_OPERAND_TYPE_INPUT_GS_INSTANCE_ID:
      break;
    default:
      // unhandled geometry shader input: the declaration is ignored
      break;
    }
    break;
  }
  // DXBC declares the primitive ID a geometry shader outputs as system generated, DXIL as the others
  case D3D10_SB_OPCODE_DCL_OUTPUT_SGV:
  case D3D10_SB_OPCODE_DCL_OUTPUT_SIV: {
    unsigned reg = Inst.m_Operands[0].m_Index[0].m_RegIndex;
    auto mask = Inst.m_Operands[0].m_WriteMask >> 4;
    auto siv = Inst.m_OpCode == D3D10_SB_OPCODE_DCL_OUTPUT_SGV ? Inst.m_OutputDeclSGV.Name : Inst.m_OutputDeclSIV.Name;
    switch (siv) {
    // what the pixel shader reads as the primitive ID (D3D11.3 8.17), a primitive's from its leading vertex
    case D3D10_SB_NAME_PRIMITIVE_ID: {
      func_signature.DefineMeshPrimitiveOutput(OutputPrimitiveID{});
      mesh_output_handlers.push_back([=](MeshOutputContext& output) -> IREffect {
        return pop_mesh_output_primitive_id(reg, mask, output.primitive_id);
      });
      break;
    }
    case D3D10_SB_NAME_CLIP_DISTANCE: {
      for (unsigned i = 0; i < 4; i++) {
        if (mask & (1 << i)) {
          sm50_shader->clip_distance_scalars.push_back({.component = (uint8_t)(i), .reg = (uint8_t)reg});
        }
      }
      define_mesh_varyings(sm50_shader, reg, RegisterComponentType::Float);
      max_output_register = std::max(reg + 1, max_output_register);
      break;
    }
    case D3D10_SB_NAME_CULL_DISTANCE: {
      for (unsigned i = 0; i < 4; i++) {
        if (mask & (1 << i)) {
          sm50_shader->cull_distance_scalars.push_back({.component = (uint8_t)(i), .reg = (uint8_t)reg});
        }
      }
      define_mesh_varyings(sm50_shader, reg, RegisterComponentType::Float);
      max_output_register = std::max(reg + 1, max_output_register);
      break;
    }
    case D3D10_SB_NAME_POSITION: {
      func_signature.DefineMeshVertexOutput(OutputPosition{.type = msl_float4});
      mesh_output_handlers.push_back([=](MeshOutputContext& output) -> IREffect {
        return pop_mesh_output_position(reg, mask, output.vertex_id);
      });
      break;
    }
    case D3D10_SB_NAME_RENDER_TARGET_ARRAY_INDEX: {
      func_signature.DefineMeshPrimitiveOutput(OutputRenderTargetArrayIndex{});
      mesh_output_handlers.push_back([=](MeshOutputContext& output) -> IREffect {
        return pop_mesh_output_render_target_array_index(reg, mask, output.primitive_id);
      });
      break;
    }
    case D3D10_SB_NAME_VIEWPORT_ARRAY_INDEX: {
      func_signature.DefineMeshPrimitiveOutput(OutputViewportArrayIndex{});
      mesh_output_handlers.push_back([=](MeshOutputContext& output) -> IREffect {
        return pop_mesh_output_viewport_array_index(reg, mask, output.primitive_id);
      });
      break;
    }
    // a varying, as the vertex shader's: flat in the pixel shader, so the primitive's first vertex's
    case D3D12_SB_NAME_SHADING_RATE:
      define_mesh_varyings(sm50_shader, reg, RegisterComponentType::Uint);
      break;
    default:
      // Unexpected/unhandled geometry shader output siv: the declaration is ignored
      break;
    }
    max_output_register = std::max(reg + 1, max_output_register);
    break;
  }
  case D3D10_SB_OPCODE_DCL_OUTPUT: {
    D3D10_SB_OPERAND_TYPE RegType = Inst.m_Operands[0].m_Type;
    switch (RegType) {
    case D3D10_SB_OPERAND_TYPE_OUTPUT: {
      // normal output register
      auto reg = Inst.m_Operands[0].m_Index[0].m_RegIndex;
      auto mask = Inst.m_Operands[0].m_WriteMask >> 4;
      auto sig = findOutputElement([=](Signature sig) {
        return (sig.reg() == reg) && ((sig.mask() & mask) != 0);
      });
      max_output_register = std::max(reg + 1, max_output_register);
      if (sig.mask() == 0) break;
      define_mesh_varyings(sm50_shader, reg, sig.componentType());
      break;
    }
    default:
      // unhandled geometry shader output: the declaration is ignored
      break;
    }
    break;
  }
  default:
    // an unexpected signature declaration: the declaration is ignored
    break;
  }
};

// a compute shader whose dispatch may come in parts (groups_work_together, device_atomics64) and that reads where its
// thread or group is in the dispatch: each part's groups are numbered from the stage-in grid's origin, which is in
// groups
void declare_dispatch_origin(SM50ShaderInternal *sm50_shader) {
  if (sm50_shader->shader_type != microsoft::D3D11_SB_COMPUTE_SHADER ||
      !(sm50_shader->groups_work_together || sm50_shader->device_atomics64) || !sm50_shader->reads_dispatch_position)
    return;
  auto index = sm50_shader->func_signature.DefineInput(InputStageInGridOrigin{});
  sm50_shader->signature_handlers.push_back([=](SignatureContext &ctx) {
    ctx.prologue << make_effect([=](struct context ctx) {
      auto origin = ctx.function->getArg(index);
      auto &res = ctx.resource;
      if (res.thread_group_id_arg)
        res.thread_group_id_arg = ctx.builder.CreateAdd(res.thread_group_id_arg, origin);
      if (res.thread_id_arg)
        res.thread_id_arg = ctx.builder.CreateAdd(
            res.thread_id_arg,
            ctx.builder.CreateMul(
                origin, llvm::ConstantDataVector::get(ctx.llvm, llvm::ArrayRef<uint32_t>(sm50_shader->threadgroup_size))
            )
        );
      return std::monostate{};
    });
  });
}

void handle_signature(
  microsoft::CSignatureParser &inputParser,
  microsoft::CSignatureParser5 &outputParser,
  microsoft::D3D10ShaderBinary::CInstruction &Inst, SM50ShaderInternal *shader,
  uint32_t phase
) {
  switch (shader->shader_type) {
  case D3D10_SB_PIXEL_SHADER:
    return handle_signature_ps(inputParser, *outputParser.Signature(0), Inst, shader, phase);
  case D3D10_SB_VERTEX_SHADER:
    return handle_signature_vs(inputParser, *outputParser.Signature(0), Inst, shader, phase);
  case D3D11_SB_HULL_SHADER:
    return handle_signature_hs(inputParser, *outputParser.Signature(0), Inst, shader, phase);
  case D3D11_SB_DOMAIN_SHADER:
    return handle_signature_ds(inputParser, *outputParser.Signature(0), Inst, shader, phase);
  case D3D11_SB_COMPUTE_SHADER:
    return handle_signature_cs(inputParser, *outputParser.Signature(0), Inst, shader, phase);
  case D3D10_SB_GEOMETRY_SHADER:
    return handle_signature_gs(inputParser, *outputParser.Signature(0), Inst, shader, phase);
  default:
    // an unexpected signature declaration: the declaration is ignored
    break;
  }
}

}; // namespace mullion::dxbc