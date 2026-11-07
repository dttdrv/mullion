#include "../../src/airconv/packed_variant_list.hpp"
int main() {
  mullion::PackedVariantList<int, float> source;
  source.push_back(42);
  source.push_back(1.5f);

  mullion::PackedVariantList<int, float> copy;
  copy.push_back(-1);
  copy = source;

  int integer = 0;
  float real = 0;
  copy.for_each([&](const auto &value) {
    using T = std::remove_cvref_t<decltype(value)>;
    if constexpr (std::is_same_v<T, int>)
      integer = value;
    else
      real = value;
  });
  if (integer != 42 || real != 1.5f)
    return 1;

  auto *self = &copy;
  copy = *self;
  integer = 0;
  copy.for_each([&](const auto &value) {
    if constexpr (std::is_same_v<std::remove_cvref_t<decltype(value)>, int>)
      integer = value;
  });
  return integer == 42 ? 0 : 1;
}
