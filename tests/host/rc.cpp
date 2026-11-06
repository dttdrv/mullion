#include "../../src/util/rc/util_rc_ptr.hpp"

#include <cassert>
#include <utility>

struct RefCounted {
  unsigned references = 0;

  void incRef() { ++references; }
  void decRef() { --references; }
};

template <typename T> void move_assign(T &destination, T &source) {
  destination = std::move(source);
}

int main() {
  RefCounted object;
  {
    mullion::Rc<RefCounted> owner(&object);
    assert(object.references == 1);

    move_assign(owner, owner);
    assert(owner.ptr() == &object);
    assert(object.references == 1);
  }
  assert(object.references == 0);
}
