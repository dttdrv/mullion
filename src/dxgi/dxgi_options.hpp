#pragma once

#include "config/config.hpp"

namespace mullion {

/**
 * \brief DXGI options
 *
 * Per-app options that control the
 * behaviour of some DXGI classes.
 */
struct DxgiOptions {
  DxgiOptions(const Config &config);

  /// Override PCI vendor and device IDs reported to the
  /// application. This may make apps think they are running
  /// on a different GPU than they do and behave differently.
  int32_t customVendorId;
  int32_t customDeviceId;
  /// the PCI subsystem ID and revision reported (dxgi.customSubSysId, up to
  /// eight hex digits, and dxgi.customRevision, up to two), or -1 for 0
  int64_t customSubSysId;
  int32_t customRevision;
  std::string customDeviceDesc;
  bool forceSDR;
  /// The most memory the adapter reports, in bytes (dxgi.maxDeviceMemory, in MB, as DXVK names it; 0 for all Metal
  /// recommends). Titles size their streaming to the adapter's memory and the budget: a Mac of 16 GB shares it with
  /// everything else (dttdrv/mullion#32, Starfield)
  uint64_t maxDeviceMemory;
};

} // namespace mullion