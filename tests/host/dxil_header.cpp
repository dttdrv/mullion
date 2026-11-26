// contract: a container's DXIL part is read only within its bytes. the part starts with DxilProgramHeader (a version
// word with the shader kind, the program's size in 32-bit words) and DxilBitcodeHeader in it (the magic 'DXIL', a
// version, the bitcode's offset from that header and its size), and DirectXShaderCompiler takes one only when the part
// holds both headers and the program, the magic is 'DXIL' and the bitcode is nonempty and within the part
// (DxilContainer.h, IsValidDxilProgramHeader and IsValidDxilBitcodeHeader); Metal Shader Converter checks the magic
// before it parses. SM50Initialize must refuse each part that fails one of
// those, with an error and without reading past the container: each container here ends where a page the process
// may not read begins, so a read past it ends the test
#include "DXBCParser/BlobContainer.h"
#include "airconv_public.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

using namespace microsoft;

// a container of an empty input signature, an empty output signature and the DXIL part `dxil`
static std::vector<uint8_t>
container(const std::vector<uint32_t> &dxil) {
  const uint32_t empty[2] = {0, 8};
  struct Blob {
    DXBCFourCC fourcc;
    const void *data;
    uint32_t size;
  } blobs[] = {
      {DXBC_InputSignature, empty, sizeof(empty)},
      {DXBC_OutputSignature, empty, sizeof(empty)},
      {DXBC_DXIL, dxil.data(), uint32_t(dxil.size() * sizeof(uint32_t))},
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

int
main() {
  // a vs_6_0's version word, 'DXIL' and DxilBitcodeHeader's version (1.0)
  constexpr uint32_t vs_6_0 = 1u << 16 | 0x60, magic = 0x4c495844, version = 0x100;
  const struct {
    const char *name;
    std::vector<uint32_t> part;
  } cases[] = {
      {"an empty part", {}},
      {"a part of the program header alone", {vs_6_0, 2}},
      {"a part that ends in the bitcode header", {vs_6_0, 4, magic, version}},
      {"a part without the DXIL magic", {vs_6_0, 7, 0x43425844, version, 16, 4, 0x0b17c0de}},
      {"a program larger than its part", {vs_6_0, 64, magic, version, 16, 4, 0x0b17c0de}},
      {"no bitcode", {vs_6_0, 6, magic, version, 16, 0}},
      {"bitcode past the part", {vs_6_0, 7, magic, version, 16, 4096, 0x0b17c0de}},
      {"bitcode that starts past the part", {vs_6_0, 7, magic, version, 0x10000000, 4, 0x0b17c0de}},
      {"a bitcode offset and size whose sum wraps", {vs_6_0, 7, magic, version, 0xfffffffc, 8, 0x0b17c0de}},
  };
  // what each case said shows before a read past it ends the test
  setvbuf(stdout, nullptr, _IONBF, 0);
  long page = sysconf(_SC_PAGESIZE);
  auto pages = (uint8_t *)mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (pages == MAP_FAILED || mprotect(pages + page, page, PROT_NONE)) {
    printf("failed: no guard page\n");
    return 1;
  }
  unsigned wrong = 0;
  for (auto &c : cases) {
    auto bytes = container(c.part);
    if (bytes.size() > size_t(page)) {
      printf("wrong: %s does not fit a page\n", c.name);
      wrong++;
      continue;
    }
    auto at = pages + page - bytes.size();
    memcpy(at, bytes.data(), bytes.size());
    sm50_shader_t shader = nullptr;
    sm50_error_t error = nullptr;
    if (!SM50Initialize(at, bytes.size(), &shader, nullptr, &error)) {
      printf("wrong: %s is taken\n", c.name);
      SM50Destroy(shader);
      wrong++;
      continue;
    }
    char message[512] = "";
    if (!error || !SM50GetErrorMessage(error, message, sizeof(message))) {
      printf("wrong: %s is refused without an error\n", c.name);
      wrong++;
    } else {
      printf("%s: %s\n", c.name, message);
    }
    if (error)
      SM50FreeError(error);
  }
  printf("%s: %u wrong results\n", wrong ? "failed" : "passed", wrong);
  return wrong ? 1 : 0;
}
