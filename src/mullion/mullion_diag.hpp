#pragma once
#include "Metal.hpp"
#include "util_env.hpp"
#include "util_string.hpp"
#include "thread.hpp"
#include "log/log.hpp"
#include "log/trace.hpp"
#include <algorithm>
#include <string_view>
#include <vector>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace mullion::diag {
// the compiler checks a flight record's format against its arguments here, which must each be 64 bits
[[gnu::format(printf, 1, 2)]] inline void
FlightFormat(const char *, ...) {}

template <typename T>
uint64_t
FlightWord(T value) {
  static_assert(sizeof(T) == sizeof(uint64_t) || std::is_pointer_v<T>);
  if constexpr (std::is_pointer_v<T>)
    return uint64_t(uintptr_t(value));
  else
    return uint64_t(value);
}

template <typename... Args>
void
Flight(const void *site, Args... args) {
  static_assert(sizeof...(Args) <= 4);
  uint64_t words[4] = {FlightWord(args)...};
  WMTFlightRecord(site, words[0], words[1], words[2], words[3]);
}

// a flight record's line in the trace log too (MULLION_TRACE_<CATEGORY>)
template <typename... Args>
void
FlightTrace(trace::Category category, const char *format, Args... args) {
  uint64_t words[4] = {FlightWord(args)...};
  trace::WriteFlight(category, format, words);
}

struct TimestampSamples {
  mullion::mutex mutex;
  mullion::condition_variable returned;
  std::vector<WMT::Reference<WMT::CounterSampleBuffer>> free;
  uint32_t made = 0;
  // the most samples Metal lets a buffer hold
  uint32_t limit = 0;

  // `metal4`: a Metal 4 counter heap in place of a counter sample buffer
  WMT::Reference<WMT::CounterSampleBuffer>
  Take(WMT::Device metal, bool wait = true, bool metal4 = false) {
    auto make = [&](uint64_t count) {
      return metal4 ? WMT::Reference<WMT::CounterSampleBuffer>(MTL4CounterHeap_new(metal, count))
                    : metal.newCounterSampleBuffer(count);
    };
    std::unique_lock<mullion::mutex> lock(mutex);
    if (!made)
      for (limit = 1; make(limit * 2); limit *= 2)
        ;
    for (;;) {
      if (!free.empty()) {
        auto buffer = std::move(free.back());
        free.pop_back();
        return buffer;
      }
      if (auto buffer = make(limit)) {
        made++;
        return buffer;
      }
      if (!made || !wait)
        return {};
      if (Logger::logLevel() == LogLevel::Trace)
        TRACE("timestamps: waits for one of ", made, " sample buffers");
      returned.wait(lock);
    }
  }

  void
  Return(WMT::Reference<WMT::CounterSampleBuffer> &&buffer) {
    std::lock_guard<mullion::mutex> lock(mutex);
    free.push_back(std::move(buffer));
    returned.notify_one();
  }
};

// the bridge times its calls from the first library that profiles
// MULLION_DIAG=profile, or MULLION_TRACE_CPU (whose records go next to the trace log)
inline const bool profile =
    (env::getEnvVar("MULLION_DIAG") == "profile" || trace::On(trace::Cpu)) && (WMTDiagTimeCalls(), true);
// MULLION_D3D12_GPU_ERRORS, or MULLION_TRACE_GPU=heavy: a failed command buffer's report names its passes and what became
// of each, with the most its indirect commands asked for (the GPU records them as it runs)
inline const bool errors = !env::getEnvVar("MULLION_D3D12_GPU_ERRORS").empty() || trace::Heavy(trace::Gpu);
// MULLION_TRACE_ENCODERS, or MULLION_TRACE_PASS=heavy: every D3D11 render pass is logged with its size and draws as it is
// encoded, and D3D12 queues count their Metal encoders
inline const bool trace_encoders = !env::getEnvVar("MULLION_TRACE_ENCODERS").empty() || trace::Heavy(trace::Pass);

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
  static const auto directory = [] {
    auto given = env::getEnvVar("MULLION_DIAG_PATH");
    return given.empty() && trace::On(trace::Cpu) ? trace::Directory() : given;
  }();
  if (!WMTDiagWrite(directory.c_str(), lines.data(), lines.size()))
    ERR("Mullion profile: record could not be written");
}

