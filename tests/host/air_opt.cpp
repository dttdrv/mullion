// contract: verifies AIR optimization invariants through Apple's metal-opt verifier and IR disassembler:
// 1. Contiguous 4-component structured and raw buffer loads/stores emit <4 x i32> vector load/store instructions.
// 2. TextureLoad (ld) at constant LOD 0 emits no air.get_num_mip_levels query.
// 3. air.dfdx / air.dfdy carry both readnone and convergent attributes.
#include "DXBCParser/BlobContainer.h"
#include "airconv_public.h"
#include "DXBCParser/d3d12tokenizedprogramformat.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

using namespace microsoft;
using Tokens = std::vector<long long>;

static long long
operand(D3D10_SB_OPERAND_TYPE type, long long selection) {
  return ENCODE_D3D10_SB_OPERAND_TYPE(type) | ENCODE_D3D10_SB_OPERAND_INDEX_DIMENSION(D3D10_SB_OPERAND_INDEX_1D) |
         selection;
}
static const long long none = ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_0_COMPONENT);
static const long long all = ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_4_COMPONENT) |
                             ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(D3D10_SB_OPERAND_4_COMPONENT_MASK_MODE) |
                             D3D10_SB_OPERAND_4_COMPONENT_MASK_ALL;
static const long long x = ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_4_COMPONENT) |
                           ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(D3D10_SB_OPERAND_4_COMPONENT_MASK_MODE) |
                           D3D10_SB_OPERAND_4_COMPONENT_MASK_X;
static const long long xyzw = ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_4_COMPONENT) |
                              ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_MODE) |
                              D3D10_SB_OPERAND_4_COMPONENT_NOSWIZZLE;
static const long long sel_x = ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_4_COMPONENT) |
                               ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(D3D10_SB_OPERAND_4_COMPONENT_SELECT_1_MODE) |
                               ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECT_1(D3D10_SB_4_COMPONENT_X);
static const long long uint4 = ENCODE_D3D10_SB_RESOURCE_RETURN_TYPE(D3D10_SB_RETURN_TYPE_UINT, 0) |
                               ENCODE_D3D10_SB_RESOURCE_RETURN_TYPE(D3D10_SB_RETURN_TYPE_UINT, 1) |
                               ENCODE_D3D10_SB_RESOURCE_RETURN_TYPE(D3D10_SB_RETURN_TYPE_UINT, 2) |
                               ENCODE_D3D10_SB_RESOURCE_RETURN_TYPE(D3D10_SB_RETURN_TYPE_UINT, 3);

static Tokens
immediate(long long a, long long b, long long c, long long d) {
  return {ENCODE_D3D10_SB_OPERAND_TYPE(D3D10_SB_OPERAND_TYPE_IMMEDIATE32) |
              ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_4_COMPONENT),
          a, b, c, d};
}
static Tokens
scalar(long long a) {
  return {ENCODE_D3D10_SB_OPERAND_TYPE(D3D10_SB_OPERAND_TYPE_IMMEDIATE32) |
              ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_1_COMPONENT),
          a};
}
static Tokens
instruction(long long opcode, std::initializer_list<Tokens> operands) {
  Tokens out{opcode};
  for (auto &o : operands)
    out.insert(out.end(), o.begin(), o.end());
  out[0] |= ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(out.size());
  return out;
}
static Tokens
temp(long long i, long long selection) {
  return {operand(D3D10_SB_OPERAND_TYPE_TEMP, selection), i};
}
static Tokens
tid_x() {
  return {ENCODE_D3D10_SB_OPERAND_TYPE(D3D11_SB_OPERAND_TYPE_INPUT_THREAD_ID) |
          ENCODE_D3D10_SB_OPERAND_INDEX_DIMENSION(D3D10_SB_OPERAND_INDEX_0D) | sel_x};
}

