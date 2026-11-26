// contract: a geometry pipeline's mesh threadgroup runs several of the geometry shader's invocations, a thread each,
// where they fit one Metal mesh, as Metal Shader Converter packs them: its MeshInvocations (MTL_GEOMETRY_SHADER_REFLECTION)
// is the most up to a SIMD group's 32 whose vertex slots, each invocation maxvertexcount of them, fit the mesh's 256
// vertices and 512 primitives ("Mesh shaders can output up to 256 vertices and 512 primitives", Metal Feature Set
// Tables) and whose mesh fits the 16 kilobytes the draw's budget counts a mesh threadgroup ("The total size of
// metal::mesh cannot exceed 16 kilobytes", WWDC22 10162, 11:54). the mesh function declares that many invocations'
// vertices and primitives, the object function starts a mesh threadgroup for that many of its primitives, rounded up,
// and a conservative pipeline's (SM50GeometryMeshInvocations) runs one invocation a threadgroup as before. each case is
// a gs_4_0 shader of points emitting one vertex of `registers` output registers, declaring `count` vertices at most,
// written here from the tokenized program format
#include "DXBCParser/BlobContainer.h"
#include "airconv_public.h"
#include "DXBCParser/d3d12tokenizedprogramformat.hpp"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/MemoryBuffer.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace microsoft;

// the token macros give int
using Tokens = std::initializer_list<long long>;

static void
append(std::vector<uint32_t> &program, Tokens tokens) {
  program.insert(program.end(), tokens.begin(), tokens.end());
}

static long long
operand(D3D10_SB_OPERAND_TYPE type, D3D10_SB_OPERAND_INDEX_DIMENSION dimension, long long selection) {
  return ENCODE_D3D10_SB_OPERAND_TYPE(type) | ENCODE_D3D10_SB_OPERAND_INDEX_DIMENSION(dimension) |
         ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_4_COMPONENT) | selection;
}
static const long long mask_all =
    ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(D3D10_SB_OPERAND_4_COMPONENT_MASK_MODE) |
    D3D10_SB_OPERAND_4_COMPONENT_MASK_ALL;
static const long long no_swizzle =
    ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_MODE) |
    D3D10_SB_OPERAND_4_COMPONENT_NOSWIZZLE;
static long long
opcode(D3D10_SB_OPCODE_TYPE type, uint32_t length, long long more = 0) {
  return ENCODE_D3D10_SB_OPCODE_TYPE(type) | ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(length) | more;
}

static void
finish(std::vector<uint32_t> &program) {
  append(program, {opcode(D3D10_SB_OPCODE_RET, 1)});
  program[1] = ENCODE_D3D10_SB_TOKENIZED_PROGRAM_LENGTH(program.size());
}

// writes o0 (SV_Position) to o<registers - 1>, each l(1, 2, 3, 4)
static std::vector<uint32_t>
vertex_shader(uint32_t registers) {
  std::vector<uint32_t> program{ENCODE_D3D10_SB_TOKENIZED_PROGRAM_VERSION_TOKEN(D3D10_SB_VERTEX_SHADER, 5, 0), 0};
  auto output = operand(D3D10_SB_OPERAND_TYPE_OUTPUT, D3D10_SB_OPERAND_INDEX_1D, mask_all);
  append(program, {opcode(D3D10_SB_OPCODE_DCL_OUTPUT_SIV, 4), output, 0, ENCODE_D3D10_SB_NAME(D3D10_SB_NAME_POSITION)});
  for (uint32_t reg = 1; reg < registers; reg++)
    append(program, {opcode(D3D10_SB_OPCODE_DCL_OUTPUT, 3), output, reg});
  auto immediate = ENCODE_D3D10_SB_OPERAND_TYPE(D3D10_SB_OPERAND_TYPE_IMMEDIATE32) |
                   ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_4_COMPONENT);
  for (uint32_t reg = 0; reg < registers; reg++)
    append(program, {opcode(D3D10_SB_OPCODE_MOV, 8), output, reg, immediate, 0x3f800000, 0x40000000, 0x40400000,
                     0x40800000});
  finish(program);
  return program;
}

