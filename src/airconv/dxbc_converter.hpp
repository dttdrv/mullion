#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "DXBCParser/DXBCUtils.h"
#include "air_operations.hpp"
#include "air_signature.hpp"
#include "dxbc_constants.hpp"
#include "dxbc_instructions.hpp"
#include "nt/air_builder.hpp"
#include "nt/dxbc_binding_map.hpp"
#include "shader_common.hpp"

#include "airconv_public.h"

namespace dxmt::dxbc {

struct ResourceRange {
  uint32_t range_id;
  uint32_t lower_bound;
  uint32_t size;
  uint32_t space;
};

// the register space of a range standing for a whole descriptor heap, indexed by SM 6.6 createHandleFromHeap. D3D12
// reserves spaces from 0xfffffff0 for the system, so no application range uses it
constexpr uint32_t kDescriptorHeapSpace = ~0u;

struct ShaderResourceViewInfo {
  ResourceRange range;
  shader::common::ScalerDataType scaler_type;
  shader::common::ResourceType resource_type;
  bool read = false;
  bool sampled = false;
  bool compared = false; // therefore we use depth texture!

  uint32_t structure_stride = 0;
  uint32_t arg_index;
  uint32_t arg_metadata_index;
};
struct UnorderedAccessViewInfo {
  ResourceRange range;
  shader::common::ScalerDataType scaler_type;
  shader::common::ResourceType resource_type;
  bool read = false;
  bool written = false;
  bool global_coherent = false;
  bool rasterizer_order = false;
  bool with_counter = false;

  uint32_t structure_stride = 0;
  uint32_t arg_index;
  uint32_t arg_metadata_index;
  uint32_t arg_counter_index;
};
struct ConstantBufferInfo {
  ResourceRange range;
  uint32_t size_in_vec4;
  uint32_t arg_index;
  uint32_t arg_metadata_index;
};
struct SamplerInfo {
  ResourceRange range;
  uint32_t arg_index;
  uint32_t arg_cube_index;
  uint32_t arg_metadata_index;
};

struct ThreadgroupBufferInfo {
  uint32_t size_in_uint;
  uint32_t size;
  uint32_t stride;
  bool structured;
};

struct PhaseInfo {
  uint32_t tempRegisterCount = 0;
  std::map<
    uint32_t, std::pair<uint32_t /* count */, uint32_t /* mask */>>
    indexableTempRegisterCounts;
};

class ShaderInfo {
public:
  std::vector<std::array<uint32_t, 4>> immConstantBufferData;
  std::map<uint32_t, ShaderResourceViewInfo> srvMap;
  std::map<uint32_t, UnorderedAccessViewInfo> uavMap;
  std::map<uint32_t, ConstantBufferInfo> cbufferMap;
  std::map<uint32_t, SamplerInfo> samplerMap;
  std::map<uint32_t, ThreadgroupBufferInfo> tgsmMap;
  uint32_t tempRegisterCount = 0;
  std::map<
    uint32_t, std::pair<uint32_t /* count */, uint32_t /* mask */>>
    indexableTempRegisterCounts;
  air::ArgumentBufferBuilder binding_table_cbuffer;
  air::ArgumentBufferBuilder binding_table;
  bool skipOptimization = false;
  bool refactoringAllowed = false;
  bool use_cmp_exch = false;
  bool no_control_point_phase_passthrough = false;
  bool output_control_point_read = false;
  bool use_msad = false;
  bool use_samplepos = false;
  // a compute shader whose quads (SM 6.6) are 2x2 blocks of its thread IDs, not four consecutive threads
  bool quads_2d = false;
  // the wave size a shader requires ([WaveSize], SM 6.6), or 0
  uint32_t wave_size = 0;
  std::vector<PhaseInfo> phases;
  uint32_t pull_mode_reg_mask = 0;
};

Instruction readInstruction(
  const microsoft::D3D10ShaderBinary::CInstruction &Inst,
  ShaderInfo &shader_info, uint32_t phase
);

using pvalue = dxmt::air::pvalue;
using epvalue = llvm::Expected<pvalue>;
using dxbc::Swizzle;
using dxbc::swizzle_identity;

struct context;
using IRValue = ReaderIO<context, pvalue>;
using IREffect = ReaderIO<context, std::monostate>;
using IndexedIRValue = std::function<IRValue(pvalue)>;

struct register_file {
  llvm::Value *ptr_int4 = nullptr;
  llvm::Value *ptr_float4 = nullptr;
};

struct indexable_register_file {
  llvm::Value *ptr_int_vec = nullptr;
  llvm::Value *ptr_float_vec = nullptr;
  uint32_t vec_size = 0;
};

struct phase_temp {
  register_file temp{};
  std::unordered_map<uint32_t, indexable_register_file> indexable_temp_map{};
};

struct sampler_descriptor {
  IndexedIRValue handle;
  IndexedIRValue handle_cube;
  IndexedIRValue bias;
};

struct texture_descriptor {
  air::MSLTexture texture_info;
  IndexedIRValue resource_id;
  IndexedIRValue metadata;
  bool global_coherent;
};

struct buffer_descriptor {
  uint32_t structure_stride;
  IndexedIRValue resource_id;
  IndexedIRValue metadata;
  bool global_coherent;
};

// a register the pixel shader interpolates itself: the function's argument for each component it takes of it
struct interpolant_descriptor {
  std::array<uint32_t, 4> component{~0u, ~0u, ~0u, ~0u};
  bool perspective = false;
};

struct io_binding_map {
  llvm::GlobalVariable *icb = nullptr;
  llvm::Value *icb_float = nullptr;
  std::unordered_map<uint32_t, std::pair<uint32_t, llvm::GlobalVariable *>>
    tgsm_map{};
  std::unordered_map<uint32_t, interpolant_descriptor> interpolant_map{};

