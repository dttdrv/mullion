#include "dxbc_converter.hpp"
#include "DXBCParser/BlobContainer.h"
#include "DXBCParser/ShaderBinary.h"
#include "airconv_error.hpp"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/FMF.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"
#include <bit>
#include <memory>
#include <string>
#include <vector>

#include "metallib_writer.hpp"
#include "nt/air_builder.hpp"
#include "shader_common.hpp"
#include "tessellation_limits.hpp"

#include "airconv_context.hpp"

#include "ftl.hpp"

class SM50CompiledBitcodeInternal {
public:
  llvm::SmallVector<char, 0> vec;
};

class SM50ErrorInternal {
public:
  llvm::SmallVector<char, 0> buf;
};

namespace mullion::dxbc {

inline mullion::shader::common::ResourceType
to_shader_resource_type(microsoft::D3D10_SB_RESOURCE_DIMENSION dim) {
  switch (dim) {
  case microsoft::D3D10_SB_RESOURCE_DIMENSION_UNKNOWN:
  case microsoft::D3D10_SB_RESOURCE_DIMENSION_BUFFER:
  case microsoft::D3D11_SB_RESOURCE_DIMENSION_RAW_BUFFER:
  case microsoft::D3D11_SB_RESOURCE_DIMENSION_STRUCTURED_BUFFER:
    return shader::common::ResourceType::TextureBuffer;
  case microsoft::D3D10_SB_RESOURCE_DIMENSION_TEXTURE1D:
    return shader::common::ResourceType::Texture1D;
  case microsoft::D3D10_SB_RESOURCE_DIMENSION_TEXTURE2D:
    return shader::common::ResourceType::Texture2D;
  case microsoft::D3D10_SB_RESOURCE_DIMENSION_TEXTURE2DMS:
    return shader::common::ResourceType::Texture2DMultisampled;
  case microsoft::D3D10_SB_RESOURCE_DIMENSION_TEXTURE3D:
    return shader::common::ResourceType::Texture3D;
  case microsoft::D3D10_SB_RESOURCE_DIMENSION_TEXTURECUBE:
    return shader::common::ResourceType::TextureCube;
  case microsoft::D3D10_SB_RESOURCE_DIMENSION_TEXTURE1DARRAY:
    return shader::common::ResourceType::Texture1DArray;
  case microsoft::D3D10_SB_RESOURCE_DIMENSION_TEXTURE2DARRAY:
    return shader::common::ResourceType::Texture2DArray;
  case microsoft::D3D10_SB_RESOURCE_DIMENSION_TEXTURE2DMSARRAY:
    return shader::common::ResourceType::Texture2DMultisampledArray;
  case microsoft::D3D10_SB_RESOURCE_DIMENSION_TEXTURECUBEARRAY:
    return shader::common::ResourceType::TextureCubeArray;
  }
  assert(0 && "invalid D3D10_SB_RESOURCE_DIMENSION");
};

inline mullion::shader::common::ScalerDataType
to_shader_scaler_type(microsoft::D3D10_SB_RESOURCE_RETURN_TYPE type) {
  switch (type) {

  case microsoft::D3D10_SB_RETURN_TYPE_UNORM:
  case microsoft::D3D10_SB_RETURN_TYPE_SNORM:
  case microsoft::D3D10_SB_RETURN_TYPE_FLOAT:
    return shader::common::ScalerDataType::Float;
  case microsoft::D3D10_SB_RETURN_TYPE_SINT:
    return shader::common::ScalerDataType::Int;
  case microsoft::D3D10_SB_RETURN_TYPE_UINT:
    return shader::common::ScalerDataType::Uint;
  case microsoft::D3D10_SB_RETURN_TYPE_MIXED:
    return shader::common::ScalerDataType::Uint; // kinda weird but ok
  case microsoft::D3D11_SB_RETURN_TYPE_DOUBLE:
  case microsoft::D3D11_SB_RETURN_TYPE_CONTINUED:
  case microsoft::D3D11_SB_RETURN_TYPE_UNUSED:
    break;
  }
  assert(0 && "invalid D3D10_SB_RESOURCE_RETURN_TYPE");
}

uint32_t next_pow2(uint32_t x) {
  return x == 1 ? 1 : 1 << (32 - __builtin_clz(x - 1));
}


void setup_immediate_constant_buffer(
  const ShaderInfo *shader_info, io_binding_map &resource_map,
  air::AirType &types, llvm::Module &module, llvm::IRBuilder<> &builder
) {
  if (!shader_info->immConstantBufferData.size()) {
    return;
  }
  auto &context = module.getContext();
  auto type = llvm::ArrayType::get(
    types._int4, shader_info->immConstantBufferData.size()
  );
  auto const_data = llvm::ConstantArray::get(
    type,
    shader_info->immConstantBufferData |
      [&](auto data) {
        return llvm::ConstantVector::get(
          {llvm::ConstantInt::get(context, llvm::APInt{32, data[0], false}),
           llvm::ConstantInt::get(context, llvm::APInt{32, data[1], false}),
           llvm::ConstantInt::get(context, llvm::APInt{32, data[2], false}),
           llvm::ConstantInt::get(context, llvm::APInt{32, data[3], false})}
        );
      }
  );
  llvm::GlobalVariable *icb = new llvm::GlobalVariable(
    module, type, true, llvm::GlobalValue::InternalLinkage, const_data, "icb",
    nullptr, llvm::GlobalValue::NotThreadLocal, 2
  );
  icb->setAlignment(llvm::Align(4));
  resource_map.icb = icb;
  resource_map.icb_float = builder.CreateBitCast(
    resource_map.icb, llvm::ArrayType::get(
                        types._float4, shader_info->immConstantBufferData.size()
                      )
                        ->getPointerTo(2)
  );
}

void setup_tgsm(
  const ShaderInfo *shader_info, io_binding_map &resource_map,
  air::AirType &types, llvm::Module &module
) {
  for (auto &[id, tgsm] : shader_info->tgsmMap) {
    auto type = llvm::ArrayType::get(types._int, tgsm.size_in_uint);
    llvm::GlobalVariable *tgsm_h = new llvm::GlobalVariable(
      module, type, false, llvm::GlobalValue::InternalLinkage,
      llvm::UndefValue::get(type), "g" + std::to_string(id), nullptr,
      llvm::GlobalValue::NotThreadLocal, 3
    );
    tgsm_h->setAlignment(llvm::Align(4));
    resource_map.tgsm_map[id] = {tgsm.structured ? tgsm.stride : 0, tgsm_h};
  }
}

llvm::Value *
index_from_view(llvm::IRBuilder<> &builder, air::AirType &types, llvm::Value *view, llvm::Value *format, llvm::Value *position) {
  // an index is one or two ushorts of the view
  constexpr uint32_t kFormatR32Uint = 42; // DXGI_FORMAT_R32_UINT
  auto view_ty = view->getType()->getNonOpaquePointerElementType();
  auto ushort_ptr = builder.getInt16Ty()->getPointerTo((uint32_t)air::AddressSpace::device);
  auto wide = builder.CreateICmpEQ(format, builder.getInt32(kFormatR32Uint));
  auto shorts = builder.CreateSelect(wide, builder.getInt64(2), builder.getInt64(1));
  auto first = builder.CreateMul(builder.CreateZExt(position, types._long), shorts);
  auto in_view = builder.CreateICmpULE(
      builder.CreateShl(builder.CreateAdd(first, shorts), 1),
      builder.CreateZExt(builder.CreateLoad(types._int, builder.CreateStructGEP(view_ty, view, 1)), types._long)
  );
  // outside the view both loads read the view itself, and their value is discarded
  auto p = builder.CreateSelect(
      in_view,
      builder.CreateIntToPtr(
          // an indirect command's view is only 4-byte aligned
          builder.CreateAdd(
              builder.CreateAlignedLoad(types._long, builder.CreateStructGEP(view_ty, view, 0), llvm::Align(4)),
              builder.CreateShl(first, 1)
          ),
          ushort_ptr
      ),
      builder.CreateBitCast(view, ushort_ptr)
  );
  auto lo = builder.CreateZExt(builder.CreateLoad(builder.getInt16Ty(), p), types._int);
  auto hi = builder.CreateZExt(
      builder.CreateLoad(builder.getInt16Ty(), builder.CreateGEP(builder.getInt16Ty(), p, {builder.CreateZExt(wide, types._int)})),
      types._int
  );
  return builder.CreateSelect(
      in_view, builder.CreateOr(lo, builder.CreateSelect(wide, builder.CreateShl(hi, 16), builder.getInt32(0))),
      builder.getInt32(0)
  );
}

void setup_temp_register(
  const ShaderInfo *shader_info, io_binding_map &resource_map,
  air::AirType &types, llvm::Module &module, llvm::IRBuilder<> &builder
) {
  resource_map.temp.ptr_int4 = builder.CreateAlloca(
    llvm::ArrayType::get(types._int, 4 * shader_info->tempRegisterCount)
  );
  resource_map.temp.ptr_float4 = builder.CreateBitCast(
    resource_map.temp.ptr_int4,
    llvm::ArrayType::get(types._float, 4 * shader_info->tempRegisterCount)
      ->getPointerTo()
  );
  for (auto &phase : shader_info->phases) {
    resource_map.phases.push_back({});
    auto &phase_temp = resource_map.phases.back();

    phase_temp.temp.ptr_int4 = builder.CreateAlloca(
      llvm::ArrayType::get(types._int, 4 * phase.tempRegisterCount)
    );
    phase_temp.temp.ptr_float4 = builder.CreateBitCast(
      phase_temp.temp.ptr_int4,
      llvm::ArrayType::get(types._float, 4 * phase.tempRegisterCount)
        ->getPointerTo()
    );

    for (auto &[idx, info] : phase.indexableTempRegisterCounts) {
      auto &[numRegisters, mask] = info;
      auto channel_count = std::bit_width(mask);
      auto ptr_int_vec = builder.CreateAlloca(llvm::ArrayType::get(types._int, numRegisters * channel_count));
      auto ptr_float_vec = builder.CreateBitCast(
          ptr_int_vec, llvm::ArrayType::get(types._float, numRegisters * channel_count)->getPointerTo()
      );
      phase_temp.indexable_temp_map[idx] = {
        ptr_int_vec, ptr_float_vec, (uint32_t)channel_count
      };
    }
  }
  for (auto &[idx, info] : shader_info->indexableTempRegisterCounts) {
    auto &[numRegisters, mask] = info;
    auto channel_count = std::bit_width(mask);
    auto ptr_int_vec = builder.CreateAlloca(llvm::ArrayType::get(types._int, numRegisters * channel_count));
    auto ptr_float_vec = builder.CreateBitCast(
        ptr_int_vec, llvm::ArrayType::get(types._float, numRegisters * channel_count)->getPointerTo()
    );
    resource_map.indexable_temp_map[idx] = {
      ptr_int_vec, ptr_float_vec, (uint32_t)channel_count
    };
  }
  if (shader_info->use_cmp_exch) {
    resource_map.cmp_exch_temp = builder.CreateAlloca(types._int);
  }
}

void setup_metal_version(llvm::Module &module, SM50_SHADER_METAL_VERSION metal_version) {
  using namespace llvm;
  auto &context = module.getContext();
  auto createUnsignedInteger = [&](uint32_t s) {
    return ConstantAsMetadata::get(ConstantInt::get(context, APInt{32, s, false}));
  };
  auto createString = [&](auto s) { return MDString::get(context, s); };

  auto airVersion = module.getOrInsertNamedMetadata("air.version");
  auto airLangVersion = module.getOrInsertNamedMetadata("air.language_version");
  switch (metal_version) {
  case SM50_SHADER_METAL_320: {
    airVersion->addOperand(
        MDTuple::get(context, {createUnsignedInteger(2), createUnsignedInteger(7), createUnsignedInteger(0)})
    );
    airLangVersion->addOperand(MDTuple::get(
        context, {createString("Metal"), createUnsignedInteger(3), createUnsignedInteger(2), createUnsignedInteger(0)}
    ));
    module.setTargetTriple("air64-apple-macosx15.0.0");
    break;
  }
  default: {
    airVersion->addOperand(
        MDTuple::get(context, {createUnsignedInteger(2), createUnsignedInteger(6), createUnsignedInteger(0)})
    );
    airLangVersion->addOperand(MDTuple::get(
        context, {createString("Metal"), createUnsignedInteger(3), createUnsignedInteger(1), createUnsignedInteger(0)}
    ));
    break;
  }
  }
}

RegisterComponentType
component_type_from_pixel_format(air::MTLPixelFormat format) {
  switch (format) {
  case air::MTLPixelFormat::R8Sint:
  case air::MTLPixelFormat::R16Sint:
  case air::MTLPixelFormat::RG8Sint:
  case air::MTLPixelFormat::R32Sint:
  case air::MTLPixelFormat::RG16Sint:
  case air::MTLPixelFormat::RGBA8Sint:
  case air::MTLPixelFormat::RG32Sint:
  case air::MTLPixelFormat::RGBA16Sint:
  case air::MTLPixelFormat::RGBA32Sint:
    return RegisterComponentType::Int;
  case air::MTLPixelFormat::R8Uint:
  case air::MTLPixelFormat::R16Uint:
  case air::MTLPixelFormat::RG8Uint:
  case air::MTLPixelFormat::R32Uint:
  case air::MTLPixelFormat::RG16Uint:
  case air::MTLPixelFormat::RGBA8Uint:
  case air::MTLPixelFormat::RGB10A2Uint:
  case air::MTLPixelFormat::RG32Uint:
  case air::MTLPixelFormat::RGBA16Uint:
  case air::MTLPixelFormat::RGBA32Uint:
    return RegisterComponentType::Uint;
  case air::MTLPixelFormat::A8Unorm:
  case air::MTLPixelFormat::R8Unorm:
  case air::MTLPixelFormat::R8Unorm_sRGB:
  case air::MTLPixelFormat::R8Snorm:
  case air::MTLPixelFormat::R16Unorm:
  case air::MTLPixelFormat::R16Snorm:
  case air::MTLPixelFormat::R16Float:
  case air::MTLPixelFormat::RG8Unorm:
  case air::MTLPixelFormat::RG8Unorm_sRGB:
  case air::MTLPixelFormat::RG8Snorm:
  case air::MTLPixelFormat::B5G6R5Unorm:
  case air::MTLPixelFormat::A1BGR5Unorm:
  case air::MTLPixelFormat::ABGR4Unorm:
  case air::MTLPixelFormat::BGR5A1Unorm:
  case air::MTLPixelFormat::R32Float:
  case air::MTLPixelFormat::RG16Unorm:
  case air::MTLPixelFormat::RG16Snorm:
  case air::MTLPixelFormat::RG16Float:
  case air::MTLPixelFormat::RGBA8Unorm:
  case air::MTLPixelFormat::RGBA8Unorm_sRGB:
  case air::MTLPixelFormat::RGBA8Snorm:
  case air::MTLPixelFormat::BGRA8Unorm:
  case air::MTLPixelFormat::BGRA8Unorm_sRGB:
  case air::MTLPixelFormat::RGB10A2Unorm:
  case air::MTLPixelFormat::RG11B10Float:
  case air::MTLPixelFormat::RGB9E5Float:
  case air::MTLPixelFormat::BGR10A2Unorm:
  case air::MTLPixelFormat::BGR10_XR:
  case air::MTLPixelFormat::BGR10_XR_sRGB:
  case air::MTLPixelFormat::RG32Float:
  case air::MTLPixelFormat::RGBA16Unorm:
  case air::MTLPixelFormat::RGBA16Snorm:
  case air::MTLPixelFormat::RGBA16Float:
  case air::MTLPixelFormat::BGRA10_XR:
  case air::MTLPixelFormat::BGRA10_XR_sRGB:
  case air::MTLPixelFormat::RGBA32Float:
  case air::MTLPixelFormat::BC1_RGBA:
  case air::MTLPixelFormat::BC1_RGBA_sRGB:
  case air::MTLPixelFormat::BC2_RGBA:
  case air::MTLPixelFormat::BC2_RGBA_sRGB:
  case air::MTLPixelFormat::BC3_RGBA:
  case air::MTLPixelFormat::BC3_RGBA_sRGB:
  case air::MTLPixelFormat::BC4_RUnorm:
  case air::MTLPixelFormat::BC4_RSnorm:
  case air::MTLPixelFormat::BC5_RGUnorm:
  case air::MTLPixelFormat::BC5_RGSnorm:
  case air::MTLPixelFormat::BC6H_RGBFloat:
  case air::MTLPixelFormat::BC6H_RGBUfloat:
  case air::MTLPixelFormat::BC7_RGBAUnorm:
  case air::MTLPixelFormat::BC7_RGBAUnorm_sRGB:
    return RegisterComponentType::Float;
  default:
    break;
  }
  return RegisterComponentType::Unknown;
}

// whether a conservative rasterizer leaves out the pixel at `position`: its square, a unit wide, has no point in the
// triangle with these window coordinates (SM50_CONSERVATIVE_VARYINGS). the separating axis test: the axes, then each
// edge's normal. a first coordinate that is NaN takes every pixel. and whether the square is inside the triangle,
// every edge having all of it (SV_InnerCoverage), which a triangle of no area has of no square
static std::pair<llvm::Value *, llvm::Value *>
conservative_overlap(llvm::IRBuilder<> &builder, llvm::air::AIRBuilder &air, llvm::Value *position, llvm::Value *const *window) {
  using llvm::air::AIRBuilder;
  auto f = [&](double v) { return air.getFloat(v); };
  auto abs = [&](llvm::Value *v) { return air.CreateFPUnOp(AIRBuilder::fabs, v, false); };
  auto min = [&](llvm::Value *l, llvm::Value *r) { return air.CreateFPBinOp(AIRBuilder::fmin, l, r, false); };
  auto max = [&](llvm::Value *l, llvm::Value *r) { return air.CreateFPBinOp(AIRBuilder::fmax, l, r, false); };
  // the pixel's center; a little more than half a pixel around it, the uncertainty tier 1 has room for
  const double half = 0.5 + 1.0 / 256;
  llvm::Value *center[2];
  for (uint32_t axis = 0; axis < 2; axis++)
    center[axis] = builder.CreateFAdd(
        air.CreateFPUnOp(AIRBuilder::floor, builder.CreateExtractElement(position, axis), false), f(0.5)
    );
  auto at = [&](uint32_t k, uint32_t axis) { return window[2 * k + axis]; };
  auto x = [&](uint32_t k) { return at(k, 0); };
  auto y = [&](uint32_t k) { return at(k, 1); };
  llvm::Value *outside = builder.getFalse();
  for (uint32_t axis = 0; axis < 2; axis++) {
    auto of = [&](uint32_t k) { return at(k, axis); };
    auto low = min(min(of(0), of(1)), of(2)), high = max(max(of(0), of(1)), of(2));
    outside = builder.CreateOr(outside, builder.CreateFCmpOLT(builder.CreateFAdd(center[axis], f(half)), low));
    outside = builder.CreateOr(outside, builder.CreateFCmpOGT(builder.CreateFSub(center[axis], f(half)), high));
  }
  auto area = builder.CreateFSub(
      builder.CreateFMul(builder.CreateFSub(x(1), x(0)), builder.CreateFSub(y(2), y(0))),
      builder.CreateFMul(builder.CreateFSub(x(2), x(0)), builder.CreateFSub(y(1), y(0)))
  );
  auto inward = builder.CreateSelect(builder.CreateFCmpOGT(area, f(0)), f(-1), f(1));
  llvm::Value *inside = builder.CreateFCmpONE(area, f(0));
  for (uint32_t i = 0; i < 3; i++) {
    uint32_t n = (i + 1) % 3;
    auto a = builder.CreateFMul(builder.CreateFSub(y(n), y(i)), inward);
    auto b = builder.CreateFNeg(builder.CreateFMul(builder.CreateFSub(x(n), x(i)), inward));
    // the edge's value at the center, against its most at any point of the square
    auto value = builder.CreateFAdd(
        builder.CreateFMul(a, builder.CreateFSub(center[0], x(i))), builder.CreateFMul(b, builder.CreateFSub(center[1], y(i)))
    );
    auto reach = builder.CreateFMul(builder.CreateFAdd(abs(a), abs(b)), f(half));
    outside = builder.CreateOr(outside, builder.CreateFCmpOLT(builder.CreateFAdd(value, reach), f(0)));
    inside = builder.CreateAnd(inside, builder.CreateFCmpOGE(builder.CreateFSub(value, reach), f(0)));
  }
  return {builder.CreateAnd(outside, builder.CreateNot(air.CreateIsNaN(x(0)))), builder.CreateAnd(inside, builder.CreateNot(air.CreateIsNaN(x(0))))};
}

llvm::Error convert_dxbc_pixel_shader(
  SM50ShaderInternal *pShaderInternal, const char *name,
  llvm::LLVMContext &context, llvm::Module &module,
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs
) {
  using namespace microsoft;

  auto func_signature = pShaderInternal->func_signature; // copy
  auto shader_info = &(pShaderInternal->shader_info);

  uint32_t max_input_register = pShaderInternal->max_input_register;
  uint32_t max_output_register = pShaderInternal->max_output_register;
  uint32_t pso_sample_mask = 0xffffffff;
  bool pso_dual_source_blending = false;
  bool pso_disable_depth_output = false;
  uint32_t pso_unorm_output_reg_mask = 0;
  uint32_t *pso_pixel_formats = nullptr;
  SM50_SHADER_PSO_PIXEL_SHADER_DATA *pso_data = nullptr;
  if (args_get_data<SM50_SHADER_PSO_PIXEL_SHADER, SM50_SHADER_PSO_PIXEL_SHADER_DATA>(pArgs, &pso_data)) {
    pso_dual_source_blending = pso_data->dual_source_blending;
    pso_disable_depth_output = pso_data->disable_depth_output;
    pso_unorm_output_reg_mask = pso_data->unorm_output_reg_mask;
    pso_sample_mask = pso_data->sample_mask;
    pso_pixel_formats = pso_data->pixel_formats;
    if (pso_data->late_tests)
      func_signature.UseLateFragmentTests();
  }
  SM50_SHADER_METAL_VERSION metal_version = SM50_SHADER_METAL_310;
  SM50_SHADER_FLAG shader_flags = {};
  SM50_SHADER_COMMON_DATA *sm50_common = nullptr;
  if (args_get_data<SM50_SHADER_COMMON, SM50_SHADER_COMMON_DATA>(pArgs, &sm50_common)) {
    metal_version = sm50_common->metal_version;
    shader_flags = sm50_common->flags;
  }
  SM50_SHADER_ROOT_SIGNATURE_DATA *rootsig = nullptr;
  args_get_data<SM50_SHADER_ROOT_SIGNATURE, SM50_SHADER_ROOT_SIGNATURE_DATA>(pArgs, &rootsig);

  IREffect prologue([](auto) { return std::monostate(); });
  IRValue epilogue([](struct context ctx) -> pvalue {
    auto retTy = ctx.function->getReturnType();
    if (retTy->isVoidTy()) {
      return nullptr;
    }
    return llvm::ConstantAggregateZero::get(retTy);
  });

  io_binding_map resource_map;
  air::AirType types(context);

  {
    SignatureContext sig_ctx(prologue, epilogue, func_signature, resource_map);
    sig_ctx.dual_source_blending = pso_dual_source_blending;
    sig_ctx.disable_depth_output = pso_disable_depth_output;
    sig_ctx.pull_mode_reg_mask = shader_info->pull_mode_reg_mask;
    sig_ctx.unorm_output_reg_mask = pso_unorm_output_reg_mask;
    if (pso_pixel_formats)
      memcpy(sig_ctx.pixel_formats, pso_pixel_formats, sizeof(sig_ctx.pixel_formats));
    for (auto &p : pShaderInternal->signature_handlers) {
      p(sig_ctx);
    }
  }
  if (pso_sample_mask != 0xffffffff) {
    auto assigned_index =
      func_signature.DefineOutput(air::OutputCoverageMask{});
    epilogue >> [=](pvalue value) -> IRValue {
      return make_irvalue([=](struct context ctx) {
        auto &builder = ctx.builder;
        if (ctx.resource.coverage_mask_reg)
          return value;
        return builder.CreateInsertValue(
          value, builder.getInt32(ctx.pso_sample_mask), {assigned_index}
        );
      });
    };
  }

  // variable rate shading, of a shader that is not run per pixel or sample whatever the rate
  bool coarse_shading =
      pso_data && pso_data->coarse_shading && !pShaderInternal->ps_fine_shading && !shader_info->pull_mode_reg_mask;

  std::optional<uint32_t> statistics;
  uint32_t sample = 0, coverage = ~0u;
  if (pso_data && pso_data->pipeline_statistics) {
    statistics = func_signature.DefineInput(air::ArgumentBindingBuffer{
        .buffer_size = sizeof(uint64_t),
        .location_index = SM50_BINDING_INDEX_PIPELINE_STATISTICS,
        .array_size = 1,
        .memory_access = air::MemoryAccess::read_write,
        .address_space = air::AddressSpace::device,
        .type = air::msl_uint,
        .arg_name = "pipeline_statistics",
    });
    // with a sample index input Metal runs the function once for each covered sample, each run given the pixel's
    // coverage: the run of the first covered sample counts, unless the shader runs per sample itself, which its own
    // sample index, declared before the counter, tells
    // sample index, declared before the counter, tells. a coarse pixel's leader alone counts (CoarseShading)
    if (!coarse_shading) {
      sample = func_signature.DefineInput(air::InputSampleIndex{});
      coverage = sample < *statistics ? ~0u : func_signature.DefineInput(air::InputInputCoverage{});
    }
  }

  // the depth bounds test of a GPU without one: the pixel's position, whether the draw tests, and the samples that pass
  uint32_t bounds_position = ~0u, bounds_enabled = ~0u, bounds_mask = ~0u, bounds_coverage = ~0u;
  if (pso_data && pso_data->depth_bounds) {
    bounds_position =
        func_signature.DefineInput(air::InputPosition{.interpolation = air::Interpolation::center_no_perspective});
    bounds_enabled = func_signature.DefineInput(air::ArgumentBindingBuffer{
        .buffer_size = 8,
        .location_index = SM50_BINDING_INDEX_DEPTH_BOUNDS,
        .array_size = 0,
        .memory_access = air::MemoryAccess::read,
        .address_space = air::AddressSpace::constant,
        .type = air::msl_uint2,
        .arg_name = "depth_bounds",
        .raster_order_group = {}
    });
    bounds_mask = func_signature.DefineInput(air::ArgumentBindingBuffer{
        .buffer_size = {},
        .location_index = SM50_BINDING_INDEX_DEPTH_BOUNDS_SAMPLES,
        .array_size = 0,
        .memory_access = air::MemoryAccess::read,
        .address_space = air::AddressSpace::device,
        .type = air::msl_uint,
        .arg_name = "depth_bounds_samples",
        .raster_order_group = {}
    });
    bounds_coverage = func_signature.DefineOutput(air::OutputCoverageMask{});
  }

  std::optional<CoarseShading> coarse;
  if (coarse_shading)
    coarse.emplace(func_signature, pso_data->shading_rate_varying);

  // conservative rasterization: the pixel's position, and the triangle the mesh stage passes on; the inputs it
  // interpolates over w, with that 1/w at each place they are interpolated (SM50_CONSERVATIVE_RCP_W), and the coverage,
  // of every sample (or this one, when the shader runs per sample)
  uint32_t conservative_position = ~0u, conservative_window[SM50_CONSERVATIVE_VARYINGS],
           conservative_rcp_w[SM50_CONSERVATIVE_RCP_W], conservative_coverage = ~0u, conservative_sample = ~0u;
  std::vector<std::pair<uint32_t, uint32_t>> conservative_over_w; // an input, and where it is interpolated
  if (pso_data && pso_data->conservative) {
    auto &inputs = func_signature.Inputs();
    for (uint32_t i = 0; i < inputs.size(); i++) {
      if (auto in = std::get_if<air::InputFragmentStageIn>(&inputs[i])) {
        using air::Interpolation;
        uint32_t at = in->pull_mode                                              ? 3
                      : in->interpolation == Interpolation::center_perspective   ? 0
                      : in->interpolation == Interpolation::centroid_perspective ? 1
                      : in->interpolation == Interpolation::sample_perspective   ? 2
                                                                                 : ~0u;
        bool perspective = in->interpolation == Interpolation::center_perspective ||
                           in->interpolation == Interpolation::centroid_perspective ||
                           in->interpolation == Interpolation::sample_perspective;
        if (~at && perspective)
          conservative_over_w.push_back({i, at});
      }
      if (std::holds_alternative<air::InputInputCoverage>(inputs[i]))
        conservative_coverage = i;
      // the shader's own, not the pipeline statistics' (declared after it)
      if (std::holds_alternative<air::InputSampleIndex>(inputs[i]) && (!statistics || i < *statistics))
        conservative_sample = i;
    }
    const air::Interpolation rcp_w_at[SM50_CONSERVATIVE_RCP_W] = {
        air::Interpolation::center_no_perspective, air::Interpolation::centroid_no_perspective,
        air::Interpolation::sample_no_perspective, air::Interpolation::center_perspective
    };
    for (uint32_t i = 0; i < SM50_CONSERVATIVE_RCP_W; i++)
      conservative_rcp_w[i] = func_signature.DefineInput(
          air::InputFragmentStageIn{
              .user = "mullion_conservative" + std::to_string(SM50_CONSERVATIVE_VARYINGS + i),
              .type = air::msl_float,
              .interpolation = rcp_w_at[i],
              .pull_mode = i == 3
          }
      );
    conservative_position =
        func_signature.DefineInput(air::InputPosition{.interpolation = air::Interpolation::center_no_perspective});
    for (uint32_t i = 0; i < SM50_CONSERVATIVE_VARYINGS; i++)
      conservative_window[i] = func_signature.DefineInput(air::InputFragmentStageIn{
          .user = "mullion_conservative" + std::to_string(i),
          .type = air::msl_float,
          .interpolation = air::Interpolation::flat,
          .pull_mode = false
      });
  }

  auto binding_map = rootsig ? setup_binding_rootsig(
                                   shader_info, func_signature, module, D3D10_SB_PIXEL_SHADER, rootsig->bytecode,
                                   rootsig->bytecode_length
                               )
                             : setup_binding_table2(shader_info, func_signature, module);
  if (!binding_map)
    return llvm::make_error<UnsupportedFeature>("invalid root signature or missing resource binding");

  auto [function, function_metadata] =
    func_signature.CreateFunction(name, context, module, 0, false);

  auto entry_bb = llvm::BasicBlock::Create(context, "entry", function);
  auto epilogue_bb = llvm::BasicBlock::Create(context, "epilogue", function);
  llvm::IRBuilder<> builder(entry_bb);
  llvm::raw_null_ostream nulldbg{};
  llvm::air::AIRBuilder air(
      {
          .sampleNaNToZero = bool(shader_flags & SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO),
          .defuseFma = bool(shader_flags & SM50_SHADER_FLAG_DEFUSE_FMA),
          .noAtomicMinMax64 = bool(shader_flags & SM50_SHADER_FLAG_NO_ATOMIC_MINMAX64),
      },
      builder, nulldbg
  );

  setup_metal_version(module, metal_version);

  // the samples in the bounds, all without the test; none discards the pixel before anything else
  llvm::Value *bounded = nullptr;
  if (bounds_position != ~0u) {
    auto words = builder.CreateLoad(llvm::FixedVectorType::get(builder.getInt32Ty(), 2), function->getArg(bounds_enabled));
    auto enabled = builder.CreateICmpNE(builder.CreateExtractElement(words, uint64_t(0)), builder.getInt32(0));
    auto position = function->getArg(bounds_position);
    auto x = builder.CreateFPToUI(builder.CreateExtractElement(position, uint64_t(0)), builder.getInt32Ty());
    auto y = builder.CreateFPToUI(builder.CreateExtractElement(position, uint64_t(1)), builder.getInt32Ty());
    auto head = builder.GetInsertBlock();
    auto test = llvm::BasicBlock::Create(context, "depth_bounds", function, epilogue_bb);
    auto discard = llvm::BasicBlock::Create(context, "depth_bounds_discard", function, epilogue_bb);
    auto kept = llvm::BasicBlock::Create(context, "depth_bounds_kept", function, epilogue_bb);
    builder.CreateCondBr(enabled, test, kept);
    builder.SetInsertPoint(test);
    auto at = builder.CreateAdd(builder.CreateMul(y, builder.CreateExtractElement(words, uint64_t(1))), x);
    auto bytes = builder.CreateBitCast(
        function->getArg(bounds_mask),
        builder.getInt8Ty()->getPointerTo(function->getArg(bounds_mask)->getType()->getPointerAddressSpace())
    );
    auto samples = builder.CreateZExt(
        builder.CreateLoad(builder.getInt8Ty(), builder.CreateGEP(builder.getInt8Ty(), bytes, {at})), builder.getInt32Ty()
    );
    builder.CreateCondBr(builder.CreateICmpEQ(samples, builder.getInt32(0)), discard, kept);
    builder.SetInsertPoint(discard);
    air.CreateDiscard();
    builder.CreateBr(kept);
    builder.SetInsertPoint(kept);
    auto phi = builder.CreatePHI(builder.getInt32Ty(), 3);
    phi->addIncoming(builder.getInt32(~0u), head);
    phi->addIncoming(samples, test);
    phi->addIncoming(builder.getInt32(0), discard);
    bounded = phi;
    // the samples the shader or the pipeline keeps, of those
    epilogue >> [=](pvalue value) -> IRValue {
      return make_irvalue([=](struct context ctx) {
        auto &builder = ctx.builder;
        llvm::Value *kept = ctx.resource.coverage_mask_reg || ctx.pso_sample_mask != 0xffffffff
                                ? builder.CreateExtractValue(value, {bounds_coverage})
                                : builder.getInt32(~0u);
        return builder.CreateInsertValue(value, builder.CreateAnd(kept, bounded), {bounds_coverage});
      });
    };
  }

  std::vector<std::pair<llvm::Argument *, llvm::Value *>> conservative_inputs;
  llvm::SmallPtrSet<llvm::Instruction *, 32> conservative_own;
  if (conservative_position != ~0u) {
    pvalue window[SM50_CONSERVATIVE_VARYINGS];
    for (uint32_t i = 0; i < SM50_CONSERVATIVE_VARYINGS; i++)
      window[i] = function->getArg(conservative_window[i]);
    auto position = function->getArg(conservative_position);
    // what reads the inputs gets them divided by the 1/w Metal interpolates with them, and the position's w over it
    auto rcp_w = [&](uint32_t at) { return function->getArg(conservative_rcp_w[at]); };
    for (auto [index, at] : conservative_over_w) {
      auto input = function->getArg(index);
      if (at == 3)
        continue; // at each evaluation instead
      auto vector = llvm::dyn_cast<llvm::FixedVectorType>(input->getType());
      conservative_inputs.push_back(
          {input, builder.CreateFDiv(
                      input, vector ? builder.CreateVectorSplat(vector->getNumElements(), rcp_w(at)) : rcp_w(at)
                  )}
      );
    }
    conservative_inputs.push_back(
        {position,
         builder.CreateInsertElement(
             position, builder.CreateFMul(builder.CreateExtractElement(position, uint64_t(3)), rcp_w(0)), uint64_t(3)
         )}
    );
    // the grown triangles share the pixels on their edges, each with some of their samples
    if (~conservative_coverage) {
      auto all =
          builder.CreateSub(builder.CreateShl(builder.getInt32(1), air.CreateGetNumSamples()), builder.getInt32(1));
      conservative_inputs.push_back(
          {function->getArg(conservative_coverage),
           ~conservative_sample ? builder.CreateShl(builder.getInt32(1), function->getArg(conservative_sample)) : all}
      );
    }
    auto [culled, inside] = conservative_overlap(builder, air, position, window);
    resource_map.inner_coverage = builder.CreateZExt(inside, builder.getInt32Ty());
    auto discard = llvm::BasicBlock::Create(context, "conservative_discard", function, epilogue_bb);
    auto live = llvm::BasicBlock::Create(context, "conservative_live", function, epilogue_bb);
    builder.CreateCondBr(culled, discard, live);
    builder.SetInsertPoint(discard);
    air.CreateDiscard();
    builder.CreateBr(live);
    builder.SetInsertPoint(live);
    for (auto &block : *function)
      for (auto &instruction : block)
        conservative_own.insert(&instruction);
  }

  if (coarse)
    coarse->Enter(function, builder, air, resource_map);

  if (statistics) {
    // D3D11.3 20.4.7 counts executions even when an output depth fails the test.
    auto counter = function->getArg(*statistics);
    llvm::Value *counts = builder.getTrue();
    if (coverage != ~0u) {
      auto before = builder.CreateSub(builder.CreateShl(builder.getInt32(1), function->getArg(sample)), builder.getInt32(1));
      counts = builder.CreateICmpEQ(builder.CreateAnd(function->getArg(coverage), before), builder.getInt32(0));
    }
    auto low = air.CreateAtomicRMW(llvm::AtomicRMWInst::Add, counter, builder.CreateZExt(counts, builder.getInt32Ty()));
    air.CreateAtomicRMW(
        llvm::AtomicRMWInst::Add, builder.CreateConstGEP1_32(builder.getInt32Ty(), counter, 1),
        builder.CreateZExt(builder.CreateAnd(counts, builder.CreateICmpEQ(low, builder.getInt32(UINT32_MAX))),
                           builder.getInt32Ty()));
  }

  resource_map.input.ptr_int4 =
    builder.CreateAlloca(llvm::ArrayType::get(types._int4, max_input_register));
  resource_map.input.ptr_float4 = builder.CreateBitCast(
    resource_map.input.ptr_int4,
    llvm::ArrayType::get(types._float4, max_input_register)->getPointerTo()
  );
  resource_map.input_element_count = max_input_register;
  resource_map.output.ptr_int4 =
    builder.CreateAlloca(llvm::ArrayType::get(types._int4, max_output_register)
    );
  resource_map.output.ptr_float4 = builder.CreateBitCast(
    resource_map.output.ptr_int4,
    llvm::ArrayType::get(types._float4, max_output_register)->getPointerTo()
  );
  resource_map.output_element_count = max_output_register;

  setup_temp_register(shader_info, resource_map, types, module, builder);
  setup_immediate_constant_buffer(
    shader_info, resource_map, types, module, builder
  );

  struct context ctx{
      .builder = builder,
      .air = air,
      .binding = *binding_map,
      .llvm = context,
      .module = module,
      .function = function,
      .resource = resource_map,
      .types = types,
      .pso_sample_mask = pso_sample_mask,
      .shader_type = pShaderInternal->shader_type,
      .metal_version = metal_version,
      .simd_width = sm50_common ? sm50_common->simd_width : 0,
      .coherent_group_sync = pShaderInternal->groups_work_together && (shader_flags & SM50_SHADER_FLAG_COHERENT_GROUP_SYNC),
  };

  if (auto err = prologue.build(ctx).takeError()) {
    return err;
  }
  auto real_entry = convert_basicblocks(pShaderInternal->entry(), ctx, epilogue_bb);
  if (auto err = real_entry.takeError()) {
    return err;
  }
  builder.CreateBr(real_entry.get());

  builder.SetInsertPoint(epilogue_bb);
  if (coarse)
    coarse->Leave(builder, air, resource_map);
  auto epilogue_result = epilogue.build(ctx);
  if (auto err = epilogue_result.takeError()) {
    return err;
  }
  auto value = epilogue_result.get();
  if (value == nullptr) {
    builder.CreateRetVoid();
  } else {
    builder.CreateRet(value);
  }
  if (coarse)
    coarse->Finish();
  // the shader reads the conservative rasterizer's inputs, and divides what it evaluates of one over w by 1/w's there
  for (auto [input, value] : conservative_inputs)
    input->replaceUsesWithIf(value, [&](llvm::Use &use) {
      auto user = llvm::dyn_cast<llvm::Instruction>(use.getUser());
      return user && !conservative_own.contains(user);
    });
  for (auto [index, at] : conservative_over_w) {
    if (at != 3)
      continue;
    std::vector<llvm::CallInst *> evaluations;
    for (auto user : function->getArg(index)->users())
      if (auto call = llvm::dyn_cast<llvm::CallInst>(user);
          call && call->getCalledFunction() && call->getCalledFunction()->getName().startswith("air.interpolate_"))
        evaluations.push_back(call);
    for (auto call : evaluations) {
      auto rcp_w = llvm::cast<llvm::CallInst>(call->clone());
      rcp_w->setArgOperand(0, function->getArg(conservative_rcp_w[3]));
      rcp_w->insertAfter(call);
      builder.SetInsertPoint(rcp_w->getNextNode());
      auto divided = builder.CreateFDiv(call, rcp_w);
      call->replaceUsesWithIf(divided, [&](llvm::Use &use) { return use.getUser() != divided; });
    }
  }
  // the registers' allocas made after the depth bounds test split the entry block go back to it, where SROA promotes
  // them
  auto &entry = function->getEntryBlock();
  for (auto &block : *function)
    if (&block != &entry)
      for (auto &instruction : llvm::make_early_inc_range(block))
        if (auto alloca = llvm::dyn_cast<llvm::AllocaInst>(&instruction);
            alloca && llvm::isa<llvm::ConstantInt>(alloca->getArraySize()))
          alloca->moveBefore(&*entry.getFirstInsertionPt());

  module.getOrInsertNamedMetadata("air.fragment")
    ->addOperand(function_metadata);

  return llvm::Error::success();
};

llvm::Error convert_dxbc_compute_shader(
  SM50ShaderInternal *pShaderInternal, const char *name,
  llvm::LLVMContext &context, llvm::Module &module,
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs
) {
  using namespace microsoft;

  auto func_signature = pShaderInternal->func_signature; // copy
  auto shader_info = &(pShaderInternal->shader_info);

  SM50_SHADER_METAL_VERSION metal_version = SM50_SHADER_METAL_310;
  SM50_SHADER_FLAG shader_flags = {};
  SM50_SHADER_COMMON_DATA *sm50_common = nullptr;
  if (args_get_data<SM50_SHADER_COMMON, SM50_SHADER_COMMON_DATA>(pArgs, &sm50_common)) {
    metal_version = sm50_common->metal_version;
    shader_flags = sm50_common->flags;
  }
  SM50_SHADER_ROOT_SIGNATURE_DATA *rootsig = nullptr;
  args_get_data<SM50_SHADER_ROOT_SIGNATURE, SM50_SHADER_ROOT_SIGNATURE_DATA>(pArgs, &rootsig);

  IREffect prologue([](auto) { return std::monostate(); });
  IRValue epilogue([](struct context ctx) -> pvalue {
    auto retTy = ctx.function->getReturnType();
    if (retTy->isVoidTy()) {
      return nullptr;
    }
    return llvm::ConstantAggregateZero::get(retTy);
  });

  io_binding_map resource_map;
  air::AirType types(context);

  {
    SignatureContext sig_ctx(prologue, epilogue, func_signature, resource_map);
    for (auto &p : pShaderInternal->signature_handlers) {
      p(sig_ctx);
    }
  }

  auto binding_map = rootsig ? setup_binding_rootsig(
                                   shader_info, func_signature, module, D3D11_SB_COMPUTE_SHADER, rootsig->bytecode,
                                   rootsig->bytecode_length
                               )
                             : setup_binding_table2(shader_info, func_signature, module);
  if (!binding_map)
    return llvm::make_error<UnsupportedFeature>("invalid root signature or missing resource binding");
  setup_tgsm(shader_info, resource_map, types, module);

  auto [function, function_metadata] =
    func_signature.CreateFunction(name, context, module, 0, false);

  auto entry_bb = llvm::BasicBlock::Create(context, "entry", function);
  auto epilogue_bb = llvm::BasicBlock::Create(context, "epilogue", function);
  llvm::IRBuilder<> builder(entry_bb);
  llvm::raw_null_ostream nulldbg{};
  llvm::air::AIRBuilder air(
      {
          .sampleNaNToZero = bool(shader_flags & SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO),
          .defuseFma = bool(shader_flags & SM50_SHADER_FLAG_DEFUSE_FMA),
          .noAtomicMinMax64 = bool(shader_flags & SM50_SHADER_FLAG_NO_ATOMIC_MINMAX64),
      },
      builder, nulldbg
  );

  setup_metal_version(module, metal_version);
  setup_temp_register(shader_info, resource_map, types, module, builder);
  setup_immediate_constant_buffer(
    shader_info, resource_map, types, module, builder
  );

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
      .shader_type = pShaderInternal->shader_type,
      .metal_version = metal_version,
      .simd_width = sm50_common ? sm50_common->simd_width : 0,
      .coherent_group_sync = pShaderInternal->groups_work_together && (shader_flags & SM50_SHADER_FLAG_COHERENT_GROUP_SYNC),
  };

