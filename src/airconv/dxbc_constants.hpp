#pragma once

#include <cstdint>

namespace dxmt::dxbc {

enum class RegisterComponentType : uint32_t {
  Unknown,
  Uint = 1,
  Int = 2,
  Float = 3,
  // native 16-bit types (shader model 6.2)
  Uint16 = 4,
  Int16 = 5,
  Float16 = 6,
};

// a register holds 16-bit types widened
constexpr RegisterComponentType
widened(RegisterComponentType type) {
  switch (type) {
  case RegisterComponentType::Uint16:
    return RegisterComponentType::Uint;
  case RegisterComponentType::Int16:
    return RegisterComponentType::Int;
  case RegisterComponentType::Float16:
    return RegisterComponentType::Float;
  default:
    return type;
  }
}

#define _UNREACHABLE assert(0 && "unreachable");

/* anything in CAPITAL is not handled properly yet (rename in progress) */

} // namespace dxmt::dxbc
