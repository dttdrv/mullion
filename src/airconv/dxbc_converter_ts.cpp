#include "air_signature.hpp"
#include "airconv_error.hpp"
#include "dxbc_converter.hpp"
#include "nt/air_builder.hpp"
#include "nt/dxbc_converter_base.hpp"
#include "tessellation_limits.hpp"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/AtomicOrdering.h"

namespace dxmt::dxbc {

// air_tessellation.metal's tess_workload
struct TessMeshWorkload {
  struct {
    uint32_t factor, kept;
    uint8_t first;
    int8_t segments, owned;
    uint8_t kind;
  } edge[4];
  int32_t patch_index;
};

TessellatorPartitioning
get_partitioning(SM50ShaderInternal *pHullStage) {
  TessellatorPartitioning partitioning;
  switch (pHullStage->tessellation_partition) {
  case microsoft::D3D11_SB_TESSELLATOR_PARTITIONING_POW2:
    partitioning = TessellatorPartitioning::pow2;
    break;
  case microsoft::D3D11_SB_TESSELLATOR_PARTITIONING_FRACTIONAL_ODD:
    partitioning = TessellatorPartitioning::fractional_odd;
    break;
  case microsoft::D3D11_SB_TESSELLATOR_PARTITIONING_FRACTIONAL_EVEN:
    partitioning = TessellatorPartitioning::fractional_even;
    break;
  default:
    partitioning = TessellatorPartitioning::integer;
    break;
  }
  return partitioning;
}

TessellatorOutputPrimitive
get_output_primitive(SM50ShaderInternal *pHullStage) {
  TessellatorOutputPrimitive primitive;
  switch (pHullStage->tessellator_output_primitive) {
  case microsoft::D3D11_SB_TESSELLATOR_OUTPUT_LINE:
    primitive = TessellatorOutputPrimitive::line;
    break;
  case microsoft::D3D11_SB_TESSELLATOR_OUTPUT_TRIANGLE_CW:
    primitive = TessellatorOutputPrimitive::triangle;
    break;
  case microsoft::D3D11_SB_TESSELLATOR_OUTPUT_TRIANGLE_CCW:
    primitive = TessellatorOutputPrimitive::triangle_ccw;
    break;
  default:
    primitive = TessellatorOutputPrimitive::point;
    break;
  }
  return primitive;
}

size_t
estimate_payload_size(SM50ShaderInternal *pHullStage, float factor, uint32_t patch_per_group) {
  auto max_hs_output_register = pHullStage->max_output_register;
  auto cp_size_per_point = sizeof(uint32_t) * 4 * max_hs_output_register;
  auto cp_size_per_patch = cp_size_per_point * pHullStage->output_control_point_count;
  auto cp_size_per_group = cp_size_per_patch * patch_per_group;
  auto pc_size_per_patch = sizeof(uint32_t) * pHullStage->patch_constant_scalars.size();
  auto pc_size_per_group = pc_size_per_patch * patch_per_group;

  auto factor_int = get_integer_factor(factor, pHullStage->tessellation_partition);
  uint32_t max_workload_count = get_max_potential_workload_count(factor_int, pHullStage->tessellation_domain, pHullStage->tessellation_partition);
  constexpr uint32_t size_workload_info = sizeof(TessMeshWorkload);

  // the patches' control points and constants, the first patch's number, the workloads, and the grid's first
  // workload and instance (tessellation_payload_type)
  return cp_size_per_group + pc_size_per_group + sizeof(uint32_t) +
         max_workload_count * patch_per_group * size_workload_info + 2 * sizeof(uint32_t);
};

std::pair<uint32_t, uint32_t>
mesh_threadgroups_per_workload(
    SM50ShaderInternal *pHullStage, SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs, uint32_t factor_int
) {
  SM50_SHADER_PSO_TESSELLATOR_DATA *pso_tess = nullptr;
  // without a geometry shader, as many as share the workload's primitives
  if (!args_get_data<SM50_SHADER_PSO_TESSELLATOR, SM50_SHADER_PSO_TESSELLATOR_DATA>(pArgs, &pso_tess) ||
      !pso_tess->geometry)
    return {tessellation_pieces(
                factor_int, pHullStage->tessellation_domain, pHullStage->tessellator_output_primitive,
                pso_tess ? pso_tess->mesh_vertex_size : 0
            )
                .count,
            1};
  // a workload's primitives do not outnumber its vertices
  return {get_max_workload_vertices(factor_int, pHullStage->tessellation_domain),
          ((SM50ShaderInternal *)pso_tess->geometry)->gs_instance_count};
}

std::pair<float, uint32_t>
get_final_maxtessfactor(SM50ShaderInternal *pHullStage, SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs) {
  SM50_SHADER_PSO_TESSELLATOR_DATA *pso_tess = nullptr;
  args_get_data<SM50_SHADER_PSO_TESSELLATOR, SM50_SHADER_PSO_TESSELLATOR_DATA>(pArgs, &pso_tess);
  return get_final_factor(
      pHullStage->max_tesselation_factor, pHullStage->tessellation_partition,
      pso_tess ? pso_tess->max_potential_tess_factor : UINT32_MAX
  );
}

// the payload the hull stage leaves the mesh stage: the control points and patch constants of an object threadgroup's
// patches, its first patch's number, and its workloads
llvm::StructType *
tessellation_payload_type(
    SM50ShaderInternal *pHullStage, uint32_t factor_int, air::AirType &types, llvm::LLVMContext &context
) {
  uint32_t patch_per_group = next_pow2(32 / next_pow2(pHullStage->hull_maximum_threads_per_patch));
  auto per_patch = llvm::ArrayType::get(
      llvm::ArrayType::get(types._int4, pHullStage->max_output_register), pHullStage->output_control_point_count
  );
  return llvm::StructType::create(
      context,
      {llvm::ArrayType::get(per_patch, patch_per_group),
       llvm::ArrayType::get(llvm::ArrayType::get(types._int, pHullStage->patch_constant_scalars.size()), patch_per_group),
       types._int,
       llvm::ArrayType::get(
           types._int,
           get_max_potential_workload_count(factor_int, pHullStage->tessellation_domain, pHullStage->tessellation_partition) * patch_per_group * sizeof(TessMeshWorkload) / 4
       ),
       // the grid's first workload and first geometry instance
       types._int, types._int},
      "payload"
  );
}

// what the domain shader takes of a workload's patch from the payload (its control points, constants and number),
// and registers for what it outputs. returns the payload's workloads
llvm::Value *
setup_domain_stage(
    SM50ShaderInternal *pDomainStage, SM50ShaderInternal *pHullStage, llvm::StructType *payload_type,
    llvm::Value *payload_argument, llvm::Value *workload_index, struct context &ctx
) {
  auto &builder = ctx.builder;
  auto &types = ctx.types;
  auto &resource_map = ctx.resource;
  uint32_t max_input_register = pDomainStage->max_input_register;
  uint32_t max_output_register = pDomainStage->max_output_register;

  auto payload = builder.CreateBitCast(payload_argument, payload_type->getPointerTo(6));
  auto data = builder.CreateGEP(payload_type, payload, {builder.getInt32(0), builder.getInt32(3), builder.getInt32(0)});
  auto batched_patch_start =
      builder.CreateLoad(types._int, builder.CreateGEP(payload_type, payload, {builder.getInt32(0), builder.getInt32(2)}));
  auto patch_index = dxbc::Converter(ctx.air, ctx, resource_map).DomainGetPatchIndex(workload_index, data);

  resource_map.patch_id = builder.CreateAdd(batched_patch_start, patch_index);

  resource_map.input.ptr_int4 = builder.CreateGEP(
      payload_type, payload,
      {builder.getInt32(0), // struct
       builder.getInt32(0), // controlpoint
       patch_index}
  );
  resource_map.input.ptr_float4 = builder.CreateBitCast(
      resource_map.input.ptr_int4,
      llvm::ArrayType::get(
          llvm::ArrayType::get(types._float4, pHullStage->max_output_register), pHullStage->output_control_point_count
      )
          ->getPointerTo(6)
  );
  resource_map.input_element_count = pHullStage->max_output_register;

  resource_map.patch_constant_output.ptr_int4 =
      builder.CreateAlloca(llvm::ArrayType::get(types._int4, max_input_register));
  resource_map.patch_constant_output.ptr_float4 = builder.CreateBitCast(
      resource_map.patch_constant_output.ptr_int4,
      llvm::ArrayType::get(types._float4, max_input_register)->getPointerTo()
  );

  resource_map.output.ptr_int4 = builder.CreateAlloca(llvm::ArrayType::get(types._int4, max_output_register));
  resource_map.output.ptr_float4 = builder.CreateBitCast(
      resource_map.output.ptr_int4,
      llvm::ArrayType::get(types._float4, max_output_register)->getPointerTo(
          cast<llvm::PointerType>(resource_map.output.ptr_int4->getType())->getPointerAddressSpace()
      )
  );
  resource_map.output_element_count = max_output_register;

  /* setup patch constant register */
  for (auto x : llvm::enumerate(pHullStage->patch_constant_scalars)) {
    if (x.value().reg >= max_input_register)
      continue;
    auto src_ptr = builder.CreateGEP(
        payload_type, payload,
        {builder.getInt32(0), // struct
         builder.getInt32(1), // pcout
         patch_index, builder.getInt32(x.index())}
    );
    auto dst_ptr = builder.CreateGEP(
        llvm::ArrayType::get(types._int4, max_input_register), resource_map.patch_constant_output.ptr_int4,
        {builder.getInt32(0), builder.getInt32(x.value().reg), builder.getInt32(x.value().component)}
    );
    builder.CreateStore(builder.CreateLoad(types._int, src_ptr), dst_ptr);
  }
  return data;
}

// the domain location a domain shader reads: a triangle's third coordinate is what the other two leave of 1, which
// a quad's and a line's shaders do not read
llvm::Value *
domain_location(llvm::Value *location, llvm::IRBuilder<> &builder, llvm::air::AIRBuilder &air) {
  llvm::Value *u = builder.CreateExtractElement(location, 0ull), *v = builder.CreateExtractElement(location, 1ull);
  llvm::Value *domain = llvm::UndefValue::get(air.getFloatTy(3));
  domain = builder.CreateInsertElement(domain, u, 0ull);
  domain = builder.CreateInsertElement(domain, v, 1ull);
  return builder.CreateInsertElement(domain, builder.CreateFSub(air.getFloat(1.0f), builder.CreateFAdd(u, v)), 2ull);
}

llvm::Error
convert_dxbc_vertex_hull_shader(
    SM50ShaderInternal *pVertexStage, SM50ShaderInternal *pHullStage, const char *name, llvm::LLVMContext &context,
    llvm::Module &module, SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs
) {
  using namespace microsoft;

  // NOTE: func_signature of vertex stage only defines some outputs
  // that we don't need them in vertex-hull object shader.
  // And func_signature of hull stage simply has nothing defined yet
  // TODO: should get rid of func_signature in SM50ShaderInternal and
  // always create a new fresh instance + use signature handlers
  auto func_signature = pHullStage->func_signature;
  auto &vertex_shader_info = pVertexStage->shader_info;
  auto &hull_shader_info = pHullStage->shader_info;

  uint32_t max_vs_input_register = pVertexStage->max_input_register;
  uint32_t max_vs_output_register = 0;
  for (auto &out : pVertexStage->output_signature) {
    max_vs_output_register = std::max(max_vs_output_register, out.reg() + 1);
  }
  uint32_t max_hs_output_register = pHullStage->max_output_register;
  uint32_t max_patch_constant_output_register = pHullStage->max_patch_constant_output_register;

  auto [final_maxtessfactor, factor_int] = get_final_maxtessfactor(pHullStage, pArgs);

  SM50_SHADER_METAL_VERSION metal_version = SM50_SHADER_METAL_310;
  SM50_SHADER_FLAG shader_flags = {};
  SM50_SHADER_COMMON_DATA *sm50_common = nullptr;
  if (args_get_data<SM50_SHADER_COMMON, SM50_SHADER_COMMON_DATA>(pArgs, &sm50_common)) {
    metal_version = sm50_common->metal_version;
    shader_flags = sm50_common->flags;
  }
  SM50_SHADER_IA_INPUT_LAYOUT_DATA *ia_layout = nullptr;
  args_get_data<SM50_SHADER_IA_INPUT_LAYOUT, SM50_SHADER_IA_INPUT_LAYOUT_DATA>(pArgs, &ia_layout);
  SM50_SHADER_ROOT_SIGNATURE_DATA *rootsig = nullptr;
  args_get_data<SM50_SHADER_ROOT_SIGNATURE, SM50_SHADER_ROOT_SIGNATURE_DATA>(pArgs, &rootsig);
  SM50_SHADER_ROOT_SIGNATURE_DATA *rootsig_vs = nullptr;
  args_get_data<SM50_SHADER_ROOT_SIGNATURE2, SM50_SHADER_ROOT_SIGNATURE_DATA>(pArgs, &rootsig_vs);

  bool is_indexed_draw = ia_layout && ia_layout->index_buffer_format > 0;
  bool index_view = ia_layout && ia_layout->index_buffer_format == SM50_INDEX_BUFFER_FORMAT_VIEW;
  // a hull shader may make its control points from nothing, and declare none: the draw then names its patch size
  if (!pHullStage->input_control_point_count && !index_view)
    return llvm::make_error<UnsupportedFeature>("a hull shader without input control points needs its draw's patch size");

  IREffect prologue_vs([](auto) { return std::monostate(); });
  IREffect prologue_hs([](auto) { return std::monostate(); });
  IRValue epilogue_vs([](struct context ctx) -> pvalue {
    // just return null since the function returns void
    // and we handle output registers directly
    return nullptr;
  });
  IRValue epilogue_hs([](struct context ctx) -> pvalue {
    // just return null since the function returns void
    // and we handle output registers directly
    return nullptr;
  });

  io_binding_map resource_map_vs;
  io_binding_map resource_map_hs;
  air::AirType types(context);

  {
    SignatureContext sig_ctx_vs(prologue_vs, epilogue_vs, func_signature, resource_map_vs);
    sig_ctx_vs.ia_layout = ia_layout;
    sig_ctx_vs.skip_vertex_output = true;
    for (auto &p : pVertexStage->signature_handlers) {
      p(sig_ctx_vs);
    }
    SignatureContext sig_ctx_hs(prologue_hs, epilogue_hs, func_signature, resource_map_hs);
    for (auto &p : pHullStage->signature_handlers) {
      p(sig_ctx_hs);
    }
  }

  auto binding_map = rootsig_vs ? setup_binding_rootsig(
                                   &vertex_shader_info, func_signature, module, D3D10_SB_VERTEX_SHADER,
                                   rootsig_vs->bytecode, rootsig_vs->bytecode_length
                               )
                             : setup_binding_table2(
                                   &vertex_shader_info, func_signature, module, SM50_BINDING_INDEX_CONSTANT_BUFFER2,
                                   SM50_BINDING_INDEX_ARGUMENT_TABLE2
                               );
  if (!binding_map)
    return llvm::make_error<UnsupportedFeature>("invalid root signature or missing resource binding");
  auto binding_map_hs = rootsig ? setup_binding_rootsig(
                                      &hull_shader_info, func_signature, module, D3D11_SB_HULL_SHADER,
                                      rootsig->bytecode, rootsig->bytecode_length
                                  )
                                : setup_binding_table2(&hull_shader_info, func_signature, module);
  if (!binding_map_hs)
    return llvm::make_error<UnsupportedFeature>("invalid root signature or missing resource binding");

  uint32_t threads_per_patch = next_pow2(pHullStage->hull_maximum_threads_per_patch);
  uint32_t patch_per_group = next_pow2(32 / threads_per_patch);

  auto hs_output_per_point_type = llvm::ArrayType::get(types._int4, max_hs_output_register);
  auto hs_output_per_patch_type =
      llvm::ArrayType::get(hs_output_per_point_type, pHullStage->output_control_point_count);
  auto hs_output_per_group_type = llvm::ArrayType::get(hs_output_per_patch_type, patch_per_group);
  auto hs_output_per_point_type_float = llvm::ArrayType::get(types._float4, max_hs_output_register);
  auto hs_output_per_patch_type_float =
      llvm::ArrayType::get(hs_output_per_point_type_float, pHullStage->output_control_point_count);

  auto hs_pcout_per_patch_type = llvm::ArrayType::get(types._int4, max_patch_constant_output_register);
  auto hs_pcout_per_group_type = llvm::ArrayType::get(hs_pcout_per_patch_type, patch_per_group);
  auto hs_pcout_per_patch_type_float = llvm::ArrayType::get(types._float4, max_patch_constant_output_register);

  uint32_t max_workload_count = get_max_potential_workload_count(factor_int, pHullStage->tessellation_domain, pHullStage->tessellation_partition);
  auto [workload_primitives, geometry_instances] = mesh_threadgroups_per_workload(pHullStage, pArgs, factor_int);
  SM50_SHADER_PSO_TESSELLATOR_DATA *pso_tess = nullptr;
  args_get_data<SM50_SHADER_PSO_TESSELLATOR, SM50_SHADER_PSO_TESSELLATOR_DATA>(pArgs, &pso_tess);
  // the most a grid has (tessellation_grids). left out while the pipeline is to tell what the device lets one have
  uint32_t grid_limit = pso_tess ? pso_tess->max_mesh_threadgroups : 0;
  if (!pso_tess || grid_limit) {
    auto grids = tessellation_grids(max_workload_count * patch_per_group, workload_primitives, geometry_instances, grid_limit);
    func_signature.UseMaxMeshWorkgroupSize(grids.workloads * workload_primitives * grids.instances);
  }

  auto payload_struct_type = tessellation_payload_type(pHullStage, factor_int, types, context);
  uint32_t payload_struct_size = module.getDataLayout().getTypeAllocSize(payload_struct_type);

  uint32_t payload_idx = func_signature.DefineInput(air::InputPayload{.size = payload_struct_size});
  uint32_t thread_id_idx = func_signature.DefineInput(air::InputThreadPositionInThreadgroup{});
  // (batched_patch_id_start, instance_id, 0)
  uint32_t tg_id_idx = func_signature.DefineInput(air::InputThreadgroupPositionInGrid{});
  func_signature.DefineInput(air::InputMeshGridProperties{});
  uint32_t draw_argument_idx = func_signature.DefineInput(air::ArgumentBindingBuffer{
      .buffer_size = {},
      .location_index = SM50_BINDING_INDEX_DRAW_ARGUMENTS,
      .array_size = 0,
      .memory_access = air::MemoryAccess::read,
      .address_space = air::AddressSpace::constant,
      .type = is_indexed_draw //
                  ? air::MSLWhateverStruct{"draw_indexed_arguments", types._dxmt_draw_indexed_arguments}
                  : air::MSLWhateverStruct{"draw_arguments", types._dxmt_draw_arguments},
      .arg_name = "draw_arguments",
      .raster_order_group = {}
  });

  uint32_t index_buffer_idx = ~0u;
  if (is_indexed_draw) {
    index_buffer_idx = func_signature.DefineInput(air::ArgumentBindingBuffer{
        .buffer_size = {},
        .location_index = SM50_BINDING_INDEX_INDEX_BUFFER,
        .array_size = 0,
        .memory_access = air::MemoryAccess::read,
        .address_space = air::AddressSpace::device,
        .type = index_view ? air::MSLRepresentableType(air::MSLWhateverStruct{
                                 "index_buffer_view", llvm::StructType::get(context, {types._long, types._int, types._int})
                             })
                : ia_layout->index_buffer_format == 1 ? air::MSLRepresentableType(air::MSLUshort{})
                                                      : air::MSLRepresentableType(air::MSLUint{}),
        .raster_order_group = {}
    });
  }

  uint32_t patch_size_idx = ~0u;
  if (!pHullStage->input_control_point_count)
    patch_size_idx = func_signature.DefineInput(air::ArgumentBindingBuffer{
        .buffer_size = {},
        .location_index = SM50_BINDING_INDEX_PATCH_SIZE,
        .array_size = 0,
        .memory_access = air::MemoryAccess::read,
        .address_space = air::AddressSpace::constant,
        .type = air::MSLRepresentableType(air::MSLUint{}),
        .arg_name = "patch_size",
        .raster_order_group = {}
    });

  auto [function, function_metadata] = func_signature.CreateFunction(name, context, module, 0, true);

  auto entry_bb_global = llvm::BasicBlock::Create(context, "entry", function);
  auto vertex_stage_end = llvm::BasicBlock::Create(context, "vertex_end", function);
  llvm::IRBuilder<> builder(entry_bb_global);
  llvm::raw_null_ostream nulldbg{};
  llvm::air::AIRBuilder air(
      {
          .sampleNaNToZero = bool(shader_flags & SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO),
          .defuseFma = bool(shader_flags & SM50_SHADER_FLAG_DEFUSE_FMA),
      },
      builder, nulldbg
  );

  // these values are initialized by vertex stage and accessed by hull stage
  llvm::Value *instance_id = nullptr;
  llvm::Value *batched_patch_start = nullptr;
  llvm::Value *patch_id = nullptr;
  llvm::Value *patch_count = nullptr;

  setup_temp_register(&vertex_shader_info, resource_map_vs, types, module, builder);
  setup_temp_register(&hull_shader_info, resource_map_hs, types, module, builder);
  setup_immediate_constant_buffer(&vertex_shader_info, resource_map_vs, types, module, builder);
  setup_immediate_constant_buffer(&hull_shader_info, resource_map_hs, types, module, builder);

  auto output_reg_per_point_type = llvm::ArrayType::get(types._int4, max_vs_output_register);
  auto output_reg_per_patch_type =
      llvm::ArrayType::get(output_reg_per_point_type, pHullStage->input_control_point_count);
  auto output_reg_per_group_type = llvm::ArrayType::get(output_reg_per_patch_type, patch_per_group);
  auto output_reg_per_point_type_float = llvm::ArrayType::get(types._float4, max_vs_output_register);
  auto output_reg_per_patch_type_float =
      llvm::ArrayType::get(output_reg_per_point_type_float, pHullStage->input_control_point_count);

  llvm::GlobalVariable *vertex_out = new llvm::GlobalVariable(
      module, output_reg_per_group_type, false, llvm::GlobalValue::InternalLinkage,
      llvm::UndefValue::get(output_reg_per_group_type), "vertex_out_hull_in", nullptr,
      llvm::GlobalValue::NotThreadLocal, (uint32_t)air::AddressSpace::threadgroup
  );
  vertex_out->setAlignment(llvm::Align(4));

  auto thread_position_in_group = function->getArg(thread_id_idx);
  auto control_point_id_in_patch = builder.CreateExtractElement(thread_position_in_group, (uint32_t)0);
  auto patch_offset_in_group = builder.CreateExtractElement(thread_position_in_group, 1);

  // how many workloads each patch of the threadgroup has: none until its patch constant function has counted them,
  // so none for a patch past the draw's last. and the patch's factors, kept from counting to writing
  auto workload_counts_type = llvm::ArrayType::get(types._int, patch_per_group);
  llvm::GlobalVariable *workload_counts = new llvm::GlobalVariable(
      module, workload_counts_type, false, llvm::GlobalValue::InternalLinkage,
      llvm::UndefValue::get(workload_counts_type), "workload_counts", nullptr, llvm::GlobalValue::NotThreadLocal,
      (uint32_t)air::AddressSpace::threadgroup
  );
  workload_counts->setAlignment(llvm::Align(4));
  auto workload_count = [&](llvm::Value *patch) {
    return builder.CreateGEP(workload_counts_type, workload_counts, {builder.getInt32(0), patch});
  };
  builder.CreateStore(builder.getInt32(0), workload_count(patch_offset_in_group));
  auto next_workload = builder.CreateAlloca(types._int);
  auto factors_type = llvm::ArrayType::get(types._float, 6);
  auto kept_factors = builder.CreateAlloca(factors_type);

  {
    /* Vertex Shader Zone */
    auto shader_info = vertex_shader_info;
    auto &resource_map = resource_map_vs;
    auto &prologue = prologue_vs;
    auto &epilogue = epilogue_vs;
    auto max_input_register = max_vs_input_register;
    auto max_output_register = max_vs_output_register;

    auto epilogue_bb = llvm::BasicBlock::Create(context, "epilogue_vertex", function);
    setup_metal_version(module, metal_version);

    auto active = llvm::BasicBlock::Create(context, "active_vertex", function);

    auto threadgroup_position_in_grid = function->getArg(tg_id_idx);
    batched_patch_start = builder.CreateMul(
        builder.CreateExtractElement(threadgroup_position_in_grid, (uint32_t)0),
        builder.getInt32(32 / threads_per_patch)
    );
    patch_id = builder.CreateAdd(batched_patch_start, patch_offset_in_group);
    instance_id = builder.CreateExtractElement(threadgroup_position_in_grid, (uint32_t)1);
    auto control_point_index = builder.CreateAdd(
        builder.CreateMul(patch_id, builder.getInt32(pHullStage->input_control_point_count)), control_point_id_in_patch
    );

    // the arguments are read field by field: a draw without indices has one field less, which is never read
    auto draw_arguments = function->getArg(draw_argument_idx);
    auto argument = [&](unsigned field) {
      return builder.CreateStructGEP(
          is_indexed_draw ? types._dxmt_draw_indexed_arguments : types._dxmt_draw_arguments, draw_arguments, field
      );
    };
    // with an index buffer view, whether the draw has indices is known only at run time
    llvm::Value *indexed = builder.getInt1(is_indexed_draw && !index_view), *view = nullptr, *format = nullptr;
    if (index_view) {
      view = function->getArg(index_buffer_idx);
      format = builder.CreateLoad(types._int, builder.CreateStructGEP(view->getType()->getNonOpaquePointerElementType(), view, 2));
      indexed = builder.CreateICmpNE(format, builder.getInt32(0));
    }
    // a field of indexed arguments, which sits one field later than in arguments without indices
    auto shifted = [&](unsigned field) {
      return is_indexed_draw ? builder.CreateSelect(indexed, argument(field), argument(field - 1)) : argument(field - 1);
    };

    llvm::Value *patch_size = patch_size_idx == ~0u ? (llvm::Value *)builder.getInt32(pHullStage->input_control_point_count)
                                            : builder.CreateLoad(types._int, function->getArg(patch_size_idx));
    patch_count = builder.CreateUDiv(builder.CreateLoad(types._int, argument(0)), patch_size);

    resource_map.input.ptr_int4 = builder.CreateAlloca(llvm::ArrayType::get(types._int4, max_input_register));
    resource_map.input.ptr_float4 = builder.CreateBitCast(
        resource_map.input.ptr_int4, llvm::ArrayType::get(types._float4, max_input_register)->getPointerTo()
    );
    resource_map.input_element_count = max_input_register;

    // of type "output_reg_per_point_type"
    resource_map.output.ptr_int4 = builder.CreateGEP(
        output_reg_per_group_type, vertex_out, {builder.getInt32(0), patch_offset_in_group, control_point_id_in_patch}
    );
    resource_map.output.ptr_float4 = builder.CreateBitCast(
        resource_map.output.ptr_int4, output_reg_per_point_type_float->getPointerTo(vertex_out->getAddressSpace())
    );

    resource_map.output_element_count = max_output_register;

    struct context ctx {
        .builder = builder,
        .air = air,
        .binding = *binding_map,
        .llvm = context,
        .module = module,
        .function = function,
        .resource = resource_map,
        .types = types,
        .pso_sample_mask = 0xffffffff,
        .shader_type = pVertexStage->shader_type,
        .metal_version = metal_version,
        .simd_width = sm50_common ? sm50_common->simd_width : 0,
    };

    builder.CreateCondBr(
        builder.CreateLogicalAnd(
            builder.CreateICmp(
                llvm::CmpInst::ICMP_ULT, control_point_id_in_patch,
                builder.getInt32(pHullStage->input_control_point_count)
            ),
            builder.CreateICmp(llvm::CmpInst::ICMP_ULT, patch_id, patch_count)
        ),
        active, vertex_stage_end
    );
    builder.SetInsertPoint(active);

    if (index_view) {
      auto index = index_from_view(
          builder, types, view, format, builder.CreateAdd(builder.CreateLoad(types._int, argument(2)), control_point_index)
      );
      resource_map.vertex_id = builder.CreateSelect(indexed, index, control_point_index);
    } else if (index_buffer_idx != ~0u) {
      auto start_index = builder.CreateLoad(types._int, argument(2));
      auto index_buffer = function->getArg(index_buffer_idx);
      auto index_buffer_element_type = index_buffer->getType()->getNonOpaquePointerElementType();
      auto vertex_id = builder.CreateLoad(
          index_buffer_element_type,
          builder.CreateGEP(
              index_buffer_element_type, index_buffer, {builder.CreateAdd(start_index, control_point_index)}
          )
      );
      resource_map.vertex_id = builder.CreateZExt(vertex_id, types._int);
    } else {
      resource_map.vertex_id = control_point_index;
    }

    resource_map.base_vertex_id = builder.CreateLoad(types._int, shifted(3));
    resource_map.instance_id = instance_id;
    resource_map.vertex_id_with_base = builder.CreateAdd(resource_map.vertex_id, resource_map.base_vertex_id);
    resource_map.base_instance_id = builder.CreateLoad(types._int, shifted(4));
    resource_map.instance_id_with_base = builder.CreateAdd(resource_map.instance_id, resource_map.base_instance_id);

    if (auto err = prologue.build(ctx).takeError()) {
      return err;
    }
    auto real_entry = convert_basicblocks(pVertexStage->entry(), ctx, epilogue_bb);
    if (auto err = real_entry.takeError()) {
      return err;
    }
    builder.CreateBr(real_entry.get());

    builder.SetInsertPoint(epilogue_bb);
    auto epilogue_result = epilogue.build(ctx);
    if (auto err = epilogue_result.takeError()) {
      return err;
    }

    builder.CreateBr(vertex_stage_end);
    builder.SetInsertPoint(vertex_stage_end);

    air.CreateBarrier(llvm::air::MemFlags::Threadgroup);
  }

  {

    /* Hull Shader Zone */
    auto shader_info = hull_shader_info;
    auto &resource_map = resource_map_hs;
    auto &prologue = prologue_hs;
    auto &epilogue = epilogue_hs;

    auto epilogue_bb = llvm::BasicBlock::Create(context, "epilogue_hull", function);

    uint32_t vertex_max_output_register = max_vs_output_register;

    auto payload = builder.CreateBitCast(function->getArg(payload_idx), payload_struct_type->getPointerTo(6));

    assert(pHullStage->input_control_point_count != ~0u);

    resource_map.input.ptr_int4 =
        builder.CreateGEP(output_reg_per_group_type, vertex_out, {builder.getInt32(0), patch_offset_in_group});
    resource_map.input.ptr_float4 = builder.CreateBitCast(
        resource_map.input.ptr_int4, output_reg_per_patch_type_float->getPointerTo(vertex_out->getAddressSpace())
    );
    resource_map.input_element_count = vertex_max_output_register;

    resource_map.instance_id = instance_id;

    resource_map.patch_id = patch_id;

    resource_map.thread_id_in_patch = builder.CreateSelect(
        builder.CreateICmp(llvm::CmpInst::ICMP_ULT, resource_map.patch_id, patch_count), control_point_id_in_patch,
        builder.getInt32(32) // so all phases are effectively skipped
    );

    if (shader_info.no_control_point_phase_passthrough) {
      // TODO: check this out carefully......
      assert(pHullStage->output_control_point_count != ~0u);

      if (shader_info.output_control_point_read) {
        llvm::GlobalVariable *control_point_phase_out = new llvm::GlobalVariable(
            module, hs_output_per_group_type, false, llvm::GlobalValue::InternalLinkage,
            llvm::UndefValue::get(hs_output_per_group_type), "hull_control_point_phase_out", nullptr,
            llvm::GlobalValue::NotThreadLocal, (uint32_t)air::AddressSpace::threadgroup
        );
        control_point_phase_out->setAlignment(llvm::Align(4));
        resource_map.output.ptr_int4 = builder.CreateGEP(
            hs_output_per_group_type, control_point_phase_out, {builder.getInt32(0), patch_offset_in_group}
        );

        resource_map.hull_cp_passthrough_dst = builder.CreateGEP(
            payload_struct_type, payload,
            {builder.getInt32(0), builder.getInt32(0), patch_offset_in_group, resource_map.thread_id_in_patch}
        );
        resource_map.hull_cp_passthrough_src = builder.CreateGEP(
            hs_output_per_group_type, control_point_phase_out,
            {builder.getInt32(0), patch_offset_in_group, resource_map.thread_id_in_patch}
        );
        resource_map.hull_cp_passthrough_type = hs_output_per_point_type;
      } else {
        resource_map.output.ptr_int4 = builder.CreateGEP(
            payload_struct_type, payload,
            {builder.getInt32(0), builder.getInt32(0) /* field: control points*/, patch_offset_in_group}
        );
      }
      resource_map.output.ptr_float4 = builder.CreateBitCast(
          resource_map.output.ptr_int4,
          hs_output_per_patch_type_float->getPointerTo(
              cast<llvm::PointerType>(resource_map.output.ptr_int4->getType())->getPointerAddressSpace()
          )
      );
      resource_map.output_element_count = max_hs_output_register;
    } else {
      /* since no control point phase */
      if (vertex_max_output_register < max_hs_output_register) {
        return llvm::make_error<UnsupportedFeature>(
            "Hull shader has control point phase pass-through, but vertex shader "
            "output signature doesn't match hull shader input signature."
        );
      }
      resource_map.output.ptr_float4 = resource_map.input.ptr_float4;
      resource_map.output.ptr_int4 = resource_map.input.ptr_int4;
      resource_map.output_element_count = resource_map.input_element_count;

      resource_map.hull_cp_passthrough_dst = builder.CreateGEP(
          payload_struct_type, payload,
          {builder.getInt32(0), builder.getInt32(0), patch_offset_in_group, resource_map.thread_id_in_patch}
      );
      resource_map.hull_cp_passthrough_src = builder.CreateGEP(
          output_reg_per_group_type, vertex_out,
          {builder.getInt32(0), patch_offset_in_group, resource_map.thread_id_in_patch}
      );
      resource_map.hull_cp_passthrough_type = hs_output_per_point_type;
    }

    if (max_patch_constant_output_register) {
      /* all instances write to the same output (threadgroup memory) */
      llvm::GlobalVariable *patch_constant_out = new llvm::GlobalVariable(
          module, hs_pcout_per_group_type, false, llvm::GlobalValue::InternalLinkage,
          llvm::UndefValue::get(hs_pcout_per_group_type), "hull_patch_constant_out", nullptr,
          llvm::GlobalValue::NotThreadLocal, (uint32_t)air::AddressSpace::threadgroup
      );
      patch_constant_out->setAlignment(llvm::Align(4));
      resource_map.patch_constant_output.ptr_int4 =
          builder.CreateGEP(hs_pcout_per_group_type, patch_constant_out, {builder.getInt32(0), patch_offset_in_group});
      resource_map.patch_constant_output.ptr_float4 = builder.CreateBitCast(
          resource_map.patch_constant_output.ptr_int4,
          hs_pcout_per_patch_type_float->getPointerTo((uint32_t)air::AddressSpace::threadgroup)
      );
    }

    struct context ctx {
        .builder = builder,
        .air = air,
        .binding = *binding_map_hs,
        .llvm = context,
        .module = module,
        .function = function,
        .resource = resource_map,
        .types = types,
        .pso_sample_mask = 0xffffffff,
        .shader_type = pHullStage->shader_type,
        .metal_version = metal_version,
    };
    dxbc::Converter dxbc(ctx.air, ctx, ctx.resource);

    if (auto err = prologue.build(ctx).takeError()) {
      return err;
    }
    auto real_entry = convert_basicblocks(pHullStage->entry(), ctx, epilogue_bb);
    if (auto err = real_entry.takeError()) {
      return err;
    }
    builder.CreateBr(real_entry.get());

    builder.SetInsertPoint(epilogue_bb);

    /* populate patch constant output */

    auto write_patch_constant = llvm::BasicBlock::Create(context, "write_patch_constant", function);
    auto counted = llvm::BasicBlock::Create(context, "counted", function);
    auto write_workloads = llvm::BasicBlock::Create(context, "write_workloads", function);
    auto dispatch_mesh = llvm::BasicBlock::Create(context, "dispatch_mesh", function);
    auto real_return = llvm::BasicBlock::Create(context, "real_return", function);

    builder.CreateCondBr(
        builder.CreateICmp(llvm::CmpInst::ICMP_EQ, resource_map.thread_id_in_patch, builder.getInt32(0)),
        write_patch_constant, counted
    );

    builder.SetInsertPoint(write_patch_constant);
    if (auto err = epilogue.build(ctx).takeError()) {
      return err;
    }

    std::array<llvm::Value *, 6> tess_factors;
    tess_factors.fill(air.getFloat(0));

    auto max_tess_factor_value = air.getFloat(final_maxtessfactor);

    for (unsigned i = 0; i < pHullStage->patch_constant_scalars.size(); i++) {
      auto pc_scalar = pHullStage->patch_constant_scalars[i];
      auto dst_ptr = builder.CreateGEP(
          payload_struct_type, payload,
          {builder.getInt32(0),   // struct
           builder.getInt32(1),   // pcout
           patch_offset_in_group, // patch_id,
           builder.getInt32(i)}
      );
      auto src_ptr = builder.CreateGEP(
          hs_pcout_per_patch_type, resource_map.patch_constant_output.ptr_int4,
          {builder.getInt32(0), builder.getInt32(pc_scalar.reg), builder.getInt32(pc_scalar.component)}
      );
      if (pc_scalar.tess_factor_index >= 0 && pc_scalar.tess_factor_index < 6) {
        auto value = builder.CreateBitCast(builder.CreateLoad(types._int, src_ptr), types._float);
        // a NaN factor culls the patch, so the clamp keeps it
        auto value_clamp =
            builder.CreateSelect(builder.CreateFCmpOGT(value, max_tess_factor_value), max_tess_factor_value, value);
        tess_factors[pc_scalar.tess_factor_index] = value_clamp;
        builder.CreateStore(builder.CreateBitCast(value_clamp, types._int), dst_ptr);
      } else {
        builder.CreateStore(builder.CreateLoad(types._int, src_ptr), dst_ptr);
      }
    };

    // the factors are in the order of their system values: a quad's and a triangle's edges, then their inside; a
    // line's detail, then the lines' density
    std::pair<const char *, std::vector<unsigned>> domain;
    switch (pHullStage->tessellation_domain) {
    case microsoft::D3D11_SB_TESSELLATOR_DOMAIN_ISOLINE:
      domain = {"isoline", {1, 0}};
      break;
    case microsoft::D3D11_SB_TESSELLATOR_DOMAIN_QUAD:
      domain = {"quad", {4, 5, 0, 1, 2, 3}};
      break;
    default:
      domain = {"triangle", {3, 0, 1, 2}};
    }
    for (size_t i = 0; i < domain.second.size(); i++)
      builder.CreateStore(
          tess_factors[domain.second[i]],
          builder.CreateGEP(factors_type, kept_factors, {builder.getInt32(0), builder.getInt32(i)})
      );
    // the patch's workloads from `first` on, of which those before `capacity` are written; returns where they end
    auto generate = [&](llvm::Value *first, uint32_t capacity) {
      std::vector<llvm::Value *> factors;
      for (size_t i = 0; i < domain.second.size(); i++)
        factors.push_back(builder.CreateLoad(
            types._float, builder.CreateGEP(factors_type, kept_factors, {builder.getInt32(0), builder.getInt32(i)})
        ));
      builder.CreateStore(first, next_workload);
      dxbc.HullGenerateWorkload(
          domain.first, patch_offset_in_group, next_workload,
          builder.CreateGEP(payload_struct_type, payload, {builder.getInt32(0), builder.getInt32(3), builder.getInt32(0)}),
          capacity, get_partitioning(pHullStage), factors
      );
      return builder.CreateLoad(types._int, next_workload);
    };
    builder.CreateStore(generate(builder.getInt32(0), 0), workload_count(patch_offset_in_group));
    builder.CreateBr(counted);

    // every patch of the threadgroup is counted: a patch's workloads come after those of the patches before it, in
    // the patches' order, whichever thread is first
    builder.SetInsertPoint(counted);
    air.CreateBarrier(llvm::air::MemFlags::Threadgroup);
    builder.CreateCondBr(
        builder.CreateICmp(llvm::CmpInst::ICMP_EQ, resource_map.thread_id_in_patch, builder.getInt32(0)),
        write_workloads, real_return
    );

    builder.SetInsertPoint(write_workloads);
    llvm::Value *before = builder.getInt32(0), *all = builder.getInt32(0);
    for (uint32_t i = 0; i < patch_per_group; i++) {
      auto count = builder.CreateLoad(types._int, workload_count(builder.getInt32(i)));
      all = builder.CreateAdd(all, count);
      before = builder.CreateAdd(
          before, builder.CreateSelect(builder.CreateICmpULT(builder.getInt32(i), patch_offset_in_group), count, builder.getInt32(0))
      );
    }
    generate(before, max_workload_count * patch_per_group);

    builder.CreateCondBr(
        builder.CreateICmp(llvm::CmpInst::ICMP_EQ, patch_offset_in_group, builder.getInt32(0)), dispatch_mesh,
        real_return
    );

    builder.SetInsertPoint(dispatch_mesh);

    // the mesh threadgroups of the workloads that were written: one for each, or with a geometry shader one for
    // each primitive and instance. a grid has no more than the device lets it, so the object threadgroups along the
    // grid's third axis share them (tessellation_grids): each takes the workloads and instances after those of the
    // ones before it
    auto grids = tessellation_grids(max_workload_count * patch_per_group, workload_primitives, geometry_instances, grid_limit);
    auto part = builder.CreateExtractElement(function->getArg(tg_id_idx), 2);
    auto first_workload = builder.CreateMul(
        builder.CreateUDiv(part, builder.getInt32(grids.instance_parts)), builder.getInt32(grids.workloads)
    );
    auto first_instance = builder.CreateMul(
        builder.CreateURem(part, builder.getInt32(grids.instance_parts)), builder.getInt32(grids.instances)
    );
    auto written = air.CreateIntBinOp(llvm::air::AIRBuilder::min, all, builder.getInt32(max_workload_count * patch_per_group));
    llvm::Value *meshgroup_to_dispatch = air.getInt3(1, workload_primitives, 1);
    meshgroup_to_dispatch = builder.CreateInsertElement(
        meshgroup_to_dispatch,
        air.CreateIntBinOp(
            llvm::air::AIRBuilder::min,
            builder.CreateSub(air.CreateIntBinOp(llvm::air::AIRBuilder::max, written, first_workload), first_workload),
            builder.getInt32(grids.workloads)
        ),
        (uint64_t)0
    );
    meshgroup_to_dispatch = builder.CreateInsertElement(
        meshgroup_to_dispatch,
        air.CreateIntBinOp(
            llvm::air::AIRBuilder::min, builder.CreateSub(builder.getInt32(geometry_instances), first_instance),
            builder.getInt32(grids.instances)
        ),
        (uint64_t)2
    );
    air.CreateSetMeshProperties(meshgroup_to_dispatch);

    builder.CreateStore(
        batched_patch_start,
        builder.CreateGEP(payload_struct_type, payload, {builder.getInt32(0), builder.getInt32(2)})
    );
    builder.CreateStore(first_workload, builder.CreateGEP(payload_struct_type, payload, {builder.getInt32(0), builder.getInt32(4)}));
    builder.CreateStore(first_instance, builder.CreateGEP(payload_struct_type, payload, {builder.getInt32(0), builder.getInt32(5)}));

    builder.CreateBr(real_return);
    builder.SetInsertPoint(real_return);
  }

  air.CreateBarrier(llvm::air::MemFlags::Threadgroup);
  builder.CreateRetVoid();

  module.getOrInsertNamedMetadata("air.object")->addOperand(function_metadata);
  return llvm::Error::success();
}

llvm::Error
convert_dxbc_tesselator_domain_shader(
    SM50ShaderInternal *pShaderInternal, const char *name, SM50ShaderInternal *pHullStage, llvm::LLVMContext &context,
    llvm::Module &module, SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs
) {
  using namespace microsoft;

  auto func_signature = pShaderInternal->func_signature; // copy
  auto shader_info = &(pShaderInternal->shader_info);

  uint32_t max_output_register = pShaderInternal->max_output_register;
  SM50_SHADER_GS_PASS_THROUGH_DATA *gs_passthrough = nullptr;
  bool rasterization_disabled =
      (args_get_data<SM50_SHADER_GS_PASS_THROUGH, SM50_SHADER_GS_PASS_THROUGH_DATA>(pArgs, &gs_passthrough) &&
       gs_passthrough->RasterizationDisabled);
  SM50_SHADER_METAL_VERSION metal_version = SM50_SHADER_METAL_310;
  SM50_SHADER_FLAG shader_flags = {};
  SM50_SHADER_COMMON_DATA *sm50_common = nullptr;
  if (args_get_data<SM50_SHADER_COMMON, SM50_SHADER_COMMON_DATA>(pArgs, &sm50_common)) {
    metal_version = sm50_common->metal_version;
    shader_flags = sm50_common->flags;
  }
  SM50_SHADER_ROOT_SIGNATURE_DATA *rootsig = nullptr;
  args_get_data<SM50_SHADER_ROOT_SIGNATURE, SM50_SHADER_ROOT_SIGNATURE_DATA>(pArgs, &rootsig);

  auto [final_maxtessfactor, factor_int] = get_final_maxtessfactor(pHullStage, pArgs);

  IREffect prologue([](auto) { return std::monostate(); });
  IRValue epilogue([](struct context ctx) -> pvalue {
    return nullptr; // a mesh shader...
  });

  io_binding_map resource_map;
  air::AirType types(context);

  {
    SignatureContext sig_ctx(prologue, epilogue, func_signature, resource_map);
    for (auto &p : pShaderInternal->signature_handlers) {
      p(sig_ctx);
    }
  }

  // TESS TODO: 17e30e0855effcaa4da9a05c018a3ba7189d7653 changes

  auto &ds_output_handlers = pShaderInternal->mesh_output_handlers;

  auto binding_map = rootsig ? setup_binding_rootsig(
                                   shader_info, func_signature, module, D3D11_SB_DOMAIN_SHADER, rootsig->bytecode,
                                   rootsig->bytecode_length
                               )
                             : setup_binding_table2(shader_info, func_signature, module);
  if (!binding_map)
    return llvm::make_error<UnsupportedFeature>("invalid root signature or missing resource binding");

  // every primitive a patch generates carries the patch's SV_PrimitiveID to the pixel shader
  func_signature.DefineMeshPrimitiveOutput(air::OutputPrimitiveID{});

  uint32_t thread_id_idx = func_signature.DefineInput(air::InputThreadPositionInThreadgroup{});
  uint32_t tg_id_idx = func_signature.DefineInput(air::InputThreadgroupPositionInGrid{});

  auto payload_struct_type = tessellation_payload_type(pHullStage, factor_int, types, context);
  uint32_t payload_struct_size = module.getDataLayout().getTypeAllocSize(payload_struct_type);

  uint32_t payload_idx = func_signature.DefineInput(air::InputPayload{.size = payload_struct_size});

  bool point_rasterization = false;

  // the most vertices a mesh threadgroup makes, which its primitives do not outnumber: its workload's, or those of
  // the primitives it makes of a workload that several share (tessellation_pieces)
  SM50_SHADER_PSO_TESSELLATOR_DATA *pso_tess = nullptr;
  args_get_data<SM50_SHADER_PSO_TESSELLATOR, SM50_SHADER_PSO_TESSELLATOR_DATA>(pArgs, &pso_tess);
  auto pieces = tessellation_pieces(
      factor_int, pHullStage->tessellation_domain, pHullStage->tessellator_output_primitive,
      pso_tess ? pso_tess->mesh_vertex_size : 0
  );
  uint32_t corners = get_primitive_vertices(pHullStage->tessellator_output_primitive);
  uint32_t mesh_vertices = pieces.primitives ? pieces.primitives * corners
                                             : get_max_workload_vertices(factor_int, pHullStage->tessellation_domain);
  uint32_t mesh_primitives = pieces.primitives ? pieces.primitives : mesh_vertices;
  switch (pHullStage->tessellator_output_primitive) {
  case microsoft::D3D11_SB_TESSELLATOR_OUTPUT_POINT: {
    func_signature.DefineInput(air::InputMesh{mesh_vertices, mesh_primitives, air::MeshOutputTopology::Point});
    point_rasterization = true; // TESS TODO: not necessary... if combined with GS/SO
    break;
  }
  case microsoft::D3D11_SB_TESSELLATOR_OUTPUT_LINE: {
    func_signature.DefineInput(air::InputMesh{mesh_vertices, mesh_primitives, air::MeshOutputTopology::Line});
    break;
  }
  default: {
    func_signature.DefineInput(air::InputMesh{mesh_vertices, mesh_primitives, air::MeshOutputTopology::Triangle});
    break;
  }
  }
  // Metal culls by primitive, where Direct3D discards one whose vertices are all behind a cull distance (D3D11.3
  // 15.4.2): the group's threads say what each vertex is behind, a bit for each distance, and the thread that makes
  // the primitives culls them
  llvm::GlobalVariable *behind = nullptr;
  if (pShaderInternal->cull_distance_scalars.size()) {
    func_signature.DefineMeshPrimitiveOutput(air::OutputPrimitiveCulled{});
    auto type = llvm::ArrayType::get(types._int, mesh_vertices);
    behind = new llvm::GlobalVariable(
        module, type, false, llvm::GlobalValue::InternalLinkage, llvm::UndefValue::get(type), "", nullptr,
        llvm::GlobalValue::NotThreadLocal, (uint32_t)air::AddressSpace::threadgroup
    );
  }

  if (point_rasterization) {
    func_signature.DefineMeshVertexOutput(air::OutputPointSize {});
  }

  if (pShaderInternal->clip_distance_scalars.size() > 0) {
    func_signature.DefineMeshVertexOutput(
        air::OutputClipDistance{.count = pShaderInternal->clip_distance_scalars.size()}
    );
  }

  auto [function, function_metadata] = func_signature.CreateFunction(name, context, module, 0, rasterization_disabled);

  auto entry_bb = llvm::BasicBlock::Create(context, "entry", function);
  auto vertex_start = llvm::BasicBlock::Create(context, "vertex_start", function);
  auto vertex_emit = llvm::BasicBlock::Create(context, "vertex_emit", function);
  auto vertex_end = llvm::BasicBlock::Create(context, "vertex_end", function);
  auto generate_primitive_pre = llvm::BasicBlock::Create(context, "generate_primitive_pre", function);
  auto generate_primitive = llvm::BasicBlock::Create(context, "generate_primitive", function);
  auto real_return = llvm::BasicBlock::Create(context, "real_return", function);
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
  setup_temp_register(shader_info, resource_map, types, module, builder);
  setup_immediate_constant_buffer(shader_info, resource_map, types, module, builder);

  // the threadgroup's workload: its place in the grid, counted on from the grid's first (the hull stage shares the
  // patches' workloads between the grids of several object threadgroups)
  auto workload_index = builder.CreateAdd(
      builder.CreateExtractElement(function->getArg(tg_id_idx), 0ull),
      builder.CreateLoad(
          types._int,
          builder.CreateGEP(
              payload_struct_type,
              builder.CreateBitCast(function->getArg(payload_idx), payload_struct_type->getPointerTo(6)),
              {builder.getInt32(0), builder.getInt32(4)}
          )
      )
  );
  auto thread_index = builder.CreateExtractElement(function->getArg(thread_id_idx), 0ull);
  // the first of the workload's primitives that are the threadgroup's, when several share them
  auto first_primitive = builder.CreateMul(
      builder.CreateExtractElement(function->getArg(tg_id_idx), 1), builder.getInt32(pieces.primitives)
  );

  struct context ctx {
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
  };
  dxbc::Converter dxbc(ctx.air, ctx, ctx.resource);

  auto data = setup_domain_stage(
      pShaderInternal, pHullStage, payload_struct_type, function->getArg(payload_idx), workload_index, ctx
  );

  builder.CreateBr(vertex_start);

  builder.SetInsertPoint(vertex_start);

  auto phi_thread_index_base = builder.CreatePHI(types._int, 2);
  phi_thread_index_base->addIncoming(air.getInt(0), entry_bb);

  auto actual_thread_index = builder.CreateAdd(phi_thread_index_base, thread_index);

  auto [location, active, iterate] =
      pieces.primitives ? dxbc.DomainGetPieceLocation(
                              workload_index, first_primitive, actual_thread_index, pieces.primitives,
                              get_output_primitive(pHullStage), data
                          )
                        : dxbc.DomainGetLocation(workload_index, actual_thread_index, data);

  resource_map.domain = domain_location(location, builder, air);

  auto real_entry = convert_basicblocks(pShaderInternal->entry(), ctx, vertex_emit);
  if (auto err = real_entry.takeError()) {
    return err;
  }
  builder.CreateCondBr(active, real_entry.get(), vertex_end);

  builder.SetInsertPoint(vertex_emit);

  auto vertex_id = actual_thread_index;
  // a primitive's vertices are `corners` threads' when it has its own
  auto primitive_id =
      pieces.primitives ? builder.CreateUDiv(actual_thread_index, builder.getInt32(corners)) : actual_thread_index;
  MeshOutputContext gs_out_ctx{vertex_id, primitive_id};
  for (auto &h : ds_output_handlers) {
    if (auto err = h(gs_out_ctx).build(ctx).takeError()) {
      return err;
    }
  }

  for (auto x : llvm::enumerate(pShaderInternal->clip_distance_scalars)) {
    if (x.value().reg >= max_output_register)
      continue;
    auto src_ptr = builder.CreateGEP(
        llvm::ArrayType::get(types._float4, max_output_register), resource_map.output.ptr_float4,
        {builder.getInt32(0), builder.getInt32(x.value().reg), builder.getInt32(x.value().component)}
    );
    air.CreateSetMeshClipDistance(
        vertex_id, builder.getInt32(x.index()), builder.CreateLoad(types._float, src_ptr)
    );
  }

  if (behind) {
    llvm::Value *mine = builder.getInt32(0);
    for (auto distance : llvm::enumerate(pShaderInternal->cull_distance_scalars)) {
      auto value = builder.CreateLoad(
          types._float, builder.CreateGEP(
                            llvm::ArrayType::get(types._float4, max_output_register), resource_map.output.ptr_float4,
                            {builder.getInt32(0), builder.getInt32(distance.value().reg),
                             builder.getInt32(distance.value().component)}
                        )
      );
      // negative, or NaN
      mine = builder.CreateOr(
          mine, builder.CreateShl(
                    builder.CreateZExt(builder.CreateFCmpULT(value, llvm::ConstantFP::get(types._float, 0)), types._int),
                    distance.index()
                )
      );
    }
    builder.CreateStore(mine, builder.CreateInBoundsGEP(behind->getValueType(), behind, {builder.getInt32(0), vertex_id}));
  }

  air.CreateSetMeshPrimitiveID(primitive_id, resource_map.patch_id);
  if (point_rasterization) {
    air.CreateSetMeshPointSize(vertex_id, air.getFloat(1.0));
  }

  builder.CreateBr(vertex_end);

  builder.SetInsertPoint(vertex_end);
  phi_thread_index_base->addIncoming(builder.CreateAdd(phi_thread_index_base, air.getInt(32)), vertex_end);
  builder.CreateCondBr(iterate, vertex_start, generate_primitive_pre);

  builder.SetInsertPoint(generate_primitive_pre);
  if (behind)
    air.CreateBarrier(llvm::air::MemFlags::Threadgroup);
  builder.CreateCondBr(
      builder.CreateICmp(llvm::CmpInst::ICMP_EQ, thread_index, builder.getInt32(0)), generate_primitive, real_return
  );
  builder.SetInsertPoint(generate_primitive);

  auto behind_ptr =
      behind ? builder.CreateInBoundsGEP(behind->getValueType(), behind, {builder.getInt32(0), builder.getInt32(0)}) : nullptr;
  if (pieces.primitives)
    dxbc.DomainGeneratePiece(
        workload_index, first_primitive, pieces.primitives, data, get_output_primitive(pHullStage), behind_ptr
    );
  else
    dxbc.DomainGeneratePrimitives(workload_index, data, get_output_primitive(pHullStage), behind_ptr);

  builder.CreateBr(real_return);
  builder.SetInsertPoint(real_return);
  builder.CreateRetVoid();

  module.getOrInsertNamedMetadata("air.mesh")->addOperand(function_metadata);
  return llvm::Error::success();
}

} // namespace dxmt::dxbc