  if (auto err = prologue.build(ctx).takeError()) {
    return err;
  }
  auto real_entry = convert_basicblocks(pShaderInternal->entry(), ctx, epilogue_bb);
  if (auto err = real_entry.takeError()) {
    return err;
  }
  builder.CreateBr(real_entry.get());

  builder.SetInsertPoint(epilogue_bb);

  if (auto err = epilogue.build(ctx).takeError()) {
    return err;
  }
  builder.CreateRetVoid();

  module.getOrInsertNamedMetadata("air.kernel")->addOperand(function_metadata);

  return llvm::Error::success();
};

llvm::Error convert_dxbc_vertex_shader(
  SM50ShaderInternal *pShaderInternal, const char *name,
  llvm::LLVMContext &context, llvm::Module &module,
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs
) {
  using namespace microsoft;

  auto func_signature = pShaderInternal->func_signature; // copy
  auto shader_info = &(pShaderInternal->shader_info);

  uint32_t max_input_register = pShaderInternal->max_input_register;
  uint32_t max_output_register = pShaderInternal->max_output_register;
  SM50_SHADER_IA_INPUT_LAYOUT_DATA *ia_layout = nullptr;
  args_get_data<SM50_SHADER_IA_INPUT_LAYOUT, SM50_SHADER_IA_INPUT_LAYOUT_DATA>(pArgs, &ia_layout);
  SM50_SHADER_GS_PASS_THROUGH_DATA *gs_passthrough = nullptr;
  bool rasterization_disabled =
      args_get_data<SM50_SHADER_GS_PASS_THROUGH, SM50_SHADER_GS_PASS_THROUGH_DATA>(pArgs, &gs_passthrough) &&
      gs_passthrough->RasterizationDisabled;
  SM50_SHADER_METAL_VERSION metal_version = SM50_SHADER_METAL_310;
  SM50_SHADER_FLAG shader_flags = {};
  SM50_SHADER_COMMON_DATA *sm50_common = nullptr;
  if (args_get_data<SM50_SHADER_COMMON, SM50_SHADER_COMMON_DATA>(pArgs, &sm50_common)) {
    metal_version = sm50_common->metal_version;
    shader_flags = sm50_common->flags;
  }
  SM50_SHADER_ROOT_SIGNATURE_DATA *rootsig = nullptr;
  args_get_data<SM50_SHADER_ROOT_SIGNATURE, SM50_SHADER_ROOT_SIGNATURE_DATA>(pArgs, &rootsig);

  IREffect prologue([](auto) { return std::monostate(); });
  IRValue epilogue([](struct context ctx) -> pvalue {
    auto retTy = ctx.function->getReturnType();
    if (retTy->isVoidTy()) {
      return nullptr;
    }
    return llvm::ConstantAggregateZero::get(retTy);
  });

  io_binding_map resource_map;
  air::AirType types(context);

  {
    SignatureContext sig_ctx(prologue, epilogue, func_signature, resource_map);
    sig_ctx.ia_layout = ia_layout;
    sig_ctx.skip_vertex_output = rasterization_disabled;
    for (auto &p : pShaderInternal->signature_handlers) {
      p(sig_ctx);
    }

    for (auto& out : pShaderInternal->output_signature) {
      if(out.isSystemValue()) continue;
      auto types = pShaderInternal->register_types(out.reg(), out.componentType());
      for (unsigned c = 0; c < 4; c++)
        func_signature.DefineOutput(air::OutputVertex{.user = varying_name(out.reg(), c), .type = varying_type(types[c])});
    }
  }
  uint32_t vertex_idx = func_signature.DefineInput(air::InputVertexID{});
  uint32_t base_vertex_idx = func_signature.DefineInput(air::InputBaseVertex{});
  uint32_t instance_idx = func_signature.DefineInput(air::InputInstanceID{});
  uint32_t base_instance_idx =
    func_signature.DefineInput(air::InputBaseInstance{});

  uint32_t clip_distance_out_idx = ~0u;
  if (pShaderInternal->clip_distance_scalars.size()) {
    clip_distance_out_idx = func_signature.DefineOutput(
      air::OutputClipDistance{pShaderInternal->clip_distance_scalars.size()}
    );
  };

  auto binding_map = rootsig ? setup_binding_rootsig(
                                   shader_info, func_signature, module, D3D10_SB_VERTEX_SHADER, rootsig->bytecode,
                                   rootsig->bytecode_length
                               )
                             : setup_binding_table2(shader_info, func_signature, module);
  if (!binding_map)
    return llvm::make_error<UnsupportedFeature>("invalid root signature or missing resource binding");

  uint32_t rta_idx_out = ~0u;
  if (gs_passthrough && gs_passthrough->Data.RenderTargetArrayIndexReg != 255) {
    rta_idx_out =
      func_signature.DefineOutput(air::OutputRenderTargetArrayIndex{});
  }
  uint32_t va_idx_out = ~0u;
  if (gs_passthrough && gs_passthrough->Data.ViewportArrayIndexReg != 255) {
    va_idx_out = func_signature.DefineOutput(air::OutputViewportArrayIndex{});
  }

  auto [function, function_metadata] = func_signature.CreateFunction(
    name, context, module, 0,
    rasterization_disabled
  );

  auto entry_bb = llvm::BasicBlock::Create(context, "entry", function);
  auto epilogue_bb = llvm::BasicBlock::Create(context, "epilogue", function);
  llvm::IRBuilder<> builder(entry_bb);
  llvm::raw_null_ostream nulldbg{};
  llvm::air::AIRBuilder air(
      {
          .sampleNaNToZero = bool(shader_flags & SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO),
          .defuseFma = bool(shader_flags & SM50_SHADER_FLAG_DEFUSE_FMA),
          .noAtomicMinMax64 = bool(shader_flags & SM50_SHADER_FLAG_NO_ATOMIC_MINMAX64),
      },
      builder, nulldbg
  );

  setup_metal_version(module, metal_version);

  resource_map.vertex_id_with_base = function->getArg(vertex_idx);
  resource_map.base_vertex_id = function->getArg(base_vertex_idx);
  resource_map.instance_id_with_base = function->getArg(instance_idx);
  resource_map.base_instance_id = function->getArg(base_instance_idx);
  resource_map.vertex_id = builder.CreateSub(
    resource_map.vertex_id_with_base, resource_map.base_vertex_id
  );
  resource_map.instance_id = builder.CreateSub(
    resource_map.instance_id_with_base, resource_map.base_instance_id
  );

  resource_map.input.ptr_int4 =
    builder.CreateAlloca(llvm::ArrayType::get(types._int4, max_input_register));
  resource_map.input.ptr_float4 = builder.CreateBitCast(
    resource_map.input.ptr_int4,
    llvm::ArrayType::get(types._float4, max_input_register)->getPointerTo()
  );
  resource_map.input_element_count = max_input_register;
  resource_map.output.ptr_int4 =
    builder.CreateAlloca(llvm::ArrayType::get(types._int4, max_output_register)
    );
  resource_map.output.ptr_float4 = builder.CreateBitCast(
    resource_map.output.ptr_int4,
    llvm::ArrayType::get(types._float4, max_output_register)->getPointerTo()
  );
  resource_map.output_element_count = max_output_register;

  setup_temp_register(shader_info, resource_map, types, module, builder);
  setup_immediate_constant_buffer(
    shader_info, resource_map, types, module, builder
  );

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
      .shader_type = pShaderInternal->shader_type,
      .metal_version = metal_version,
      .simd_width = sm50_common ? sm50_common->simd_width : 0,
      .coherent_group_sync = pShaderInternal->groups_work_together && (shader_flags & SM50_SHADER_FLAG_COHERENT_GROUP_SYNC),
  };

  if (auto err = prologue.build(ctx).takeError()) {
    return err;
  }
  auto real_entry = convert_basicblocks(pShaderInternal->entry(), ctx, epilogue_bb);
  if (auto err = real_entry.takeError()) {
    return err;
  }
  builder.CreateBr(real_entry.get());

  builder.SetInsertPoint(epilogue_bb);
  auto epilogue_result = epilogue.build(ctx);
  if (auto err = epilogue_result.takeError()) {
    return err;
  }
  auto value = epilogue_result.get();

  if (pShaderInternal->clip_distance_scalars.size()) {
    auto clip_distance_ty = llvm::ArrayType::get(
      types._float, pShaderInternal->clip_distance_scalars.size()
    );
    pvalue clip_distance_array = llvm::ConstantAggregateZero::get(clip_distance_ty);
    unsigned clip_distance_idx = 0;
    for (auto &scalar : pShaderInternal->clip_distance_scalars) {
      auto src_ptr = builder.CreateGEP(
        resource_map.output.ptr_float4->getType()
          ->getNonOpaquePointerElementType(),
        resource_map.output.ptr_float4,
        {builder.getInt32(0), builder.getInt32(scalar.reg),
         builder.getInt32(scalar.component)}
      );
      clip_distance_array = builder.CreateInsertValue(
        clip_distance_array, builder.CreateLoad(types._float, src_ptr), {clip_distance_idx++}
      );
    }
    value = builder.CreateInsertValue(
      value, clip_distance_array, {clip_distance_out_idx}
    );
  };

  if (value == nullptr) {
    builder.CreateRetVoid();
  } else {
    if (rta_idx_out != ~0u) {
      auto src_ptr = builder.CreateGEP(
        resource_map.output.ptr_int4->getType()->getNonOpaquePointerElementType(
        ),
        resource_map.output.ptr_int4,
        {builder.getInt32(0),
         builder.getInt32(gs_passthrough->Data.RenderTargetArrayIndexReg),
         builder.getInt32(gs_passthrough->Data.RenderTargetArrayIndexComponent)}
      );
      value = builder.CreateInsertValue(
        value, builder.CreateLoad(types._int, src_ptr), {rta_idx_out}
      );
    }
    if (va_idx_out != ~0u) {
      auto src_ptr = builder.CreateGEP(
        resource_map.output.ptr_int4->getType()->getNonOpaquePointerElementType(
        ),
        resource_map.output.ptr_int4,
        {builder.getInt32(0),
         builder.getInt32(gs_passthrough->Data.ViewportArrayIndexReg),
         builder.getInt32(gs_passthrough->Data.ViewportArrayIndexComponent)}
      );
      value = builder.CreateInsertValue(
        value, builder.CreateLoad(types._int, src_ptr), {va_idx_out}
      );
    }
    builder.CreateRet(value);
  }

  module.getOrInsertNamedMetadata("air.vertex")->addOperand(function_metadata);
  return llvm::Error::success();
};

