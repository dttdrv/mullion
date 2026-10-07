#pragma once
#include "Metal.hpp"
#include <string>

namespace dxmt {

std::string GetVersionDescriptionText(uint32_t ApiVersion, uint32_t FeatureLevel);

// the Metal language version shaders are converted for: the newest the system and the GPU have (3.2 from macOS 15 on
// an Apple GPU), or an older one that dxmt.shaderMetalVersion asks for. memory barriers across threadgroups and
// globally coherent accesses exist from 3.2 on, so every API takes it from here
WMTMetalVersion ShaderMetalVersion(WMT::Device device);

}; // namespace dxmt
