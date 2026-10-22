/*
 * Copyright 2026 Feifan He for CodeWeavers
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

#include "mullion_window.hpp"
#include "d3d12_device.hpp"
#include "dxgi_interfaces.h"
#include "dxgi_object.hpp"
#include "dxgi_output.hpp"
#include "mullion_hud_state.hpp"
#include "mullion_presenter.hpp"
#include "mullion_format.hpp"
#include "dxgi_swapchain_settings.hpp"
#include "mullion_info.hpp"
#include "com/com_pointer.hpp"
#include "log/log.hpp"
#include "wsi_window.hpp"
#include "util_cpu_fence.hpp"
#include "util_hotpatch.h"
#include "config/config.hpp"
#include <cfloat>

/**
Ref: https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-setmaximumframelatency
This value is 1 by default.
*/
constexpr size_t kSwapchainLatency = 1;

/**
TODO: some implementations can be shared with d3d11 swapchain
*/

namespace mullion {

WMTColorSpace
ConvertColorSpace(DXGI_COLOR_SPACE_TYPE color_space, bool hdr) {
  switch (color_space) {
  case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
    return WMTColorSpaceSRGB;
  case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
    return hdr ? WMTColorSpaceHDR_scRGB : WMTColorSpaceSRGBLinear;
  case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
    return WMTColorSpaceHDR_PQ;
  case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020:
    return WMTColorSpaceBT2020;
  default:
    return WMTColorSpaceInvalid;
  }
}

/**
 FIXME: duplicated implementation in dxgi_output.cpp
*/
uint32_t
GetMonitorFormatBpp(DXGI_FORMAT Format) {
  switch (Format) {
  case DXGI_FORMAT_R8G8B8A8_UNORM:
  case DXGI_FORMAT_B8G8R8A8_UNORM:
  case DXGI_FORMAT_B8G8R8X8_UNORM:
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
  case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
  case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
  case DXGI_FORMAT_R10G10B10A2_UNORM:
  case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:
    return 32;

  case DXGI_FORMAT_R16G16B16A16_FLOAT:
    return 64;

  default:
    Logger::warn(str::format("GetMonitorFormatBpp: Unknown format: ", Format));
    return 32;
  }
}

class ModeSetGuard {
  std::atomic_flag in_progress_;
public:
  class ModeSetInProgress {
    ModeSetGuard &guard_;
    bool protected_;

  public:
    ModeSetInProgress(ModeSetGuard &guard) : guard_(guard) {
      protected_ = guard_.in_progress_.test_and_set();
    }
    ~ModeSetInProgress() {
      if (!protected_) {
        guard_.in_progress_.clear();
      }
    }
    operator bool() {
      return protected_;
    }
  };
};

class MTLD3D12SwapChain final : public MTLDXGISubObject<IDXGISwapChain4, MTLD3D12Device> {

  Com<IDXGIFactory1> factory_;
  Com<MTLD3D12CommandQueue> queue_;
  HWND hWnd;
  HMONITOR monitor_;
  Com<IDXGIOutput1> target_;
  wsi::MullionWindowState window_state_;
  ULONG presentation_count_ = 0;
  uint64_t dump_id_ = 0;
  // the back buffer the next Present shows
  UINT current_buffer_ = 0;
  DXGI_SWAP_CHAIN_DESC1 desc_;
  SwapChainSettings settings_;
  D3D12_RESOURCE_DESC backbuffer_desc_;
  DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen_desc_;
  // the display mode the chain applies when it enters fullscreen: its created mode, or the mode a
  // ResizeTarget set while it was fullscreen
  DXGI_MODE_DESC1 transition_mode_;
  DXGI_MODE_DESC1 created_mode_;
  HANDLE present_semaphore_;
  std::unique_ptr<CpuFence> frame_latency_fence_;
  WMT::Object native_view_;
  WindowFollower follower_;
  WMT::MetalLayer layer_weak_;
  Rc<Presenter> presenter;
  uint32_t frame_latency;
  DXGI_COLOR_SPACE_TYPE colorspace_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
  float scale_factor = 1.0;
  HUDState hud;
  double init_refresh_rate_ = DBL_MAX;
  int preferred_max_frame_rate = 0;
  ModeSetGuard modeset_guard_;
  mullion::mutex mutex_;

  D3D12SwapChainBufferRefs buffer_refs_{this};
  std::vector<Com<MTLD3D12Resource, false>> backbuffers_;

  bool
  LayerSupportEDR() {
    WMTEDRValue edr_value;
    MetalLayer_getEDRValue(layer_weak_, &edr_value);
    return edr_value.maximum_potential_edr_color_component_value > 1.0f;
  };

