// Deny allocations in the setters without exhausting the host. A failed replacement leaves the old value in place,
// and an interface whose entry could not be allocated has only its caller's reference.
#include "com/com_private_data.hpp"
#include <cstdio>
#include <cstdlib>
#include <new>

static bool deny_allocations;
void *operator new(size_t size) {
  if (!deny_allocations)
    if (void *p = std::malloc(size ? size : 1))
      return p;
  throw std::bad_alloc();
}
void *operator new[](size_t size) { return ::operator new(size); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, size_t) noexcept { std::free(p); }
void operator delete[](void *p, size_t) noexcept { std::free(p); }

struct Object : IUnknown {
  ULONG refs = 1;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **) override { return E_NOINTERFACE; }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override { return --refs; }
};

template<typename Store>
static bool check() {
  const GUID key = {};
  Object object;
  Store data, interfaces;
  unsigned old = 42, replacement = 7, value = 0;
  UINT size = sizeof(value);
  if (data.setData(key, sizeof(old), &old) != S_OK)
    return false;
  deny_allocations = true;
  HRESULT bytes = data.setData(key, sizeof(replacement), &replacement);
  HRESULT iface = interfaces.setInterface(key, &object);
  deny_allocations = false;
  return bytes == E_OUTOFMEMORY && iface == E_OUTOFMEMORY && object.refs == 1 &&
         data.getData(key, &size, &value) == S_OK && size == sizeof(value) && value == old &&
         interfaces.getData(key, &size, nullptr) == DXGI_ERROR_NOT_FOUND &&
         interfaces.setInterface(key, &object) == S_OK && object.refs == 2;
}

int main() {
  try {
    if (check<mullion::ComPrivateData>() && check<mullion::ConcurrentComPrivateData>()) {
      std::puts("passed");
      return 0;
    }
    std::puts("failed: an allocation failure changed a stored value or interface reference, or returned the wrong result");
  } catch (const std::bad_alloc &) {
    deny_allocations = false;
    std::puts("failed: an allocation failure escaped the private data setter");
  }
  return 1;
}