llvm::Error convertDXBC(
  sm50_shader_t pShader, const char *name, llvm::LLVMContext &context,
  llvm::Module &module, SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs
) {
  using namespace microsoft;

  auto pShaderInternal = (SM50ShaderInternal *)pShader;

  switch (pShaderInternal->shader_type) {
  case microsoft::D3D10_SB_PIXEL_SHADER:
    return convert_dxbc_pixel_shader(
      pShaderInternal, name, context, module, pArgs
    );
  case microsoft::D3D10_SB_VERTEX_SHADER:
    return convert_dxbc_vertex_shader(
      pShaderInternal, name, context, module, pArgs
    );
  case microsoft::D3D11_SB_COMPUTE_SHADER:
    return convert_dxbc_compute_shader(
      pShaderInternal, name, context, module, pArgs
    );
  case microsoft::D3D11_SB_HULL_SHADER:
  case microsoft::D3D11_SB_DOMAIN_SHADER:
    return llvm::make_error<UnsupportedFeature>(
      "Hull and domain shader cannot be independently converted."
    );
  case microsoft::D3D10_SB_GEOMETRY_SHADER:
    return llvm::make_error<UnsupportedFeature>(
      "Geometry shader cannot be independently converted."
    );
  case microsoft::D3D12_SB_MESH_SHADER:
  case microsoft::D3D12_SB_AMPLIFICATION_SHADER:
    return convert_dxil_mesh_stage(pShaderInternal, name, context, module, pArgs);
  case microsoft::D3D11_SB_RESERVED0:
    break;
  }
  if (pShaderInternal->shader_type == kShaderLibrary)
    return convert_dxil_ray_shader(pShaderInternal, name, context, module, pArgs);
  return llvm::make_error<UnsupportedFeature>("Not supported shader type");
};
} // namespace mullion::dxbc

