
#include <algorithm>
#include "Metal.hpp"
#include "dxmt_format.hpp"
#include "dxmt_presenter.hpp"
#include "util_likely.hpp"


namespace dxmt {

Presenter::Presenter(WMT::Device device, WMT::MetalLayer layer, InternalCommandLibrary &lib, float scale_factor, uint8_t sample_count, HWND window) :
    device_(device),
    layer_(layer),
    lib_(lib),
    sample_count_(sample_count),
    window_(window) {
  layer_.getProps(layer_props_);
  layer_props_.device = device;
  layer_props_.opaque = true;
  layer_props_.display_sync_enabled = false;
  layer_props_.framebuffer_only = false; // how strangely setting it true results in worse performance
  layer_props_.contents_scale = layer_props_.contents_scale * scale_factor;

  WMTTextureInfo texture_info{};
  texture_info.type = WMTTextureType2D;
  texture_info.pixel_format = WMTPixelFormatRGBA32Float;
  texture_info.usage = WMTTextureUsageShaderRead;
  texture_info.options = WMTResourceStorageModeShared;
  texture_info.width = DXMT_GAMMA_CP_COUNT;
  texture_info.height = 1;
  texture_info.depth = 1;
  texture_info.mipmap_level_count = 1;
  texture_info.sample_count = 1;
  texture_info.array_length = 1;
  gamma_lut_texture_ = device.newTexture(texture_info);
}

bool
Presenter::changeLayerProperties(
    WMTPixelFormat format, WMTColorSpace colorspace, double width, double height, uint8_t sample_count
) {
  bool should_invalidated = colorspace_ != colorspace;
  should_invalidated |= source_format_ != format;
  source_format_ = format;
  should_invalidated |= sample_count_ != sample_count;
  sample_count_ = sample_count;
  // a 32-bit GDI bitmap is BGRA
  layer_props_.pixel_format = layer_ ? Forget_sRGB(format) : WMTPixelFormatBGRA8Unorm;
  layer_props_.drawable_height = height;
  layer_props_.drawable_width = width;
  colorspace_ = colorspace;
  if (should_invalidated)
    pso_valid.clear(); // defer changes
  else
    layer_.setProps(layer_props_);
  return should_invalidated;
}

bool
Presenter::changeLayerColorSpace(WMTColorSpace colorspace) {
  bool should_invalidated = colorspace_ != colorspace;
  colorspace_ = colorspace;
  if (should_invalidated)
    pso_valid.clear();
  return should_invalidated;
}

void
Presenter::changeHDRMetadata(const WMTHDRMetadata *metadata) {
  if (metadata) {
    has_hdr_metadata_ = true;
    hdr_metadata_ = *metadata;
    pso_valid.clear();
  } else {
    if (has_hdr_metadata_) {
      has_hdr_metadata_ = false;
      pso_valid.clear();
    }
  }
}

void
Presenter::changeGammaRamp(const DXMTGammaRamp *gamma_ramp) {
  if (!gamma_ramp && gamma_version_ != 0) {
    gamma_version_ = 0;
    pso_valid.clear();
    return;
  }
  if (gamma_ramp && gamma_version_ != gamma_ramp->version) {
    gamma_version_ = gamma_ramp->version;
    for (uint32_t i = 0; i < DXMT_GAMMA_CP_COUNT; i++) {
      gamma_lut_rgba_[i * 4 + 0] = std::clamp(gamma_ramp->red[i], 0.f, 1.f);
      gamma_lut_rgba_[i * 4 + 1] = std::clamp(gamma_ramp->green[i], 0.f, 1.f);
      gamma_lut_rgba_[i * 4 + 2] = std::clamp(gamma_ramp->blue[i], 0.f, 1.f);
      gamma_lut_rgba_[i * 4 + 3] = 1.0f;
    }
    gamma_lut_texture_.replaceRegion(
          {0, 0, 0}, {DXMT_GAMMA_CP_COUNT, 1, 1}, 0, 0,
          gamma_lut_rgba_.data(),
          DXMT_GAMMA_CP_COUNT * sizeof(float) * 4,
          0);
    pso_valid.clear();
  }
}

Presenter::PresentState
Presenter::synchronizeLayerProperties() {
  uint64_t display_setting_version = 0;

  WMTQueryDisplaySettingForLayer(
      layer_.handle, &display_setting_version, &display_colorspace_, &display_hdr_metadata_, &display_edr_value_
  );

  if (display_setting_version != display_setting_version_) {
    display_setting_version_ = display_setting_version;
    pso_valid.clear();
  }

  auto final_colorspace = display_setting_version_ > 0 ? display_colorspace_ : colorspace_;
  auto is_hdr = WMT_COLORSPACE_IS_HDR(final_colorspace);
  auto hdr_metadata = display_setting_version_ > 0 ? &display_hdr_metadata_
                      : has_hdr_metadata_          ? &hdr_metadata_
                                                   : nullptr;
  if (unlikely(!pso_valid.test_and_set())) {
    frame_presented_.wait(frame_requested_);
    buildRenderPipelineState(final_colorspace == WMTColorSpaceHDR_PQ, is_hdr && hdr_metadata != nullptr, sample_count_ > 1, gamma_version_ != 0);
    layer_.setProps(layer_props_);
    layer_.setColorSpace(final_colorspace);
  }

  DXMTPresentMetadata metadata;

  metadata.presentation = presentation_;
  metadata.edr_scale = 1.0;
  metadata.max_content_luminance = 10000;
  metadata.max_display_luminance = display_edr_value_.maximum_potential_edr_color_component_value * 100;

  if (is_hdr) {
    metadata.edr_scale = display_edr_value_.maximum_edr_color_component_value /
                display_edr_value_.maximum_potential_edr_color_component_value;

    if (hdr_metadata) {
      metadata.max_content_luminance = std::max<uint32_t>(
          {(uint32_t)metadata.max_display_luminance, hdr_metadata->max_content_light_level,
           hdr_metadata->max_mastering_luminance, hdr_metadata->max_frame_average_light_level}
      );
    }
  }

  if (final_colorspace == WMTColorSpaceHDR_scRGB)
    metadata.edr_scale *= 0.8;

  return {metadata, ++frame_requested_, this};
}

WMT::MetalDrawable
Presenter::encodeCommands(
    WMT::CommandBuffer cmdbuf, WMT::Texture backbuffer, DXMTPresentMetadata metadata,
    std::function<void(WMT::RenderCommandEncoder)> &&wait_fences,
    std::function<void(WMT::RenderCommandEncoder)> &&update_fences
) {
  double width = layer_props_.drawable_width;
  double height = layer_props_.drawable_height;

  auto drawable = layer_ ? layer_.nextDrawable() : WMT::MetalDrawable{};
  if (!drawable && (!window_target_ || window_target_.width() != width || window_target_.height() != height)) {
    WMTTextureInfo texture_info{};
    texture_info.type = WMTTextureType2D;
    texture_info.pixel_format = layer_props_.pixel_format;
    texture_info.usage = WMTTextureUsageRenderTarget;
    texture_info.options = WMTResourceStorageModePrivate;
    texture_info.width = width;
    texture_info.height = height;
    texture_info.depth = 1;
    texture_info.mipmap_level_count = 1;
    texture_info.sample_count = 1;
    texture_info.array_length = 1;
    window_target_ = device_.newTexture(texture_info);
  }
  WMT::Texture target = drawable ? drawable.texture() : window_target_;

  WMTRenderPassInfo info;
  WMT::InitializeRenderPassInfo(info);
  info.colors[0].load_action = WMTLoadActionDontCare;
  info.colors[0].store_action = WMTStoreActionStore;
  info.colors[0].texture = target;
  auto encoder = cmdbuf.renderCommandEncoder(info);
  wait_fences(encoder);
  encoder.setFragmentTexture(backbuffer, 0);
  encoder.setFragmentTexture(gamma_lut_texture_, 1);

  encoder.setFragmentBytes(&metadata, sizeof(metadata), 0);
  // a back buffer as large as the target is read texel for texel, when all of it is presented as it is
  if (backbuffer.width() == (uint64_t)width && backbuffer.height() == (uint64_t)height && metadata.presentation.plain()) {
    encoder.setRenderPipelineState(present_blit_);
  } else {
    encoder.setRenderPipelineState(present_scale_);
  }
  encoder.setViewport({0, 0, width, height, 0, 1});
  encoder.drawPrimitives(WMTPrimitiveTypeTriangle, 0, 3);
  update_fences(encoder);
  encoder.endEncoding();

  // a layer whose drawable timed out drops this frame; only a window without a layer takes frames through GDI
  if (!layer_) {
    WindowFrame frame{{}, nullptr, (uint32_t)width, (uint32_t)height};
    WMTBufferInfo buffer_info{.length = frame.width * frame.height * 4ull, .options = WMTResourceStorageModeShared};
    buffer_info.memory.set(nullptr);
    frame.buffer = device_.newBuffer(buffer_info);
    frame.pixels = buffer_info.memory.get();
    wmtcmd_blit_copy_from_texture_to_buffer copy{.type = WMTBlitCommandCopyFromTextureToBuffer};
    copy.next.set(nullptr);
    copy.src = target.handle;
    copy.size = {frame.width, frame.height, 1};
    copy.dst = frame.buffer.handle;
    copy.bytes_per_row = frame.width * 4;
    copy.bytes_per_image = frame.width * frame.height * 4;
    auto blit = cmdbuf.blitCommandEncoder();
    blit.encodeCommands((const wmtcmd_blit_nop *)&copy);
    blit.endEncoding();
    std::lock_guard lock(window_mutex_);
    window_frames_.push_back(std::move(frame));
  }
  return drawable;
}

void
Presenter::drawToWindow(double after) {
  WindowFrame frame;
  {
    std::lock_guard lock(window_mutex_);
    if (window_frames_.empty())
      return;
    frame = std::move(window_frames_.front());
    window_frames_.pop_front();
  }
  std::this_thread::sleep_until(window_last_draw_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                        std::chrono::duration<double>(after)
                                                    ));
  // a top-down bitmap: negative height
  BITMAPINFO bitmap{{sizeof(BITMAPINFOHEADER), (LONG)frame.width, -(LONG)frame.height, 1, 32, BI_RGB}};
  // the window may be gone, and its own DC may be in use on its thread (CS_OWNDC): a cache DC, or no frame
  RECT client;
  HDC dc;
  if (!GetClientRect(window_, &client) || !(dc = GetDCEx(window_, nullptr, DCX_CACHE)))
    return;
  StretchDIBits(
      dc, 0, 0, client.right, client.bottom, 0, 0, frame.width, frame.height, frame.pixels, &bitmap, DIB_RGB_COLORS,
      SRCCOPY
  );
  ReleaseDC(window_, dc);
  window_last_draw_ = std::chrono::steady_clock::now();
}

