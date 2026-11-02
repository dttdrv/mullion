#include "mullion_capture.hpp"
#include "log/log.hpp"
#include "util_env.hpp"
#include <ctime>

#ifdef __WIN32
#include "windows.h"
#endif

namespace mullion {

CaptureState::CaptureState() {
  if (env::getEnvVar("MTL_CAPTURE_ENABLED") != "1")
    return;
  if (env::getExeBaseName() != env::getEnvVar("MULLION_CAPTURE_EXECUTABLE"))
    return;
  WARN("Mullion capture enabled");
  state = State::Idle;
  try {
    if (auto frame = env::getEnvVar("MULLION_CAPTURE_FRAME"); !frame.empty())
      scheduleNextFrameCapture(std::stoull(frame));
  } catch (const std::exception &) {
  }
}

CaptureState::~CaptureState() {
  if (stopping_.joinable())
    stopping_.join();
}

void
CaptureState::frame(WMT::Device device, uint64_t frame) {
  switch (getNextAction(frame)) {
  case NextAction::StartCapture: {
    // Metal takes one capture at a time
    if (stopping_.joinable())
      stopping_.join();
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
    stopping_ = mullion::thread([] { WMT::CaptureManager::sharedCaptureManager().stopCapture(); });
    break;
  case NextAction::Nothing:
    if (shouldCaptureNextFrame())
      scheduleNextFrameCapture(frame + 1);
    break;
  }
}

CaptureState::NextAction
CaptureState::getNextAction(uint64_t frame) {
  switch (state) {
  case State::Freezed:
    return NextAction::Nothing;
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
};

void
CaptureState::scheduleNextFrameCapture(uint64_t next) {
  if (state != State::Idle)
    return;
  state = State::WillCapture;
  next_capture_frame = next;
}

bool
CaptureState::shouldCaptureNextFrame() {
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
};

} // namespace mullion
