#include "air_operations.hpp"
#include "air_signature.hpp"
#include "airconv_error.hpp"
#include "dxbc_converter.hpp"
#include "nt/air_builder.hpp"
#include "nt/dxbc_converter_base.hpp"
#include "llvm/IR/Constants.h"
#include "llvm/Support/Alignment.h"
#include "llvm/Support/AtomicOrdering.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include <cstdint>
#include <format>
#include <utility>

namespace dxmt::dxbc {

using namespace microsoft;

// the vertices of an input primitive
static uint32_t
vertices_per_primitive(D3D10_SB_PRIMITIVE primitive) {
  switch (primitive) {
  case D3D10_SB_PRIMITIVE_POINT:
    return 1;
  case D3D10_SB_PRIMITIVE_LINE:
    return 2;
  case D3D10_SB_PRIMITIVE_TRIANGLE:
    return 3;
  case D3D10_SB_PRIMITIVE_LINE_ADJ:
    return 4;
  case D3D10_SB_PRIMITIVE_TRIANGLE_ADJ:
    return 6;
  default:
    return primitive >= D3D11_SB_PRIMITIVE_1_CONTROL_POINT_PATCH && primitive <= D3D11_SB_PRIMITIVE_32_CONTROL_POINT_PATCH
               ? uint32_t(primitive) - uint32_t(D3D11_SB_PRIMITIVE_1_CONTROL_POINT_PATCH) + 1
               : 0;
  }
}

// an object threadgroup's warp (SM50GeometryWarp): how far on the next starts, its whole primitives, and the vertices
// of one
std::tuple<uint32_t, uint32_t, uint32_t>
get_vertex_primitive_count_in_warp(D3D10_SB_PRIMITIVE primitive, bool strip, uint32_t registers) {
  auto per_primitive = vertices_per_primitive(primitive);
  auto warp = SM50GeometryWarp(per_primitive ? per_primitive : 1, strip, registers);
  return {warp.vertices, warp.primitives, per_primitive};
}

// SM50_STREAM_OUTPUT_TARGETS, and where a draw's scratch area keeps what (airconv_public.h)
static llvm::StructType *
stream_output_targets_type(llvm::LLVMContext &context, air::AirType &types) {
  auto qwords = llvm::ArrayType::get(types._long, 4);
  return llvm::StructType::get(context, {qwords, qwords, qwords, types._long, types._int, types._int, qwords});
}

// a stream's statistics query, if one is active (`address`): the primitives written and those that would have been
static void
add_stream_output_statistics(llvm::IRBuilder<> &builder, air::AirType &types, pvalue address, pvalue written, pvalue needed) {
  auto &context = builder.getContext();
  auto function = builder.GetInsertBlock()->getParent();
  auto yes = llvm::BasicBlock::Create(context, "so_statistics", function), no = llvm::BasicBlock::Create(context, "so_counted", function);
  builder.CreateCondBr(builder.CreateICmpNE(address, builder.getInt64(0)), yes, no);
  builder.SetInsertPoint(yes);
  auto data = builder.CreateIntToPtr(address, types._long->getPointerTo((uint32_t)air::AddressSpace::device));
  pvalue values[2] = {written, needed};
  for (uint32_t i = 0; i < 2; i++) {
    auto ptr = builder.CreateConstInBoundsGEP1_32(types._long, data, i);
    builder.CreateStore(builder.CreateAdd(builder.CreateLoad(types._long, ptr), values[i]), ptr);
  }
  builder.CreateBr(no);
  builder.SetInsertPoint(no);
}
constexpr uint32_t kStreamOutputTotals = 8; // after the 4 filled sizes, in 32-bit words
// the last words of the payload: the primitives each stream wrote before the object threadgroup's, 64 bits each as
// the counters they end in are (D3D11.3 14.5)
constexpr uint32_t kPayloadStreamOutputBase = SM50_GEOMETRY_PAYLOAD_SIZE / 4 - 8;
// what a strip's primitives need from before their object threadgroup: the whole primitives there, and how far into
// its strip the group starts
constexpr uint32_t kPayloadPrimitivesBefore = 3;
constexpr uint32_t kPayloadStripOffset = kPayloadStreamOutputBase - 1;

// a strip primitive's parity counts from its strip's start: the last invalid primitive below it in the group (a cut),
// else the group's offset in its strip
static pvalue
strip_parity(llvm::IRBuilder<> &builder, llvm::air::AIRBuilder &air, pvalue valid, pvalue primitive, pvalue carry) {
  auto one = builder.getInt32(1);
  auto invalid_below = builder.CreateAnd(builder.CreateNot(valid), builder.CreateSub(builder.CreateShl(one, primitive), one));
  auto start = builder.CreateSub(builder.getInt32(32), air.CreateCountZero(invalid_below, false));
  return builder.CreateAnd(
      builder.CreateSelect(
          builder.CreateICmpEQ(invalid_below, builder.getInt32(0)), builder.CreateAdd(primitive, carry),
          builder.CreateSub(primitive, start)
      ),
      one
  );
}

static uint32_t
stream_output_input(air::FunctionSignatureBuilder &signature, llvm::LLVMContext &context, air::AirType &types) {
  return signature.DefineInput(air::ArgumentBindingBuffer{
      .buffer_size = {},
      .location_index = SM50_BINDING_INDEX_STREAM_OUTPUT0,
      .array_size = 0,
      .memory_access = air::MemoryAccess::read,
      .address_space = air::AddressSpace::constant,
      .type = air::MSLWhateverStruct{"stream_output_targets", stream_output_targets_type(context, types)},
      .raster_order_group = {}
  });
}

llvm::Error
convert_dxbc_geometry_shader(
    SM50ShaderInternal *pShaderInternal, const char *name, SM50ShaderInternal *pVertexStage, llvm::LLVMContext &context,
    llvm::Module &module, SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs, SM50ShaderInternal *pHullStage
) {
  auto func_signature = pShaderInternal->func_signature; // copy
  auto shader_info = &(pShaderInternal->shader_info);

  bool is_strip = false;

  uint32_t max_output_register = pShaderInternal->max_output_register;
  SM50_SHADER_PSO_GEOMETRY_SHADER_DATA *pso_data = nullptr;
  if (args_get_data<SM50_SHADER_PSO_GEOMETRY_SHADER, SM50_SHADER_PSO_GEOMETRY_SHADER_DATA>(pArgs, &pso_data)) {
    is_strip = pso_data->strip_topology;
  }
  SM50_SHADER_STREAM_OUTPUT_DATA *so = nullptr;
  args_get_data<SM50_SHADER_STREAM_OUTPUT, SM50_SHADER_STREAM_OUTPUT_DATA>(pArgs, &so);
  uint32_t rasterized_stream = so ? so->rasterized_stream : 0;
  SM50_SHADER_METAL_VERSION metal_version = SM50_SHADER_METAL_310;
  SM50_SHADER_FLAG shader_flags = {};
  SM50_SHADER_COMMON_DATA *sm50_common = nullptr;
  if (args_get_data<SM50_SHADER_COMMON, SM50_SHADER_COMMON_DATA>(pArgs, &sm50_common)) {
    metal_version = sm50_common->metal_version;
    shader_flags = sm50_common->flags;
  }
  SM50_SHADER_ROOT_SIGNATURE_DATA *rootsig = nullptr;
  args_get_data<SM50_SHADER_ROOT_SIGNATURE, SM50_SHADER_ROOT_SIGNATURE_DATA>(pArgs, &rootsig);
  // after a tessellator (`pHullStage`) the stage before is the domain shader, which this function runs as well, on
  // the mesh stage's usual bindings: the geometry shader then takes the stage's second ones
  auto rootsig_ds = rootsig;
  if (pHullStage)
    args_get_data<SM50_SHADER_ROOT_SIGNATURE2, SM50_SHADER_ROOT_SIGNATURE_DATA>(pArgs, &rootsig);
  if (pHullStage && so)
    return llvm::make_error<UnsupportedFeature>("stream output of a tessellated draw");

  IREffect prologue([](auto) { return std::monostate(); });
  IRValue epilogue([](struct context ctx) -> pvalue { return nullptr; });

  io_binding_map resource_map, domain_map;
  air::AirType types(context);

  {
    SignatureContext sig_ctx(prologue, epilogue, func_signature, resource_map);
    for (auto &p : pShaderInternal->signature_handlers) {
      p(sig_ctx);
    }
    // the domain shader's outputs go to the geometry shader, not the mesh
    IREffect none([](auto) { return std::monostate(); });
    IRValue nothing([](struct context ctx) -> pvalue { return nullptr; });
    SignatureContext domain_ctx(none, nothing, func_signature, domain_map);
    if (pHullStage)
      for (auto &p : pVertexStage->signature_handlers)
        p(domain_ctx);
  }
  auto& gs_output_handlers = pShaderInternal->mesh_output_handlers;

  auto binding_map = rootsig ? setup_binding_rootsig(
                                   shader_info, func_signature, module, D3D10_SB_GEOMETRY_SHADER, rootsig->bytecode,
                                   rootsig->bytecode_length
                               )
                     : pHullStage
                         ? setup_binding_table2(
                               shader_info, func_signature, module, SM50_BINDING_INDEX_CONSTANT_BUFFER2,
                               SM50_BINDING_INDEX_ARGUMENT_TABLE2
                           )
                         : setup_binding_table2(shader_info, func_signature, module);
  auto domain_binding = !pHullStage ? nullptr
                        : rootsig_ds
                            ? setup_binding_rootsig(
                                  &pVertexStage->shader_info, func_signature, module, D3D11_SB_DOMAIN_SHADER,
                                  rootsig_ds->bytecode, rootsig_ds->bytecode_length
                              )
                            : setup_binding_table2(&pVertexStage->shader_info, func_signature, module);

  auto gs_output_topology = pShaderInternal->gs_output_topology;
  int32_t max_vertex_out = pShaderInternal->gs_max_vertex_output;
  air::MeshOutputTopology topology = air::MeshOutputTopology::Point;
  switch (gs_output_topology) {
  case microsoft::D3D10_SB_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
    topology = air::MeshOutputTopology::Triangle;
    break;
  case microsoft::D3D10_SB_PRIMITIVE_TOPOLOGY_LINESTRIP:
    topology = air::MeshOutputTopology::Line;
    break;
  // a shader that emits nothing declares no topology
  case microsoft::D3D10_SB_PRIMITIVE_TOPOLOGY_UNDEFINED:
  case microsoft::D3D10_SB_PRIMITIVE_TOPOLOGY_POINTLIST:
    break;
  default:
    return llvm::make_error<UnsupportedFeature>("unsupported geometry shader output topology");
  }

  // the hull stage's payload has the workloads whose primitives the mesh threadgroups take
  auto tessellation_payload =
      pHullStage ? tessellation_payload_type(pHullStage, get_final_maxtessfactor(pHullStage, pArgs).second, types, context)
                 : nullptr;
  uint32_t payload_idx = func_signature.DefineInput(air::InputPayload{
      .size = pHullStage ? (uint32_t)module.getDataLayout().getTypeAllocSize(tessellation_payload)
                         : SM50_GEOMETRY_PAYLOAD_SIZE
  });
  // it's intended to declare a bit more max primitive count
  auto mesh_idx = func_signature.DefineInput(air::InputMesh{(uint32_t)max_vertex_out, (uint32_t)max_vertex_out, topology});
  uint32_t tg_in_grid_idx = func_signature.DefineInput(air::InputThreadgroupPositionInGrid{});
  // a tessellated draw's mesh threadgroups have the domain stage's threads, of which the first does the work
  uint32_t thread_idx = pHullStage ? func_signature.DefineInput(air::InputThreadPositionInThreadgroup{}) : ~0u;
  uint32_t so_idx = so ? stream_output_input(func_signature, context, types) : ~0u;

  if (pShaderInternal->clip_distance_scalars.size() > 0) {
    func_signature.DefineMeshVertexOutput(
        air::OutputClipDistance{.count = pShaderInternal->clip_distance_scalars.size()}
    );
  }
  // Metal's points have a size to be given, Direct3D's are a pixel wide (D3D11.3 3.4.6)
  if (topology == air::MeshOutputTopology::Point)
    func_signature.DefineMeshVertexOutput(air::OutputPointSize{});
  // Metal culls by primitive, where Direct3D discards one whose vertices are all behind a cull distance (15.4.2)
  bool culls = pShaderInternal->cull_distance_scalars.size();
  if (culls)
    func_signature.DefineMeshPrimitiveOutput(air::OutputPrimitiveCulled{});

  auto [function, function_metadata] = func_signature.CreateFunction(name, context, module, 0, false);

  auto entry_bb = llvm::BasicBlock::Create(context, "entry", function);
  auto active_ = llvm::BasicBlock::Create(context, "active", function);
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

  auto [warp_vertex_count, warp_primitive_count, vertex_per_primitive] =
      get_vertex_primitive_count_in_warp(pShaderInternal->gs_input_primitive, is_strip, pVertexStage->max_output_register);
  if (pHullStage && vertex_per_primitive != (pHullStage->tessellator_output_primitive == D3D11_SB_TESSELLATOR_OUTPUT_POINT ? 1u
                                              : pHullStage->tessellator_output_primitive == D3D11_SB_TESSELLATOR_OUTPUT_LINE ? 2u
                                                                                                                              : 3u))
    return llvm::make_error<UnsupportedFeature>("the geometry shader does not take what the tessellator outputs");
  // the grid has an object threadgroup's primitives, and the shader's instances of each. after a tessellator it has
  // workloads, each one's primitives and their instances, counted on from the grid's first workload and instance
  // (the hull stage shares them between the grids of several object threadgroups)
  auto tg_position_in_grid = function->getArg(tg_in_grid_idx);
  auto primitive_id_in_warp = builder.CreateExtractElement(tg_position_in_grid, uint64_t(!!pHullStage));
  resource_map.gs_instance_id = builder.CreateExtractElement(tg_position_in_grid, uint64_t(1 + !!pHullStage));
  llvm::Value *workload = nullptr;
  if (pHullStage) {
    auto first = [&](uint32_t field) {
      return builder.CreateLoad(
          types._int, builder.CreateGEP(
                          tessellation_payload,
                          builder.CreateBitCast(function->getArg(payload_idx), tessellation_payload->getPointerTo(6)),
                          {builder.getInt32(0), builder.getInt32(field)}
                      )
      );
    };
    workload = builder.CreateAdd(builder.CreateExtractElement(tg_position_in_grid, uint64_t(0)), first(4));
    resource_map.gs_instance_id = builder.CreateAdd(resource_map.gs_instance_id, first(5));
  }

  auto const zero_const = builder.getInt32(0);
  auto const one_const = builder.getInt32(1);
  auto const two_const = builder.getInt32(2);
  auto next_write_vertex = builder.CreateAlloca(types._int, nullptr, "next_write_vertex");
  builder.CreateStore(zero_const, next_write_vertex);
  auto vertex_offset = builder.CreateAlloca(types._int, nullptr, "vertex_offset");
  builder.CreateStore(zero_const, vertex_offset);
  auto primitive_count = builder.CreateAlloca(types._int, nullptr, "primitive_count");
  builder.CreateStore(zero_const, primitive_count);

  auto mesh_ptr = function->getArg(mesh_idx);

  resource_map.mesh = mesh_ptr;

  auto emit_clip_distances = [&](llvm::Value *vertex) {
    for (auto x : llvm::enumerate(pShaderInternal->clip_distance_scalars)) {
      if (x.value().reg >= max_output_register)
        continue;
      auto src_ptr = builder.CreateGEP(
          llvm::ArrayType::get(types._float4, max_output_register), resource_map.output.ptr_float4,
          {builder.getInt32(0), builder.getInt32(x.value().reg), builder.getInt32(x.value().component)}
      );
      air.CreateSetMeshClipDistance(vertex, builder.getInt32(x.index()), builder.CreateLoad(types._float, src_ptr));
    }
  };

  // the cull distances the last vertices of the strip are behind, a bit for each: negative, or NaN
  llvm::Value *behind[2] = {};
  for (auto &before : behind)
    if (culls)
      builder.CreateStore(zero_const, before = builder.CreateAlloca(types._int));
  // the vertex being emitted completes the primitive `vertices` - 1 back in its strip, once the strip has that many:
  // it is culled when one distance has all its vertices behind
  auto cull_primitive = [&](llvm::Value *written, llvm::Value *first_primitive, uint32_t vertices) {
    if (!culls)
      return;
    llvm::Value *mine = zero_const;
    for (auto distance : llvm::enumerate(pShaderInternal->cull_distance_scalars)) {
      auto value = builder.CreateLoad(
          types._float, builder.CreateGEP(
                            llvm::ArrayType::get(types._float4, max_output_register), resource_map.output.ptr_float4,
                            {zero_const, builder.getInt32(distance.value().reg), builder.getInt32(distance.value().component)}
                        )
      );
      mine = builder.CreateOr(
          mine, builder.CreateShl(
                    builder.CreateZExt(builder.CreateFCmpULT(value, llvm::ConstantFP::get(types._float, 0)), types._int),
                    distance.index()
                )
      );
    }
    llvm::Value *all = mine;
    for (uint32_t back = 0; back + 1 < vertices; back++)
      all = builder.CreateAnd(all, builder.CreateLoad(types._int, behind[back]));
    builder.CreateStore(builder.CreateLoad(types._int, behind[0]), behind[1]);
    builder.CreateStore(mine, behind[0]);
    // before that the write goes to a primitive the strip does not have yet, and is written again when it has
    auto complete = builder.CreateICmpUGE(written, builder.getInt32(vertices - 1));
    air.CreateSetMeshPrimitiveCulled(
        builder.CreateAdd(first_primitive, builder.CreateSelect(complete, builder.CreateSub(written, builder.getInt32(vertices - 1)), written)),
        builder.CreateAnd(complete, builder.CreateICmpNE(all, zero_const))
    );
  };

  // what the rasterized stream's emit and cut make of the mesh
  std::function<IREffect()> mesh_emit, mesh_cut;
  if (topology == air::MeshOutputTopology::Triangle) {
    mesh_emit = [&]() -> IREffect {
      auto current_write_vertex = builder.CreateLoad(types._int, next_write_vertex);
      auto current_vertex_offset = builder.CreateLoad(types._int, vertex_offset);
      auto current_vertex_with_offset = builder.CreateAdd(current_vertex_offset, current_write_vertex);

      auto current_primitive_count = builder.CreateLoad(types._int, primitive_count);
      auto current_primitive_idx = builder.CreateAdd(current_primitive_count, current_write_vertex);

      auto even_winding = builder.CreateZExt(
          builder.CreateICmpEQ(zero_const, builder.CreateAnd(current_write_vertex, one_const)), types._int
      );

      MeshOutputContext gs_out_ctx{current_vertex_with_offset, current_primitive_idx};
      for (auto &h : gs_output_handlers) {
        co_yield h(gs_out_ctx);
      }
      emit_clip_distances(current_vertex_with_offset);
      cull_primitive(current_write_vertex, current_primitive_count, 3);

      auto triple_primitive_idx = builder.CreateMul(current_primitive_idx, builder.getInt32(3));
      air.CreateSetMeshIndex(
          builder.CreateAdd(triple_primitive_idx, zero_const), current_vertex_with_offset
      );
      air.CreateSetMeshIndex(
          builder.CreateAdd(triple_primitive_idx, one_const),
          builder.CreateSub(builder.CreateAdd(current_vertex_with_offset, two_const), even_winding)
      );
      air.CreateSetMeshIndex(
          builder.CreateAdd(triple_primitive_idx, two_const),
          builder.CreateAdd(builder.CreateAdd(current_vertex_with_offset, one_const), even_winding)
      );

      builder.CreateStore(builder.CreateAdd(one_const, current_write_vertex), next_write_vertex);

      co_return {};
    };
    mesh_cut = [&]() -> IREffect {
      auto current_write_vertex = builder.CreateLoad(types._int, next_write_vertex);
      builder.CreateStore(zero_const, next_write_vertex);

      auto has_valid_primitive = builder.CreateICmpUGT(current_write_vertex, builder.getInt32(2));
      auto add_primitive_count = builder.CreateSelect(
          has_valid_primitive, builder.CreateSub(current_write_vertex, builder.getInt32(2)), zero_const
      );
      auto add_vertex_count = builder.CreateSelect(has_valid_primitive, current_write_vertex, zero_const);
      builder.CreateStore(
          builder.CreateAdd(builder.CreateLoad(types._int, vertex_offset), add_vertex_count), vertex_offset
      );
      builder.CreateStore(
          builder.CreateAdd(builder.CreateLoad(types._int, primitive_count), add_primitive_count), primitive_count
      );
      co_return {};
    };
  } else if (topology == air::MeshOutputTopology::Line) {
    mesh_emit = [&]() -> IREffect {
      auto current_write_vertex = builder.CreateLoad(types._int, next_write_vertex);
      builder.CreateStore(builder.CreateAdd(one_const, current_write_vertex), next_write_vertex);

      auto current_vertex_offset = builder.CreateLoad(types._int, vertex_offset);
      auto current_vertex_with_offset = builder.CreateAdd(current_vertex_offset, current_write_vertex);

      auto current_primitive_count = builder.CreateLoad(types._int, primitive_count);
      auto current_primitive_idx = builder.CreateAdd(current_primitive_count, current_write_vertex);

      MeshOutputContext gs_out_ctx{current_vertex_with_offset, current_primitive_idx};
      for (auto &h : gs_output_handlers) {
        co_yield h(gs_out_ctx);
      }
      emit_clip_distances(current_vertex_with_offset);
      cull_primitive(current_write_vertex, current_primitive_count, 2);

      auto double_primitive_idx = builder.CreateMul(current_primitive_idx, builder.getInt32(2));
      air.CreateSetMeshIndex(
          builder.CreateAdd(double_primitive_idx, zero_const), current_vertex_with_offset
      );
      air.CreateSetMeshIndex(
          builder.CreateAdd(double_primitive_idx, one_const),
          builder.CreateAdd(current_vertex_with_offset, one_const)
      );

      co_return {};
    };
    mesh_cut = [&]() -> IREffect {
      auto current_write_vertex = builder.CreateLoad(types._int, next_write_vertex);
      builder.CreateStore(zero_const, next_write_vertex);

      auto has_valid_primitive = builder.CreateICmpUGT(current_write_vertex, builder.getInt32(1));
      auto add_primitive_count = builder.CreateSelect(
          has_valid_primitive, builder.CreateSub(current_write_vertex, builder.getInt32(1)), zero_const
      );
      auto add_vertex_count = builder.CreateSelect(has_valid_primitive, current_write_vertex, zero_const);
      builder.CreateStore(
          builder.CreateAdd(builder.CreateLoad(types._int, vertex_offset), add_vertex_count), vertex_offset
      );
      builder.CreateStore(
          builder.CreateAdd(builder.CreateLoad(types._int, primitive_count), add_primitive_count), primitive_count
      );
      co_return {};
    };
  } else {
    mesh_emit = [&]() -> IREffect {
      // only one accumulator to maintain, simple one ~
      auto current_write_vertex = builder.CreateLoad(types._int, next_write_vertex);
      builder.CreateStore(builder.CreateAdd(one_const, current_write_vertex), next_write_vertex);

      MeshOutputContext gs_out_ctx{current_write_vertex, current_write_vertex};
      for (auto &h : gs_output_handlers) {
        co_yield h(gs_out_ctx);
      }
      emit_clip_distances(current_write_vertex);
      cull_primitive(current_write_vertex, zero_const, 1);
      air.CreateSetMeshPointSize(current_write_vertex, air.getFloat(1.0));
      air.CreateSetMeshIndex(current_write_vertex, current_write_vertex);
      co_return {};
    };
    mesh_cut = []() -> IREffect {
      // there is nothing to cut!
      co_return {};
    };
  }

  // stream output: per stream, the elements that go out, its buffer slots, and while it runs, the primitives this
  // invocation has completed and the vertices of its current strip, the last three kept
  uint32_t vertices_per_primitive = topology == air::MeshOutputTopology::Triangle ? 3
                                    : topology == air::MeshOutputTopology::Line   ? 2
                                                                                  : 1;
  std::vector<uint32_t> so_elements[4];
  uint32_t so_slots[4] = {};
  for (uint32_t i = 0; so && i < so->num_elements; i++) {
    so_elements[so->elements[i].stream].push_back(i);
    so_slots[so->elements[i].stream] |= 1u << so->elements[i].output_slot;
  }
  llvm::Value *so_count[4] = {}, *so_strip[4] = {}, *so_ring[4] = {}, *so_base[4] = {}, *so_limit[4] = {},
              *so_snapshot[4] = {}, *so_address[4] = {}, *so_scratch = nullptr;
  auto i64 = [&](pvalue v) { return builder.CreateZExt(v, types._long); };
  auto output_component = [&](const SM50_STREAM_OUTPUT_ELEMENT2 &e) {
    return builder.CreateLoad(
        types._int, builder.CreateGEP(
                        llvm::ArrayType::get(types._int4, max_output_register), resource_map.output.ptr_int4,
                        {zero_const, builder.getInt32(e.reg_id), builder.getInt32(e.component)}
                    )
    );
  };
  // what is built next runs only when `cond` holds, until the returned block: an emit may be built mid-block (DXIL),
  // which then splits there
  auto guard = [&](pvalue cond) {
    auto block = builder.GetInsertBlock();
    auto then = llvm::BasicBlock::Create(context, "so_then", block->getParent());
    llvm::BasicBlock *merge;
    if (builder.GetInsertPoint() == block->end()) {
      merge = llvm::BasicBlock::Create(context, "so_merge", block->getParent());
    } else {
      merge = llvm::SplitBlock(block, &*builder.GetInsertPoint());
      block->getTerminator()->eraseFromParent();
      builder.SetInsertPoint(block);
    }
    builder.CreateCondBr(cond, then, merge);
    builder.SetInsertPoint(then);
    return merge;
  };
  auto rejoin = [&](llvm::BasicBlock *merge) {
    builder.CreateBr(merge);
    builder.SetInsertPoint(merge, merge->begin());
  };
  // a completed primitive of stream `s`, whose vertex j's element k is value(j, k): the second pass writes it, in
  // order after every one before it, unless it would not fit in all the stream's buffers
  auto so_primitive = [&](uint32_t s, std::function<pvalue(uint32_t, uint32_t)> value) {
    auto index = builder.CreateLoad(types._int, so_count[s]);
    builder.CreateStore(builder.CreateAdd(index, one_const), so_count[s]);
    if (so->counting)
      return;
    auto g = builder.CreateAdd(so_base[s], i64(index));
    auto written = guard(builder.CreateICmpULT(g, so_limit[s]));
    for (uint32_t j = 0; j < vertices_per_primitive; j++)
      for (uint32_t k = 0; k < so_elements[s].size(); k++) {
        auto &e = so->elements[so_elements[s][k]];
        if (e.reg_id == ~0u)
          continue;
        auto vertex = builder.CreateAdd(builder.CreateMul(g, builder.getInt64(vertices_per_primitive)), builder.getInt64(j));
        auto at = builder.CreateAdd(
            builder.CreateAdd(so_address[e.output_slot], so_snapshot[e.output_slot]),
            builder.CreateAdd(builder.CreateMul(vertex, builder.getInt64(so->strides[e.output_slot])), builder.getInt64(e.offset))
        );
        builder.CreateStore(
            value(j, k), builder.CreateIntToPtr(at, types._int->getPointerTo((uint32_t)air::AddressSpace::device))
        );
      }
    rejoin(written);
  };
  // an emit completes a point, or a strip's line or triangle (odd triangles as (n, n + 2, n + 1), keeping winding)
  auto so_emit = [&](uint32_t s) {
    if (so_elements[s].empty())
      return;
    auto &elements = so_elements[s];
    if (vertices_per_primitive == 1) {
      so_primitive(s, [&](uint32_t, uint32_t k) { return output_component(so->elements[elements[k]]); });
      return;
    }
    auto ring_type = llvm::ArrayType::get(llvm::ArrayType::get(types._int, elements.size()), 3);
    auto n = builder.CreateLoad(types._int, so_strip[s]);
    for (uint32_t k = 0; k < elements.size(); k++)
      if (so->elements[elements[k]].reg_id != ~0u)
        builder.CreateStore(
            output_component(so->elements[elements[k]]),
            builder.CreateGEP(ring_type, so_ring[s], {zero_const, builder.CreateURem(n, builder.getInt32(3)), builder.getInt32(k)})
        );
    auto count = builder.CreateAdd(n, one_const);
    builder.CreateStore(count, so_strip[s]);
    auto next = guard(builder.CreateICmpUGE(count, builder.getInt32(vertices_per_primitive)));
    auto first = builder.CreateSub(count, builder.getInt32(vertices_per_primitive));
    auto odd = vertices_per_primitive == 3 ? builder.CreateAnd(first, one_const) : zero_const;
    pvalue positions[3] = {
        first, builder.CreateAdd(builder.CreateAdd(first, one_const), odd),
        builder.CreateSub(builder.CreateAdd(first, two_const), odd)
    };
    so_primitive(s, [&](uint32_t j, uint32_t k) {
      return builder.CreateLoad(
          types._int, builder.CreateGEP(
                          ring_type, so_ring[s],
                          {zero_const, builder.CreateURem(positions[j], builder.getInt32(3)), builder.getInt32(k)}
                      )
      );
    });
    rejoin(next);
  };
  // stream output sees every stream, the rasterizer only its own
  resource_map.call_emit = [&](uint32_t stream) -> IREffect {
    if (so && stream < 4)
      so_emit(stream);
    if (stream == rasterized_stream)
      co_yield mesh_emit();
    co_return {};
  };
  resource_map.call_cut = [&](uint32_t stream) -> IREffect {
    if (so && stream < 4 && so_strip[stream])
      builder.CreateStore(zero_const, so_strip[stream]);
    if (stream == rasterized_stream)
      co_yield mesh_cut();
    co_return {};
  };

  // the geometry payload's words; a tessellated primitive has none of them
  auto payload = function->getArg(payload_idx);
  auto valid_primitive_mask =
      builder.CreateLoad(types._int, builder.CreateConstInBoundsGEP1_32(types._int, payload, 1));
  auto warp_id =
      builder.CreateLoad(types._int, builder.CreateConstInBoundsGEP1_32(types._int, payload, 2));

  // a strip's primitive counts the whole ones before it, in the warp and (the carry) before the warp
  auto below = builder.CreateSub(builder.CreateShl(one_const, primitive_id_in_warp), one_const);
  auto before = builder.CreateLoad(types._int, builder.CreateConstInBoundsGEP1_32(types._int, payload, kPayloadPrimitivesBefore));
  auto strip_offset = builder.CreateLoad(types._int, builder.CreateConstInBoundsGEP1_32(types._int, payload, kPayloadStripOffset));
  bool restarts = is_strip && pShaderInternal->gs_input_primitive != D3D10_SB_PRIMITIVE_POINT;
  resource_map.patch_id = restarts
                              ? builder.CreateAdd(
                                    before,
                                    air.CreateIntUnOp(llvm::air::AIRBuilder::popcount, builder.CreateAnd(valid_primitive_mask, below))
                                )
                              : builder.CreateAdd(
                                    builder.CreateMul(warp_id, builder.getInt32(warp_primitive_count)), primitive_id_in_warp
                                );
  auto strip_odd = strip_parity(builder, air, valid_primitive_mask, primitive_id_in_warp, strip_offset);

  auto input_per_vertex_type = llvm::ArrayType::get(types._int4, pVertexStage->max_output_register);
  auto input_per_vertex_type_float = llvm::ArrayType::get(types._float4, pVertexStage->max_output_register);

  auto input_ptr_int4_type = llvm::ArrayType::get(input_per_vertex_type,  vertex_per_primitive);
  resource_map.input.ptr_int4 = builder.CreateAlloca(input_ptr_int4_type);
  resource_map.input.ptr_float4 = builder.CreateBitCast(
      resource_map.input.ptr_int4,
      llvm::ArrayType::get(input_per_vertex_type_float, vertex_per_primitive)->getPointerTo()
  );
  resource_map.input_element_count = pVertexStage->max_output_register;

  resource_map.output.ptr_int4 = builder.CreateAlloca(llvm::ArrayType::get(types._int4, max_output_register));
  resource_map.output.ptr_float4 = builder.CreateBitCast(
      resource_map.output.ptr_int4, llvm::ArrayType::get(types._float4, max_output_register)->getPointerTo()
  );
  resource_map.output_element_count = max_output_register;

  setup_temp_register(shader_info, resource_map, types, module, builder);
  setup_immediate_constant_buffer(shader_info, resource_map, types, module, builder);

  // this invocation's place: draw instance, object threadgroup, primitive, geometry instance
  pvalue so_invocation = nullptr, so_warp = nullptr, so_counts = nullptr;
  if (so) {
    auto targets_type = stream_output_targets_type(context, types);
    auto targets = function->getArg(so_idx);
    auto field = [&](uint32_t f, int32_t i = -1) {
      auto ptr = builder.CreateStructGEP(targets_type, targets, f);
      if (i >= 0)
        ptr = builder.CreateConstInBoundsGEP2_32(targets_type->getElementType(f), ptr, 0, i);
      return builder.CreateLoad(i < 0 ? targets_type->getElementType(f) : types._long, ptr);
    };
    so_scratch = builder.CreateIntToPtr(field(3), types._int->getPointerTo((uint32_t)air::AddressSpace::device));
    auto warps = field(4), instances = field(5);
    auto instance = builder.CreateLoad(types._int, builder.CreateConstInBoundsGEP1_32(types._int, payload, 0));
    so_warp = builder.CreateAdd(builder.CreateMul(instance, warps), warp_id);
    so_invocation = builder.CreateAdd(
        builder.CreateMul(
            builder.CreateAdd(builder.CreateMul(so_warp, builder.getInt32(warp_primitive_count)), primitive_id_in_warp),
            builder.getInt32(pShaderInternal->gs_instance_count)
        ),
        resource_map.gs_instance_id
    );
    so_counts = builder.CreateAdd(
        builder.getInt32(kStreamOutputTotals), builder.CreateMul(builder.CreateMul(warps, instances), builder.getInt32(4))
    );
    auto snapshots = builder.CreateBitCast(so_scratch, types._long->getPointerTo((uint32_t)air::AddressSpace::device));
    for (uint32_t b = 0; b < 4; b++) {
      so_address[b] = field(0, b);
      so_snapshot[b] = so->counting ? nullptr : builder.CreateLoad(types._long, builder.CreateConstInBoundsGEP1_32(types._long, snapshots, b));
    }
    for (uint32_t s = 0; s < 4; s++) {
      if (so_elements[s].empty())
        continue;
      so_count[s] = builder.CreateAlloca(types._int);
      builder.CreateStore(zero_const, so_count[s]);
      so_strip[s] = builder.CreateAlloca(types._int);
      builder.CreateStore(zero_const, so_strip[s]);
      so_ring[s] = builder.CreateAlloca(llvm::ArrayType::get(llvm::ArrayType::get(types._int, so_elements[s].size()), 3));
      if (so->counting)
        continue;
      // a stream's limit: the primitives that fit in every one of its buffers past their start, none when unbound
      so_limit[s] = builder.getInt64(~0ull);
      for (uint32_t b = 0; b < 4; b++) {
        if (!(so_slots[s] & (1u << b)))
          continue;
        auto size = field(1, b);
        auto room = builder.CreateSub(size, builder.CreateSelect(builder.CreateICmpULT(so_snapshot[b], size), so_snapshot[b], size));
        auto fit = builder.CreateUDiv(room, builder.getInt64(uint64_t(vertices_per_primitive) * so->strides[b]));
        fit = builder.CreateSelect(builder.CreateICmpEQ(so_address[b], builder.getInt64(0)), builder.getInt64(0), fit);
        so_limit[s] = builder.CreateSelect(builder.CreateICmpULT(fit, so_limit[s]), fit, so_limit[s]);
      }
      // its first primitive: those of the object threadgroups before (the object stage summed them) and of this
      // group's invocations before this one
      auto from = builder.CreateMul(so_warp, builder.getInt32(warp_primitive_count * pShaderInternal->gs_instance_count));
      auto head = builder.GetInsertBlock();
      auto loop = llvm::BasicBlock::Create(context, "so_prefix", function, active_);
      auto done = llvm::BasicBlock::Create(context, "so_prefixed", function, active_);
      auto before = builder.CreateLoad(
          types._long, builder.CreateBitCast(
                           builder.CreateConstInBoundsGEP1_32(types._int, payload, kPayloadStreamOutputBase + 2 * s),
                           types._long->getPointerTo(payload->getType()->getPointerAddressSpace())
                       )
      );
      builder.CreateCondBr(builder.CreateICmpULT(from, so_invocation), loop, done);
      builder.SetInsertPoint(loop);
      auto j = builder.CreatePHI(types._int, 2), sum = builder.CreatePHI(types._int, 2);
      j->addIncoming(from, head);
      sum->addIncoming(zero_const, head);
      auto count = builder.CreateLoad(
          types._int, builder.CreateGEP(
                          types._int, so_scratch,
                          {builder.CreateAdd(so_counts, builder.CreateAdd(builder.CreateMul(j, builder.getInt32(4)), builder.getInt32(s)))}
                      )
      );
      auto next_sum = builder.CreateAdd(sum, count), next_j = builder.CreateAdd(j, one_const);
      j->addIncoming(next_j, loop);
      sum->addIncoming(next_sum, loop);
      builder.CreateCondBr(builder.CreateICmpULT(next_j, so_invocation), loop, done);
      builder.SetInsertPoint(done);
      auto total = builder.CreatePHI(types._int, 2);
      total->addIncoming(zero_const, head);
      total->addIncoming(next_sum, loop);
      so_base[s] = builder.CreateAdd(before, i64(total));
    }
  }

  // the domain shader's patch is its workload's; the grid has more threadgroups than most workloads have primitives
  struct context domain_context{
      .builder = builder,
      .air = air,
      .binding = pHullStage ? *domain_binding : *binding_map,
      .llvm = context,
      .module = module,
      .function = function,
      .resource = domain_map,
      .types = types,
      .pso_sample_mask = 0xffffffff,
      .shader_type = pVertexStage->shader_type,
      .metal_version = metal_version,
  };
  dxbc::Converter domain(air, domain_context, domain_map);
  pvalue workloads = nullptr;
  if (pHullStage) {
    setup_temp_register(&pVertexStage->shader_info, domain_map, types, module, builder);
    setup_immediate_constant_buffer(&pVertexStage->shader_info, domain_map, types, module, builder);
    workloads = setup_domain_stage(pVertexStage, pHullStage, tessellation_payload, payload, workload, domain_context);
    // every primitive of a patch has the patch's number (D3D11.3 11.7.9.1)
    resource_map.patch_id = domain_map.patch_id;
  }
  auto corner_location = [&](pvalue corner) {
    return domain.DomainGetPrimitiveLocation(workload, primitive_id_in_warp, corner, get_output_primitive(pHullStage), workloads);
  };
  if (pHullStage) {
    auto first = llvm::BasicBlock::Create(context, "first_thread", function, active_);
    auto idle = llvm::BasicBlock::Create(context, "idle", function, active_);
    builder.CreateCondBr(
        builder.CreateICmpEQ(builder.CreateExtractElement(function->getArg(thread_idx), 0ull), zero_const), first, idle
    );
    builder.SetInsertPoint(idle);
    builder.CreateRetVoid();
    builder.SetInsertPoint(first);
  }
  builder.CreateCondBr(
      pHullStage ? corner_location(zero_const).second
                 : builder.CreateICmpNE(
                       builder.CreateAnd(valid_primitive_mask, builder.CreateShl(one_const, primitive_id_in_warp)), zero_const
                   ),
      active_, epilogue_bb
  );
  builder.SetInsertPoint(active_);

  auto vertices_ptr = builder.CreateBitCast(
      builder.CreateConstInBoundsGEP1_32(types._int, function->getArg(payload_idx), 4),
      types._int4->getPointerTo((uint32_t)air::AddressSpace::object_data)
  );

  auto load_vertex = [&](pvalue vertex_index, uint32_t vid) {
    for (unsigned reg = 0; reg < pVertexStage->max_output_register; reg++) {
      builder.CreateStore(
          builder.CreateLoad(
              types._int4, builder.CreateGEP(
                               types._int4, vertices_ptr,
                               {builder.CreateAdd(
                                   builder.CreateMul(vertex_index, builder.getInt32(pVertexStage->max_output_register)),
                                   builder.getInt32(reg)
                               )}
                           )
          ),
          builder.CreateGEP(
              input_ptr_int4_type, resource_map.input.ptr_int4,
              {zero_const, builder.getInt32(vid), builder.getInt32(reg)}
          )
      );
    }
  };

  if (pHullStage) {
    // the domain shader makes the primitive's vertices, a corner at a time
    auto head = builder.GetInsertBlock();
    auto next_corner = llvm::BasicBlock::Create(context, "corner", function);
    auto made = llvm::BasicBlock::Create(context, "corner_made", function);
    auto assembled = llvm::BasicBlock::Create(context, "assembled", function);
    builder.CreateBr(next_corner);
    builder.SetInsertPoint(next_corner);
    auto corner = builder.CreatePHI(types._int, 2);
    corner->addIncoming(zero_const, head);
    domain_map.domain = domain_location(corner_location(corner).first, builder, air);
    auto entry = convert_basicblocks(pVertexStage->entry(), domain_context, made);
    if (auto err = entry.takeError())
      return err;
    builder.CreateBr(entry.get());
    builder.SetInsertPoint(made);
    for (unsigned reg = 0; reg < pVertexStage->max_output_register; reg++)
      builder.CreateStore(
          builder.CreateLoad(
              types._int4,
              builder.CreateConstInBoundsGEP2_32(input_per_vertex_type, domain_map.output.ptr_int4, 0, reg)
          ),
          builder.CreateGEP(input_ptr_int4_type, resource_map.input.ptr_int4, {zero_const, corner, builder.getInt32(reg)})
      );
    auto following = builder.CreateAdd(corner, one_const);
    corner->addIncoming(following, builder.GetInsertBlock());
    builder.CreateCondBr(builder.CreateICmpULT(following, builder.getInt32(vertex_per_primitive)), next_corner, assembled);
    builder.SetInsertPoint(assembled);
    // an adjacency strip's triangles come assembled, as a list's
  } else if (restarts && pShaderInternal->gs_input_primitive != D3D10_SB_PRIMITIVE_TRIANGLE_ADJ) {
    auto leading_vertex_index = primitive_id_in_warp;
    switch (pShaderInternal->gs_input_primitive) {
    case microsoft::D3D10_SB_PRIMITIVE_TRIANGLE: {
      /*
      primitive 0: {0, 1, 2}
      primitive 1: {1, 3, 2}
      ...
      primitive n: {n, n + 1 + (n & 1), n + 2 - (n & 1)}

      counted from the strip's start, so a cut restarts the winding
      */
      auto odd_bit = strip_odd;
      load_vertex(leading_vertex_index, 0);
      load_vertex(builder.CreateAdd(builder.CreateAdd(leading_vertex_index, builder.getInt32(1)), odd_bit), 1);
      load_vertex(builder.CreateSub(builder.CreateAdd(leading_vertex_index, builder.getInt32(2)), odd_bit), 2);
      break;
    }
    case microsoft::D3D10_SB_PRIMITIVE_LINE: {
      /*
      primitive 0: {0, 1}
      primitive 1: {1, 2}
      ...
      primitive n: {n, n+1}
      */
      load_vertex(leading_vertex_index, 0);
      load_vertex(builder.CreateAdd(leading_vertex_index, builder.getInt32(1)), 1);
      break;
    }
    case microsoft::D3D10_SB_PRIMITIVE_LINE_ADJ: {
      /*
      primitive 0: {0, 1, 2, 3}
      primitive 1: {1, 2, 3, 4}
      ...
      primitive n: {n, n+1, n+2, n+3}
      */
      load_vertex(leading_vertex_index, 0);
      load_vertex(builder.CreateAdd(leading_vertex_index, builder.getInt32(1)), 1);
      load_vertex(builder.CreateAdd(leading_vertex_index, builder.getInt32(2)), 2);
      load_vertex(builder.CreateAdd(leading_vertex_index, builder.getInt32(3)), 3);
      break;
    }
    default:
      return llvm::make_error<UnsupportedFeature>(std::format(
          "unhandled geometry shader input primitive strip: {}", (uint32_t)pShaderInternal->gs_input_primitive
      ));
    }
  } else {
    auto leading_vertex_index = builder.CreateMul(builder.getInt32(vertex_per_primitive), primitive_id_in_warp);
    for (uint32_t vid = 0; vid < vertex_per_primitive; vid++) {
      load_vertex(builder.CreateAdd(leading_vertex_index, builder.getInt32(vid)), vid);
    }
  }

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
  };

