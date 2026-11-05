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
#include "airconv_error.hpp"
#include "dxbc_converter.hpp"
#include "nt/dxbc_io_helper.hpp"
#include "llvm/Support/raw_ostream.h"
#include <bit>

namespace mullion::d3d9 {

using namespace dxbc;
using namespace air;
using d3d9bc::Usage;

namespace {

SrcOperand
temp_src(uint32_t reg, Swizzle swz = swizzle_identity, OperandDataType type = OperandDataType::Float) {
  return SrcOperandTemp{._ = {swz, false, false, type}, .regid = reg, .phase = ~0u};
}

DstOperand
temp_dst(uint32_t reg, uint32_t mask, OperandDataType type = OperandDataType::Float) {
  return DstOperandTemp{._ = {mask, type}, .regid = reg, .phase = ~0u};
}

SrcOperand
runtime_src(uint32_t index, Swizzle swz = swizzle_identity) {
  return SrcOperandConstantBuffer{
      ._ = {swz, false, false, OperandDataType::Float}, .rangeid = cb::Runtime, .rangeindex = cb::Runtime,
      .regindex = index
  };
}

SrcOperand
imm(float v) {
  SrcOperandImmediate32 imm{._ = {swizzle_identity, false, false, OperandDataType::Float}};
  for (auto &f : imm.fvalue)
    f = v;
  return imm;
}

SrcOperand
neg(SrcOperand src) {
  std::visit([](auto &o) { o._.neg = !o._.neg; }, src);
  return src;
}

Swizzle
rep(uint8_t c) {
  return {c, c, c, c};
}

// D3DCMPFUNC
constexpr uint32_t D3DCMP_NEVER_VALUE = 1;
constexpr uint32_t D3DCMP_LESS_VALUE = 2;
constexpr uint32_t D3DCMP_EQUAL_VALUE = 3;
constexpr uint32_t D3DCMP_LESSEQUAL_VALUE = 4;
constexpr uint32_t D3DCMP_GREATER_VALUE = 5;
constexpr uint32_t D3DCMP_NOTEQUAL_VALUE = 6;
constexpr uint32_t D3DCMP_ALWAYS_VALUE = 8;

constexpr InstructionCommon kNoSat = {false, 0};
constexpr InstructionCommon kSat = {true, 0};

struct Driver {
  SM50ShaderInternal *shader;
  ShaderData &data;
  llvm::LLVMContext &context;
  llvm::Module &module;
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *args;

  FunctionSignatureBuilder func_signature;
  Prologues prologue;
  Epilogues epilogue;
  io_binding_map resource_map;
  AirType types;
  SM50_SHADER_METAL_VERSION metal_version = SM50_SHADER_METAL_310;
  SM30_SHADER_D3D9_DATA variant = {};
  std::vector<std::unique_ptr<BasicBlock>> epilogue_blocks;
  SM50_SHADER_FLAG shader_flags = {};
  uint32_t max_output_register;

  Driver(
      SM50ShaderInternal *shader, llvm::LLVMContext &context, llvm::Module &module,
      SM50_SHADER_COMPILATION_ARGUMENT_DATA *args
  ) :
      shader(shader),
      data(*shader->d3d9),
      context(context),
      module(module),
      args(args),
      func_signature(shader->func_signature),
      types(context),
      max_output_register(shader->max_output_register) {
    SM50_SHADER_COMMON_DATA *common = nullptr;
    if (args_get_data<SM50_SHADER_COMMON, SM50_SHADER_COMMON_DATA>(args, &common)) {
      metal_version = common->metal_version;
      shader_flags = common->flags;
    }
    SM30_SHADER_D3D9_DATA *d3d9 = nullptr;
    if (args_get_data<SM30_SHADER_D3D9, SM30_SHADER_D3D9_DATA>(args, &d3d9))
      variant = *d3d9;
    if (!data.version.pixel)
      variant.fog_mode = SM30_FOG_NONE;
  }

