#pragma once
#include "Metal.hpp"
#include "thread.hpp"
#include "airconv_public.h"
#include "mullion_format.hpp"
#include <span>
#include <string>

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

class ShaderCache {
public:
  template <typename T> class LockProtected {
  public:
    [[nodiscard]] LockProtected(mullion::mutex &mutex, T object) : object(object), mutex_(&mutex) {
      mutex_->lock();
    }
    [[nodiscard]] LockProtected() : object({}), mutex_(nullptr) {}
    ~LockProtected() {
      if (mutex_) {
        mutex_->unlock();
        mutex_ = nullptr;
      }
      object = {};
    }
    T object;
    LockProtected(const LockProtected &copy) = delete;
    LockProtected(LockProtected &&move) = default;

    operator bool() const {
      return mutex_ != nullptr;
    }

    T *
    operator->() {
      return &object;
    }

  private:
    mullion::mutex *mutex_;
  };

  static ShaderCache &getInstance(WMTMetalVersion version);

  LockProtected<WMT::CacheWriter>
  getWriter() {
    if (!scache_writer_)
      return {};
    return {scache_writer_mutex_, scache_writer_};
  }

  LockProtected<WMT::CacheReader>
  getReader() {
    if (!scache_reader_)
      return {};
    return {scache_reader_mutex_, scache_reader_};
  }

  ShaderCache(WMTMetalVersion metal_version);
  ShaderCache(const ShaderCache &copy) = delete;

private:
  WMT::Reference<WMT::CacheWriter> scache_writer_;
  mullion::mutex scache_writer_mutex_;

  WMT::Reference<WMT::CacheReader> scache_reader_;
  mullion::mutex scache_reader_mutex_;
};

} // namespace mullion