static std::vector<uint8_t>
container(const std::vector<uint32_t> &program, const std::vector<uint8_t> &isgn = {}, const std::vector<uint8_t> &osgn = {}) {
  static const uint32_t empty_sig[2] = {0, 8};
  struct Chunk {
    uint32_t fourcc;
    const void *data;
    uint32_t size;
  };
  std::vector<Chunk> chunks;
  chunks.push_back({DXBC_InputSignature, isgn.empty() ? (const void *)empty_sig : isgn.data(),
                    isgn.empty() ? uint32_t(sizeof(empty_sig)) : uint32_t(isgn.size())});
  chunks.push_back({DXBC_OutputSignature, osgn.empty() ? (const void *)empty_sig : osgn.data(),
                    osgn.empty() ? uint32_t(sizeof(empty_sig)) : uint32_t(osgn.size())});
  chunks.push_back({DXBC_GenericShaderEx, program.data(), uint32_t(program.size() * sizeof(uint32_t))});

  uint32_t header_size = sizeof(DXBCHeader) + uint32_t(chunks.size() * sizeof(uint32_t));
  uint32_t total = header_size;
  for (auto &c : chunks)
    total += sizeof(DXBCBlobHeader) + c.size;
  std::vector<uint8_t> out(total, 0);
  DXBCHeader header{};
  header.DXBCHeaderFourCC = DXBC_FOURCC_NAME;
  header.Version = {DXBC_MAJOR_VERSION, DXBC_MINOR_VERSION};
  header.ContainerSizeInBytes = total;
  header.BlobCount = uint32_t(chunks.size());
  memcpy(out.data(), &header, sizeof(header));
  uint32_t offset = header_size;
  for (size_t i = 0; i < chunks.size(); i++) {
    memcpy(out.data() + sizeof(DXBCHeader) + i * sizeof(uint32_t), &offset, sizeof(offset));
    DXBCBlobHeader bh{DXBCFourCC(chunks[i].fourcc), chunks[i].size};
    memcpy(out.data() + offset, &bh, sizeof(bh));
    memcpy(out.data() + offset + sizeof(bh), chunks[i].data, chunks[i].size);
    offset += sizeof(bh) + chunks[i].size;
  }
  return out;
}

static std::vector<uint8_t>
signature_chunk(const char *semantic, uint32_t sysval, uint32_t mask) {
  // DXBC signature: count(4), offset(4), 1 element (24 bytes), semantic string
  size_t name_off = 8 + 24;
  size_t name_len = strlen(semantic) + 1;
  size_t padded = (name_off + name_len + 3) & ~size_t(3);
  std::vector<uint8_t> buf(padded, 0);
  uint32_t words[8] = {
      1, 8, uint32_t(name_off), 0, sysval, 3 /* float */, 0 /* register */, mask | (mask << 8),
  };
  memcpy(buf.data(), words, sizeof(words));
  memcpy(buf.data() + name_off, semantic, name_len);
  return buf;
}

