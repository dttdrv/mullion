#include "mullion_shader_cache.hpp"
#include "util_env.hpp"
#include "util_string.hpp"

namespace mullion {

ShaderCache &
ShaderCache::getInstance(WMTMetalVersion version) {
  static mullion::mutex mutex;
  static std::unordered_map<WMTMetalVersion, std::unique_ptr<ShaderCache>> caches;

  std::lock_guard<mullion::mutex> lock(mutex);
  auto iter = caches.find(version);
  if (iter == caches.end()) {
    auto inserted = caches.insert({version, std::make_unique<ShaderCache>(version)});
    return *inserted.first->second;
  }
  return *iter->second;
}

std::string
ShaderCacheDirectory() {
  if (env::getEnvVar("MULLION_SHADER_CACHE") == "0")
    return {};
  if (auto path = env::getEnvVar("MULLION_SHADER_CACHE_PATH"); !path.empty() && path.starts_with("/"))
    return path.ends_with('/') ? path : path + "/";
  // Mullion's shaders differ from stock DXMT's at the same AIRCONV_VERSION, so the caches never share a directory
  return str::format("mullion/", env::getExeName(), "/");
}

ShaderCache::ShaderCache(WMTMetalVersion metal_version) {
  auto path = ShaderCacheDirectory();
  if (path.empty())
    return;
  path += str::format("shaders_", (unsigned int)metal_version, ".db");
  scache_writer_ = WMT::CacheWriter::alloc_init(path.c_str(), kMullionShaderCacheVersion);
  scache_reader_ = WMT::CacheReader::alloc_init(path.c_str(), kMullionShaderCacheVersion);
}

} // namespace mullion