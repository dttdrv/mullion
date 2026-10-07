// contract: the tests' own container hash (tests/dxbc_hash.hpp) is the hash Microsoft's compilers write. every
// container found in the directories given (as arguments, or separated by ':' in MULLION_HASHED) either has no hash
// (16 zero bytes: what DXC writes without its signing library), the hash that says "do not check" (16 bytes of 1:
// HLSL specification INF-0004, Validator Hashing), or the hash of its own bytes. the hash ends in one of two ways,
// by how many bytes do not fill a block: containers of both kinds have to be among those found, or the test has
// not seen what it is for.
#include "../dxbc_hash.hpp"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <vector>

int
main(int argc, char **argv) {
  std::vector<std::string> places(argv + 1, argv + argc);
  if (auto named = getenv("MULLION_HASHED")) {
    std::stringstream list(named);
    for (std::string place; std::getline(list, place, ':');)
      places.push_back(place);
  }
  if (places.empty()) {
    printf("skipped: no directories of shader containers named\n");
    return 77;
  }
  unsigned held = 0, wrong = 0, unsigned_ones = 0, unchecked = 0, alone = 0;
  std::set<size_t> remainders;
  for (auto &place : places)
    for (auto &entry : std::filesystem::recursive_directory_iterator(place)) {
      if (!entry.is_regular_file())
        continue;
      std::ifstream file(entry.path(), std::ios::binary);
      std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
      if (bytes.size() < dxbc::hashed_from || bytes.compare(0, 4, "DXBC"))
        continue;
      dxbc::Hash given;
      memcpy(given.data(), bytes.data() + dxbc::hash_at, given.size());
      auto all = [&](uint8_t value) { return std::all_of(given.begin(), given.end(), [=](uint8_t byte) { return byte == value; }); };
      if (all(0) || all(1)) {
        (all(0) ? unsigned_ones : unchecked)++;
        continue;
      }
      const size_t rest = (bytes.size() - dxbc::hashed_from) % 64;
      if (dxbc::hash(bytes.data(), bytes.size()) == given)
        held++, remainders.insert(rest), alone += rest >= 56;
      else if (wrong++ < 8)
        printf("wrong: %s (%zu bytes, %zu past a block) does not carry the hash the tests give it\n", entry.path().c_str(), bytes.size(), rest);
    }
  const bool both = alone && alone < held;
  if (!both)
    printf("wrong: of %u containers %u end in a block of their own: both endings have to be seen\n", held, alone);
  printf("%s: %u containers carry the tests' hash (%u with the ending in a block of its own, %zu different numbers of bytes past a block), "
         "%u do not; %u without a hash, %u not to be checked\n",
         wrong || !both ? "failed" : "passed", held, alone, remainders.size(), wrong, unsigned_ones, unchecked);
  return wrong || !both;
}
