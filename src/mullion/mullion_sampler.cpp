#include "mullion_sampler.hpp"
#include "winemetal.h"
#include "airconv_public.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <unordered_set>
#include "log/log.hpp"
#include "thread.hpp"

namespace mullion {

// Metal holds maxArgumentBufferSamplerCount distinct samplers ("unique argument buffer samplers per app") and the
// ones past it sample wrong with no error (measured on an M1 Pro): the first of them says so. an id Metal gives again
// is the slot it reuses, so the ids seen count the slots taken
static void
Hold(WMT::Device device, uint64_t id) {
  static mullion::mutex mutex;
  static std::unordered_set<uint64_t> ids;
  std::lock_guard<mullion::mutex> lock(mutex);
  if (ids.insert(id).second && ids.size() == device.maxArgumentBufferSamplerCount() + 1)
    WARN("Sampler: past the ", ids.size() - 1, " distinct samplers Metal holds; the newer ones sample wrong");
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

// whether the device's samplers reduce as their reductionMode says: Metal ignores the mode unless the min, mag and mip
// filters are all linear (MTLSamplerDescriptor.reductionMode), and an M4 (Apple9, macOS 27.2) ignores it always. the
// bridge measures each device once, when its first sampler that reduces is made
static bool
NativeReduction(WMT::Device device) {
  bool honoured = device.supportsSamplerReduction();
  static std::atomic<bool> said = false;
  if (!said.exchange(true))
    Logger::info(str::format("Sampler: Metal's reductionMode ", honoured ? "is" : "is not",
                             " honoured; minimum and maximum filters are ",
                             honoured ? "Metal's where every filter is linear" : "emulated in shaders"));
  return honoured;
}

Rc<Sampler>
Sampler::createSampler(
    WMT::Device device, const WMTSamplerInfo &info, float lod_bias, const float *border_color, unsigned reduction
) {
  Rc<Sampler> ret = new Sampler();
  // the GPU keeps LOD clamps as 0 to 14 in steps of 1/64, rounded down, and anisotropy as a power of two: samplers
  // that differ only within one step share one Metal sampler
  auto step = [](float lod) { return std::floor(std::clamp(lod, 0.0f, 14.0f) * 64.0f) / 64.0f; };
  WMTSamplerInfo capped = info;
  capped.lod_min_clamp = step(capped.lod_min_clamp);
  capped.lod_max_clamp = step(capped.lod_max_clamp);
  capped.max_anisotroy = std::bit_ceil(std::max(capped.max_anisotroy, 1u));
  // a minimum or maximum filter is Metal's own where the device honours it, otherwise the shader's
  // (SM50_SAMPLER_REDUCTION)
  bool native = reduction != SM50_SAMPLER_REDUCTION_NONE && info.min_filter == WMTSamplerMinMagFilterLinear &&
                info.mag_filter == WMTSamplerMinMagFilterLinear && info.mip_filter == WMTSamplerMipFilterLinear &&
                NativeReduction(device);
  capped.reduction_mode = !native                                    ? WMTSamplerReductionModeWeightedAverage
                          : reduction == SM50_SAMPLER_REDUCTION_MINIMUM ? WMTSamplerReductionModeMinimum
                                                                     : WMTSamplerReductionModeMaximum;
  ret->info_ = capped;

  ret->sampler_state = device.newSamplerState(ret->info_);
  if (!ret->sampler_state) {
    return nullptr;
  }
  ret->sampler_state_handle = ret->info_.gpu_resource_id;
  Hold(device, ret->sampler_state_handle);

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
  ret->sampler_state_cube = device.newSamplerState(cube_sampler_info);
  if (!ret->sampler_state_cube) {
    return nullptr;
  }
  ret->sampler_state_cube_handle = cube_sampler_info.gpu_resource_id;
  Hold(device, ret->sampler_state_cube_handle);
  if (border_color) {
    WMTSamplerInfo white_info = capped;
    white_info.border_color = WMTSamplerBorderColorOpaqueWhite;
    ret->sampler_state_white = device.newSamplerState(white_info);
    if (!ret->sampler_state_white)
      return nullptr;
    ret->sampler_state_white_handle = white_info.gpu_resource_id;
    Hold(device, ret->sampler_state_white_handle);
    std::copy_n(border_color, 4, ret->border_color);
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
                  uint64_t(native ? SM50_SAMPLER_REDUCTION_NONE : reduction) << SM50_SAMPLER_METADATA_REDUCTION |
                  (uint64_t)address(info.r_address_mode) << SM50_SAMPLER_METADATA_ADDRESS_W |
                  field(info.lod_min_clamp) << SM50_SAMPLER_METADATA_MIN_LOD |
                  (field_max - field(info.lod_max_clamp)) << SM50_SAMPLER_METADATA_MAX_LOD |
                  (uint64_t)info.compare_function << SM50_SAMPLER_METADATA_COMPARE |
                  (uint64_t)(address(info.s_address_mode) + SM50_SAMPLER_ADDRESS_COUNT * address(info.t_address_mode))
                      << SM50_SAMPLER_METADATA_ADDRESS;

  return ret;
};

} // namespace mullion