void
Presenter::buildRenderPipelineState(bool is_pq, bool with_hdr_metadata, bool is_ms, bool gamma_enable) {
  auto pool = WMT::MakeAutoreleasePool();

  auto library = lib_.getLibrary();

  uint32_t true_data = true, false_data = false;
  WMTFunctionConstant constants[6];
  constants[0].data.set(&true_data);
  constants[0].type = WMTDataTypeBool;
  constants[0].index = kPresentFCIndex_BackbufferSizeMatched;
  constants[1].data.set(is_pq ? &true_data : &false_data);
  constants[1].type = WMTDataTypeBool;
  constants[1].index = kPresentFCIndex_HDRPQ;
  constants[2].data.set(with_hdr_metadata ? &true_data : &false_data);
  constants[2].type = WMTDataTypeBool;
  constants[2].index = kPresentFCIndex_WithHDRMetadata;
  constants[3].data.set(Is_sRGBVariant(source_format_) ? &true_data : &false_data);
  constants[3].type = WMTDataTypeBool;
  constants[3].index = kPresentFCIndex_BackbufferIsSRGB;
  constants[4].data.set(is_ms ? &true_data : &false_data);
  constants[4].type = WMTDataTypeBool;
  constants[4].index = kPresentFCIndex_BackbufferIsMS;
  constants[5].data.set(gamma_enable ? &true_data : &false_data);
  constants[5].type  = WMTDataTypeBool;
  constants[5].index = kPresentFCIndex_GammaEnabled;

  WMT::Reference<WMT::Error> error;
  auto vs_present_quad = library.newFunction("vs_present_quad");
  auto fs_present_quad = library.newFunctionWithConstants("fs_present_quad", constants, std::size(constants), error);

  constants[0].data.set(&false_data);
  auto fs_present_quad_scaled =
      library.newFunctionWithConstants("fs_present_quad", constants, std::size(constants), error);
  {
    WMTRenderPipelineInfo present_pipeline;
    WMT::InitializeRenderPipelineInfo(present_pipeline);
    present_pipeline.colors[0].pixel_format = layer_props_.pixel_format;
    present_pipeline.vertex_function = vs_present_quad;
    present_pipeline.fragment_function = fs_present_quad;
    present_blit_ = device_.newRenderPipelineState(present_pipeline, error);
    present_pipeline.fragment_function = fs_present_quad_scaled;
    present_scale_ = device_.newRenderPipelineState(present_pipeline, error);
  }
}

} // namespace dxmt