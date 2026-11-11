/*
 * Copyright 2026 David Skoland
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#pragma once

#include "d3d9_device_child.hpp"
#include "d3d9_surface.hpp"
#include "com/com_pointer.hpp"
#include "mullion_presenter.hpp"
#include "mullion_window.hpp"
#include "wsi_window.hpp"
#include <unordered_map>
#include <vector>

namespace mullion {

class MTLD3D9Device;

class MTLD3D9SwapChain final : public MTLD3D9DeviceChild<IDirect3DSwapChain9Ex> {
public:
  MTLD3D9SwapChain(MTLD3D9Device *pDevice, const D3DPRESENT_PARAMETERS &Params);
  ~MTLD3D9SwapChain();

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final;

  HRESULT STDMETHODCALLTYPE Present(
      const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion,
      DWORD dwFlags
  ) final;
  HRESULT STDMETHODCALLTYPE GetFrontBufferData(IDirect3DSurface9 *pDestSurface) final;
  HRESULT STDMETHODCALLTYPE
  GetBackBuffer(UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9 **ppBackBuffer) final;
  HRESULT STDMETHODCALLTYPE GetRasterStatus(D3DRASTER_STATUS *pRasterStatus) final;
  HRESULT STDMETHODCALLTYPE GetDisplayMode(D3DDISPLAYMODE *pMode) final;
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) final;
  HRESULT STDMETHODCALLTYPE GetPresentParameters(D3DPRESENT_PARAMETERS *pPresentationParameters) final;
  HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT *pLastPresentCount) final;
  HRESULT STDMETHODCALLTYPE GetPresentStats(D3DPRESENTSTATS *pPresentationStatistics) final;
  HRESULT STDMETHODCALLTYPE GetDisplayModeEx(D3DDISPLAYMODEEX *pMode, D3DDISPLAYROTATION *pRotation) final;

  // focus change of a fullscreen app (WM_ACTIVATEAPP on the focus window)
  void Activate(bool Active);
  // WM_WINDOWPOSCHANGING on the focus window
  void KeepOnMonitor(HWND Window, WINDOWPOS *pPos);
  bool
  IsFullscreen() const {
    return fullscreen_;
  }

  HRESULT Reset(D3DPRESENT_PARAMETERS *pParams);

  MTLD3D9Surface *
  BackBuffer(UINT Index) {
    return Index < backbuffers_.size() ? backbuffers_[Index].ptr() : nullptr;
  }

  const D3DPRESENT_PARAMETERS &
  Params() const {
    return params_;
  }

  void SetGammaRamp(DWORD Flags, const D3DGAMMARAMP *pRamp);
  void GetGammaRamp(D3DGAMMARAMP *pRamp);

private:
  HRESULT CreateBackBuffers();
  void ApplyLayerProps();
  UINT SyncInterval() const;
  HRESULT CaptureFront(const Rc<Texture> &Source);
  HRESULT SetFullscreen(const D3DPRESENT_PARAMETERS &Params);
  HRESULT ComposeRects(const RECT *pSourceRect, const RECT *pDestRect, HWND Window, Rc<Texture> &Out);
  Rc<Presenter> OverridePresenter(HWND Window);
  HRESULT AttachWindow(HWND Window);

  struct OverrideTarget {
    WMT::Object view;
    WMT::MetalLayer layer;
    Rc<Presenter> presenter;
  };

  MTLD3D9Device *parent_;
  D3DPRESENT_PARAMETERS params_;
  HWND window_;
  HMONITOR monitor_;
  WMT::Object native_view_;
  WindowFollower follower_;
  WMT::MetalLayer layer_weak_;
  Rc<Presenter> presenter_;
  std::vector<Com<MTLD3D9Surface, false>> backbuffers_;
  double refresh_rate_ = 60.0;
  UINT present_count_ = 0;
  D3DGAMMARAMP gamma_ramp_;
  MullionGammaRamp gamma_lut_;
  bool gamma_enabled_ = false;
  // a copy of each presented frame, kept once GetFrontBufferData has been called
  Rc<Texture> front_;
  Rc<Texture> composed_;
  Rc<Texture> front_8bit_;
  bool fullscreen_ = false;
  // the display mode a fullscreen chain set before moving to another window
  bool mode_changed_ = false;
  std::unordered_map<HWND, OverrideTarget> overrides_;
  wsi::MullionWindowState window_state_;
  bool keep_front_ = false;
};

} // namespace mullion
