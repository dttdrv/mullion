#pragma once

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "../winemetal.h"

struct WMTPlacementRect {
  double x;
  double y;
  double width;
  double height;
};

static inline struct WMTPlacementRect
WMTComputeWindowPlacementRect(
    struct WMTWindowPlacement placement, double screen_w, double screen_h, double content_w, double content_h,
    bool content_is_flipped
) {
  // Proton's emulate_modeset scales virtual display modes uniformly into the physical monitor by
  // min(phys_w / virt_w, phys_h / virt_h), which in Cocoa points is 1 / max(virt_w / cocoa_w, virt_h / cocoa_h).
  // Using only screen_width breaks whenever the virtual mode's aspect ratio differs from the monitor's
  // (e.g. 1680x1050 16:10 on a 1920x1080 16:9 screen).
  double scale_x = placement.screen_width / screen_w;
  double scale_y = placement.screen_height ? placement.screen_height / screen_h : scale_x;
  double scale = fmax(scale_x, scale_y);
  if (scale <= 0.0)
    scale = 1.0;

  double scaled_w = placement.top_level_width / scale;
  double scaled_h = placement.top_level_height / scale;
  double border, above;
  if (placement.window_width == placement.top_level_width &&
      placement.window_height == placement.top_level_height) {
    // Borderless / fullscreen window: any difference between contentView.bounds and the scaled client
    // size is fullscreen pillarboxing or letterboxing from emulate_modeset, which win32u centers on both axes.
    border = (content_w - scaled_w) / 2.0;
    above = (content_h - scaled_h) / 2.0;
  } else {
    // the content view is the top-level window less what Cocoa draws around it. what Wine draws there itself
    // is a border as wide below as beside the client area, and the rest (caption, menu) above
    border = (content_w - scaled_w) / 2.0;
    above = content_h - scaled_h - border;
  }

  struct WMTPlacementRect area = {
      .x = border + placement.x / scale,
      .y = above + placement.y / scale,
      .width = placement.width / scale,
      .height = placement.height / scale,
  };
  if (!content_is_flipped)
    area.y = content_h - (area.y + area.height);
  return area;
}