bool CheckGSBBIsPassThrough(mullion::dxbc::BasicBlock *bb) {
  using namespace mullion::dxbc;
  if (bb->instructions.any(patterns{
        [](InstNop &) { return false; }, [](InstMov &mov) { return false; },
        [](InstEmit &) { return false; }, [](InstCut &) { return false; },
        [](auto &) { return true; }
      }))
    return false;

  return std::visit(
    patterns{
      [](mullion::dxbc::BasicBlockReturn) { return true; },
      [](mullion::dxbc::BasicBlockUnconditionalBranch uncond) {
        return CheckGSBBIsPassThrough(uncond.target);
      },
      [](auto) { return false; }
    },
    bb->target
  );
}

bool CheckGSSignatureIsPassThrough(
  microsoft::CSignatureParser &input, microsoft::CSignatureParser5 &output,
  MTL_GEOMETRY_SHADER_PASS_THROUGH &data
) {
  using namespace microsoft;

  data.RenderTargetArrayIndexComponent = 255;
  data.RenderTargetArrayIndexReg = 255;
  data.ViewportArrayIndexComponent = 255;
  data.ViewportArrayIndexReg = 255;

  if (output.NumStreams() > 1) {
    return false;
  }
  if (output.RasterizedStream()) {
    return false;
  }

  const D3D11_SIGNATURE_PARAMETER *input_parameters, *output_parameters;
  input.GetParameters(&input_parameters);
  output.Signature(0)->GetParameters(&output_parameters);
  size_t num_input, num_output;
  num_input = input.GetNumParameters();
  num_output = output.Signature(0)->GetNumParameters();
  if (num_output > num_input) {
    // at least it is a non-trivial shader that generates new data
    return false;
  }
  bool has_match_record;
  for (unsigned i = 0; i < num_output; i++) {
    has_match_record = false;
    const D3D11_SIGNATURE_PARAMETER &out = output_parameters[i];
    for (unsigned j = 0; j < num_input; j++) {
      const D3D11_SIGNATURE_PARAMETER &in = input_parameters[j];
      if (out.Register == in.Register && (out.Mask & in.Mask) == out.Mask) {
        if (out.SystemValue == D3D10_SB_NAME_RENDER_TARGET_ARRAY_INDEX) {
          data.RenderTargetArrayIndexReg = out.Register;
          data.RenderTargetArrayIndexComponent = __builtin_ctz(out.Mask);
        } else if (out.SystemValue == D3D10_SB_NAME_VIEWPORT_ARRAY_INDEX) {
          data.ViewportArrayIndexReg = out.Register;
          data.ViewportArrayIndexComponent = __builtin_ctz(out.Mask);
        } else {
          if (out.SemanticIndex != in.SemanticIndex) {
            return false;
          }
          if (strcasecmp(out.SemanticName, in.SemanticName)) {
            return false;
          }
        }
        has_match_record = true;
        break;
      }
    }
    if (!has_match_record) {
      return false;
    }
  }

  return true;
};