  void
  defineVertexInputs() {
    SM50_SHADER_IA_INPUT_LAYOUT_DATA *ia_layout = nullptr;
    args_get_data<SM50_SHADER_IA_INPUT_LAYOUT, SM50_SHADER_IA_INPUT_LAYOUT_DATA>(args, &ia_layout);
    uint32_t vbuf_table = ~0u;
    for (auto &input : data.inputs) {
      const SM50_IA_INPUT_ELEMENT *element = nullptr;
      for (unsigned i = 0; ia_layout && i < ia_layout->num_elements; i++)
        if (ia_layout->elements[i].reg == input.reg)
          element = &ia_layout->elements[i];
      uint32_t reg = input.reg;
      if (element) {
        if (vbuf_table == ~0u)
          vbuf_table = func_signature.DefineInput(ArgumentBindingBuffer{
              .buffer_size = {},
              .location_index = SM50_BINDING_INDEX_VERTEX_BUFFER,
              .array_size = 0,
              .memory_access = MemoryAccess::read,
              .address_space = AddressSpace::constant,
              .type = msl_uint,
              .arg_name = "vertex_buffers",
              .raster_order_group = {},
          });
        auto info = *element;
        uint32_t fixup = info.format >> 16;
        info.format &= 0xFFFF;
        auto slot_mask = ia_layout->slot_mask;
        prologue.Add([=](IOHelper &io) -> llvm::Error {
          if (auto err = io.PullVertexInput(vbuf_table, reg, 0xF, info, slot_mask))
            return err;
          return io.FixupVertexInput(reg, fixup);
        });
      } else if (!ia_layout) {
        auto index = func_signature.DefineInput(InputVertexStageIn{
            .attribute = reg, .type = InputAttributeComponentType::Float, .name = input.attributeName()
        });
        prologue.Add([=](IOHelper &io) -> llvm::Error { return io.InitializeInput(index, reg, 0xF); });
      }
    }
  }

  void
  defineVertexOutputs() {
    auto has = [&](Usage usage, uint32_t index) {
      for (auto &out : data.outputs)
        if (out.usage == usage && out.usage_index == index)
          return true;
      return false;
    };

    for (auto &out : data.outputs) {
      uint32_t reg = out.reg;
      if (out.usage == Usage::Position && out.usage_index == 0) {
        auto index = func_signature.DefineOutput(OutputPosition{.type = msl_float4});
        epilogue.Add([=](llvm::Value *ret, IOHelper &io) -> llvm::Expected<llvm::Value *> {
          return io.PopulateOutputSanitizePosition(reg, 0xF, ret, index);
        });
        continue;
      }
      if (out.usage == Usage::PSize)
        continue;
      auto index = func_signature.DefineOutput(OutputVertex{.user = out.attributeName(), .type = msl_float4});
      epilogue.Add([=](llvm::Value *ret, IOHelper &io) -> llvm::Expected<llvm::Value *> {
        return io.PopulateOutput(reg, 0xF, ret, index);
      });
    }

    // pixel shaders before ps_3_0 read colors and texture coordinates the vertex shader may not write; Metal
    // refuses to link a fragment input with no matching vertex output, so emit zeros for the missing ones.
    uint32_t fill_reg = shader->max_output_register;
    auto fill = [&](Usage usage, uint32_t usage_index) {
      if (has(usage, usage_index))
        return;
      IOElement element{fill_reg++, usage, usage_index, 0xF, false};
      uint32_t reg = element.reg;
      auto index = func_signature.DefineOutput(OutputVertex{.user = element.attributeName(), .type = msl_float4});
      epilogue.Add([=](llvm::Value *ret, IOHelper &io) -> llvm::Expected<llvm::Value *> {
        return io.PopulateOutput(reg, 0xF, ret, index);
      });
    };
    for (uint32_t i = 0; i < 2; i++)
      fill(Usage::Color, i);
    for (uint32_t i = 0; i < 8; i++)
      fill(Usage::TexCoord, i);
    fill(Usage::Fog, 0);
    max_output_register = std::max(fill_reg, reg::VSOutCount);

    if (variant.flags & SM30_SHADER_FLAG_POINT_SIZE) {
      auto index = func_signature.DefineOutput(OutputPointSize{});
      epilogue.Add([=](llvm::Value *ret, IOHelper &io) -> llvm::Expected<llvm::Value *> {
        return io.PopulateOutput(reg::VSOutPointSize, 0x1, ret, index);
      });
    }
  }