  register_file input{};
  register_file output{};
  register_file temp{};
  std::unordered_map<uint32_t, indexable_register_file> indexable_temp_map{};
  std::vector<phase_temp> phases;
  register_file patch_constant_output{};
  uint32_t input_element_count = 0;
  uint32_t output_element_count = 0;

  // special registers (input)
  // a ray tracing shader's context (ray_context_type) and the arguments that stand for its DXIL function's
  llvm::Value *ray_context = nullptr;
  llvm::Value *ray_arguments[2] = {};

  llvm::Value *thread_id_arg = nullptr;
  llvm::Value *thread_group_id_arg = nullptr;
  llvm::Value *thread_id_in_group_arg = nullptr;
  llvm::Value *thread_id_in_group_flat_arg = nullptr;
  llvm::Value *coverage_mask_arg = nullptr;

  llvm::Value *domain = nullptr;
  llvm::Value *patch_id = nullptr;

  llvm::Value *thread_id_in_patch = nullptr;

  llvm::Value *gs_instance_id = nullptr;

  // special registers (output)
  llvm::AllocaInst *depth_output_reg = nullptr;
  llvm::AllocaInst *stencil_ref_reg = nullptr;
  llvm::AllocaInst *coverage_mask_reg = nullptr;

  llvm::AllocaInst *cmp_exch_temp = nullptr;

  // special buffers for tessellation
  llvm::Type *hull_cp_passthrough_type = nullptr;
  llvm::Value *hull_cp_passthrough_src = nullptr;
  llvm::Value *hull_cp_passthrough_dst = nullptr;
  llvm::Value *tess_factor_buffer;    // half*

  // temp for fast look-up
  llvm::Value *vertex_id = nullptr;
  llvm::Value *vertex_id_with_base = nullptr;
  llvm::Value *instance_id = nullptr;
  llvm::Value *instance_id_with_base = nullptr;
  llvm::Value *base_vertex_id = nullptr;
  llvm::Value *base_instance_id = nullptr;
  llvm::Value *vertex_buffer_table = nullptr;

