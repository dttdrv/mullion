// what a test was doing when it went wrong. a test names each connection before it exercises it (step), and the
// name goes into every line that reports a wrong result, a fault or a hang:
// - output is unbuffered, so the log of a test that never ends holds the step it stopped in;
// - a fault in the test's process, the library's code included, ends the test as failed, with the module and the
//   offset it happened at (the offset is the one a disassembly or a map of that module shows).
#pragma once
#include <windows.h>
#include <cstdarg>
#include <cstdio>

namespace trace {
inline char doing[512] = "starting";
inline unsigned wrong;

inline LONG WINAPI
fault(EXCEPTION_POINTERS *info) {
  auto at = (char *)info->ExceptionRecord->ExceptionAddress;
  HMODULE module = nullptr;
  char name[MAX_PATH] = "no module";
  if (GetModuleHandleExA(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, at, &module
      ))
    GetModuleFileNameA(module, name, sizeof(name));
  printf(
      "failed: exception %08lx at %s+0x%llx while: %s\n", info->ExceptionRecord->ExceptionCode, name,
      (unsigned long long)(at - (char *)module), doing
  );
  ExitProcess(1);
}

inline const int installed = (setvbuf(stdout, nullptr, _IONBF, 0), SetUnhandledExceptionFilter(fault), 0);
} // namespace trace

// names what the test does next
inline void
step(const char *format, ...) {
  va_list args;
  va_start(args, format);
  vsnprintf(trace::doing, sizeof(trace::doing), format, args);
  va_end(args);
  printf("step: %s\n", trace::doing);
}

// counts and reports a result that is not the wanted one, with the step it belongs to; returns `ok`
inline bool
expect(bool ok, const char *format, ...) {
  if (ok)
    return true;
  va_list args;
  va_start(args, format);
  printf("wrong: ");
  vprintf(format, args);
  va_end(args);
  printf(" (while: %s)\n", trace::doing);
  trace::wrong++;
  return false;
}

// the test's last word and its exit status
inline int
verdict() {
  printf("%s: %u wrong results\n", trace::wrong ? "failed" : "passed", trace::wrong);
  return trace::wrong != 0;
}
