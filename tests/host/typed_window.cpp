// contract: a typed buffer view's Metal window starts near FirstElement, is aligned, and never exceeds 2^28 texels
// (Metal Feature Set Tables, textureBuffer). a clamp of the whole allocation at 0 is wrong: R32 2^28+4096 with a
// view at FirstElement=2^28-2048, NumElements=4096 must land in a window that contains those elements
// (GitHub #67).
#include "../../src/mullion/mullion_typed_window.hpp"
#include <cstdio>

int
main() {
  unsigned wrong = 0, checks = 0;
  auto expect = [&](bool ok, const char *what) {
    checks++;
    if (!ok && wrong++ < 16)
      printf("wrong: %s\n", what);
  };
  using mullion::typedBufferWindow;
  using mullion::kMetalTextureBufferMaxWidth;

  auto at_origin = typedBufferWindow(0, 256, 4);
  expect(at_origin.byte_offset == 0 && at_origin.byte_length == 256 && at_origin.element_offset == 0 &&
             !at_origin.capped,
         "a 64-element R32 view at 0 was not a 256-byte window");

  auto shifted = typedBufferWindow(20, 16, 4);
  expect(shifted.byte_offset == 16 && shifted.element_offset == 1 && shifted.byte_length >= 20 && !shifted.capped,
         "a view 20 bytes in was not aligned down to 16 with a shader-visible extra texel");

  // maintainer case: R32 allocation of 2^28+4096, view FirstElement=2^28-2048, NumElements=4096
  const uint64_t texel = 4;
  const uint64_t first = (kMetalTextureBufferMaxWidth - 2048) * texel;
  auto straddle = typedBufferWindow(first, 4096 * texel, uint32_t(texel));
  expect(!straddle.capped, "a 4096-texel view that only starts near 2^28 was capped");
  expect(straddle.byte_offset <= first &&
             straddle.byte_offset + straddle.byte_length >= first + 4096 * texel,
         "the straddling view's window does not cover FirstElement..FirstElement+4096");
  expect(straddle.element_offset == (first - straddle.byte_offset) / texel,
         "the shader index into the window is not the extra texels past the aligned start");

  auto past = typedBufferWindow(kMetalTextureBufferMaxWidth * texel, 64 * texel, uint32_t(texel));
  expect(past.byte_offset == kMetalTextureBufferMaxWidth * texel && past.element_offset == 0 && !past.capped,
         "a legal small view that starts at 2^28 was not a window of its own");

  auto huge = typedBufferWindow(0, (kMetalTextureBufferMaxWidth + 4096) * texel, uint32_t(texel));
  expect(huge.capped && huge.byte_length == kMetalTextureBufferMaxWidth * texel,
         "a view larger than 2^28 was not documented as the first 2^28");

  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong ? 1 : 0;
}
