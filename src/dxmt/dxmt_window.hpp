#pragma once

#include "Metal.hpp"
#include "winemetal.h"
#include <cstring>
#include <windows.h>

namespace dxmt {

// where a window's client area is (WMTWindowPlacement), in pixels no DPI awareness has scaled
inline WMTWindowPlacement
windowPlacement(HWND hWindow) {
  auto awareness = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  auto top_level = GetAncestor(hWindow, GA_ROOT);
  RECT client{}, around{};
  GetClientRect(hWindow, &client);
  GetClientRect(top_level, &around);
  MapWindowPoints(hWindow, top_level, reinterpret_cast<POINT *>(&client), 2);
  WMTWindowPlacement placement{
      (uint64_t)(uintptr_t)top_level, client.left, client.top, client.right - client.left, client.bottom - client.top,
      around.right, around.bottom, (uint32_t)GetSystemMetrics(SM_CXSCREEN)
  };
  SetThreadDpiAwarenessContext(awareness);
  return placement;
}

// keeps a Metal view where its window's client area is, for a Wine whose views do not follow their windows
class WindowFollower {
public:
  // the placement to make the window's view with
  WMTWindowPlacement
  placement(HWND hWindow) {
    return placement_ = windowPlacement(hWindow);
  }
  // after the view is made, and before each frame is presented
  void
  follow(HWND hWindow, WMT::Object view, bool created = false) {
    if (created)
      follows_ = view && WMT::SetMetalViewPlacement(view, placement_);
    if (!follows_)
      return;
    auto now = windowPlacement(hWindow);
    if (!memcmp(&now, &placement_, sizeof(now)))
      return;
    placement_ = now;
    WMT::SetMetalViewPlacement(view, now);
  }

private:
  WMTWindowPlacement placement_{};
  bool follows_ = false;
};

} // namespace dxmt
