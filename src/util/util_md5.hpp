/*
 * Original source:
 * https://github.com/doitsujin/dxbc-spirv/blob/main/util/util_md5.h
 * MIT License, Copyright (c) 2025 Philip Rebohle
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dxmt::md5 {

/** 128-bit MD5 digest */
struct Digest {
  std::array<uint8_t, 16u> data = {};

  bool operator==(const Digest &other) const {
    return !std::memcmp(data.data(), other.data.data(), sizeof(data));
  }
  bool operator!=(const Digest &other) const {
    return std::memcmp(data.data(), other.data.data(), sizeof(data));
  }
};

/** MD5 hash state */
class Hasher {
  constexpr static size_t BlockSize = 64u;

public:
  Hasher() = default;

  /** Processes data to hash */
  void update(const void *data, size_t size);

  /** Finalizes hash and computes digest. The hasher
   *  will be returned to its default state after. */
  Digest finalize();

  /** Retrieves current digest without finalizing the
   *  stream properly. */
  Digest getDigest();

  /** Convenience method to compute a hash for a simple
   *  binary blob of data in memory. */
  static Digest compute(const void *data, size_t size);

private:
  std::array<uint8_t, BlockSize> m_block = {};
  uint64_t m_size = 0u;

  std::array<uint32_t, 4u> m_state = {
      0x67452301u,
      0xefcdab89u,
      0x98badcfeu,
      0x10325476u,
  };

  void processBlock(const unsigned char *data);

  void padBlock();

  void reset();

  static uint32_t readDword(const unsigned char *src);
};

Digest hashDxbcBinary(const void *data, size_t size);

// what the sixteen bytes after a container's name say of it (HLSL specifications, INF-0004 Validator Hashing): the
// hash of its contents, or BYPASS, sixteen bytes of 1, which stands for it; no hash, which is zeros, as a compiler
// without the validator leaves them, or PREVIEW_BYPASS, sixteen bytes of 2: Direct3D takes those only with
// experimental shader models switched on; or neither, a container that is not what a compiler wrote
enum class DxbcHash { Wrong, None, Holds };
DxbcHash checkDxbcHash(const void *data, size_t size);

} // namespace dxmt::md5
