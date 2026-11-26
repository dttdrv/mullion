#pragma once
#include "nvngx.hpp"
#include <cstdint>

class IMTLD3D12Upscaler;

namespace mullion {

// the ratios of the output's size to the render size that MetalFX's temporal scaler takes on the device NGX started
// with, within which DLSS's optimal settings keep the render sizes they give (nvngx.cpp)
void SetTemporalScales(float min_scale, float max_scale);

struct CommonFeature {
  unsigned int handle;
  NVNGX_FEATURE feature;
};

struct DLSSFeature : CommonFeature {
  uint32_t width;
  uint32_t height;
  uint32_t target_width;
  uint32_t target_height;
  int quality;
  int flag;
  int enable_output_subrects;
  // D3D12: the MetalFX scaler the command list made for it
  IMTLD3D12Upscaler *upscaler = nullptr;
};

// DLSS Ray Reconstruction (D3D12), on MetalFX's temporal denoised scaler
struct DLSSDFeature : DLSSFeature {
  unsigned int roughness_mode; // 1: roughness in the normals' alpha
  unsigned int hw_depth;       // 0: linear depth, which MetalFX does not take
};

}