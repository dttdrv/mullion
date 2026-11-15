#include "mullion_info.hpp"
#include "config/config.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include "log/log.hpp"
#include "util_env.hpp"
#include "util_string.hpp"
#include <version.h>

namespace mullion {

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
  case 0xc200:
    feature_level_text = "12_2";
    break;
  }
  return str::format("Mullion D3D", ApiVersion, " FL_", feature_level_text, " ", MULLION_VERSION);
}

WMTMetalVersion
ShaderMetalVersion(WMT::Device device) {
  int asked = Config::getInstance().getOption<int>("mullion.shaderMetalVersion", Config::getInstance().getOption<int>("dxmt.shaderMetalVersion", 0));
  auto version = asked == WMTMetal310 || asked == WMTMetal320 || asked == WMTMetal400 ? WMTMetalVersion(asked)
                                                                                      : WMTMetalVersionMax;
  uint64_t macos_major_version = 0;
  WMTGetOSVersion(&macos_major_version, nullptr, nullptr);
  // Metal 4.0 is macOS 26's and its GPU family's
  if (macos_major_version < 26 || !device.supportsFamily(WMTGPUFamilyMetal4))
    version = std::min(WMTMetal320, version);
  // Metal 3.2 is macOS 15's, and what the converter needs of it is basically not there on other GPUs
  if (macos_major_version < 15 || !device.supportsFamily(WMTGPUFamilyApple7))
    version = std::min(WMTMetal310, version);
  return version;
}

// MULLION_D3D12_COARSE_HSR=0 (read once): coarse shading pixel shaders keep their discard, as before airconv 93
static SM50_SHADER_FLAG
SwitchedShaderFlags() {
  static const bool coarse_discard = env::getEnvVar("MULLION_D3D12_COARSE_HSR") == "0";
  return SM50_SHADER_FLAG(coarse_discard ? SM50_SHADER_FLAG_COARSE_DISCARD_ALWAYS : 0);
}

SM50_SHADER_FLAG
GPUShaderFlags(WMT::Device device) {
  auto given = env::getEnvVar("MULLION_SHADER_FLAGS");
  if (!given.empty()) {
    char *end;
    errno = 0;
    auto flags = std::strtoul(given.c_str(), &end, 0);
    constexpr auto known = SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO | SM50_SHADER_FLAG_DEFUSE_FMA |
                           SM50_SHADER_FLAG_NO_ATOMIC_MINMAX64 | SM50_SHADER_FLAG_COLLAPSE_CULLED_LINES |
                           SM50_SHADER_FLAG_COHERENT_GROUP_SYNC;
    if (end != given.c_str() && !*end && errno != ERANGE &&
        !(flags & ~static_cast<unsigned long>(known)) && given.find('-') == std::string::npos)
      return SM50_SHADER_FLAG(flags | SwitchedShaderFlags());
    ERR("Invalid MULLION_SHADER_FLAGS '", given, "'; using device defaults");
  }
  uint32_t flags = 0;
  if (!device.supportsFamily(WMTGPUFamilyApple8))
    flags |= SM50_SHADER_FLAG_NO_ATOMIC_MINMAX64;
  // a NaN texture coordinate is 0 once it is snapped to fixed point (D3D11.3 3.2.4.1), so a sample reads the first
  // texel of that axis; what Metal's sampler reads for a NaN is not defined. MULLION_SHADER_FLAGS without the flag,
  // or d3d11.sampleNaNToZero = False, leaves coordinates as they are
  flags |= SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO;
  if (!device.supportsFamily(WMTGPUFamilyApple9))
    flags |= SM50_SHADER_FLAG_COLLAPSE_CULLED_LINES | SM50_SHADER_FLAG_COHERENT_GROUP_SYNC;
  return SM50_SHADER_FLAG(flags | SwitchedShaderFlags());
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

uint32_t
MeshGridLimit(WMT::Device device) {
  static const char source[] = R"(#include <metal_stdlib>
using namespace metal;
struct Vertex {
  float4 position [[position]];
};
[[mesh]] void grid_limit(mesh<Vertex, void, 3, 1, topology::triangle> out) {
  out.set_primitive_count(0);
}
)";
  WMT::Reference<WMT::Error> error;
  auto library = device.newLibraryWithSource(source, error);
  auto function = library ? library.newFunction("grid_limit") : WMT::Reference<WMT::Function>{};
  WMTMeshRenderPipelineInfo info;
  WMT::InitializeMeshRenderPipelineInfo(info);
  info.mesh_function = function.handle;
  info.rasterization_enabled = false;
  auto pipeline = function ? device.newRenderPipelineState(info, error) : WMT::Reference<WMT::RenderPipelineState>{};
  if (!pipeline) {
    ERR("Failed to ask the device's mesh grid limit: ", error ? error.description().getUTF8String() : "no function");
    return 0;
  }
  return uint32_t(std::min<uint64_t>(pipeline.maxTotalThreadgroupsPerMeshGrid(), UINT32_MAX));
}

} // namespace mullion
