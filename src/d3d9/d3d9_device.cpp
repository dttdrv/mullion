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

#include "d3d9_device.hpp"
#include "d3d9_window.hpp"
#include "d3d9_query.hpp"
#include "d3d9_buffer.hpp"
#include "d3d9_texture.hpp"
#include "d3d9_format.hpp"
#include "d3d9_state.hpp"
#include "log/log.hpp"
#include "util_string.hpp"
#include <algorithm>
#include <cstring>

namespace mullion {

MTLD3D9Device::MTLD3D9Device(
    MTLD3D9 *pParent, MTLD3D9Adapter *pAdapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
    bool Extended
) :
    parent_(pParent),
    adapter_(pAdapter),
    extended_(Extended),
    multithreaded_(BehaviorFlags & D3DCREATE_MULTITHREADED) {
  creation_params_.AdapterOrdinal = pAdapter->GetOrdinal();
  creation_params_.DeviceType = DeviceType;
  creation_params_.hFocusWindow = hFocusWindow;
  creation_params_.BehaviorFlags = BehaviorFlags;

  mullion_device_ = CreateMullionDevice({.device = pAdapter->GetMTLDevice()});
}

MTLD3D9Device::~MTLD3D9Device() {
  ReleaseFocusWindow();
  if (cursor_)
    DestroyCursor(cursor_);
  ClearBindings();
  render_targets_ = {};
  depth_stencil_ = nullptr;
  auto_depth_stencil_ = nullptr;
  swapchain_ = nullptr;
  WaitUntilGPUIdle();
}

HRESULT
MTLD3D9Device::Initialize(D3DPRESENT_PARAMETERS *pParams, D3DDISPLAYMODEEX *pFullscreenMode) {
  if (!pParams)
    return D3DERR_INVALIDCALL;

  HWND app_window = pParams->hDeviceWindow;
  if (!pParams->hDeviceWindow)
    pParams->hDeviceWindow = creation_params_.hFocusWindow;
  if (!pParams->hDeviceWindow && !pParams->Windowed) {
    ERR("D3D9: fullscreen CreateDevice without a device or focus window");
    return D3DERR_INVALIDCALL;
  }

  // like wined3d, before the swap chain goes fullscreen
  if (!pParams->Windowed)
    AcquireFocusWindow(creation_params_.hFocusWindow ? creation_params_.hFocusWindow : pParams->hDeviceWindow);
  swapchain_ = new MTLD3D9SwapChain(this, *pParams);
  HRESULT hr = swapchain_->Reset(pParams);
  if (FAILED(hr))
    return hr;

  hr = ResetState(pParams, true);
  // the parameters come back filled in, but a NULL device window stays NULL (Wine's test_swapchain_parameters)
  pParams->hDeviceWindow = app_window;
  return hr;
}

void
MTLD3D9Device::AcquireFocusWindow(HWND Window) {
  if (focus_window_ || !Window)
    return;
  if (!D3D9HookFocusWindow(Window, this))
    return;
  focus_window_ = Window;
  SetWindowPos(Window, nullptr, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE);
  BOOL screensaver = FALSE;
  SystemParametersInfoW(SPI_GETSCREENSAVEACTIVE, 0, &screensaver, 0);
  if ((restore_screensaver_ = screensaver))
    SystemParametersInfoW(SPI_SETSCREENSAVEACTIVE, FALSE, nullptr, 0);
}

void
MTLD3D9Device::ReleaseFocusWindow() {
  if (!focus_window_)
    return;
  D3D9UnhookFocusWindow(focus_window_, this);
  focus_window_ = nullptr;
  if (restore_screensaver_)
    SystemParametersInfoW(SPI_SETSCREENSAVEACTIVE, TRUE, nullptr, 0);
  restore_screensaver_ = false;
}

// runs on the window's thread, possibly inside a Reset's mode change, so it doesn't take the device lock.
void
MTLD3D9Device::ProcessFocusMessage(HWND Window, UINT Message, WPARAM wParam, LPARAM lParam, bool Unicode) {
  if (Message == WM_ACTIVATEAPP) {
    if (swapchain_)
      swapchain_->Activate(wParam);
  } else if (Message == WM_WINDOWPOSCHANGING) {
    if (swapchain_)
      swapchain_->KeepOnMonitor(Window, reinterpret_cast<WINDOWPOS *>(lParam));
  } else if (Message == WM_SYSCOMMAND && wParam == SC_RESTORE) {
    if (Unicode)
      DefWindowProcW(Window, Message, wParam, lParam);
    else
      DefWindowProcA(Window, Message, wParam, lParam);
  }
}

void
MTLD3D9Device::SetActive(bool Active) {
  DeviceState lost = DeviceState::Lost, ok = DeviceState::Ok;
  if (!Active)
    state_.compare_exchange_strong(ok, DeviceState::Lost);
  else
    state_.compare_exchange_strong(lost, extended_ ? DeviceState::Ok : DeviceState::NotReset);
}

HRESULT
MTLD3D9Device::CreateAutoDepthStencil(const D3DPRESENT_PARAMETERS &Params) {
  auto_depth_stencil_ = nullptr;
  if (!Params.EnableAutoDepthStencil)
    return D3D_OK;

  D3DSURFACE_DESC desc = {};
  desc.Format = Params.AutoDepthStencilFormat;
  desc.Type = D3DRTYPE_SURFACE;
  desc.Usage = D3DUSAGE_DEPTHSTENCIL;
  desc.Pool = D3DPOOL_DEFAULT;
  desc.MultiSampleType = Params.MultiSampleType;
  desc.MultiSampleQuality = Params.MultiSampleQuality;
  desc.Width = Params.BackBufferWidth;
  desc.Height = Params.BackBufferHeight;

  D3D9_FORMAT_DESC format;
  if (!D3D9QueryFormat(desc.Format, format) || !(format.Flags & D3D9_FORMAT_DEPTH))
    return D3DERR_INVALIDCALL;

  Rc<Texture> texture;
  HRESULT hr = CreateD3D9SurfaceTexture(this, desc, texture);
  if (FAILED(hr))
    return hr;
  auto_depth_stencil_ = new MTLD3D9Surface(this, desc, std::move(texture));
  return D3D_OK;
}

void
MTLD3D9Device::ClearBindings() {
  for (auto &binding : textures_)
    ReleaseTextureBinding(binding);
  vertex_shader_ = nullptr;
  pixel_shader_ = nullptr;
  vertex_declaration_ = nullptr;
  fvf_ = 0;
  for (auto &stream : streams_)
    stream = {};
  index_buffer_ = nullptr;
  for (auto &c : const_float_)
    c.fill(0.0f);
  for (auto &c : const_int_)
    c.fill(0);
  for (auto &c : const_bool_)
    c.fill(0);
  const_dirty_[0] = const_dirty_[1] = 15;
  clip_planes_ = {};
}

HRESULT
MTLD3D9Device::ResetState(D3DPRESENT_PARAMETERS *pParams, bool Initial) {
  HRESULT hr = CreateAutoDepthStencil(*pParams);
  if (FAILED(hr))
    return hr;

  render_targets_ = {};
  render_targets_[0] = swapchain_->BackBuffer(0);
  depth_stencil_ = auto_depth_stencil_;
  float min_z = viewport_.MinZ, max_z = viewport_.MaxZ;
  SetDefaultViewport(render_targets_[0].ptr());
  if (extended_ && !Initial) {
    viewport_.MinZ = min_z;
    viewport_.MaxZ = max_z;
  }
  recording_ = nullptr;
  // a D3D9Ex Reset keeps an open scene (Wine's d3d9ex test_scene)
  if (!extended_ || Initial)
    in_scene_ = false;
  state_ = DeviceState::Ok;
  // D3D9Ex Reset keeps the rest of the device state, bound resources included (Wine's d3d9ex test_reset)
  if (extended_ && !Initial)
    return D3D_OK;
  D3D9InitRenderStates(render_states_, pParams->EnableAutoDepthStencil);
  D3D9InitTextureStageStates(texture_stage_states_);
  for (auto &m : transforms_)
    m = {{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}};
  material_ = {};
  lights_.clear();
  for (auto &ss : sampler_states_) {
    ss.fill(0);
    ss[D3DSAMP_ADDRESSU] = D3DTADDRESS_WRAP;
    ss[D3DSAMP_ADDRESSV] = D3DTADDRESS_WRAP;
    ss[D3DSAMP_ADDRESSW] = D3DTADDRESS_WRAP;
    ss[D3DSAMP_MAGFILTER] = D3DTEXF_POINT;
    ss[D3DSAMP_MINFILTER] = D3DTEXF_POINT;
    ss[D3DSAMP_MIPFILTER] = D3DTEXF_NONE;
    ss[D3DSAMP_MAXANISOTROPY] = 1;
  }
  ClearBindings();
  return D3D_OK;
}

void
MTLD3D9Device::SetDefaultViewport(MTLD3D9Surface *pTarget) {
  if (!pTarget)
    return;
  auto &desc = pTarget->Desc();
  viewport_ = {0, 0, desc.Width, desc.Height, 0.0f, 1.0f};
  scissor_rect_ = {0, 0, (LONG)desc.Width, (LONG)desc.Height};
}

void
MTLD3D9Device::FlushCommands() {
  EndRenderPass();
  queue().CommitCurrentChunk();
}

void
MTLD3D9Device::WaitUntilGPUIdle() {
  uint64_t seq = queue().CurrentSeqId();
  FlushCommands();
  queue().WaitCPUFence(seq);
}

HRESULT
MTLD3D9Device::MapStaging(Rc<StagingResource> &Staging, bool Read, bool Write, bool DoNotWait, void **ppData) {
  bool flushed = false;
  for (;;) {
    uint64_t coherent = queue().CoherentSeqId();
    auto result = Staging->tryMap(coherent, Read, Write);
    if (result == StagingMapResult::Mapped)
      return D3DERR_INVALIDCALL;
    if (result == StagingMapResult::Renamable) {
      auto next = Staging->allocate(coherent);
      std::memcpy(Staging->mappedMemory(next), Staging->mappedImmediateMemory(), Staging->length);
      Staging->updateImmediateName(queue().CurrentSeqId(), next);
      Emit([Staging, next](ArgumentEncodingContext &enc) mutable {
        auto _ = Staging->buffer()->rename(Staging->allocation(next));
      });
      result = StagingMapResult::Mappable;
    }
    if (result == StagingMapResult::Mappable) {
      *ppData = Staging->mappedImmediateMemory();
      return D3D_OK;
    }
    if (DoNotWait)
      return D3DERR_WASSTILLDRAWING;
    if (!flushed) {
      FlushCommands();
      flushed = true;
    }
    queue().WaitCPUFence(coherent + uint64_t(result));
  }
}

static uint64_t
BoxOffset(const D3DBOX &Box, const D3D9_FORMAT_LAYOUT &Layout, uint32_t BytesPerRow) {
  return (uint64_t)(Box.Top / Layout.BlockSize) * BytesPerRow + (Box.Left / Layout.BlockSize) * Layout.BlockBytes;
}

void
MTLD3D9Device::CopyTextureToBuffer(
    const Rc<Texture> &Src, uint32_t Level, uint32_t Slice, const D3DBOX &Box, const D3D9Shadow &Dst,
    const D3D9_FORMAT_LAYOUT &Layout
) {
  if (Layout.CpuOnly)
    return;
  Dst.resource->useCopyDestination(queue().CurrentSeqId());
  uint64_t offset = Dst.offset + BoxOffset(Box, Layout, Dst.pitch);
  EmitBlit([=, staging = Dst.resource](ArgumentEncodingContext &enc) {
    auto src = enc.access(Src, Level, Slice, ResourceAccess::Read);
    auto [dst, dst_offset] = enc.access(staging->buffer(), 0, staging->length, ResourceAccess::Write);
    auto &cmd = enc.encodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer>();
    cmd.type = WMTBlitCommandCopyFromTextureToBuffer;
    cmd.src = src;
    cmd.slice = Slice;
    cmd.level = Level;
    cmd.origin = {Box.Left, Box.Top, Box.Front};
    cmd.size = {Box.Right - Box.Left, Box.Bottom - Box.Top, Box.Back - Box.Front};
    cmd.dst = dst->buffer();
    cmd.offset = dst_offset + offset;
    cmd.bytes_per_row = Dst.pitch;
    cmd.bytes_per_image = Dst.image;
  });
}

void
MTLD3D9Device::CopyBufferToTexture(
    const D3D9Shadow &Src, const D3D9_FORMAT_LAYOUT &Layout, const Rc<Texture> &Dst, uint32_t Level,
    uint32_t Slice, const D3DBOX &Box, uint32_t DstX, uint32_t DstY
) {
  if (Layout.CpuOnly)
    return;
  Src.resource->useCopySource(queue().CurrentSeqId());
  uint64_t offset = Src.offset + BoxOffset(Box, Layout, Src.pitch);
  uint32_t x = DstX == ~0u ? Box.Left : DstX;
  uint32_t y = DstY == ~0u ? Box.Top : DstY;
  EmitBlit([=, staging = Src.resource](ArgumentEncodingContext &enc) {
    auto [src, src_offset] = enc.access(staging->buffer(), 0, staging->length, ResourceAccess::Read);
    auto dst = enc.access(Dst, Level, Slice, ResourceAccess::Write);
    auto &cmd = enc.encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture>();
    cmd.type = WMTBlitCommandCopyFromBufferToTexture;
    cmd.src = src->buffer();
    cmd.src_offset = src_offset + offset;
    cmd.bytes_per_row = Src.pitch;
    cmd.bytes_per_image = Src.image;
    cmd.size = {Box.Right - Box.Left, Box.Bottom - Box.Top, Box.Back - Box.Front};
    cmd.dst = dst;
    cmd.slice = Slice;
    cmd.level = Level;
    cmd.origin = {x, y, Box.Front};
  });
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::QueryInterface(REFIID riid, void **ppvObject) {
  if (!ppvObject)
    return E_POINTER;
  *ppvObject = nullptr;

  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DDevice9) ||
      (extended_ && riid == __uuidof(IDirect3DDevice9Ex))) {
    *ppvObject = ref(this);
    return S_OK;
  }