  // a mesh shader's outputs, per DXIL element (per vertex, then per primitive): the clip or cull distance its first
  // component is, and for each of its components, row by row, the Metal mesh data the pixel shader takes it as (~0u
  // when it takes none). then the payload it reads, or an amplification shader's
  std::vector<uint32_t> mesh_data[2];
  std::vector<std::vector<uint32_t>> mesh_varyings[2];
  llvm::Value *payload = nullptr;

  // geometry shader ops
  llvm::Value *mesh = nullptr;
  // a geometry shader's emit and cut on a stream
  std::function<IREffect(uint32_t)> call_emit;
  std::function<IREffect(uint32_t)> call_cut;
};

struct context {
  llvm::IRBuilder<> &builder;
  llvm::air::AIRBuilder &air;
  BindingMap &binding;
  llvm::LLVMContext &llvm;
  llvm::Module &module;
  llvm::Function *function;
  io_binding_map &resource;
  air::AirType &types; // hmmm
  uint32_t pso_sample_mask;
  microsoft::D3D10_SB_TOKENIZED_PROGRAM_TYPE shader_type;
  SM50_SHADER_METAL_VERSION metal_version;
  uint32_t simd_width = 0; // the device's SIMD-group width, which vertex functions have no input for; 0 when unknown
};

template <typename S> IRValue make_irvalue(S &&fs) {
  return IRValue(std::forward<S>(fs));
}

template <typename S> IRValue make_irvalue_bind(S &&fs) {
  return IRValue([fs = std::forward<S>(fs)](auto ctx) {
    return fs(ctx).build(ctx);
  });
}

template <typename S> IREffect make_effect(S &&fs) {
  return IREffect(std::forward<S>(fs));
}

template <typename S> IREffect make_effect_bind(S &&fs) {
  return IREffect([fs = std::forward<S>(fs)](auto ctx) mutable {
    return fs(ctx).build(ctx);
  });
}

IREffect store_at_vec4_array_masked(
  llvm::Value *array, pvalue index, pvalue maybe_vec4, uint32_t mask
);

IREffect init_input_reg(
  uint32_t with_fnarg_at, uint32_t to_reg, uint32_t mask,
  bool fix_w_component = false, uint32_t sample_index_at = ~0u
);

IREffect init_input_reg_with_interpolation(
  uint32_t with_fnarg_at, uint32_t to_reg, uint32_t mask,
  air::Interpolation interpolation, uint32_t sampleidx_at
);

std::function<IRValue(pvalue)>
pop_output_reg(uint32_t from_reg, uint32_t mask, uint32_t to_element);

std::function<IRValue(pvalue)>
pop_output_reg_fix_unorm(uint32_t from_reg, uint32_t mask, uint32_t to_element);

std::function<IRValue(pvalue)>
pop_output_reg_sanitize_pos(uint32_t from_reg, uint32_t mask, uint32_t to_element);

IREffect pull_vertex_input(
  air::FunctionSignatureBuilder &func_signature, uint32_t to_reg, uint32_t mask,
  SM50_IA_INPUT_ELEMENT element_info, uint32_t slot_mask
);

IREffect pop_mesh_output_render_target_array_index(uint32_t from_reg, uint32_t mask, pvalue primitive_id);
IREffect pop_mesh_output_viewport_array_index(uint32_t from_reg, uint32_t mask, pvalue primitive_id);
IREffect pop_mesh_output_primitive_id(uint32_t from_reg, uint32_t mask, pvalue primitive_id);
IREffect pop_mesh_output_position(uint32_t from_reg, uint32_t mask, pvalue vertex_id);
IREffect
pop_mesh_output_vertex_data(uint32_t from_reg, uint32_t mask, uint32_t idx, pvalue vertex_id, air::MSLScalerOrVectorType desired_type);

llvm::Expected<llvm::BasicBlock *> convert_basicblocks(
  BasicBlock *entry, context &ctx, llvm::BasicBlock *return_bb
);

constexpr air::MSLScalerOrVectorType to_msl_type(RegisterComponentType type) {
  switch (type) {
  default:
  case RegisterComponentType::Unknown: {
    assert(0 && "unknown component type");
    break;
  }
  case RegisterComponentType::Uint:
    return air::msl_uint4;
  case RegisterComponentType::Int:
    return air::msl_int4;
  case RegisterComponentType::Float:
    return air::msl_float4;
  }
  return air::msl_float4;
}

// a varying is one component of a register. Direct3D links stages by register and component, whatever elements each
// declares there, and interpolates a register one way (D3D11.3 16.4); Metal links by name and exact type, and its limit
// counts components. so a stage writes whole each register it has varyings in, and the pixel shader takes the
// components it declares. integers of either sign are the same bits
inline std::string
varying_name(uint32_t reg, uint32_t component) {
  return "reg" + std::to_string(reg) + "_" + std::to_string(component);
}
constexpr air::MSLScalerOrVectorType
varying_type(RegisterComponentType type) {
  return type == RegisterComponentType::Float ? air::MSLScalerOrVectorType(air::msl_float) : air::msl_uint;
}

struct ScalarInfo {
  uint8_t component : 2;
  uint8_t reg : 6;
};

struct PatchConstantScalarInfo {
  uint8_t component : 2;
  uint8_t reg : 6;
  int8_t tess_factor_index;
};

struct SignatureContext {
  IREffect &prologue;
  IRValue &epilogue;
  air::FunctionSignatureBuilder &func_signature;
  io_binding_map &resource;
  SM50_SHADER_IA_INPUT_LAYOUT_DATA *ia_layout;
  bool dual_source_blending;
  bool disable_depth_output;
  bool skip_vertex_output;
  uint32_t pull_mode_reg_mask;
  uint32_t unorm_output_reg_mask;
  // it is considered optional (as a hint)
  air::MTLPixelFormat pixel_formats[8];