// [maxvertexcount(count)] a point's position and `registers - 1` more registers, emitted as one vertex of `topology`
static std::vector<uint32_t>
geometry_shader(uint32_t count, uint32_t registers, D3D10_SB_PRIMITIVE_TOPOLOGY topology) {
  std::vector<uint32_t> program{ENCODE_D3D10_SB_TOKENIZED_PROGRAM_VERSION_TOKEN(D3D10_SB_GEOMETRY_SHADER, 4, 0), 0};
  append(program, {opcode(D3D10_SB_OPCODE_DCL_INPUT_SIV, 5),
                   operand(D3D10_SB_OPERAND_TYPE_INPUT, D3D10_SB_OPERAND_INDEX_2D, mask_all), 1, 0,
                   ENCODE_D3D10_SB_NAME(D3D10_SB_NAME_POSITION)});
  append(program, {opcode(D3D10_SB_OPCODE_DCL_GS_INPUT_PRIMITIVE, 1,
                          ENCODE_D3D10_SB_GS_INPUT_PRIMITIVE(D3D10_SB_PRIMITIVE_POINT))});
  append(program, {opcode(D3D10_SB_OPCODE_DCL_GS_OUTPUT_PRIMITIVE_TOPOLOGY, 1,
                          ENCODE_D3D10_SB_GS_OUTPUT_PRIMITIVE_TOPOLOGY(topology))});
  auto output = operand(D3D10_SB_OPERAND_TYPE_OUTPUT, D3D10_SB_OPERAND_INDEX_1D, mask_all);
  append(program, {opcode(D3D10_SB_OPCODE_DCL_OUTPUT_SIV, 4), output, 0, ENCODE_D3D10_SB_NAME(D3D10_SB_NAME_POSITION)});
  for (uint32_t reg = 1; reg < registers; reg++)
    append(program, {opcode(D3D10_SB_OPCODE_DCL_OUTPUT, 3), output, reg});
  append(program, {opcode(D3D10_SB_OPCODE_DCL_MAX_OUTPUT_VERTEX_COUNT, 2), count});
  for (uint32_t reg = 0; reg < registers; reg++)
    append(program, {opcode(D3D10_SB_OPCODE_MOV, 6), output, reg,
                     operand(D3D10_SB_OPERAND_TYPE_INPUT, D3D10_SB_OPERAND_INDEX_2D, no_swizzle), 0, 0});
  append(program, {opcode(D3D10_SB_OPCODE_EMIT, 1)});
  finish(program);
  return program;
}

// a vs_5_0 vertex shader writing SV_Position from its input
static std::vector<uint32_t>
vertex_shader() {
  std::vector<uint32_t> program{ENCODE_D3D10_SB_TOKENIZED_PROGRAM_VERSION_TOKEN(D3D10_SB_VERTEX_SHADER, 5, 0), 0};
  auto output = operand(D3D10_SB_OPERAND_TYPE_OUTPUT, D3D10_SB_OPERAND_INDEX_1D, mask_all);
  append(program, {opcode(D3D10_SB_OPCODE_DCL_OUTPUT_SIV, 4), output, 0, ENCODE_D3D10_SB_NAME(D3D10_SB_NAME_POSITION)});
  auto immediate = ENCODE_D3D10_SB_OPERAND_TYPE(D3D10_SB_OPERAND_TYPE_IMMEDIATE32) |
                   ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_4_COMPONENT);
  append(program, {opcode(D3D10_SB_OPCODE_MOV, 8), output, 0, immediate, 0, 0, 0, 0x3f800000});
  finish(program);
  return program;
}