  InternalCommandLibrary lib;

public:
  MTLD3D12SwapChain(
      IDXGIFactory1 *pFactory, MTLD3D12Device *pDevice, MTLD3D12CommandQueue *pQueue, HWND hWnd,
      const DXGI_SWAP_CHAIN_DESC1 *pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc
  ) :
      MTLDXGISubObject(pDevice),
      factory_(pFactory),
      queue_(pQueue),
      hWnd(hWnd),
      monitor_(wsi::getWindowMonitor(hWnd)),
      desc_(*pDesc),
      hud(WMT::DeveloperHUDProperties::instance()),
      lib(pDevice->GetMTLDevice()) {

    if (!dump_frames.empty()) {
      static std::atomic_uint64_t next_id = 0;
      dump_id_ = ++next_id;
    }

    // frames can go to the window through GDI instead, as they do when a window gets no Metal view: slower, and
    // readable from the window
    if (!Config::getInstance().getOption<bool>("dxgi.presentThroughGDI", false))
      native_view_ = WMT::CreateMetalViewFromHWND((intptr_t)hWnd, follower_.placement(hWnd), pDevice->GetMTLDevice(), layer_weak_);
    follower_.follow(hWnd, native_view_, true);

    if (!native_view_) {
      WARN("No Metal view for this window; presenting through GDI (slower).");
    }

    presenter =
        Rc(new Presenter(pDevice->GetMTLDevice(), layer_weak_, lib, scale_factor, desc_.SampleDesc.Count, hWnd));

    frame_latency = kSwapchainLatency;
    present_semaphore_ = CreateSemaphore(nullptr, frame_latency, DXGI_MAX_SWAP_CHAIN_BUFFERS, nullptr);

    if (desc_.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
      frame_latency_fence_ = std::make_unique<CpuFence>();
    }

    if (desc_.Width == 0 || desc_.Height == 0) {
      wsi::getWindowSize(hWnd, &desc_.Width, &desc_.Height);
    }

    fullscreen_desc_ = pFullscreenDesc ? *pFullscreenDesc : DXGI_SWAP_CHAIN_FULLSCREEN_DESC{};
    fullscreen_desc_.Windowed = TRUE;
    transition_mode_ = {desc_.Width, desc_.Height, fullscreen_desc_.RefreshRate, desc_.Format,
                        fullscreen_desc_.ScanlineOrdering, fullscreen_desc_.Scaling};
    created_mode_ = transition_mode_;

    preferred_max_frame_rate = Config::getInstance().getOption<int>("d3d12.preferredMaxFrameRate", 0);
    wsi::WsiMode current_mode;
    if (wsi::getCurrentDisplayMode(monitor_, &current_mode) && current_mode.refreshRate.denominator != 0 &&
        current_mode.refreshRate.numerator != 0) {
      init_refresh_rate_ = (double)current_mode.refreshRate.numerator / (double)current_mode.refreshRate.denominator;
    }

    hud.initialize(GetVersionDescriptionText(12, device_->GetFeatureLevel()));

    backbuffer_desc_ = D3D12_RESOURCE_DESC{
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,
        .Width = desc_.Width,
        .Height = desc_.Height,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = desc_.Format,
        .SampleDesc =
            {
                .Count = 1,
                .Quality = 0,
            },
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
    };
    if (desc_.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS)
      backbuffer_desc_.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  }

