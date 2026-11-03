// what the GPU capture tests read of the traces they make, and how they remove them
#pragma once
#include <windows.h>
#include <fstream>
#include <iterator>
#include <string>

// whether a file under `path` holds `text`
inline bool
holds(const std::string &path, const std::string &text) {
  WIN32_FIND_DATAA found;
  HANDLE find = FindFirstFileA((path + "\\*").c_str(), &found);
  bool held = false;
  for (BOOL more = find != INVALID_HANDLE_VALUE; more && !held; more = FindNextFileA(find, &found)) {
    std::string name = found.cFileName, at = path + "\\" + name;
    if (name == "." || name == "..")
      continue;
    if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
      held = holds(at, text);
    else {
      std::ifstream file(at, std::ios::binary);
      std::string bytes{std::istreambuf_iterator<char>(file), {}};
      held = bytes.find(text) != bytes.npos;
    }
  }
  if (find != INVALID_HANDLE_VALUE)
    FindClose(find);
  return held;
}

// removes `path` and everything under it. a trace holds symbolic links, which Windows' file calls under Wine leave
// behind, so the host's rm takes the tree
inline bool
remove_tree(const std::string &path) {
  auto unix_name = reinterpret_cast<char *(__cdecl *)(const WCHAR *)>(
      GetProcAddress(GetModuleHandleA("kernel32"), "wine_get_unix_file_name")
  );
  auto spawn =
      reinterpret_cast<LONG(WINAPI *)(char *const *, int)>(GetProcAddress(GetModuleHandleA("ntdll"), "__wine_unix_spawnvp"));
  WCHAR wide[MAX_PATH];
  char *name;
  if (!unix_name || !spawn || !MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, wide, MAX_PATH) ||
      !(name = unix_name(wide)))
    return false;
  char rm[] = "rm", flags[] = "-rf";
  char *const args[] = {rm, flags, name, nullptr};
  bool removed = spawn(args, TRUE) == 0;
  HeapFree(GetProcessHeap(), 0, name);
  return removed && GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES;
}
