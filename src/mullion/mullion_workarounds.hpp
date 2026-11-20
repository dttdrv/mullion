#pragma once
#include <cstdint>

namespace mullion::workaround {

// Mullion's workarounds for faults in Apple's Metal stack (docs/workarounds.md): each has a name, applies only where
// its fault was seen, and MULLION_DISABLE_WORKAROUNDS turns it off, a comma-separated list of names or `all`, to see
// whether a macOS or GPU still needs it (as LunarG's KosmicKrisp does for its own, docs.mesa3d.org's workarounds)
enum class Id : uint32_t {
  // allocating past recommendedMaxWorkingSetSize and making it resident panics the kernel on macOS 26 (KosmicKrisp's
  // KK_WORKAROUND_20, fixed in macOS 27): application allocations past 95% of it are refused before macOS 27
  MemoryLimit,
  Count,
};

// the workaround's name, as MULLION_DISABLE_WORKAROUNDS and the log name it
const char *Name(Id id);

// whether the workaround is on: MULLION_DISABLE_WORKAROUNDS does not name it (read once)
bool Enabled(Id id);

// the first time a workaround changes what happens, a line in the log says so, and every time with
// MULLION_TRACE_API=heavy
void Acted(Id id, const char *what);

} // namespace mullion::workaround
