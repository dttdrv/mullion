// contract: a converted library's header names the macOS its module is for. a metallib's header has its platform
// word, its file version, its type, the OS (0x81, macOS) and the OS's major and minor version, which Apple's metallib
// writes from the module's target triple (MetalLibObjectWriter::writeHeader, Triple::getOSVersion), as `xcrun metal
// -mmacosx-version-min=26.0 -std=metal4.0` and `metallib` do: 26.0. a cs_5_0 kernel of one typed store, written
// here from the tokenized program format, is converted for each Metal version; each library's header must name the
// macOS of its module's triple, and Metal 4's AIR (air64_v28) is for macOS 26
#include "DXBCParser/BlobContainer.h"
#include "airconv_public.h"
#include "DXBCParser/d3d12tokenizedprogramformat.hpp"
#include "llvm/ADT/Triple.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/MemoryBuffer.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace microsoft;

static std::vector<uint32_t>
kernel() {
  auto uav = [](unsigned components) {
    return ENCODE_D3D10_SB_OPERAND_TYPE(D3D11_SB_OPERAND_TYPE_UNORDERED_ACCESS_VIEW) |
           ENCODE_D3D10_SB_OPERAND_INDEX_DIMENSION(D3D10_SB_OPERAND_INDEX_1D) | components;
  };
  auto immediate = ENCODE_D3D10_SB_OPERAND_TYPE(D3D10_SB_OPERAND_TYPE_IMMEDIATE32) |
                   ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_4_COMPONENT);
  auto uint4 = ENCODE_D3D10_SB_RESOURCE_RETURN_TYPE(D3D10_SB_RETURN_TYPE_UINT, 0) |
               ENCODE_D3D10_SB_RESOURCE_RETURN_TYPE(D3D10_SB_RETURN_TYPE_UINT, 1) |
               ENCODE_D3D10_SB_RESOURCE_RETURN_TYPE(D3D10_SB_RETURN_TYPE_UINT, 2) |
               ENCODE_D3D10_SB_RESOURCE_RETURN_TYPE(D3D10_SB_RETURN_TYPE_UINT, 3);
  // the token macros give int
  std::initializer_list<long long> tokens{
      ENCODE_D3D10_SB_TOKENIZED_PROGRAM_VERSION_TOKEN(D3D11_SB_COMPUTE_SHADER, 5, 0), 0,
      // dcl_uav_typed_texture2d (uint, uint, uint, uint) u0
      ENCODE_D3D10_SB_OPCODE_TYPE(D3D11_SB_OPCODE_DCL_UNORDERED_ACCESS_VIEW_TYPED) |
          ENCODE_D3D10_SB_RESOURCE_DIMENSION(D3D10_SB_RESOURCE_DIMENSION_TEXTURE2D) |
          ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(4),
      uav(ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_0_COMPONENT)), 0, uint4,
      // dcl_thread_group 1, 1, 1
      ENCODE_D3D10_SB_OPCODE_TYPE(D3D11_SB_OPCODE_DCL_THREAD_GROUP) | ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(4),
      1, 1, 1,
      // store_uav_typed u0.xyzw, l(0, 0, 0, 0), l(1, 2, 3, 4)
      ENCODE_D3D10_SB_OPCODE_TYPE(D3D11_SB_OPCODE_STORE_UAV_TYPED) | ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(13),
      uav(ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_4_COMPONENT) |
          ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(D3D10_SB_OPERAND_4_COMPONENT_MASK_MODE) |
          D3D10_SB_OPERAND_4_COMPONENT_MASK_ALL),
      0, immediate, 0, 0, 0, 0, immediate, 1, 2, 3, 4,
      ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_RET) | ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(1),
  };
  std::vector<uint32_t> program(tokens.begin(), tokens.end());
  program[1] = ENCODE_D3D10_SB_TOKENIZED_PROGRAM_LENGTH(program.size());
  return program;
}