  SignatureContext(
      IREffect &prologue, IRValue &epilogue, air::FunctionSignatureBuilder &func_signature, io_binding_map &resource
  ) :
      prologue(prologue),
      epilogue(epilogue),
      func_signature(func_signature),
      resource(resource),
      ia_layout(nullptr),
      dual_source_blending(false),
      disable_depth_output(false),
      skip_vertex_output(false),
      pull_mode_reg_mask(0),
      unorm_output_reg_mask(0) {
    memset(pixel_formats, 0, sizeof(pixel_formats));
  };
};

RegisterComponentType component_type_from_pixel_format(air::MTLPixelFormat format);

struct MeshOutputContext {
  llvm::Value *vertex_id;
  llvm::Value *primitive_id;
};

class Signature {

public:
  Signature(const microsoft::D3D11_SIGNATURE_PARAMETER &parameter) :
      semantic_name_(parameter.SemanticName),
      semantic_index_(parameter.SemanticIndex),
      stream_(parameter.Stream),
      mask_(parameter.Mask),
      register_(parameter.Register),
      system_value_(parameter.SystemValue),
      component_type_(widened((RegisterComponentType)parameter.ComponentType)) {
    std::transform(semantic_name_.begin(), semantic_name_.end(), semantic_name_.begin(), [](auto c) {
      return std::tolower(c);
    });
  }

  Signature()
      : semantic_name_("INVALID"), semantic_index_(0), stream_(0), mask_(0),
        register_(0), system_value_(microsoft::D3D10_SB_NAME_UNDEFINED),
        component_type_(RegisterComponentType::Unknown) {}

  std::string_view semanticName() const { return semantic_name_; }

  uint32_t semanticIndex() const { return semantic_index_; }

  std::string fullSemanticString() const {
    return semantic_name_ + std::to_string(semantic_index_);
  }

  std::string consistentAttributeName() const {
    return "reg" + std::to_string(register_) + "_" + std::to_string(std::countr_zero(mask_ & 0xFu));
  }

  uint32_t stream() const { return stream_; }

  uint32_t mask() const { return mask_; }

  uint32_t reg() const { return register_; }

  bool isSystemValue() const {
    return system_value_ != microsoft::D3D10_SB_NAME_UNDEFINED;
  }

  microsoft::D3D10_SB_NAME systemValue() const { return system_value_; }

