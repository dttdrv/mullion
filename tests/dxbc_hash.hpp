// the hash a shader container carries in the 16 bytes after its name: the tests' own, so that a container a test
// compiles or changes can be given the hash Direct3D checks. written from the algorithm as Microsoft publishes it
// (HLSL specification INF-0004, Validator Hashing, appendix 1, "Retail Hash Diffs"; DirectXShaderCompiler,
// lib/DxilHash/DxilHash.cpp, ComputeHashRetail): MD5's rounds (RFC 1321) over the container from its version on,
// with an ending of its own: the bytes that do not fill a block are followed by 0x80 and zeros, and the last block
// begins with the number of bits hashed ("x[0] = byteCount << 3") and ends with "1 | (byteCount << 1)".
// tests/host/dxbchash.cpp holds it to the hashes that Microsoft's compilers wrote into real shaders.
#pragma once
#include <array>
#include <cstdint>
#include <cstring>

namespace dxbc {
// where the hash lies, and where what it is of begins
constexpr size_t hash_at = 4, hashed_from = 20;
using Hash = std::array<uint8_t, 16>;

// one block of 64 bytes into MD5's state (RFC 1321, 3.4)
inline void
md5_block(uint32_t state[4], const uint8_t *block) {
  static const uint32_t shifts[4][4] = {{7, 12, 17, 22}, {5, 9, 14, 20}, {4, 11, 16, 23}, {6, 10, 15, 21}};
  // floor(2^32 * abs(sin(i + 1)))
  static const uint32_t sines[64] = {
      0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1,
      0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453,
      0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942,
      0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
      0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d,
      0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
  uint32_t words[16], a = state[0], b = state[1], c = state[2], d = state[3];
  memcpy(words, block, sizeof(words));
  for (uint32_t i = 0; i < 64; i++) {
    const uint32_t round = i / 16;
    uint32_t mixed, word;
    if (round == 0)
      mixed = (b & c) | (~b & d), word = i;
    else if (round == 1)
      mixed = (d & b) | (~d & c), word = (5 * i + 1) % 16;
    else if (round == 2)
      mixed = b ^ c ^ d, word = (3 * i + 5) % 16;
    else
      mixed = c ^ (b | ~d), word = 7 * i % 16;
    const uint32_t sum = a + mixed + sines[i] + words[word], shift = shifts[round][i % 4];
    a = d, d = c, c = b;
    b += sum << shift | sum >> (32 - shift);
  }
  state[0] += a, state[1] += b, state[2] += c, state[3] += d;
}

// the hash of a container of `size` bytes; one too short to have anything after its hash has the hash of nothing
inline Hash
hash(const void *container, size_t size) {
  uint32_t state[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
  auto bytes = static_cast<const uint8_t *>(container) + hashed_from;
  const size_t length = size > hashed_from ? size - hashed_from : 0, rest = length % 64;
  for (size_t at = 0; at + 64 <= length; at += 64)
    md5_block(state, bytes + at);
  const uint32_t bits = uint32_t(length * 8), ending = bits >> 2 | 1;
  uint8_t last[2][64] = {};
  // the bit count wants four bytes before the rest and the ending four after its 0x80: with less room the rest
  // has a block of its own
  const bool alone = rest >= 56;
  uint8_t *block = last[alone];
  if (alone) {
    memcpy(last[0], bytes + length - rest, rest);
    last[0][rest] = 0x80;
    md5_block(state, last[0]);
    memcpy(block, &bits, sizeof(bits));
  } else {
    memcpy(block, &bits, sizeof(bits));
    memcpy(block + sizeof(bits), bytes + length - rest, rest);
    block[sizeof(bits) + rest] = 0x80;
  }
  memcpy(block + 60, &ending, sizeof(ending));
  md5_block(state, block);
  Hash out;
  memcpy(out.data(), state, sizeof(state));
  return out;
}

// writes a container's hash into it
template <typename Bytes>
void
sign(Bytes &container) {
  if (container.size() >= hashed_from)
    memcpy(container.data() + hash_at, hash(container.data(), container.size()).data(), sizeof(Hash));
}
} // namespace dxbc
