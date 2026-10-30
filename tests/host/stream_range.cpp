// contract: a draw whose vertex range wraps 64-bit byte arithmetic is refused, never accepted as a smaller range.
// IDirect3DDevice9::DrawPrimitive: the vertices the draw reads must lie in the bound stream. a start near 2^32 and a
// stride near 2^32 make a product that does not fit in uint64_t; wrapping that product to 0 would look like an empty
// range inside a 16-byte buffer. stream_range_fits must reject it without allocating the buffer.
#include "../../src/d3d9/d3d9_range.hpp"
#include <cstdint>
#include <cstdio>

int
main() {
  unsigned wrong = 0, checks = 0;
  auto expect = [&](bool ok, const char *what) {
    checks++;
    if (!ok && wrong++ < 16)
      printf("wrong: %s\n", what);
  };

  using mullion::d3d9::stream_range_fits;
  expect(stream_range_fits(0, 0, 3, 20, 60), "three vertices of stride 20 in 60 bytes were refused");
  expect(!stream_range_fits(0, 0, 4, 20, 60), "four vertices of stride 20 in 60 bytes were accepted");
  expect(!stream_range_fits(8, 0, 3, 20, 60), "an 8-byte offset that pushes three vertices past 60 was accepted");

  // the wrapping product: 2^32 vertices of stride 2^32 is 2^64 bytes, which wraps to 0 in uint64_t and would look
  // like it fits a 16-byte buffer
  const uint64_t naive = (uint64_t(1) << 32) * (uint64_t(1) << 32);
  expect(naive == 0, "the wrapping product this test is built on did not wrap to 0");
  expect(!stream_range_fits(0, uint64_t(1) << 32, 0, uint64_t(1) << 32, 16),
         "a range whose byte count wraps 64 bits was accepted as fitting 16 bytes");

  // start + count itself wraps
  expect(!stream_range_fits(0, UINT64_MAX, 1, 20, UINT64_MAX),
         "a start plus a count that wraps 64 bits was accepted");

  // UINT_MAX start, UINT_MAX stride, three extra vertices: the product overflows uint64_t
  expect(!stream_range_fits(0, UINT32_MAX, 3, UINT32_MAX, 16),
         "UINT_MAX start and stride with three vertices was accepted as fitting 16 bytes");

  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
