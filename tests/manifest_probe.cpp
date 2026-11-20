// contract: the manifest names what the Wine actually loaded, not what the runner meant to install
// (tests/manifest.py, docs/TESTING.md section 3). this probe asks the loader for each module by name, then reports the
// full path the loader resolved -- GetModuleFileName's answer, not the directory the runner copied into -- and exits
// without creating a device. run under WINEDEBUG=+loaddll, so the loader's own record corroborates the paths.
#include <windows.h>

#include <cstdio>

int main(int argc, char **argv) {
    // the process architecture decides how the loader interprets C:\windows\system32: a 32-bit process on a 64-bit
    // prefix sees the files in syswow64 through that name, and the manifest must hash the redirected file
    std::printf("probe-arch: %d\n", static_cast<int>(sizeof(void *) * 8));
    std::fflush(stdout);
    for (int i = 1; i < argc; ++i) {
        HMODULE module = LoadLibraryA(argv[i]);
        if (module == nullptr) {
            std::printf("probe-failed: %s error=%lu\n", argv[i], GetLastError());
            std::fflush(stdout);
            continue;
        }
        char path[MAX_PATH * 8];
        DWORD length = GetModuleFileNameA(module, path, sizeof path);
        std::printf("probe: %s %.*s\n", argv[i], static_cast<int>(length), path);
        std::fflush(stdout);
        FreeLibrary(module);
    }
    return 0;
}
