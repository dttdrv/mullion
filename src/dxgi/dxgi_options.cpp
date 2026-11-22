#include "dxgi_options.hpp"
#include <algorithm>

namespace mullion {

static int32_t parsePciId(const std::string &str) {
  if (str.size() != 4)
    return -1;

  int32_t id = 0;

  for (size_t i = 0; i < str.size(); i++) {
    id *= 16;

    if (str[i] >= '0' && str[i] <= '9')
      id += str[i] - '0';
    else if (str[i] >= 'A' && str[i] <= 'F')
      id += str[i] - 'A' + 10;
    else if (str[i] >= 'a' && str[i] <= 'f')
      id += str[i] - 'a' + 10;
    else
      return -1;
  }

  return id;
}

// a hexadecimal number of 1 to `digits` digits, or -1
static int64_t parseHex(const std::string &str, size_t digits) {
  if (str.empty() || str.size() > digits)
    return -1;
  int64_t id = 0;
  for (char c : str) {
    int value = c >= '0' && c <= '9'   ? c - '0'
                : c >= 'A' && c <= 'F' ? c - 'A' + 10
                : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                       : -1;
    if (value < 0)
      return -1;
    id = id * 16 + value;
  }
  return id;
}

DxgiOptions::DxgiOptions(const Config &config) {
  // Fetch these as a string representing a hexadecimal number and parse it.
  this->customVendorId =
      parsePciId(config.getOption<std::string>("dxgi.customVendorId"));
  this->customDeviceId =
      parsePciId(config.getOption<std::string>("dxgi.customDeviceId"));
  this->customSubSysId =
      parseHex(config.getOption<std::string>("dxgi.customSubSysId"), 8);
  this->customRevision = int32_t(
      parseHex(config.getOption<std::string>("dxgi.customRevision"), 2));
  this->customDeviceDesc =
      config.getOption<std::string>("dxgi.customDeviceDesc", "");
  this->forceSDR = config.getOption<bool>("dxgi.forceSDR", false);
  this->maxDeviceMemory = uint64_t(std::max(config.getOption<int32_t>("dxgi.maxDeviceMemory", 0), 0)) << 20;
}

} // namespace mullion
