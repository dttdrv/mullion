#pragma once
#include <cstring>
#include "Metal.hpp"
#include "dxmt_command.hpp"
#include "rc/util_rc.hpp"
#include "thread.hpp"
#include "util_cpu_fence.hpp"
#include "winemetal.h"
#include <atomic>
#include <deque>

namespace dxmt {

// how a back buffer is put on its target: the part of it that is presented, from its origin, as a fraction of each
// side; the target's size over the picture's, 1 for a picture stretched over the target; the back buffer's place for
// a place in the picture (rows of a matrix, and what each adds), which turns a picture that the back buffer holds
// rotated; and the color around a picture smaller than the target. Metal aligns a vector to its size
struct DXMTPresentation {
  alignas(2 * sizeof(float)) float source[2] = {1, 1};
  float scale[2] = {1, 1};
  alignas(4 * sizeof(float)) float turn[4] = {1, 0, 0, 1};
  float shift[2] = {0, 0};
  alignas(4 * sizeof(float)) float background[4] = {0, 0, 0, 1};

  // all of a back buffer as it is, over all of the target
  bool
  plain() const {
    static constexpr float whole[] = {1, 1, 1, 1, 1, 0, 0, 1, 0, 0};
    return !memcmp(source, whole, sizeof(source)) && !memcmp(scale, whole + 2, sizeof(scale)) &&
           !memcmp(turn, whole + 4, sizeof(turn)) && !memcmp(shift, whole + 8, sizeof(shift));
  }
};

struct DXMTPresentMetadata {
  float edr_scale;
  float max_content_luminance;
  float max_display_luminance;
  DXMTPresentation presentation;
};

constexpr uint32_t DXMT_GAMMA_CP_COUNT = 1024;

struct DXMTGammaRamp {
  float red[DXMT_GAMMA_CP_COUNT];
  float green[DXMT_GAMMA_CP_COUNT];
  float blue[DXMT_GAMMA_CP_COUNT];
  uint64_t version;
};

class Presenter : public RcObject {
public:
  // without a layer (a Wine whose winemac.drv hides the Metal view exports), frames reach `window` through GDI
  Presenter(
      WMT::Device device, WMT::MetalLayer layer, InternalCommandLibrary &lib, float scale_factor, uint8_t sample_count,
      HWND window
  );

  bool changeLayerProperties(
      WMTPixelFormat format, WMTColorSpace colorspace, double width, double height, uint8_t sample_count
  );

  bool changeLayerColorSpace(WMTColorSpace colorspace);

  void changeHDRMetadata(const WMTHDRMetadata *metadata);

  void changeGammaRamp(const DXMTGammaRamp *gamma_ramp);

  class PresentState {
  public:
    DXMTPresentMetadata metadata;
    uint64_t frame_id;
    Presenter *presenter;
    PresentState(DXMTPresentMetadata metadata, uint64_t frame_id, Presenter *presenter) :
        metadata(metadata),
        frame_id(frame_id),
        presenter(presenter) {}
    PresentState(const PresentState &copy) = delete;
    PresentState(PresentState &&move)  {
      metadata = move.metadata;
      frame_id = move.frame_id;
      presenter = move.presenter;
      move.presenter = nullptr;
    };
    ~PresentState() {
      if (presenter) {
        presenter->frame_presented_.signal(frame_id);
        presenter = nullptr;
      }
    };
  };

  PresentState synchronizeLayerProperties();

  // how later frames are put on the target (the swap chain's settings)
  void
  changePresentation(const DXMTPresentation &presentation) {
    presentation_ = presentation;
  }

  WMT::MetalDrawable encodeCommands(
      WMT::CommandBuffer cmdbuf, WMT::Texture backbuffer, DXMTPresentMetadata metadata,
      std::function<void(WMT::RenderCommandEncoder)> &&wait_fences,
      std::function<void(WMT::RenderCommandEncoder)> &&update_fences
  );

  // draws the oldest frame encoded without a layer, at least `after` seconds after the previous one (as
  // presentDrawableAfterMinimumDuration does); call it once that frame's command buffer has completed
  void drawToWindow(double after);

private:
  void buildRenderPipelineState(bool is_pq, bool with_hdr_metadata, bool is_ms, bool gamma_enable);

  WMT::Device device_;
  WMT::MetalLayer layer_;
  InternalCommandLibrary &lib_;
  WMTLayerProps layer_props_;
  uint32_t sample_count_;
  WMTColorSpace colorspace_ = WMTColorSpaceSRGB;
  WMTHDRMetadata hdr_metadata_;
  bool has_hdr_metadata_ = false;
  WMTPixelFormat source_format_ = WMTPixelFormatInvalid;
  uint64_t display_setting_version_ = 0;
  WMTColorSpace display_colorspace_ = WMTColorSpaceSRGB;
  WMTHDRMetadata display_hdr_metadata_;
  WMTEDRValue display_edr_value_{0.0, 1.0};
  DXMTPresentation presentation_;
  uint64_t gamma_version_ = 0;
  std::array<float, DXMT_GAMMA_CP_COUNT * 4> gamma_lut_rgba_;
  WMT::Reference<WMT::Texture> gamma_lut_texture_;
  WMT::Reference<WMT::RenderPipelineState> present_blit_;
  WMT::Reference<WMT::RenderPipelineState> present_scale_;
  std::atomic_flag pso_valid = 0;
  uint64_t frame_requested_ = 0;
  CpuFence frame_presented_ = 0;
  HWND window_;
  WMT::Reference<WMT::Texture> window_target_;
  struct WindowFrame {
    WMT::Reference<WMT::Buffer> buffer;
    void *pixels;
    uint32_t width, height;
  };
  std::deque<WindowFrame> window_frames_;
  std::chrono::steady_clock::time_point window_last_draw_;
  dxmt::mutex window_mutex_;
};
} // namespace dxmt