AIRCONV_API int SM50Initialize(
  const void *pBytecode, size_t BytecodeSize, sm50_shader_t *ppShader,
  MTL_SHADER_REFLECTION *pRefl, sm50_error_t *ppError
) {
  using namespace microsoft;
  using namespace mullion::dxbc;
  using namespace mullion::air;
  using namespace mullion::shader::common;
  if (ppError) {
    *ppError = nullptr;
  }
  auto errorObj = new SM50ErrorInternal();
  llvm::raw_svector_ostream errorOut(errorObj->buf);

  if (ppShader == nullptr) {
    errorOut << "ppShader can not be null\0";
    *ppError = (sm50_error_t)errorObj;
    return 1;
  }

  CDXBCParser DXBCParser;
  if (DXBCParser.ReadDXBC(pBytecode, BytecodeSize) != S_OK) {
    errorOut << "Invalid DXBC bytecode\0";
    *ppError = (sm50_error_t)errorObj;
    return 1;
  }

  UINT codeBlobIdx = DXBCParser.FindNextMatchingBlob(DXBC_GenericShaderEx);
  if (codeBlobIdx == DXBC_BLOB_NOT_FOUND) {
    codeBlobIdx = DXBCParser.FindNextMatchingBlob(DXBC_GenericShader);
  }
  UINT dxilBlobIdx = DXBCParser.FindNextMatchingBlob(DXBC_DXIL);
  if (codeBlobIdx == DXBC_BLOB_NOT_FOUND && dxilBlobIdx == DXBC_BLOB_NOT_FOUND) {
    errorOut << "Invalid DXBC bytecode: shader blob not found\0";
    *ppError = (sm50_error_t)errorObj;
    return 1;
  }

  // DxilProgramHeader's version word has the shader's kind: a library has no signatures, only functions
  bool library = dxilBlobIdx != DXBC_BLOB_NOT_FOUND &&
                 *(const uint32_t *)DXBCParser.GetBlob(dxilBlobIdx) >> 16 == kShaderLibrary;
  CSignatureParser inputParser;
  if (!library && DXBCGetInputSignature(pBytecode, &inputParser) != S_OK) {
    errorOut << "Invalid DXBC bytecode: input signature not found\0";
    *ppError = (sm50_error_t)errorObj;
    return 1;
  }
  CSignatureParser5 outputParser;
  if (!library && DXBCGetOutputSignature(pBytecode, &outputParser) != S_OK) {
    errorOut << "Invalid DXBC bytecode: output signature not found\0";
    *ppError = (sm50_error_t)errorObj;
    return 1;
  }

  auto sm50_shader = new SM50ShaderInternal();
  auto shader_info = &(sm50_shader->shader_info);

  if (!library) {
    const D3D11_SIGNATURE_PARAMETER *parameters;
    outputParser.RastSignature()->GetParameters(&parameters);
    for (unsigned i = 0; i < outputParser.RastSignature()->GetNumParameters(); i++) {
      sm50_shader->output_signature.push_back(Signature(parameters[i]));
    }
  }

  if (dxilBlobIdx != DXBC_BLOB_NOT_FOUND) {
    // hull and domain shaders carry one; it names each tessellation factor register
    CSignatureParser patchParser;
    DXBCGetPatchConstantSignature(pBytecode, &patchParser);
    if (auto err = read_dxil(DXBCParser.GetBlob(dxilBlobIdx), sm50_shader, inputParser, outputParser, patchParser)) {
      errorOut << llvm::toString(std::move(err));
      delete sm50_shader;
      *ppError = (sm50_error_t)errorObj;
      return 1;
    }
  } else {
    // 1. Collect information about the shader.
    D3D10ShaderBinary::CShaderCodeParser CodeParser((CShaderToken *)DXBCParser.GetBlob(codeBlobIdx));
    sm50_shader->shader_type = CodeParser.ShaderType();
    sm50_shader->bbs = read_control_flow(CodeParser, sm50_shader, inputParser, outputParser);
  }

  // a library's shaders bind their resources through root signatures only
  if (library) {
    *ppShader = sm50_shader;
    return 0;
  }

  auto &binding_table = shader_info->binding_table;
  auto &binding_table_cbuffer = shader_info->binding_table_cbuffer;

  uint16_t binding_cbuffer_mask = 0;
  uint16_t binding_sampler_mask = 0;
  uint64_t binding_uav_mask = 0;
  uint64_t binding_srv_hi_mask = 0;
  uint64_t binding_srv_lo_mask = 0;

  for (auto &[range_id, cbv] : shader_info->cbufferMap) {
    // TODO: abstract SM 5.0 binding
    cbv.arg_index = binding_table_cbuffer.DefineBuffer(
      "cb" + std::to_string(range_id), AddressSpace::constant,
      MemoryAccess::read, msl_uint4,
      GetArgumentIndex(SM50BindingType::ConstantBuffer, range_id)
    );
    cbv.arg_metadata_index = binding_table_cbuffer.DefineInteger64(
      "meta_cb" + std::to_string(range_id),
      GetArgumentIndex(SM50BindingType::ConstantBuffer, range_id) + 1
    );
    sm50_shader->args_reflection_cbuffer.push_back({
      .Type = SM50BindingType::ConstantBuffer,
      .SM50BindingSlot = range_id,
      .Flags =
        MTL_SM50_SHADER_ARGUMENT_BUFFER | MTL_SM50_SHADER_ARGUMENT_READ_ACCESS,
      .StructurePtrOffset = cbv.arg_index,
    });
    binding_cbuffer_mask |= (1 << range_id);
  }
  for (auto &[range_id, sampler] : shader_info->samplerMap) {
    // TODO: abstract SM 5.0 binding
    auto attr_index = GetArgumentIndex(SM50BindingType::Sampler, range_id);
    sampler.arg_index =
      binding_table.DefineSampler("s" + std::to_string(range_id), attr_index);
    sampler.arg_cube_index =
      binding_table.DefineSampler("cube_s" + std::to_string(range_id), attr_index + 1);
    sampler.arg_metadata_index = binding_table.DefineInteger64(
      "meta_s" + std::to_string(range_id), attr_index + 2
    );
    sm50_shader->args_reflection.push_back({
      .Type = SM50BindingType::Sampler,
      .SM50BindingSlot = range_id,
      .Flags = (MTL_SM50_SHADER_ARGUMENT_FLAG)0,
      .StructurePtrOffset = sampler.arg_index,
    });
    binding_sampler_mask |= (1 << range_id);
  }
  for (auto &[range_id, srv] : shader_info->srvMap) {
    auto attr_index = GetArgumentIndex(SM50BindingType::SRV, range_id);
    if (srv.resource_type != ResourceType::NonApplicable) {
      // TODO: abstract SM 5.0 binding
      auto access = srv.sampled ? MemoryAccess::sample : MemoryAccess::read;
      auto texture_kind = to_air_resource_type(srv.resource_type, srv.compared);
      auto scaler_type = to_air_scaler_type(srv.scaler_type);
      srv.arg_index = binding_table.DefineTexture(
        "t" + std::to_string(range_id), texture_kind, access, scaler_type,
        attr_index
      );
    } else {
      srv.arg_index = binding_table.DefineBuffer(
        "t" + std::to_string(range_id),
        AddressSpace::device, // it needs to be `device` since `constant` has
                              // size and alignment restriction
        MemoryAccess::read, msl_uint, attr_index
      );
    }
    srv.arg_metadata_index = binding_table.DefineInteger64(
      "meta_t" + std::to_string(range_id), attr_index + 1
    );
    MTL_SM50_SHADER_ARGUMENT_FLAG flags = (MTL_SM50_SHADER_ARGUMENT_FLAG)0;
    if (srv.resource_type == ResourceType::NonApplicable) {
      flags |= MTL_SM50_SHADER_ARGUMENT_ELEMENT_WIDTH |
               MTL_SM50_SHADER_ARGUMENT_BUFFER;
    } else if (srv.resource_type == ResourceType::TextureBuffer) {
      flags |= MTL_SM50_SHADER_ARGUMENT_TBUFFER_OFFSET |
               MTL_SM50_SHADER_ARGUMENT_TEXTURE;
    } else {
      flags |= MTL_SM50_SHADER_ARGUMENT_TEXTURE_MINLOD_CLAMP |
               MTL_SM50_SHADER_ARGUMENT_TEXTURE;
      if (llvm::air::AIRBuilder::getTextureInfo(to_air_resource_type(srv.resource_type, srv.compared)).is_ms)
        flags |= MTL_SM50_SHADER_ARGUMENT_TEXTURE_MULTISAMPLE;
      switch (srv.resource_type) {
      case ResourceType::Texture1DArray:
      case ResourceType::Texture2DArray:
      case ResourceType::Texture2DMultisampledArray:
      case ResourceType::TextureCubeArray:
        flags |= MTL_SM50_SHADER_ARGUMENT_TEXTURE_ARRAY;
        break;
      default:
        break;
      }
    }
    if (srv.read || srv.sampled || srv.compared) {
      flags |= MTL_SM50_SHADER_ARGUMENT_READ_ACCESS;
    }
    sm50_shader->args_reflection.push_back({
      .Type = SM50BindingType::SRV,
      .SM50BindingSlot = range_id,
      .Flags = flags,
      .StructurePtrOffset = srv.arg_index,
    });
    if (range_id & 64) {
      binding_srv_hi_mask |= (1ULL << (range_id - 64));
    } else {
      binding_srv_lo_mask |= (1ULL << range_id);
    }
  }
  for (auto &[range_id, uav] : shader_info->uavMap) {
    auto access =
      uav.written ? (uav.read ? MemoryAccess::read_write : MemoryAccess::write)
                  : MemoryAccess::read;
    auto attr_index = GetArgumentIndex(SM50BindingType::UAV, range_id);
    if (uav.resource_type != ResourceType::NonApplicable) {
      auto texture_kind = to_air_resource_type(uav.resource_type);
      auto scaler_type = to_air_scaler_type(uav.scaler_type);
      uav.arg_index = binding_table.DefineTexture(
        "u" + std::to_string(range_id), texture_kind, access, scaler_type,
        attr_index, uav.rasterizer_order ? std::optional(1) : std::nullopt
      );
    } else {
      uav.arg_index = binding_table.DefineBuffer(
        "u" + std::to_string(range_id), AddressSpace::device, access, msl_uint,
        attr_index, uav.rasterizer_order ? std::optional(1) : std::nullopt
      );
    }
    uav.arg_metadata_index = binding_table.DefineInteger64(
      "meta_u" + std::to_string(range_id), attr_index + 1
    );
    if (uav.with_counter) {
      uav.arg_counter_index = binding_table.DefineBuffer(
        "counter" + std::to_string(range_id), AddressSpace::device,
        MemoryAccess::read_write, msl_uint, attr_index + 2,
        uav.rasterizer_order ? std::optional(1) : std::nullopt
      );
    }
    MTL_SM50_SHADER_ARGUMENT_FLAG flags = (MTL_SM50_SHADER_ARGUMENT_FLAG)0;
    if (uav.resource_type == ResourceType::NonApplicable) {
      flags |= MTL_SM50_SHADER_ARGUMENT_ELEMENT_WIDTH |
               MTL_SM50_SHADER_ARGUMENT_BUFFER;
    } else if (uav.resource_type == ResourceType::TextureBuffer) {
      flags |= MTL_SM50_SHADER_ARGUMENT_TBUFFER_OFFSET |
               MTL_SM50_SHADER_ARGUMENT_TEXTURE;
    } else {
      flags |= MTL_SM50_SHADER_ARGUMENT_TEXTURE_MINLOD_CLAMP |
               MTL_SM50_SHADER_ARGUMENT_TEXTURE;
      switch (uav.resource_type) {
      case ResourceType::Texture1DArray:
      case ResourceType::Texture2DArray:
      case ResourceType::Texture2DMultisampledArray:
      case ResourceType::TextureCubeArray:
        flags |= MTL_SM50_SHADER_ARGUMENT_TEXTURE_ARRAY;
        break;
      default:
        break;
      }
    }
    if (uav.read) {
      flags |= MTL_SM50_SHADER_ARGUMENT_READ_ACCESS;
    }
    if (uav.written) {
      flags |= MTL_SM50_SHADER_ARGUMENT_WRITE_ACCESS;
    }
    if (uav.with_counter) {
      flags |= MTL_SM50_SHADER_ARGUMENT_UAV_COUNTER;
    }
    sm50_shader->args_reflection.push_back({
      .Type = SM50BindingType::UAV,
      .SM50BindingSlot = range_id,
      .Flags = flags,
      .StructurePtrOffset = uav.arg_index,
    });
    binding_uav_mask |= (1ULL << range_id);
  }

  declare_dispatch_origin(sm50_shader);

  if (sm50_shader->shader_type == microsoft::D3D11_SB_HULL_SHADER &&
      !sm50_shader->shader_info.no_control_point_phase_passthrough) {
    assert(sm50_shader->max_output_register == 0);
    uint32_t num_input_params = inputParser.GetNumParameters();
    D3D11_SIGNATURE_PARAMETER const * params;
    inputParser.GetParameters(&params);
    for (unsigned i = 0; i < num_input_params; i++) {
      sm50_shader->max_output_register = std::max(
        sm50_shader->max_output_register,
        params[i].Register + 1
      );
    }
  }

  if (pRefl) {
    pRefl->ConstanttBufferTableBindIndex =
      sm50_shader->args_reflection_cbuffer.size() > 0 ? 29 : ~0u;
    pRefl->ArgumentBufferBindIndex =
      sm50_shader->args_reflection.size() > 0 ? 30 : ~0u;
    pRefl->ConstantBufferSlotMask = binding_cbuffer_mask;
    pRefl->SamplerSlotMask = binding_sampler_mask;
    pRefl->SRVSlotMaskHi = binding_srv_hi_mask;
    pRefl->SRVSlotMaskLo = binding_srv_lo_mask;
    pRefl->UAVSlotMask = binding_uav_mask;
    pRefl->NumConstantBuffers = sm50_shader->args_reflection_cbuffer.size();
    pRefl->NumArguments = sm50_shader->args_reflection.size();
    // a compute shader's threadgroup size, as a mesh or amplification shader's
    if (sm50_shader->shader_type == microsoft::D3D11_SB_COMPUTE_SHADER ||
        sm50_shader->shader_type == microsoft::D3D12_SB_MESH_SHADER ||
        sm50_shader->shader_type == microsoft::D3D12_SB_AMPLIFICATION_SHADER) {
      pRefl->ThreadgroupSize[0] = sm50_shader->threadgroup_size[0];
      pRefl->ThreadgroupSize[1] = sm50_shader->threadgroup_size[1];
      pRefl->ThreadgroupSize[2] = sm50_shader->threadgroup_size[2];
    }
    pRefl->GroupsWorkTogether = sm50_shader->groups_work_together;
    pRefl->DeviceAtomics64 = sm50_shader->device_atomics64;
    // a mesh shader's conservative fans: its outputs wait in threadgroup memory, 16 KB of it, for a mesh of fans of
    // its triangles, each vertex its components and the window coordinates, each primitive its own and four words
    pRefl->MeshMaxPrimitives = sm50_shader->mesh_max_primitives;
    pRefl->MeshConservativeFans = 0;
    if (sm50_shader->shader_type == microsoft::D3D12_SB_MESH_SHADER && !sm50_shader->mesh_lines &&
        !sm50_shader->writes_resources) {
      uint32_t components[2] = {};
      for (uint32_t primitive = 0; primitive < 2; primitive++)
        for (auto &e : sm50_shader->mesh_outputs[primitive])
          components[primitive] += e.rows * e.cols;
      uint32_t staged = sm50_shader->mesh_max_vertices * (components[0] + 1) * 4 +
                        sm50_shader->mesh_max_primitives * (components[1] * 4 + 8);
      uint32_t vertex_size = (components[0] + SM50_CONSERVATIVE_VARYINGS) * 4;
      uint32_t fans = staged <= mullion::dxbc::conservative_stash_limit
                          ? std::min({256u / 9, 512u / 7, std::max(sm50_shader->mesh_max_primitives, 1u)})
                          : 0;
      while (fans && mullion::dxbc::mesh_size(9 * fans, vertex_size, 7 * fans * (4 + components[1])) >
                         mullion::dxbc::mesh_size_limit)
        fans--;
      pRefl->MeshConservativeFans = fans;
    }
    if (sm50_shader->shader_type == microsoft::D3D11_SB_HULL_SHADER) {
      auto threads_per_patch = next_pow2(sm50_shader->hull_maximum_threads_per_patch);
      if (threads_per_patch > 32) {
        errorOut << "Threadgroup size of tessellation pipeline is too large.";
        *ppError = (sm50_error_t)errorObj;
        return 1;
      }
      auto patch_per_group = 32 / threads_per_patch;
      float max_tesselation_factor = sm50_shader->max_tesselation_factor;

      while (estimate_payload_size(sm50_shader, max_tesselation_factor, patch_per_group) > 16384) {
        if (patch_per_group == 1) {
          if (max_tesselation_factor > 1.0f) {
            max_tesselation_factor = std::max(max_tesselation_factor - 2.0f, 1.0f);
          } else {
            errorOut << "Payload size of tessellation pipeline is too large.";
            *ppError = (sm50_error_t)errorObj;
            return 1;
          }
        } else {
          patch_per_group = patch_per_group >> 1;
        }
      }
      sm50_shader->hull_maximum_threads_per_patch = 32 / patch_per_group;
      sm50_shader->max_tesselation_factor = max_tesselation_factor;

      pRefl->ThreadsPerPatch = sm50_shader->hull_maximum_threads_per_patch;
      pRefl->Tessellator = {
        .Partition = sm50_shader->tessellation_partition,
        .MaxFactor = sm50_shader->max_tesselation_factor,
        .OutputPrimitive = (MTL_TESSELLATOR_OUTPUT_PRIMITIVE
        )sm50_shader->tessellator_output_primitive,
        .Domain = sm50_shader->tessellation_domain,
      };
    }
    if (sm50_shader->shader_type == microsoft::D3D11_SB_DOMAIN_SHADER) {
      // every output register is a float4 there, and each clip distance a float
      pRefl->PostTessellator = {
          .MeshVertexSize = uint32_t(
              sm50_shader->max_output_register * sizeof(float[4]) +
              sm50_shader->clip_distance_scalars.size() * sizeof(float)
          )
      };
    }
    if (sm50_shader->shader_type == microsoft::D3D10_SB_GEOMETRY_SHADER) {
      if (binding_cbuffer_mask || binding_sampler_mask || binding_uav_mask ||
          binding_srv_hi_mask || binding_srv_lo_mask ||
          !CheckGSBBIsPassThrough(sm50_shader->entry())) {
        pRefl->GeometryShader.GSPassThrough = ~0u;
      } else {
        CheckGSSignatureIsPassThrough(
          inputParser, outputParser, pRefl->GeometryShader.Data
        );
      }
      pRefl->GeometryShader.Primitive = sm50_shader->gs_input_primitive;
      pRefl->GeometryShader.InstanceCount = sm50_shader->gs_instance_count;
      // a fan's vertex: the position, the varyings and the window coordinates, and clip distances; its triangles
      // carry what Metal's primitives may, counted as four words each. Metal has 256 vertices and 512 primitives
      uint32_t fans = sm50_shader->gs_max_vertex_output >= 3 ? sm50_shader->gs_max_vertex_output - 2 : 0;
      uint32_t vertex_size = sizeof(float[4]) + (sm50_shader->num_mesh_vertex_data + SM50_CONSERVATIVE_VARYINGS) * 4 +
                             sm50_shader->clip_distance_scalars.size() * sizeof(float);
      pRefl->GeometryShader.ConservativeFits =
          sm50_shader->gs_output_topology == microsoft::D3D10_SB_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP && fans &&
          9 * fans <= 256 && 7 * fans <= 512 &&
          mullion::dxbc::mesh_size(9 * fans, vertex_size, 7 * fans * 4) <= mullion::dxbc::mesh_size_limit;
    }
    if (sm50_shader->shader_type == microsoft::D3D10_SB_PIXEL_SHADER) {
      pRefl->PixelShader = {sm50_shader->pso_valid_output_reg_mask, sm50_shader->ps_has_coverage_output, 0, 0, 0,
                            sm50_shader->ps_flat_input_mask, sm50_shader->ps_noperspective_input_mask};
      // EvaluateAttribute* evaluates where the shader chooses, which coarse shading does not move
      pRefl->PixelShader.FineShading = sm50_shader->ps_fine_shading || sm50_shader->shader_info.pull_mode_reg_mask;
      for (auto &out : sm50_shader->output_signature) {
        if (out.reg() >= std::size(SM50_SHADER_PSO_PIXEL_SHADER_DATA{}.pixel_formats))
          continue;
        pRefl->PixelShader.SintRenderTargets |=
            uint32_t(out.componentType() == RegisterComponentType::Int) << out.reg();
        pRefl->PixelShader.UintRenderTargets |=
            uint32_t(out.componentType() == RegisterComponentType::Uint) << out.reg();
      }
    }
    pRefl->ShadingRateOutput = 0;
    for (auto &out : sm50_shader->output_signature)
      if (out.systemValue() == D3D12_SB_NAME_SHADING_RATE &&
          (sm50_shader->shader_type == microsoft::D3D10_SB_VERTEX_SHADER ||
           sm50_shader->shader_type == microsoft::D3D10_SB_GEOMETRY_SHADER))
        pRefl->ShadingRateOutput = 1 + out.reg() * 4 + std::countr_zero(out.mask());
    pRefl->NumOutputElement = sm50_shader->max_output_register;
    pRefl->CullDistances = sm50_shader->cull_distance_scalars.size();
    pRefl->ArgumentTableQwords = binding_table.Size();
  }

  *ppShader = sm50_shader;
  return 0;
};

