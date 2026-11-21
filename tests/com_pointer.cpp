// Deferred D3D11 queries and D3D12 state objects keep COM references in vectors. Growing the vector
// transfers its references without entering the object to acquire and release each one again.
#include "../src/util/com/com_pointer.hpp"
#include <array>
#include <cstdio>
#include <vector>

struct Counted {
  unsigned adds = 0, releases = 0;
  void AddRef() { ++adds; }
  void Release() { ++releases; }
};

int main() {
  std::array<Counted, 32> objects{};
  {
    std::vector<mullion::Com<Counted>> owners;
    owners.reserve(objects.size());
    for (auto &object : objects)
      owners.emplace_back(&object);
    owners.reserve(owners.capacity() + 1);
    for (size_t i = 0; i < objects.size(); ++i)
      if (owners[i].ptr() != &objects[i] || objects[i].adds != 1 || objects[i].releases != 0)
        return std::printf("wrong: object %zu after growth has %u AddRef and %u Release calls\n", i, objects[i].adds,
                           objects[i].releases), 1;
  }
  for (auto &object : objects)
    if (object.releases != 1)
      return std::printf("wrong: an object was released %u times\n", object.releases), 1;
  std::puts("passed");
}
