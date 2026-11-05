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

namespace mullion {

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::ValidateDevice(DWORD *pNumPasses) {
  if (!pNumPasses)
    return D3DERR_INVALIDCALL;
  *pNumPasses = 1;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetClipStatus(const D3DCLIPSTATUS9 *pClipStatus) {
  auto lock = Lock();
  if (!pClipStatus)
    return D3DERR_INVALIDCALL;
  clip_status_ = *pClipStatus;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetClipStatus(D3DCLIPSTATUS9 *pClipStatus) {
  auto lock = Lock();
  if (!pClipStatus)
    return D3DERR_INVALIDCALL;
  *pClipStatus = clip_status_;
  return D3D_OK;
}

// palettized formats aren't exposed, so palettes are only stored for the getters.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetPaletteEntries(UINT PaletteNumber, const PALETTEENTRY *pEntries) {
  auto lock = Lock();
  if (!pEntries || PaletteNumber == 0xFFFF)
    return D3DERR_INVALIDCALL;
  // without D3DPTEXTURECAPS_ALPHAPALETTE every entry must be opaque
  if (std::any_of(pEntries, pEntries + 256, [](const PALETTEENTRY &e) { return e.peFlags != 0xff; }))
    return D3DERR_INVALIDCALL;
  std::copy(pEntries, pEntries + 256, palettes_[PaletteNumber].begin());
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY *pEntries) {
  auto lock = Lock();
  auto it = palettes_.find(PaletteNumber);
  if (!pEntries || it == palettes_.end())
    return D3DERR_INVALIDCALL;
  std::copy(it->second.begin(), it->second.end(), pEntries);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetCurrentTexturePalette(UINT PaletteNumber) {
  auto lock = Lock();
  if (!palettes_.contains(PaletteNumber))
    return D3DERR_INVALIDCALL;
  current_palette_ = PaletteNumber;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetCurrentTexturePalette(UINT *pPaletteNumber) {
  auto lock = Lock();
  if (!pPaletteNumber)
    return D3DERR_INVALIDCALL;
  *pPaletteNumber = current_palette_;
  return D3D_OK;
}

// N-patches and rect/tri patches aren't in the caps; the mode is kept for the getter.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetNPatchMode(float nSegments) {
  auto lock = Lock();
  npatch_segments_ = nSegments;
  return D3D_OK;
}

float STDMETHODCALLTYPE
MTLD3D9Device::GetNPatchMode() {
  return npatch_segments_;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::DrawRectPatch(UINT Handle, const float *pNumSegs, const D3DRECTPATCH_INFO *pRectPatchInfo) {
  return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::DrawTriPatch(UINT Handle, const float *pNumSegs, const D3DTRIPATCH_INFO *pTriPatchInfo) {
  return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::DeletePatch(UINT Handle) {
  return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9 *pDestSurface) {
  if (iSwapChain != 0)
    return D3DERR_INVALIDCALL;
  return swapchain_->GetFrontBufferData(pDestSurface);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS *pPresentationParameters, IDirect3DSwapChain9 **ppSwapChain) {
  auto lock = Lock();
  InitReturnPtr(ppSwapChain);
  if (!pPresentationParameters || !ppSwapChain)
    return D3DERR_INVALIDCALL;
  // additional swap chains are windowed, and none exist beside a fullscreen one (Wine's test_swapchain)
  if (!pPresentationParameters->Windowed || swapchain_->IsFullscreen())
    return D3DERR_INVALIDCALL;
  D3DPRESENT_PARAMETERS params = *pPresentationParameters;
  if (!params.hDeviceWindow)
    params.hDeviceWindow = creation_params_.hFocusWindow;
  if (!params.hDeviceWindow)
    return D3DERR_INVALIDCALL;
  Com<MTLD3D9SwapChain> swapchain = new MTLD3D9SwapChain(this, params);
  HRESULT hr = swapchain->Reset(&params);
  if (FAILED(hr))
    return hr;
  // the app's parameters come back filled in, but a NULL device window stays NULL (Wine's test_swapchain)
  HWND window = pPresentationParameters->hDeviceWindow;
  *pPresentationParameters = params;
  pPresentationParameters->hDeviceWindow = window;
  *ppSwapChain = swapchain.ref();
  return D3D_OK;
}

// both only apply to monochrome D3DFMT_A1 surfaces, which aren't supported.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetConvolutionMonoKernel(UINT Width, UINT Height, float *pRows, float *pColumns) {
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::ComposeRects(
    IDirect3DSurface9 *pSrc, IDirect3DSurface9 *pDst, IDirect3DVertexBuffer9 *pSrcRectDescs, UINT NumRects,
    IDirect3DVertexBuffer9 *pDstRectDescs, D3DCOMPOSERECTSOP Operation, INT Xoffset, INT Yoffset
) {
  return D3DERR_INVALIDCALL;
}

} // namespace mullion
