// contract: winemetal's exception handler leaves with the DLL. a launcher that loads Direct3D only to read the GPU's
// details frees it and goes on, and its next exception must reach its own handlers, not code that is gone
// (RemoveVectoredExceptionHandler, Microsoft Learn: a handler is the registrant's to remove). the program links no
// Mullion DLL, loads winemetal.dll, frees it, checks it is gone and raises an exception its own handler continues from.
// tests/flight.py runs it and reads its 'fact' lines
#include <windows.h>
#include <cstdio>

constexpr DWORD kOurs = 0xE0000001;
static LONG handled = 0;

static LONG CALLBACK
ours(EXCEPTION_POINTERS *pointers) {
  if (pointers->ExceptionRecord->ExceptionCode != kOurs)
    return EXCEPTION_CONTINUE_SEARCH;
  handled++;
  return EXCEPTION_CONTINUE_EXECUTION;
}

int
main() {
  HMODULE winemetal = LoadLibraryA("winemetal.dll");
  if (!winemetal) {
    printf("failed: winemetal.dll does not load (%lu)\n", GetLastError());
    return 1;
  }
  FreeLibrary(winemetal);
  if (GetModuleHandleA("winemetal.dll")) {
    printf("failed: winemetal.dll stays loaded after its only reference is freed\n");
    return 1;
  }
  // after the DLL's handler would have been called: it registered itself first
  AddVectoredExceptionHandler(0, ours);
  RaiseException(kOurs, 0, 0, nullptr);
  printf("fact unloaded %ld\n", handled);
  fflush(stdout);
  return handled != 1;
}
