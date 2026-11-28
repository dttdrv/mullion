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

/* The router: which Direct3D generation answers a request, and why.

 * A game loads only the libraries its calls touch, but it also asks for generations it does not render with -- a
 * launcher that probes D3D9, a video player that makes a D3D9Ex device for Bink -- and a generation whose device
 * would not serve the title is better refused cleanly, with the error the API documents for "no such device here",
 * than half-offered.
 *
 * The rule set itself is mullion_router_policy.hpp, which needs nothing but its input so a host test can run every
 * rule. This header is the Windows side: it reads the configuration and the environment into that input, asks,
 * logs the answer with the rule that gave it, and appends the request and the answer to the per-title record in the
 * trace folder. It is header-only on purpose: d3d9.dll and d3d10core.dll link the Direct3D 11 and DXGI libraries and
 * the utility library, not the shared Mullion one, so a router they all consult has to be made of inline functions. */

#pragma once

#include "mullion_router_policy.hpp"

#include "config/config.hpp"
#include "log/log.hpp"
#include "log/trace.hpp"
#include "util_env.hpp"
#include "util_string.hpp"

#include <cstdio>

namespace mullion::router {

/* the process's bitness, which the loader has already chosen: a 64-bit process runs the 64-bit libraries and a
 * 32-bit one the 32-bit ones, so "which components load" needs no decision here -- only which generation answers,
 * and one generation's answer depends on it */
inline const char *
bitness() {
#ifdef __i386__
  return "32-bit";
#elif defined(__x86_64__) || defined(__arm64ec__)
  return "64-bit";
#else
  return "arm64";
#endif
}

inline bool
process_is_32_bit() {
#if defined(__i386__)
  return true;
#else
  return false;
#endif
}

namespace {

// what one word of the environment means. A refusal says so (0, false, off, no); "auto" means the same as not
// naming it, which is how a launcher can ask for this build's rule explicitly; anything else offers.
policy::Setting
parse_environment(const std::string &value) {
  if (value.empty())
    return policy::Setting::Follow;
  if (value == "auto" || value == "Auto")
    return policy::Setting::Follow;
  if (value == "0" || value == "false" || value == "FALSE" || value == "off" || value == "no")
    return policy::Setting::Refuse;
  return policy::Setting::Offer;
}

// the feature level a request named, as the record writes it: D3D_FEATURE_LEVEL's own major.minor
inline std::string
feature_level_text(uint32_t level) {
  if (!level)
    return "none";
  return std::to_string(level >> 12) + "_" + std::to_string((level >> 8) & 0xfu);
}

// the per-title record: one line a request, in the folder docs/tracing.md describes. It is written while any trace
// category is on or when MULLION_ROUTER_RECORD is set, so a plain run leaves no files behind, and it is appended to
// rather than rewritten because the libraries that answer are separate DLLs, each seeing part of the game.
inline void
record(const Request &request, const policy::Decision &decision) {
  if (!trace::Any() && env::getEnvVar("MULLION_ROUTER_RECORD").empty())
    return;
  const std::string directory = trace::WindowsDirectory();
  if (directory.empty())
    return;
  FILE *file = fopen((directory + "\\router.log").c_str(), "at");
  if (!file)
    return;
  fprintf(file, "%s %s feature_level=%s extended=%s on12=%s by=%s: %s\n", name(request.api),
          decision.offer ? "offer" : "refuse", feature_level_text(request.feature_level).c_str(),
          request.extended ? "1" : "0", request.on12 ? "1" : "0", policy::name(decision.source),
          decision.reason.c_str());
  fclose(file);
}

} // namespace

// the input the rule set works from: what the request said, what the configuration and the environment say, and
// what the process is
inline policy::Input
input_for(const Request &request) {
  policy::Input input;
  input.request = request;
  input.process_is_32_bit = process_is_32_bit();
  Config &config = Config::getInstance();
  for (unsigned at = 0; at < 4; at++) {
    const Api api = Api(at);
    // Tristate: Auto follows this build's rule, False refuses, True offers. The option comes from the merged
    // configuration, so a [App.exe] section or a built-in profile that names it is what the title gets.
    const Tristate setting = config.getOption<Tristate>((std::string("router.") + name(api)).c_str(), Tristate::Auto);
    input.settings[at] = setting == Tristate::False  ? policy::Setting::Refuse
                         : setting == Tristate::True ? policy::Setting::Offer
                                                     : policy::Setting::Follow;
    input.environment[at] = parse_environment(env::getEnvVar(environment_name(api).c_str()));
  }
  // `router.d3d12.bitness`: "refuse" applies this build's own rule to Direct3D 12 in a 32-bit process
  input.refuse_32_bit_d3d12 = config.getOption<std::string>("router.d3d12.bitness", "") == "refuse";
  return input;
}

// asks for a request an entry point can describe, and records what was answered and why
inline bool
offered(const Request &request) {
  const policy::Decision decision = policy::decide(input_for(request));
  record(request, decision);
  if (!decision.offer)
    Logger::warn(str::format("routing: ", decision.reason, " (", bitness(), ", ", env::getExeName(), ")"));
  else if (decision.source != policy::Source::Default)
    Logger::info(str::format("routing: ", decision.reason, " (", bitness(), ", ", env::getExeName(), ")"));
  return decision.offer;
}

// asks for the generation a bare entry point names: the form a call that sees no feature level and no Ex flag uses
inline bool
offered(Api api) {
  Request request;
  request.api = api;
  return offered(request);
}

/* what a device creation logs when it goes ahead, so a game's log says which generation answered it */
inline void
offered_log(Api api, const char *what) {
  Logger::info(str::format("routing: ", name(api), " (", bitness(), ") offers ", what));
}

} // namespace mullion::router
