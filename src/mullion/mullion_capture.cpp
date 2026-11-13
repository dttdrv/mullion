#include "mullion_capture.hpp"
#include "log/log.hpp"
#include "thread.hpp"
#include "util_env.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <ctime>

#ifdef __WIN32
#include "windows.h"
#endif

namespace mullion {

namespace {

enum class NextAction {
  StartCapture,
  StopCapture,
  Nothing,
};

enum class State {
  Idle,
  WillCapture,
  Capturing,
};

struct Capture {
  const bool enabled = env::getEnvVar("MTL_CAPTURE_ENABLED") == "1" &&
                       env::getExeBaseName() == env::getEnvVar("MULLION_CAPTURE_EXECUTABLE");
  mullion::mutex mutex;
  State state = State::Idle;
  uint64_t next_capture_frame = 0;
  // how many frames a capture holds: MULLION_CAPTURE_FRAMES, or the count a trigger file gives
  uint64_t frame_count = 1;
  uint64_t default_frame_count = 1;
  // MULLION_CAPTURE_TRIGGER: a file whose appearance captures the next frames (and which is then deleted), so a
  // capture can start from outside the game; it may hold the number of frames. A Unix path or a Windows one.
  std::string trigger_path;
#ifdef _WIN32
  ULONGLONG trigger_checked = 0;
#endif
  bool capture_key_pressed_ = false;
  unsigned holds = 0;
  mullion::thread stopping_;
  std::atomic<bool> stopped_ = true;

  Capture() {
    if (!enabled)
      return;
    WARN("Mullion capture enabled");
    try {
      if (auto count = env::getEnvVar("MULLION_CAPTURE_FRAMES"); !count.empty())
        default_frame_count = std::max<uint64_t>(std::stoull(count), 1);
    } catch (const std::exception &) {
    }
    frame_count = default_frame_count;
    try {
      if (auto frame = env::getEnvVar("MULLION_CAPTURE_FRAME"); !frame.empty())
        scheduleNextFrameCapture(std::stoull(frame));
    } catch (const std::exception &) {
    }
    trigger_path = env::getEnvVar("MULLION_CAPTURE_TRIGGER");
    if (!trigger_path.empty() && trigger_path.front() == '/') {
      for (auto &c : trigger_path)
        if (c == '/')
          c = '\\';
      trigger_path = "Z:" + trigger_path;
    }
    if (!trigger_path.empty())
      WARN("Mullion capture: a capture starts when ", trigger_path, " appears");
  }

  NextAction
  getNextAction(uint64_t frame) {
    switch (state) {
    case State::Idle:
      break;
    case State::WillCapture:
      if (frame == next_capture_frame) {
        state = State::Capturing;
        return NextAction::StartCapture;
      }
      break;
    case State::Capturing:
      if (frame >= next_capture_frame + frame_count) {
        next_capture_frame = 0;
        state = State::Idle;
        return NextAction::StopCapture;
      }
      break;
    }
    return NextAction::Nothing;
  }

  void
  scheduleNextFrameCapture(uint64_t next = 0, uint64_t count = 0) {
    if (state != State::Idle)
      return;
    state = State::WillCapture;
    next_capture_frame = next;
    frame_count = count ? count : default_frame_count;
  }

  // the number of frames the trigger file asks for (the default where it gives none), or 0 where there is no file;
  // looked for at most four times a second
  uint64_t
  triggeredFrameCount() {
#ifdef _WIN32
    if (trigger_path.empty() || state != State::Idle)
      return 0;
    ULONGLONG now = GetTickCount64();
    if (now - trigger_checked < 250)
      return 0;
    trigger_checked = now;
    if (GetFileAttributesA(trigger_path.c_str()) == INVALID_FILE_ATTRIBUTES)
      return 0;
    uint64_t count = default_frame_count;
    if (auto file = std::fopen(trigger_path.c_str(), "r")) {
      unsigned long long n = 0;
      if (std::fscanf(file, "%llu", &n) == 1 && n)
        count = n;
      std::fclose(file);
    }
    DeleteFileA(trigger_path.c_str());
    return count;
#else
    return 0;
#endif
  }

