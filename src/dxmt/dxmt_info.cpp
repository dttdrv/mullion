#include "dxmt_info.hpp"
#include "config/config.hpp"
#include <algorithm>
#include "util_env.hpp"
#include "util_string.hpp"
#include <version.h>

namespace dxmt {

std::string
GetVersionDescriptionText(uint32_t ApiVersion, uint32_t FeatureLevel) {
  std::string feature_level_text = "UNKNOWN";
  switch (FeatureLevel) {
  case 0x9100:
    feature_level_text = "9_1";
    break;
  case 0x9200:
    feature_level_text = "9_2";
    break;
  case 0x9300:
    feature_level_text = "9_3";
    break;
  case 0xa000:
    feature_level_text = "10_0";
    break;
  case 0xa100:
    feature_level_text = "10_1";
    break;
  case 0xb000:
    feature_level_text = "11_0";
    break;
  case 0xb100:
    feature_level_text = "11_1";
    break;
  case 0xc000:
    feature_level_text = "12_0";
    break;
  case 0xc100:
    feature_level_text = "12_1";
    break;
  }
  return str::format("DXMT D3D", ApiVersion, " FL_", feature_level_text, " ", DXMT_VERSION);
}

WMTMetalVersion
ShaderMetalVersion(WMT::Device device) {
  int asked = Config::getInstance().getOption<int>("mullion.shaderMetalVersion", Config::getInstance().getOption<int>("dxmt.shaderMetalVersion", 0));
  auto version = asked == WMTMetal310 || asked == WMTMetal320 ? WMTMetalVersion(asked) : WMTMetalVersionMax;
  uint64_t macos_major_version = 0;
  WMTGetOSVersion(&macos_major_version, nullptr, nullptr);
  // Metal 3.2 is macOS 15's, and what the converter needs of it is basically not there on other GPUs
  if (macos_major_version < 15 || !device.supportsFamily(WMTGPUFamilyApple7))
    version = std::min(WMTMetal310, version);
  return version;
}

uint64_t
MeshBytesPerDraw(WMT::Device device) {
  auto limit = env::getEnvVar("MULLION_MAX_MESH_THREADGROUPS");
  if (!limit.empty()) {
    uint64_t value = std::strtoull(limit.c_str(), nullptr, 0);
    return value ? value * 2048 : -1ull;
  }
  // "up to 4 GB of payload and mesh geometry per draw for devices in the Apple7 and Apple8 GPU families" (Metal
  // Feature Set Tables): 2^32 bytes, which 2^21 mesh threadgroups of 2 KB fill (measured on an M1 Pro)
  return device.supportsFamily(WMTGPUFamilyApple7) && !device.supportsFamily(WMTGPUFamilyApple9) ? 1ull << 32 : -1ull;
}

} // namespace dxmt
