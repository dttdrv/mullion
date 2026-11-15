#include "mullion_device.hpp"
#include "config/config.hpp"
#include "mullion_command_queue.hpp"
#include "Metal.hpp"
#include "mullion_info.hpp"
#include <mutex>

namespace mullion {

class DeviceImpl : public Device {
public:
  virtual WMT::Device
  device() override {
    return device_;
  };
  virtual CommandQueue &
  queue() override {
    return cmd_queue_;
  };

  virtual WMTMetalVersion metalVersion() override {
    return metal_version_;
  };

  virtual uint64_t maxObjectThreadgroups() final {
    return max_object_threadgroups_;
  };

  virtual uint32_t maxMeshThreadgroups() final {
    std::call_once(mesh_grid_asked_, [&] { max_mesh_threadgroups_ = MeshGridLimit(device_); });
    return max_mesh_threadgroups_;
  };

  DeviceImpl(const DEVICE_DESC &desc) : device_(desc.device), cmd_queue_(device_) {
    uint64_t macos_major_version = 0, macos_minor_version = 0;
    metal_version_ = ShaderMetalVersion(device_);
    WMTGetOSVersion(&macos_major_version, &macos_minor_version, nullptr);
    if (!device_.supportsFamily(WMTGPUFamilyApple7)) {
      WARN("Experimental non-Apple GPU support");
      max_object_threadgroups_ = 1024;
      // macOS 26 bug: setShouldMaximizeConcurrentCompilation crashes on AMDGPU
      if (!(macos_major_version >= 16 && macos_major_version <= 26 && macos_minor_version < 2))
        device_.setShouldMaximizeConcurrentCompilation(true);
    } else {
      max_object_threadgroups_ = -1ull;
      device_.setShouldMaximizeConcurrentCompilation(true);
    }
  }

private:
  WMT::Reference<WMT::Device> device_;
  CommandQueue cmd_queue_;
  WMTMetalVersion metal_version_;
  uint64_t max_object_threadgroups_;
  std::once_flag mesh_grid_asked_;
  uint32_t max_mesh_threadgroups_ = 0;
};

std::unique_ptr<Device>
CreateMullionDevice(const DEVICE_DESC &desc) {
  return std::make_unique<DeviceImpl>(desc);
};

} // namespace mullion
