#pragma once
#include <cstdint>
#include <vector>

namespace mullion {

// the tessellation templates a device's tessellated draws read (SM50_BINDING_INDEX_TESSELLATION_TEMPLATES), as the
// words of a buffer: made once a process, from the tessellator's own source (air_tessellation.metal's tess_template)
const std::vector<uint32_t> &TessellationTemplates();

} // namespace mullion