// an ISGN or OSGN part: SV_Position in register 0, then TEXCOORD0 on in the next `registers - 1`
static std::vector<uint8_t>
signature(uint32_t registers) {
  struct Element {
    uint32_t name, index, system_value, component_type, reg;
    uint8_t mask, used, pad[2];
  };
  const char position[] = "SV_Position", texcoord[] = "TEXCOORD";
  uint32_t names = 8 + registers * sizeof(Element);
  std::vector<uint8_t> out(names);
  uint32_t header[2] = {registers, 8};
  memcpy(out.data(), header, sizeof(header));
  for (uint32_t reg = 0; reg < registers; reg++) {
    Element element{reg ? uint32_t(names + sizeof(position)) : names, reg ? reg - 1 : 0, reg ? 0u : 1u /* D3D_NAME_POSITION */,
                    3 /* D3D_REGISTER_COMPONENT_FLOAT32 */, reg, 0xf, 0xf, {}};
    memcpy(out.data() + 8 + reg * sizeof(Element), &element, sizeof(element));
  }
  out.insert(out.end(), position, position + sizeof(position));
  out.insert(out.end(), texcoord, texcoord + sizeof(texcoord));
  while (out.size() % 4)
    out.push_back(0);
  return out;
}

static std::vector<uint8_t>
container(const std::vector<uint8_t> &input, const std::vector<uint8_t> &output, const std::vector<uint32_t> &program) {
  struct Blob {
    DXBCFourCC fourcc;
    const void *data;
    uint32_t size;
  } blobs[] = {
      {DXBC_InputSignature, input.data(), uint32_t(input.size())},
      {DXBC_OutputSignature, output.data(), uint32_t(output.size())},
      {DXBC_GenericShaderEx, program.data(), uint32_t(program.size() * sizeof(uint32_t))},
  };
  constexpr uint32_t count = sizeof(blobs) / sizeof(blobs[0]);
  std::vector<uint8_t> out(sizeof(DXBCHeader) + count * sizeof(uint32_t));
  std::vector<uint32_t> offsets;
  for (auto &blob : blobs) {
    offsets.push_back(out.size());
    DXBCBlobHeader header{blob.fourcc, blob.size};
    out.insert(out.end(), (const uint8_t *)&header, (const uint8_t *)&header + sizeof(header));
    out.insert(out.end(), (const uint8_t *)blob.data, (const uint8_t *)blob.data + blob.size);
  }
  DXBCHeader header{};
  header.DXBCHeaderFourCC = DXBC_FOURCC_NAME;
  header.Version = {DXBC_MAJOR_VERSION, DXBC_MINOR_VERSION};
  header.ContainerSizeInBytes = out.size();
  header.BlobCount = count;
  memcpy(out.data(), &header, sizeof(header));
  memcpy(out.data() + sizeof(header), offsets.data(), count * sizeof(uint32_t));
  return out;
}

static std::unique_ptr<llvm::Module>
module_of(const std::string &library, llvm::LLVMContext &context, std::string &message) {
  auto wrapper = library.find("\xde\xc0\x17\x0b");
  uint32_t header[5];
  if (wrapper == std::string::npos || wrapper + sizeof(header) > library.size()) {
    message = "a library without a bitcode wrapper";
    return nullptr;
  }
  memcpy(header, library.data() + wrapper, sizeof(header));
  context.setOpaquePointers(false);
  auto module = llvm::parseBitcodeFile(
      llvm::MemoryBufferRef(llvm::StringRef(library.data() + wrapper + header[2], header[3]), "shader"), context
  );
  if (!module) {
    message = llvm::toString(module.takeError());
    return nullptr;
  }
  return std::move(*module);
}

