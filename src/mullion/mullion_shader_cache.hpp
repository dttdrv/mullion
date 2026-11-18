#pragma once
#include "Metal.hpp"
#include "thread.hpp"
#include "airconv_public.h"
#include "mullion_format.hpp"
#include "sha1/sha1_util.hpp"
#include <shared_mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mullion {

constexpr int kMullionShaderCacheVersion = AIRCONV_VERSION;

inline auto
PixelShaderOutputFormats(
    const MTL_PIXEL_SHADER_REFLECTION &shader, std::span<const WMTPixelFormat> formats, bool dual
) {
  // hints name the 32-bit output register types (D3D11.3 7.6, 16.6), so equal types share a variant
  static_assert(WMTPixelFormatR32Float <= UINT8_MAX && WMTPixelFormatR32Sint <= UINT8_MAX &&
                WMTPixelFormatR32Uint <= UINT8_MAX);
  std::array<uint32_t, std::size(SM50_SHADER_PSO_PIXEL_SHADER_DATA{}.pixel_formats)> outputs{};
  for (uint32_t reg = 0; reg < outputs.size(); reg++) {
    auto slot = dual ? 0 : reg;
    if (!(shader.ValidRenderTargets & (1u << reg)) || (dual && reg > 1) || slot >= formats.size() ||
        formats[slot] == WMTPixelFormatInvalid)
      continue;
    auto format = WMTPixelFormat(ORIGINAL_FORMAT(formats[slot]));
    bool integer = IsIntegerFormat(format);
    bool unsigned_output = integer && MTLGetUnsignedIntegerFormat(format) == format;
    if (bool(shader.UintRenderTargets & (1u << reg)) != unsigned_output ||
        bool(shader.SintRenderTargets & (1u << reg)) != (integer && !unsigned_output))
      outputs[reg] =
          integer ? (unsigned_output ? WMTPixelFormatR32Uint : WMTPixelFormatR32Sint) : WMTPixelFormatR32Float;
    auto channels = RenderTargetChannels(formats[slot]);
    if (channels != RenderTargetChannels(format)) {
      outputs[reg] |= SM50_PIXEL_FORMAT_CHANNELS;
      for (uint32_t i = 0; i < channels.size(); i++)
        outputs[reg] |= i << (8 + 2 * channels[i]);
    }
  }
  return outputs;
}

// where a program's caches are, in the user's cache directory when relative (the bridge's resolve_cache_dir): none
// with MULLION_SHADER_CACHE=0
std::string ShaderCacheDirectory();

// what SM50Initialize reports of a shader, which a pipeline reads before any of its functions is converted
struct ShaderReflection {
  MTL_SHADER_REFLECTION reflection{};
  // its constant buffers' arguments, then its other arguments (SM50GetArgumentsInfo)
  std::vector<MTL_SM50_SHADER_ARGUMENT> arguments;
};

// that of the shader SM50Initialize made, which reported `reflection`
ShaderReflection ReadShaderReflection(sm50_shader_t shader, const MTL_SHADER_REFLECTION &reflection);

class ShaderCache {
public:
  static ShaderCache &getInstance(WMTMetalVersion version);

  // the store's writer and reader, null without a store (MULLION_SHADER_CACHE=0). any thread may use them at any
  // time: the bridge's reader gives each lookup a connection of its own, and its writer queues what it is given and
  // writes it in that order on a thread of its own
  WMT::CacheWriter
  getWriter() {
    return scache_writer_;
  }

  WMT::CacheReader
  getReader() {
    return scache_reader_;
  }

  // the reflection of the shader with this SHA-1, as an earlier pipeline or an earlier process stored it: with it and
  // the shader's converted functions, a pipeline is made without its bytecode being parsed at all. false when there
  // is none, and when what is stored is not a whole record. it is kept in the cache's table, which belongs to one
  // AIRCONV_VERSION: a change to what SM50Initialize reports raises that version
  bool getReflection(const Sha1Digest &shader, ShaderReflection &reflection);
  void setReflection(const Sha1Digest &shader, const ShaderReflection &reflection);

  // whether the program's pipeline archive holds a pipeline of this key, as a device recorded once the archive's file
  // that holds it was written, in this process or an earlier one. kept in the cache's table, as the functions the
  // pipeline is made of are
  bool isArchived(const Sha1Digest &pipeline);
  void setArchived(const Sha1Digest &pipeline);

  ShaderCache(WMTMetalVersion metal_version);
  ShaderCache(const ShaderCache &copy) = delete;

private:
  // those read or stored in this process
  std::shared_mutex reflections_mutex_;
  std::unordered_map<Sha1Digest, ShaderReflection> reflections_;
  std::shared_mutex archived_mutex_;
  std::unordered_set<Sha1Digest> archived_;

  WMT::Reference<WMT::CacheWriter> scache_writer_;
  WMT::Reference<WMT::CacheReader> scache_reader_;
};

} // namespace mullion