  void
  definePixelInputs() {
    bool flat = variant.flags & SM30_SHADER_FLAG_FLAT_SHADING;
    bool point_sprite = variant.flags & SM30_SHADER_FLAG_POINT_SPRITE;
    bool known = variant.flags & SM30_SHADER_FLAG_VS_OUTPUTS;
    for (auto &input : data.inputs) {
      uint32_t reg = input.reg;
      // Metal can't link an input nothing writes; the zero-initialized register stands in, as in D3D9
      uint32_t bit = (uint32_t)input.usage * 16 + input.usage_index;
      if (known && bit < 224 && !(variant.vs_outputs[bit / 32] & (1u << (bit % 32))))
        continue;
      if (point_sprite && input.usage == Usage::TexCoord) {
        auto index = func_signature.DefineInput(InputPointCoord{});
        prologue.Add([=](IOHelper &io) -> llvm::Error { return io.InitializeInput(index, reg, 0x3); });
        continue;
      }
      auto interpolation = input.centroid ? Interpolation::centroid_perspective : Interpolation::center_perspective;
      if (flat && input.usage == Usage::Color)
        interpolation = Interpolation::flat;
      auto index = func_signature.DefineInput(InputFragmentStageIn{
          .user = input.attributeName(),
          .type = msl_float4,
          .interpolation = interpolation,
          .pull_mode = false,
      });
      prologue.Add([=](IOHelper &io) -> llvm::Error { return io.InitializeInput(index, reg, 0xF); });
    }
    if (variant.fog_mode == SM30_FOG_VERTEX) {
      auto index = func_signature.DefineInput(InputFragmentStageIn{
          .user = IOElement{0, Usage::Fog, 0, 0xF, false}.attributeName(),
          .type = msl_float4,
          .interpolation = Interpolation::center_perspective,
          .pull_mode = false,
      });
      prologue.Add([=](IOHelper &io) -> llvm::Error { return io.InitializeInput(index, reg::PSInputFog, 0xF); });
    }
    if (data.uses_vpos || variant.fog_mode >= SM30_FOG_EXP || biasesDepth()) {
      auto index = func_signature.DefineInput(InputPosition{.interpolation = Interpolation::center_no_perspective});
      prologue.Add([=](IOHelper &io) -> llvm::Error { return io.InitializeInput(index, reg::PSInputVPos, 0xF); });
    }
    if (data.uses_vface) {
      auto index = func_signature.DefineInput(InputFrontFacing{});
      prologue.Add([=](IOHelper &io) -> llvm::Error { return io.InitializeInput(index, reg::PSInputVFace, 0x1); });
    }
  }

  void
  definePixelOutputs() {
    SM50_SHADER_PSO_PIXEL_SHADER_DATA *pso = nullptr;
    args_get_data<SM50_SHADER_PSO_PIXEL_SHADER, SM50_SHADER_PSO_PIXEL_SHADER_DATA>(args, &pso);

    for (uint32_t rt = 0; rt < 4; rt++) {
      if (!(data.color_output_mask & (1 << rt)))
        continue;
      auto type = pso ? component_type_from_pixel_format((MTLPixelFormat)pso->pixel_formats[rt])
                      : RegisterComponentType::Unknown;
      if (type == RegisterComponentType::Unknown)
        type = RegisterComponentType::Float;
      auto index = func_signature.DefineOutput(
          OutputRenderTarget{.dual_source_blending = false, .index = rt, .type = to_msl_type(type)}
      );
      bool fix_unorm = pso && type == RegisterComponentType::Float && (pso->unorm_output_reg_mask & (1 << rt));
      epilogue.Add([=](llvm::Value *ret, IOHelper &io) -> llvm::Expected<llvm::Value *> {
        return fix_unorm ? io.PopulateOutputFixUnorm(rt, 0xF, ret, index) : io.PopulateOutput(rt, 0xF, ret, index);
      });
    }

    if (data.writes_depth || biasesDepth()) {
      prologue.Add([](IOHelper &io) -> llvm::Error { return io.CreateDepthOutputRegister(); });
      if (!pso || !pso->disable_depth_output) {
        auto index = func_signature.DefineOutput(OutputDepth{.depth_argument = DepthArgument::any});
        epilogue.Add([=](llvm::Value *ret, IOHelper &io) -> llvm::Expected<llvm::Value *> {
          return io.PopulateOutputDepth(ret, index);
        });
      }
    }

    if (pso && pso->sample_mask != 0xffffffff) {
      auto sample_mask = pso->sample_mask;
      auto index = func_signature.DefineOutput(OutputCoverageMask{});
      epilogue.Add([=](llvm::Value *ret, IOHelper &io) -> llvm::Expected<llvm::Value *> {
        return io.PopulateDefaultCoverageMask(ret, index, sample_mask);
      });
    }
  }

