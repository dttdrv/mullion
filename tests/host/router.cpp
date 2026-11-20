// contract: which Direct3D generation the router answers, and which input made it answer. The rules are the whole of
// mullion_router_policy.hpp's decide(), run here as the pure data they are: no configuration file, no Wine, no GPU.
//
// The precedence the router promises (docs/tracing.md, mullion.conf): the environment beats the configuration, the
// configuration beats what this build can do, and what this build can do beats offering. A refusal is only worth
// anything if a person can see which of the four said so, so every case checks the source as well as the answer.
#include "../../src/mullion/mullion_router_policy.hpp"

#include <cstdio>
#include <string>

namespace {

using namespace mullion::router;
using namespace mullion::router::policy;

unsigned failures = 0, checks = 0;

void
check(const Decision &decision, bool offer, Source source, const char *what) {
  checks++;
  if (decision.offer != offer || decision.source != source) {
    failures++;
    std::printf("wrong: %s: got \"%s\" (%s), want offer=%d (%s)\n", what, decision.reason.c_str(),
                name(decision.source), (int)offer, name(source));
  }
  // a refusal that does not say why is the failure mode the router exists to avoid
  if (decision.reason.empty())
    failures++, std::printf("wrong: %s: the decision carries no reason\n", what);
}

Input
input(Api api, bool is_32_bit = false) {
  Input in;
  in.request.api = api;
  in.process_is_32_bit = is_32_bit;
  return in;
}

} // namespace

int
main() {
  // a layer of this build asking another of its own layers never reaches these rules at all: it comes in through
  // d3d11.dll's internal entry (d3d11_interfaces.hpp's mullion_d3d11_create_device), which is not an application's
  // API and so has nothing to route. The contract test for that is the GPU one: with router.d3d11 = False a Direct3D
  // 9 device still renders (tests/dxgi/dxgi_router.cpp), which is what the entry exists for.

  // the default: every generation is offered, by the rule that says nothing was named
  for (unsigned at = 0; at < 4; at++)
    check(decide(input(Api(at))), true, Source::Default, "an unnamed generation is offered");

  // what this build can do: Direct3D 12 in a 32-bit process, when the rule is turned on
  {
    Input in = input(Api::D3D12, true);
    in.refuse_32_bit_d3d12 = true;
    check(decide(in), false, Source::Derived, "the 32-bit Direct3D 12 rule refuses");
  }
  // ... and only that generation, only there
  for (Api api : {Api::D3D9, Api::D3D10, Api::D3D11}) {
    Input in = input(api, true);
    in.refuse_32_bit_d3d12 = true;
    check(decide(in), true, Source::Default, "the 32-bit rule does not reach another generation");
  }
  {
    Input in = input(Api::D3D12, false);
    in.refuse_32_bit_d3d12 = true;
    check(decide(in), true, Source::Default, "the 32-bit rule does not reach a 64-bit process");
  }
  {
    Input in = input(Api::D3D12, true);
    check(decide(in), true, Source::Default, "the 32-bit rule is off until it is asked for");
  }

  // the configuration wins over the rule, in both directions
  {
    Input in = input(Api::D3D12, true);
    in.refuse_32_bit_d3d12 = true;
    in.settings[unsigned(Api::D3D12)] = Setting::Offer;
    check(decide(in), true, Source::Config, "router.d3d12=On offers what the rule would refuse");
  }
  {
    Input in = input(Api::D3D9);
    in.settings[unsigned(Api::D3D9)] = Setting::Refuse;
    check(decide(in), false, Source::Config, "router.d3d9=false refuses");
  }
  // naming one generation says nothing about the others
  {
    Input in = input(Api::D3D11);
    in.settings[unsigned(Api::D3D9)] = Setting::Refuse;
    check(decide(in), true, Source::Default, "a setting for another generation does not answer this one");
  }

  // the environment wins over the configuration, in both directions
  {
    Input in = input(Api::D3D12, true);
    in.refuse_32_bit_d3d12 = true;
    in.settings[unsigned(Api::D3D12)] = Setting::Refuse;
    in.environment[unsigned(Api::D3D12)] = Setting::Offer;
    check(decide(in), true, Source::Environment, "MULLION_ROUTER_D3D12 overrides router.d3d12");
  }
  {
    Input in = input(Api::D3D9);
    in.settings[unsigned(Api::D3D9)] = Setting::Offer;
    in.environment[unsigned(Api::D3D9)] = Setting::Refuse;
    check(decide(in), false, Source::Environment, "MULLION_ROUTER_D3D9=0 refuses what the file offered");
  }
  // an environment that asks for the build's own rule is the same as naming nothing
  {
    Input in = input(Api::D3D11);
    in.environment[unsigned(Api::D3D11)] = Setting::Follow;
    check(decide(in), true, Source::Default, "an environment left at auto follows the rule");
  }

  // the reason names the input that gave the answer, which is what a person reads in a log
  {
    Input in = input(Api::D3D10);
    in.settings[unsigned(Api::D3D10)] = Setting::Refuse;
    decide(in);
    checks++;
    if (decide(in).reason.find("router.d3d10") == std::string::npos)
      failures++, std::printf("wrong: the configuration's reason does not name the option: \"%s\"\n",
                              decide(in).reason.c_str());
  }
  {
    Input in = input(Api::D3D11);
    in.environment[unsigned(Api::D3D11)] = Setting::Refuse;
    checks++;
    if (decide(in).reason.find("MULLION_ROUTER_D3D11") == std::string::npos)
      failures++, std::printf("wrong: the environment's reason does not name the variable: \"%s\"\n",
                              decide(in).reason.c_str());
  }

  // a request an entry point describes does not change which generation answers: the Ex and On12 forms and the
  // feature levels are recorded, not routed on, so a launcher probing D3D9Ex is answered like any other D3D9 call
  for (bool extended : {false, true})
    for (bool on12 : {false, true})
      for (uint32_t level : {0u, 0xc000u, 0xd0000u}) {
        Input in = input(Api::D3D9);
        in.request.extended = extended;
        in.request.on12 = on12;
        in.request.feature_level = level;
        check(decide(in), true, Source::Default, "the form of a request does not route it away");
      }

  std::printf("%s: %u wrong of %u checks\n", failures ? "failed" : "passed", failures, checks);
  return failures != 0;
}
