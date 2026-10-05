#pragma once

#include "dxgi1_3.h"
#include "dxmt_presenter.hpp"

namespace dxmt {

// what a swap chain keeps for the settings of IDXGISwapChain1 and IDXGISwapChain2 that leave its buffers as they are
struct SwapChainSettings {
  DXGI_RGBA background{};
  DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_IDENTITY;
  // the part of the back buffer that is presented, from its origin; all of it when 0
  UINT source_width = 0, source_height = 0;

  HRESULT
  SetBackgroundColor(const DXGI_RGBA *color) {
    auto in_range = [](float c) { return c >= 0 && c <= 1; };
    if (!color || !in_range(color->r) || !in_range(color->g) || !in_range(color->b) || !in_range(color->a))
      return E_INVALIDARG;
    background = *color;
    return S_OK;
  }

  // the back buffers of a flip model swap chain may hold their picture rotated, as an application draws it for a
  // rotated display
  HRESULT
  SetRotation(const DXGI_SWAP_CHAIN_DESC1 &desc, DXGI_MODE_ROTATION value) {
    if (desc.SwapEffect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL && desc.SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD)
      return DXGI_ERROR_INVALID_CALL;
    rotation = value;
    return S_OK;
  }

  HRESULT
  SetSourceSize(const DXGI_SWAP_CHAIN_DESC1 &desc, UINT width, UINT height) {
    if (!width || !height || width > desc.Width || height > desc.Height)
      return E_INVALIDARG;
    source_width = width;
    source_height = height;
    return S_OK;
  }

  HRESULT
  GetSourceSize(const DXGI_SWAP_CHAIN_DESC1 &desc, UINT *width, UINT *height) {
    if (!width || !height)
      return E_INVALIDARG;
    *width = source_width ? source_width : desc.Width;
    *height = source_height ? source_height : desc.Height;
    return S_OK;
  }

  // new buffers are presented whole
  void
  Resized() {
    source_width = source_height = 0;
  }

  // the size of what frames are presented on, in back buffer pixels: the back buffer's, which the window stretches
  // (DXGI_SCALING_STRETCH), or the window's, when the picture keeps its size in it (DXGI_SCALING_NONE, "without any
  // scaling", which is of windows: a full-screen swap chain fills its output)
  UINT target_width = 0, target_height = 0;

  // tells the presenter how the next frame is presented. true if the target's size is another than before
  bool
  Update(const DXGI_SWAP_CHAIN_DESC1 &desc, bool windowed, HWND window, Presenter &presenter) {
    UINT width = source_width ? source_width : desc.Width, height = source_height ? source_height : desc.Height;
    DXMTPresentation presentation{{(float)width / desc.Width, (float)height / desc.Height}};
    // the back buffer holds the picture turned clockwise by quarters, and moved back into its sides: for the
    // picture's place (u, v), the back buffer's is (u cos - v sin, u sin + v cos), plus 1 where that is below 0
    // ("Supporting screen rotation": Matrix3x2F::Rotation and the Translation after it)
    UINT quarters = rotation > DXGI_MODE_ROTATION_IDENTITY ? rotation - DXGI_MODE_ROTATION_IDENTITY : 0;
    const float cosines[] = {1, 0, -1, 0};
    float cosine = cosines[quarters], sine = cosines[(quarters + 3) % 4];
    presentation.turn[0] = presentation.turn[3] = cosine;
    presentation.turn[1] = -sine;
    presentation.turn[2] = sine;
    presentation.shift[0] = cosine < 0 || sine > 0;
    presentation.shift[1] = cosine < 0 || sine < 0;
    memcpy(presentation.background, &background, sizeof(background));
    UINT width_before = target_width, height_before = target_height;
    target_width = desc.Width;
    target_height = desc.Height;
    RECT client;
    if (desc.Scaling == DXGI_SCALING_NONE && windowed && GetClientRect(window, &client) && client.right && client.bottom) {
      target_width = client.right;
      target_height = client.bottom;
      // the picture's sides as it is shown
      presentation.scale[0] = (float)target_width / (quarters % 2 ? height : width);
      presentation.scale[1] = (float)target_height / (quarters % 2 ? width : height);
    }
    presenter.changePresentation(presentation);
    return target_width != width_before || target_height != height_before;
  }
};

} // namespace dxmt