// a container of an empty input signature, an empty output signature and the program
static std::vector<uint8_t>
container(const std::vector<uint32_t> &program) {
  const uint32_t empty[2] = {0, 8};
  struct Blob {
    DXBCFourCC fourcc;
    const void *data;
    uint32_t size;
  } blobs[] = {
      {DXBC_InputSignature, empty, sizeof(empty)},
      {DXBC_OutputSignature, empty, sizeof(empty)},
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

static std::string
convert(const std::vector<uint8_t> &bytecode, SM50_SHADER_METAL_VERSION version) {
  sm50_shader_t shader = nullptr;
  sm50_error_t error = nullptr;
  char message[512] = "";
  std::string library;
  if (SM50Initialize(bytecode.data(), bytecode.size(), &shader, nullptr, &error)) {
    SM50GetErrorMessage(error, message, sizeof(message));
    printf("wrong: the kernel does not parse: %s\n", message);
    SM50FreeError(error);
    return library;
  }
  SM50_SHADER_COMMON_DATA data{};
  data.type = SM50_SHADER_COMMON;
  data.metal_version = version;
  sm50_bitcode_t bitcode = nullptr;
  if (!SM50Compile(shader, (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&data, "kernel", &bitcode, &error)) {
    SM50_COMPILED_BITCODE compiled;
    SM50GetCompiledBitcode(bitcode, &compiled);
    library.assign((const char *)compiled.Data, compiled.Size);
    SM50DestroyBitcode(bitcode);
  } else {
    SM50GetErrorMessage(error, message, sizeof(message));
    printf("wrong: the kernel does not convert: %s\n", message);
    SM50FreeError(error);
  }
  SM50Destroy(shader);
  return library;
}

int
main() {
  unsigned wrong = 0;
  auto bytecode = container(kernel());
  for (auto version : {SM50_SHADER_METAL_310, SM50_SHADER_METAL_320, SM50_SHADER_METAL_400}) {
    auto library = convert(bytecode, version);
    if (library.size() < 16 || library.compare(0, 4, "MTLB")) {
      printf("wrong: Metal %u: no library\n", unsigned(version));
      wrong++;
      continue;
    }
    // the header: 'MTLB', the platform word, the file version (two 16-bit words), the type, the OS, its major and
    // minor version (16 bits each)
    uint16_t platform, major, minor;
    memcpy(&platform, library.data() + 4, 2);
    memcpy(&major, library.data() + 12, 2);
    memcpy(&minor, library.data() + 14, 2);
    uint8_t os = library[11];
    auto wrapper = library.find("\xde\xc0\x17\x0b");
    uint32_t bitcode[5];
    if (wrapper == std::string::npos || wrapper + sizeof(bitcode) > library.size()) {
      printf("wrong: Metal %u: a library without a bitcode wrapper\n", unsigned(version));
      wrong++;
      continue;
    }
    memcpy(bitcode, library.data() + wrapper, sizeof(bitcode));
    llvm::LLVMContext context;
    context.setOpaquePointers(false);
    auto module = llvm::parseBitcodeFile(
        llvm::MemoryBufferRef(llvm::StringRef(library.data() + wrapper + bitcode[2], bitcode[3]), "kernel"), context
    );
    if (!module) {
      printf("wrong: Metal %u: %s\n", unsigned(version), llvm::toString(module.takeError()).c_str());
      wrong++;
      continue;
    }
    llvm::Triple triple((*module)->getTargetTriple());
    auto wanted = triple.getOSVersion();
    printf("Metal %u: %s, header for macOS %u.%u\n", unsigned(version), triple.str().c_str(), unsigned(major),
           unsigned(minor));
    if (platform != 0x8001 || os != 0x81 || !triple.isMacOSX() || major != wanted.getMajor() ||
        minor != wanted.getMinor().value_or(0)) {
      printf("wrong: Metal %u: the header names OS 0x%02x %u.%u on platform 0x%04x, its module's triple %s\n",
             unsigned(version), os, unsigned(major), unsigned(minor), platform, triple.str().c_str());
      wrong++;
    }
    if (version == SM50_SHADER_METAL_400 && major != 26) {
      printf("wrong: Metal 4's library is for macOS %u.%u\n", unsigned(major), unsigned(minor));
      wrong++;
    }
  }
  printf("%s: %u wrong results\n", wrong ? "failed" : "passed", wrong);
  return wrong ? 1 : 0;
}