  if (auto err = prologue.build(ctx).takeError()) {
    return err;
  }

  // the counting pass of a stream output draw runs the shader again: only the writing pass leaves its writes
  air.DiscardWrites = so && so->counting;
  auto real_entry = convert_basicblocks(pShaderInternal->entry(), ctx, epilogue_bb);
  air.DiscardWrites = false;
  if (auto err = real_entry.takeError()) {
    return err;
  }
  builder.CreateBr(real_entry.get());
  builder.SetInsertPoint(epilogue_bb);

  if (auto err = epilogue.build(ctx).takeError()) {
    return err;
  }
  if (auto err = mesh_cut().build(ctx).takeError()) {
    return err;
  }
  // a point is a primitive as it is emitted; a strip's are counted where it ends
  air.CreateSetMeshPrimitiveCount(builder.CreateLoad(
      types._int, topology == air::MeshOutputTopology::Point ? next_write_vertex : primitive_count
  ));
  // the first pass leaves each invocation's counts and each object threadgroup's totals, and the filled sizes as the
  // draw starts; the second pass's last invocation, which knows every stream's total, moves the filled sizes on
  if (so) {
    auto at = [&](pvalue index) { return builder.CreateGEP(types._int, so_scratch, {index}); };
    auto targets_type = stream_output_targets_type(context, types);
    auto filled = [&](uint32_t b) {
      return builder.CreateLoad(
          types._long, builder.CreateConstInBoundsGEP2_32(
                           targets_type->getElementType(2), builder.CreateStructGEP(targets_type, function->getArg(so_idx), 2), 0, b
                       )
      );
    };
    auto last = so->counting ? builder.CreateICmpEQ(so_invocation, zero_const)
                             : builder.CreateICmpEQ(
                                   builder.CreateAdd(so_invocation, one_const),
                                   builder.CreateMul(
                                       builder.CreateLShr(builder.CreateSub(so_counts, builder.getInt32(kStreamOutputTotals)), 2),
                                       builder.getInt32(warp_primitive_count * pShaderInternal->gs_instance_count)
                                   )
                               );
    for (uint32_t s = 0; so->counting && s < 4; s++) {
      if (so_elements[s].empty())
        continue;
      auto count = builder.CreateLoad(types._int, so_count[s]);
      builder.CreateStore(count, at(builder.CreateAdd(so_counts, builder.CreateAdd(builder.CreateMul(so_invocation, builder.getInt32(4)), builder.getInt32(s)))));
      air.CreateAtomicRMW(
          llvm::AtomicRMWInst::Add,
          at(builder.CreateAdd(builder.getInt32(kStreamOutputTotals), builder.CreateAdd(builder.CreateMul(so_warp, builder.getInt32(4)), builder.getInt32(s)))),
          count
      );
    }
    auto once = llvm::BasicBlock::Create(context, "so_once", function), end = llvm::BasicBlock::Create(context, "so_end", function);
    builder.CreateCondBr(last, once, end);
    builder.SetInsertPoint(once);
    auto snapshots = builder.CreateBitCast(so_scratch, types._long->getPointerTo((uint32_t)air::AddressSpace::device));
    for (uint32_t b = 0; b < 4; b++) {
      uint32_t stream = 4;
      for (uint32_t s = 0; s < 4; s++)
        if (so_slots[s] & (1u << b))
          stream = s;
      if (stream == 4)
        continue;
      auto location = builder.CreateIntToPtr(filled(b), types._long->getPointerTo((uint32_t)air::AddressSpace::device));
      auto bound = llvm::BasicBlock::Create(context, "so_bound", function), skip = llvm::BasicBlock::Create(context, "so_skip", function);
      builder.CreateCondBr(builder.CreateICmpNE(filled(b), builder.getInt64(0)), bound, skip);
      builder.SetInsertPoint(bound);
      if (so->counting) {
        builder.CreateStore(builder.CreateLoad(types._long, location), builder.CreateConstInBoundsGEP1_32(types._long, snapshots, b));
      } else {
        auto total = builder.CreateAdd(so_base[stream], i64(builder.CreateLoad(types._int, so_count[stream])));
        auto written = builder.CreateSelect(builder.CreateICmpULT(total, so_limit[stream]), total, so_limit[stream]);
        builder.CreateStore(
            builder.CreateAdd(so_snapshot[b], builder.CreateMul(written, builder.getInt64(uint64_t(vertices_per_primitive) * so->strides[b]))),
            location
        );
      }
      builder.CreateBr(skip);
      builder.SetInsertPoint(skip);
    }
    for (uint32_t s = 0; !so->counting && s < 4; s++) {
      if (so_elements[s].empty())
        continue;
      auto total = builder.CreateAdd(so_base[s], i64(builder.CreateLoad(types._int, so_count[s])));
      auto written = builder.CreateSelect(builder.CreateICmpULT(total, so_limit[s]), total, so_limit[s]);
      add_stream_output_statistics(
          builder, types,
          builder.CreateLoad(types._long, builder.CreateConstInBoundsGEP2_32(
                                              targets_type->getElementType(6), builder.CreateStructGEP(targets_type, function->getArg(so_idx), 6), 0, s
                                          )),
          written, total
      );
    }
    builder.CreateBr(end);
    builder.SetInsertPoint(end);
  }