AIRCONV_API void SM50GetArgumentsInfo(
  sm50_shader_t pShader, struct MTL_SM50_SHADER_ARGUMENT *pConstantBuffers,
  struct MTL_SM50_SHADER_ARGUMENT *pArguments
) {
  auto sm50_shader = (mullion::dxbc::SM50ShaderInternal *)pShader;
  if (pConstantBuffers && sm50_shader->args_reflection_cbuffer.size())
    memcpy(
      pConstantBuffers, sm50_shader->args_reflection_cbuffer.data(),
      sm50_shader->args_reflection_cbuffer.size() *
        sizeof(struct MTL_SM50_SHADER_ARGUMENT)
    );
  if (pArguments && sm50_shader->args_reflection.size())
    memcpy(
      pArguments, sm50_shader->args_reflection.data(),
      sm50_shader->args_reflection.size() *
        sizeof(struct MTL_SM50_SHADER_ARGUMENT)
    );
}

AIRCONV_API int SM50GetRayShader(
  sm50_shader_t pShader, uint32_t Index, char *pName, size_t NameSize, struct SM50_RAY_SHADER_INFO *pInfo
) {
  auto &shaders = ((mullion::dxbc::SM50ShaderInternal *)pShader)->ray_shaders;
  if (Index >= shaders.size())
    return 0;
  if (NameSize)
    pName[shaders[Index].name.copy(pName, NameSize - 1)] = 0;
  *pInfo = shaders[Index].info;
  return 1;
}

