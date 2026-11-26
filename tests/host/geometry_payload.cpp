// contract: a geometry pipeline's object and mesh functions declare the payload the object threadgroup fills, and
// no more: four words, then the output registers of each vertex the group reads (16 bytes each, at most 32 vertices
// as a SIMD group's threads, fewer where the registers would not fit 16256 bytes), then nine words, rounded up to 16
// bytes as Metal Shader Converter pads its payload. a payload is "space in memory
// where the object shader can pass along data to the mesh shader" whose size "can't exceed the limit of 16
// kilobytes" (WWDC22 10162, 8:50); what is declared beyond what the group stores is memory each group reserves for
// nothing. each case is a vs_5_0 vertex shader of `registers` output registers, SV_Position the first, written here
// from the tokenized program format, with a gs_4_0 geometry shader that emits its point's position, or with none
// (triangles passed on, D3D11.3 8.15); both functions of each must declare the same size, SM50GeometryPayloadSize's
#include "DXBCParser/BlobContainer.h"
#include "airconv_public.h"
#include "DXBCParser/d3d12tokenizedprogramformat.hpp"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/MemoryBuffer.h"
#include <cstdio>
#include <cstring>
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

// [maxvertexcount(1)] a point's position, emitted as a point
static std::vector<uint32_t>
geometry_shader() {
  std::vector<uint32_t> program{ENCODE_D3D10_SB_TOKENIZED_PROGRAM_VERSION_TOKEN(D3D10_SB_GEOMETRY_SHADER, 4, 0), 0};
  append(program, {opcode(D3D10_SB_OPCODE_DCL_INPUT_SIV, 5),
                   operand(D3D10_SB_OPERAND_TYPE_INPUT, D3D10_SB_OPERAND_INDEX_2D, mask_all), 1, 0,
                   ENCODE_D3D10_SB_NAME(D3D10_SB_NAME_POSITION)});
  append(program, {opcode(D3D10_SB_OPCODE_DCL_GS_INPUT_PRIMITIVE, 1,
                          ENCODE_D3D10_SB_GS_INPUT_PRIMITIVE(D3D10_SB_PRIMITIVE_POINT))});
  append(program, {opcode(D3D10_SB_OPCODE_DCL_GS_OUTPUT_PRIMITIVE_TOPOLOGY, 1,
                          ENCODE_D3D10_SB_GS_OUTPUT_PRIMITIVE_TOPOLOGY(D3D10_SB_PRIMITIVE_TOPOLOGY_POINTLIST))});
  auto output = operand(D3D10_SB_OPERAND_TYPE_OUTPUT, D3D10_SB_OPERAND_INDEX_1D, mask_all);
  append(program, {opcode(D3D10_SB_OPCODE_DCL_OUTPUT_SIV, 4), output, 0, ENCODE_D3D10_SB_NAME(D3D10_SB_NAME_POSITION)});
  append(program, {opcode(D3D10_SB_OPCODE_DCL_MAX_OUTPUT_VERTEX_COUNT, 2), 1});
  append(program, {opcode(D3D10_SB_OPCODE_MOV, 6), output, 0,
                   operand(D3D10_SB_OPERAND_TYPE_INPUT, D3D10_SB_OPERAND_INDEX_2D, no_swizzle), 0, 0});
  append(program, {opcode(D3D10_SB_OPCODE_EMIT, 1)});
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

// the air.arg_type_size of the air.payload argument somewhere in `node`, or -1
static long long
payload_size(const llvm::MDNode *node, unsigned depth = 0) {
  if (!node || depth > 4)
    return -1;
  for (unsigned i = 0; i < node->getNumOperands(); i++) {
    auto string = llvm::dyn_cast_or_null<llvm::MDString>(node->getOperand(i).get());
    if (!string || string->getString() != "air.payload")
      continue;
    for (unsigned j = i + 1; j + 1 < node->getNumOperands(); j++)
      if (auto key = llvm::dyn_cast_or_null<llvm::MDString>(node->getOperand(j).get());
          key && key->getString() == "air.arg_type_size")
        if (auto value = llvm::mdconst::dyn_extract_or_null<llvm::ConstantInt>(node->getOperand(j + 1)))
          return value->getZExtValue();
  }
  for (unsigned i = 0; i < node->getNumOperands(); i++)
    if (auto found = payload_size(llvm::dyn_cast_or_null<llvm::MDNode>(node->getOperand(i).get()), depth + 1); found >= 0)
      return found;
  return -1;
}

// the payload the library's `stage` function (air.object or air.mesh) declares, or -1
static long long
declared_payload(const std::string &library, const char *stage, std::string &message) {
  auto wrapper = library.find("\xde\xc0\x17\x0b");
  uint32_t header[5];
  if (wrapper == std::string::npos || wrapper + sizeof(header) > library.size()) {
    message = "a library without a bitcode wrapper";
    return -1;
  }
  memcpy(header, library.data() + wrapper, sizeof(header));
  llvm::LLVMContext context;
  context.setOpaquePointers(false);
  auto module = llvm::parseBitcodeFile(
      llvm::MemoryBufferRef(llvm::StringRef(library.data() + wrapper + header[2], header[3]), "shader"), context
  );
  if (!module) {
    message = llvm::toString(module.takeError());
    return -1;
  }
  auto functions = (*module)->getNamedMetadata(stage);
  if (!functions || functions->getNumOperands() != 1) {
    message = std::string("no one ") + stage + " function";
    return -1;
  }
  auto size = payload_size(functions->getOperand(0));
  if (size < 0)
    message = "no air.payload argument";
  return size;
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
  // the payload's words for each count of registers: 1 and 4 leave room for 32 vertices, 32 for (4064 - 13) / 128 = 31
  const struct {
    uint32_t registers, bytes;
  } cases[] = {
      {1, ((4 + 32 * 4 * 1 + 9) * 4 + 15) / 16 * 16},
      {4, ((4 + 32 * 4 * 4 + 9) * 4 + 15) / 16 * 16},
      {32, ((4 + 31 * 4 * 32 + 9) * 4 + 15) / 16 * 16},
  };
  for (auto &c : cases)
    for (bool geometry : {true, false}) {
      sm50_shader_t vs = nullptr, gs = nullptr;
      auto empty = signature(0);
      if (!parse(container(empty, signature(c.registers), vertex_shader(c.registers)), vs) ||
          (geometry && !parse(container(signature(1), signature(1), geometry_shader()), gs))) {
        wrong++;
        continue;
      }
      SM50_SHADER_PSO_GEOMETRY_SHADER_DATA pso{};
      pso.type = SM50_SHADER_PSO_GEOMETRY_SHADER;
      pso.input_primitive = geometry ? 0 : D3D10_SB_PRIMITIVE_TRIANGLE;
      pso.pass_through = !geometry;
      std::string object, mesh;
      if (convert(SM50CompileGeometryPipelineVertex, vs, gs, pso, object) &&
          convert(SM50CompileGeometryPipelineGeometry, vs, gs, pso, mesh)) {
        std::string message;
        auto object_size = declared_payload(object, "air.object", message);
        auto mesh_size = declared_payload(mesh, "air.mesh", message);
        if (object_size < 0 || mesh_size < 0) {
          printf("wrong: %u registers, %s: %s\n", c.registers, geometry ? "a geometry shader" : "passed through",
                 message.c_str());
          wrong++;
        } else if (object_size != c.bytes || mesh_size != c.bytes || SM50GeometryPayloadSize(c.registers) != c.bytes) {
          printf("wrong: %u registers, %s: the object function's payload is %lld bytes, the mesh function's %lld, "
                 "the pipeline's %u, not %u\n",
                 c.registers, geometry ? "a geometry shader" : "passed through", object_size, mesh_size,
                 SM50GeometryPayloadSize(c.registers), c.bytes);
          wrong++;
        }
      } else {
        wrong++;
      }
      SM50Destroy(vs);
      if (gs)
        SM50Destroy(gs);
    }
  printf("%s: %u wrong results\n", wrong ? "failed" : "passed", wrong);
  return wrong ? 1 : 0;
}
