#pragma once
#include "Metal.hpp"
#include "airconv_public.h"
#include <string>

namespace mullion {

std::string GetVersionDescriptionText(uint32_t ApiVersion, uint32_t FeatureLevel);

// the Metal language version shaders are converted for: the newest the system and the GPU have (3.2 from macOS 15 on
// an Apple GPU), or an older one that mullion.shaderMetalVersion asks for. memory barriers across threadgroups and
// globally coherent accesses exist from 3.2 on, so every API takes it from here
WMTMetalVersion ShaderMetalVersion(WMT::Device device);

// the shader compiler's flags for what this GPU lacks, or MULLION_SHADER_FLAGS when set (a number, 0 for none)
SM50_SHADER_FLAG GPUShaderFlags(WMT::Device device);

// the mesh memory a draw's mesh threadgroups share, 4 GiB on Apple7 and Apple8 GPUs (M1, M2), or 2 KB for each of
// MULLION_MAX_MESH_THREADGROUPS (0: none)
uint64_t MeshBytesPerDraw(WMT::Device device);

// the most threadgroups a mesh grid of the device may have. Metal tells it only of a pipeline (MTLRenderPipelineState's
// maxTotalThreadgroupsPerMeshGrid), which has the limit its functions declare or, of functions that declare none, the
// device's, whatever else the pipeline is: so this asks a pipeline of one mesh function that outputs nothing. 0 when
// that pipeline cannot be made
uint32_t MeshGridLimit(WMT::Device device);

}; // namespace mullion
