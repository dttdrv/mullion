// contract: a stage before the rasterizer computes its position and clip distances as written, and may contract and
// reassociate what they do not depend on. a depth prepass and the pass after it draw with different shaders whose
// positions have the same expression, and an EQUAL depth test after the prepass needs the same depths from both: what
// decides whether Metal fuses a multiply and an add is the operations' contract flag (LLVM LangRef, "Fast-Math
// Flags"; a multiply and add with it fused, and without it not, on an M6), and whether two shaders contract the
// same way depends on the code around them, so what the position depends on keeps no flag. a varying has no such use
// and may be fused, as a pixel shader's operations are (HLSL asks for an expression to be computed as written with
// "precise"). each case is a shader written here from the tokenized program format whose outputs are each a multiply
// and an add of its inputs: a vs_5_0 with a position, a varying and a clip distance, and a gs_4_0 with a position
// and a varying after a vertex shader. the converted module's position and clip distance must depend on no operation
// with contract, reassoc or arcp and on no fused one, and its varying's multiply and add must have contract
#include "DXBCParser/BlobContainer.h"
#include "airconv_public.h"
#include "DXBCParser/d3d12tokenizedprogramformat.hpp"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using namespace microsoft;

using Tokens = std::initializer_list<long long>;

static void
append(std::vector<uint32_t> &program, Tokens tokens) {
  program.insert(program.end(), tokens.begin(), tokens.end());
}
static long long
operand(D3D10_SB_OPERAND_TYPE type, long long selection, D3D10_SB_OPERAND_INDEX_DIMENSION dimension = D3D10_SB_OPERAND_INDEX_1D) {
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
// `op` dst, a, b of whole registers: dst and sources (type, index)
static void
binary(std::vector<uint32_t> &program, D3D10_SB_OPCODE_TYPE op, D3D10_SB_OPERAND_TYPE dst, uint32_t d,
       D3D10_SB_OPERAND_TYPE a, uint32_t ai, D3D10_SB_OPERAND_TYPE b, uint32_t bi) {
  append(program, {opcode(op, 7), operand(dst, mask_all), d, operand(a, no_swizzle), ai, operand(b, no_swizzle), bi});
}

// an ISGN or OSGN part of whole float4 elements: (name, system value) in registers 0 on
static std::vector<uint8_t>
signature(std::vector<std::pair<const char *, uint32_t>> elements) {
  struct Element {
    uint32_t name, index, system_value, component_type, reg;
    uint8_t mask, used, pad[2];
  };
  uint32_t names = 8 + elements.size() * sizeof(Element);
  std::vector<uint8_t> out(names);
  uint32_t header[2] = {uint32_t(elements.size()), 8};
  memcpy(out.data(), header, sizeof(header));
  for (uint32_t i = 0; i < elements.size(); i++) {
    Element element{uint32_t(out.size()), 0, elements[i].second, 3 /* D3D_REGISTER_COMPONENT_FLOAT32 */, i, 0xf, 0xf, {}};
    memcpy(out.data() + 8 + i * sizeof(Element), &element, sizeof(element));
    out.insert(out.end(), elements[i].first, elements[i].first + strlen(elements[i].first) + 1);
  }
  while (out.size() % 4)
    out.push_back(0);
  return out;
}

static std::vector<uint8_t>
container(const std::vector<uint8_t> &input, const std::vector<uint8_t> &output, std::vector<uint32_t> program) {
  append(program, {opcode(D3D10_SB_OPCODE_RET, 1)});
  program[1] = ENCODE_D3D10_SB_TOKENIZED_PROGRAM_LENGTH(program.size());
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

// vs_5_0: o0 (SV_Position) = v0 * v1 + v1, o1 (TEXCOORD0) = v1 * v1 + v0, o2 (SV_ClipDistance) = v0 * v0 + v1
static std::vector<uint8_t>
vertex_shader() {
  std::vector<uint32_t> p{ENCODE_D3D10_SB_TOKENIZED_PROGRAM_VERSION_TOKEN(D3D10_SB_VERTEX_SHADER, 5, 0), 0};
  auto input = operand(D3D10_SB_OPERAND_TYPE_INPUT, mask_all), output = operand(D3D10_SB_OPERAND_TYPE_OUTPUT, mask_all);
  // dcl_globalFlags refactoringAllowed, as fxc declares it but for /Gis: without it every operation is precise
  append(p, {opcode(D3D10_SB_OPCODE_DCL_GLOBAL_FLAGS, 1, D3D10_SB_GLOBAL_FLAG_REFACTORING_ALLOWED)});
  append(p, {opcode(D3D10_SB_OPCODE_DCL_INPUT, 3), input, 0});
  append(p, {opcode(D3D10_SB_OPCODE_DCL_INPUT, 3), input, 1});
  append(p, {opcode(D3D10_SB_OPCODE_DCL_OUTPUT_SIV, 4), output, 0, ENCODE_D3D10_SB_NAME(D3D10_SB_NAME_POSITION)});
  append(p, {opcode(D3D10_SB_OPCODE_DCL_OUTPUT, 3), output, 1});
  append(p, {opcode(D3D10_SB_OPCODE_DCL_OUTPUT_SIV, 4), output, 2, ENCODE_D3D10_SB_NAME(D3D10_SB_NAME_CLIP_DISTANCE)});
  append(p, {opcode(D3D10_SB_OPCODE_DCL_TEMPS, 2), 3});
  auto I = D3D10_SB_OPERAND_TYPE_INPUT, O = D3D10_SB_OPERAND_TYPE_OUTPUT, R = D3D10_SB_OPERAND_TYPE_TEMP;
  binary(p, D3D10_SB_OPCODE_MUL, R, 0, I, 0, I, 1);
  binary(p, D3D10_SB_OPCODE_ADD, O, 0, R, 0, I, 1);
  binary(p, D3D10_SB_OPCODE_MUL, R, 1, I, 1, I, 1);
  binary(p, D3D10_SB_OPCODE_ADD, O, 1, R, 1, I, 0);
  binary(p, D3D10_SB_OPCODE_MUL, R, 2, I, 0, I, 0);
  binary(p, D3D10_SB_OPCODE_ADD, O, 2, R, 2, I, 1);
  return container(signature({{"POSITION", 0}, {"TEXCOORD", 0}}),
                   signature({{"SV_Position", 1}, {"TEXCOORD", 0}, {"SV_ClipDistance", 2}}), p);
}

// vs_5_0 for a geometry shader: o0 (SV_Position) = v0, o1 (TEXCOORD0) = v1
static std::vector<uint8_t>
passing_vertex_shader() {
  std::vector<uint32_t> p{ENCODE_D3D10_SB_TOKENIZED_PROGRAM_VERSION_TOKEN(D3D10_SB_VERTEX_SHADER, 5, 0), 0};
  auto input = operand(D3D10_SB_OPERAND_TYPE_INPUT, mask_all), output = operand(D3D10_SB_OPERAND_TYPE_OUTPUT, mask_all);
  append(p, {opcode(D3D10_SB_OPCODE_DCL_INPUT, 3), input, 0});
  append(p, {opcode(D3D10_SB_OPCODE_DCL_INPUT, 3), input, 1});
  append(p, {opcode(D3D10_SB_OPCODE_DCL_OUTPUT_SIV, 4), output, 0, ENCODE_D3D10_SB_NAME(D3D10_SB_NAME_POSITION)});
  append(p, {opcode(D3D10_SB_OPCODE_DCL_OUTPUT, 3), output, 1});
  for (uint32_t reg = 0; reg < 2; reg++)
    append(p, {opcode(D3D10_SB_OPCODE_MOV, 5), output, reg, operand(D3D10_SB_OPERAND_TYPE_INPUT, no_swizzle), reg});
  return container(signature({{"POSITION", 0}, {"TEXCOORD", 0}}), signature({{"SV_Position", 1}, {"TEXCOORD", 0}}), p);
}

// gs_4_0 of a point, emitted as a point: o0 (SV_Position) = v[0][1] * v[0][1] + v[0][0], o1 (TEXCOORD0) =
// v[0][0] * v[0][0] + v[0][1]
static std::vector<uint8_t>
geometry_shader() {
  std::vector<uint32_t> p{ENCODE_D3D10_SB_TOKENIZED_PROGRAM_VERSION_TOKEN(D3D10_SB_GEOMETRY_SHADER, 4, 0), 0};
  auto input = operand(D3D10_SB_OPERAND_TYPE_INPUT, mask_all, D3D10_SB_OPERAND_INDEX_2D),
       output = operand(D3D10_SB_OPERAND_TYPE_OUTPUT, mask_all);
  append(p, {opcode(D3D10_SB_OPCODE_DCL_GLOBAL_FLAGS, 1, D3D10_SB_GLOBAL_FLAG_REFACTORING_ALLOWED)});
  append(p, {opcode(D3D10_SB_OPCODE_DCL_INPUT_SIV, 5), input, 1, 0, ENCODE_D3D10_SB_NAME(D3D10_SB_NAME_POSITION)});
  append(p, {opcode(D3D10_SB_OPCODE_DCL_INPUT, 4), input, 1, 1});
  append(p, {opcode(D3D10_SB_OPCODE_DCL_GS_INPUT_PRIMITIVE, 1, ENCODE_D3D10_SB_GS_INPUT_PRIMITIVE(D3D10_SB_PRIMITIVE_POINT))});
  append(p, {opcode(D3D10_SB_OPCODE_DCL_GS_OUTPUT_PRIMITIVE_TOPOLOGY, 1,
                    ENCODE_D3D10_SB_GS_OUTPUT_PRIMITIVE_TOPOLOGY(D3D10_SB_PRIMITIVE_TOPOLOGY_POINTLIST))});
  append(p, {opcode(D3D10_SB_OPCODE_DCL_OUTPUT_SIV, 4), output, 0, ENCODE_D3D10_SB_NAME(D3D10_SB_NAME_POSITION)});
  append(p, {opcode(D3D10_SB_OPCODE_DCL_OUTPUT, 3), output, 1});
  append(p, {opcode(D3D10_SB_OPCODE_DCL_TEMPS, 2), 2});
  append(p, {opcode(D3D10_SB_OPCODE_DCL_MAX_OUTPUT_VERTEX_COUNT, 2), 1});
  auto source = operand(D3D10_SB_OPERAND_TYPE_INPUT, no_swizzle, D3D10_SB_OPERAND_INDEX_2D);
  for (uint32_t out = 0; out < 2; out++) {
    uint32_t a = out ? 0 : 1, b = out ? 1 : 0;
    append(p, {opcode(D3D10_SB_OPCODE_MUL, 9), operand(D3D10_SB_OPERAND_TYPE_TEMP, mask_all), out, source, 0, a, source, 0, a});
    append(p, {opcode(D3D10_SB_OPCODE_ADD, 8), output, out, operand(D3D10_SB_OPERAND_TYPE_TEMP, no_swizzle), out, source, 0, b});
  }
  append(p, {opcode(D3D10_SB_OPCODE_EMIT, 1)});
  auto io = signature({{"SV_Position", 1}, {"TEXCOORD", 0}});
  return container(io, io, p);
}

static sm50_shader_t
parse(const std::vector<uint8_t> &bytecode) {
  sm50_shader_t shader = nullptr;
  sm50_error_t error = nullptr;
  if (!SM50Initialize(bytecode.data(), bytecode.size(), &shader, nullptr, &error))
    return shader;
  char text[512] = "";
  SM50GetErrorMessage(error, text, sizeof(text));
  SM50FreeError(error);
  printf("wrong: a shader does not parse: %s\n", text);
  return nullptr;
}

// the module of the library a conversion made, or none
static std::unique_ptr<llvm::Module>
module_of(int result, sm50_bitcode_t bitcode, sm50_error_t error, llvm::LLVMContext &context) {
  if (result) {
    char text[512] = "";
    SM50GetErrorMessage(error, text, sizeof(text));
    SM50FreeError(error);
    printf("wrong: a shader does not convert: %s\n", text);
    return nullptr;
  }
  SM50_COMPILED_BITCODE compiled;
  SM50GetCompiledBitcode(bitcode, &compiled);
  std::string library((const char *)compiled.Data, compiled.Size);
  SM50DestroyBitcode(bitcode);
  auto wrapper = library.find("\xde\xc0\x17\x0b");
  uint32_t header[5];
  if (wrapper == std::string::npos || wrapper + sizeof(header) > library.size()) {
    printf("wrong: a library without a bitcode wrapper\n");
    return nullptr;
  }
  memcpy(header, library.data() + wrapper, sizeof(header));
  auto module = llvm::parseBitcodeFile(
      llvm::MemoryBufferRef(llvm::StringRef(library.data() + wrapper + header[2], header[3]), "shader"), context
  );
  if (!module) {
    printf("wrong: %s\n", llvm::toString(module.takeError()).c_str());
    return nullptr;
  }
  return std::move(*module);
}

// the instructions `values` depend on, through their operands
static std::set<const llvm::Instruction *>
depended(const std::vector<const llvm::Value *> &values) {
  std::set<const llvm::Instruction *> seen;
  std::vector<const llvm::Instruction *> work;
  auto add = [&](const llvm::Value *v) {
    if (auto i = llvm::dyn_cast<llvm::Instruction>(v); i && seen.insert(i).second)
      work.push_back(i);
  };
  for (auto v : values)
    add(v);
  while (!work.empty()) {
    auto i = work.back();
    work.pop_back();
    for (auto &use : i->operands())
      add(use.get());
  }
  return seen;
}

static bool
contractible(const llvm::Instruction *i) {
  return llvm::isa<llvm::FPMathOperator>(i) &&
         (i->hasAllowContract() || i->hasAllowReassoc() || i->hasAllowReciprocal());
}
static bool
fused(const llvm::Instruction *i) {
  auto call = llvm::dyn_cast<llvm::CallInst>(i);
  auto callee = call ? call->getCalledFunction() : nullptr;
  return callee && (callee->getName().contains("fma") || callee->getName().contains("fmuladd"));
}

// `what`'s `kept` must have neither a contractible nor a fused operation, and the multiplies and adds of `free` that
// `kept` does not depend on must all be contractible, at least one of each
static unsigned
check(const char *what, const std::vector<const llvm::Value *> &kept, const std::vector<const llvm::Value *> &free) {
  unsigned wrong = 0;
  auto as_written = depended(kept);
  for (auto i : as_written)
    if (contractible(i) || fused(i)) {
      std::string text;
      llvm::raw_string_ostream out(text);
      i->print(out);
      printf("wrong: %s: the position depends on %s\n", what, out.str().c_str());
      wrong++;
    }
  unsigned multiplies = 0, adds = 0;
  for (auto i : depended(free)) {
    if (as_written.count(i) || (i->getOpcode() != llvm::Instruction::FMul && i->getOpcode() != llvm::Instruction::FAdd))
      continue;
    if (!i->hasAllowContract()) {
      std::string text;
      llvm::raw_string_ostream out(text);
      i->print(out);
      printf("wrong: %s: a varying depends on %s, which may not be contracted\n", what, out.str().c_str());
      wrong++;
    }
    (i->getOpcode() == llvm::Instruction::FMul ? multiplies : adds)++;
  }
  if (!multiplies || !adds) {
    printf("wrong: %s: the varying has %u multiplies and %u adds of its own\n", what, multiplies, adds);
    wrong++;
  }
  return wrong;
}

static const char *
output_kind(const llvm::MDNode *output) {
  auto kind = output && output->getNumOperands() ? llvm::dyn_cast<llvm::MDString>(output->getOperand(0).get()) : nullptr;
  return kind ? kind->getString().data() : "";
}

// the vertex function's position and clip distance fields, and its other fields
static unsigned
check_vertex(llvm::Module &module) {
  auto functions = module.getNamedMetadata("air.vertex");
  auto node = functions && functions->getNumOperands() == 1 ? functions->getOperand(0) : nullptr;
  auto function = node ? llvm::mdconst::dyn_extract_or_null<llvm::Function>(node->getOperand(0)) : nullptr;
  if (!function) {
    printf("wrong: no one vertex function\n");
    return 1;
  }
  auto outputs = llvm::cast<llvm::MDNode>(node->getOperand(1).get());
  std::vector<const llvm::Value *> kept, free;
  for (auto &i : llvm::instructions(*function))
    if (auto ret = llvm::dyn_cast<llvm::ReturnInst>(&i))
      for (auto v = ret->getReturnValue(); auto insert = llvm::dyn_cast_or_null<llvm::InsertValueInst>(v);
           v = insert->getAggregateOperand()) {
        auto kind = std::string(output_kind(llvm::cast<llvm::MDNode>(outputs->getOperand(insert->getIndices()[0]).get())));
        (kind == "air.position" || kind == "air.clip_distance" ? kept : free).push_back(insert->getInsertedValueOperand());
      }
  return check("the vertex shader", kept, free);
}

// the mesh function's positions, and its vertex data
static unsigned
check_mesh(llvm::Module &module) {
  auto functions = module.getNamedMetadata("air.mesh");
  auto node = functions && functions->getNumOperands() == 1 ? functions->getOperand(0) : nullptr;
  auto function = node ? llvm::mdconst::dyn_extract_or_null<llvm::Function>(node->getOperand(0)) : nullptr;
  if (!function) {
    printf("wrong: no one mesh function\n");
    return 1;
  }
  std::vector<const llvm::Value *> kept, free;
  for (auto &i : llvm::instructions(*function))
    if (auto call = llvm::dyn_cast<llvm::CallInst>(&i); call && call->getCalledFunction()) {
      auto name = call->getCalledFunction()->getName();
      if (name == "air.set_position_mesh")
        kept.push_back(call->getArgOperand(2));
      else if (name.startswith("air.set_vertex_data_mesh"))
        free.push_back(call->getArgOperand(3));
    }
  return check("the geometry shader", kept, free);
}

int
main() {
  unsigned wrong = 0;
  llvm::LLVMContext context;
  context.setOpaquePointers(false);
  SM50_SHADER_COMMON_DATA common{};
  common.type = SM50_SHADER_COMMON;
  common.metal_version = SM50_SHADER_METAL_320;
  if (auto vs = parse(vertex_shader())) {
    sm50_bitcode_t bitcode = nullptr;
    sm50_error_t error = nullptr;
    int result = SM50Compile(vs, (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&common, "main", &bitcode, &error);
    if (auto module = module_of(result, bitcode, error, context))
      wrong += check_vertex(*module);
    else
      wrong++;
    SM50Destroy(vs);
  } else {
    wrong++;
  }
  auto vs = parse(passing_vertex_shader()), gs = parse(geometry_shader());
  if (vs && gs) {
    SM50_SHADER_PSO_GEOMETRY_SHADER_DATA pso{};
    pso.type = SM50_SHADER_PSO_GEOMETRY_SHADER;
    pso.next = &common;
    sm50_bitcode_t bitcode = nullptr;
    sm50_error_t error = nullptr;
    int result =
        SM50CompileGeometryPipelineGeometry(vs, gs, (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&pso, "main", &bitcode, &error);
    if (auto module = module_of(result, bitcode, error, context)) {
      wrong += check_mesh(*module);
    } else {
      wrong++;
    }
  } else {
    wrong++;
  }
  if (vs)
    SM50Destroy(vs);
  if (gs)
    SM50Destroy(gs);
  printf("%s: %u wrong results\n", wrong ? "failed" : "passed", wrong);
  return wrong ? 1 : 0;
}
