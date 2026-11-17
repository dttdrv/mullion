// contract: a compute shader with a device-coherent texture access converts for a Metal 4 device to what it converts
// to for Metal 3.2, as Apple's compiler for the M5 Ultra aborts on that access in Metal 4's AIR and builds it in 3.2's
// (dttdrv/mullion#238); a compute shader without one converts for Metal 4 to Metal 4's AIR. each is a cs_5_0 kernel
// of one typed store to a 2D texture UAV, written here from the tokenized program format, and the two differ only in
// D3D11_SB_GLOBALLY_COHERENT_ACCESS on the UAV's declaration
#include "DXBCParser/BlobContainer.h"
#include "airconv_public.h"
#include "DXBCParser/d3d12tokenizedprogramformat.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace microsoft;

static std::vector<uint32_t>
kernel(bool coherent) {
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
      // dcl_uav_typed_texture2d (uint, uint, uint, uint) u0, with or without globallycoherent
      ENCODE_D3D10_SB_OPCODE_TYPE(D3D11_SB_OPCODE_DCL_UNORDERED_ACCESS_VIEW_TYPED) |
          ENCODE_D3D10_SB_RESOURCE_DIMENSION(D3D10_SB_RESOURCE_DIMENSION_TEXTURE2D) |
          (coherent ? D3D11_SB_GLOBALLY_COHERENT_ACCESS : 0) | ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(4),
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

static bool
convert(const std::vector<uint8_t> &bytecode, SM50_SHADER_METAL_VERSION version, std::string &library) {
  sm50_shader_t shader = nullptr;
  sm50_error_t error = nullptr;
  char message[512] = "";
  if (SM50Initialize(bytecode.data(), bytecode.size(), &shader, nullptr, &error)) {
    SM50GetErrorMessage(error, message, sizeof(message));
    printf("wrong: the kernel does not parse: %s\n", message);
    SM50FreeError(error);
    return false;
  }
  SM50_SHADER_COMMON_DATA data{};
  data.type = SM50_SHADER_COMMON;
  data.metal_version = version;
  sm50_bitcode_t bitcode = nullptr;
  bool converted = !SM50Compile(shader, (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&data, "kernel", &bitcode, &error);
  if (converted) {
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
  return converted;
}

int
main() {
  unsigned wrong = 0;
  for (bool coherent : {true, false}) {
    std::string metal32, metal4;
    auto bytecode = container(kernel(coherent));
    if (!convert(bytecode, SM50_SHADER_METAL_320, metal32) || !convert(bytecode, SM50_SHADER_METAL_400, metal4)) {
      wrong++;
      continue;
    }
    if (coherent && metal4 != metal32) {
      printf("wrong: a kernel with a device-coherent texture store converts for Metal 4 to other than Metal 3.2's\n");
      wrong++;
    }
    if (!coherent && metal4 == metal32) {
      printf("wrong: a kernel without device-coherent access converts for Metal 4 to Metal 3.2's\n");
      wrong++;
    }
  }
  printf("%s: %u wrong results\n", wrong ? "failed" : "passed", wrong);
  return wrong ? 1 : 0;
}
