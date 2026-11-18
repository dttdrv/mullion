// contract: every texture read and texture atomic of a converted shader has the arguments of its module's AIR version,
// as Apple's verifier (the Metal toolchain's metal-opt) checks a module. Metal 4's AIR (2.8) gives a read the read
// sampler (air.get_read_sampler) after the texture, and a read or atomic of a 2D, 2D array or 3D kind, depth ones too,
// an offset after the position and array index; Metal 3.2's (2.7) has neither (Apple's compiler's output for each
// kind and version). each case is a cs_5_0 kernel of its own, written here from the tokenized program format: a load
// of each kind of resource (ld, ldms, ld_uav_typed, ld_uav_typed of a globally coherent UAV, and ld of a texture a
// comparison sample makes a depth texture), an atomic add on each kind of typed UAV, and a compare-exchange; each is
// converted for Metal 3.2 and for Metal 4, must hold the access it is for, and must pass the verifier
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

// the token macros give int
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
// l(a, b, c, d) and l(a)
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
static const long long uint4 = ENCODE_D3D10_SB_RESOURCE_RETURN_TYPE(D3D10_SB_RETURN_TYPE_UINT, 0) |
                               ENCODE_D3D10_SB_RESOURCE_RETURN_TYPE(D3D10_SB_RETURN_TYPE_UINT, 1) |
                               ENCODE_D3D10_SB_RESOURCE_RETURN_TYPE(D3D10_SB_RETURN_TYPE_UINT, 2) |
                               ENCODE_D3D10_SB_RESOURCE_RETURN_TYPE(D3D10_SB_RETURN_TYPE_UINT, 3);
static const long long float4 = ENCODE_D3D10_SB_RESOURCE_RETURN_TYPE(D3D10_SB_RETURN_TYPE_FLOAT, 0) |
                                ENCODE_D3D10_SB_RESOURCE_RETURN_TYPE(D3D10_SB_RETURN_TYPE_FLOAT, 1) |
                                ENCODE_D3D10_SB_RESOURCE_RETURN_TYPE(D3D10_SB_RETURN_TYPE_FLOAT, 2) |
                                ENCODE_D3D10_SB_RESOURCE_RETURN_TYPE(D3D10_SB_RETURN_TYPE_FLOAT, 3);

// an instruction: its opcode token, with its length, then its operands
static Tokens
instruction(long long opcode, std::initializer_list<Tokens> operands) {
  Tokens out{opcode};
  for (auto &o : operands)
    out.insert(out.end(), o.begin(), o.end());
  out[0] |= ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(out.size());
  return out;
}

// a kernel of one thread: r0 and r1, u7 a uint buffer the loaded values are stored to, `declarations` and `body`
static std::vector<uint32_t>
kernel(std::initializer_list<Tokens> declarations, std::initializer_list<Tokens> body) {
  Tokens program{ENCODE_D3D10_SB_TOKENIZED_PROGRAM_VERSION_TOKEN(D3D11_SB_COMPUTE_SHADER, 5, 0), 0};
  auto add = [&](const Tokens &t) { program.insert(program.end(), t.begin(), t.end()); };
  add(instruction(
      ENCODE_D3D10_SB_OPCODE_TYPE(D3D11_SB_OPCODE_DCL_UNORDERED_ACCESS_VIEW_TYPED) |
          ENCODE_D3D10_SB_RESOURCE_DIMENSION(D3D10_SB_RESOURCE_DIMENSION_BUFFER),
      {{operand(D3D11_SB_OPERAND_TYPE_UNORDERED_ACCESS_VIEW, none), 7}, {uint4}}
  ));
  for (auto &d : declarations)
    add(d);
  add(instruction(ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_DCL_TEMPS), {{2}}));
  add(instruction(ENCODE_D3D10_SB_OPCODE_TYPE(D3D11_SB_OPCODE_DCL_THREAD_GROUP), {{1, 1, 1}}));
  for (auto &b : body)
    add(b);
  add(instruction(ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_RET), {}));
  program[1] = ENCODE_D3D10_SB_TOKENIZED_PROGRAM_LENGTH(program.size());
  return std::vector<uint32_t>(program.begin(), program.end());
}