AIRCONV_API void SM50Destroy(sm50_shader_t pShader) {
  delete (mullion::dxbc::SM50ShaderInternal *)pShader;
}

AIRCONV_API int SM50Compile(
  sm50_shader_t pShader, SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
) {
  using namespace llvm;
  using namespace mullion;

  if (ppError) {
    *ppError = nullptr;
  }
  auto errorObj = new SM50ErrorInternal();
  llvm::raw_svector_ostream errorOut(errorObj->buf);
  if (ppBitcode == nullptr) {
    errorOut << "ppBitcode can not be null\0";
    *ppError = (sm50_error_t)errorObj;
    return 1;
  }

  // pArgs is ignored for now
  LLVMContext context;

  context.setOpaquePointers(false); // I suspect Metal uses LLVM 14...

  auto &shader_info = ((mullion::dxbc::SM50ShaderInternal *)pShader)->shader_info;

  auto pModule = std::make_unique<Module>("shader.air", context);
  initializeModule(*pModule);

  if (auto err = mullion::dxbc::convertDXBC(
        pShader, FunctionName, context, *pModule, pArgs
      )) {
    int result = SM50_COMPILE_FAILED;
    llvm::handleAllErrors(std::move(err), [&](const llvm::ErrorInfoBase &error) {
      if (error.convertToErrorCode() == std::errc::no_such_file_or_directory)
        result = SM50_COMPILE_UNRESOLVED;
      error.log(errorOut);
    });
    *ppError = (sm50_error_t)errorObj;
    return result;
  }

  if (shader_info.use_msad)
    linkMSAD(*pModule);
  if (shader_info.use_samplepos)
    linkSamplePos(*pModule);

  if (!runOptimizationPasses(*pModule, errorOut)) {
    *ppError = (sm50_error_t)errorObj;
    return 1;
  }

  // Serialize AIR
  auto compiled = new SM50CompiledBitcodeInternal();

  raw_svector_ostream OS(compiled->vec);

  metallib::MetallibWriter writer;

  writer.Write(*pModule, OS);

  pModule.reset();

  *ppBitcode = (sm50_bitcode_t)compiled;
  return 0;
}