// the vertices and primitives of the air.mesh_type_info somewhere in `node`, or {-1, -1}
static std::pair<long long, long long>
mesh_type(const llvm::MDNode *node, unsigned depth = 0) {
  if (!node || depth > 6)
    return {-1, -1};
  for (unsigned i = 0; i + 4 < node->getNumOperands(); i++)
    if (auto string = llvm::dyn_cast_or_null<llvm::MDString>(node->getOperand(i).get());
        string && string->getString() == "air.mesh_type_info") {
      auto vertices = llvm::mdconst::dyn_extract_or_null<llvm::ConstantInt>(node->getOperand(i + 3));
      auto primitives = llvm::mdconst::dyn_extract_or_null<llvm::ConstantInt>(node->getOperand(i + 4));
      if (vertices && primitives)
        return {vertices->getSExtValue(), primitives->getSExtValue()};
    }
  for (unsigned i = 0; i < node->getNumOperands(); i++)
    if (auto found = mesh_type(llvm::dyn_cast_or_null<llvm::MDNode>(node->getOperand(i).get()), depth + 1);
        found.first >= 0)
      return found;
  return {-1, -1};
}

// the grid width the object function gives its mesh threadgroups, a constant, or -1
static long long
mesh_grid_width(const llvm::Module &module) {
  long long width = -1;
  for (auto &function : module)
    for (auto &block : function)
      for (auto &instruction : block)
        if (auto call = llvm::dyn_cast<llvm::CallInst>(&instruction);
            call && call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "air.set_threadgroups_per_grid_mesh_properties")
          if (auto grid = llvm::dyn_cast<llvm::Constant>(call->getArgOperand(1)))
            if (auto x = llvm::dyn_cast_or_null<llvm::ConstantInt>(grid->getAggregateElement(0u)))
              width = x->getSExtValue();
  return width;
}

static bool
parse(const std::vector<uint8_t> &bytecode, sm50_shader_t &shader) {
  sm50_error_t error = nullptr;
  if (!SM50Initialize(bytecode.data(), bytecode.size(), &shader, nullptr, &error))
    return true;
  char message[512] = "";
  SM50GetErrorMessage(error, message, sizeof(message));
  printf("wrong: a shader does not parse: %s\n", message);
  SM50FreeError(error);
  return false;
}

