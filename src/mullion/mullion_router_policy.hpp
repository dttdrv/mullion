/*
 * Copyright 2026 the Mullion contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/* The router's rule set, on its own: no configuration, no logger, no Wine, no GPU. What a generation is offered for
 * is decided here and nowhere else, so every rule can be run by a host test (tests/host/router.cpp) and read by
 * whoever has to answer for a refusal. mullion_router.hpp is the Windows side that feeds it.

 * Every input here comes from what the application asked for. This build's own layer-to-layer calls are not routed
 * at all and do not ask: the Direct3D 9 device is made over the Direct3D 11 one through d3d11.dll's internal entry
 * (d3d11_interfaces.hpp's mullion_d3d11_create_device), not through the API's D3D11CreateDevice, because a flag
 * carried across the boundary between two DLLs cannot be a process's state -- the static a header-inline function
 * keeps is one per image, so a scope set in d3d9.dll is not the one d3d11.dll reads. */

#pragma once

#include <cstdint>
#include <string>

namespace mullion::router {

enum class Api { D3D9, D3D10, D3D11, D3D12 };

inline const char *
name(Api api) {
  switch (api) {
  case Api::D3D9: return "d3d9";
  case Api::D3D10: return "d3d10";
  case Api::D3D11: return "d3d11";
  case Api::D3D12: return "d3d12";
  }
  return "?";
}

/* what a request is, as far as the entry point that received it can see. The Ex and On12 forms are carried because a
 * title that asks for one is telling us what it expects; neither changes which generation answers. */
struct Request {
  Api api = Api::D3D11;
  bool extended = false;
  bool on12 = false;
  // the highest feature level the request names, as a D3D_FEATURE_LEVEL value; 0 when it names none
  uint32_t feature_level = 0;
};

// a generation's name as the environment spells it: the variables are the API's capitals, the configuration option
// its lowercase, and a reason that names the wrong one of the two sends a reader to a variable that does not exist
inline std::string
environment_name(Api api) {
  std::string out = "MULLION_ROUTER_";
  for (const char *c = name(api); *c; c++)
    out += char(*c >= 'a' && *c <= 'z' ? *c - 32 : *c);
  return out;
}

namespace policy {

/* which kind of input gave the answer. Precedence, highest first: the environment, then the configuration (which has
 * already merged the built-in per-title profile and the mullion.conf [App.exe] section), then what this build can
 * actually do, then offering. */
enum class Source : uint8_t { Default, Derived, Config, Environment };

inline const char *
name(Source source) {
  switch (source) {
  case Source::Default: return "default";
  case Source::Derived: return "this build";
  case Source::Config: return "configuration";
  case Source::Environment: return "environment";
  }
  return "?";
}

// a generation's setting: follow this build's rule, refuse it, or offer it whatever the rule says
enum class Setting : uint8_t { Follow, Refuse, Offer };

struct Input {
  // what `router.d3d9` and its siblings say, merged from mullion.conf, its [App.exe] section and the built-in
  // profiles; Follow where none of them names it
  Setting settings[4] = {Setting::Follow, Setting::Follow, Setting::Follow, Setting::Follow};
  // what MULLION_ROUTER_* says; Follow when the variable is not set or says auto
  Setting environment[4] = {Setting::Follow, Setting::Follow, Setting::Follow, Setting::Follow};
  Request request;
  bool process_is_32_bit = false;
  // `router.d3d12.bitness = refuse`: this build does not render Direct3D 12 in a 32-bit process, and a title that
  // is refused takes its next renderer. It is off by default only because the 283 Direct3D 12 tests each make their
  // device with a CHECK, so refusing by default turns 283 results into unclear failures instead of the named skips
  // docs/TESTING.md 6.2 requires; turning it on is the step after those tests ask for a device through a helper
  // that skips. See docs/MULLION_REQUIREMENTS.md, "PE32 Direct3D 12".
  bool refuse_32_bit_d3d12 = false;
};

struct Decision {
  bool offer = true;
  Source source = Source::Default;
  // what was decided and which rule gave it, as one line for the log and the record
  std::string reason;
};

inline Decision
decide(const Input &input) {
  const unsigned which = unsigned(input.request.api);
  const std::string api(name(input.request.api));
  Decision decision;

  // the environment first: it is what a launcher or a person sets to mean "this, now", for one process
  if (input.environment[which] != Setting::Follow) {
    decision.offer = input.environment[which] == Setting::Offer;
    decision.source = Source::Environment;
    decision.reason = api + " is " + (decision.offer ? "offered" : "refused") + " by " + environment_name(input.request.api);
    return decision;
  }

  // then the configuration, which is where a per-title profile speaks
  if (input.settings[which] != Setting::Follow) {
    decision.offer = input.settings[which] == Setting::Offer;
    decision.source = Source::Config;
    decision.reason = api + " is " + (decision.offer ? "offered" : "refused") + " by router." + api;
    return decision;
  }

  // then what this build can do. Direct3D 12 in a 32-bit process is the one generation this build does not render
  // with: the commands it records reach the GPU identical to the 64-bit ones and the readbacks come back as the clear
  // (docs/MULLION_REQUIREMENTS.md, "PE32 Direct3D 12"). A refused title takes its next renderer; an offered one
  // gets hundreds of suites of cleared frames and no way to notice.
  if (input.refuse_32_bit_d3d12 && input.request.api == Api::D3D12 && input.process_is_32_bit) {
    decision.offer = false;
    decision.source = Source::Derived;
    decision.reason =
        api + " is refused: this build does not render Direct3D 12 in a 32-bit process";
    return decision;
  }

  decision.offer = true;
  decision.source = Source::Default;
  decision.reason = api + " is offered";
  return decision;
}

} // namespace policy
} // namespace mullion::router
