// contract: winemetal.h's WMTColorSpace distinguishes SDR from HDR PQ and scRGB. the HDR predicate must compare
// equal to a matching Boolean layer state, as MetalLayer_setColorSpace requires, and accept an expression argument.
#include "../../src/winemetal/winemetal.h"
#include <cstdio>

int
main() {
  const struct {
    WMTColorSpace colorspace;
    bool hdr;
  } cases[] = {
      {WMTColorSpaceSRGB, false}, {WMTColorSpaceSRGBLinear, false}, {WMTColorSpaceBT2020, false},
      {WMTColorSpaceHDR_PQ, true}, {WMTColorSpaceHDR10, true}, {WMTColorSpaceHDR_scRGB, true},
  };
  unsigned wrong = 0;
  for (auto c : cases) {
    if ((WMT_COLORSPACE_IS_HDR(c.colorspace)) != c.hdr) {
      printf("wrong: colorspace=%llu HDR predicate differs from %u\n", (unsigned long long)c.colorspace, c.hdr);
      wrong++;
    }
    for (unsigned enabled = 0; enabled <= 1; enabled++) {
      bool matches = enabled == WMT_COLORSPACE_IS_HDR(c.colorspace);
      if (matches != (enabled == c.hdr)) {
        printf("wrong: colorspace=%llu enabled=%u equality=%u\n", (unsigned long long)c.colorspace, enabled, matches);
        wrong++;
      }
    }
    if ((WMT_COLORSPACE_IS_HDR(true ? c.colorspace : WMTColorSpaceSRGB)) != c.hdr) {
      printf("wrong: colorspace=%llu conditional argument differs from %u\n", (unsigned long long)c.colorspace, c.hdr);
      wrong++;
    }
  }
  printf("%s: %u wrong results\n", wrong ? "failed" : "passed", wrong);
  return wrong != 0;
}