  WARN("IDirect3DDevice9::QueryInterface: Unknown interface query ", str::format(riid));
  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::TestCooperativeLevel() {
  if (extended_)
    return D3D_OK;
  switch (state_.load()) {
  case DeviceState::Lost:
    return D3DERR_DEVICELOST;
  case DeviceState::NotReset:
    return D3DERR_DEVICENOTRESET;
  default:
    return D3D_OK;
  }
}

UINT STDMETHODCALLTYPE
MTLD3D9Device::GetAvailableTextureMem() {
  // what the GPU's working set has left, so creating resources lowers it, except on D3D9Ex where video memory is
  // virtualized (Wine's test_vidmem_accounting in both suites)
  uint64_t total = std::min<uint64_t>(GetMTLDevice().recommendedMaxWorkingSetSize(), 0xFFE00000ull);
  uint64_t used = extended_ ? 0 : GetMTLDevice().currentAllocatedSize();
  uint64_t bytes = total > used ? total - used : 0;
  return (UINT)(bytes & ~0xFFFFFull);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::EvictManagedResources() {
  auto lock = Lock();
  managed_epoch_++;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetDirect3D(IDirect3D9 **ppD3D9) {
  if (!ppD3D9)
    return D3DERR_INVALIDCALL;
  *ppD3D9 = parent_.ref();
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetDeviceCaps(D3DCAPS9 *pCaps) {
  return adapter_->GetDeviceCaps(D3DDEVTYPE_HAL, pCaps);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE *pMode) {
  if (iSwapChain != 0)
    return D3DERR_INVALIDCALL;
  return swapchain_->GetDisplayMode(pMode);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParameters) {
  if (!pParameters)
    return D3DERR_INVALIDCALL;
  *pParameters = creation_params_;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9 *pCursorBitmap) {
  auto lock = Lock();
  if (!pCursorBitmap)
    return D3DERR_INVALIDCALL;
  D3DSURFACE_DESC desc;
  pCursorBitmap->GetDesc(&desc);
  bool pow2 = desc.Width && !(desc.Width & (desc.Width - 1)) && desc.Height && !(desc.Height & (desc.Height - 1));
  if (desc.Format != D3DFMT_A8R8G8B8 || !pow2 || XHotSpot >= desc.Width || YHotSpot >= desc.Height)
    return D3DERR_INVALIDCALL;
  D3DDISPLAYMODE mode;
  if (SUCCEEDED(swapchain_->GetDisplayMode(&mode)) && (desc.Width > mode.Width || desc.Height > mode.Height))
    return D3DERR_INVALIDCALL;
  D3DLOCKED_RECT lr;
  if (FAILED(pCursorBitmap->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
    return D3DERR_INVALIDCALL;
  std::vector<DWORD> pixels(desc.Width * desc.Height);
  for (UINT y = 0; y < desc.Height; y++)
    std::memcpy(&pixels[y * desc.Width], (char *)lr.pBits + y * lr.Pitch, desc.Width * 4);
  pCursorBitmap->UnlockRect();

  std::vector<BYTE> mask((desc.Width + 15) / 16 * 2 * desc.Height, 0);
  ICONINFO info = {};
  info.fIcon = FALSE;
  info.xHotspot = XHotSpot;
  info.yHotspot = YHotSpot;
  info.hbmMask = CreateBitmap(desc.Width, desc.Height, 1, 1, mask.data());
  info.hbmColor = CreateBitmap(desc.Width, desc.Height, 1, 32, pixels.data());
  HCURSOR cursor = CreateIconIndirect(&info);
  DeleteObject(info.hbmMask);
  DeleteObject(info.hbmColor);
  if (!cursor)
    return D3DERR_INVALIDCALL;
  if (cursor_)
    DestroyCursor(cursor_);
  cursor_ = cursor;
  if (cursor_visible_)
    SetCursor(cursor_);
  return D3D_OK;
}

void STDMETHODCALLTYPE
MTLD3D9Device::SetCursorPosition(int X, int Y, DWORD Flags) {
  POINT pt;
  // moving the system cursor to where it already is would still fight the user's input
  if (GetCursorPos(&pt) && (pt.x != X || pt.y != Y))
    SetCursorPos(X, Y);
}

BOOL STDMETHODCALLTYPE
MTLD3D9Device::ShowCursor(BOOL bShow) {
  auto lock = Lock();
  BOOL previous = cursor_visible_;
  cursor_visible_ = bShow && cursor_;
  if (cursor_)
    SetCursor(bShow ? cursor_ : nullptr);
  return previous;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9 **ppSwapChain) {
  InitReturnPtr(ppSwapChain);
  if (!ppSwapChain || iSwapChain != 0)
    return D3DERR_INVALIDCALL;
  *ppSwapChain = ref(swapchain_.ptr());
  return D3D_OK;
}

UINT STDMETHODCALLTYPE
MTLD3D9Device::GetNumberOfSwapChains() {
  return 1;
}

void
D3D9CountDefaultResource(IDirect3DDevice9Ex *pDevice, int Delta) {
  static_cast<MTLD3D9Device *>(pDevice)->default_resources_ += Delta;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::Reset(D3DPRESENT_PARAMETERS *pPresentationParameters) {
  auto lock = Lock();
  if (!pPresentationParameters)
    return D3DERR_INVALIDCALL;
  if (!extended_ && state_ == DeviceState::Lost)
    return D3DERR_DEVICELOST;

  if (!extended_ && default_resources_) {
    state_ = DeviceState::NotReset;
    return D3DERR_INVALIDCALL;
  }

  WaitUntilGPUIdle();
  render_targets_ = {};
  depth_stencil_ = nullptr;
  auto_depth_stencil_ = nullptr;

  HWND app_window = pPresentationParameters->hDeviceWindow;
  filter_messages_ = true;
  HRESULT hr = swapchain_->Reset(pPresentationParameters);
  filter_messages_ = false;
  if (SUCCEEDED(hr))
    hr = ResetState(pPresentationParameters, false);
  if (FAILED(hr)) {
    state_ = DeviceState::NotReset;
    return D3DERR_INVALIDCALL;
  }
  if (pPresentationParameters->Windowed)
    ReleaseFocusWindow();
  else
    AcquireFocusWindow(creation_params_.hFocusWindow ? creation_params_.hFocusWindow : pPresentationParameters->hDeviceWindow);
  pPresentationParameters->hDeviceWindow = app_window;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::Present(const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion) {
  return PresentEx(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, 0);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetBackBuffer(
    UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9 **ppBackBuffer
) {
  InitReturnPtr(ppBackBuffer);
  if (iSwapChain != 0)
    return D3DERR_INVALIDCALL;
  return swapchain_->GetBackBuffer(iBackBuffer, Type, ppBackBuffer);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS *pRasterStatus) {
  if (iSwapChain != 0)
    return D3DERR_INVALIDCALL;
  return swapchain_->GetRasterStatus(pRasterStatus);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetDialogBoxMode(BOOL bEnableDialogs) {
  return D3D_OK;
}

void STDMETHODCALLTYPE
MTLD3D9Device::SetGammaRamp(UINT iSwapChain, DWORD Flags, const D3DGAMMARAMP *pRamp) {
  if (iSwapChain == 0)
    swapchain_->SetGammaRamp(Flags, pRamp);
}

void STDMETHODCALLTYPE
MTLD3D9Device::GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP *pRamp) {
  if (iSwapChain == 0)
    swapchain_->GetGammaRamp(pRamp);
}

static HRESULT
CreateStandaloneSurface(
    MTLD3D9Device *pDevice, const D3DSURFACE_DESC &Desc, bool Lockable, IDirect3DSurface9 **ppSurface
) {
  InitReturnPtr(ppSurface);
  if (!ppSurface || !Desc.Width || !Desc.Height || !D3D9SizeInWholeBlocks(Desc.Format, Desc.Width, Desc.Height))
    return D3DERR_INVALIDCALL;

  Rc<Texture> texture;
  D3D9_FORMAT_DESC format;
  bool null_target = D3D9QueryFormat(Desc.Format, format) && (format.Flags & D3D9_FORMAT_NULL_RT);
  bool cpu_only = format.Flags & D3D9_FORMAT_CPU_ONLY;
  if (cpu_only && (Desc.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)))
    return D3DERR_INVALIDCALL;
  if (Desc.Pool != D3DPOOL_SYSTEMMEM && Desc.Pool != D3DPOOL_SCRATCH && !null_target && !cpu_only) {
    HRESULT hr = CreateD3D9SurfaceTexture(pDevice, Desc, texture);
    if (FAILED(hr))
      return hr;
  }
  *ppSurface = ref(new MTLD3D9Surface(pDevice, Desc, std::move(texture), nullptr, Lockable));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateTexture(
    UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9 **ppTexture,
    HANDLE *pSharedHandle
) {
  auto lock = Lock();
  // shared resources are a D3D9Ex feature; Windows XP answers E_NOTIMPL (Wine's test_shared_handle)
  if (pSharedHandle && !extended_)
    return E_NOTIMPL;
  InitReturnPtr(ppTexture);
  // D3D9Ex: a single-level system memory texture on app memory
  bool user_memory = pSharedHandle && extended_ && Pool == D3DPOOL_SYSTEMMEM && Levels == 1;
  if (pSharedHandle && !user_memory) {
    if (Pool != D3DPOOL_SYSTEMMEM && Pool != D3DPOOL_SCRATCH)
      ERR("IDirect3DDevice9::CreateTexture: shared textures are not implemented");
    return D3DERR_INVALIDCALL;
  }
  D3D9_TEXTURE_DESC desc = {D3DRTYPE_TEXTURE, Width, Height, 1, Levels, Usage, Format, Pool};
  HRESULT hr = CreateD3D9Texture(this, desc, reinterpret_cast<IDirect3DBaseTexture9 **>(ppTexture));
  if (SUCCEEDED(hr) && user_memory) {
    IDirect3DSurface9 *level = nullptr;
    (*ppTexture)->GetSurfaceLevel(0, &level);
    static_cast<MTLD3D9Surface *>(level)->UseUserMemory(*pSharedHandle);
    level->Release();
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateCubeTexture(
    UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9 **ppCubeTexture,
    HANDLE *pSharedHandle
) {
  auto lock = Lock();
  if (pSharedHandle && !extended_)
    return E_NOTIMPL;
  InitReturnPtr(ppCubeTexture);
  if (pSharedHandle)
    return D3DERR_INVALIDCALL;
  D3D9_TEXTURE_DESC desc = {D3DRTYPE_CUBETEXTURE, EdgeLength, EdgeLength, 1, Levels, Usage, Format, Pool};
  return CreateD3D9Texture(this, desc, reinterpret_cast<IDirect3DBaseTexture9 **>(ppCubeTexture));
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateVolumeTexture(
    UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DVolumeTexture9 **ppVolumeTexture, HANDLE *pSharedHandle
) {
  auto lock = Lock();
  if (pSharedHandle && !extended_)
    return E_NOTIMPL;
  InitReturnPtr(ppVolumeTexture);
  if (pSharedHandle)
    return D3DERR_INVALIDCALL;
  D3D9_TEXTURE_DESC desc = {D3DRTYPE_VOLUMETEXTURE, Width, Height, Depth, Levels, Usage, Format, Pool};
  return CreateD3D9Texture(this, desc, reinterpret_cast<IDirect3DBaseTexture9 **>(ppVolumeTexture));
}

// buffers can't be scratch, render targets, depth stencils or autogen mipmaps (Wine's test_resource_access)
static bool
ValidBufferPoolAndUsage(D3DPOOL Pool, DWORD Usage) {
  return Pool != D3DPOOL_SCRATCH &&
         !(Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_AUTOGENMIPMAP | D3DUSAGE_DMAP));
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateVertexBuffer(
    UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9 **ppVertexBuffer, HANDLE *pSharedHandle
) {
  auto lock = Lock();
  if (pSharedHandle && !extended_)
    return E_NOTIMPL;
  InitReturnPtr(ppVertexBuffer);
  if (pSharedHandle && extended_ && Pool == D3DPOOL_SYSTEMMEM)
    return D3DERR_NOTAVAILABLE;
  // D3D9Ex has no managed pool
  if (extended_ && Pool == D3DPOOL_MANAGED)
    return D3DERR_INVALIDCALL;
  if (!ppVertexBuffer || !Length || pSharedHandle || !ValidBufferPoolAndUsage(Pool, Usage))
    return D3DERR_INVALIDCALL;
  D3DVERTEXBUFFER_DESC desc = {D3DFMT_VERTEXDATA, D3DRTYPE_VERTEXBUFFER, Usage, Pool, Length, FVF};
  *ppVertexBuffer = ref(new MTLD3D9VertexBuffer(this, desc));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateIndexBuffer(
    UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9 **ppIndexBuffer,
    HANDLE *pSharedHandle
) {
  auto lock = Lock();
  if (pSharedHandle && !extended_)
    return E_NOTIMPL;
  InitReturnPtr(ppIndexBuffer);
  if (pSharedHandle && extended_ && Pool == D3DPOOL_SYSTEMMEM)
    return D3DERR_NOTAVAILABLE;
  if (extended_ && Pool == D3DPOOL_MANAGED)
    return D3DERR_INVALIDCALL;
  if (!ppIndexBuffer || !Length || pSharedHandle || !ValidBufferPoolAndUsage(Pool, Usage))
    return D3DERR_INVALIDCALL;
  if (Format != D3DFMT_INDEX16 && Format != D3DFMT_INDEX32)
    return D3DERR_INVALIDCALL;
  D3DINDEXBUFFER_DESC desc = {Format, D3DRTYPE_INDEXBUFFER, Usage, Pool, Length};
  *ppIndexBuffer = ref(new MTLD3D9IndexBuffer(this, desc));
  return D3D_OK;
}

template <typename T>
static T *
AsTextureBase(IDirect3DBaseTexture9 *pTexture) {
  return static_cast<T *>(pTexture);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::UpdateTexture(IDirect3DBaseTexture9 *pSourceTexture, IDirect3DBaseTexture9 *pDestinationTexture) {
  auto lock = Lock();
  if (!pSourceTexture || !pDestinationTexture)
    return D3DERR_INVALIDCALL;
  D3DRESOURCETYPE type = pSourceTexture->GetType();
  if (type != pDestinationTexture->GetType())
    return D3DERR_INVALIDCALL;

  auto copy = [&](auto *src, auto *dst) -> HRESULT {
    auto &sd = src->Desc();
    auto &dd = dst->Desc();
    if (sd.Pool != D3DPOOL_SYSTEMMEM || dd.Pool != D3DPOOL_DEFAULT || sd.Format != dd.Format || !dst->texture())
      return D3DERR_INVALIDCALL;
    uint32_t offset = 0;
    while (offset < sd.Levels && std::max(1u, sd.Width >> offset) != dd.Width)
      offset++;
    if (offset == sd.Levels || sd.Levels - offset < (dd.Usage & D3DUSAGE_AUTOGENMIPMAP ? 1 : dd.Levels))
      return D3DERR_INVALIDCALL;

    // as in native d3d9, a source without dirty regions isn't copied again
    if (!src->Dirty())
      return D3D_OK;
    uint32_t levels = dd.Usage & D3DUSAGE_AUTOGENMIPMAP ? 1 : dd.Levels;
    for (uint32_t level = 0; level < levels; level++) {
      D3DBOX box = src->DirtyRegion(level + offset);
      if (type == D3DRTYPE_VOLUMETEXTURE) {
        auto sv = src->volume(level + offset);
        uint32_t mask = sv->layout().BlockSize - 1;
        box.Left &= ~mask;
        box.Top &= ~mask;
        CopyBufferToTexture(sv->Shadow(), sv->layout(), dst->texture(), level, 0, box);
        continue;
      }
      for (uint32_t face = 0; face < dst->faces(); face++) {
        auto ss = src->surface(face, level + offset);
        uint32_t mask = ss->layout().BlockSize - 1;
        D3DBOX aligned = box;
        aligned.Left &= ~mask;
        aligned.Top &= ~mask;
        aligned.Right = std::min((aligned.Right + mask) & ~mask, ss->Desc().Width);
        aligned.Bottom = std::min((aligned.Bottom + mask) & ~mask, ss->Desc().Height);
        CopyBufferToTexture(ss->Shadow(), ss->layout(), dst->texture(), level, face, aligned);
      }
    }
    src->CleanDirtyRegion();
    dst->MarkMipsDirty();
    return D3D_OK;
  };

  switch (type) {
  case D3DRTYPE_TEXTURE:
    return copy(AsTextureBase<MTLD3D9Texture>(pSourceTexture), AsTextureBase<MTLD3D9Texture>(pDestinationTexture));
  case D3DRTYPE_CUBETEXTURE:
    return copy(
        AsTextureBase<MTLD3D9CubeTexture>(pSourceTexture), AsTextureBase<MTLD3D9CubeTexture>(pDestinationTexture)
    );
  case D3DRTYPE_VOLUMETEXTURE:
    return copy(
        AsTextureBase<MTLD3D9VolumeTexture>(pSourceTexture), AsTextureBase<MTLD3D9VolumeTexture>(pDestinationTexture)
    );
  default:
    return D3DERR_INVALIDCALL;
  }
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateRenderTarget(
    UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality,
    BOOL Lockable, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle
) {
  return CreateRenderTargetEx(
      Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle, 0
  );
}

constexpr DWORD kExSurfaceUsages =
    D3DUSAGE_RESTRICTED_CONTENT | D3DUSAGE_RESTRICT_SHARED_RESOURCE | D3DUSAGE_RESTRICT_SHARED_RESOURCE_DRIVER;

bool
MTLD3D9Device::ValidMultisampleQuality(D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD Quality) {
  if (MultiSample == D3DMULTISAMPLE_NONE)
    return true;
  DWORD levels = 0;
  return SUCCEEDED(adapter_->CheckDeviceMultiSampleType(D3DDEVTYPE_HAL, Format, TRUE, MultiSample, &levels)) &&
         Quality < levels;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateRenderTargetEx(
    UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality,
    BOOL Lockable, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle, DWORD Usage
) {
  auto lock = Lock();
  if (pSharedHandle && !extended_)
    return E_NOTIMPL;
  InitReturnPtr(ppSurface);
  // only the D3D9Ex restriction flags are usages here (Wine's d3d9ex test_resource_access)
  if (Usage & ~kExSurfaceUsages)
    return D3DERR_INVALIDCALL;
  if (pSharedHandle) {
    ERR("IDirect3DDevice9::CreateRenderTarget: shared surfaces are not implemented");
    return D3DERR_INVALIDCALL;
  }

  D3D9_FORMAT_DESC format;
  if (!D3D9QueryFormat(Format, format) || (format.Flags & (D3D9_FORMAT_DEPTH | D3D9_FORMAT_BC)))
    return D3DERR_INVALIDCALL;
  if (!ValidMultisampleQuality(Format, MultiSample, MultisampleQuality))
    return D3DERR_INVALIDCALL;

  D3DSURFACE_DESC desc = {};
  desc.Format = Format;
  desc.Type = D3DRTYPE_SURFACE;
  desc.Usage = D3DUSAGE_RENDERTARGET | (Usage & D3DUSAGE_RESTRICTED_CONTENT);
  desc.Pool = D3DPOOL_DEFAULT;
  desc.MultiSampleType = MultiSample;
  desc.MultiSampleQuality = MultisampleQuality;
  desc.Width = Width;
  desc.Height = Height;
  return CreateStandaloneSurface(this, desc, Lockable, ppSurface);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateDepthStencilSurface(
    UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality,
    BOOL Discard, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle
) {
  return CreateDepthStencilSurfaceEx(
      Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle, 0
  );
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateDepthStencilSurfaceEx(
    UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality,
    BOOL Discard, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle, DWORD Usage
) {
  auto lock = Lock();
  if (pSharedHandle && !extended_)
    return E_NOTIMPL;
  // an invalid usage leaves *ppSurface untouched, other failures clear it (Wine's d3d9ex
  // test_create_depth_stencil_surface_ex and test_format_unknown); only the D3D9Ex restriction flags are usages here
  if (Usage & ~kExSurfaceUsages)
    return D3DERR_INVALIDCALL;
  InitReturnPtr(ppSurface);
  if (pSharedHandle) {
    ERR("IDirect3DDevice9::CreateDepthStencilSurface: shared surfaces are not implemented");
    return D3DERR_INVALIDCALL;
  }

  D3D9_FORMAT_DESC format;
  if (!D3D9QueryFormat(Format, format) || !(format.Flags & D3D9_FORMAT_DEPTH))
    return D3DERR_INVALIDCALL;
  if (!ValidMultisampleQuality(Format, MultiSample, MultisampleQuality))
    return D3DERR_INVALIDCALL;

  D3DSURFACE_DESC desc = {};
  desc.Format = Format;
  desc.Type = D3DRTYPE_SURFACE;
  desc.Usage = D3DUSAGE_DEPTHSTENCIL | (Usage & D3DUSAGE_RESTRICTED_CONTENT);
  desc.Pool = D3DPOOL_DEFAULT;
  desc.MultiSampleType = MultiSample;
  desc.MultiSampleQuality = MultisampleQuality;
  desc.Width = Width;
  desc.Height = Height;
  return CreateStandaloneSurface(this, desc, format.Flags & D3D9_FORMAT_LOCKABLE, ppSurface);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateOffscreenPlainSurface(
    UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle
) {
  return CreateOffscreenPlainSurfaceEx(Width, Height, Format, Pool, ppSurface, pSharedHandle, 0);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateOffscreenPlainSurfaceEx(
    UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle,
    DWORD Usage
) {
  auto lock = Lock();
  if (pSharedHandle && !extended_)
    return E_NOTIMPL;
  InitReturnPtr(ppSurface);
  bool user_memory = pSharedHandle && extended_ && Pool == D3DPOOL_SYSTEMMEM;
  if (pSharedHandle && !user_memory) {
    if (Pool != D3DPOOL_SYSTEMMEM && Pool != D3DPOOL_SCRATCH)
      ERR("IDirect3DDevice9::CreateOffscreenPlainSurface: shared surfaces are not implemented");
    return D3DERR_INVALIDCALL;
  }
  if (Pool == D3DPOOL_MANAGED)
    return D3DERR_INVALIDCALL;

  D3D9_FORMAT_DESC format;
  if (!D3D9QueryFormat(Format, format) || (format.Flags & (D3D9_FORMAT_DEPTH | D3D9_FORMAT_NULL_RT)))
    return D3DERR_INVALIDCALL;

  D3DSURFACE_DESC desc = {};
  desc.Format = Format;
  desc.Type = D3DRTYPE_SURFACE;
  desc.Usage = Usage & D3DUSAGE_RESTRICTED_CONTENT;
  desc.Pool = Pool;
  desc.MultiSampleType = D3DMULTISAMPLE_NONE;
  desc.Width = Width;
  desc.Height = Height;
  HRESULT hr = CreateStandaloneSurface(this, desc, true, ppSurface);
  if (SUCCEEDED(hr) && user_memory)
    static_cast<MTLD3D9Surface *>(*ppSurface)->UseUserMemory(*pSharedHandle);
  return hr;
}

static D3DBOX
RectToBox(const RECT *pRect, const D3DSURFACE_DESC &Desc) {
  if (!pRect)
    return {0, 0, Desc.Width, Desc.Height, 0, 1};
  return {(UINT)pRect->left, (UINT)pRect->top, (UINT)pRect->right, (UINT)pRect->bottom, 0, 1};
}

static bool
IsTextureLevel(MTLD3D9Surface *pSurface) {
  Com<IDirect3DBaseTexture9> texture;
  return SUCCEEDED(pSurface->GetContainer(__uuidof(IDirect3DBaseTexture9), (void **)&texture));
}

static bool
BoxInside(const D3DBOX &Box, const D3DSURFACE_DESC &Desc) {
  return Box.Left < Box.Right && Box.Top < Box.Bottom && Box.Right <= Desc.Width && Box.Bottom <= Desc.Height;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetRenderTargetData(IDirect3DSurface9 *pRenderTarget, IDirect3DSurface9 *pDestSurface) {
  auto lock = Lock();
  auto src = static_cast<MTLD3D9Surface *>(pRenderTarget);
  auto dst = static_cast<MTLD3D9Surface *>(pDestSurface);
  if (!src || !dst || src == dst)
    return D3DERR_INVALIDCALL;

  auto &sd = src->Desc();
  auto &dd = dst->Desc();
  if (sd.Pool != D3DPOOL_DEFAULT || dd.Pool != D3DPOOL_SYSTEMMEM || !src->texture() || dst->IsLocked())
    return D3DERR_INVALIDCALL;
  if (sd.Width != dd.Width || sd.Height != dd.Height || sd.Format != dd.Format)
    return D3DERR_INVALIDCALL;
  // documented to fail for multisampled render targets; apps resolve with StretchRect first
  if (sd.MultiSampleType != D3DMULTISAMPLE_NONE)
    return D3DERR_INVALIDCALL;

  CopyTextureToBuffer(
      src->texture(), src->level(), src->slice(), RectToBox(nullptr, sd), dst->Shadow(), dst->layout()
  );
  dst->SyncToUserMemory();
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::UpdateSurface(
    IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect, IDirect3DSurface9 *pDestinationSurface,
    const POINT *pDestPoint
) {
  auto lock = Lock();
  auto src = static_cast<MTLD3D9Surface *>(pSourceSurface);
  auto dst = static_cast<MTLD3D9Surface *>(pDestinationSurface);
  if (!src || !dst)
    return D3DERR_INVALIDCALL;

  auto &sd = src->Desc();
  auto &dd = dst->Desc();
  if (sd.Pool != D3DPOOL_SYSTEMMEM || dd.Pool != D3DPOOL_DEFAULT || !dst->texture() || src->IsLocked())
    return D3DERR_INVALIDCALL;
  if (sd.Format != dd.Format || dd.MultiSampleType != D3DMULTISAMPLE_NONE)
    return D3DERR_INVALIDCALL;

  // block-compressed rects may reach the block-padded size, and must end on a block boundary or a surface edge.
  uint32_t block = src->layout().BlockSize, mask = block - 1;
  auto padded = [&](const D3DSURFACE_DESC &d) {
    D3DSURFACE_DESC p = d;
    p.Width = (d.Width + mask) & ~mask;
    p.Height = (d.Height + mask) & ~mask;
    return p;
  };
  D3DBOX box = RectToBox(pSourceRect, sd);
  if (!BoxInside(box, padded(sd)))
    return D3DERR_INVALIDCALL;
  uint32_t x = pDestPoint ? pDestPoint->x : box.Left;
  uint32_t y = pDestPoint ? pDestPoint->y : box.Top;
  uint32_t width = box.Right - box.Left, height = box.Bottom - box.Top;
  // the part of the source inside the surface must fit the destination (Wine's visual update_surface_test)
  uint32_t real_width = std::min(box.Right, sd.Width) - box.Left, real_height = std::min(box.Bottom, sd.Height) - box.Top;
  if (x + real_width > dd.Width || y + real_height > dd.Height)
    return D3DERR_INVALIDCALL;
  auto edge_ok = [&](uint32_t end, uint32_t size) { return !(end & mask) || end >= size; };
  if ((box.Left | box.Top | x | y) & mask || !edge_ok(box.Right, sd.Width) || !edge_ok(box.Bottom, sd.Height) ||
      !edge_ok(x + width, dd.Width) || !edge_ok(y + height, dd.Height))
    return D3DERR_INVALIDCALL;
  box.Right = std::min(box.Right, std::min(sd.Width, box.Left + (dd.Width - x)));
  box.Bottom = std::min(box.Bottom, std::min(sd.Height, box.Top + (dd.Height - y)));

  CopyBufferToTexture(src->Shadow(), src->layout(), dst->texture(), dst->level(), dst->slice(), box, x, y);
  dst->MarkWritten();
  return D3D_OK;
}

// YUY2/UYVY fills take the color as packed (Y << 16 | U << 8 | V) and write whole pixel pairs
HRESULT
MTLD3D9Device::FillYUV(MTLD3D9Surface *pSurface, const RECT *pRect, D3DCOLOR Color) {
  auto &desc = pSurface->Desc();
  D3DBOX box = RectToBox(pRect, desc);
  if (!BoxInside(box, desc) || (box.Left & 1) || ((box.Right & 1) && box.Right != desc.Width))
    return D3DERR_INVALIDCALL;
  uint8_t y = Color >> 16, u = Color >> 8, v = Color;
  DWORD pair = desc.Format == D3DFMT_YUY2 ? y | u << 8 | y << 16 | (DWORD)v << 24 : u | y << 8 | v << 16 | (DWORD)y << 24;
  D3DLOCKED_RECT lr;
  if (FAILED(pSurface->LockRect(&lr, nullptr, 0)))
    return D3DERR_INVALIDCALL;
  for (UINT row = box.Top; row < box.Bottom; row++)
    for (UINT x = box.Left; x < box.Right; x += 2)
      *(DWORD *)((uint8_t *)lr.pBits + row * lr.Pitch + x * 2) = pair;
  pSurface->UnlockRect();
  return D3D_OK;
}

// DXTn surfaces are filled with solid blocks written through the CPU shadow, in whole blocks only
HRESULT
MTLD3D9Device::FillBlocks(MTLD3D9Surface *pSurface, const RECT *pRect, D3DCOLOR Color) {
  auto &desc = pSurface->Desc();
  D3DBOX box = RectToBox(pRect, desc);
  auto edge = [](UINT v, UINT size) { return !(v & 3) || v == size; };
  if (!BoxInside(box, desc) || (box.Left & 3) || (box.Top & 3) || !edge(box.Right, desc.Width) ||
      !edge(box.Bottom, desc.Height))
    return D3DERR_INVALIDCALL;

  uint8_t a = Color >> 24, r = Color >> 16, g = Color >> 8, b = Color;
  uint16_t rgb565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
  uint8_t color[8] = {(uint8_t)rgb565, (uint8_t)(rgb565 >> 8), (uint8_t)rgb565, (uint8_t)(rgb565 >> 8)};
  uint8_t block[16] = {};
  uint32_t size = desc.Format == D3DFMT_DXT1 ? 8 : 16;
  if (desc.Format == D3DFMT_DXT2 || desc.Format == D3DFMT_DXT3)
    std::memset(block, (a >> 4) * 0x11, 8);
  else if (desc.Format != D3DFMT_DXT1)
    block[0] = block[1] = a;
  std::memcpy(block + size - 8, color, 8);

  auto shadow = pSurface->EnsureShadow();
  void *data = nullptr;
  HRESULT hr = MapStaging(shadow, false, true, false, &data);
  if (FAILED(hr))
    return hr;
  auto dst = pSurface->Shadow();
  for (UINT y = box.Top; y < box.Bottom; y += 4)
    for (UINT x = box.Left; x < box.Right; x += 4)
      std::memcpy((char *)data + dst.offset + (y / 4) * dst.pitch + (x / 4) * size, block, size);
  shadow->unmap();
  CopyBufferToTexture(dst, pSurface->layout(), pSurface->texture(), pSurface->level(), pSurface->slice(), box);
  pSurface->MarkWritten();
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::ColorFill(IDirect3DSurface9 *pSurface, const RECT *pRect, D3DCOLOR Color) {
  auto lock = Lock();
  auto surface = static_cast<MTLD3D9Surface *>(pSurface);
  if (!surface || surface->Desc().Pool != D3DPOOL_DEFAULT || surface->IsDepthStencil())
    return D3DERR_INVALIDCALL;
  if (surface->Desc().Format == D3DFMT_YUY2 || surface->Desc().Format == D3DFMT_UYVY)
    return FillYUV(surface, pRect, Color);
  if (!surface->texture())
    return D3DERR_INVALIDCALL;
  // texture levels need D3DUSAGE_RENDERTARGET; offscreen plain surfaces don't
  if (IsTextureLevel(surface) && !(surface->Desc().Usage & D3DUSAGE_RENDERTARGET))
    return D3DERR_INVALIDCALL;
  if (D3D9IsDXTn(surface->Desc().Format))
    return FillBlocks(surface, pRect, Color);
  if (!(surface->texture()->usage() & WMTTextureUsageRenderTarget)) {
    ERR_ONCE("IDirect3DDevice9::ColorFill: non-renderable format ", surface->Desc().Format);
    return D3DERR_INVALIDCALL;
  }

  D3DBOX box = RectToBox(pRect, surface->Desc());
  if (!BoxInside(box, surface->Desc()))
    return D3DERR_INVALIDCALL;
  RECT rect = {(LONG)box.Left, (LONG)box.Top, (LONG)box.Right, (LONG)box.Bottom};
  ClearSurface(surface, rect, D3DCLEAR_TARGET, Color, 0.0f, 0);
  surface->MarkWritten();
  return D3D_OK;
}

// YUY2/UYVY sources are converted on the CPU (BT.601, studio range, as drivers do) into a temporary
// A8R8G8B8 surface, which is then stretched like any other
HRESULT
MTLD3D9Device::StretchFromYUV(
    MTLD3D9Surface *pSource, const RECT *pSourceRect, MTLD3D9Surface *pDest, const RECT *pDestRect,
    D3DTEXTUREFILTERTYPE Filter
) {
  auto &sd = pSource->Desc();
  if (sd.Pool != D3DPOOL_DEFAULT || pDest->Desc().Pool != D3DPOOL_DEFAULT)
    return D3DERR_INVALIDCALL;
  D3DBOX box = RectToBox(pSourceRect, sd);
  if (!BoxInside(box, sd))
    return D3DERR_INVALIDCALL;
  UINT width = box.Right - box.Left, height = box.Bottom - box.Top;

  D3DLOCKED_RECT in;
  if (FAILED(pSource->LockRect(&in, nullptr, D3DLOCK_READONLY)))
    return D3DERR_INVALIDCALL;
  Com<IDirect3DSurface9> temp;
  D3DSURFACE_DESC td = {D3DFMT_A8R8G8B8, D3DRTYPE_SURFACE, 0, D3DPOOL_DEFAULT, D3DMULTISAMPLE_NONE, 0, width, height};
  D3DLOCKED_RECT out;
  if (FAILED(CreateStandaloneSurface(this, td, true, &temp)) || FAILED(temp->LockRect(&out, nullptr, 0))) {
    pSource->UnlockRect();
    return D3DERR_INVALIDCALL;
  }
  bool uyvy = sd.Format == D3DFMT_UYVY;
  auto clamp = [](float v) { return (DWORD)std::clamp(v + 0.5f, 0.0f, 255.0f); };
  for (UINT y = 0; y < height; y++) {
    auto row = (const uint8_t *)in.pBits + (box.Top + y) * in.Pitch;
    auto dst_row = (DWORD *)((uint8_t *)out.pBits + y * out.Pitch);
    for (UINT x = 0; x < width; x++) {
      UINT sx = box.Left + x;
      const uint8_t *pair = row + (sx & ~1u) * 2;
      float luma = pair[uyvy ? 1 + (sx & 1) * 2 : (sx & 1) * 2];
      float u = pair[uyvy ? 0 : 1], v = pair[uyvy ? 2 : 3];
      float c = 1.164f * (luma - 16.0f), d = u - 128.0f, e = v - 128.0f;
      dst_row[x] = 0xFF000000 | clamp(c + 1.596f * e) << 16 | clamp(c - 0.391f * d - 0.813f * e) << 8 |
                   clamp(c + 2.018f * d);
    }
  }
  temp->UnlockRect();
  pSource->UnlockRect();
  return StretchRect(temp.ptr(), nullptr, pDest, pDestRect, Filter);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::StretchRect(
    IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect, IDirect3DSurface9 *pDestSurface,
    const RECT *pDestRect, D3DTEXTUREFILTERTYPE Filter
) {
  auto lock = Lock();
  auto src = static_cast<MTLD3D9Surface *>(pSourceSurface);
  auto dst = static_cast<MTLD3D9Surface *>(pDestSurface);
  if (src && dst && src != dst && dst->texture() && (src->Desc().Format == D3DFMT_YUY2 || src->Desc().Format == D3DFMT_UYVY))
    return StretchFromYUV(src, pSourceRect, dst, pDestRect, Filter);
  if (!src || !dst || src == dst || !src->texture() || !dst->texture())
    return D3DERR_INVALIDCALL;

  auto &sd = src->Desc();
  auto &dd = dst->Desc();
  if (sd.Pool != D3DPOOL_DEFAULT || dd.Pool != D3DPOOL_DEFAULT)
    return D3DERR_INVALIDCALL;

  D3DBOX sbox = RectToBox(pSourceRect, sd);
  D3DBOX dbox = RectToBox(pDestRect, dd);
  if (!BoxInside(sbox, sd) || !BoxInside(dbox, dd))
    return D3DERR_INVALIDCALL;
  if (IsTextureLevel(dst) && !(dd.Usage & D3DUSAGE_RENDERTARGET))
    return D3DERR_INVALIDCALL;
  if (Filter != D3DTEXF_NONE && Filter != D3DTEXF_POINT && Filter != D3DTEXF_LINEAR)
    return D3DERR_INVALIDCALL;
  // an offscreen plain destination only takes an unscaled copy of another offscreen plain surface
  auto offscreen = [](MTLD3D9Surface *s) {
    return !IsTextureLevel(s) && !(s->Desc().Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL));
  };
  if (offscreen(dst) && (!offscreen(src) || sbox.Right - sbox.Left != dbox.Right - dbox.Left ||
                         sbox.Bottom - sbox.Top != dbox.Bottom - dbox.Top))
    return D3DERR_INVALIDCALL;
  dst->MarkWritten();
  bool depth = (sd.Usage | dd.Usage) & D3DUSAGE_DEPTHSTENCIL;
  // depth copies are whole-surface only
  auto whole = [](const D3DBOX &b, const D3DSURFACE_DESC &d) {
    return b.Left == 0 && b.Top == 0 && b.Right == d.Width && b.Bottom == d.Height;
  };
  if (depth && (!whole(sbox, sd) || !whole(dbox, dd) || sd.Width != dd.Width || sd.Height != dd.Height || sd.Format != dd.Format))
    return D3DERR_INVALIDCALL;

  bool same_size = sbox.Right - sbox.Left == dbox.Right - dbox.Left && sbox.Bottom - sbox.Top == dbox.Bottom - dbox.Top;
  bool same_format = ORIGINAL_FORMAT(src->texture()->pixelFormat()) == ORIGINAL_FORMAT(dst->texture()->pixelFormat());
  bool src_ms = sd.MultiSampleType != D3DMULTISAMPLE_NONE;
  bool dst_ms = dd.MultiSampleType != D3DMULTISAMPLE_NONE;

  if (src_ms && !dst_ms && same_format && same_size && !pSourceRect && !pDestRect && sd.Width == dd.Width &&
      sd.Height == dd.Height) {
    EndRenderPass();
    Emit([src = src->texture(), src_view = src->viewKey(), dst = dst->texture(),
          dst_view = dst->viewKey()](ArgumentEncodingContext &enc) mutable {
      enc.resolveTexture(std::move(src), src_view, std::move(dst), dst_view);
    });
    return D3D_OK;
  }

  if (same_size && same_format && src_ms == dst_ms) {
    EmitBlit([src = src->texture(), src_level = src->level(), src_slice = src->slice(), dst = dst->texture(),
              dst_level = dst->level(), dst_slice = dst->slice(), sbox, dbox](ArgumentEncodingContext &enc) {
      auto &cmd = enc.encodeBlitCommand<wmtcmd_blit_copy_from_texture_to_texture>();
      cmd.type = WMTBlitCommandCopyFromTextureToTexture;
      cmd.src = enc.access(src, src_level, src_slice, ResourceAccess::Read);
      cmd.src_slice = src_slice;
      cmd.src_level = src_level;
      cmd.src_origin = {sbox.Left, sbox.Top, 0};
      cmd.src_size = {sbox.Right - sbox.Left, sbox.Bottom - sbox.Top, 1};
      cmd.dst = enc.access(dst, dst_level, dst_slice, ResourceAccess::Write);
      cmd.dst_slice = dst_slice;
      cmd.dst_level = dst_level;
      cmd.dst_origin = {dbox.Left, dbox.Top, 0};
    });
    return D3D_OK;
  }

  if (depth)
    return D3DERR_INVALIDCALL;
  if (!(dst->texture()->usage() & WMTTextureUsageRenderTarget)) {
    ERR_ONCE("IDirect3DDevice9::StretchRect: destination format ", dd.Format, " is not renderable");
    return D3DERR_INVALIDCALL;
  }

  Rc<Texture> source = src->texture();
  TextureViewKey source_view = src->viewKey();
  EndRenderPass();
  if (src_ms) {
    if (!resolve_temp_ || resolve_temp_->width() != sd.Width || resolve_temp_->height() != sd.Height ||
        resolve_temp_->pixelFormat() != source->pixelFormat()) {
      D3D9_TEXTURE_DESC desc = {D3DRTYPE_SURFACE, sd.Width, sd.Height, 1, 1, D3DUSAGE_RENDERTARGET, sd.Format, D3DPOOL_DEFAULT};
      if (FAILED(CreateD3D9TextureStorage(this, desc, 1, 1, resolve_temp_)))
        return D3DERR_INVALIDCALL;
    }
    Emit([src = source, src_view = source_view, dst = resolve_temp_](ArgumentEncodingContext &enc) mutable {
      auto view = dst->fullView;
      enc.resolveTexture(std::move(src), src_view, std::move(dst), view);
    });
    source = resolve_temp_;
    source_view = resolve_temp_->fullView;
  }
  float src_rect[4] = {(float)sbox.Left, (float)sbox.Top, (float)sbox.Right, (float)sbox.Bottom};
  WMTScissorRect dst_rect = {dbox.Left, dbox.Top, dbox.Right - dbox.Left, dbox.Bottom - dbox.Top};
  bool linear = Filter == D3DTEXF_LINEAR || Filter == D3DTEXF_ANISOTROPIC;
  Emit([source = std::move(source), source_view, src_rect = std::to_array(src_rect), dst = dst->texture(),
        dst_view = dst->viewKey(), dst_rect, linear](ArgumentEncodingContext &enc) mutable {
    float rect[4] = {src_rect[0], src_rect[1], src_rect[2], src_rect[3]};
    enc.blit_color_cmd.blit(source, source_view, rect, dst, dst_view, dst_rect, linear);
  });
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 *pRenderTarget) {
  auto lock = Lock();
  if (RenderTargetIndex >= caps::MaxSimultaneousRenderTargets || (RenderTargetIndex == 0 && !pRenderTarget))
    return D3DERR_INVALIDCALL;

  auto surface = static_cast<MTLD3D9Surface *>(pRenderTarget);
  if (surface && (!(surface->Desc().Usage & D3DUSAGE_RENDERTARGET) || surface->GetParentDevice() != this))
    return D3DERR_INVALIDCALL;

  if (render_targets_[RenderTargetIndex].ptr() != surface) {
    EndRenderPass();
    if (auto previous = render_targets_[RenderTargetIndex].ptr())
      previous->MarkWritten();
  }
  render_targets_[RenderTargetIndex] = surface;
  if (RenderTargetIndex == 0)
    SetDefaultViewport(surface);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 **ppRenderTarget) {
  auto lock = Lock();
  InitReturnPtr(ppRenderTarget);
  if (!ppRenderTarget || RenderTargetIndex >= caps::MaxSimultaneousRenderTargets)
    return D3DERR_INVALIDCALL;
  if (!render_targets_[RenderTargetIndex])
    return D3DERR_NOTFOUND;
  *ppRenderTarget = render_targets_[RenderTargetIndex].ref();
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetDepthStencilSurface(IDirect3DSurface9 *pNewZStencil) {
  auto lock = Lock();
  auto surface = static_cast<MTLD3D9Surface *>(pNewZStencil);
  if (surface && (!surface->IsDepthStencil() || surface->GetParentDevice() != this))
    return D3DERR_INVALIDCALL;
  if (depth_stencil_.ptr() != surface)
    EndRenderPass();
  depth_stencil_ = surface;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetDepthStencilSurface(IDirect3DSurface9 **ppZStencilSurface) {
  auto lock = Lock();
  InitReturnPtr(ppZStencilSurface);
  if (!ppZStencilSurface)
    return D3DERR_INVALIDCALL;
  if (!depth_stencil_)
    return D3DERR_NOTFOUND;
  *ppZStencilSurface = depth_stencil_.ref();
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::BeginScene() {
  auto lock = Lock();
  if (in_scene_)
    return D3DERR_INVALIDCALL;
  in_scene_ = true;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::EndScene() {
  auto lock = Lock();
  if (!in_scene_)
    return D3DERR_INVALIDCALL;
  in_scene_ = false;
  FlushCommands();
  return D3D_OK;
}

static bool
IntersectRect(RECT &Dst, const RECT &A, const RECT &B) {
  Dst.left = std::max(A.left, B.left);
  Dst.top = std::max(A.top, B.top);
  Dst.right = std::min(A.right, B.right);
  Dst.bottom = std::min(A.bottom, B.bottom);
  return Dst.left < Dst.right && Dst.top < Dst.bottom;
}

void
MTLD3D9Device::ClearSurface(
    MTLD3D9Surface *pSurface, const RECT &Rect, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil, bool Srgb
) {
  EndRenderPass();
  auto &desc = pSurface->Desc();
  bool full = Rect.left == 0 && Rect.top == 0 && (UINT)Rect.right == desc.Width && (UINT)Rect.bottom == desc.Height;
  uint32_t x = Rect.left, y = Rect.top;
  uint32_t width = Rect.right - Rect.left, height = Rect.bottom - Rect.top;

  if (!pSurface->IsDepthStencil()) {
    WMTClearColor color = {
        ((Color >> 16) & 0xFF) / 255.0, ((Color >> 8) & 0xFF) / 255.0, (Color & 0xFF) / 255.0,
        ((Color >> 24) & 0xFF) / 255.0
    };
    // Clear writes through D3DRS_SRGBWRITEENABLE like a draw; ColorFill doesn't
    TextureViewKey target_view = Srgb ? RenderTargetView(pSurface) : pSurface->viewKey();
    if (full) {
      Emit([texture = pSurface->texture(), view = target_view, color](ArgumentEncodingContext &enc) mutable {
        enc.clearColor(std::move(texture), view, 1, 0, color);
      });
      return;
    }
    std::array<float, 4> rgba = {(float)color.r, (float)color.g, (float)color.b, (float)color.a};
    Emit([texture = pSurface->texture(), view = target_view, x, y, width, height,
          rgba](ArgumentEncodingContext &enc) {
      enc.clear_rt_cmd.begin(texture, view, 0);
      enc.clear_rt_cmd.clear(x, y, width, height, 1, rgba);
      enc.clear_rt_cmd.end();
    });
    return;
  }

  unsigned planes = 0;
  if (Flags & D3DCLEAR_ZBUFFER)
    planes |= 1;
  if (Flags & D3DCLEAR_STENCIL)
    planes |= 2;

  Emit([texture = pSurface->texture(), view = pSurface->viewKey(), planes, full, x, y, width, height, Z,
        stencil = (uint8_t)Stencil](ArgumentEncodingContext &enc) mutable {
    unsigned flags = planes & DepthStencilPlanarFlags(texture->pixelFormat());
    if (!flags)
      return;
    if (full) {
      enc.clearDepthStencil(std::move(texture), view, 1, flags, Z, stencil);
      return;
    }
    enc.clear_rt_cmd.begin(texture, view, 0, flags);
    enc.clear_rt_cmd.clear(x, y, width, height, Z, stencil);
    enc.clear_rt_cmd.end();
  });
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::Clear(DWORD Count, const D3DRECT *pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) {
  auto lock = Lock();
  // rects with a zero count clear nothing, and a count without rects clears everything
  if (!Count && pRects)
    return D3D_OK;
  if (!pRects)
    Count = 0;

  if (Flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) {
    if (!depth_stencil_)
      return D3DERR_INVALIDCALL;
    D3D9_FORMAT_DESC format;
    D3D9QueryFormat(depth_stencil_->Desc().Format, format);
    if ((Flags & D3DCLEAR_STENCIL) && !(format.Flags & D3D9_FORMAT_STENCIL))
      return D3DERR_INVALIDCALL;
  }

  RECT area = {(LONG)viewport_.X, (LONG)viewport_.Y, (LONG)(viewport_.X + viewport_.Width),
               (LONG)(viewport_.Y + viewport_.Height)};
  if (render_states_[D3DRS_SCISSORTESTENABLE] && !IntersectRect(area, area, scissor_rect_))
    return D3D_OK;

  auto clear = [&](MTLD3D9Surface *surface, DWORD flags) {
    if (!surface->texture())
      return;
    auto &desc = surface->Desc();
    RECT bounds = {0, 0, (LONG)desc.Width, (LONG)desc.Height};
    RECT target;
    if (!IntersectRect(target, area, bounds))
      return;
    if (!Count) {
      ClearSurface(surface, target, flags, Color, Z, Stencil, true);
      return;
    }
    for (DWORD i = 0; i < Count; i++) {
      RECT rect = {pRects[i].x1, pRects[i].y1, pRects[i].x2, pRects[i].y2};
      if (IntersectRect(rect, rect, target))
        ClearSurface(surface, rect, flags, Color, Z, Stencil, true);
    }
  };

  if (Flags & D3DCLEAR_TARGET) {
    for (auto &rt : render_targets_)
      if (rt)
        clear(rt.ptr(), D3DCLEAR_TARGET);
  }
  if (Flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL))
    clear(depth_stencil_.ptr(), Flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL));

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetViewport(const D3DVIEWPORT9 *pViewport) {
  auto lock = Lock();
  if (!pViewport)
    return D3DERR_INVALIDCALL;
  if (recording_) {
    recording_->SetViewport(*pViewport);
    return D3D_OK;
  }
  viewport_ = *pViewport;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetViewport(D3DVIEWPORT9 *pViewport) {
  auto lock = Lock();
  if (!pViewport)
    return D3DERR_INVALIDCALL;
  *pViewport = viewport_;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) {
  auto lock = Lock();
  if ((uint32_t)State >= render_states_.size())
    return D3DERR_INVALIDCALL;
  if (recording_) {
    recording_->SetRenderState(State, Value);
    return D3D_OK;
  }
  if (State == D3DRS_SRGBWRITEENABLE && render_states_[State] != Value)
    EndRenderPass();
  render_states_[State] = Value;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetRenderState(D3DRENDERSTATETYPE State, DWORD *pValue) {
  auto lock = Lock();
  if (!pValue)
    return D3DERR_INVALIDCALL;
  if ((uint32_t)State >= render_states_.size()) {
    *pValue = 0;
    return D3DERR_INVALIDCALL;
  }
  *pValue = render_states_[State];
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetScissorRect(const RECT *pRect) {
  auto lock = Lock();
  if (!pRect)
    return D3DERR_INVALIDCALL;
  if (recording_) {
    recording_->SetScissorRect(*pRect);
    return D3D_OK;
  }
  scissor_rect_ = *pRect;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetScissorRect(RECT *pRect) {
  auto lock = Lock();
  if (!pRect)
    return D3DERR_INVALIDCALL;
  *pRect = scissor_rect_;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetSoftwareVertexProcessing(BOOL bSoftware) {
  return D3D_OK;
}

BOOL STDMETHODCALLTYPE
MTLD3D9Device::GetSoftwareVertexProcessing() {
  return FALSE;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::PresentEx(
    const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion,
    DWORD dwFlags
) {
  auto lock = Lock();
  return swapchain_->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetGPUThreadPriority(INT *pPriority) {
  if (!pPriority)
    return D3DERR_INVALIDCALL;
  *pPriority = gpu_thread_priority_;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetGPUThreadPriority(INT Priority) {
  if (Priority < -7 || Priority > 7)
    return D3DERR_INVALIDCALL;
  gpu_thread_priority_ = Priority;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::WaitForVBlank(UINT iSwapChain) {
  if (iSwapChain != 0)
    return D3DERR_INVALIDCALL;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CheckResourceResidency(IDirect3DResource9 **pResourceArray, UINT32 NumResources) {
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetMaximumFrameLatency(UINT MaxLatency) {
  auto lock = Lock();
  if (MaxLatency > 30)
    return D3DERR_INVALIDCALL;
  max_frame_latency_ = MaxLatency ? MaxLatency : 3;
  queue().SetMaxLatency(max_frame_latency_);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetMaximumFrameLatency(UINT *pMaxLatency) {
  if (!pMaxLatency)
    return D3DERR_INVALIDCALL;
  *pMaxLatency = max_frame_latency_;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CheckDeviceState(HWND hDestinationWindow) {
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::ResetEx(D3DPRESENT_PARAMETERS *pPresentationParameters, D3DDISPLAYMODEEX *pFullscreenDisplayMode) {
  // fullscreen needs the display mode, and the back buffer has its size (Wine's d3d9ex test_reset_ex)
  if (pPresentationParameters && !pPresentationParameters->Windowed &&
      (!pFullscreenDisplayMode || pPresentationParameters->BackBufferWidth != pFullscreenDisplayMode->Width ||
       pPresentationParameters->BackBufferHeight != pFullscreenDisplayMode->Height))
    return D3DERR_INVALIDCALL;
  if (pPresentationParameters && pPresentationParameters->Windowed && pFullscreenDisplayMode)
    return D3DERR_INVALIDCALL;
  return Reset(pPresentationParameters);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetDisplayModeEx(UINT iSwapChain, D3DDISPLAYMODEEX *pMode, D3DDISPLAYROTATION *pRotation) {
  if (iSwapChain != 0)
    return D3DERR_INVALIDCALL;
  return swapchain_->GetDisplayModeEx(pMode, pRotation);
}

HRESULT
CreateD3D9Device(
    MTLD3D9 *pParent, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
    D3DPRESENT_PARAMETERS *pParams, D3DDISPLAYMODEEX *pFullscreenMode, bool Extended, IDirect3DDevice9Ex **ppDevice
) {
  InitReturnPtr(ppDevice);
  if (!ppDevice || !pParams)
    return D3DERR_INVALIDCALL;

  auto adapter = pParent->GetAdapter(Adapter);
  if (!adapter)
    return D3DERR_INVALIDCALL;
  if (DeviceType != D3DDEVTYPE_HAL)
    return D3DERR_NOTAVAILABLE;

  auto device = new MTLD3D9Device(pParent, adapter, DeviceType, hFocusWindow, BehaviorFlags, Extended);
  HRESULT hr = device->Initialize(pParams, pFullscreenMode);
  if (FAILED(hr)) {
    device->Release();
    return hr;
  }
  *ppDevice = device;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9 **ppQuery) {
  // an unsupported type leaves *ppQuery untouched
  if (!MTLD3D9Query::Supported(Type))
    return D3DERR_NOTAVAILABLE;
  if (!ppQuery)
    return D3D_OK;
  *ppQuery = ref(new MTLD3D9Query(this, Type));
  return D3D_OK;
}

} // namespace mullion
