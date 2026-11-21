// contract: Mullion's SHA-1 (src/util/sha1) gives FIPS 180's digests, and the same digest whichever path compresses
// a message's blocks. An Arm build compresses runs of whole 64-byte blocks with the ARMv8 SHA-1 instructions and the
// rest with SHA1Transform: an update of fewer than 64 bytes never reaches the block loop, so feeding a message in such
// pieces digests it all with SHA1Transform, and feeding it whole digests its blocks on the instructions. Every digest
// Mullion keys a shader or pipeline by would otherwise change between builds.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "sha1/sha1.h"
}

static std::string
digest(const uint8_t *data, size_t length, size_t piece) {
  SHA1_CTX context;
  SHA1Init(&context);
  for (size_t i = 0; i < length; i += piece)
    SHA1Update(&context, data + i, length - i < piece ? length - i : piece);
  uint8_t out[SHA1_DIGEST_LENGTH];
  SHA1Final(out, &context);
  char text[2 * SHA1_DIGEST_LENGTH + 1];
  for (int i = 0; i < SHA1_DIGEST_LENGTH; i++)
    snprintf(text + 2 * i, 3, "%02x", out[i]);
  return text;
}

int
main() {
  int wrong = 0;
  auto expect = [&](bool ok, const char *what) {
    if (!ok && wrong++ < 8)
      printf("wrong: %s\n", what);
  };
  // FIPS 180-2 appendix A's three messages
  const std::string abc = "abc", two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", million(1000000, 'a');
  for (size_t piece : {size_t(1), size_t(63), size_t(1) << 30}) {
    expect(digest((const uint8_t *)abc.data(), abc.size(), piece) == "a9993e364706816aba3e25717850c26c9cd0d89d", "\"abc\"");
    expect(digest((const uint8_t *)two.data(), two.size(), piece) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1",
           "the 448-bit message");
    expect(digest((const uint8_t *)million.data(), million.size(), piece) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f",
           "a million 'a's");
  }
  // random messages, whole against in pieces of fewer than 64 bytes
  std::vector<uint8_t> bytes(1 << 20);
  uint32_t x = 777;
  for (auto &b : bytes) {
    x = x * 1664525 + 1013904223;
    b = uint8_t(x >> 24);
  }
  for (int i = 0; i < 400; i++) {
    x = x * 1664525 + 1013904223;
    size_t offset = (x >> 4) % 17, length = x % (bytes.size() - offset), piece = 1 + (x >> 9) % 63;
    expect(digest(&bytes[offset], length, length + 1) == digest(&bytes[offset], length, piece),
           "a random message whole against in pieces");
  }
  printf("%s: %d wrong results\n", wrong ? "failed" : "passed", wrong);
  return wrong ? 1 : 0;
}
