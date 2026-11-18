// contract: SM50Initialize shares one parse among equal bytecodes, and a parse holds what it read of a shader, not the
// bytecode. pipelines made of the same shader get the same conversion state while one holds it (SM50Initialize's
// cache), and bytecodes that differ in any byte get their own. a container's private data (the PRIV part,
// D3D_BLOB_PRIVATE_DATA) is the application's, which no conversion reads: a shader that carries megabytes of it adds
// a parse's few kilobytes to the converter's memory, not the bytecode (Mullion kept a copy of each live shader's
// bytecode, which doubled what GoW Ragnarok's 33,940 shaders held). destroying every reference gives it all back.
#include "airconv_public.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <malloc/malloc.h>
#include <algorithm>
#include <set>
#include <string>
#include <vector>

static uint64_t
in_use() {
  malloc_statistics_t stats;
  malloc_zone_statistics(nullptr, &stats);
  return stats.size_in_use;
}

static void
put(std::string &bytes, uint32_t value) {
  bytes.append(reinterpret_cast<const char *>(&value), sizeof(value));
}

// a container (DxilContainer.h: magic, a hash no conversion reads, version 1, its size, the parts' offsets) of a pixel
// shader 5.0 that returns, with empty signatures and `private_bytes` of private data, each byte `seed` plus its place
static std::string
container(size_t private_bytes, uint8_t seed) {
  std::vector<std::string> parts;
  for (const char *signature : {"ISGN", "OSGN"}) {
    std::string part(signature, 4);
    put(part, 8), put(part, 0), put(part, 8);
    parts.push_back(part);
  }
  std::string shex("SHEX", 4);
  // the version token (D3D10_SB_PIXEL_SHADER is 0, model 5.0), the length in tokens, then ret
  put(shex, 12), put(shex, 0x00000050), put(shex, 3), put(shex, 0x0100003e);
  parts.push_back(shex);
  std::string data("PRIV", 4);
  put(data, uint32_t(private_bytes));
  for (size_t i = 0; i < private_bytes; i++)
    data.push_back(char(uint8_t(seed + i * 131)));
  parts.push_back(data);
  std::string bytes("DXBC", 4);
  bytes.append(16, '\0');
  put(bytes, 1);
  size_t header = bytes.size() + 8 + 4 * parts.size(), size = header;
  for (auto &part : parts)
    size += part.size();
  put(bytes, uint32_t(size)), put(bytes, uint32_t(parts.size()));
  for (size_t at = header; auto &part : parts)
    put(bytes, uint32_t(at)), at += part.size();
  for (auto &part : parts)
    bytes += part;
  return bytes;
}

static sm50_shader_t
initialize(const std::string &bytes) {
  sm50_shader_t shader = nullptr;
  MTL_SHADER_REFLECTION reflection{};
  sm50_error_t error = nullptr;
  if (SM50Initialize(bytes.data(), bytes.size(), &shader, &reflection, &error)) {
    if (error)
      SM50FreeError(error);
    return nullptr;
  }
  return shader;
}

int
main() {
  unsigned wrong = 0;
  auto expect = [&](bool held, const char *what) {
    if (!held) {
      printf("wrong: %s\n", what);
      wrong++;
    }
  };
  // shaders of 256 KB of private data each, which malloc gives from its regions as any parse's memory, not from its
  // cache of large blocks: that one counts a freed block as in use until it lets it go, which hid a copy of 4 MB
  constexpr size_t kPrivate = 256 << 10, kShaders = 64;
  // warm the converter's own first-use allocations, so that what follows counts only the shaders
  if (auto shader = initialize(container(16, 0)))
    SM50Destroy(shader);
  else
    return printf("wrong: a shader that returns was not initialized\n"), 1;

  std::vector<std::string> codes;
  for (size_t i = 0; i < kShaders; i++)
    codes.push_back(container(kPrivate, uint8_t(i + 1)));
  auto same = codes[0];
  expect(codes[0].size() == codes[1].size() && codes[0] != codes[1], "two containers are not of one size and other bytes");
  std::vector<sm50_shader_t> shaders;
  uint64_t before = in_use();
  for (auto &code : codes)
    shaders.push_back(initialize(code));
  uint64_t kept = in_use() - before;
  auto again = initialize(same);
  expect(std::find(shaders.begin(), shaders.end(), nullptr) == shaders.end() && again, "a container was not initialized");
  expect(again == shaders[0], "equal bytecodes in two buffers have two parses");
  expect(std::set(shaders.begin(), shaders.end()).size() == kShaders, "bytecodes that differ share a parse");
  // the private data is many times what a parse of `ret` takes: a copy of the bytecode is most of what is kept
  printf("kept %llu KB for %zu containers of %zu KB\n", (unsigned long long)kept >> 10, kShaders, codes[0].size() >> 10);
  expect(kept < kShaders * kPrivate / 4, "a parse keeps the bytecode");
  shaders.push_back(again);
  for (auto shader : shaders)
    if (shader)
      SM50Destroy(shader);
  // a destroyed parse is not handed out again: the same bytes parse anew, with what they hold given back after
  uint64_t idle = in_use();
  auto anew = initialize(codes[0]);
  expect(anew, "a container was not initialized after its parse was destroyed");
  if (anew)
    SM50Destroy(anew);
  expect(in_use() <= idle + (64 << 10), "a destroyed parse's memory stays");
  if (!wrong)
    printf("passed\n");
  return wrong ? 1 : 0;
}
