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

ShaderReflection
ReadShaderReflection(sm50_shader_t shader, const MTL_SHADER_REFLECTION &reflection) {
  ShaderReflection read{reflection};
  read.arguments.resize(size_t(reflection.NumConstantBuffers) + reflection.NumArguments);
  if (!read.arguments.empty())
    SM50GetArgumentsInfo(shader, read.arguments.data(), read.arguments.data() + reflection.NumConstantBuffers);
  return read;
}

namespace {
// the reflection's size, then those of the stages' parts of it: they share its bytes, so one of them can grow and
// leave the whole's size as it was
constexpr uint32_t kReflectionLayout[] = {
    sizeof(MTL_SHADER_REFLECTION),           sizeof(MTL_TESSELLATOR_REFLECTION), sizeof(MTL_GEOMETRY_SHADER_REFLECTION),
    sizeof(MTL_POST_TESSELLATOR_REFLECTION), sizeof(MTL_PIXEL_SHADER_REFLECTION)
};
// a stored reflection: this, the reflection, its arguments, and the SHA-1 of all three. the sizes tell a record of
// another layout from this one, and the digest a damaged one
struct ReflectionRecord {
  char magic[8];
  uint32_t layout[std::size(kReflectionLayout)];
  uint32_t argument_size;
  uint32_t arguments;
};
constexpr char kReflectionMagic[] = "SM50REFL";
static_assert(sizeof(kReflectionMagic) - 1 == sizeof(ReflectionRecord::magic));

Sha1Digest
ReflectionKey(const Sha1Digest &shader) {
  Sha1HashState h;
  h.update(kReflectionMagic, sizeof(kReflectionMagic));
  h.update(shader);
  return h.final();
}

constexpr char kArchivedMagic[] = "ARCHIVED";

Sha1Digest
ArchivedKey(const Sha1Digest &pipeline) {
  Sha1HashState h;
  h.update(kArchivedMagic, sizeof(kArchivedMagic));
  h.update(pipeline);
  return h.final();
}
} // namespace

bool
ShaderCache::isArchived(const Sha1Digest &pipeline) {
  if (!scache_reader_)
    return false;
  {
    std::shared_lock lock(archived_mutex_);
    if (archived_.contains(pipeline))
      return true;
  }
  if (!getReader().get(ArchivedKey(pipeline)))
    return false;
  std::unique_lock lock(archived_mutex_);
  archived_.insert(pipeline);
  return true;
}

void
ShaderCache::setArchived(const Sha1Digest &pipeline) {
  if (!scache_writer_)
    return;
  {
    std::unique_lock lock(archived_mutex_);
    if (!archived_.insert(pipeline).second)
      return;
  }
  char held = 1;
  getWriter().set(ArchivedKey(pipeline), WMT::MakeDispatchData(&held, sizeof(held)));
}

bool
ShaderCache::getReflection(const Sha1Digest &shader, ShaderReflection &reflection) {
  if (!scache_reader_)
    return false;
  {
    std::shared_lock lock(reflections_mutex_);
    if (auto found = reflections_.find(shader); found != reflections_.end()) {
      reflection = found->second;
      return true;
    }
  }
  WMT::Reference<WMT::DispatchData> stored;
  if (auto reader = getReader())
    stored = reader.get(ReflectionKey(shader));
  if (!stored)
    return false;
  std::string bytes(stored.getBytes(nullptr, 0), '\0');
  stored.getBytes(bytes.data(), bytes.size());
  ReflectionRecord record;
  if (bytes.size() < sizeof(record) + sizeof(Sha1Digest))
    return false;
  memcpy(&record, bytes.data(), sizeof(record));
  size_t body = bytes.size() - sizeof(Sha1Digest);
  if (memcmp(record.magic, kReflectionMagic, sizeof(record.magic)) ||
      memcmp(record.layout, kReflectionLayout, sizeof(record.layout)) ||
      record.argument_size != sizeof(MTL_SM50_SHADER_ARGUMENT) ||
      body != sizeof(record) + sizeof(MTL_SHADER_REFLECTION) +
                  uint64_t(record.arguments) * sizeof(MTL_SM50_SHADER_ARGUMENT) ||
      memcmp(Sha1HashState::compute(bytes.data(), body).data, bytes.data() + body, sizeof(Sha1Digest)))
    return false;
  ShaderReflection read;
  memcpy(&read.reflection, bytes.data() + sizeof(record), sizeof(read.reflection));
  if (size_t(read.reflection.NumConstantBuffers) + read.reflection.NumArguments != record.arguments)
    return false;
  read.arguments.resize(record.arguments);
  if (record.arguments)
    memcpy(read.arguments.data(), bytes.data() + sizeof(record) + sizeof(read.reflection),
           record.arguments * sizeof(MTL_SM50_SHADER_ARGUMENT));
  reflection = read;
  std::unique_lock lock(reflections_mutex_);
  reflections_.emplace(shader, std::move(read));
  return true;
}

void
ShaderCache::setReflection(const Sha1Digest &shader, const ShaderReflection &reflection) {
  if (!scache_reader_)
    return;
  {
    std::unique_lock lock(reflections_mutex_);
    if (!reflections_.emplace(shader, reflection).second)
      return;
  }
  ReflectionRecord record{{}, {}, sizeof(MTL_SM50_SHADER_ARGUMENT), uint32_t(reflection.arguments.size())};
  memcpy(record.magic, kReflectionMagic, sizeof(record.magic));
  memcpy(record.layout, kReflectionLayout, sizeof(record.layout));
  std::string bytes(reinterpret_cast<const char *>(&record), sizeof(record));
  bytes.append(reinterpret_cast<const char *>(&reflection.reflection), sizeof(reflection.reflection));
  bytes.append(reinterpret_cast<const char *>(reflection.arguments.data()),
               reflection.arguments.size() * sizeof(MTL_SM50_SHADER_ARGUMENT));
  auto digest = Sha1HashState::compute(bytes.data(), bytes.size());
  bytes.append(reinterpret_cast<const char *>(digest.data), sizeof(digest));
  if (auto writer = getWriter())
    writer.set(ReflectionKey(shader), WMT::MakeDispatchData(bytes.data(), bytes.size()));
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