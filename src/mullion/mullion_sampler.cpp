#include "mullion_sampler.hpp"
#include "winemetal.h"
#include "airconv_public.h"
#include "wsi_platform.hpp"
#include "util_env.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_set>
#include "log/log.hpp"
#include "thread.hpp"

namespace mullion {

// Metal holds maxArgumentBufferSamplerCount distinct samplers ("unique argument buffer samplers per app") and the
// ones past it sample wrong with no error (measured on an M1 Pro). MULLION_SAMPLER_UNIQUE_LIMIT lowers that cap for
// tests. past the cap this build reuses the first Metal sampler rather than taking another unique slot
static uint64_t
uniqueSamplerLimit(WMT::Device device) {
  uint64_t max = device.maxArgumentBufferSamplerCount();
  auto asked_limit = env::getEnvVar("MULLION_SAMPLER_UNIQUE_LIMIT");
  if (!asked_limit.empty()) {
    char *end = nullptr;
    unsigned long asked = strtoul(asked_limit.c_str(), &end, 10);
    if (end != asked_limit.c_str() && asked)
      max = std::min(max, (uint64_t)asked);
  }
  return max ? max : 1;
}

static WMT::Reference<WMT::SamplerState>
makeOrReuseSampler(WMT::Device device, WMTSamplerInfo &info, uint64_t &out_id) {
  static mullion::mutex mutex;
  static std::unordered_set<uint64_t> ids;
  static WMT::Reference<WMT::SamplerState> fallback;
  static uint64_t fallback_id = 0;
  std::lock_guard<mullion::mutex> lock(mutex);
  const uint64_t limit = uniqueSamplerLimit(device);
  if (ids.size() >= limit && fallback) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      WARN("Sampler: reusing one Metal sampler past the ", limit, " distinct samplers it holds");
    }
    out_id = fallback_id;
    return fallback;
  }
  auto state = device.newSamplerState(info);
  if (!state)
    return {};
  out_id = info.gpu_resource_id;
  if (ids.insert(out_id).second && !fallback) {
    fallback = state;
    fallback_id = out_id;
  }
  return state;
}

void
Sampler::incRef() {
  refcount_.fetch_add(1u, std::memory_order_acquire);
};

void
Sampler::decRef() {
  if (refcount_.fetch_sub(1u, std::memory_order_release) == 1u)
    delete this;
};

Sampler::~Sampler() {
  border_buffer = {};
  wsi::aligned_free(border_host_);
}