  ~MTLD3D12SwapChain() {
    if (!fullscreen_desc_.Windowed)
      static_cast<MTLDXGIOutput *>(target_.ptr())->SetFullscreen(false);
    WMT::ReleaseMetalView(native_view_);
    native_view_ = {};
    CloseHandle(present_semaphore_);
  };

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) final {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDeviceSubObject) ||
        riid == __uuidof(IDXGISwapChain) || riid == __uuidof(IDXGISwapChain1) || riid == __uuidof(IDXGISwapChain2) ||
        riid == __uuidof(IDXGISwapChain3) || riid == __uuidof(IDXGISwapChain4)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(IDXGISwapChain1), riid)) {
      WARN("DXGISwapChain: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  };

  HRESULT
  STDMETHODCALLTYPE
  GetParent(REFIID riid, void **parent) final {
    return factory_->QueryInterface(riid, parent);
  };

  MULLION_HOTPATCHABLE
  HRESULT
  STDMETHODCALLTYPE
  Present(UINT sync_interval, UINT flags) final {
    return Present1(sync_interval, flags, nullptr);
  };

  HRESULT
  STDMETHODCALLTYPE
  GetBuffer(UINT Buffer, REFIID riid, void **ppSurface) final {
    if (Buffer >= backbuffers_.size())
      return DXGI_ERROR_NOT_FOUND;
    return backbuffers_[Buffer]->QueryInterface(riid, ppSurface);
  };

  MULLION_HOTPATCHABLE
  HRESULT
  STDMETHODCALLTYPE
  SetFullscreenState(BOOL Fullscreen, IDXGIOutput *pTarget) final {
    if (!Fullscreen && pTarget)
      return DXGI_ERROR_INVALID_CALL;
    ModeSetGuard::ModeSetInProgress modeset_inprogress(modeset_guard_);
    if (modeset_inprogress)
      return !!Fullscreen == !fullscreen_desc_.Windowed ? S_OK : DXGI_STATUS_MODE_CHANGE_IN_PROGRESS;
    Com<IDXGIOutput1> target;

    if (pTarget) {
      DXGI_OUTPUT_DESC desc;

      if (HRESULT hr = pTarget->QueryInterface(IID_PPV_ARGS(&target)); FAILED(hr))
        return hr;
      if (HRESULT hr = target->GetDesc(&desc); FAILED(hr))
        return hr;

      if (!fullscreen_desc_.Windowed && Fullscreen && monitor_ != desc.Monitor) {
        HRESULT hr = LeaveFullscreenMode();
        if (FAILED(hr))
          return hr;
        // another output takes the chain's own mode, not the mode the output it leaves was put in
        std::lock_guard<mullion::mutex> lock(mutex_);
        transition_mode_ = created_mode_;
      }
    }

    if (fullscreen_desc_.Windowed && Fullscreen)
      return EnterFullscreenMode(target.ptr());
    else if (!fullscreen_desc_.Windowed && !Fullscreen)
      return LeaveFullscreenMode();

    if (target && !fullscreen_desc_.Windowed)
      target_ = std::move(target);
    return S_OK;
  };

  HRESULT
  EnterFullscreenMode(IDXGIOutput1 *pTarget) {
    Com<IDXGIOutput1> output = pTarget;

    if (!wsi::isWindow(hWnd))
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;

    if (output == nullptr) {
      if (FAILED(GetOutputFromMonitor(wsi::getWindowMonitor(hWnd), &output))) {
        ERR("DXGI: EnterFullscreenMode: Cannot query containing output");
        return E_FAIL;
      }
    }

    if (!GetWindowRect(hWnd, &window_state_.rect))
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;

    DXGI_OUTPUT_DESC desc;
    if (HRESULT hr = output->GetDesc(&desc); FAILED(hr))
      return hr;

    // the mode this transition applies is the one the chain is in, size and rate together: a
    // ResizeTarget while fullscreen sets the display mode and the next entry re-applies that mode.
    // Taking the rate from there and the size from the buffer instead composes a mode nobody asked
    // for -- 640x480@120 after a ResizeTarget to 1728x1117@120 -- which ChangeDisplayMode's
    // exact-mode rule then refuses on any output that has the size at one rate only.
    DXGI_MODE_DESC1 preferred_display_mode = transition_mode_;
    if (preferred_display_mode.Format == DXGI_FORMAT_UNKNOWN)
      preferred_display_mode.Format = desc_.Format;
    bool modeSwitch = (desc_.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH) != 0u;
    if (modeSwitch && FAILED(ChangeDisplayMode(output.ptr(), &preferred_display_mode))) {
      ERR("DXGI: EnterFullscreenMode: Failed to change display mode");
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    }

    if (!wsi::enterFullscreenMode(desc.Monitor, hWnd, &window_state_, modeSwitch)) {
      if (modeSwitch && FAILED(RestoreDisplayMode(desc.Monitor)))
        ERR("DXGI: EnterFullscreenMode: Failed to restore display mode");
      ERR("DXGI: EnterFullscreenMode: Failed to enter fullscreen mode");
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    }
    std::lock_guard<mullion::mutex> lock(mutex_);
    fullscreen_desc_.Windowed = FALSE;
    static_cast<MTLDXGIOutput *>(output.ptr())->SetFullscreen(true);
    monitor_ = desc.Monitor;
    target_ = std::move(output);
    return S_OK;
  }

  HRESULT
  LeaveFullscreenMode() {
    if (HRESULT hr = RestoreDisplayMode(monitor_); FAILED(hr)) {
      WARN("DXGI: LeaveFullscreenMode: Failed to restore display mode");
      return hr;
    }

    if (wsi::isWindow(hWnd) && !wsi::leaveFullscreenMode(hWnd, &window_state_, true)) {
      ERR("DXGI: LeaveFullscreenMode: Failed to exit fullscreen mode");
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    }
    auto monitor = wsi::getWindowMonitor(hWnd);
    std::lock_guard<mullion::mutex> lock(mutex_);
    fullscreen_desc_.Windowed = TRUE;
    static_cast<MTLDXGIOutput *>(target_.ptr())->SetFullscreen(false);
    target_ = nullptr;
    monitor_ = monitor;
    presenter->changeGammaRamp(nullptr);

    return S_OK;
  }

  HRESULT
  ChangeDisplayMode(IDXGIOutput1 *pOutput, DXGI_MODE_DESC1 *pDisplayMode) {
    if (!pOutput)
      return DXGI_ERROR_INVALID_CALL;

    // Find a mode that the output supports

    DXGI_MODE_DESC1 preferred_mode = *pDisplayMode;
    DXGI_MODE_DESC1 selected_mode = {};

    if (preferred_mode.Format == DXGI_FORMAT_UNKNOWN)
      preferred_mode.Format = desc_.Format;

    HRESULT hr = pOutput->FindClosestMatchingMode1(&preferred_mode, &selected_mode, nullptr);

    if (FAILED(hr)) {
      ERR("DXGI: Failed to query closest mode:"
          "\n"
          "  Format: ",
          preferred_mode.Format,
          "\n"
          "  Mode:   ",
          preferred_mode.Width, "x", preferred_mode.Height, "@",
          preferred_mode.RefreshRate.Numerator / std::max(preferred_mode.RefreshRate.Denominator, 1u));
      return hr;
    }

    // A fullscreen transition applies the mode the swap chain was created with, or it fails. Wine's own dxgi
    // does not pass WINED3D_SWAPCHAIN_USE_CLOSEST_MATCHING_MODE to wined3d (dlls/dxgi/utils.c, dxgi_swapchain
    // flags), so wined3d requires the exact mode and its failure becomes DXGI_ERROR_NOT_CURRENTLY_AVAILABLE.
    // Taking the closest match here and reporting success instead is what d3d11_fullscreen recorded as a switch
    // that handed back a different mode than it asked for: 960x600@59 was answered with 960x600@60, and
    // 2624x1696 with 1728x1117, both with S_OK.
    if (!selected_mode.RefreshRate.Denominator)
      selected_mode.RefreshRate.Denominator = 1;
    const bool same_size =
        selected_mode.Width == preferred_mode.Width && selected_mode.Height == preferred_mode.Height;
    const bool rate_asked =
        preferred_mode.RefreshRate.Numerator && preferred_mode.RefreshRate.Denominator;
    const bool same_rate =
        !rate_asked || uint64_t(selected_mode.RefreshRate.Numerator) * preferred_mode.RefreshRate.Denominator ==
                           uint64_t(preferred_mode.RefreshRate.Numerator) * selected_mode.RefreshRate.Denominator;
    if (!same_size || !same_rate) {
      ERR("DXGI: the output has no mode ",
          preferred_mode.Width, "x", preferred_mode.Height, "@",
          preferred_mode.RefreshRate.Numerator / std::max(preferred_mode.RefreshRate.Denominator, 1u),
          "; the closest is ", selected_mode.Width, "x", selected_mode.Height, "@",
          selected_mode.RefreshRate.Numerator / std::max(selected_mode.RefreshRate.Denominator, 1u));
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    }

    DXGI_OUTPUT_DESC output_desc;
    if (HRESULT hr = pOutput->GetDesc(&output_desc); FAILED(hr))
      return hr;
    wsi::WsiMode wsi_mode{
        selected_mode.Width,
        selected_mode.Height,
        {selected_mode.RefreshRate.Numerator, selected_mode.RefreshRate.Denominator},
        GetMonitorFormatBpp(selected_mode.Format),
        selected_mode.ScanlineOrdering == DXGI_MODE_SCANLINE_ORDER_UPPER_FIELD_FIRST ||
            selected_mode.ScanlineOrdering == DXGI_MODE_SCANLINE_ORDER_LOWER_FIELD_FIRST
    };
    if (!wsi::setWindowMode(output_desc.Monitor, hWnd, wsi_mode))
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;

    *pDisplayMode = selected_mode;
    init_refresh_rate_ = double(selected_mode.RefreshRate.Numerator) / double(selected_mode.RefreshRate.Denominator);
    return S_OK;
  }

  HRESULT
  RestoreDisplayMode(HMONITOR hMonitor) {
    if (!hMonitor)
      return DXGI_ERROR_INVALID_CALL;

    if (!wsi::restoreDisplayMode(hMonitor))
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;

    init_refresh_rate_ = DBL_MAX;
    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  GetFullscreenState(BOOL *pFullscreen, IDXGIOutput **ppTarget) final {
    HRESULT hr = S_OK;

    if (pFullscreen != nullptr)
      *pFullscreen = !fullscreen_desc_.Windowed;

    if (ppTarget != nullptr)
      *ppTarget = target_.ref();

    return hr;
  };

  HRESULT
  STDMETHODCALLTYPE
  GetDesc(DXGI_SWAP_CHAIN_DESC *pDesc) final {
    if (!pDesc)
      return E_INVALIDARG;

    pDesc->BufferDesc.Width = desc_.Width;
    pDesc->BufferDesc.Height = desc_.Height;
    pDesc->BufferDesc.RefreshRate = fullscreen_desc_.RefreshRate;
    pDesc->BufferDesc.Format = desc_.Format;
    pDesc->BufferDesc.ScanlineOrdering = fullscreen_desc_.ScanlineOrdering;
    pDesc->BufferDesc.Scaling = fullscreen_desc_.Scaling;
    pDesc->SampleDesc = desc_.SampleDesc;
    pDesc->BufferUsage = desc_.BufferUsage;
    pDesc->BufferCount = desc_.BufferCount;
    pDesc->OutputWindow = hWnd;
    pDesc->Windowed = fullscreen_desc_.Windowed;
    pDesc->SwapEffect = desc_.SwapEffect;
    pDesc->Flags = desc_.Flags;
    return S_OK;
  };

  MULLION_HOTPATCHABLE
  HRESULT
  STDMETHODCALLTYPE
  ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format, UINT flags) final {
    // held buffers cannot be retired (IDXGISwapChain::ResizeBuffers, Remarks).
    {
      std::lock_guard<mullion::mutex> lock(buffer_refs_.mutex);
      if (buffer_refs_.referenced_buffers)
        return DXGI_ERROR_INVALID_CALL;
    }
    // the new buffers first: the swap chain stays as it is when they cannot be made. 0 and DXGI_FORMAT_UNKNOWN keep
    // what there is (the window's size for a size of 0), and the flags are the swap chain's from now on, except that
    // tearing is allowed for a swap chain's whole life (IDXGISwapChain::ResizeBuffers)
    auto desc = desc_;
    wsi::getWindowSize(hWnd, &desc.Width, &desc.Height);
    if (Width)
      desc.Width = Width;
    if (Height)
      desc.Height = Height;
    if (Format != DXGI_FORMAT_UNKNOWN)
      desc.Format = Format;
    if (BufferCount)
      desc.BufferCount = BufferCount;
    if (!IsFlipSwapChainFormat(desc.Format))
      return DXGI_ERROR_INVALID_CALL;
    if (desc.BufferCount > DXGI_MAX_SWAP_CHAIN_BUFFERS)
      return E_INVALIDARG;
    if ((flags ^ desc_.Flags) & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)
      return DXGI_ERROR_INVALID_CALL;
    desc.Flags = flags;
    auto backbuffer_desc = backbuffer_desc_;
    backbuffer_desc.Width = std::max(desc.Width, 1u);
    backbuffer_desc.Height = std::max(desc.Height, 1u);
    backbuffer_desc.Format = desc.Format;
    decltype(backbuffers_) backbuffers;
    D3D12_HEAP_PROPERTIES heap_props = device_->GetCustomHeapProperties(0, D3D12_HEAP_TYPE_DEFAULT);
    for (unsigned i = 0; i < desc.BufferCount; i++) {
      Com<ID3D12Resource> backbuffer;
      if (HRESULT hr = mullion::CreateCommittedTexture(
              device_, &heap_props, D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES, &backbuffer_desc,
              D3D12_RESOURCE_STATE_PRESENT, nullptr, IID_PPV_ARGS(&backbuffer), &buffer_refs_
          );
          FAILED(hr))
        return hr;
      backbuffers.push_back(reinterpret_cast<MTLD3D12Resource *>(backbuffer.ptr()));
    }
    desc_ = desc;
    backbuffer_desc_ = backbuffer_desc;
    backbuffers_ = std::move(backbuffers);
    // the first of the new buffers is the next to be presented
    current_buffer_ = 0;
    settings_.Resized();
    ApplyLayerProps();
    return S_OK;
  };

  MULLION_HOTPATCHABLE
  HRESULT
  STDMETHODCALLTYPE
  ResizeTarget(const DXGI_MODE_DESC *pDesc) final {
    if (!pDesc)
      return DXGI_ERROR_INVALID_CALL;

    if (!wsi::isWindow(hWnd))
      return DXGI_ERROR_INVALID_CALL;

    ModeSetGuard::ModeSetInProgress modeset_inprogress(modeset_guard_);
    if (modeset_inprogress)
      return DXGI_STATUS_MODE_CHANGE_IN_PROGRESS;

    // a mode with no pixels on an axis is not a size; whatever windowed or fullscreen means here,
    // accepting it would report a resize that cannot be one. a nested call is answered above, as
    // Wine's own dxgi answers the transition in progress before looking at the mode.
    if (pDesc->Width == 0 || pDesc->Height == 0)
      return DXGI_ERROR_INVALID_CALL;

    // Promote display mode
    DXGI_MODE_DESC1 newDisplayMode = {};
    newDisplayMode.Width = pDesc->Width;
    newDisplayMode.Height = pDesc->Height;
    newDisplayMode.RefreshRate = pDesc->RefreshRate;
    newDisplayMode.Format = pDesc->Format;
    newDisplayMode.ScanlineOrdering = pDesc->ScanlineOrdering;
    newDisplayMode.Scaling = pDesc->Scaling;

    bool applied = false;
    if (fullscreen_desc_.Windowed) {
      if (!wsi::resizeWindow(hWnd, &window_state_, newDisplayMode.Width, newDisplayMode.Height))
        return DXGI_ERROR_INVALID_CALL;
    } else {
      RECT bounds{}, rect{};
      if (!wsi::getDesktopCoordinates(monitor_, &bounds) || !GetWindowRect(hWnd, &rect))
        return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
      auto state = window_state_;
      if (rect.right - rect.left != bounds.right - bounds.left ||
          rect.bottom - rect.top != bounds.bottom - bounds.top) {
        state.rect = rect;
        state.style = GetWindowLongW(hWnd, GWL_STYLE);
        state.exstyle = GetWindowLongW(hWnd, GWL_EXSTYLE);
      }
      if (desc_.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH)
        if (HRESULT hr = ChangeDisplayMode(target_.ptr(), &newDisplayMode); FAILED(hr))
          return hr;
      if (!wsi::updateFullscreenWindow(monitor_, hWnd, &state, false))
        return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
      window_state_ = state;
      applied = true;
    }
    std::lock_guard<mullion::mutex> lock(mutex_);
    if (applied)
      transition_mode_ = newDisplayMode;
    if (pDesc->RefreshRate.Numerator != 0)
      fullscreen_desc_.RefreshRate = pDesc->RefreshRate;
    fullscreen_desc_.ScanlineOrdering = pDesc->ScanlineOrdering;
    fullscreen_desc_.Scaling = pDesc->Scaling;
    return S_OK;
  };

  void
  ApplyLayerProps() {
    settings_.Update(desc_, fullscreen_desc_.Windowed, hWnd, *presenter);
    auto target_color_space = ConvertColorSpace(
        desc_.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 : colorspace_,
        LayerSupportEDR()
    );
    if (presenter->changeLayerProperties(
            desc_.Format, target_color_space, settings_.target_width * scale_factor,
            settings_.target_height * scale_factor, desc_.SampleDesc.Count
        )) {
      // TODO(d3d12): flush command queue
    }
  };

  HRESULT GetOutputFromMonitor(
          HMONITOR                  Monitor,
          IDXGIOutput1**            ppOutput) {
    if (!ppOutput)
      return DXGI_ERROR_INVALID_CALL;

    Com<IDXGIAdapter> adapter;
    Com<IDXGIOutput> output;

    if (FAILED(device_->GetAdapter(IID_PPV_ARGS(&adapter))))
      return E_FAIL;

    for (uint32_t i = 0; SUCCEEDED(adapter->EnumOutputs(i, &output)); i++) {
      DXGI_OUTPUT_DESC outputDesc;
      output->GetDesc(&outputDesc);
      
      if (outputDesc.Monitor == Monitor)
        return output->QueryInterface(IID_PPV_ARGS(ppOutput));
      
      output = nullptr;
    }
    
    return DXGI_ERROR_NOT_FOUND;
  }

  HRESULT
  STDMETHODCALLTYPE
  GetContainingOutput(IDXGIOutput **ppOutput) final {
    InitReturnPtr(ppOutput);
    
    if (!wsi::isWindow(hWnd))
      return DXGI_ERROR_INVALID_CALL;
    
    Com<IDXGIOutput1> output;

    if (target_ == nullptr) {
      HRESULT hr = GetOutputFromMonitor(wsi::getWindowMonitor(hWnd), &output);

      if (FAILED(hr))
        return hr;
    } else {
      output = target_;
    }

    *ppOutput = output.ref();
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  GetFrameStatistics(DXGI_FRAME_STATISTICS *stats) final {
    DEBUG("DXGISwapChain::GetFrameStatistics: stub");
    stats->PresentCount = presentation_count_;
    stats->SyncRefreshCount = presentation_count_;
    stats->PresentRefreshCount = presentation_count_;
    stats->SyncGPUTime = {};
    stats->SyncQPCTime = {};
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  GetLastPresentCount(UINT *last_present_count) final {
    if (last_present_count == NULL) {
      return E_POINTER;
    }
    *last_present_count = presentation_count_;
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  GetDesc1(DXGI_SWAP_CHAIN_DESC1 *pDesc) final {
    if (pDesc == NULL) {
      return E_POINTER;
    }
    *pDesc = desc_;
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pDesc) final {
    if (pDesc == NULL) {
      return E_POINTER;
    }
    *pDesc = fullscreen_desc_;
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  GetHwnd(HWND *pHwnd) final {
    if (pHwnd == NULL) {
      return E_POINTER;
    }
    *pHwnd = hWnd;
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  GetCoreWindow(REFIID refiid, void **ppUnk) final {
    ERR("Not implemented");
    return E_NOTIMPL;
  };

  MULLION_HOTPATCHABLE
  HRESULT
  STDMETHODCALLTYPE
  Present1(UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS *pPresentParameters) final {
    if (FAILED(device_->GetDeviceRemovedReason()))
      return DXGI_ERROR_DEVICE_REMOVED;
    HRESULT hr = S_OK;
    follower_.follow(hWnd, native_view_);
    if (desc_.Width == 0 || desc_.Height == 0)
      hr = DXGI_STATUS_OCCLUDED;
    if (PresentFlags & DXGI_PRESENT_TEST)
      return hr;
    // a window the picture keeps its size in may have another size by now
    if (settings_.Update(desc_, fullscreen_desc_.Windowed, hWnd, *presenter))
      ApplyLayerProps();

    double vsync_duration = std::max(
        SyncInterval * 1.0 / (preferred_max_frame_rate ? preferred_max_frame_rate : init_refresh_rate_),
        preferred_max_frame_rate ? 1.0 / preferred_max_frame_rate : 0
    );

    auto &backbuffer = backbuffers_[current_buffer_];
    // full screen, the output's gamma ramp
    if (target_)
      presenter->changeGammaRamp(static_cast<MTLDXGIOutput *>(target_.ptr())->GetGammaRamp());
    hr = queue_->Present(
        this, this->presenter.ptr(), backbuffer.ptr(), present_semaphore_, vsync_duration, dump_id_, presentation_count_
    );

    presentation_count_ += 1;
    current_buffer_ = (current_buffer_ + 1) % backbuffers_.size();

    return hr;
  };

  BOOL STDMETHODCALLTYPE
  IsTemporaryMonoSupported() final {
    return FALSE;
  };

  HRESULT
  STDMETHODCALLTYPE
  GetRestrictToOutput(IDXGIOutput **ppRestrictToOutput) final {
    if (!ppRestrictToOutput)
      return E_INVALIDARG;
    // a swap chain here is restricted to no output
    *ppRestrictToOutput = nullptr;
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  SetBackgroundColor(const DXGI_RGBA *pColor) final {
    return settings_.SetBackgroundColor(pColor);
  };

  HRESULT
  STDMETHODCALLTYPE
  GetBackgroundColor(DXGI_RGBA *pColor) final {
    if (!pColor)
      return E_INVALIDARG;
    *pColor = settings_.background;
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  SetRotation(DXGI_MODE_ROTATION Rotation) final {
    return settings_.SetRotation(desc_, Rotation);
  };

  HRESULT
  STDMETHODCALLTYPE
  GetRotation(DXGI_MODE_ROTATION *pRotation) final {
    if (!pRotation)
      return E_INVALIDARG;
    *pRotation = settings_.rotation;
    return S_OK;
  };

  HRESULT STDMETHODCALLTYPE
  SetSourceSize(UINT Width, UINT Height) override {
    return settings_.SetSourceSize(desc_, Width, Height);
  };

  HRESULT STDMETHODCALLTYPE
  GetSourceSize(UINT *pWidth, UINT *pHeight) override {
    return settings_.GetSourceSize(desc_, pWidth, pHeight);
  };

  HRESULT STDMETHODCALLTYPE
  SetMaximumFrameLatency(UINT max_latency) override {
    if (!(desc_.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))
      return DXGI_ERROR_INVALID_CALL;
    if (max_latency == 0 || max_latency > DXGI_MAX_SWAP_CHAIN_BUFFERS) {
      return DXGI_ERROR_INVALID_CALL;
    }
    if (max_latency > frame_latency) {
      ReleaseSemaphore(present_semaphore_, max_latency - frame_latency, nullptr);
    }
    frame_latency = max_latency;

    return S_OK;
  };

  HRESULT STDMETHODCALLTYPE
  GetMaximumFrameLatency(UINT *max_latency) override {
    if (!(desc_.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))
      return DXGI_ERROR_INVALID_CALL;
    if (max_latency) {
      *max_latency = frame_latency;
    }
    return S_OK;
  };

  HANDLE STDMETHODCALLTYPE
  GetFrameLatencyWaitableObject() override {
    if (!(desc_.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)) {
      return nullptr;
    }

    HANDLE result = nullptr;
    HANDLE processHandle = GetCurrentProcess();

    if (!DuplicateHandle(processHandle, present_semaphore_, processHandle, &result, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
      return nullptr;
    }

    return result;
  };

  HRESULT STDMETHODCALLTYPE
  SetMatrixTransform(const DXGI_MATRIX_3X2_F *matrix) override {
    return DXGI_ERROR_INVALID_CALL;
  };

  HRESULT STDMETHODCALLTYPE
  GetMatrixTransform(DXGI_MATRIX_3X2_F *matrix) override {
    return DXGI_ERROR_INVALID_CALL;
  };

  HRESULT STDMETHODCALLTYPE
  CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace, UINT *pColorSpaceSupport) override {
    if (!pColorSpaceSupport)
      return E_INVALIDARG;
    *pColorSpaceSupport = CGColorSpace_checkColorSpaceSupported(ConvertColorSpace(ColorSpace, false))
                              ? DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT
                              : 0;
    return S_OK;
  };

  UINT STDMETHODCALLTYPE
  GetCurrentBackBufferIndex() override {
    return current_buffer_;
  }

  MULLION_HOTPATCHABLE
  HRESULT STDMETHODCALLTYPE
  ResizeBuffers1(
      UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format, UINT SwapChainFlags, const UINT *pCreationNodeMask,
      IUnknown *const *ppPresentQueue
  ) override {
    WARN("DXGISwapChain3::ResizeBuffers1: ignoring d3d12 related parameters");
    return ResizeBuffers(BufferCount, Width, Height, Format, SwapChainFlags);
  }

  HRESULT STDMETHODCALLTYPE
  SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace) override {
    UINT support = 0;
    CheckColorSpaceSupport(ColorSpace, &support);
    if (!(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
      return E_INVALIDARG;
    auto target_color_space = ConvertColorSpace(ColorSpace, LayerSupportEDR());
    if (presenter->changeLayerColorSpace(target_color_space)) {
      // TODO(d3d12): flush command queue
    }
    colorspace_ = ColorSpace;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void *pMetaData) override {
    // the metadata is for a monitor to adjust its output by and "may be ignored" (IDXGISwapChain4::SetHDRMetaData):
    // it does not change the pixels presented
    return S_OK;
  }
};

HRESULT
CreateSwapChain(
    IDXGIFactory1 *pFactory, MTLD3D12Device *pDevice, MTLD3D12CommandQueue *pQueue, HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC1 *pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc,
    IDXGISwapChain1 **ppSwapChain
) {
  // Direct3D 12 Swap Chains, Swap effects: only flip sequential and flip discard are supported
  if (pDesc->SwapEffect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL && pDesc->SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD)
    return DXGI_ERROR_INVALID_CALL;
  auto swapchain = Com<MTLD3D12SwapChain>::transfer(new MTLD3D12SwapChain(pFactory, pDevice, pQueue, hWnd, pDesc, pFullscreenDesc));
  HRESULT hr = swapchain->ResizeBuffers(0, pDesc->Width, pDesc->Height, DXGI_FORMAT_UNKNOWN, pDesc->Flags);
  if (FAILED(hr))
    return hr;
  if (pFullscreenDesc && !pFullscreenDesc->Windowed) {
    hr = swapchain->SetFullscreenState(TRUE, nullptr);
    if (FAILED(hr)) {
      HRESULT restore_hr = swapchain->SetFullscreenState(FALSE, nullptr);
      if (FAILED(restore_hr))
        return restore_hr;
      if (hr != DXGI_ERROR_NOT_CURRENTLY_AVAILABLE)
        return hr;
      // fullscreen unavailable at creation returns a windowed swap chain (IDXGIFactory::CreateSwapChain, Remarks).
      hr = DXGI_STATUS_OCCLUDED;
    }
  }
  *ppSwapChain = swapchain.takeOwnership();
  return hr;
}

}; // namespace mullion
