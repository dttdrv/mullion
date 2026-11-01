#include "mullion_scaler.hpp"

namespace mullion {

void
SpatialScaler::incRef() {
  refcount_.fetch_add(1u, std::memory_order_acquire);
}

void
SpatialScaler::decRef() {
  if (refcount_.fetch_sub(1u, std::memory_order_release) == 1u)
    delete this;
}

SpatialScaler::SpatialScaler(WMT::Device device, const WMTFXSpatialScalerInfo &info, bool metal4) {
  scaler_ = metal4 ? device.newSpatialScaler4(info) : device.newSpatialScaler(info);
  fence_ = device.newFence();
};

void
TemporalScaler::incRef() {
  refcount_.fetch_add(1u, std::memory_order_acquire);
}

void
TemporalScaler::decRef() {
  if (refcount_.fetch_sub(1u, std::memory_order_release) == 1u)
    delete this;
}

TemporalScaler::TemporalScaler(WMT::Device device, const WMTFXTemporalScalerInfo &info, bool metal4) {
  scaler_ = metal4 ? device.newTemporalScaler4(info) : device.newTemporalScaler(info);
  fence_ = device.newFence();
}

void
DenoisedScaler::incRef() {
  refcount_.fetch_add(1u, std::memory_order_acquire);
}

void
DenoisedScaler::decRef() {
  if (refcount_.fetch_sub(1u, std::memory_order_release) == 1u)
    delete this;
}

DenoisedScaler::DenoisedScaler(WMT::Device device, const WMTFXDenoisedScalerInfo &info, bool metal4) {
  scaler_ = device.newDenoisedScaler(info, metal4);
  fence_ = device.newFence();
}

} // namespace mullion
