#pragma once
#include "dxmt_command_queue.hpp"
#include <memory>

namespace dxmt {

class Device {
public:
  virtual ~Device() {}

  virtual WMT::Device device() = 0;
  virtual CommandQueue& queue() = 0;
  virtual WMTMetalVersion metalVersion() = 0;
  virtual uint64_t maxObjectThreadgroups() = 0;

  // the most threadgroups a mesh grid may have, which only a pipeline tells
  // (MTLRenderPipelineState's maxTotalThreadgroupsPerMeshGrid); 0 until one was asked
  std::atomic<uint32_t> max_mesh_threadgroups{0};
};

struct DEVICE_DESC {
  WMT::Device device;
};

std::unique_ptr<Device> CreateDXMTDevice(const DEVICE_DESC &desc);

} // namespace dxmt
