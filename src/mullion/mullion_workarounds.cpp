#include "mullion_workarounds.hpp"
#include "log/log.hpp"
#include "log/trace.hpp"
#include "util_env.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <sstream>
#include <string>

namespace mullion::workaround {

namespace {

constexpr std::array<const char *, size_t(Id::Count)> kNames = {
    "memory_limit",
};

struct Switches {
  std::array<bool, size_t(Id::Count)> disabled{};

  Switches() {
    std::stringstream list(env::getEnvVar("MULLION_DISABLE_WORKAROUNDS"));
    for (std::string name; std::getline(list, name, ',');) {
      if (name.empty())
        continue;
      bool known = name == "all";
      for (size_t i = 0; i < kNames.size(); i++)
        if (name == "all" || name == kNames[i]) {
          disabled[i] = true;
          known = true;
        }
      if (!known)
        WARN("MULLION_DISABLE_WORKAROUNDS: no workaround is named ", name);
      else if (name != "all")
        WARN("workaround ", name, " is off (MULLION_DISABLE_WORKAROUNDS)");
    }
    if (std::find(disabled.begin(), disabled.end(), false) == disabled.end() && kNames.size())
      WARN("every workaround is off (MULLION_DISABLE_WORKAROUNDS=all)");
  }
};

const Switches &
switches() {
  static const Switches instance;
  return instance;
}

std::array<std::atomic<bool>, size_t(Id::Count)> acted{};

} // namespace

const char *
Name(Id id) {
  return id < Id::Count ? kNames[size_t(id)] : "?";
}

bool
Enabled(Id id) {
  return id < Id::Count && !switches().disabled[size_t(id)];
}

void
Acted(Id id, const char *what) {
  if (id >= Id::Count)
    return;
  if (!acted[size_t(id)].exchange(true))
    WARN("workaround ", Name(id), ": ", what, " (MULLION_DISABLE_WORKAROUNDS=", Name(id), " turns it off)");
  else if (trace::Heavy(trace::Api))
    TRACE_IN(Api, "workaround ", Name(id), ": ", what);
}

} // namespace mullion::workaround
