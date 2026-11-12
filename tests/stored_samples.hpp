// how many of a multisample resource's `samples` the GPU stores apart: all of them, or half on a GPU that has no
// textures of that many, where one stored sample stands for the samples 2i and 2i + 1 (an 8-sample resource on a GPU
// of 4: feature level 11_0 has 8 samples whatever the GPU). asked of the Metal bridge, which is what knows; Direct3D
// itself stores them all
#pragma once
#include <windows.h>

inline unsigned
stored_samples(unsigned samples) {
  auto bridge = LoadLibraryA("winemetal.dll");
  auto devices = (UINT64 (*)())GetProcAddress(bridge, "WMTCopyAllDevices");
  auto object = (UINT64 (*)(UINT64, UINT64))GetProcAddress(bridge, "NSArray_object");
  auto supports = (bool (*)(UINT64, UINT8))GetProcAddress(bridge, "MTLDevice_supportsTextureSampleCount");
  auto release = (void (*)(UINT64))GetProcAddress(bridge, "NSObject_release");
  if (!devices || !object || !supports || !release)
    return samples;
  auto all = devices();
  auto device = object(all, 0);
  bool halved = samples % 2 == 0 && !supports(device, UINT8(samples)) && supports(device, UINT8(samples / 2));
  release(all);
  return halved ? samples / 2 : samples;
}
