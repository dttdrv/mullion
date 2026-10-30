#pragma once
#include <cstdint>

namespace mullion::d3d9 {

// whether a draw's vertices, from `start` for `count` at `stride` bytes, starting `offset` bytes into a buffer of
// `size`, stay inside it. the sums and the product are checked so a start near UINT_MAX and a stride near UINT_MAX
// are a range past the buffer, never a wrapped product that looks small enough to draw (Microsoft Learn,
// IDirect3DDevice9::DrawPrimitive)
inline bool
stream_range_fits(uint64_t offset, uint64_t start, uint64_t count, uint64_t stride, uint64_t size) {
  uint64_t vertices = 0, bytes = 0, end = 0;
  if (__builtin_add_overflow(start, count, &vertices))
    return false;
  if (__builtin_mul_overflow(vertices, stride, &bytes))
    return false;
  if (__builtin_add_overflow(offset, bytes, &end))
    return false;
  return end <= size;
}

} // namespace mullion::d3d9
