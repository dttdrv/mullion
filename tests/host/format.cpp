// contract: str::format writes a byte as its number, as it writes every other integer (a sample count of 8 is "8",
// not the character 8, backspace), and a char still as its character
#include "../../src/util/util_string.hpp"
#include <cstdio>

int
main() {
  using mullion::str::format;
  unsigned failures = 0;
  auto check = [&](const std::string &got, const std::string &want) {
    if (got != want)
      failures++, std::printf("got \"%s\", want \"%s\"\n", got.c_str(), want.c_str());
  };
  for (unsigned value : {0u, 8u, 65u, 255u}) {
    check(format(uint8_t(value)), std::to_string(value));
    check(format("x", uint8_t(value), 'y'), "x" + std::to_string(value) + "y");
  }
  check(format('a', "b"), "ab");
  std::printf("%s: %u wrong results\n", failures ? "failed" : "passed", failures);
  return failures != 0;
}
