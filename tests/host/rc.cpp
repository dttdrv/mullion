#include "../../src/util/rc/util_rc_ptr.hpp"

#include <cassert>
#include <utility>

struct RefCounted {
  unsigned references = 0;

  void incRef() { ++references; }
  void decRef() { --references; }
};

int main() {
  RefCounted object;
  {
    mullion::Rc<RefCounted> owner(&object);
    assert(object.references == 1);

    auto *source = &owner;
    owner = std::move(*source);
    assert(owner.ptr() == &object);
    assert(object.references == 1);
  }
  assert(object.references == 0);
}
