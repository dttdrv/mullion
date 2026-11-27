// contract: WMTComputeWindowPlacementRect uniformly scales and centers emulated fullscreen modes
// (pillarboxed and letterboxed) while preserving Wine-drawn border/caption placement for decorated windows.
#include <cmath>
#include <cstdio>

#include "unix/mullion_placement.h"

namespace {

int failures = 0;

void
check_rect(const char *name, WMTPlacementRect got, double ex, double ey, double ew, double eh) {
  constexpr double eps = 1e-3;
  if (std::fabs(got.x - ex) > eps || std::fabs(got.y - ey) > eps || std::fabs(got.width - ew) > eps ||
      std::fabs(got.height - eh) > eps) {
    std::fprintf(
        stderr, "FAIL %s: got (%.3f, %.3f, %.3f, %.3f), expected (%.3f, %.3f, %.3f, %.3f)\n", name,
        got.x, got.y, got.width, got.height, ex, ey, ew, eh
    );
    failures++;
  }
}

} // namespace

int
main() {
  // 1. Native 4K Retina fullscreen (3840x2160 Wine px on 1920x1080 Cocoa pt)
  {
    WMTWindowPlacement p{
        .top_level = 1,
        .x = 0,
        .y = 0,
        .width = 3840,
        .height = 2160,
        .top_level_width = 3840,
        .top_level_height = 2160,
        .window_width = 3840,
        .window_height = 2160,
        .screen_width = 3840,
        .screen_height = 2160,
    };
    check_rect(
        "native_4k_fullscreen", WMTComputeWindowPlacementRect(p, 1920, 1080, 1920, 1080, true), 0, 0, 1920, 1080
    );
  }

  // 2. 16:10 emulated pillarboxed fullscreen (1680x1050 on 1920x1080 Cocoa pt):
  //    scale = max(1680/1920, 1050/1080) = 1050/1080 = 35/36
  //    scaled_w = 1680 / (35/36) = 1728, scaled_h = 1080, centered -> x = 96, y = 0
  {
    WMTWindowPlacement p{
        .top_level = 1,
        .x = 0,
        .y = 0,
        .width = 1680,
        .height = 1050,
        .top_level_width = 1680,
        .top_level_height = 1050,
        .window_width = 1680,
        .window_height = 1050,
        .screen_width = 1680,
        .screen_height = 1050,
    };
    check_rect(
        "pillarbox_16_10_flipped", WMTComputeWindowPlacementRect(p, 1920, 1080, 1920, 1080, true), 96, 0, 1728, 1080
    );
    check_rect(
        "pillarbox_16_10_unflipped", WMTComputeWindowPlacementRect(p, 1920, 1080, 1920, 1080, false), 96, 0, 1728, 1080
    );
  }

  // 3. 21:9 emulated letterboxed fullscreen (2560x1080 on 1920x1080 Cocoa pt):
  //    scale = max(2560/1920, 1080/1080) = 4/3
  //    scaled_w = 1920, scaled_h = 810, centered -> x = 0, y = 135
  {
    WMTWindowPlacement p{
        .top_level = 1,
        .x = 0,
        .y = 0,
        .width = 2560,
        .height = 1080,
        .top_level_width = 2560,
        .top_level_height = 1080,
        .window_width = 2560,
        .window_height = 1080,
        .screen_width = 2560,
        .screen_height = 1080,
    };
    check_rect(
        "letterbox_21_9_flipped", WMTComputeWindowPlacementRect(p, 1920, 1080, 1920, 1080, true), 0, 135, 1920, 810
    );
    check_rect(
        "letterbox_21_9_unflipped", WMTComputeWindowPlacementRect(p, 1920, 1080, 1920, 1080, false), 0, 135, 1920, 810
    );
  }

  // 4. Decorated window with Wine-drawn borders/caption + child control offset:
  //    2x Retina: screen 3840x2160 -> scale = 2.0
  //    contentView = 646x426 pt, client = 1280x800 px (640x400 pt), outer window = 1292x852 px
  //    border = (646 - 640)/2 = 3 pt, above = 426 - 400 - 3 = 23 pt
  //    child at (20, 40) size 400x200 px -> (10, 20) size 200x100 pt
  //    flipped:   (3 + 10, 23 + 20, 200, 100) = (13, 43, 200, 100)
  //    unflipped: y = 426 - (43 + 100) = 283
  {
    WMTWindowPlacement p{
        .top_level = 1,
        .x = 20,
        .y = 40,
        .width = 400,
        .height = 200,
        .top_level_width = 1280,
        .top_level_height = 800,
        .window_width = 1292,
        .window_height = 852,
        .screen_width = 3840,
        .screen_height = 2160,
    };
    check_rect(
        "decorated_child_flipped", WMTComputeWindowPlacementRect(p, 1920, 1080, 646, 426, true), 13, 43, 200, 100
    );
    check_rect(
        "decorated_child_unflipped", WMTComputeWindowPlacementRect(p, 1920, 1080, 646, 426, false), 13, 283, 200, 100
    );
  }

  if (failures)
    return 1;
  std::puts("window_placement: all cases passed");
  return 0;
}
