#pragma once

#include "../src/winemetal/winemetal.h"
#include <windows.h>

// whether this GPU has 8x MSAA, which D3D requires and Apple's GPUs before M4 lack; asks after a device has loaded Metal
inline bool metal_has_8x_samples() {
  static const bool has = [] {
    auto metal = GetModuleHandleA("winemetal.dll");
    auto all = reinterpret_cast<decltype(&WMTCopyAllDevices)>(GetProcAddress(metal, "WMTCopyAllDevices"));
    auto at = reinterpret_cast<decltype(&NSArray_object)>(GetProcAddress(metal, "NSArray_object"));
    auto supports = reinterpret_cast<decltype(&MTLDevice_supportsTextureSampleCount)>(
        GetProcAddress(metal, "MTLDevice_supportsTextureSampleCount")
    );
    auto release = reinterpret_cast<decltype(&NSObject_release)>(GetProcAddress(metal, "NSObject_release"));
    auto devices = all();
    bool result = supports(at(devices, 0), 8);
    release(devices);
    return result;
  }();
  return has;
}
