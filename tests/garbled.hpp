// every way a shader container's own layout gives to garble it: for the tests of what Direct3D makes of a container
// that is not what a compiler wrote (tests/d3d12/d3d12_garbled.cpp, tests/d3d11/d3d11_garbled.cpp). a container
// is a header with a hash of everything after it, the size, and the offsets of parts of a four-character name and a
// size each (DirectXShaderCompiler, DxilContainer.h: DxilContainerHeader, DxilPartHeader).
#pragma once
#include "dxbc_hash.hpp"
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include <windows.h>

// the container's words: the hash is 16 bytes after the four of "DXBC"
enum { Hash = 4, Size = 24, Count = 28, Offsets = 32 };

inline UINT
word(const std::string &code, size_t at) {
  UINT value;
  memcpy(&value, code.data() + at, sizeof(value));
  return value;
}

// every garbling of a container, with what was done
inline std::vector<std::pair<std::string, std::string>>
garbled(const std::string &code) {
  std::vector<std::pair<std::string, std::string>> all;
  auto with = [&](std::string what, size_t at, UINT value) {
    std::string changed = code;
    memcpy(changed.data() + at, &value, sizeof(value));
    all.emplace_back(what + " at byte " + std::to_string(at) + " set to " + std::to_string(value), changed);
  };
  auto flipped = [&](std::string what, size_t at) {
    std::string changed = code;
    changed[at] ^= 0x5a;
    all.emplace_back(what + ": byte " + std::to_string(at) + " flipped", changed);
  };
  auto cut = [&](std::string what, size_t size) { all.emplace_back(what + ": cut to " + std::to_string(size) + " bytes", code.substr(0, size)); };
  const UINT parts = word(code, Count);
  cut("nothing left", 0), cut("inside the name", 3), cut("inside the hash", Hash + 8), cut("before the offsets", Offsets);
  cut("the last byte gone", code.size() - 1);
  flipped("the hash", Hash), flipped("the name", 0);
  with("the total size", Size, 0), with("the total size", Size, ~0u), with("the total size", Size, code.size() + 4);
  with("the part count", Count, 0), with("the part count", Count, parts + 1), with("the part count", Count, ~0u);
  for (UINT part = 0; part < parts; part++) {
    const size_t entry = Offsets + 4 * part, at = word(code, entry), size = word(code, at + 4), data = at + 8;
    std::string name = "part " + code.substr(at, 4);
    cut(name + " not begun", at), cut(name + " without its size", at + 4), cut(name + " half there", data + size / 2);
    with(name + "'s offset", entry, 0), with(name + "'s offset", entry, code.size()), with(name + "'s offset", entry, ~0u);
    with(name + "'s size", at + 4, 0), with(name + "'s size", at + 4, size + 4), with(name + "'s size", at + 4, ~0u);
    flipped(name + "'s name", at);
    if (size)
      flipped(name + "'s first byte", data), flipped(name + "'s middle", data + size / 2), flipped(name + "'s last byte", data + size - 1);
  }
  return all;
}

// a container with these 16 bytes for its hash
inline std::string
hashed(std::string code, char byte) {
  if (code.size() >= dxbc::hashed_from)
    code.replace(dxbc::hash_at, sizeof(dxbc::Hash), sizeof(dxbc::Hash), byte);
  return code;
}

// whether a container's own table of parts is whole (its size is the size it has, and every part lies inside it
// behind the table), and whether it then has a part of this name with something in it
struct Layout {
  bool whole, has;
};
inline Layout
layout(const std::string &code, const char *name) {
  Layout found{code.size() >= Offsets && word(code, Size) == code.size(), false};
  const size_t parts = found.whole ? word(code, Count) : 0, table_end = Offsets + 4 * parts;
  found.whole = found.whole && table_end <= code.size();
  for (size_t part = 0; found.whole && part < parts; part++) {
    const size_t at = word(code, Offsets + 4 * part);
    found.whole = at >= table_end && at + 8 <= code.size() && at + 8 + size_t(word(code, at + 4)) <= code.size();
    found.has |= found.whole && !code.compare(at, 4, name) && word(code, at + 4);
  }
  return found;
}
