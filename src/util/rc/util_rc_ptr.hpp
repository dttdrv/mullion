/*
 * This file is part of DXMT, Copyright (c) 2023 Feifan He
 *
 * Derived from a part of DXVK (originally under zlib License),
 * Copyright (c) 2017 Philip Rebohle
 * Copyright (c) 2019 Joshua Ashton
 *
 * See <https://github.com/doitsujin/dxvk/blob/master/LICENSE>
 */
 
#pragma once

#include <ostream>
#include "../util_likely.hpp"

namespace mullion {

/**
 * \brief Pointer for reference-counted objects
 *
 * This only requires the given type to implement \c incRef
 * and \c decRef methods that adjust the reference count.
 * \tparam T Object type
 */
template <typename T> class Rc {
  template <typename Tx> friend class Rc;

public:
  Rc() {}
  Rc(std::nullptr_t) {}

  Rc(T *object) : m_object(object) {
    this->incRef();
  }

  Rc(const Rc &other) : m_object(other.m_object) {
    this->incRef();
  }

  template <std::derived_from<T> Tx> Rc(const Rc<Tx> &other) : m_object(other.m_object) {
    this->incRef();
  }

  Rc(Rc &&other) noexcept : m_object(other.m_object) {
    other.m_object = nullptr;
  }

  template <std::derived_from<T> Tx> Rc(Rc<Tx> &&other) noexcept : m_object(other.m_object) {
    other.m_object = nullptr;
  }

  Rc &operator=(std::nullptr_t) {
    this->decRef();
    m_object = nullptr;
    return *this;
  }

  Rc &operator=(const Rc &other) {
    if (this == &other)
      return *this;
    other.incRef();
    this->decRef();
    m_object = other.m_object;
    return *this;
  }

  template <std::derived_from<T> Tx> Rc &operator=(const Rc<Tx> &other) {
    if (static_cast<const void *>(this) == static_cast<const void *>(&other))
      return *this;
    other.incRef();
    this->decRef();
    m_object = other.m_object;
    return *this;
  }

  Rc &operator=(Rc &&other) noexcept {
    if (this != &other) {
      this->decRef();
      this->m_object = other.m_object;
      other.m_object = nullptr;
    }
    return *this;
  }

  template <std::derived_from<T> Tx> Rc &operator=(Rc<Tx> &&other) noexcept {
    if (static_cast<const void *>(this) != static_cast<const void *>(&other)) {
      this->decRef();
      this->m_object = other.m_object;
      other.m_object = nullptr;
    }
    return *this;
  }

  ~Rc() {
    this->decRef();
  }

  T &operator*() const { return *m_object; }
  constexpr T *operator->() const { return m_object; }
  constexpr T *ptr() const { return m_object; }

  operator bool() const { return this->m_object != nullptr; }

  bool operator==(const Rc &other) const { return m_object == other.m_object; }
  bool operator!=(const Rc &other) const { return m_object != other.m_object; }

  bool operator==(std::nullptr_t) const { return m_object == nullptr; }
  bool operator!=(std::nullptr_t) const { return m_object != nullptr; }

private:
  T *m_object = nullptr;

  force_inline void incRef() const {
    if (m_object != nullptr)
      m_object->incRef();
  }

  force_inline void decRef() const {
    if (m_object != nullptr) {
      m_object->decRef();
    }
  }
};

} // namespace mullion

template <typename T>
std::ostream &operator<<(std::ostream &os, const mullion::Rc<T> &rc) {
  return os << rc.ptr();
}