static Tokens
temp(unsigned i, long long selection) {
  return {operand(D3D10_SB_OPERAND_TYPE_TEMP, selection), i};
}
static Tokens
srv(long long selection) {
  return {operand(D3D10_SB_OPERAND_TYPE_RESOURCE, selection), 0};
}
static Tokens
uav(long long selection) {
  return {operand(D3D11_SB_OPERAND_TYPE_UNORDERED_ACCESS_VIEW, selection), 0};
}
static Tokens
dcl_srv(D3D10_SB_RESOURCE_DIMENSION dimension, unsigned samples, long long returns = uint4) {
  return instruction(
      ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_DCL_RESOURCE) | ENCODE_D3D10_SB_RESOURCE_DIMENSION(dimension) |
          ENCODE_D3D10_SB_RESOURCE_SAMPLE_COUNT(samples),
      {srv(none), {returns}}
  );
}
static Tokens
dcl_uav(D3D10_SB_RESOURCE_DIMENSION dimension, long long flags = 0) {
  return instruction(
      ENCODE_D3D10_SB_OPCODE_TYPE(D3D11_SB_OPCODE_DCL_UNORDERED_ACCESS_VIEW_TYPED) |
          ENCODE_D3D10_SB_RESOURCE_DIMENSION(dimension) | ENCODE_D3D11_SB_RESOURCE_FLAGS(flags),
      {uav(none), {uint4}}
  );
}
// store_uav_typed u7.xyzw, l(i, 0, 0, 0), r<i>.xyzw: keeps what r<i> holds
static Tokens
keep(unsigned i) {
  return instruction(
      ENCODE_D3D10_SB_OPCODE_TYPE(D3D11_SB_OPCODE_STORE_UAV_TYPED),
      {{operand(D3D11_SB_OPERAND_TYPE_UNORDERED_ACCESS_VIEW, all), 7}, immediate(i, 0, 0, 0), temp(i, xyzw)}
  );
}
static const auto origin = immediate(0, 0, 0, 0);

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

// the module's bitcode, out of the library its bitcode wrapper (0x0B17C0DE: magic, version, offset, size, cputype)
// places it in; empty when the kernel does not convert
static std::string
convert(const std::vector<uint8_t> &bytecode, SM50_SHADER_METAL_VERSION version, std::string &message) {
  sm50_shader_t shader = nullptr;
  sm50_error_t error = nullptr;
  char text[512] = "";
  std::string library;
  if (SM50Initialize(bytecode.data(), bytecode.size(), &shader, nullptr, &error)) {
    SM50GetErrorMessage(error, text, sizeof(text));
    SM50FreeError(error);
    message = std::string("does not parse: ") + text;
    return "";
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
    SM50GetErrorMessage(error, text, sizeof(text));
    SM50FreeError(error);
    message = std::string("does not convert: ") + text;
  }
  SM50Destroy(shader);
  auto wrapper = library.find("\xde\xc0\x17\x0b");
  uint32_t header[5];
  if (wrapper == std::string::npos || wrapper + sizeof(header) > library.size()) {
    if (message.empty())
      message = "converts to a library without a bitcode wrapper";
    return "";
  }
  memcpy(header, library.data() + wrapper, sizeof(header));
  return library.substr(wrapper + header[2], header[3]);
}