// the bridge's calls since the frame before: a 'call' line naming each code once, then 'unix' with code:count:time
inline std::string
Calls() {
  static bool named[WMTUnixCallCount];
  WMTDiagCall calls[WMTUnixCallCount];
  WMTDiagCallTotals(calls);
  std::string lines, totals = "unix";
  for (uint32_t code = 0; code < WMTUnixCallCount; code++) {
    if (!calls[code].count)
      continue;
    if (!std::exchange(named[code], true))
      lines += Line("call", code, calls[code].name);
    totals += str::format('\t', code, ':', calls[code].count, ':', Nanoseconds(calls[code].ticks));
  }
  return lines + totals + '\n';
}

// the GPU's driver statistics at a frame's end, as the device's IORegistry entry has them
inline std::string
Statistics(WMT::Device device) {
  std::string text;
  for (uint64_t length = 0; text.size() != (length = WMTGPUStatistics(device.handle, text.data(), text.size()));)
    text.resize(length);
  return "gpu" + text + '\n';
}

// MULLION_DIAG=profile, or Metal's capture: the application's debug events name the passes recorded in them
inline const bool events = profile || env::getEnvVar("MTL_CAPTURE_ENABLED") == "1";
// ends each name in an event path; names never hold it, as EventName makes control characters spaces
constexpr char kEventEnd = '\x1f';

// the name D3D12's BeginEvent gives: a wide (metadata 0) or ANSI (1) string, or a PIX event, whose format string
// is the name (pix3_win.h): 2 is PIXEventsLegacy.h's blob, WINPIX_EVENT_PIX3BLOB_V2 PIXEventsCommon.h's
inline std::string
EventName(UINT metadata, const void *data, UINT size) {
  constexpr UINT pix3blob = 2, pix3blob_v2 = 6345127;
  auto words = static_cast<const uint64_t *>(data);
  size_t count = size / sizeof(uint64_t), at = 1;
  bool ansi = metadata == 1;
  if (metadata == pix3blob || metadata == pix3blob_v2) {
    if (!count)
      return {};
    if (metadata == pix3blob)
      at++; // the color
    else {
      auto flags = words[0] >> 12 & 0xff;
      // PIX_EVENT_METADATA_HAS_COLOR: a color word follows; otherwise an index color is in the flags
      at += (flags & 0xf0) == 0xf0;
      // PIX_EVENT_METADATA_ON_CONTEXT: then the context pointer
      at += flags & 0x1;
    }
    // the string's info word, a zero word for a null string; its bit 54 says ANSI
    if (at >= count || !words[at])
      return {};
    ansi = words[at] >> 54 & 1;
    data = words + ++at;
    size = (count - at) * sizeof(uint64_t);
  } else if (metadata > 1) {
    return {};
  }
  std::string name;
  if (ansi) {
    name.assign(static_cast<const char *>(data), strnlen(static_cast<const char *>(data), size));
  } else {
    auto wide = static_cast<const WCHAR *>(data);
    name = str::fromws(std::basic_string<WCHAR>(wide, std::find(wide, wide + size / sizeof(WCHAR), 0)).c_str());
  }
  std::replace_if(name.begin(), name.end(), [](unsigned char c) { return c < ' '; }, ' ');
  return name;
}

inline void
PushEvent(std::string &path, const std::string &name) {
  path += name + kEventEnd;
}

// an end with no event open is the application's error, which ends nothing
inline void
PopEvent(std::string &path) {
  if (path.empty())
    return;
  path.pop_back();
  path.erase(path.find_last_of(kEventEnd) + 1);
}

// the events open on a command buffer as Metal debug groups, which Xcode's captures show around their passes
struct DebugGroups {
  std::string open;