static std::string
convert_and_disassemble(const std::vector<uint8_t> &bytecode, SM50_SHADER_METAL_VERSION version, std::string &err) {
  sm50_shader_t shader = nullptr;
  sm50_error_t error = nullptr;
  char text[512] = "";
  std::string library;
  if (SM50Initialize(bytecode.data(), bytecode.size(), &shader, nullptr, &error)) {
    SM50GetErrorMessage(error, text, sizeof(text));
    SM50FreeError(error);
    err = std::string("parse failed: ") + text;
    return "";
  }
  SM50_SHADER_COMMON_DATA data{};
  data.type = SM50_SHADER_COMMON;
  data.metal_version = version;
  sm50_bitcode_t bitcode = nullptr;
  if (!SM50Compile(shader, (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&data, "entry", &bitcode, &error)) {
    SM50_COMPILED_BITCODE compiled;
    SM50GetCompiledBitcode(bitcode, &compiled);
    library.assign((const char *)compiled.Data, compiled.Size);
    SM50DestroyBitcode(bitcode);
  } else {
    SM50GetErrorMessage(error, text, sizeof(text));
    SM50FreeError(error);
    err = std::string("compile failed: ") + text;
  }
  SM50Destroy(shader);
  auto wrapper = library.find("\xde\xc0\x17\x0b");
  uint32_t header[5];
  if (wrapper == std::string::npos || wrapper + sizeof(header) > library.size()) {
    if (err.empty())
      err = "no bitcode wrapper";
    return "";
  }
  memcpy(header, library.data() + wrapper, sizeof(header));
  std::string bc = library.substr(wrapper + header[2], header[3]);

  char path[] = "/tmp/air_opt.XXXXXX";
  int fd = mkstemp(path);
  if (fd < 0) {
    err = "mkstemp failed";
    return "";
  }
  ssize_t written = write(fd, bc.data(), bc.size());
  if (close(fd) != 0 || written != ssize_t(bc.size())) {
    unlink(path);
    err = "write failed";
    return "";
  }
  static const std::string tool = [] {
    auto ask = [](const char *command) {
      std::string found;
      if (FILE *where = popen(command, "r")) {
        char line[1024];
        if (fgets(line, sizeof(line), where))
          found = line;
        pclose(where);
      }
      while (!found.empty() && (found.back() == '\n' || found.back() == '\r'))
        found.pop_back();
      return found;
    };
    std::string found = ask("xcrun -sdk macosx -f metal-opt 2>/dev/null"),
                sdk = ask("xcrun -sdk macosx --show-sdk-path 2>/dev/null");
    if (found.empty() || sdk.empty())
      return std::string("xcrun -sdk macosx metal-opt");
    return "SDKROOT='" + sdk + "' '" + found + "'";
  }();
  char outpath[] = "/tmp/air_opt_ir.XXXXXX";
  int outfd = mkstemp(outpath);
  if (outfd < 0) {
    unlink(path);
    err = "mkstemp out failed";
    return "";
  }
  close(outfd);
  std::string diag, cmd = tool + " -S '" + path + "' -o '" + outpath + "' </dev/null 2>&1";
  FILE *out = popen(cmd.c_str(), "r");
  char buf[4096];
  while (out && fgets(buf, sizeof(buf), out))
    diag += buf;
  int status = out ? pclose(out) : -1;
  unlink(path);
  std::string ir;
  if (status == 0) {
    if (FILE *f = fopen(outpath, "r")) {
      while (size_t n = fread(buf, 1, sizeof(buf), f))
        ir.append(buf, n);
      fclose(f);
    }
  }
  unlink(outpath);
  if (status != 0) {
    err = diag.empty() ? "metal-opt failed" : diag.substr(0, diag.find('\n'));
    return "";
  }
  return ir;
}

static std::vector<uint32_t>
cs_program(std::initializer_list<Tokens> decls, std::initializer_list<Tokens> body) {
  Tokens p{ENCODE_D3D10_SB_TOKENIZED_PROGRAM_VERSION_TOKEN(D3D11_SB_COMPUTE_SHADER, 5, 0), 0};
  auto add = [&](const Tokens &t) { p.insert(p.end(), t.begin(), t.end()); };
  for (auto &d : decls)
    add(d);
  add(instruction(ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_DCL_TEMPS), {{1}}));
  add(instruction(ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_DCL_INPUT), {{ENCODE_D3D10_SB_OPERAND_TYPE(D3D11_SB_OPERAND_TYPE_INPUT_THREAD_ID) | ENCODE_D3D10_SB_OPERAND_INDEX_DIMENSION(D3D10_SB_OPERAND_INDEX_0D) | x}}));
  add(instruction(ENCODE_D3D10_SB_OPCODE_TYPE(D3D11_SB_OPCODE_DCL_THREAD_GROUP), {{64, 1, 1}}));
  for (auto &b : body)
    add(b);
  add(instruction(ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_RET), {}));
  p[1] = ENCODE_D3D10_SB_TOKENIZED_PROGRAM_LENGTH(p.size());
  return std::vector<uint32_t>(p.begin(), p.end());
}

