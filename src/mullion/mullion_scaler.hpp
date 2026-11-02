#pragma once

#include "Metal.hpp"
#include <atomic>

namespace mullion {

class SpatialScaler {
public:
  void incRef();
  void decRef();

  // metal4: a scaler for Metal 4 command buffers (MTL4FXSpatialScaler)
  SpatialScaler(WMT::Device device, const WMTFXSpatialScalerInfo &info, bool metal4 = false);

  WMT::FXSpatialScaler scaler() {
    return scaler_;
  }

  WMT::Fence fence() {
    return fence_;
  }

private:
  WMT::Reference<WMT::FXSpatialScaler> scaler_;
  WMT::Reference<WMT::Fence> fence_;
  std::atomic<uint32_t> refcount_;
};

class TemporalScaler {
public:
  void incRef();
  void decRef();

  // metal4: a scaler for Metal 4 command buffers (MTL4FXTemporalScaler)
  TemporalScaler(WMT::Device device, const WMTFXTemporalScalerInfo &info, bool metal4 = false);

  WMT::FXTemporalScaler scaler() {
    return scaler_;
  }

  WMT::Fence fence() {
    return fence_;
  }

private:
  WMT::Reference<WMT::FXTemporalScaler> scaler_;
  WMT::Reference<WMT::Fence> fence_;
  std::atomic<uint32_t> refcount_;
};

// MetalFX temporal denoised upscaling (MTLFXTemporalDenoisedScaler)
class DenoisedScaler {
public:
  void incRef();
  void decRef();

  DenoisedScaler(WMT::Device device, const WMTFXDenoisedScalerInfo &info, bool metal4);

  WMT::FXDenoisedScaler scaler() {
    return scaler_;
  }

  WMT::Fence fence() {
    return fence_;
  }

private:
  WMT::Reference<WMT::FXDenoisedScaler> scaler_;
  WMT::Reference<WMT::Fence> fence_;
  std::atomic<uint32_t> refcount_;
};

} // namespace mullion