static bool
convert(
    int (*compile)(sm50_shader_t, sm50_shader_t, SM50_SHADER_COMPILATION_ARGUMENT_DATA *, const char *, sm50_bitcode_t *,
                   sm50_error_t *),
    sm50_shader_t vs, sm50_shader_t gs, SM50_SHADER_PSO_GEOMETRY_SHADER_DATA &pso, std::string &library
) {
  sm50_bitcode_t bitcode = nullptr;
  sm50_error_t error = nullptr;
  if (compile(vs, gs, (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&pso, "main", &bitcode, &error)) {
    char message[512] = "";
    SM50GetErrorMessage(error, message, sizeof(message));
    printf("wrong: the pipeline does not convert: %s\n", message);
    SM50FreeError(error);
    return false;
  }
  SM50_COMPILED_BITCODE compiled;
  SM50GetCompiledBitcode(bitcode, &compiled);
  library.assign((const char *)compiled.Data, compiled.Size);
  SM50DestroyBitcode(bitcode);
  return true;
}

int
main() {
  unsigned wrong = 0;
  sm50_shader_t vs = nullptr;
  auto empty = signature(0);
  if (!parse(container(empty, signature(1), vertex_shader()), vs))
    return 1;
  // the vertex shader's warp: 32 points
  uint32_t warp = SM50GeometryWarp(1, false, 1).primitives;
  const struct {
    uint32_t count, registers;
    D3D10_SB_PRIMITIVE_TOPOLOGY topology;
    uint32_t invocations; // 0: fewer than the vertex limit's, the mesh's bytes deciding
  } cases[] = {
      {1, 1, D3D10_SB_PRIMITIVE_TOPOLOGY_POINTLIST, 32},  {4, 1, D3D10_SB_PRIMITIVE_TOPOLOGY_POINTLIST, 32},
      {8, 1, D3D10_SB_PRIMITIVE_TOPOLOGY_POINTLIST, 32},  {16, 1, D3D10_SB_PRIMITIVE_TOPOLOGY_POINTLIST, 16},
      {64, 1, D3D10_SB_PRIMITIVE_TOPOLOGY_POINTLIST, 4},  {256, 1, D3D10_SB_PRIMITIVE_TOPOLOGY_POINTLIST, 1},
      {4, 1, D3D10_SB_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, 32}, {3, 1, D3D10_SB_PRIMITIVE_TOPOLOGY_LINESTRIP, 32},
      {4, 32, D3D10_SB_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, 0},
  };
  for (auto &c : cases)
    for (bool conservative : {false, true}) {
      sm50_shader_t gs = nullptr;
      MTL_SHADER_REFLECTION reflection{};
      sm50_error_t error = nullptr;
      auto bytecode = container(signature(1), signature(c.registers), geometry_shader(c.count, c.registers, c.topology));
      if (SM50Initialize(bytecode.data(), bytecode.size(), &gs, &reflection, &error)) {
        printf("wrong: a geometry shader of %u vertices does not parse\n", c.count);
        SM50FreeError(error);
        wrong++;
        continue;
      }
      uint32_t reflected = reflection.GeometryShader.MeshInvocations;
      uint32_t expected = SM50GeometryMeshInvocations(reflected, conservative, false);
      bool fits = reflected >= 1 && reflected <= 32 && reflected * c.count <= 256 &&
                  (c.invocations ? reflected == c.invocations
                                 : reflected < std::min(32u, 256 / c.count) &&
                                       reflected * c.count * c.registers * sizeof(float[4]) <= 16 * 1024);
      if (!fits) {
        printf("wrong: %u vertices of %u registers: %u invocations a mesh threadgroup\n", c.count, c.registers, reflected);
        wrong++;
      }
      SM50_SHADER_PSO_GEOMETRY_SHADER_DATA pso{};
      pso.type = SM50_SHADER_PSO_GEOMETRY_SHADER;
      pso.input_primitive = D3D10_SB_PRIMITIVE_POINT;
      pso.conservative = conservative;
      std::string object, mesh, message;
      if (convert(SM50CompileGeometryPipelineVertex, vs, gs, pso, object) &&
          convert(SM50CompileGeometryPipelineGeometry, vs, gs, pso, mesh)) {
        llvm::LLVMContext object_context, mesh_context;
        auto object_module = module_of(object, object_context, message);
        auto mesh_module = module_of(mesh, mesh_context, message);
        auto functions = mesh_module ? mesh_module->getNamedMetadata("air.mesh") : nullptr;
        if (!object_module || !functions || functions->getNumOperands() != 1) {
          printf("wrong: %u vertices: %s\n", c.count, message.empty() ? "no one air.mesh function" : message.c_str());
          wrong++;
        } else {
          auto [vertices, primitives] = mesh_type(functions->getOperand(0));
          long long width = mesh_grid_width(*object_module);
          // a conservative rasterizer's triangles are each three vertices of their own, as before
          bool grown = conservative && c.topology == D3D10_SB_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP && c.count >= 3;
          long long declared = (long long)expected * std::min(c.count, 256u);
          long long declared_vertices = grown ? 3 * (c.count - 2) : declared, declared_primitives = grown ? c.count - 2 : declared;
          long long groups = (warp + expected - 1) / expected;
          if (vertices != declared_vertices || primitives != declared_primitives || width != groups) {
            printf("wrong: %u vertices%s: the mesh has %lld vertices and %lld primitives, not %lld and %lld, and the "
                   "object stage starts %lld mesh threadgroups, not %lld\n",
                   c.count, conservative ? ", conservative" : "", vertices, primitives, declared_vertices,
                   declared_primitives, width, groups);
            wrong++;
          }
        }
      } else {
        wrong++;
      }
      SM50Destroy(gs);
    }
  SM50Destroy(vs);
  printf("%s: %u wrong results\n", wrong ? "failed" : "passed", wrong);
  return wrong ? 1 : 0;
}
