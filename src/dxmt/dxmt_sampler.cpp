#include "dxmt_sampler.hpp"
#include "winemetal.h"
#include "airconv_public.h"
#include <algorithm>
#include <bit>
#include <cmath>

namespace dxmt {

void
Sampler::incRef() {
  refcount_.fetch_add(1u, std::memory_order_acquire);
};

void
Sampler::decRef() {
  if (refcount_.fetch_sub(1u, std::memory_order_release) == 1u)
    delete this;
};

Rc<Sampler>
Sampler::createSampler(WMT::Device device, const WMTSamplerInfo &info, float lod_bias, const float *border_color) {
  Rc<Sampler> ret = new Sampler();
  ret->info_ = info;

  ret->sampler_state = device.newSamplerState(ret->info_);
  if (!ret->sampler_state) {
    return nullptr;
  }
  ret->sampler_state_handle = ret->info_.gpu_resource_id;

  WMTSamplerInfo cube_sampler_info = info;
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
  if (border_color) {
    WMTSamplerInfo white_info = info;
    white_info.border_color = WMTSamplerBorderColorOpaqueWhite;
    ret->sampler_state_white = device.newSamplerState(white_info);
    if (!ret->sampler_state_white)
      return nullptr;
    ret->sampler_state_white_handle = white_info.gpu_resource_id;
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
                  field(info.lod_min_clamp) << SM50_SAMPLER_METADATA_MIN_LOD |
                  (field_max - field(info.lod_max_clamp)) << SM50_SAMPLER_METADATA_MAX_LOD |
                  (uint64_t)info.compare_function << SM50_SAMPLER_METADATA_COMPARE |
                  (uint64_t)(address(info.s_address_mode) + SM50_SAMPLER_ADDRESS_COUNT * address(info.t_address_mode))
                      << SM50_SAMPLER_METADATA_ADDRESS;

  return ret;
};

} // namespace dxmt