  builder.CreateRetVoid();
  module.getOrInsertNamedMetadata("air.mesh")->addOperand(function_metadata);

  return llvm::Error::success();
}

// the mesh stage of a pipeline with stream output and no geometry shader, which gets no threadgroups
llvm::Error
convert_null_geometry_shader(const char *name, llvm::LLVMContext &context, llvm::Module &module) {
  air::FunctionSignatureBuilder func_signature;
  func_signature.DefineInput(air::InputPayload{.size = SM50_GEOMETRY_PAYLOAD_SIZE});
  func_signature.DefineInput(air::InputMesh{1, 1, air::MeshOutputTopology::Point});
  auto [function, function_metadata] = func_signature.CreateFunction(name, context, module, 0, false);
  llvm::IRBuilder<> builder(llvm::BasicBlock::Create(context, "entry", function));
  setup_metal_version(module, SM50_SHADER_METAL_310);
  builder.CreateRetVoid();
  module.getOrInsertNamedMetadata("air.mesh")->addOperand(function_metadata);
  return llvm::Error::success();
}

// without a geometry shader a primitive is its own vertices: those adjacent to it are seen by nothing (D3D11.3 8.15).
// where its vertex `corner` is among the payload's, for the group's primitive `lane`: a strip's primitives share
// their vertices, odd triangles as (n, n + 2, n + 1) with `odd` counted from the strip's start, unless the object
// stage assembled them, as a list's are
struct OwnVertices {
  uint32_t count = 0, first = 0, every = 1;
};
static OwnVertices
own_vertices(D3D10_SB_PRIMITIVE primitive) {
  switch (primitive) {
  case D3D10_SB_PRIMITIVE_POINT:
    return {1};
  case D3D10_SB_PRIMITIVE_LINE:
    return {2};
  case D3D10_SB_PRIMITIVE_TRIANGLE:
    return {3};
  case D3D10_SB_PRIMITIVE_LINE_ADJ:
    return {2, 1};
  case D3D10_SB_PRIMITIVE_TRIANGLE_ADJ:
    return {3, 0, 2};
  default:
    return {};
  }
}
static pvalue
own_vertex(
    llvm::IRBuilder<> &builder, D3D10_SB_PRIMITIVE primitive, bool strip, uint32_t registers, pvalue lane, pvalue odd,
    uint32_t corner
) {
  auto own = own_vertices(primitive);
  auto [vertices, primitives, per_primitive] = get_vertex_primitive_count_in_warp(primitive, strip, registers);
  if (!strip || vertices != primitives)
    return builder.CreateAdd(
        builder.CreateMul(lane, builder.getInt32(per_primitive)), builder.getInt32(own.first + corner * own.every)
    );
  auto at = builder.CreateAdd(lane, builder.getInt32(own.first + corner));
  return own.count < 3 || !corner ? at : corner == 1 ? builder.CreateAdd(at, odd) : builder.CreateSub(at, odd);
}

