#pragma once
#include "Metal.hpp"
#include <cstdint>

namespace mullion {

// a queue's hold on the process's GPU capture: Metal takes one capture at a time (MTLCaptureManager), so the process
// has one, which the frames of whichever queue reach it first start and stop
class CaptureState {
public:
  // a queue calls it before it commits the work of a frame: a capture of the device holds the frame
  // MULLION_CAPTURE_FRAME names, or the one after F10 is pressed
  void frame(WMT::Device device, uint64_t frame);

  CaptureState();
  // the last hold stops a capture still under way and waits for the stop: no queue submits anything more by then
  ~CaptureState();
};

} // namespace mullion