  bool
  biasesDepth() const {
    return data.version.pixel && (variant.flags & SM30_SHADER_FLAG_DEPTH_BIAS) && !data.writes_depth;
  }

  BasicBlock *
  newBlock(const char *name) {
    epilogue_blocks.push_back(std::make_unique<BasicBlock>(name));
    return epilogue_blocks.back().get();
  }

  // compiled-in D3D9 fixed-function steps, spliced between the shader body and the output copy.
  void
  buildEpilogueBlocks() {
    BasicBlock *cur = newBlock("d3d9_epilogue");
    BasicBlock *first = cur;
    uint32_t e = reg::EpilogueBase;
    auto discard_unless = [&](BasicBlockCondition pass) {
      auto discard = newBlock("d3d9_alpha_discard");
      auto next = newBlock("d3d9_epilogue_next");
      discard->instructions.push_back(InstPixelDiscard{});
      discard->target = BasicBlockUnconditionalBranch{next};
      cur->target = BasicBlockConditionalBranch{pass, next, discard};
      cur = next;
    };

    bool color0 = data.color_output_mask & 1;
    if (data.version.pixel && color0 && variant.alpha_func && variant.alpha_func != D3DCMP_ALWAYS_VALUE) {
      SrcOperand a = temp_src(reg::OutColorBase, rep(3)), ref = runtime_src(0, rep(0));
      auto mask = temp_dst(e, 0x1, OperandDataType::Integer);
      switch (variant.alpha_func) {
      case D3DCMP_NEVER_VALUE:
        cur->instructions.push_back(InstMov{kNoSat, mask, SrcOperandImmediate32{._ = {swizzle_identity, false, false, OperandDataType::Integer}, .uvalue = {0, 0, 0, 0}}});
        break;
      case D3DCMP_LESS_VALUE:
        cur->instructions.push_back(InstFloatCompare{kNoSat, FloatComparison::LessThan, mask, a, ref});
        break;
      case D3DCMP_EQUAL_VALUE:
        cur->instructions.push_back(InstFloatCompare{kNoSat, FloatComparison::Equal, mask, a, ref});
        break;
      case D3DCMP_LESSEQUAL_VALUE:
        cur->instructions.push_back(InstFloatCompare{kNoSat, FloatComparison::GreaterEqual, mask, ref, a});
        break;
      case D3DCMP_GREATER_VALUE:
        cur->instructions.push_back(InstFloatCompare{kNoSat, FloatComparison::LessThan, mask, ref, a});
        break;
      case D3DCMP_NOTEQUAL_VALUE:
        cur->instructions.push_back(InstFloatCompare{kNoSat, FloatComparison::NotEqual, mask, a, ref});
        break;
      default:
        cur->instructions.push_back(InstFloatCompare{kNoSat, FloatComparison::GreaterEqual, mask, a, ref});
        break;
      }
      discard_unless({temp_src(e, rep(0), OperandDataType::Integer), true});
    }

    SrcOperand pixel_z = SrcOperandInput{._ = {rep(2), false, false, OperandDataType::Float}, .regid = reg::PSInputVPos};
    if (biasesDepth()) {
      cur->instructions.push_back(InstFloatBinaryOp{
          kNoSat, FloatBinaryOp::Add, temp_dst(e + 2, 0x1), pixel_z, runtime_src(offsetof(SM30_RUNTIME_CONSTANTS, depth_bias) / 16, rep(0))
      });
      pixel_z = temp_src(e + 2, rep(0));
      cur->instructions.push_back(InstMov{kSat, DstOperandOutputDepth{._ = {0x1, OperandDataType::Float}}, pixel_z});
    }

    if (data.version.pixel && color0 && variant.fog_mode != SM30_FOG_NONE) {
      auto f = temp_dst(e, 0x1);
      SrcOperand fx = temp_src(e, rep(0));
      SrcOperand z = pixel_z;
      if (variant.flags & SM30_SHADER_FLAG_FOG_W) {
        // [[position]].w holds 1 / w
        SrcOperand rw = SrcOperandInput{._ = {rep(3), false, false, OperandDataType::Float}, .regid = reg::PSInputVPos};
        cur->instructions.push_back(InstFloatUnaryOp{kNoSat, FloatUnaryOp::Rcp, temp_dst(e + 3, 0x1), rw});
        z = temp_src(e + 3, rep(0));
      }
      auto &ins = cur->instructions;
      switch (variant.fog_mode) {
      case SM30_FOG_VERTEX:
        ins.push_back(InstMov{kSat, f, SrcOperandInput{._ = {rep(0), false, false, OperandDataType::Float}, .regid = reg::PSInputFog}});
        break;
      case SM30_FOG_EXP:
        ins.push_back(InstFloatBinaryOp{kNoSat, FloatBinaryOp::Mul, f, z, runtime_src(2, rep(2))});
        ins.push_back(InstFloatBinaryOp{kNoSat, FloatBinaryOp::Mul, f, fx, imm(-1.4426950f)});
        ins.push_back(InstFloatUnaryOp{kSat, FloatUnaryOp::Exp2, f, fx});
        break;
      case SM30_FOG_EXP2:
        ins.push_back(InstFloatBinaryOp{kNoSat, FloatBinaryOp::Mul, f, z, runtime_src(2, rep(2))});
        ins.push_back(InstFloatBinaryOp{kNoSat, FloatBinaryOp::Mul, f, fx, fx});
        ins.push_back(InstFloatBinaryOp{kNoSat, FloatBinaryOp::Mul, f, fx, imm(-1.4426950f)});
        ins.push_back(InstFloatUnaryOp{kSat, FloatUnaryOp::Exp2, f, fx});
        break;
      default:
        ins.push_back(InstFloatBinaryOp{kNoSat, FloatBinaryOp::Add, f, runtime_src(2, rep(1)), neg(z)});
        ins.push_back(InstFloatBinaryOp{kSat, FloatBinaryOp::Mul, f, fx, runtime_src(2, rep(3))});
        break;
      }
      ins.push_back(InstFloatBinaryOp{
          kNoSat, FloatBinaryOp::Add, temp_dst(e + 1, 0x7), temp_src(reg::OutColorBase), neg(runtime_src(1))
      });
      ins.push_back(InstFloatMAD{kNoSat, temp_dst(reg::OutColorBase, 0x7), temp_src(e + 1), fx, runtime_src(1)});
    }

    if (!data.version.pixel) {
      uint32_t k = 0;
      for (uint32_t i = 0; i < 6; i++) {
        if (!(variant.clip_plane_mask & (1 << i)))
          continue;
        cur->instructions.push_back(InstDotProduct{
            kNoSat, DstOperandOutput{._ = {0x1, OperandDataType::Float}, .regid = reg::VSOutClipBase + k++, .phase = ~0u},
            temp_src(reg::OutPos), runtime_src(3 + i), 4
        });
      }
      if (variant.flags & SM30_SHADER_FLAG_POINT_SIZE) {
        SrcOperand size = data.writes_psize ? temp_src(reg::OutPSize, rep(0)) : runtime_src(0, rep(1));
        cur->instructions.push_back(InstFloatBinaryOp{kNoSat, FloatBinaryOp::Max, temp_dst(e, 0x1), size, runtime_src(0, rep(2))});
        cur->instructions.push_back(InstFloatBinaryOp{
            kNoSat, FloatBinaryOp::Min,
            DstOperandOutput{._ = {0x1, OperandDataType::Float}, .regid = reg::VSOutPointSize, .phase = ~0u},
            temp_src(e, rep(0)), runtime_src(0, rep(3))
        });
      }
    }

    cur->target = BasicBlockUnconditionalBranch{data.copy_bb};
    data.final_bb->target = BasicBlockUnconditionalBranch{first};
  }