// the mesh stage of a pipeline without a geometry shader that still has to assemble primitives: each mesh
// threadgroup passes a primitive's own vertices on as the vertex shader output them. Metal has no topologies with
// adjacency, and culls by primitive where Direct3D discards one whose vertices are all behind a cull distance
// (D3D11.3 15.4.2)
llvm::Error
convert_pass_through_geometry_shader(
    const SM50ShaderInternal *pVertexStage, const char *name, llvm::LLVMContext &context, llvm::Module &module,
    SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs
) {
  SM50_SHADER_PSO_GEOMETRY_SHADER_DATA *pso_data = nullptr;
  args_get_data<SM50_SHADER_PSO_GEOMETRY_SHADER, SM50_SHADER_PSO_GEOMETRY_SHADER_DATA>(pArgs, &pso_data);
  auto primitive = (D3D10_SB_PRIMITIVE)pso_data->input_primitive;
  bool strip = pso_data->strip_topology;
  auto own = own_vertices(primitive);
  if (!own.count)
    return llvm::make_error<UnsupportedFeature>("a primitive that only a geometry shader can take");
  SM50_SHADER_METAL_VERSION metal_version = SM50_SHADER_METAL_310;
  SM50_SHADER_COMMON_DATA *sm50_common = nullptr;
  if (args_get_data<SM50_SHADER_COMMON, SM50_SHADER_COMMON_DATA>(pArgs, &sm50_common))
    metal_version = sm50_common->metal_version;

  air::AirType types(context);
  air::FunctionSignatureBuilder func_signature;
  uint32_t payload_idx = func_signature.DefineInput(air::InputPayload{.size = SM50_GEOMETRY_PAYLOAD_SIZE});
  func_signature.DefineInput(air::InputMesh{
      own.count, 1,
      own.count == 3   ? air::MeshOutputTopology::Triangle
      : own.count == 2 ? air::MeshOutputTopology::Line
                       : air::MeshOutputTopology::Point
  });
  uint32_t tg_in_grid_idx = func_signature.DefineInput(air::InputThreadgroupPositionInGrid{});
  func_signature.DefineMeshPrimitiveOutput(air::OutputPrimitiveID{});
  // what the vertex shader outputs, as the mesh's: the position, the layer and viewport its first vertex chooses,
  // and its other registers as varyings
  struct Varying {
    uint32_t reg, component, index;
  };
  std::vector<Varying> varyings;
  uint32_t declared = 0;
  for (auto &element : pVertexStage->output_signature) {
    switch (element.systemValue()) {
    case D3D10_SB_NAME_POSITION:
      func_signature.DefineMeshVertexOutput(air::OutputPosition{.type = air::msl_float4});
      continue;
    case D3D10_SB_NAME_RENDER_TARGET_ARRAY_INDEX:
      func_signature.DefineMeshPrimitiveOutput(air::OutputRenderTargetArrayIndex{});
      continue;
    case D3D10_SB_NAME_VIEWPORT_ARRAY_INDEX:
      func_signature.DefineMeshPrimitiveOutput(air::OutputViewportArrayIndex{});
      continue;
    case D3D10_SB_NAME_UNDEFINED:
    case D3D10_SB_NAME_CLIP_DISTANCE:
    case D3D10_SB_NAME_CULL_DISTANCE:
      break;
    default:
      continue;
    }
    if (std::exchange(declared, declared | 1u << element.reg()) >> element.reg() & 1)
      continue;
    auto component_types = pVertexStage->register_types(element.reg(), element.componentType());
    for (uint32_t c = 0; c < 4; c++) {
      varyings.push_back({element.reg(), c, (uint32_t)varyings.size()});
      func_signature.DefineMeshVertexOutput(air::OutputMeshData{
          .user = varying_name(element.reg(), c), .type = varying_type(component_types[c]), .index = varyings.back().index
      });
    }
  }
  if (pVertexStage->clip_distance_scalars.size())
    func_signature.DefineMeshVertexOutput(air::OutputClipDistance{.count = pVertexStage->clip_distance_scalars.size()});
  if (pVertexStage->cull_distance_scalars.size())
    func_signature.DefineMeshPrimitiveOutput(air::OutputPrimitiveCulled{});
  // Metal's points have a size to be given, Direct3D's are a pixel wide (D3D11.3 3.4.6)
  if (own.count == 1)
    func_signature.DefineMeshVertexOutput(air::OutputPointSize{});

  auto [function, function_metadata] = func_signature.CreateFunction(name, context, module, 0, false);
  auto entry = llvm::BasicBlock::Create(context, "entry", function);
  auto active = llvm::BasicBlock::Create(context, "active", function);
  auto done = llvm::BasicBlock::Create(context, "done", function);
  llvm::IRBuilder<> builder(entry);
  llvm::raw_null_ostream nulldbg{};
  llvm::air::AIRBuilder air({}, builder, nulldbg);
  setup_metal_version(module, metal_version);
  auto payload = function->getArg(payload_idx);
  auto word = [&](uint32_t i) {
    return builder.CreateLoad(types._int, builder.CreateConstInBoundsGEP1_32(types._int, payload, i));
  };
  auto lane = builder.CreateExtractElement(function->getArg(tg_in_grid_idx), (uint32_t)0);
  auto valid = word(1);
  auto one = builder.getInt32(1), zero = builder.getInt32(0);
  auto is_valid = builder.CreateICmpNE(builder.CreateAnd(valid, builder.CreateShl(one, lane)), zero);
  auto count = builder.CreateAlloca(types._int);
  builder.CreateStore(zero, count);
  builder.CreateCondBr(is_valid, active, done);

  builder.SetInsertPoint(active);
  // mesh threadgroups each emit one primitive, so its ID counts the draw's preceding primitives (D3D11.3 8.17)
  auto below = builder.CreateSub(builder.CreateShl(one, lane), one);
  air.CreateSetMeshPrimitiveID(
      zero, builder.CreateAdd(
                word(kPayloadPrimitivesBefore),
                air.CreateIntUnOp(llvm::air::AIRBuilder::popcount, builder.CreateAnd(valid, below))
            )
  );
  auto odd = strip_parity(builder, air, valid, lane, word(kPayloadStripOffset));
  uint32_t registers = pVertexStage->max_output_register;
  // component `c` of register `reg` of the primitive's vertex `corner`
  auto component = [&](uint32_t corner, uint32_t reg, uint32_t c) {
    auto vertex = own_vertex(builder, primitive, strip, registers, lane, odd, corner);
    return builder.CreateLoad(
        types._int, builder.CreateGEP(
                        types._int, payload,
                        {builder.CreateAdd(
                            builder.CreateMul(vertex, builder.getInt32(registers * 4)), builder.getInt32(4 + reg * 4 + c)
                        )}
                    )
    );
  };
  auto real = [&](pvalue bits) { return builder.CreateBitCast(bits, types._float); };
  for (uint32_t corner = 0; corner < own.count; corner++) {
    auto vertex = builder.getInt32(corner);
    for (auto &element : pVertexStage->output_signature) {
      if (element.systemValue() != D3D10_SB_NAME_POSITION)
        continue;
      pvalue position = llvm::UndefValue::get(types._float4);
      for (uint32_t c = 0; c < 4; c++)
        position = builder.CreateInsertElement(position, real(component(corner, element.reg(), c)), c);
      air.CreateSetMeshPosition(vertex, position);
    }
    for (auto &v : varyings) {
      auto value = component(corner, v.reg, v.component);
      bool integer = pVertexStage->register_types(v.reg, RegisterComponentType::Float)[v.component] != RegisterComponentType::Float;
      air.CreateSetMeshVertexData(vertex, builder.getInt32(v.index), integer ? value : real(value));
    }
    for (auto distance : llvm::enumerate(pVertexStage->clip_distance_scalars))
      air.CreateSetMeshClipDistance(
          vertex, builder.getInt32(distance.index()), real(component(corner, distance.value().reg, distance.value().component))
      );
    if (own.count == 1)
      air.CreateSetMeshPointSize(vertex, air.getFloat(1.0));
    air.CreateSetMeshIndex(vertex, vertex);
  }
  // a primitive's layer and viewport are its first vertex's
  for (auto &element : pVertexStage->output_signature) {
    auto value = [&] { return component(0, element.reg(), std::countr_zero(element.mask())); };
    if (element.systemValue() == D3D10_SB_NAME_RENDER_TARGET_ARRAY_INDEX)
      air.CreateSetMeshRenderTargetArrayIndex(zero, value());
    if (element.systemValue() == D3D10_SB_NAME_VIEWPORT_ARRAY_INDEX)
      air.CreateSetMeshViewportArrayIndex(zero, value());
  }
  // culled by a distance that every vertex is behind: negative, or NaN
  pvalue culled = builder.getFalse();
  for (auto &distance : pVertexStage->cull_distance_scalars) {
    pvalue behind = builder.getTrue();
    for (uint32_t corner = 0; corner < own.count; corner++)
      behind = builder.CreateAnd(
          behind, builder.CreateFCmpULT(real(component(corner, distance.reg, distance.component)), llvm::ConstantFP::get(types._float, 0))
      );
    culled = builder.CreateOr(culled, behind);
  }
  if (pVertexStage->cull_distance_scalars.size())
    air.CreateSetMeshPrimitiveCulled(zero, culled);
  builder.CreateStore(one, count);
  builder.CreateBr(done);

  builder.SetInsertPoint(done);
  air.CreateSetMeshPrimitiveCount(builder.CreateLoad(types._int, count));
  builder.CreateRetVoid();
  module.getOrInsertNamedMetadata("air.mesh")->addOperand(function_metadata);
  return llvm::Error::success();
}