AIRCONV_API int SM50CompileTessellationPipelineHull(
  sm50_shader_t pVertexShader, sm50_shader_t pHullShader,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pHullShaderArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
) {
  using namespace llvm;
  using namespace mullion;

  if (ppError) {
    *ppError = nullptr;
  }
  auto errorObj = new SM50ErrorInternal();
  llvm::raw_svector_ostream errorOut(errorObj->buf);
  if (ppBitcode == nullptr) {
    errorOut << "ppBitcode can not be null\0";
    *ppError = (sm50_error_t)errorObj;
    return 1;
  }

  // pArgs is ignored for now
  LLVMContext context;

  context.setOpaquePointers(false); // I suspect Metal uses LLVM 14...

  auto &shader_info =
    ((mullion::dxbc::SM50ShaderInternal *)pHullShader)->shader_info;

  auto pModule = std::make_unique<Module>("shader.air", context);
  initializeModule(*pModule);

  if (auto err = mullion::dxbc::convert_dxbc_vertex_hull_shader(
        (dxbc::SM50ShaderInternal *)pVertexShader, (dxbc::SM50ShaderInternal *)pHullShader, 
        FunctionName, context, *pModule, pHullShaderArgs
      )) {
    errorOut << llvm::toString(std::move(err));
    *ppError = (sm50_error_t)errorObj;
    return 1;
  }

  if (shader_info.use_msad)
    linkMSAD(*pModule);
  if (shader_info.use_samplepos)
    linkSamplePos(*pModule);
  linkTessellation(*pModule);

  if (!runOptimizationPasses(*pModule, errorOut)) {
    *ppError = (sm50_error_t)errorObj;
    return 1;
  }

  // Serialize AIR
  auto compiled = new SM50CompiledBitcodeInternal();

  raw_svector_ostream OS(compiled->vec);

  metallib::MetallibWriter writer;

  writer.Write(*pModule, OS);

  pModule.reset();

  *ppBitcode = (sm50_bitcode_t)compiled;
  return 0;
}

AIRCONV_API int SM50CompileTessellationPipelineDomain(
  sm50_shader_t pHullShader, sm50_shader_t pDomainShader,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pDomainShaderArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
) {
  using namespace llvm;
  using namespace mullion;

  if (ppError) {
    *ppError = nullptr;
  }
  auto errorObj = new SM50ErrorInternal();
  llvm::raw_svector_ostream errorOut(errorObj->buf);
  if (ppBitcode == nullptr) {
    errorOut << "ppBitcode can not be null\0";
    *ppError = (sm50_error_t)errorObj;
    return 1;
  }

  // pArgs is ignored for now
  LLVMContext context;

  context.setOpaquePointers(false); // I suspect Metal uses LLVM 14...

  auto &shader_info =
    ((mullion::dxbc::SM50ShaderInternal *)pDomainShader)->shader_info;
  // with a geometry shader after it the domain shader runs in the geometry shader's function
  SM50_SHADER_PSO_TESSELLATOR_DATA *pso_tess = nullptr;
  mullion::dxbc::args_get_data<SM50_SHADER_PSO_TESSELLATOR, SM50_SHADER_PSO_TESSELLATOR_DATA>(pDomainShaderArgs, &pso_tess);
  auto pGeometryShader = pso_tess ? (dxbc::SM50ShaderInternal *)pso_tess->geometry : nullptr;

  auto pModule = std::make_unique<Module>("shader.air", context);
  initializeModule(*pModule);

  if (auto err = pGeometryShader ? mullion::dxbc::convert_dxbc_geometry_shader(
                                       pGeometryShader, FunctionName, (dxbc::SM50ShaderInternal *)pDomainShader,
                                       context, *pModule, pDomainShaderArgs, (dxbc::SM50ShaderInternal *)pHullShader
                                   )
                                 : mullion::dxbc::convert_dxbc_tesselator_domain_shader(
                                       (dxbc::SM50ShaderInternal *)pDomainShader, FunctionName,
                                       (dxbc::SM50ShaderInternal *)pHullShader, context, *pModule, pDomainShaderArgs
                                   )) {
    errorOut << llvm::toString(std::move(err));
    *ppError = (sm50_error_t)errorObj;
    return 1;
  }

  if (shader_info.use_msad || (pGeometryShader && pGeometryShader->shader_info.use_msad))
    linkMSAD(*pModule);
  if (shader_info.use_samplepos || (pGeometryShader && pGeometryShader->shader_info.use_samplepos))
    linkSamplePos(*pModule);
  linkTessellation(*pModule);
  
  if (!runOptimizationPasses(*pModule, errorOut)) {
    *ppError = (sm50_error_t)errorObj;
    return 1;
  }

  // Serialize AIR
  auto compiled = new SM50CompiledBitcodeInternal();

  raw_svector_ostream OS(compiled->vec);

  metallib::MetallibWriter writer;

  writer.Write(*pModule, OS);

  pModule.reset();

  *ppBitcode = (sm50_bitcode_t)compiled;
  return 0;
}

AIRCONV_API int SM50CompileGeometryPipelineVertex(
  sm50_shader_t pVertexShader, sm50_shader_t pGeometryShader,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pVertexShaderArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
) {
  using namespace llvm;
  using namespace mullion;

  if (ppError) {
    *ppError = nullptr;
  }
  auto errorObj = new SM50ErrorInternal();
  llvm::raw_svector_ostream errorOut(errorObj->buf);
  if (ppBitcode == nullptr) {
    errorOut << "ppBitcode can not be null\0";
    *ppError = (sm50_error_t)errorObj;
    return 1;
  }

  // pArgs is ignored for now
  LLVMContext context;

  context.setOpaquePointers(false); // I suspect Metal uses LLVM 14...

  // the vertex shader's own operations; without a geometry shader the pipeline only streams output
  auto &shader_info = ((mullion::dxbc::SM50ShaderInternal *)pVertexShader)->shader_info;

  auto pModule = std::make_unique<Module>("shader.air", context);
  initializeModule(*pModule);

  if (auto err = mullion::dxbc::convert_dxbc_vertex_for_geometry_shader(
        (dxbc::SM50ShaderInternal *)pVertexShader, FunctionName,
        (dxbc::SM50ShaderInternal *)pGeometryShader, context, *pModule,
        pVertexShaderArgs
      )) {
    errorOut << llvm::toString(std::move(err));
    *ppError = (sm50_error_t)errorObj;
    return 1;
  }

  if (shader_info.use_msad)
    linkMSAD(*pModule);
  if (shader_info.use_samplepos)
    linkSamplePos(*pModule);

  if (!runOptimizationPasses(*pModule, errorOut)) {
    *ppError = (sm50_error_t)errorObj;
    return 1;
  }

  // Serialize AIR
  auto compiled = new SM50CompiledBitcodeInternal();

  raw_svector_ostream OS(compiled->vec);

  metallib::MetallibWriter writer;

  writer.Write(*pModule, OS);

  pModule.reset();

  *ppBitcode = (sm50_bitcode_t)compiled;
  return 0;
}

AIRCONV_API int SM50CompileGeometryPipelineGeometry(
  sm50_shader_t pVertexShader, sm50_shader_t pGeometryShader,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pGeometryShaderArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
) {
  using namespace llvm;
  using namespace mullion;

  if (ppError) {
    *ppError = nullptr;
  }
  auto errorObj = new SM50ErrorInternal();
  llvm::raw_svector_ostream errorOut(errorObj->buf);
  if (ppBitcode == nullptr) {
    errorOut << "ppBitcode can not be null\0";
    *ppError = (sm50_error_t)errorObj;
    return 1;
  }

  // pArgs is ignored for now
  LLVMContext context;

  context.setOpaquePointers(false); // I suspect Metal uses LLVM 14...

  auto &shader_info = ((mullion::dxbc::SM50ShaderInternal *)(pGeometryShader ? pGeometryShader : pVertexShader))->shader_info;
  SM50_SHADER_PSO_GEOMETRY_SHADER_DATA *pso_data = nullptr;
  bool pass_through =
      mullion::dxbc::args_get_data<SM50_SHADER_PSO_GEOMETRY_SHADER, SM50_SHADER_PSO_GEOMETRY_SHADER_DATA>(pGeometryShaderArgs, &pso_data) &&
      pso_data->pass_through;

  auto pModule = std::make_unique<Module>("shader.air", context);
  initializeModule(*pModule);

  if (auto err = pGeometryShader ? mullion::dxbc::convert_dxbc_geometry_shader(
                                       (dxbc::SM50ShaderInternal *)pGeometryShader, FunctionName,
                                       (dxbc::SM50ShaderInternal *)pVertexShader, context, *pModule, pGeometryShaderArgs
                                   )
                 : pass_through ? mullion::dxbc::convert_pass_through_geometry_shader(
                                      (dxbc::SM50ShaderInternal *)pVertexShader, FunctionName, context, *pModule,
                                      pGeometryShaderArgs
                                  )
                                : mullion::dxbc::convert_null_geometry_shader(FunctionName, context, *pModule)) {
    errorOut << llvm::toString(std::move(err));
    *ppError = (sm50_error_t)errorObj;
    return 1;
  }

  if (shader_info.use_msad)
    linkMSAD(*pModule);
  if (shader_info.use_samplepos)
    linkSamplePos(*pModule);
  
  if (!runOptimizationPasses(*pModule, errorOut)) {
    *ppError = (sm50_error_t)errorObj;
    return 1;
  }

  // Serialize AIR
  auto compiled = new SM50CompiledBitcodeInternal();

  raw_svector_ostream OS(compiled->vec);

  metallib::MetallibWriter writer;

  writer.Write(*pModule, OS);

  pModule.reset();

  *ppBitcode = (sm50_bitcode_t)compiled;
  return 0;
}

AIRCONV_API void SM50GetCompiledBitcode(
  sm50_bitcode_t pBitcode, SM50_COMPILED_BITCODE *pData
) {
  auto pBitcodeInternal = (SM50CompiledBitcodeInternal *)pBitcode;
  pData->Data = pBitcodeInternal->vec.data();
  pData->Size = pBitcodeInternal->vec.size();
}

AIRCONV_API void SM50DestroyBitcode(sm50_bitcode_t pBitcode) {
  auto pBitcodeInternal = (SM50CompiledBitcodeInternal *)pBitcode;
  delete pBitcodeInternal;
}

AIRCONV_API size_t SM50GetErrorMessage(sm50_error_t pError, char *pBuffer, size_t BufferSize) {
  auto pInternal = (SM50ErrorInternal *)pError;
  auto str_len = std::min(pInternal->buf.size(), BufferSize - 1);
  memcpy(pBuffer, pInternal->buf.data(), str_len);
  pBuffer[str_len] = '\0';
  return str_len;
}

AIRCONV_API void SM50FreeError(sm50_error_t pError) {
  if (pError == nullptr)
    return;
  auto pInternal = (SM50ErrorInternal *)pError;
  delete pInternal;
}