  RegisterComponentType componentType() const {
    return (RegisterComponentType)component_type_;
  }

private:
  std::string semantic_name_;
  uint8_t semantic_index_;
  uint8_t stream_;
  uint8_t mask_;
  uint8_t register_;
  microsoft::D3D10_SB_NAME system_value_;
  RegisterComponentType component_type_;
};

class SM50ShaderInternal {
public:
  dxmt::dxbc::ShaderInfo shader_info;
  dxmt::air::FunctionSignatureBuilder func_signature;
  std::vector<Signature> output_signature;
  std::vector<std::unique_ptr<BasicBlock>> bbs;
  std::vector<std::function<void(SignatureContext &)>> signature_handlers;
  microsoft::D3D10_SB_TOKENIZED_PROGRAM_TYPE shader_type;
  /* for domain shader, it refers to patch constant input count */
  uint32_t max_input_register = 0;
  uint32_t max_output_register = 0;
  uint32_t pso_valid_output_reg_mask = 0;
  uint32_t max_patch_constant_output_register = 0;
  std::vector<MTL_SM50_SHADER_ARGUMENT> args_reflection_cbuffer;
  std::vector<MTL_SM50_SHADER_ARGUMENT> args_reflection;
  uint32_t threadgroup_size[3] = {0};
  // a compute shader with a loop that reads globally coherent memory: its threadgroups wait for one another, and its
  // dispatch comes in parts (MTL_SHADER_REFLECTION::GroupsWorkTogether), each part's groups numbered from the
  // stage-in grid's origin
  bool groups_work_together = false;
  bool reads_dispatch_position = false;
  uint32_t input_control_point_count = ~0u;
  uint32_t output_control_point_count = ~0u;
  microsoft::D3D11_SB_TESSELLATOR_PARTITIONING tessellation_partition = {};
  float max_tesselation_factor = 64.0f;
  microsoft::D3D11_SB_TESSELLATOR_OUTPUT_PRIMITIVE tessellator_output_primitive = {};
  microsoft::D3D11_SB_TESSELLATOR_DOMAIN tessellation_domain = {};
  std::vector<PatchConstantScalarInfo> patch_constant_scalars;
  uint32_t hull_maximum_threads_per_patch = 0;
  std::vector<ScalarInfo> clip_distance_scalars;
  std::vector<ScalarInfo> cull_distance_scalars;
  microsoft::D3D10_SB_PRIMITIVE gs_input_primitive = {};
  std::vector<std::function<IREffect(MeshOutputContext &)>> mesh_output_handlers;
  uint32_t num_mesh_vertex_data = 0;
  // the output registers already declared as varyings
  uint32_t varying_registers = 0;
  // the type of each component of an output register: its element's there, or `declared` where it has none
  std::array<RegisterComponentType, 4>
  register_types(uint32_t reg, RegisterComponentType declared) const {
    std::array<RegisterComponentType, 4> types;
    types.fill(declared);
    for (auto &element : output_signature)
      for (unsigned c = 0; c < 4; c++)
        if (element.reg() == reg && (element.mask() >> c & 1))
          types[c] = element.componentType();
    return types;
  }
  microsoft::D3D10_SB_PRIMITIVE_TOPOLOGY gs_output_topology = {};
  uint32_t gs_max_vertex_output = 0;
  uint32_t gs_instance_count = 1;
  uint32_t ps_has_coverage_output = 0;
  std::string dxil; // bitcode of a DXIL shader, empty for DXBC
  // a DXIL library's ray tracing shaders, by their functions' names
  struct RayShader {
    std::string name;
    SM50_RAY_SHADER_INFO info;
  };
  std::vector<RayShader> ray_shaders;
  // a mesh shader's outputs, by DXIL element: [0] per vertex, [1] per primitive. `name` and `index` are the semantic
  struct MeshOutput {
    uint32_t semantic, row, rows, col, cols;
    RegisterComponentType type;
    std::string name;
    uint32_t index;
  };
  std::vector<MeshOutput> mesh_outputs[2];
  // its vertex and primitive limits, whether it outputs lines, and the payload's size (an amplification shader's too)
  uint32_t mesh_max_vertices = 0, mesh_max_primitives = 0, payload_size = 0;
  bool mesh_lines = false;

