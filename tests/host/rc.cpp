#include "../../src/util/rc/util_rc_ptr.hpp"

#include <cstdio>
#include <utility>

struct RefCounted {
  unsigned references = 0;

  void incRef() { ++references; }
  void decRef() { --references; }
};

// assert is compiled out of a release build (b_ndebug=if-release), where the test then passed on any Rc
#define CHECK(x)                                                                                                       \
  if (!(x))                                                                                                            \
    return std::printf("failed: %s\n", #x), 1

int main() {
  RefCounted object;
  {
    mullion::Rc<RefCounted> owner(&object);
    CHECK(object.references == 1);

    auto *source = &owner;
    owner = std::move(*source);
    CHECK(owner.ptr() == &object);
    CHECK(object.references == 1);
  }
  CHECK(object.references == 0);
  return 0;
}
