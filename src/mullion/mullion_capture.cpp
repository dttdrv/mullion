#include "mullion_capture.hpp"
#include "log/log.hpp"
#include "thread.hpp"
#include "util_env.hpp"
#include <atomic>
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
  bool capture_key_pressed_ = false;
  unsigned holds = 0;
  mullion::thread stopping_;
  std::atomic<bool> stopped_ = true;

  Capture() {
    if (!enabled)
      return;
    WARN("Mullion capture enabled");
    try {
      if (auto frame = env::getEnvVar("MULLION_CAPTURE_FRAME"); !frame.empty())
        scheduleNextFrameCapture(std::stoull(frame));
    } catch (const std::exception &) {
    }
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
      if (frame > next_capture_frame) {
        next_capture_frame = 0;
        state = State::Idle;
        return NextAction::StopCapture;
      }
      break;
    }
    return NextAction::Nothing;
  }

  void
  scheduleNextFrameCapture(uint64_t next = 0) {
    if (state != State::Idle)
      return;
    state = State::WillCapture;
    next_capture_frame = next;
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
    // a trace is large: it goes where the logs go, not next to the executable, when MULLION_LOG_PATH names a directory
    auto directory = env::getEnvVar("MULLION_LOG_PATH");
    if (directory == "none")
      directory.clear();
    if (!directory.empty() && directory.back() != '/' && directory.back() != '\\')
      directory += '\\';
    auto fileUrl = env::getUnixPath(directory + env::getExeBaseName() + "_F." + std::to_string(frame) + filename);
    WARN("A new capture will be saved to ", fileUrl);
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
    break;
  }
}

} // namespace mullion