  // closes the groups the path is not in, and opens its events that are not open; "" closes all
  template <typename CommandBuffer>
  void
  Enter(CommandBuffer cmdbuf, std::string_view path) {
    size_t kept = 0;
    for (size_t end; (end = open.find(kEventEnd, kept)) != open.npos; kept = end + 1)
      if (path.compare(kept, end + 1 - kept, open, kept, end + 1 - kept))
        break;
    for (auto i = kept; i < open.size(); i++)
      if (open[i] == kEventEnd)
        cmdbuf.popDebugGroup();
    for (size_t end; (end = path.find(kEventEnd, kept)) != path.npos; kept = end + 1) {
      std::string name(path.substr(kept, end - kept));
      cmdbuf.pushDebugGroup(WMT::String::string(name.c_str(), WMTUTF8StringEncoding));
    }
    open = path;
  }
};

// a wait of a thread for the GPU or Mullion's threads, from `began` until now: tools/mullion-diag follows a frame's
// time through the waits of the thread that presents
inline void
Waited(const char *what, uint64_t began, DWORD thread = GetCurrentThreadId()) {
  Write(Line("span", "wait", what, thread, began, Now() - began));
}

// the frame latency objects the application took, each with when it presented last and on which thread: a release
// that finds an object's count at zero ends a wait the application is taken to have begun at that present
struct LatencyWait {
  uint64_t presented = 0;
  DWORD thread = 0;
};
inline mullion::mutex latency_lock;
inline std::unordered_map<HANDLE, LatencyWait> latency_waits;

inline void
LatencyTaken(HANDLE semaphore) {
  std::lock_guard<mullion::mutex> lock(latency_lock);
  latency_waits.try_emplace(semaphore);
}

inline void
LatencyPresented(HANDLE semaphore) {
  std::lock_guard<mullion::mutex> lock(latency_lock);
  if (auto wait = latency_waits.find(semaphore); wait != latency_waits.end())
    wait->second = {Now(), GetCurrentThreadId()};
}

// before the release
inline void
LatencyReleased(HANDLE semaphore) {
  std::lock_guard<mullion::mutex> lock(latency_lock);
  if (auto wait = latency_waits.find(semaphore);
      wait != latency_waits.end() && wait->second.presented && !WMTDiagSemaphoreCount(semaphore))
    Waited("latency", std::exchange(wait->second.presented, 0), wait->second.thread);
}

inline void
LatencyClosed(HANDLE semaphore) {
  std::lock_guard<mullion::mutex> lock(latency_lock);
  latency_waits.erase(semaphore);
}

template <typename Queue>
void
Frame(WMT::Device device, const Queue &queue, uint64_t number) {
  static mullion::mutex lock;
  std::lock_guard<mullion::mutex> order(lock);
  // with Metal's host clock read beside it: how a Wine's QueryPerformanceCounter maps to it varies
  auto now = Now();
  uint64_t host, gpu;
  device.sampleTimestamps(host, gpu);
  Write(Line("frame", queue, number, now, host) + Statistics(device) + Calls());
}

