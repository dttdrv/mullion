// contract: SM50CompileRayDispatcher gives a Metal library that defines the function a ray tracing pipeline's runtime
// calls its shaders of one kind through (mullion_command.metal: mullion_ray_generation_shader, _closest_, _any_hit_,
// _intersection_ and _callable_shader), whichever shaders it is given, and refuses a dispatcher that is none of the
// five and a shader with no name. a Metal library starts with "MTLB" and lists the functions it defines by name.
#include "airconv_public.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

static bool
compile(uint32_t dispatcher, const std::vector<SM50_RAY_DISPATCHED_SHADER> &shaders, bool table, std::string &library) {
  sm50_bitcode_t bitcode = nullptr;
  sm50_error_t error = nullptr;
  if (SM50CompileRayDispatcher(
          SM50_RAY_DISPATCHER(dispatcher), shaders.data(), shaders.size(), table, SM50_SHADER_METAL_310, &bitcode, &error
      )) {
    if (error)
      SM50FreeError(error);
    return false;
  }
  SM50_COMPILED_BITCODE compiled;
  SM50GetCompiledBitcode(bitcode, &compiled);
  library.assign((const char *)compiled.Data, compiled.Size);
  SM50DestroyBitcode(bitcode);
  return true;
}

int
main(int argc, char **argv) {
  static const char *const dispatchers[SM50_RAY_DISPATCHER_COUNT] = {
      "mullion_ray_generation_shader", "mullion_ray_closest_shader", "mullion_ray_any_hit_shader",
      "mullion_ray_intersection_shader", "mullion_ray_callable_shader",
  };
  unsigned wrong = 0;
  auto expect = [&](bool held, const char *what) {
    if (!held) {
      printf("wrong: %s\n", what);
      wrong++;
    }
  };
  // a slot twice is one case
  const std::vector<SM50_RAY_DISPATCHED_SHADER> shaders = {
      {5, "mullion_ray_shader_closest"}, {9, "mullion_ray_shader_miss"}, {9, "mullion_ray_shader_miss"}};
  for (uint32_t dispatcher = 0; dispatcher < SM50_RAY_DISPATCHER_COUNT; dispatcher++)
    for (bool table : {false, true})
      for (bool empty : {false, true}) {
        std::string library;
        if (!compile(dispatcher, empty ? std::vector<SM50_RAY_DISPATCHED_SHADER>{} : shaders, table, library)) {
          expect(false, "a dispatcher was not compiled");
          continue;
        }
        expect(std::string_view(library).starts_with("MTLB"), "a dispatcher is no Metal library");
        for (uint32_t other = 0; other < SM50_RAY_DISPATCHER_COUNT; other++)
          expect((library.find(dispatchers[other]) != std::string::npos) == (other == dispatcher), dispatchers[other]);
        expect((library.find(shaders[0].name) != std::string::npos) == !empty, "the direct calls of its shaders");
        // for reading with Metal's tools
        if (argc > 1 && dispatcher == SM50_RAY_DISPATCHER_CLOSEST && table && !empty)
          if (auto file = fopen(argv[1], "wb")) {
            fwrite(library.data(), 1, library.size(), file);
            fclose(file);
          }
      }
  std::string library;
  expect(!compile(SM50_RAY_DISPATCHER_COUNT, shaders, false, library), "a dispatcher of no kind was compiled");
  expect(!compile(SM50_RAY_DISPATCHER_CLOSEST, {{4, ""}}, false, library), "a shader with no name was taken");
  SM50_RAY_DISPATCHED_SHADER unterminated{4};
  memset(unterminated.name, 'a', sizeof(unterminated.name));
  expect(!compile(SM50_RAY_DISPATCHER_CLOSEST, {unterminated}, false, library), "a name with no end was taken");
  printf("%s: %u wrong results\n", wrong ? "failed" : "passed", wrong);
  return wrong != 0;
}
