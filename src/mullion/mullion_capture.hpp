#pragma once
#include "Metal.hpp"
#include "thread.hpp"
#include <cstdint>

namespace mullion {

class CaptureState {
public:
  // a queue calls it before it commits the work of a frame: a capture of the device holds the frame
  // MULLION_CAPTURE_FRAME names, or the one after F10 is pressed
  void frame(WMT::Device device, uint64_t frame);

  CaptureState();
  // waits for a stop: the queue it belongs to submits nothing more by then
  ~CaptureState();

private:
  enum class NextAction {
    StartCapture,
    StopCapture,
    Nothing,
  };
  NextAction getNextAction(uint64_t frame);
  void scheduleNextFrameCapture(uint64_t next = 0);
  bool shouldCaptureNextFrame();

  enum class State {
    Freezed,
    Idle,
    WillCapture,
    Capturing,
  };
  State state = State::Freezed;
  uint64_t next_capture_frame = 0;
  bool capture_key_pressed_ = false;
  mullion::thread stopping_;
};

} // namespace mullion
