#pragma once
#include "Metal.hpp"
#include "util_env.hpp"
#include "util_string.hpp"
#include "thread.hpp"

namespace dxmt::diag {
inline const bool profile = env::getEnvVar("DXMT_DIAG") == "profile";
inline const bool errors = !env::getEnvVar("DXMT_D3D12_GPU_ERRORS").empty();

inline uint64_t
Nanoseconds(uint64_t ticks) {
  static const uint64_t second = [] {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    return frequency.QuadPart;
  }();
  return ticks / second * 1'000'000'000 + ticks % second * 1'000'000'000 / second;
}

inline uint64_t
Now() {
  LARGE_INTEGER ticks;
  QueryPerformanceCounter(&ticks);
  return Nanoseconds(ticks.QuadPart);
}

template <typename... Fields>
std::string
Line(const char *kind, const Fields &...fields) {
  return (std::string(kind) + ... + ('\t' + str::format(fields))) + '\n';
}

inline void
Write(const std::string &lines) {
  static const auto directory = env::getEnvVar("DXMT_DIAG_PATH");
  if (!WMTDiagWrite(directory.c_str(), lines.data(), lines.size()))
    ERR("D3D12 profile: record could not be written");
}

inline void
Frame(uint64_t queue, uint64_t number) {
  static dxmt::mutex lock;
  std::lock_guard<dxmt::mutex> order(lock);
  Write(Line("frame", queue, number, Now()));
}
} // namespace dxmt::diag