// what Apple's verifier says of the module: empty when it takes it
static std::string
verify(const std::string &module) {
  char path[] = "/tmp/texel_forms.XXXXXX";
  int fd = mkstemp(path);
  if (fd < 0 || write(fd, module.data(), module.size()) != ssize_t(module.size()))
    return "the module could not be written for the verifier";
  close(fd);
  std::string said, command = std::string("xcrun -sdk macosx metal-opt -S '") + path + "' -o /dev/null 2>&1";
  FILE *out = popen(command.c_str(), "r");
  char line[512];
  while (out && fgets(line, sizeof(line), out))
    said += line;
  int status = out ? pclose(out) : -1;
  unlink(path);
  if (!status)
    return "";
  // the verifier names the module's file, which is gone
  return said.empty() ? "the verifier did not run (xcrun -sdk macosx metal-opt)" : said.substr(0, said.find('\n'));
}

int
main() {
  struct Case {
    std::string name;
    std::vector<uint32_t> program;
    const char *access; // the AIR function the case must call
  };
  std::vector<Case> cases;
  const struct {
    const char *name;
    D3D10_SB_RESOURCE_DIMENSION dimension;
  } kinds[] = {
      {"buffer", D3D10_SB_RESOURCE_DIMENSION_BUFFER},           {"1D", D3D10_SB_RESOURCE_DIMENSION_TEXTURE1D},
      {"1D array", D3D10_SB_RESOURCE_DIMENSION_TEXTURE1DARRAY}, {"2D", D3D10_SB_RESOURCE_DIMENSION_TEXTURE2D},
      {"2D array", D3D10_SB_RESOURCE_DIMENSION_TEXTURE2DARRAY}, {"3D", D3D10_SB_RESOURCE_DIMENSION_TEXTURE3D},
  };
  for (auto &kind : kinds) {
    // ld r0.xyzw, l(0, 0, 0, 0), t0.xyzw
    cases.push_back(
        {std::string("ld of a ") + kind.name + " SRV",
         kernel(
             {dcl_srv(kind.dimension, 0)},
             {instruction(ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_LD), {temp(0, all), origin, srv(xyzw)}), keep(0)}
         ),
         "air.read_"}
    );
    // ld_uav_typed r0.xyzw, l(0, 0, 0, 0), u0.xyzw
    cases.push_back(
        {std::string("ld_uav_typed of a ") + kind.name + " UAV",
         kernel(
             {dcl_uav(kind.dimension)},
             {instruction(ENCODE_D3D10_SB_OPCODE_TYPE(D3D11_SB_OPCODE_LD_UAV_TYPED), {temp(0, all), origin, uav(xyzw)}),
              keep(0)}
         ),
         "air.read_"}
    );
    // atomic_iadd u0.x, l(0, 0, 0, 0), l(1)
    cases.push_back(
        {std::string("atomic_iadd on a ") + kind.name + " UAV",
         kernel(
             {dcl_uav(kind.dimension)},
             {instruction(ENCODE_D3D10_SB_OPCODE_TYPE(D3D11_SB_OPCODE_ATOMIC_IADD), {uav(x), origin, scalar(1)})}
         ),
         "air.atomic_fetch_add_explicit_"}
    );
  }
  for (auto dimension : {D3D10_SB_RESOURCE_DIMENSION_TEXTURE2DMS, D3D10_SB_RESOURCE_DIMENSION_TEXTURE2DMSARRAY})
    // ldms r0.xyzw, l(0, 0, 0, 0), t0.xyzw, l(1)
    cases.push_back(
        {dimension == D3D10_SB_RESOURCE_DIMENSION_TEXTURE2DMS ? "ldms of a 2D multisampled SRV"
                                                               : "ldms of a 2D multisampled array SRV",
         kernel(
             {dcl_srv(dimension, 4)},
             {instruction(ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_LD_MS), {temp(0, all), origin, srv(xyzw), scalar(1)}),
              keep(0)}
         ),
         "air.read_"}
    );
  for (auto dimension : {D3D10_SB_RESOURCE_DIMENSION_TEXTURE2D, D3D10_SB_RESOURCE_DIMENSION_TEXTURE2DARRAY}) {
    // a float SRV, dcl_sampler s0, mode_comparison; sample_c_lz r1.x, l(0.5, 0.5, 0, 0), t0.xyzw, s0, l(0.5); then
    // the ld of a texture a comparison makes a depth texture
    float half = 0.5f;
    uint32_t h;
    memcpy(&h, &half, sizeof(h));
    cases.push_back(
        {dimension == D3D10_SB_RESOURCE_DIMENSION_TEXTURE2D ? "ld of a 2D SRV that is compared"
                                                             : "ld of a 2D array SRV that is compared",
         kernel(
             {dcl_srv(dimension, 0, float4),
              instruction(
                  ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_DCL_SAMPLER) |
                      ENCODE_D3D10_SB_SAMPLER_MODE(D3D10_SB_SAMPLER_MODE_COMPARISON),
                  {{operand(D3D10_SB_OPERAND_TYPE_SAMPLER, none), 0}}
              )},
             {instruction(
                  ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_SAMPLE_C_LZ),
                  {temp(1, x), immediate(h, h, 0, 0), srv(xyzw), {operand(D3D10_SB_OPERAND_TYPE_SAMPLER, none), 0},
                   scalar(h)}
              ),
              instruction(ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_LD), {temp(0, all), origin, srv(xyzw)}), keep(0),
              keep(1)}
         ),
         "air.read_depth_"}
    );
  }
  // dcl_uav_typed_texture2d (uint, uint, uint, uint), globallycoherent u0; ld_uav_typed r0.xyzw, l(0, 0, 0, 0), u0.xyzw
  cases.push_back(
      {"ld_uav_typed of a globally coherent 2D UAV",
       kernel(
           {dcl_uav(D3D10_SB_RESOURCE_DIMENSION_TEXTURE2D, D3D11_SB_GLOBALLY_COHERENT_ACCESS)},
           {instruction(ENCODE_D3D10_SB_OPCODE_TYPE(D3D11_SB_OPCODE_LD_UAV_TYPED), {temp(0, all), origin, uav(xyzw)}),
            keep(0)}
       ),
       "air.read_texture_2d.device_coherent"}
  );
  // imm_atomic_cmp_exch r0.x, u0.x, l(0, 0, 0, 0), l(0), l(1)
  cases.push_back(
      {"imm_atomic_cmp_exch on a 2D array UAV",
       kernel(
           {dcl_uav(D3D10_SB_RESOURCE_DIMENSION_TEXTURE2DARRAY)},
           {instruction(
                ENCODE_D3D10_SB_OPCODE_TYPE(D3D11_SB_OPCODE_IMM_ATOMIC_CMP_EXCH),
                {temp(0, x), uav(x), origin, scalar(0), scalar(1)}
            ),
            keep(0)}
       ),
       "air.atomic_compare_exchange_weak_explicit_"}
  );

  unsigned wrong = 0;
  for (auto &c : cases)
    for (auto [version, named] : {std::pair{SM50_SHADER_METAL_320, "Metal 3.2"}, std::pair{SM50_SHADER_METAL_400, "Metal 4"}}) {
      std::string message, module = convert(container(c.program), version, message);
      if (module.empty()) {
        printf("wrong: %s for %s %s\n", c.name.c_str(), named, message.c_str());
        wrong++;
      } else if (module.find(c.access) == std::string::npos) {
        printf("wrong: %s for %s converts to no %s call\n", c.name.c_str(), named, c.access);
        wrong++;
      } else if (auto said = verify(module); !said.empty()) {
        printf("wrong: %s for %s: %s\n", c.name.c_str(), named, said.c_str());
        wrong++;
      }
    }
  printf("fact: %zu kernels, each for Metal 3.2 and Metal 4\n", cases.size());
  printf("%s: %u wrong results\n", wrong ? "failed" : "passed", wrong);
  return wrong ? 1 : 0;
}