  uint32_t
  clipPlaneCount() const {
    return data.version.pixel ? 0 : std::popcount(variant.clip_plane_mask & 0x3F);
  }

  llvm::Error
  run(const char *name) {
    bool pixel = data.version.pixel;
    uint32_t vertex_idx = 0, base_vertex_idx = 0, instance_idx = 0, base_instance_idx = 0, clip_distance_idx = ~0u;
    // ProcessVertices: the output registers go to a buffer and nothing is rasterized
    SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA *vertex_so = nullptr;
    if (!pixel)
      args_get_data<SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT, SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA>(
          args, &vertex_so
      );
    if (pixel) {
      definePixelInputs();
      definePixelOutputs();
    } else if (vertex_so) {
      defineVertexInputs();
      auto bv = func_signature.DefineInput(InputBaseVertex{});
      auto vid = func_signature.DefineInput(InputVertexID{});
      auto so_table = func_signature.DefineInput(ArgumentBindingBuffer{
          .buffer_size = {},
          .location_index = SM50_BINDING_INDEX_STREAM_OUTPUT0,
          .array_size = 0,
          .memory_access = MemoryAccess::read,
          .address_space = AddressSpace::constant,
          .type = MSLUint{},
          .arg_name = "stream_outputs",
          .raster_order_group = {}
      });
      max_output_register = std::max(max_output_register, reg::VSOutCount);
      vertex_idx = vid;
      base_vertex_idx = bv;
      instance_idx = func_signature.DefineInput(InputInstanceID{});
      base_instance_idx = func_signature.DefineInput(InputBaseInstance{});
      epilogue.Add([=](llvm::Value *ret, IOHelper &io) -> llvm::Expected<llvm::Value *> {
        return io.PopulateOutputStreamOutput(ret, vertex_so, bv, vid, so_table);
      });
    } else {
      defineVertexInputs();
      defineVertexOutputs();
      if (clipPlaneCount())
        clip_distance_idx = func_signature.DefineOutput(OutputClipDistance{clipPlaneCount()});
      vertex_idx = func_signature.DefineInput(InputVertexID{});
      base_vertex_idx = func_signature.DefineInput(InputBaseVertex{});
      instance_idx = func_signature.DefineInput(InputInstanceID{});
      base_instance_idx = func_signature.DefineInput(InputBaseInstance{});
    }

    auto binding_map = setup_binding_table2(&shader->shader_info, func_signature, module);
    auto [function, function_metadata] = func_signature.CreateFunction(name, context, module, 0, vertex_so != nullptr);

    auto entry_bb = llvm::BasicBlock::Create(context, "entry", function);
    auto epilogue_bb = llvm::BasicBlock::Create(context, "epilogue", function);
    llvm::IRBuilder<> builder(entry_bb);
    llvm::raw_null_ostream nulldbg{};
    llvm::air::AIRBuilder air(
        {
            .sampleNaNToZero = bool(shader_flags & SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO),
            .defuseFma = bool(shader_flags & SM50_SHADER_FLAG_DEFUSE_FMA),
        },
        builder, nulldbg
    );
    setup_metal_version(module, metal_version);

    if (!pixel) {
      resource_map.vertex_id_with_base = function->getArg(vertex_idx);
      resource_map.base_vertex_id = function->getArg(base_vertex_idx);
      resource_map.instance_id_with_base = function->getArg(instance_idx);
      resource_map.base_instance_id = function->getArg(base_instance_idx);
      resource_map.vertex_id = builder.CreateSub(resource_map.vertex_id_with_base, resource_map.base_vertex_id);
      resource_map.instance_id = builder.CreateSub(resource_map.instance_id_with_base, resource_map.base_instance_id);
    }

    uint32_t max_input_register = shader->max_input_register;
    auto input_ty = llvm::ArrayType::get(types._int4, max_input_register);
    resource_map.input.ptr_int4 = builder.CreateAlloca(input_ty);
    resource_map.input.ptr_float4 = builder.CreateBitCast(
        resource_map.input.ptr_int4, llvm::ArrayType::get(types._float4, max_input_register)->getPointerTo()
    );
    builder.CreateStore(llvm::ConstantAggregateZero::get(input_ty), resource_map.input.ptr_int4);
    resource_map.input_element_count = max_input_register;

    auto output_ty = llvm::ArrayType::get(types._int4, max_output_register);
    resource_map.output.ptr_int4 = builder.CreateAlloca(output_ty);
    resource_map.output.ptr_float4 = builder.CreateBitCast(
        resource_map.output.ptr_int4, llvm::ArrayType::get(types._float4, max_output_register)->getPointerTo()
    );
    builder.CreateStore(llvm::ConstantAggregateZero::get(output_ty), resource_map.output.ptr_int4);
    resource_map.output_element_count = max_output_register;

    setup_temp_register(&shader->shader_info, resource_map, types, module, builder);
    setup_immediate_constant_buffer(&shader->shader_info, resource_map, types, module, builder);

    struct context ctx{
        .builder = builder,
        .air = air,
        .binding = *binding_map,
        .llvm = context,
        .module = module,
        .function = function,
        .resource = resource_map,
        .types = types,
        .pso_sample_mask = 0xffffffff,
        .shader_type = shader->shader_type,
        .metal_version = metal_version,
    };

    IOHelper io(air, function, types, resource_map);
    if (auto err = prologue.Run(io))
      return err;
    buildEpilogueBlocks();
    auto real_entry = convert_basicblocks(shader->entry(), ctx, epilogue_bb);
    data.final_bb->target = BasicBlockUnconditionalBranch{data.copy_bb};
    if (auto err = real_entry.takeError())
      return err;
    builder.CreateBr(real_entry.get());

    builder.SetInsertPoint(epilogue_bb);
    auto result = epilogue.Run(function->getReturnType(), io);
    if (auto err = result.takeError())
      return err;
    llvm::Value *ret = result.get();
    if (clip_distance_idx != ~0u) {
      auto array_ty = llvm::ArrayType::get(types._float, clipPlaneCount());
      llvm::Value *array = llvm::ConstantAggregateZero::get(array_ty);
      for (uint32_t k = 0; k < clipPlaneCount(); k++) {
        auto ptr = builder.CreateGEP(
            resource_map.output.ptr_float4->getType()->getNonOpaquePointerElementType(), resource_map.output.ptr_float4,
            {builder.getInt32(0), builder.getInt32(reg::VSOutClipBase + k), builder.getInt32(0)}
        );
        array = builder.CreateInsertValue(array, builder.CreateLoad(types._float, ptr), {k});
      }
      ret = builder.CreateInsertValue(ret, array, {clip_distance_idx});
    }
    if (ret)
      builder.CreateRet(ret);
    else
      builder.CreateRetVoid();

    module.getOrInsertNamedMetadata(pixel ? "air.fragment" : "air.vertex")->addOperand(function_metadata);
    return llvm::Error::success();
  }
};

} // namespace

std::unique_ptr<SM50ShaderInternal>
create_shader(
    const uint32_t *tokens, size_t count, const DecodeOptions &options, std::string &error, BindingSlotMasks &masks
) {
  d3d9bc::Version version;
  if (!count || !d3d9bc::read_version(tokens[0], version)) {
    error = "Invalid D3D9 shader bytecode";
    return nullptr;
  }
  auto shader = std::make_unique<SM50ShaderInternal>();
  shader->shader_type = version.pixel ? microsoft::D3D10_SB_PIXEL_SHADER : microsoft::D3D10_SB_VERTEX_SHADER;
  shader->d3d9 = std::make_shared<ShaderData>();
  if (auto err = decode_shader(tokens, count, *shader, *shader->d3d9, options)) {
    error = *err;
    return nullptr;
  }
  if (version.pixel) {
    shader->max_input_register = reg::PSInputCount;
    shader->max_output_register = 4;
    shader->pso_valid_output_reg_mask = shader->d3d9->color_output_mask;
  } else {
    // vs_3_0 has 16 input registers; the generated fixed-function shader uses one more for PSIZE
    shader->max_input_register = 32;
    shader->max_output_register = std::max(12u, reg::VSOutLegacyCount);
  }
  masks = define_argument_bindings(shader.get());
  return shader;
}

llvm::Error
convert_shader(
    SM50ShaderInternal *pShader, const char *name, llvm::LLVMContext &context, llvm::Module &module,
    SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs
) {
  auto &data = *pShader->d3d9;
  SM30_SHADER_D3D9_DATA *d3d9 = nullptr;
  DecodeOptions options;
  if (!data.pixel_tokens.empty() && args_get_data<SM30_SHADER_D3D9, SM30_SHADER_D3D9_DATA>(pArgs, &d3d9)) {
    options.ps1_texture_types = data.version.major == 1 ? d3d9->ps1_texture_types : 0;
    options.shadow_samplers = d3d9->shadow_samplers;
    options.fetch4_samplers = d3d9->fetch4_samplers & ~d3d9->shadow_samplers;
    options.unbound_samplers = d3d9->unbound_samplers;
  }
  if (!data.pixel_tokens.empty() && options != data.options) {
    std::string error;
    BindingSlotMasks masks;
    auto variant = create_shader(data.pixel_tokens.data(), data.pixel_tokens.size(), options, error, masks);
    if (!variant)
      return llvm::make_error<mullion::UnsupportedFeature>(error);
    Driver driver(variant.get(), context, module, pArgs);
    return driver.run(name);
  }
  Driver driver(pShader, context, module, pArgs);
  return driver.run(name);
}

} // namespace mullion::d3d9

