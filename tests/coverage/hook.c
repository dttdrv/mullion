// notes which functions of a library ran. a coverage build (tests/coverage.py) compiles the library with
// -fsanitize-coverage=func,trace-pc, which has the compiler call __sanitizer_cov_trace_pc at the start of every
// function, and links this file into it. when the process ends, the library writes the places those calls returned
// to, as offsets from its start, one a line, to %MULLION_COVERAGE%\<library>.<process>.txt.
#include <windows.h>

// a set of return addresses, open addressed: a library has far fewer functions than it has places
#define BITS 18
static void *volatile seen[1u << BITS];

void
__sanitizer_cov_trace_pc(void) {
  void *pc = __builtin_return_address(0);
  for (unsigned i = (unsigned)(((UINT64)(UINT_PTR)pc * 0x9E3779B97F4A7C15ull) >> (64 - BITS));; i = (i + 1) & ((1u << BITS) - 1)) {
    void *at = seen[i];
    if (at == pc || (!at && (at = InterlockedCompareExchangePointer((void *volatile *)&seen[i], pc, NULL), !at || at == pc)))
      return;
  }
}

__attribute__((destructor)) static void
written(void) {
  char path[2 * MAX_PATH], name[MAX_PATH], number[32];
  HMODULE library;
  DWORD length = GetEnvironmentVariableA("MULLION_COVERAGE", path, MAX_PATH), done;
  if (!length || length >= MAX_PATH ||
      !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)written, &library) ||
      !GetModuleFileNameA(library, name, sizeof(name)))
    return;
  char *base = name;
  for (char *c = name; *c; c++)
    if (*c == '\\' || *c == '/')
      base = c + 1;
  // hexadecimal, most significant digit first, without leading zeros
  #define HEX(value, end) for (UINT64 v = (value), first = 1; v || first; v >>= 4, first = 0) *--end = "0123456789abcdef"[v & 15]
  char *digits = number + sizeof(number);
  *--digits = 0;
  HEX(GetCurrentProcessId(), digits);
  lstrcatA(path, "\\");
  lstrcatA(path, base);
  lstrcatA(path, ".");
  lstrcatA(path, digits);
  lstrcatA(path, ".txt");
  HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE)
    return;
  for (unsigned i = 0; i < 1u << BITS; i++) {
    if (!seen[i])
      continue;
    char *end = number + sizeof(number);
    *--end = '\n';
    HEX((char *)seen[i] - (char *)library, end);
    WriteFile(file, end, (DWORD)(number + sizeof(number) - end), &done, NULL);
  }
  CloseHandle(file);
}
