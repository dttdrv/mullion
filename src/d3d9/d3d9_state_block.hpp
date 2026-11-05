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

#include "d3d9_constants.hpp"
#include "d3d9_device_child.hpp"
#include "com/com_pointer.hpp"
#include <array>
#include <bitset>
#include <map>

namespace mullion {

class MTLD3D9Device;

// sampler indices are the device's: pixel samplers 0-15, then vertex samplers 16-19.
class MTLD3D9StateBlock final : public MTLD3D9DeviceChild<IDirect3DStateBlock9> {
public:
  // an empty block for BeginStateBlock to record into.
  MTLD3D9StateBlock(MTLD3D9Device *pDevice);
  MTLD3D9StateBlock(MTLD3D9Device *pDevice, D3DSTATEBLOCKTYPE Type);

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final;
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) final;
  HRESULT STDMETHODCALLTYPE Capture() final;
  HRESULT STDMETHODCALLTYPE Apply() final;

  void SetRenderState(D3DRENDERSTATETYPE State, DWORD Value);
  void SetSamplerState(DWORD Index, D3DSAMPLERSTATETYPE Type, DWORD Value);
  void SetTexture(DWORD Index, IDirect3DBaseTexture9 *pTexture);
  void SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value);
  void SetTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX &Matrix);
  void SetMaterial(const D3DMATERIAL9 &Material);
  void SetLight(DWORD Index, const D3DLIGHT9 &Light);
  void LightEnable(DWORD Index, BOOL Enable);
  void SetVertexShader(IDirect3DVertexShader9 *pShader);
  void SetPixelShader(IDirect3DPixelShader9 *pShader);
  void SetVertexDeclaration(IDirect3DVertexDeclaration9 *pDecl);
  void SetFVF(DWORD FVF);
  void SetStreamSource(UINT Stream, IDirect3DVertexBuffer9 *pBuffer, UINT Offset, UINT Stride);
  void SetStreamSourceFreq(UINT Stream, UINT Setting);
  void SetIndices(IDirect3DIndexBuffer9 *pIndices);
  void SetConstantF(bool Pixel, UINT Start, const float *pData, UINT Count);
  void SetConstantI(bool Pixel, UINT Start, const int *pData, UINT Count);
  void SetConstantB(bool Pixel, UINT Start, const BOOL *pData, UINT Count);
  void SetViewport(const D3DVIEWPORT9 &Viewport);
  void SetScissorRect(const RECT &Rect);
  void SetClipPlane(DWORD Index, const float *pPlane);

private:
  static constexpr uint32_t kSamplers = caps::MaxSamplersPS + caps::MaxSamplersVS;

  struct Stream {
    Com<IDirect3DVertexBuffer9> buffer;
    UINT offset = 0;
    UINT stride = 0;
  };

  void CaptureType(D3DSTATEBLOCKTYPE Type);

  MTLD3D9Device *parent_;
  bool typed_ = false;
  bool captured_ = false;

  std::array<DWORD, 256> render_states_ = {};
  std::bitset<256> render_state_mask_;
  std::array<std::array<DWORD, D3DSAMP_DMAPOFFSET + 1>, kSamplers> sampler_states_ = {};
  std::array<uint32_t, kSamplers> sampler_state_mask_ = {};
  std::array<Com<IDirect3DBaseTexture9>, kSamplers> textures_;
  uint32_t texture_mask_ = 0;
  std::array<std::array<DWORD, D3DTSS_CONSTANT + 1>, 8> stage_states_ = {};
  std::array<uint64_t, 8> stage_state_mask_ = {};
  std::array<D3DMATRIX, 512> transforms_ = {};
  std::bitset<512> transform_mask_;
  D3DMATERIAL9 material_ = {};
  bool material_set_ = false;
  std::map<DWORD, D3DLIGHT9> lights_;
  std::map<DWORD, BOOL> light_enables_;
  Com<IDirect3DVertexShader9> vertex_shader_;
  Com<IDirect3DPixelShader9> pixel_shader_;
  bool vertex_shader_set_ = false;
  bool pixel_shader_set_ = false;
  Com<IDirect3DVertexDeclaration9> declaration_;
  DWORD fvf_ = 0;
  bool declaration_set_ = false;
  std::array<Stream, caps::MaxStreams> streams_;
  uint32_t stream_mask_ = 0;
  std::array<UINT, caps::MaxStreams> stream_freqs_ = {};
  uint32_t stream_freq_mask_ = 0;
  Com<IDirect3DIndexBuffer9> indices_;
  bool indices_set_ = false;
  std::array<std::array<float, 256 * 4>, 2> const_float_ = {};
  std::array<std::bitset<256>, 2> const_float_mask_;
  std::array<std::array<int, caps::MaxOtherConstants * 4>, 2> const_int_ = {};
  std::array<uint32_t, 2> const_int_mask_ = {};
  std::array<std::array<BOOL, caps::MaxOtherConstants>, 2> const_bool_ = {};
  std::array<uint32_t, 2> const_bool_mask_ = {};
  D3DVIEWPORT9 viewport_ = {};
  bool viewport_set_ = false;
  RECT scissor_ = {};
  bool scissor_set_ = false;
  std::array<std::array<float, 4>, caps::MaxClipPlanes> clip_planes_ = {};
  uint32_t clip_plane_mask_ = 0;
};

} // namespace mullion