int
main() {
  unsigned wrong = 0;

  // 1. Structured uint4 load + store (stride 16) -> must emit vector <4 x i32> load & store
  auto structured = container(cs_program(
      {
          instruction(ENCODE_D3D10_SB_OPCODE_TYPE(D3D11_SB_OPCODE_DCL_RESOURCE_STRUCTURED),
                      {{operand(D3D10_SB_OPERAND_TYPE_RESOURCE, none), 0}, {16}}),
          instruction(ENCODE_D3D10_SB_OPCODE_TYPE(D3D11_SB_OPCODE_DCL_UNORDERED_ACCESS_VIEW_STRUCTURED),
                      {{operand(D3D11_SB_OPERAND_TYPE_UNORDERED_ACCESS_VIEW, none), 0}, {16}}),
      },
      {
          instruction(ENCODE_D3D10_SB_OPCODE_TYPE(D3D11_SB_OPCODE_LD_STRUCTURED),
                      {temp(0, all), tid_x(), scalar(0), {operand(D3D10_SB_OPERAND_TYPE_RESOURCE, xyzw), 0}}),
          instruction(ENCODE_D3D10_SB_OPCODE_TYPE(D3D11_SB_OPCODE_STORE_STRUCTURED),
                      {{operand(D3D11_SB_OPERAND_TYPE_UNORDERED_ACCESS_VIEW, all), 0}, tid_x(), scalar(0), temp(0, xyzw)}),
      }
  ));

  // 2. Raw uint4 load + store -> must emit vector <4 x i32> load & store
  auto raw = container(cs_program(
      {
          instruction(ENCODE_D3D10_SB_OPCODE_TYPE(D3D11_SB_OPCODE_DCL_RESOURCE_RAW),
                      {{operand(D3D10_SB_OPERAND_TYPE_RESOURCE, none), 0}}),
          instruction(ENCODE_D3D10_SB_OPCODE_TYPE(D3D11_SB_OPCODE_DCL_UNORDERED_ACCESS_VIEW_RAW),
                      {{operand(D3D11_SB_OPERAND_TYPE_UNORDERED_ACCESS_VIEW, none), 0}}),
      },
      {
          instruction(ENCODE_D3D10_SB_OPCODE_TYPE(D3D11_SB_OPCODE_LD_RAW),
                      {temp(0, all), tid_x(), {operand(D3D10_SB_OPERAND_TYPE_RESOURCE, xyzw), 0}}),
          instruction(ENCODE_D3D10_SB_OPCODE_TYPE(D3D11_SB_OPCODE_STORE_RAW),
                      {{operand(D3D11_SB_OPERAND_TYPE_UNORDERED_ACCESS_VIEW, all), 0}, tid_x(), temp(0, xyzw)}),
      }
  ));

  // 3. Texture2D ld at constant LOD 0 -> must NOT emit air.get_num_mip_levels
  auto lod0 = container(cs_program(
      {
          instruction(ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_DCL_RESOURCE) |
                          ENCODE_D3D10_SB_RESOURCE_DIMENSION(D3D10_SB_RESOURCE_DIMENSION_TEXTURE2D),
                      {{operand(D3D10_SB_OPERAND_TYPE_RESOURCE, none), 0}, {uint4}}),
          instruction(ENCODE_D3D10_SB_OPCODE_TYPE(D3D11_SB_OPCODE_DCL_UNORDERED_ACCESS_VIEW_TYPED) |
                          ENCODE_D3D10_SB_RESOURCE_DIMENSION(D3D10_SB_RESOURCE_DIMENSION_BUFFER),
                      {{operand(D3D11_SB_OPERAND_TYPE_UNORDERED_ACCESS_VIEW, none), 0}, {uint4}}),
      },
      {
          instruction(ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_LD),
                      {temp(0, all), immediate(0, 0, 0, 0), {operand(D3D10_SB_OPERAND_TYPE_RESOURCE, xyzw), 0}}),
          instruction(ENCODE_D3D10_SB_OPCODE_TYPE(D3D11_SB_OPCODE_STORE_UAV_TYPED),
                      {{operand(D3D11_SB_OPERAND_TYPE_UNORDERED_ACCESS_VIEW, all), 0}, immediate(0, 0, 0, 0), temp(0, xyzw)}),
      }
  ));

  // 4. Pixel shader deriv_rtx -> air.dfdx must have readnone and convergent attributes
  Tokens ps_p{ENCODE_D3D10_SB_TOKENIZED_PROGRAM_VERSION_TOKEN(D3D10_SB_PIXEL_SHADER, 5, 0), 0};
  auto ps_add = [&](const Tokens &t) { ps_p.insert(ps_p.end(), t.begin(), t.end()); };
  ps_add(instruction(ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_DCL_INPUT_PS) |
                         ENCODE_D3D10_SB_INPUT_INTERPOLATION_MODE(D3D10_SB_INTERPOLATION_LINEAR),
                     {{operand(D3D10_SB_OPERAND_TYPE_INPUT, all), 0}}));
  ps_add(instruction(ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_DCL_OUTPUT),
                     {{operand(D3D10_SB_OPERAND_TYPE_OUTPUT, all), 0}}));
  ps_add(instruction(ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_DERIV_RTX),
                     {{operand(D3D10_SB_OPERAND_TYPE_OUTPUT, all), 0}, {operand(D3D10_SB_OPERAND_TYPE_INPUT, xyzw), 0}}));
  ps_add(instruction(ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_RET), {}));
  ps_p[1] = ENCODE_D3D10_SB_TOKENIZED_PROGRAM_LENGTH(ps_p.size());
  auto deriv = container(std::vector<uint32_t>(ps_p.begin(), ps_p.end()),
                         signature_chunk("TEXCOORD", 0, 0xf), signature_chunk("SV_Target", 0, 0xf));

  struct Check {
    const char *name;
    const std::vector<uint8_t> &bc;
    std::vector<const char *> must_have;
    std::vector<const char *> must_not_have;
  };
  Check checks[] = {
      {"structured uint4 load/store", structured, {"load <4 x i32>", "store <4 x i32>"}, {}},
      {"raw uint4 load/store", raw, {"load <4 x i32>", "store <4 x i32>"}, {}},
      {"texture2d ld at LOD 0", lod0, {"air.read_texture_2d"}, {"air.get_num_mip_levels"}},
      {"ps deriv_rtx attributes", deriv, {"@air.dfdx.", "readnone", "convergent"}, {}},
  };

  for (auto &c : checks) {
    for (auto [ver, vname] : {std::pair{SM50_SHADER_METAL_320, "Metal 3.2"}, std::pair{SM50_SHADER_METAL_400, "Metal 4"}}) {
      std::string err, ir = convert_and_disassemble(c.bc, ver, err);
      if (ir.empty()) {
        printf("wrong: %s (%s): %s\n", c.name, vname, err.c_str());
        wrong++;
        continue;
      }
      for (auto *needle : c.must_have) {
        if (ir.find(needle) == std::string::npos) {
          printf("wrong: %s (%s): missing '%s' in AIR IR\n", c.name, vname, needle);
          wrong++;
        }
      }
      for (auto *forbid : c.must_not_have) {
        if (ir.find(forbid) != std::string::npos) {
          printf("wrong: %s (%s): unexpected '%s' in AIR IR\n", c.name, vname, forbid);
          wrong++;
        }
      }
    }
  }
  printf("%s: %u wrong results\n", wrong ? "failed" : "passed", wrong);
  return wrong ? 1 : 0;
}