template <typename Head, typename Name>
std::string
PipelineRuns(Head head, Name name) {
  std::string runs, label;
  uint64_t pipeline = 0, calls = 0, vertices = 0, indices = 0;
  auto run = [&] {
    if (!calls)
      return;
    if (!runs.empty())
      runs += '|';
    runs += str::format(label, '*', calls, '*', vertices, '*', indices);
  };
  auto set = [&](uint64_t next) {
    if (pipeline == next)
      return;
    run();
    pipeline = next;
    label = name(pipeline);
    calls = vertices = indices = 0;
  };
  for (auto cmd = (wmtcmd_base *)head.next.get(); cmd; cmd = (wmtcmd_base *)cmd->next.get()) {
    if constexpr (std::is_same_v<Head, wmtcmd_render_nop>) {
      switch (WMTRenderCommandType(cmd->type)) {
      case WMTRenderCommandSetPSO:
        set(reinterpret_cast<const wmtcmd_render_setpso *>(cmd)->pso);
        break;
      case WMTRenderCommandDraw: {
        auto draw = reinterpret_cast<const wmtcmd_render_draw *>(cmd);
        calls++;
        vertices += draw->vertex_count * draw->instance_count;
        break;
      }
      case WMTRenderCommandDrawIndexed: {
        auto draw = reinterpret_cast<const wmtcmd_render_draw_indexed *>(cmd);
        calls++;
        indices += draw->index_count * draw->instance_count;
        break;
      }
      case WMTRenderCommandDispatchThreadsPerTile:
        if (!runs.empty())
          runs += '|';
        runs += str::format("Tile:", pipeline, "*1*0*0");
        break;
      case WMTRenderCommandDrawIndirect:
      case WMTRenderCommandDrawIndexedIndirect:
      case WMTRenderCommandDrawMeshThreadgroups:
      case WMTRenderCommandDrawMeshThreadgroupsIndirect:
      case WMTRenderCommandMullionGeometryDraw:
      case WMTRenderCommandMullionMeshDraw:
      case WMTRenderCommandMullionGeometryDrawIndexed:
      case WMTRenderCommandMullionGeometryDrawIndirect:
      case WMTRenderCommandMullionGeometryDrawIndexedIndirect:
      case WMTRenderCommandMullionTessellationMeshDraw:
      case WMTRenderCommandMullionTessellationMeshDrawIndexed:
      case WMTRenderCommandMullionTessellationMeshDrawIndirect:
      case WMTRenderCommandMullionTessellationMeshDrawIndexedIndirect:
      case WMTRenderCommandExecuteCommandsInBuffer:
      // ExecuteIndirect's commands, over the range its resolver writes (d3d12_command_allocator.cpp)
      case WMTRenderCommandExecuteCommandsInBufferIndirect:
        calls++;
        break;
      default:
        break;
      }
    } else {
      switch (WMTComputeCommandType(cmd->type)) {
      case WMTComputeCommandSetPSO:
        set(reinterpret_cast<const wmtcmd_compute_setpso *>(cmd)->pso);
        break;
      case WMTComputeCommandDispatch:
      case WMTComputeCommandDispatchIndirect:
      case WMTComputeCommandDispatchThreads:
      case WMTComputeCommandDispatchPart:
      case WMTComputeCommandExecuteCommandsInBuffer:
      case WMTComputeCommandExecuteCommandsInBufferIndirect:
        calls++;
        break;
      default:
        break;
      }
    }
  }
  run();
  return runs;
}

struct ProfileBuffer {
  struct Samples {
    WMT::Reference<WMT::CounterSampleBuffer> buffer;
    uint32_t used = 0;
  };
  struct Pass {
    uint64_t id, encoding;
    const char *kind;
    std::string runs, targets;
    uint32_t buffer, first, count;
    std::string event;
  };
  uint64_t submitted = 0, committed = 0, cpu_begin = 0, gpu_begin = 0;
  uint32_t presents = 0, timestamps = 0;
  std::vector<Samples> samples;
  std::vector<Pass> passes;
};
} // namespace mullion::diag

// a record in the flight recorder (WMTFlightRecord), always on: a printf format of 64-bit integers, at most four
#define FLIGHT(format, ...)                                                                                            \
  do {                                                                                                                 \
    static const void *const flight_site = WMTFlightSite(format);                                                      \
    if (false)                                                                                                         \
      ::mullion::diag::FlightFormat(format __VA_OPT__(, ) __VA_ARGS__);                                                \
    ::mullion::diag::Flight(flight_site __VA_OPT__(, ) __VA_ARGS__);                                                   \
  } while (0)

// a flight record that is also a line of the trace log when MULLION_TRACE_<CATEGORY> turns its category on
// (FLIGHT_IN), or makes it heavy (FLIGHT_HEAVY: the records of every submission, command buffer or present)
#define FLIGHT_IN(category, format, ...)                                                                               \
  do {                                                                                                                 \
    FLIGHT(format __VA_OPT__(, ) __VA_ARGS__);                                                                         \
    if (::mullion::trace::On(::mullion::trace::category))                                                             \
      ::mullion::diag::FlightTrace(::mullion::trace::category, format __VA_OPT__(, ) __VA_ARGS__);                     \
  } while (0)

#define FLIGHT_HEAVY(category, format, ...)                                                                            \
  do {                                                                                                                 \
    FLIGHT(format __VA_OPT__(, ) __VA_ARGS__);                                                                         \
    if (::mullion::trace::Heavy(::mullion::trace::category))                                                          \
      ::mullion::diag::FlightTrace(::mullion::trace::category, format __VA_OPT__(, ) __VA_ARGS__);                     \
  } while (0)
