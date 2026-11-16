#pragma once
#include "mullion_command_queue.hpp"
#include <memory>

namespace mullion {

class Device {
public:
  virtual ~Device() {}

  virtual WMT::Device device() = 0;
  virtual CommandQueue& queue() = 0;
  virtual WMTMetalVersion metalVersion() = 0;
  virtual uint64_t maxObjectThreadgroups() = 0;

  // the most threadgroups a mesh grid may have (MeshGridLimit), asked of the device once, when first needed
  virtual uint32_t maxMeshThreadgroups() = 0;
};

struct DEVICE_DESC {
  WMT::Device device;
};

std::unique_ptr<Device> CreateMullionDevice(const DEVICE_DESC &desc);

} // namespace mullion
