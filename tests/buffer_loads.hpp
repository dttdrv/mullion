// D3D11.3 22.4.10: "Out of bounds addressing on u#/t# of any given 32-bit component returns 0 for that component."
// 22.4.12 gives the same result for an out-of-range structure index; offsets within a structure stay in range here.
// raw component offsets follow the uint32_t oracle in vkd3d-proton's test_byte_buffer_addressing_wrap.
#pragma once
#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <random>

namespace buffer_loads {
constexpr UINT components = 4; // the instruction's component window (D3D11.3 22.4.10 and 22.4.12)
constexpr UINT kinds = 8;
using Address = std::array<UINT, 2>;
using Result = std::array<UINT, 2>;

static const char hlsl[] = R"hlsl(
struct Record { VALUE value;
#if PADDING
  WORD tail[PADDING];
#endif
};
ByteAddressBuffer raw_srv : register(t0);
StructuredBuffer<Record> structured_srv : register(t1);
StructuredBuffer<uint2> addresses : register(t2);
RWByteAddressBuffer raw_uav : register(u0);
RWStructuredBuffer<Record> structured_uav : register(u1);
globallycoherent RWByteAddressBuffer coherent_raw : register(u2);
globallycoherent RWStructuredBuffer<Record> coherent_structured : register(u3);
RWStructuredBuffer<uint2> results : register(u4);
groupshared WORD shared_values[CASES];
#if BITS == 32
#define RAW_LOAD(buf, at) buf.LOAD(at)
#else
#define RAW_LOAD(buf, at) buf.Load<VALUE>(at)
#endif
void save(VALUE value, uint row) {
  for (uint c = 0; c < WIDTH; c++) {
#if WIDTH == 1
    WORD scalar = value;
#else
    WORD scalar = value[c];
#endif
#if BITS == 64
    results[row * WIDTH + c] = uint2((uint)scalar, (uint)(scalar >> 32));
#else
    results[row * WIDTH + c] = uint2((uint)scalar, 0);
#endif
  }
}
[numthreads(CASES, 1, 1)] void cs(uint id : SV_GroupIndex) {
  shared_values[id] = (WORD)(id + 1);
  GroupMemoryBarrierWithGroupSync();
  uint2 at = addresses[id];
  uint offset = at.x * WORD_BYTES;
  uint row = id * KINDS;
  save(RAW_LOAD(raw_srv, offset), row);
  save(structured_srv[at.y].value, row + 1);
  save(RAW_LOAD(raw_uav, offset), row + 2);
  save(structured_uav[at.y].value, row + 3);
  save(RAW_LOAD(coherent_raw, offset), row + 4);
  save(coherent_structured[at.y].value, row + 5);
  save((VALUE)shared_values[(id + 1) % CASES], row + 6);
#if BITS == 32 && WIDTH == 4
  save(uint4(raw_srv.Load3(offset + COMPONENTS * WORD_BYTES), 0), row + KINDS - 1);
#else
  save(RAW_LOAD(raw_srv, offset + COMPONENTS * WORD_BYTES), row + KINDS - 1);
#endif
}
)hlsl";

struct Case {
  UINT seed, bits, width, padding, word_bytes, stride, first, count, raw_first, raw_bytes;
  std::vector<uint8_t> data;
  std::vector<Address> addresses;
  std::vector<Result> expected;

  Case(UINT seed, UINT bits, UINT width, UINT padding, UINT raw_alignment)
      : seed(seed), bits(bits), width(width), padding(padding) {
    std::minstd_rand random(seed);
    word_bytes = bits / CHAR_BIT;
    stride = (width + padding) * word_bytes;
    first = 1 + random() % components;
    count = components + random() % (2 * components + 1);
    raw_first = (1 + random() % components) * raw_alignment / sizeof(UINT);
    raw_bytes = count * stride / sizeof(UINT) * sizeof(UINT);
    if (word_bytes > sizeof(UINT))
      raw_bytes -= sizeof(UINT);
    data.resize((std::max<size_t>((first + count + components) * stride, raw_first * sizeof(UINT) + raw_bytes) +
                 stride - 1) / stride * stride);
    for (auto &byte : data)
      byte = 1 + random() % UINT8_MAX;
    const UINT units = raw_bytes / word_bytes, maximum = UINT32_MAX / stride;
    addresses = {{0, 0},
                 {1, 1},
                 {units - width, count - 1},
                 {units - width + 1, count},
                 {units - 1, count + 1},
                 {units, maximum - 1},
                 {units + 1, maximum},
                 {UINT32_MAX / word_bytes - components, maximum + 1},
                 {UINT32_MAX / word_bytes + (word_bytes > 1), UINT32_MAX}};
    for (UINT i = 0; i < components; i++)
      addresses.push_back({UINT32_MAX / word_bytes - i, maximum + 1});
    for (UINT i = 0; i < 2 * components; i++)
      addresses.push_back({random() % (units + components), random() % (count + components)});
    for (size_t index = 0; index < addresses.size(); index++) {
      const auto at = addresses[index];
      for (UINT kind = 0; kind < kinds; kind++)
        for (UINT c = 0; c < width; c++) {
          uint64_t value = 0;
          if (kind == kinds - 2) {
            value = (index + 1) % addresses.size() + 1;
          } else if (kind == kinds - 1 && bits == sizeof(UINT) * CHAR_BIT && width == components && c == width - 1) {
            value = 0;
          } else {
            const bool structured = kind < kinds - 2 && kind % 2;
            const uint64_t offset = structured ? uint64_t(at[1]) * stride + c * word_bytes
                                               : UINT(at[0] * word_bytes +
                                                      (c + (kind == kinds - 1 ? components : 0)) * word_bytes);
            if (!structured || at[1] < count)
              for (UINT b = 0; b < word_bytes; b++) {
                const uint64_t byte_offset = structured ? offset + b : UINT(offset + b);
                if (!structured && byte_offset >= raw_bytes)
                  continue;
                value |= uint64_t(data[(structured ? first * stride : raw_first * sizeof(UINT)) + byte_offset])
                         << (b * CHAR_BIT);
              }
          }
          expected.push_back({UINT(value), UINT(value >> (sizeof(UINT) * CHAR_BIT))});
        }
    }
  }

  std::vector<std::string> defines() const {
    const std::string word = bits == sizeof(UINT) * CHAR_BIT ? "uint" : "uint" + std::to_string(bits) + "_t";
    return {"WORD=" + word,
            "VALUE=" + word + (width == 1 ? "" : std::to_string(width)),
            "LOAD=Load" + (width == 1 ? std::string() : std::to_string(width)),
            "BITS=" + std::to_string(bits),
            "WIDTH=" + std::to_string(width),
            "PADDING=" + std::to_string(padding),
            "WORD_BYTES=" + std::to_string(word_bytes),
            "CASES=" + std::to_string(addresses.size()),
            "COMPONENTS=" + std::to_string(components),
            "KINDS=" + std::to_string(kinds)};
  }

  void check(const void *bytes) const {
    for (size_t i = 0; i < expected.size(); i++) {
      Result got;
      memcpy(&got, static_cast<const uint8_t *>(bytes) + i * sizeof(got), sizeof(got));
      expect(got == expected[i],
             "seed %u, %u-bit width %u padding %u, address %zu, kind %zu, component %zu: "
             "%08x:%08x, expected %08x:%08x",
             seed, bits, width, padding, i / (kinds * width), i / width % kinds, i % width, got[1], got[0],
             expected[i][1], expected[i][0]);
    }
  }
};
} // namespace buffer_loads
