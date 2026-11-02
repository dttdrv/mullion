#pragma once
#include "nvngx.hpp"
#include <cstdint>

class IMTLD3D12Upscaler;

namespace mullion {

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