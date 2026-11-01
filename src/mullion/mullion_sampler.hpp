#pragma once
#include "Metal.hpp"
#include "rc/util_rc_ptr.hpp"
#include <atomic>

namespace mullion {

class Sampler {
public:
  void incRef();
  void decRef();

  /* readonly */
  // what shaders read next to the sampler states (SM50_SAMPLER_METADATA)
  uint64_t metadata;
  WMT::Reference<WMT::SamplerState> sampler_state;
  WMT::Reference<WMT::SamplerState> sampler_state_cube;
  uint64_t sampler_state_handle;
  uint64_t sampler_state_cube_handle;
  // a custom border color: `info` borders in transparent black and this twin in opaque white (SM50_SAMPLER_BORDER)
  WMT::Reference<WMT::SamplerState> sampler_state_white;
  uint64_t sampler_state_white_handle = 0;
  float border_color[4] = {};

  // `reduction` (SM50_SAMPLER_REDUCTION): a minimum or maximum filter, which Metal's sampler applies where it can and
  // the shader emulates elsewhere (the metadata says which)
  static Rc<Sampler> createSampler(
      WMT::Device device, const WMTSamplerInfo &info, float lod_bias, const float *border_color = nullptr,
      unsigned reduction = 0
  );

private:
  WMTSamplerInfo info_;
  std::atomic<uint32_t> refcount_ = {0u};
};

} // namespace mullion