Rc<Sampler>
Sampler::createSampler(WMT::Device device, const WMTSamplerInfo &info, float lod_bias, const float *border_color) {
  Rc<Sampler> ret = new Sampler();
  // the GPU keeps LOD clamps as 0 to 14 in steps of 1/64, rounded down, and anisotropy as a power of two: samplers
  // that differ only within one step share one Metal sampler
  auto step = [](float lod) { return std::floor(std::clamp(lod, 0.0f, 14.0f) * 64.0f) / 64.0f; };
  WMTSamplerInfo capped = info;
  capped.lod_min_clamp = step(capped.lod_min_clamp);
  capped.lod_max_clamp = step(capped.lod_max_clamp);
  capped.max_anisotroy = std::bit_ceil(std::max(capped.max_anisotroy, 1u));
  ret->info_ = capped;

  ret->sampler_state = makeOrReuseSampler(device, ret->info_, ret->sampler_state_handle);
  if (!ret->sampler_state) {
    return nullptr;
  }

  WMTSamplerInfo cube_sampler_info = capped;
  // it's probably still wrong, but the best result we can get
  if (cube_sampler_info.min_filter == WMTSamplerMinMagFilterLinear &&
      cube_sampler_info.mag_filter == WMTSamplerMinMagFilterLinear) {
    cube_sampler_info.s_address_mode = WMTSamplerAddressModeClampToBorderColor;
    cube_sampler_info.t_address_mode = WMTSamplerAddressModeClampToBorderColor;
    cube_sampler_info.r_address_mode = WMTSamplerAddressModeClampToBorderColor;
  } else {
    cube_sampler_info.s_address_mode = WMTSamplerAddressModeClampToEdge;
    cube_sampler_info.t_address_mode = WMTSamplerAddressModeClampToEdge;
    cube_sampler_info.r_address_mode = WMTSamplerAddressModeClampToEdge;
  }
  ret->sampler_state_cube = makeOrReuseSampler(device, cube_sampler_info, ret->sampler_state_cube_handle);
  if (!ret->sampler_state_cube) {
    return nullptr;
  }
  if (border_color) {
    WMTSamplerInfo white_info = capped;
    white_info.border_color = WMTSamplerBorderColorOpaqueWhite;
    ret->sampler_state_white = makeOrReuseSampler(device, white_info, ret->sampler_state_white_handle);
    if (!ret->sampler_state_white)
      return nullptr;
    std::copy_n(border_color, 4, ret->border_color);
    ret->border_host_ = wsi::aligned_malloc(MULLION_PAGE_SIZE, MULLION_PAGE_SIZE);
    if (ret->border_host_) {
      std::memset(ret->border_host_, 0, MULLION_PAGE_SIZE);
      auto *gpu_border = reinterpret_cast<SM50_SAMPLER_BORDER *>(ret->border_host_);
      gpu_border->white_sampler = ret->sampler_state_white_handle;
      std::copy_n(border_color, 4, gpu_border->color);
      WMTBufferInfo buf{};
      buf.length = MULLION_PAGE_SIZE;
      buf.memory.set(ret->border_host_);
      buf.options = (WMTResourceOptions)(WMTResourceStorageModeShared | WMTResourceHazardTrackingModeUntracked);
      ret->border_buffer = device.newBuffer(buf);
      ret->border_gpu_address = buf.gpu_address;
    }
  }
  constexpr uint64_t field_max = (1 << SM50_SAMPLER_METADATA_LOD_BITS) - 1;
  auto field = [](float lod) {
    return (uint64_t)std::lround(std::clamp(std::ldexp(lod, SM50_SAMPLER_METADATA_LOD_FRACTION_BITS), 0.0f, (float)field_max));
  };
  // both of Metal's borders are one mode to a shader that only needs to know where a coordinate lands
  auto address = [](WMTSamplerAddressMode mode) {
    return mode == WMTSamplerAddressModeClampToBorderColor ? SM50_SAMPLER_ADDRESS_BORDER : (uint64_t)mode;
  };
  auto bias = (uint16_t)(int16_t)std::lround(
      std::clamp(std::ldexp(lod_bias, SM50_SAMPLER_METADATA_LOD_FRACTION_BITS), (float)INT16_MIN, (float)INT16_MAX)
  );
  ret->metadata = bias | uint64_t(info.min_filter == WMTSamplerMinMagFilterLinear) << SM50_SAMPLER_METADATA_MIN_LINEAR |
                  uint64_t(info.mag_filter == WMTSamplerMinMagFilterLinear) << SM50_SAMPLER_METADATA_MAG_LINEAR |
                  uint64_t(info.mip_filter == WMTSamplerMipFilterLinear) << SM50_SAMPLER_METADATA_MIP_LINEAR |
                  (uint64_t)info.max_anisotroy << SM50_SAMPLER_METADATA_ANISOTROPY |
                  field(info.lod_min_clamp) << SM50_SAMPLER_METADATA_MIN_LOD |
                  (field_max - field(info.lod_max_clamp)) << SM50_SAMPLER_METADATA_MAX_LOD |
                  (uint64_t)info.compare_function << SM50_SAMPLER_METADATA_COMPARE |
                  (uint64_t)(address(info.s_address_mode) + SM50_SAMPLER_ADDRESS_COUNT * address(info.t_address_mode))
                      << SM50_SAMPLER_METADATA_ADDRESS;

  return ret;
};

} // namespace mullion