  bool
  shouldCaptureNextFrame() {
#ifdef _WIN32
    if (state != State::Idle)
      return false;
    SHORT state_ = GetAsyncKeyState(VK_F10);
    if (state_ & 0x8000 && !capture_key_pressed_) {
      capture_key_pressed_ = true;
      return true;
    }
    capture_key_pressed_ = state_ & 0x8000;
#endif
    return false;
  }
};

// never destroyed: a hold the process never released must not meet a destroyed capture at its exit
Capture &
capture() {
  static auto &capture = *new Capture;
  return capture;
}

} // namespace

CaptureState::CaptureState() {
  auto &capture = mullion::capture();
  if (!capture.enabled)
    return;
  std::lock_guard lock(capture.mutex);
  capture.holds++;
}

CaptureState::~CaptureState() {
  auto &capture = mullion::capture();
  if (!capture.enabled)
    return;
  std::lock_guard lock(capture.mutex);
  if (--capture.holds)
    return;
  if (capture.state == State::Capturing) {
    capture.state = State::Idle;
    capture.next_capture_frame = 0;
    WMT::CaptureManager::sharedCaptureManager().stopCapture();
  }
  if (capture.stopping_.joinable())
    capture.stopping_.join();
}

void
CaptureState::frame(WMT::Device device, uint64_t frame) {
  auto &capture = mullion::capture();
  if (!capture.enabled)
    return;
  std::lock_guard lock(capture.mutex);
  switch (capture.getNextAction(frame)) {
  case NextAction::StartCapture: {
    // Metal takes one capture at a time, and the stop of the last one may wait for what this thread commits next
    if (!capture.stopped_) {
      capture.state = State::WillCapture;
      capture.next_capture_frame = frame + 1;
      break;
    }
    if (capture.stopping_.joinable())
      capture.stopping_.join();
    WMTCaptureInfo info;
    info.capture_object = device;
    info.destination = WMTCaptureDestinationGPUTraceDocument;
    char filename[1024];
    std::time_t now;
    std::time(&now);
    std::strftime(filename, 1024, "_%H'%M'%S_%m-%d-%y.gputrace", std::localtime(&now));
    // a trace is large: it goes to MULLION_CAPTURE_PATH, else where the logs go when MULLION_LOG_PATH names a
    // directory, not next to the executable
    auto directory = env::getEnvVar("MULLION_CAPTURE_PATH");
    if (directory.empty())
      directory = env::getEnvVar("MULLION_LOG_PATH");
    if (directory == "none")
      directory.clear();
    if (!directory.empty() && directory.back() != '/' && directory.back() != '\\')
      directory += '\\';
    auto fileUrl = env::getUnixPath(directory + env::getExeBaseName() + "_F." + std::to_string(frame) + filename);
    WARN("A new capture of ", capture.frame_count, " frame(s) will be saved to ", fileUrl);
    info.output_url.set(fileUrl.c_str());
    WMT::CaptureManager::sharedCaptureManager().startCapture(info);
    break;
  }
  case NextAction::StopCapture:
    // Metal's stop waits for the device's work, which can wait for what the calling thread submits next
    capture.stopped_ = false;
    capture.stopping_ = mullion::thread([&capture] {
      WMT::CaptureManager::sharedCaptureManager().stopCapture();
      capture.stopped_ = true;
    });
    break;
  case NextAction::Nothing:
    if (capture.shouldCaptureNextFrame())
      capture.scheduleNextFrameCapture(frame + 1);
    else if (auto count = capture.triggeredFrameCount())
      capture.scheduleNextFrameCapture(frame + 1, count);
    break;
  }
}

} // namespace mullion
