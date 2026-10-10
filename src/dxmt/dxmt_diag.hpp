#pragma once
#include "Metal.hpp"
#include "util_env.hpp"
#include "util_string.hpp"
#include "thread.hpp"
#include "log/log.hpp"
#include <vector>
#include <type_traits>

namespace dxmt::diag {
struct TimestampSamples {
  dxmt::mutex mutex;
  dxmt::condition_variable returned;
  std::vector<WMT::Reference<WMT::CounterSampleBuffer>> free;
  uint32_t made = 0;
  // the most samples Metal lets a buffer hold
  uint32_t limit = 0;

  WMT::Reference<WMT::CounterSampleBuffer>
  Take(WMT::Device metal, bool wait = true) {
    std::unique_lock<dxmt::mutex> lock(mutex);
    if (!limit)
      for (limit = 1; metal.newCounterSampleBuffer(limit * 2); limit *= 2)
        ;
    for (;;) {
      if (!free.empty()) {
        auto buffer = std::move(free.back());
        free.pop_back();
        return buffer;
      }
      if (auto buffer = metal.newCounterSampleBuffer(limit)) {
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
    std::lock_guard<dxmt::mutex> lock(mutex);
    free.push_back(std::move(buffer));
    returned.notify_one();
  }
};

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
    ERR("Mullion profile: record could not be written");
}

template <typename Queue>
void
Frame(const Queue &queue, uint64_t number) {
  static dxmt::mutex lock;
  std::lock_guard<dxmt::mutex> order(lock);
  Write(Line("frame", queue, number, Now()));
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
      case WMTRenderCommandDXMTGeometryDraw:
      case WMTRenderCommandDXMTGeometryDrawIndexed:
      case WMTRenderCommandDXMTGeometryDrawIndirect:
      case WMTRenderCommandDXMTGeometryDrawIndexedIndirect:
      case WMTRenderCommandDXMTTessellationMeshDraw:
      case WMTRenderCommandDXMTTessellationMeshDrawIndexed:
      case WMTRenderCommandDXMTTessellationMeshDrawIndirect:
      case WMTRenderCommandDXMTTessellationMeshDrawIndexedIndirect:
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
  };
  uint64_t submitted = 0, committed = 0;
  uint32_t presents = 0, timestamps = 0;
  std::vector<Samples> samples;
  std::vector<Pass> passes;
};
} // namespace dxmt::diag