using namespace mullion;

AIRCONV_API int
SM30Initialize(
    const void *pBytecode, size_t BytecodeSize, sm50_shader_t *ppShader, MTL_SHADER_REFLECTION *pRefl,
    sm50_error_t *ppError
) {
  using namespace mullion::dxbc;
  if (ppError)
    *ppError = nullptr;
  auto errorObj = std::make_unique<SM50ErrorInternal>();
  llvm::raw_svector_ostream errorOut(errorObj->buf);

  if (!ppShader || !pBytecode || BytecodeSize < 8) {
    errorOut << "Invalid D3D9 shader bytecode";
    SM50_RETURN_ERROR(errorObj, ppError, 1);
  }
  std::string error;
  BindingSlotMasks masks;
  auto shader = d3d9::create_shader((const uint32_t *)pBytecode, BytecodeSize / 4, {}, error, masks);
  if (!shader) {
    errorOut << error;
    SM50_RETURN_ERROR(errorObj, ppError, 1);
  }
  bool pixel = shader->d3d9->version.pixel;
  if (pRefl) {
    *pRefl = {};
    pRefl->ConstanttBufferTableBindIndex = shader->args_reflection_cbuffer.size() > 0 ? 29 : ~0u;
    pRefl->ArgumentBufferBindIndex = shader->args_reflection.size() > 0 ? 30 : ~0u;
    pRefl->ConstantBufferSlotMask = masks.cbuffer;
    pRefl->SamplerSlotMask = masks.sampler;
    pRefl->SRVSlotMaskHi = masks.srv_hi;
    pRefl->SRVSlotMaskLo = masks.srv_lo;
    pRefl->UAVSlotMask = masks.uav;
    pRefl->NumConstantBuffers = shader->args_reflection_cbuffer.size();
    pRefl->NumArguments = shader->args_reflection.size();
    if (pixel)
      pRefl->PixelShader.ValidRenderTargets = shader->pso_valid_output_reg_mask;
    pRefl->NumOutputElement = shader->max_output_register;
    pRefl->ArgumentTableQwords = shader->shader_info.binding_table.Size();
  }

  *ppShader = (sm50_shader_t)shader.release();
  return 0;
}