  BasicBlock *entry() const {
    return bbs.front().get();
  }
};

void declare_dispatch_origin(SM50ShaderInternal *sm50_shader);

void handle_signature(
  microsoft::CSignatureParser &inputParser,
  microsoft::CSignatureParser5 &outputParser,
  microsoft::D3D10ShaderBinary::CInstruction &Inst, SM50ShaderInternal *sm50_shader,
  uint32_t phase
);

llvm::Error convert_dxil_mesh_stage(
  SM50ShaderInternal *pShaderInternal, const char *name, llvm::LLVMContext &context, llvm::Module &module,
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs
);

llvm::Error read_dxil(
  const void *part, SM50ShaderInternal *shader, microsoft::CSignatureParser &inputParser,
  microsoft::CSignatureParser5 &outputParser, microsoft::CSignatureParser &patchParser
);

// `linked`: the function's module, already read, with whatever was linked into it, and that module's resources
llvm::Expected<llvm::BasicBlock *>
convert_dxil(
    SM50ShaderInternal *shader, context &ctx, const std::string &function, llvm::BasicBlock *epilogue,
    std::unique_ptr<llvm::Module> linked = nullptr, ShaderInfo *linked_info = nullptr
);

std::vector<std::unique_ptr<BasicBlock>> read_control_flow(
    microsoft::D3D10ShaderBinary::CShaderCodeParser &Parser, SM50ShaderInternal *sm50_shader,
    microsoft::CSignatureParser &inputParser, microsoft::CSignatureParser5 &outputParser
);

uint32_t next_pow2(uint32_t x);

size_t estimate_payload_size(SM50ShaderInternal *pHullStage, float factor, uint32_t patch_per_group);


// the tessellation factor a pipeline's stages are built for, and the most segments it cuts an edge into
std::pair<float, uint32_t>
get_final_maxtessfactor(SM50ShaderInternal *pHullStage, SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs);

enum class TessellatorOutputPrimitive;
TessellatorOutputPrimitive get_output_primitive(SM50ShaderInternal *pHullStage);

// the mesh threadgroups a workload takes: one, or with a geometry shader after the tessellator (the arguments' ) one
// for each primitive the workload may have and each geometry instance
std::pair<uint32_t, uint32_t>
mesh_threadgroups_per_workload(SM50ShaderInternal *pHullStage, SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs, uint32_t factor_int);

llvm::StructType *tessellation_payload_type(
    SM50ShaderInternal *pHullStage, uint32_t factor_int, air::AirType &types, llvm::LLVMContext &context
);

llvm::Value *setup_domain_stage(
    SM50ShaderInternal *pDomainStage, SM50ShaderInternal *pHullStage, llvm::StructType *payload_type,
    llvm::Value *payload_argument, llvm::Value *workload_index, struct context &ctx
);

llvm::Value *domain_location(llvm::Value *location, llvm::IRBuilder<> &builder, llvm::air::AIRBuilder &air);

std::unique_ptr<BindingMap> setup_binding_table2(
    const ShaderInfo *shader_info, air::FunctionSignatureBuilder &func_signature, llvm::Module &module,
    uint32_t argbuffer_constant_slot = SM50_BINDING_INDEX_CONSTANT_BUFFER, uint32_t argbuffer_slot = SM50_BINDING_INDEX_ARGUMENT_TABLE
);

std::unique_ptr<BindingMap> setup_binding_rootsig(
    const ShaderInfo *shader_info, air::FunctionSignatureBuilder &func_signature, llvm::Module &module,
    microsoft::D3D10_SB_TOKENIZED_PROGRAM_TYPE shader_type, const void *bytecode, size_t bytecode_length,
    uint32_t root_sig_slot = SM50_BINDING_INDEX_ROOT_ARGUMENTS,
    uint32_t static_sampler_slot = SM50_BINDING_INDEX_STATIC_SAMPLERS,
    // a local root signature, whose arguments are a shader record's (bind_rootsig_arguments)
    bool local = false
);

// a ray tracing shader finds a root signature's arguments through its context, not its function's arguments:
// `arguments` points to them, the global ones or a shader record's local ones, in their address space
void bind_rootsig_arguments(
    BindingMap &map, llvm::IRBuilder<> &builder, llvm::Value *arguments, llvm::Value *static_samplers
);

// a resource is in `first` or else in `second`; either may be null
std::unique_ptr<BindingMap> combine_bindings(std::unique_ptr<BindingMap> first, std::unique_ptr<BindingMap> second);

// DXIL's shader kind of a library, which D3D10_SB_TOKENIZED_PROGRAM_TYPE has no name for
constexpr auto kShaderLibrary = (microsoft::D3D10_SB_TOKENIZED_PROGRAM_TYPE)6;

llvm::Error convert_dxil_ray_shader(
    SM50ShaderInternal *pShaderInternal, const char *name, llvm::LLVMContext &context, llvm::Module &module,
    SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs
);

void setup_metal_version(llvm::Module &module, SM50_SHADER_METAL_VERSION metal_version);

// the index at `position` of a D3D12 index buffer view ({GPU address, size, DXGI format}) of `format`, R16 or R32;
// past the view's end it is 0, as in D3D12
llvm::Value *
index_from_view(llvm::IRBuilder<> &builder, air::AirType &types, llvm::Value *view, llvm::Value *format, llvm::Value *position);

void setup_tgsm(
  const ShaderInfo *shader_info, io_binding_map &resource_map, air::AirType &types, llvm::Module &module
);

void setup_temp_register(
  const ShaderInfo *shader_info, io_binding_map &resource_map,
  air::AirType &types, llvm::Module &module, llvm::IRBuilder<> &builder
);

void setup_immediate_constant_buffer(
  const ShaderInfo *shader_info, io_binding_map &resource_map,
  air::AirType &types, llvm::Module &module, llvm::IRBuilder<> &builder
);

llvm::Error convert_null_geometry_shader(const char *name, llvm::LLVMContext &context, llvm::Module &module);
llvm::Error convert_pass_through_geometry_shader(
  const SM50ShaderInternal *pVertexStage, const char *name, llvm::LLVMContext &context, llvm::Module &module,
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs
);

// `pHullStage`: of a tessellation pipeline, whose domain shader is then the stage before
llvm::Error convert_dxbc_geometry_shader(
  SM50ShaderInternal *pShaderInternal, const char *name,
  SM50ShaderInternal *pVertexStage, llvm::LLVMContext &context,
  llvm::Module &module, SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs,
  SM50ShaderInternal *pHullStage = nullptr
);

llvm::Error convert_dxbc_vertex_for_geometry_shader(
  const SM50ShaderInternal *pShaderInternal, const char *name,
  const SM50ShaderInternal *pGeometryStage,
  llvm::LLVMContext &context, llvm::Module &module,
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs
);

llvm::Error convert_dxbc_vertex_hull_shader(
    SM50ShaderInternal *pVertexStage, SM50ShaderInternal *pHullStage, const char *name, llvm::LLVMContext &context,
    llvm::Module &module, SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs
);

llvm::Error convert_dxbc_tesselator_domain_shader(
    SM50ShaderInternal *pShaderInternal, const char *name, SM50ShaderInternal *pHullStage, llvm::LLVMContext &context,
    llvm::Module &module, SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs
);

template <SM50_SHADER_COMPILATION_ARGUMENT_TYPE data_e, typename data_t>
bool
args_get_data(const struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *data, data_t **out) {
  const SM50_SHADER_COMPILATION_ARGUMENT_DATA *arg = data;
  while (arg) {
    switch (arg->type) {
    case data_e:
      *out = (data_t *)arg;
      return true;
    default:
      break;
    }
    arg = (const SM50_SHADER_COMPILATION_ARGUMENT_DATA *)arg->next;
  }
  *out = NULL;
  return false;
}

} // namespace dxmt::dxbc