// stream output without a geometry shader, by an object threadgroup's threads: `valid` its whole primitives of
// `primitive`, `vertices` the vertices' output registers in the payload, `group` its place in the draw and `before` the
// primitives of the groups before it (second pass)
static llvm::Error
stream_out_primitives(
    llvm::IRBuilder<> &builder, llvm::air::AIRBuilder &air, air::AirType &types, SM50_SHADER_STREAM_OUTPUT_DATA *so,
    pvalue targets, D3D10_SB_PRIMITIVE primitive, bool strip, pvalue vertices, uint32_t registers, pvalue lane,
    uint32_t primitives, pvalue valid, pvalue group, pvalue before, pvalue carry
) {
  auto &context = builder.getContext();
  uint32_t per_primitive = own_vertices(primitive).count;
  if (!per_primitive)
    return llvm::make_error<UnsupportedFeature>("stream output of a primitive that only a geometry shader can take");
  auto function = builder.GetInsertBlock()->getParent();
  auto targets_type = stream_output_targets_type(context, types);
  auto field = [&](uint32_t f, int32_t i = -1) {
    auto ptr = builder.CreateStructGEP(targets_type, targets, f);
    if (i >= 0)
      ptr = builder.CreateConstInBoundsGEP2_32(targets_type->getElementType(f), ptr, 0, i);
    return builder.CreateLoad(i < 0 ? targets_type->getElementType(f) : types._long, ptr);
  };
  auto device = [&](llvm::Type *ty, pvalue address) {
    return builder.CreateIntToPtr(address, ty->getPointerTo((uint32_t)air::AddressSpace::device));
  };
  auto when = [&](pvalue cond, auto then) {
    auto yes = llvm::BasicBlock::Create(context, "so_yes", function), no = llvm::BasicBlock::Create(context, "so_no", function);
    builder.CreateCondBr(cond, yes, no);
    builder.SetInsertPoint(yes);
    then();
    builder.CreateBr(no);
    builder.SetInsertPoint(no);
  };
  auto scratch = device(types._int, field(3));
  auto snapshots = device(types._long, field(3));
  auto zero = builder.getInt32(0), one = builder.getInt32(1);
  auto count = air.CreateIntUnOp(llvm::air::AIRBuilder::popcount, valid);
  uint32_t slots = 0;
  for (uint32_t i = 0; i < so->num_elements; i++)
    slots |= 1u << so->elements[i].output_slot;
  auto first = builder.CreateICmpEQ(lane, zero);
  if (so->counting) {
    // the group's total, and from the draw's first group, the filled sizes it starts at
    when(first, [&] {
      builder.CreateStore(count, builder.CreateGEP(types._int, scratch, {builder.CreateAdd(builder.getInt32(kStreamOutputTotals), builder.CreateMul(group, builder.getInt32(4)))}));
      when(builder.CreateICmpEQ(group, zero), [&] {
        for (uint32_t b = 0; b < 4; b++)
          if (slots & (1u << b))
            when(builder.CreateICmpNE(field(2, b), builder.getInt64(0)), [&] {
              builder.CreateStore(builder.CreateLoad(types._long, device(types._long, field(2, b))), builder.CreateConstInBoundsGEP1_32(types._long, snapshots, b));
            });
      });
    });
    return llvm::Error::success();
  }
  pvalue snapshot[4], limit = builder.getInt64(~0ull);
  for (uint32_t b = 0; b < 4; b++) {
    snapshot[b] = builder.CreateLoad(types._long, builder.CreateConstInBoundsGEP1_32(types._long, snapshots, b));
    if (!(slots & (1u << b)))
      continue;
    auto size = field(1, b);
    auto room = builder.CreateSub(size, builder.CreateSelect(builder.CreateICmpULT(snapshot[b], size), snapshot[b], size));
    auto fit = builder.CreateUDiv(room, builder.getInt64(uint64_t(per_primitive) * so->strides[b]));
    fit = builder.CreateSelect(builder.CreateICmpEQ(field(0, b), builder.getInt64(0)), builder.getInt64(0), fit);
    limit = builder.CreateSelect(builder.CreateICmpULT(fit, limit), fit, limit);
  }
  auto below = builder.CreateSub(builder.CreateShl(one, lane), one);
  auto mine = builder.CreateAnd(builder.CreateICmpULT(lane, builder.getInt32(primitives)),
                                builder.CreateICmpNE(builder.CreateAnd(valid, builder.CreateShl(one, lane)), zero));
  when(mine, [&] {
    auto g = builder.CreateAdd(before, builder.CreateZExt(air.CreateIntUnOp(llvm::air::AIRBuilder::popcount, builder.CreateAnd(valid, below)), types._long));
    when(builder.CreateICmpULT(g, limit), [&] {
      // the primitive's own vertices, a strip's parity counted from its last cut
      auto odd = per_primitive == 3 && strip ? strip_parity(builder, air, valid, lane, carry) : zero;
      for (uint32_t j = 0; j < per_primitive; j++) {
        auto position = own_vertex(builder, primitive, strip, registers, lane, odd, j);
        for (uint32_t i = 0; i < so->num_elements; i++) {
          auto &e = so->elements[i];
          if (e.reg_id == ~0u || e.stream)
            continue;
          auto value = builder.CreateLoad(
              types._int, builder.CreateGEP(types._int, vertices, {builder.CreateAdd(builder.CreateMul(position, builder.getInt32(registers * 4)), builder.getInt32(e.reg_id * 4 + e.component))})
          );
          auto vertex = builder.CreateAdd(builder.CreateMul(g, builder.getInt64(per_primitive)), builder.getInt64(j));
          builder.CreateStore(value, device(types._int, builder.CreateAdd(
                                                            builder.CreateAdd(field(0, e.output_slot), snapshot[e.output_slot]),
                                                            builder.CreateAdd(builder.CreateMul(vertex, builder.getInt64(so->strides[e.output_slot])), builder.getInt64(e.offset))
                                                        )));
        }
      }
    });
  });
  // the draw's last group moves the filled sizes on by what the stream wrote
  auto last = builder.CreateICmpEQ(builder.CreateAdd(group, one), builder.CreateMul(field(4), field(5)));
  when(builder.CreateAnd(first, last), [&] {
    auto total = builder.CreateAdd(before, builder.CreateZExt(count, types._long));
    auto written = builder.CreateSelect(builder.CreateICmpULT(total, limit), total, limit);
    add_stream_output_statistics(builder, types, field(6, 0), written, total);
    for (uint32_t b = 0; b < 4; b++)
      if (slots & (1u << b))
        when(builder.CreateICmpNE(field(2, b), builder.getInt64(0)), [&] {
          builder.CreateStore(
              builder.CreateAdd(snapshot[b], builder.CreateMul(written, builder.getInt64(uint64_t(per_primitive) * so->strides[b]))),
              device(types._long, field(2, b))
          );
        });
  });
  return llvm::Error::success();
}

llvm::Error
convert_dxbc_vertex_for_geometry_shader(
    const SM50ShaderInternal *pShaderInternal, const char *name, const SM50ShaderInternal *pGeometryStage,
    llvm::LLVMContext &context, llvm::Module &module, SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs
) {
  auto func_signature = pShaderInternal->func_signature; // copy
  auto shader_info = &(pShaderInternal->shader_info);

  bool is_strip = false;

  uint32_t max_input_register = pShaderInternal->max_input_register;
  uint32_t max_output_register = pShaderInternal->max_output_register;
  SM50_SHADER_IA_INPUT_LAYOUT_DATA *ia_layout = nullptr;
  SM50_SHADER_PSO_GEOMETRY_SHADER_DATA *pso_data = nullptr;
  args_get_data<SM50_SHADER_IA_INPUT_LAYOUT, SM50_SHADER_IA_INPUT_LAYOUT_DATA>(pArgs, &ia_layout);
  if (args_get_data<SM50_SHADER_PSO_GEOMETRY_SHADER, SM50_SHADER_PSO_GEOMETRY_SHADER_DATA>(pArgs, &pso_data)) {
    is_strip = pso_data->strip_topology;
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

  // without a geometry shader (stream output only) the pipeline names the input primitive, and the object stage
  // streams it out itself
  auto input_primitive = pGeometryStage ? pGeometryStage->gs_input_primitive
                                        : (D3D10_SB_PRIMITIVE)(pso_data ? pso_data->input_primitive : 0);
  // without a geometry shader, mesh threadgroups follow only to pass the primitives on
  bool pass_through = !pGeometryStage && pso_data && pso_data->pass_through;
  uint32_t gs_instances = pGeometryStage ? pGeometryStage->gs_instance_count : pass_through;
  // stream output's second pass starts each object threadgroup's primitives after those of every one before it
  SM50_SHADER_STREAM_OUTPUT_DATA *so = nullptr;
  args_get_data<SM50_SHADER_STREAM_OUTPUT, SM50_SHADER_STREAM_OUTPUT_DATA>(pArgs, &so);
  bool so_base = so && !so->counting;
  bool is_indexed_draw = ia_layout && ia_layout->index_buffer_format > 0;
  // with an index buffer view (D3D12), whether the draw has indices is known only at run time
  bool index_view = ia_layout && ia_layout->index_buffer_format == SM50_INDEX_BUFFER_FORMAT_VIEW;

  IREffect prologue([](auto) { return std::monostate(); });
  IRValue epilogue([](struct context ctx) -> pvalue {
    auto retTy = ctx.function->getReturnType();
    if (retTy->isVoidTy()) {
      return nullptr;
    }
    return llvm::UndefValue::get(retTy);
  });

  io_binding_map resource_map;
  air::AirType types(context);

  {
    SignatureContext sig_ctx(prologue, epilogue, func_signature, resource_map);
    sig_ctx.ia_layout = ia_layout;
    sig_ctx.skip_vertex_output = true;
    for (auto &p : pShaderInternal->signature_handlers) {
      p(sig_ctx);
    }
  }

  auto binding_map = rootsig ? setup_binding_rootsig(
                                   shader_info, func_signature, module, D3D10_SB_VERTEX_SHADER, rootsig->bytecode,
                                   rootsig->bytecode_length
                               )
                             : setup_binding_table2(shader_info, func_signature, module);

  uint32_t payload_idx = func_signature.DefineInput(air::InputPayload{.size = SM50_GEOMETRY_PAYLOAD_SIZE});
  // (warp_size, 1, 1)
  uint32_t thread_id_idx = func_signature.DefineInput(air::InputThreadPositionInThreadgroup{});
  // (warp_count, instance_count, 1)
  // warp_count = ceil(index_count / warp_size)
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

  uint32_t so_idx = so_base || (so && !pGeometryStage) ? stream_output_input(func_signature, context, types) : ~0u;

  auto [function, function_metadata] = func_signature.CreateFunction(name, context, module, 0, true);

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

  auto active = llvm::BasicBlock::Create(context, "active", function);
  auto will_dispatch = llvm::BasicBlock::Create(context, "will_dispatch", function);
  auto dispatch = llvm::BasicBlock::Create(context, "dispatch", function);
  auto return_ = llvm::BasicBlock::Create(context, "return", function);

  auto payload = function->getArg(payload_idx);
  auto thread_position_in_group = function->getArg(thread_id_idx);
  auto warp_vertex_id = builder.CreateExtractElement(thread_position_in_group, (uint32_t)0);

  auto threadgroup_position_in_grid = function->getArg(tg_id_idx);
  auto warp_id = builder.CreateExtractElement(threadgroup_position_in_grid, (uint32_t)0);
  auto instance_id = builder.CreateExtractElement(threadgroup_position_in_grid, (uint32_t)1);

  auto draw_arguments = builder.CreateLoad(
    is_indexed_draw ? types._dxmt_draw_indexed_arguments
                    : types._dxmt_draw_arguments,
    function->getArg(draw_argument_idx)
  );

  auto vertex_count = builder.CreateExtractValue(draw_arguments, 0);
  // a view's format is 0 for a draw without indices, whose arguments have no start index field
  llvm::Value *indexed = builder.getInt1(is_indexed_draw), *view = nullptr, *format = nullptr;
  if (index_view) {
    view = function->getArg(index_buffer_idx);
    format = builder.CreateLoad(types._int, builder.CreateStructGEP(view->getType()->getNonOpaquePointerElementType(), view, 2));
    indexed = builder.CreateICmpNE(format, builder.getInt32(0));
  }
  auto shifted = [&](unsigned field) {
    return builder.CreateSelect(
        indexed, builder.CreateExtractValue(draw_arguments, field), builder.CreateExtractValue(draw_arguments, field - 1)
    );
  };

  resource_map.input.ptr_int4 = builder.CreateAlloca(llvm::ArrayType::get(types._int4, max_input_register));
  resource_map.input.ptr_float4 = builder.CreateBitCast(
      resource_map.input.ptr_int4, llvm::ArrayType::get(types._float4, max_input_register)->getPointerTo()
  );
  resource_map.input_element_count = max_input_register;

  auto [warp_vertex_count, warp_primitive_count, vertex_per_primitive] =
      get_vertex_primitive_count_in_warp(input_primitive, is_strip, max_output_register);
  if (!warp_primitive_count)
    return llvm::make_error<UnsupportedFeature>(
        "a primitive whose vertices' output registers do not fit an object threadgroup's payload"
    );

  auto payload_output_ptr = builder.CreateConstInBoundsGEP1_32(
      types._int, payload, 4
  );

  resource_map.output.ptr_int4 = builder.CreateBitCast(
      payload_output_ptr,
      llvm::ArrayType::get(types._int4, max_output_register)->getPointerTo((uint32_t)air::AddressSpace::object_data)
  );
  resource_map.output.ptr_int4 = builder.CreateGEP(
      resource_map.output.ptr_int4->getType()->getNonOpaquePointerElementType(), resource_map.output.ptr_int4,
      {warp_vertex_id}
  );
  resource_map.output.ptr_float4 = builder.CreateBitCast(
      payload_output_ptr,
      llvm::ArrayType::get(types._float4, max_output_register)->getPointerTo((uint32_t)air::AddressSpace::object_data)
  );
  resource_map.output.ptr_float4 = builder.CreateGEP(
      resource_map.output.ptr_float4->getType()->getNonOpaquePointerElementType(), resource_map.output.ptr_float4,
      {warp_vertex_id}
  );

  resource_map.output_element_count = max_output_register;

  llvm::GlobalVariable *valid_vertex_mask = new llvm::GlobalVariable(
      module, types._int, false, llvm::GlobalValue::InternalLinkage, llvm::Constant::getNullValue(types._int),
      "valid_vertex_mask", nullptr, llvm::GlobalValue::NotThreadLocal, (uint32_t)air::AddressSpace::threadgroup
  );

  setup_temp_register(shader_info, resource_map, types, module, builder);
  setup_immediate_constant_buffer(shader_info, resource_map, types, module, builder);

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
  };

  // the draw's index at `position`, and whether it cuts a strip: D3D12's pipeline names the cutting index, D3D11 cuts
  // at its format's all-ones. without indices a vertex is its position
  bool strip = is_strip && input_primitive != D3D10_SB_PRIMITIVE_POINT;
  bool cuts = strip && index_buffer_idx != ~0u && (!index_view || (pso_data && pso_data->strip_cut));
  auto index_at = [&](pvalue position) -> std::pair<pvalue, pvalue> {
    if (index_view) {
      auto index = index_from_view(
          builder, types, view, format, builder.CreateAdd(builder.CreateExtractValue(draw_arguments, 2), position)
      );
      pvalue cut = builder.getFalse();
      if (cuts)
        cut = builder.CreateAnd(indexed, builder.CreateICmpEQ(index, builder.getInt32(pso_data->strip_cut_index)));
      return {builder.CreateSelect(indexed, index, position), cut};
    }
    if (index_buffer_idx == ~0u)
      return {position, builder.getFalse()};
    auto index_buffer = function->getArg(index_buffer_idx);
    auto type = index_buffer->getType()->getNonOpaquePointerElementType();
    auto index = builder.CreateLoad(
        type, builder.CreateGEP(type, index_buffer, {builder.CreateAdd(builder.CreateExtractValue(draw_arguments, 2), position)})
    );
    return {builder.CreateZExt(index, types._int),
            cuts ? builder.CreateICmpEQ(index, llvm::Constant::getAllOnesValue(type)) : (pvalue)builder.getFalse()};
  };
  auto umin = [&](pvalue a, pvalue b) { return builder.CreateSelect(builder.CreateICmpULT(a, b), a, b); };
  auto simd = [&](const char *name, pvalue value) {
    return builder.CreateCall(
        module.getOrInsertFunction(
            name, llvm::FunctionType::get(types._int, {types._int}, false),
            llvm::AttributeList::get(context, {{~0U, llvm::Attribute::get(context, llvm::Attribute::Convergent)}})
        ),
        {value}
    );
  };

  // the group's threads: a strip's primitives share vertices with the next group's, unless they are assembled here
  // (adjacency strips), each of a primitive's vertices by a thread
  uint32_t step = warp_vertex_count / warp_primitive_count;
  bool assembled = strip && step != 1;
  uint32_t threads = assembled ? warp_primitive_count * vertex_per_primitive
                     : strip   ? warp_vertex_count + vertex_per_primitive - step
                               : warp_vertex_count;
  auto group_start = builder.CreateMul(warp_id, builder.getInt32(warp_vertex_count));

  // what the group's primitives need from before it (kPayloadPrimitivesBefore, kPayloadStripOffset). only cuts make
  // them more than the group's place: then the threads scan the indices before the group in equal runs, each counting
  // the primitives that end in its run and finding the last cut
  pvalue primitives_before = builder.CreateMul(warp_id, builder.getInt32(warp_primitive_count));
  pvalue strip_offset = group_start;
  if (cuts) {
    auto zero = builder.getInt32(0), one = builder.getInt32(1), reach = builder.getInt32(vertex_per_primitive - 1);
    auto start = umin(group_start, vertex_count);
    auto end = builder.CreateAdd(start, umin(builder.CreateSub(vertex_count, start), reach));
    // where primitives lead only every `step` indices, counting them needs each run's length from its start, which a
    // run of the scan that begins mid-strip does not have: there one thread scans alone
    uint32_t scanners = step == 1 ? threads : 1;
    auto length = builder.CreateAdd(
        builder.CreateUDiv(end, builder.getInt32(scanners)),
        builder.CreateZExt(builder.CreateICmpNE(builder.CreateURem(end, builder.getInt32(scanners)), zero), types._int)
    );
    auto lo = umin(builder.CreateMul(warp_vertex_id, length), end);
    auto hi = builder.CreateAdd(lo, umin(length, builder.CreateSub(end, lo)));
    auto head = builder.GetInsertBlock();
    auto loop = llvm::BasicBlock::Create(context, "strip_scan", function, epilogue_bb);
    auto done = llvm::BasicBlock::Create(context, "strip_scanned", function, epilogue_bb);
    auto first = builder.CreateSub(lo, umin(lo, reach));
    // a draw without indices has no cuts to look for
    builder.CreateCondBr(builder.CreateAnd(indexed, builder.CreateICmpULT(first, hi)), loop, done);
    builder.SetInsertPoint(loop);
    auto i = builder.CreatePHI(types._int, 2), run = builder.CreatePHI(types._int, 2),
         whole = builder.CreatePHI(types._int, 2), cut_end = builder.CreatePHI(types._int, 2);
    auto cut = index_at(i).second;
    auto next_run = builder.CreateSelect(cut, zero, builder.CreateAdd(run, one));
    // a primitive ends at an index its strip reaches with all of its vertices, and every `step` after
    auto past = builder.CreateSub(next_run, builder.getInt32(vertex_per_primitive));
    auto ends = builder.CreateAnd(
        builder.CreateICmpUGE(next_run, builder.getInt32(vertex_per_primitive)),
        builder.CreateICmpEQ(builder.CreateURem(past, builder.getInt32(step)), zero)
    );
    auto next_whole =
        builder.CreateAdd(whole, builder.CreateZExt(builder.CreateAnd(builder.CreateICmpUGE(i, lo), ends), types._int));
    auto next_i = builder.CreateAdd(i, one);
    auto next_cut_end = builder.CreateSelect(builder.CreateAnd(cut, builder.CreateICmpULT(i, start)), next_i, cut_end);
    i->addIncoming(first, head), i->addIncoming(next_i, loop);
    run->addIncoming(zero, head), run->addIncoming(next_run, loop);
    whole->addIncoming(zero, head), whole->addIncoming(next_whole, loop);
    cut_end->addIncoming(zero, head), cut_end->addIncoming(next_cut_end, loop);
    builder.CreateCondBr(builder.CreateICmpULT(next_i, hi), loop, done);
    builder.SetInsertPoint(done);
    auto wholes = builder.CreatePHI(types._int, 2), cut_ends = builder.CreatePHI(types._int, 2);
    wholes->addIncoming(zero, head), wholes->addIncoming(next_whole, loop);
    cut_ends->addIncoming(zero, head), cut_ends->addIncoming(next_cut_end, loop);
    primitives_before = builder.CreateSelect(indexed, simd("air.simd_sum.u.i32", wholes), primitives_before);
    strip_offset = builder.CreateSub(group_start, simd("air.simd_max.u.i32", cut_ends));
  }

  // explicit initialization
  air.CreateAtomicRMW(llvm::AtomicRMWInst::BinOp::And, valid_vertex_mask, builder.getInt32(0));

  if (assembled) {
    // a triangle strip with adjacency (D3D11.3 functional spec, 8.15): triangle n of a strip leads at its index 2n and
    // needs index 2n + 5; its vertices alternate with the ones across its edges, which lie two indices back, three
    // ahead and six ahead (the strip's first and last triangles take the odd index beside them instead), and odd
    // triangles swap two corners to keep the winding. thread 6p + k runs vertex k of the group's p-th triangle
    auto zero = builder.getInt32(0), one = builder.getInt32(1);
    auto mine = builder.CreateUDiv(warp_vertex_id, builder.getInt32(vertex_per_primitive));
    auto corner = builder.CreateURem(warp_vertex_id, builder.getInt32(vertex_per_primitive));
    // whether no vertex is at an index of the group, from one before it to where the triangle after its last one
    // would end
    int32_t reach = vertex_per_primitive;
    std::vector<pvalue> missing;
    for (int32_t at = -1; at <= int32_t(warp_vertex_count) + reach; at++) {
      auto position = builder.CreateAdd(group_start, builder.getInt32(at));
      auto in_draw = builder.CreateICmpULT(position, vertex_count);
      missing.push_back(builder.CreateOr(builder.CreateNot(in_draw), index_at(builder.CreateSelect(in_draw, position, zero)).second));
    }
    auto missing_at = [&](int32_t at) { return missing[at + 1]; };
    pvalue offset = strip_offset, found = zero, lead = zero, odd = zero;
    pvalue first = builder.getFalse(), last = builder.getFalse(), valid = builder.getFalse();
    for (int32_t at = 0; at < int32_t(warp_vertex_count); at++) {
      pvalue whole = builder.CreateICmpEQ(builder.CreateAnd(offset, one), zero);
      for (int32_t v = 0; v < reach; v++)
        whole = builder.CreateAnd(whole, builder.CreateNot(missing_at(at + v)));
      auto is_mine = builder.CreateAnd(whole, builder.CreateICmpEQ(found, mine));
      lead = builder.CreateSelect(is_mine, builder.getInt32(at), lead);
      odd = builder.CreateSelect(is_mine, builder.CreateAnd(builder.CreateLShr(offset, 1), one), odd);
      first = builder.CreateSelect(is_mine, missing_at(at - 1), first);
      // the next triangle leads two indices on and needs both vertices past this one's
      last = builder.CreateSelect(is_mine, builder.CreateOr(missing_at(at + reach), missing_at(at + reach + 1)), last);
      valid = builder.CreateOr(valid, is_mine);
      found = builder.CreateAdd(found, builder.CreateZExt(whole, types._int));
      offset = builder.CreateSelect(missing_at(at), zero, builder.CreateAdd(offset, one));
    }
    auto is_odd = builder.CreateICmpNE(odd, zero);
    auto pick = [&](int32_t even, int32_t odd_) { return builder.CreateSelect(is_odd, builder.getInt32(odd_), builder.getInt32(even)); };
    pvalue ahead[] = {
        zero,
        builder.CreateSelect(first, one, pick(-2, 3)),
        pick(2, 4),
        builder.CreateSelect(last, builder.getInt32(5), builder.getInt32(6)),
        pick(4, 2),
        pick(3, -2),
    };
    pvalue relative = ahead[0];
    for (uint32_t k = 1; k < std::size(ahead); k++)
      relative = builder.CreateSelect(builder.CreateICmpEQ(corner, builder.getInt32(k)), ahead[k], relative);
    auto position = builder.CreateAdd(builder.CreateAdd(group_start, lead), relative);
    resource_map.vertex_id = index_at(builder.CreateSelect(valid, position, zero)).first;
    builder.CreateCondBr(valid, active, will_dispatch);
  } else {
    auto global_index_id = builder.CreateAdd(group_start, warp_vertex_id);
    auto index_check = llvm::BasicBlock::Create(context, "index_check", function);
    builder.CreateCondBr(
        builder.CreateICmp(llvm::CmpInst::ICMP_ULT, global_index_id, vertex_count), index_check, will_dispatch
    );
    builder.SetInsertPoint(index_check);
    auto [index, cut] = index_at(global_index_id);
    resource_map.vertex_id = index;
    builder.CreateCondBr(cut, will_dispatch, active);
  }

  builder.SetInsertPoint(active);

  air.CreateAtomicRMW(
      llvm::AtomicRMWInst::BinOp::Or, valid_vertex_mask, builder.CreateShl(builder.getInt32(1), warp_vertex_id)
  );

  resource_map.base_vertex_id = is_indexed_draw ? shifted(3) : builder.CreateExtractValue(draw_arguments, 2);
  resource_map.instance_id = instance_id;
  resource_map.vertex_id_with_base = builder.CreateAdd(resource_map.vertex_id, resource_map.base_vertex_id);
  resource_map.base_instance_id = is_indexed_draw ? shifted(4) : builder.CreateExtractValue(draw_arguments, 3);
  resource_map.instance_id_with_base = builder.CreateAdd(resource_map.instance_id, resource_map.base_instance_id);

  if (auto err = prologue.build(ctx).takeError()) {
    return err;
  }

  // the counting pass of a stream output draw runs the shader again: only the writing pass leaves its writes
  air.DiscardWrites = so && so->counting;
  auto real_entry = convert_basicblocks(pShaderInternal->entry(), ctx, epilogue_bb);
  air.DiscardWrites = false;
  if (auto err = real_entry.takeError()) {
    return err;
  }
  builder.CreateBr(real_entry.get());

  builder.SetInsertPoint(epilogue_bb);
  auto epilogue_result = epilogue.build(ctx);
  if (auto err = epilogue_result.takeError()) {
    return err;
  }

  builder.CreateBr(will_dispatch);

  builder.SetInsertPoint(will_dispatch);

  air.CreateBarrier(llvm::air::MemFlags::Threadgroup);

  // the primitives each stream had before this object threadgroup: the group's threads sum the earlier groups' totals
  // in turn, then over the SIMD group
  pvalue so_before[4] = {};
  if (so_base) {
    auto targets_type = stream_output_targets_type(context, types);
    auto targets = function->getArg(so_idx);
    auto field = [&](uint32_t f) {
      return builder.CreateLoad(targets_type->getElementType(f), builder.CreateStructGEP(targets_type, targets, f));
    };
    auto scratch = builder.CreateIntToPtr(field(3), types._int->getPointerTo((uint32_t)air::AddressSpace::device));
    auto group = builder.CreateAdd(builder.CreateMul(instance_id, field(4)), warp_id);
    auto head = builder.GetInsertBlock();
    auto loop = llvm::BasicBlock::Create(context, "so_before", function, dispatch);
    auto done = llvm::BasicBlock::Create(context, "so_summed", function, dispatch);
    builder.CreateCondBr(builder.CreateICmpULT(warp_vertex_id, group), loop, done);
    builder.SetInsertPoint(loop);
    auto g = builder.CreatePHI(types._int, 2);
    g->addIncoming(warp_vertex_id, head);
    llvm::PHINode *sums[4];
    pvalue next_sums[4];
    for (uint32_t s = 0; s < 4; s++) {
      sums[s] = builder.CreatePHI(types._long, 2);
      sums[s]->addIncoming(builder.getInt64(0), head);
    }
    for (uint32_t s = 0; s < 4; s++) {
      auto sum = sums[s];
      next_sums[s] = builder.CreateAdd(
          sum, builder.CreateZExt(
                   builder.CreateLoad(
                       types._int, builder.CreateGEP(
                                       types._int, scratch,
                                       {builder.CreateAdd(builder.getInt32(kStreamOutputTotals), builder.CreateAdd(builder.CreateMul(g, builder.getInt32(4)), builder.getInt32(s)))}
                                   )
                   ),
                   types._long
               )
      );
    }
    auto next_g = builder.CreateAdd(g, builder.getInt32(threads));
    g->addIncoming(next_g, loop);
    for (uint32_t s = 0; s < 4; s++)
      sums[s]->addIncoming(next_sums[s], loop);
    builder.CreateCondBr(builder.CreateICmpULT(next_g, group), loop, done);
    builder.SetInsertPoint(done);
    llvm::PHINode *partials[4];
    for (uint32_t s = 0; s < 4; s++) {
      partials[s] = builder.CreatePHI(types._long, 2);
      partials[s]->addIncoming(builder.getInt64(0), head);
      partials[s]->addIncoming(next_sums[s], loop);
    }
    // Metal sums 32-bit values over a SIMD group: pieces small enough that every thread's fit one sum
    uint32_t bits = 32 - std::bit_width(threads - 1);
    for (uint32_t s = 0; s < 4; s++) {
      so_before[s] = builder.getInt64(0);
      for (uint32_t shift = 0; shift < 64; shift += bits) {
        auto piece = builder.CreateTrunc(builder.CreateAnd(builder.CreateLShr(partials[s], shift), (1ull << bits) - 1), types._int);
        so_before[s] = builder.CreateAdd(
            so_before[s], builder.CreateShl(builder.CreateZExt(simd("air.simd_sum.u.i32", piece), types._long), shift)
        );
      }
    }
  }

  // TODO: dispatch phase

  /*
   * calculate valid primitive mask
   * calculate primitive indices
   * pass instance id
   * pass primitive_id_base
   */

  pvalue valid_primitive_mask = builder.getInt32(0);
  auto valid_vertex_result = builder.CreateLoad(types._int, valid_vertex_mask);

  for (unsigned primitive_id = 0; primitive_id < warp_primitive_count; primitive_id++) {
    pvalue primitive_valid_result = builder.getInt1(1);
    uint32_t primitive_vertices_mask = 0;
    // TODO: check primitive is valid
    if (strip && !assembled) {
      switch (input_primitive) {
      case microsoft::D3D10_SB_PRIMITIVE_TRIANGLE: {
        primitive_vertices_mask = 0b111 << primitive_id;
        break;
      }
      case microsoft::D3D10_SB_PRIMITIVE_LINE: {
        primitive_vertices_mask = 0b11 << primitive_id;
        break;
      }
      case microsoft::D3D10_SB_PRIMITIVE_LINE_ADJ: {
        primitive_vertices_mask = 0b1111 << primitive_id;
        break;
      }
      default:
        return llvm::make_error<UnsupportedFeature>(std::format(
            "unhandled geometry shader input primitive strip: {}", (uint32_t)input_primitive
        ));
      }
    } else {
      // primitive is valid if all vertex is valid
      for (unsigned vid_in_prim = 0; vid_in_prim < vertex_per_primitive; vid_in_prim++) {
        primitive_vertices_mask |= (1 << (vid_in_prim + vertex_per_primitive * primitive_id));
      }
    }
    primitive_valid_result = builder.CreateICmpEQ(
        builder.CreateAnd(builder.getInt32(primitive_vertices_mask), valid_vertex_result),
        builder.getInt32(primitive_vertices_mask)
    );

    valid_primitive_mask = builder.CreateOr(
        valid_primitive_mask,
        builder.CreateShl(builder.CreateZExt(primitive_valid_result, ctx.types._int), builder.getInt32(primitive_id))
    );
  }

  // without a geometry shader, each thread streams out a valid primitive of the group: its vertices from the payload,
  // after the valid ones before it (a strip's from its last cut, odd triangles as (n, n + 2, n + 1))
  if (so && !pGeometryStage) {
    if (auto err = stream_out_primitives(builder, air, types, so, function->getArg(so_idx), input_primitive, is_strip,
            payload_output_ptr, max_output_register, warp_vertex_id, warp_primitive_count, valid_primitive_mask,
            builder.CreateAdd(builder.CreateMul(instance_id, builder.CreateLoad(types._int, builder.CreateStructGEP(
                stream_output_targets_type(context, types), function->getArg(so_idx), 4))), warp_id),
            so_before[0], strip_offset))
      return err;
  }

  builder.CreateCondBr(
      builder.CreateICmp(llvm::CmpInst::ICMP_EQ, warp_vertex_id, builder.getInt32(0)), dispatch, return_
  );
  builder.SetInsertPoint(dispatch);
  for (uint32_t s = 0; so_base && s < 4; s++)
    builder.CreateStore(
        so_before[s], builder.CreateBitCast(
                          builder.CreateConstInBoundsGEP1_32(types._int, payload, kPayloadStreamOutputBase + 2 * s),
                          types._long->getPointerTo(payload->getType()->getPointerAddressSpace())
                      )
    );

  builder.CreateStore(instance_id, builder.CreateConstInBoundsGEP1_32(types._int, payload, 0));

  builder.CreateStore(valid_primitive_mask, builder.CreateConstInBoundsGEP1_32(types._int, payload, 1));

  builder.CreateStore(warp_id, builder.CreateConstInBoundsGEP1_32(types._int, payload, 2));

  builder.CreateStore(primitives_before, builder.CreateConstInBoundsGEP1_32(types._int, payload, kPayloadPrimitivesBefore));
  builder.CreateStore(strip_offset, builder.CreateConstInBoundsGEP1_32(types._int, payload, kPayloadStripOffset));

  air.CreateSetMeshProperties(air.getInt3(gs_instances ? warp_primitive_count : 0, gs_instances, 1));

  builder.CreateBr(return_);

  builder.SetInsertPoint(return_);

  builder.CreateRetVoid();

  module.getOrInsertNamedMetadata("air.object")->addOperand(function_metadata);
  return llvm::Error::success();
};

} // namespace dxmt::dxbc

template <> struct environment_cast<::dxmt::dxbc::context, ::dxmt::air::AIRBuilderContext> {
  ::dxmt::air::AIRBuilderContext
  cast(const ::dxmt::dxbc::context &src) {
    return {src.llvm, src.module, src.builder, src.types, src.air};
  };
};
